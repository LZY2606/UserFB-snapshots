/*
 * RESP3 streamed value contract tests.
 *
 * Covers the unknown-length blob string ("$?"), array ("*?"), map ("%?")
 * and set ("~?") added to the existing redisReaderFeed/GetReply pipeline.
 * Everything runs purely in-process: no Redis, TLS, sockets or extra
 * dependencies. The four entry points return the number of positive
 * assertions actually executed, or a negative value on failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#include "hiredis.h"
#include "alloc.h"

static int g_asserts;

#define A(cond) do {                                                    \
    if (!(cond)) {                                                      \
        fprintf(stderr, "ASSERT FAIL %s:%d: %s\n",                     \
                __FILE__, __LINE__, #cond);                            \
        return -1;                                                      \
    }                                                                   \
    g_asserts++;                                                        \
} while (0)

/* ------------------------------------------------------------------ */
/* Hand-written expected reply trees                                   */
/* ------------------------------------------------------------------ */

typedef struct Node {
    int type;
    long long ival;                 /* INTEGER / BOOL */
    char *str;                      /* STRING / STATUS / ERROR ... */
    size_t len;
    struct Node **kid;              /* ARRAY / MAP / SET / PUSH */
    size_t n;
} Node;

static Node *mkLeaf(int type) {
    Node *x = hi_calloc(1, sizeof(*x));
    x->type = type;
    return x;
}

static Node *mkInt(long long v) {
    Node *x = mkLeaf(REDIS_REPLY_INTEGER);
    x->ival = v;
    return x;
}

static Node *mkStr(int type, const void *data, size_t len) {
    Node *x = mkLeaf(type);
    x->len = len;
    x->str = hi_malloc(len + 1);
    memcpy(x->str, data, len);
    x->str[len] = '\0';
    return x;
}

static Node *mkAgg(int type, size_t n, ...) {
    Node *x = mkLeaf(type);
    x->n = n;
    if (n) {
        x->kid = hi_calloc(n, sizeof(*x->kid));
        va_list ap;
        va_start(ap, n);
        for (size_t i = 0; i < n; i++)
            x->kid[i] = va_arg(ap, Node*);
        va_end(ap);
    }
    return x;
}

static void freeNode(Node *x) {
    if (!x) return;
    for (size_t i = 0; i < x->n; i++)
        freeNode(x->kid[i]);
    hi_free(x->kid);
    hi_free(x->str);
    hi_free(x);
}

static int replyEqualsNode(const redisReply *r, const Node *x) {
    if (r == NULL || x == NULL)
        return r == (void*)x;
    if (r->type != x->type)
        return 0;
    switch (r->type) {
    case REDIS_REPLY_INTEGER:
    case REDIS_REPLY_BOOL:
        return r->integer == x->ival;
    case REDIS_REPLY_NIL:
        return 1;
    case REDIS_REPLY_STRING:
    case REDIS_REPLY_STATUS:
    case REDIS_REPLY_ERROR:
    case REDIS_REPLY_BIGNUM:
        return r->len == x->len && memcmp(r->str, x->str, x->len) == 0;
    case REDIS_REPLY_ARRAY:
    case REDIS_REPLY_MAP:
    case REDIS_REPLY_SET:
    case REDIS_REPLY_PUSH:
        if (r->elements != x->n)
            return 0;
        for (size_t i = 0; i < x->n; i++)
            if (!replyEqualsNode(r->element[i], x->kid[i]))
                return 0;
        return 1;
    default:
        return 0;
    }
}

/* Build the expected tree and check the completed reply against it. */
static int expectReply(redisReader *r, const Node *want) {
    void *p = NULL;
    int st = redisReaderGetReply(r, &p);
    A(st == REDIS_OK);
    A(p != NULL);
    int eq = replyEqualsNode((redisReply*)p, want);
    freeReplyObject(p);
    A(eq);
    return 0;
}

/* No reply may be published yet (incomplete input). */
static int expectPending(redisReader *r) {
    void *p = NULL;
    int st = redisReaderGetReply(r, &p);
    A(st == REDIS_OK);
    A(p == NULL);
    return 0;
}

/* Fatal check for use in helpers whose signature cannot return a code. */
#define MUST(cond) do {                                                  \
    if (!(cond)) {                                                      \
        fprintf(stderr, "FATAL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        abort();                                                        \
    }                                                                   \
} while (0)

/* Feed data in fixed-size strides, driving GetReply after every stride.
 * A complete single reply may appear on the final stride; it is returned to
 * the caller (NULL while the message is still incomplete). */
static redisReply *feedStrides(redisReader *r, const char *buf, size_t n,
                               size_t stride) {
    redisReply *done = NULL;
    for (size_t i = 0; i < n; i += stride) {
        size_t c = (n - i) < stride ? (n - i) : stride;
        MUST(redisReaderFeed(r, buf + i, c) == REDIS_OK);
        void *p = NULL;
        MUST(redisReaderGetReply(r, &p) == REDIS_OK);
        if (p != NULL) {
            MUST(done == NULL);     /* exactly one reply for these inputs */
            done = p;
        }
    }
    return done;
}

/* ------------------------------------------------------------------ */
/* 1. Chunking: every byte boundary, split markers, compaction         */
/* ------------------------------------------------------------------ */

int contract_chunks(void) {
    g_asserts = 0;

    const char *blob = "$?\r\n;3\r\nfoo\r\n;3\r\nbar\r\n;0\r\n";
    const char *arr  = "*?\r\n:1\r\n:2\r\n.\r\n";
    const char *mp   = "%?\r\n+a\r\n:1\r\n+b\r\n:2\r\n.\r\n";
    const char *set  = "~?\r\n+x\r\n+y\r\n.\r\n";
    const char *emptyBlob = "$?\r\n;0\r\n";
    const char *emptyArr  = "*?\r\n.\r\n";

    Node *wantBlob = mkStr(REDIS_REPLY_STRING, "foobar", 6);
    Node *wantArr  = mkAgg(REDIS_REPLY_ARRAY, 2, mkInt(1), mkInt(2));
    Node *wantMap  = mkAgg(REDIS_REPLY_MAP, 4,
                           mkStr(REDIS_REPLY_STATUS, "a", 1), mkInt(1),
                           mkStr(REDIS_REPLY_STATUS, "b", 1), mkInt(2));
    Node *wantSet  = mkAgg(REDIS_REPLY_SET, 2,
                           mkStr(REDIS_REPLY_STATUS, "x", 1),
                           mkStr(REDIS_REPLY_STATUS, "y", 1));
    Node *wantEmptyBlob = mkStr(REDIS_REPLY_STRING, "", 0);
    Node *wantEmptyArr  = mkAgg(REDIS_REPLY_ARRAY, 0);

    struct Case { const char *in; size_t n; Node *want; } cases[] = {
        { blob, strlen(blob), wantBlob },
        { arr,  strlen(arr),  wantArr  },
        { mp,   strlen(mp),   wantMap  },
        { set,  strlen(set),  wantSet  },
        { emptyBlob, strlen(emptyBlob), wantEmptyBlob },
        { emptyArr,  strlen(emptyArr),  wantEmptyArr  },
    };

    /* One-byte-at-a-time feeding, including splits of the '?', every chunk
     * length CRLF, payload trailing CRLF and the '.' closing marker. */
    for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++) {
        for (size_t cut = 1; cut <= cases[c].n; cut++) {
            redisReader *r = redisReaderCreate();
            A(r != NULL);
            for (size_t i = 0; i < cut; i++) {
                A(redisReaderFeed(r, cases[c].in + i, 1) == REDIS_OK);
                if (i < cases[c].n - 1)   /* last byte may complete it */
                    A(expectPending(r) == 0);
            }
            for (size_t i = cut; i < cases[c].n; i++)
                A(redisReaderFeed(r, cases[c].in + i, 1) == REDIS_OK);
            A(expectReply(r, cases[c].want) == 0);
            redisReaderFree(r);
        }
    }

    /* Every two-part cut as well, with a GetReply between the halves. */
    for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++) {
        for (size_t cut = 0; cut <= cases[c].n; cut++) {
            redisReader *r = redisReaderCreate();
            if (cut > 0)
                A(redisReaderFeed(r, cases[c].in, cut) == REDIS_OK);
            if (cut < cases[c].n)       /* full input would already finish */
                A(expectPending(r) == 0);
            if (cases[c].n - cut > 0)
                A(redisReaderFeed(r, cases[c].in + cut,
                                  cases[c].n - cut) == REDIS_OK);
            A(expectReply(r, cases[c].want) == 0);
            redisReaderFree(r);
        }
    }

    /* NUL bytes and protocol-looking bytes inside a blob payload. */
    {
        char buf[64];
        size_t n = 0;
        const char hdr[] = "$?\r\n;6\r\n";
        memcpy(buf + n, hdr, sizeof(hdr) - 1); n += sizeof(hdr) - 1;
        const unsigned char payload[6] = { 'A', 0, '\r', '\n', ';', '.' };
        memcpy(buf + n, payload, 6); n += 6;
        memcpy(buf + n, "\r\n", 2); n += 2;
        memcpy(buf + n, ";0\r\n", 4); n += 4;

        for (size_t stride = 1; stride <= 5; stride++) {
            redisReader *r = redisReaderCreate();
            redisReply *done = feedStrides(r, buf, n, stride);
            Node *want = mkStr(REDIS_REPLY_STRING, payload, 6);
            A(done != NULL);
            A(replyEqualsNode(done, want));
            freeNode(want);
            freeReplyObject(done);
            redisReaderFree(r);
        }
    }

    /* A message larger than the 1KB compaction threshold, fed in small
     * strides so r->pos crosses the sdsrange() boundary mid-stream. */
    {
        const size_t total = 2000;
        char *big = hi_malloc(total + 64);
        size_t n = 0;
        memcpy(big + n, "$?\r\n", 4); n += 4;
        char hdr[32];
        size_t hn = (size_t)snprintf(hdr, sizeof(hdr), ";%zu\r\n", total);
        memcpy(big + n, hdr, hn); n += hn;
        memset(big + n, 'Q', total); n += total;
        memcpy(big + n, "\r\n", 2); n += 2;
        memcpy(big + n, ";0\r\n", 4); n += 4;

        redisReader *r = redisReaderCreate();
        redisReply *done = feedStrides(r, big, n, 7);
        Node *want = mkLeaf(REDIS_REPLY_STRING);
        want->len = total;
        want->str = hi_malloc(total + 1);
        memset(want->str, 'Q', total);
        want->str[total] = '\0';
        A(done != NULL);
        A(replyEqualsNode(done, want));
        freeNode(want);
        freeReplyObject(done);
        redisReaderFree(r);
        hi_free(big);
    }

    /* A streamed container spanning the compaction boundary. */
    {
        size_t kids = 400;
        size_t cap = kids * 8 + 16;
        char *buf = hi_malloc(cap);
        size_t n = 0;
        memcpy(buf + n, "*?\r\n", 4); n += 4;
        for (size_t i = 0; i < kids; i++)
            n += (size_t)snprintf(buf + n, cap - n, ":%zu\r\n", i + 1);
        memcpy(buf + n, ".\r\n", 3); n += 3;

        redisReader *r = redisReaderCreate();
        redisReply *rep = feedStrides(r, buf, n, 13);
        A(rep != NULL && rep->type == REDIS_REPLY_ARRAY);
        A(rep->elements == kids);
        for (size_t i = 0; i < kids; i++)
            A(rep->element[i]->integer == (long long)(i + 1));
        freeReplyObject(rep);
        redisReaderFree(r);
        hi_free(buf);
    }

    freeNode(wantBlob); freeNode(wantArr); freeNode(wantMap);
    freeNode(wantSet); freeNode(wantEmptyBlob); freeNode(wantEmptyArr);
    return g_asserts;
}

/* ------------------------------------------------------------------ */
/* 2. Nesting, pipeline, empty streams, fixed RESP2/RESP3 controls     */
/* ------------------------------------------------------------------ */

/* Parse one complete input in one feed and return the reply. */
static redisReply *parseOne(const char *in, size_t n) {
    redisReader *r = redisReaderCreate();
    if (redisReaderFeed(r, in, n) != REDIS_OK) { redisReaderFree(r); return NULL; }
    void *p = NULL;
    if (redisReaderGetReply(r, &p) != REDIS_OK) { redisReaderFree(r); return NULL; }
    /* Keep the reply; reader must be freed separately. */
    redisReply *rep = p;
    redisReaderFree(r);
    return rep;
}

int contract_nested(void) {
    g_asserts = 0;

    /* Fixed RESP2 array control. */
    {
        const char *in = "*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
        redisReply *rep = parseOne(in, strlen(in));
        A(rep != NULL && rep->type == REDIS_REPLY_ARRAY && rep->elements == 2);
        A(rep->element[0]->len == 3 &&
          memcmp(rep->element[0]->str, "foo", 3) == 0);
        A(rep->element[1]->len == 3 &&
          memcmp(rep->element[1]->str, "bar", 3) == 0);
        freeReplyObject(rep);
    }

    /* Fixed RESP3 map control. */
    {
        const char *in = "%1\r\n+k\r\n*2\r\n:7\r\n$3\r\nfoo\r\n";
        redisReply *rep = parseOne(in, strlen(in));
        A(rep != NULL && rep->type == REDIS_REPLY_MAP && rep->elements == 2);
        A(rep->element[1]->type == REDIS_REPLY_ARRAY &&
          rep->element[1]->elements == 2);
        A(rep->element[1]->element[0]->integer == 7);
        freeReplyObject(rep);
    }

    /* Fixed container holding streamed values. */
    {
        const char *in =
            "*2\r\n"
              "$?\r\n;2\r\nhi\r\n;0\r\n"
              "*?\r\n:9\r\n.\r\n";
        Node *want = mkAgg(REDIS_REPLY_ARRAY, 2,
            mkStr(REDIS_REPLY_STRING, "hi", 2),
            mkAgg(REDIS_REPLY_ARRAY, 1, mkInt(9)));
        redisReader *r = redisReaderCreate();
        A(redisReaderFeed(r, in, strlen(in)) == REDIS_OK);
        A(expectReply(r, want) == 0);
        freeNode(want);
        redisReaderFree(r);
    }

    /* Streamed container holding fixed containers, several levels deep. */
    {
        const char *in =
            "*?\r\n"
              "*1\r\n:5\r\n"
              "%1\r\n+k\r\n$?\r\n;1\r\nz\r\n;0\r\n"
            ".\r\n";
        Node *want = mkAgg(REDIS_REPLY_ARRAY, 2,
            mkAgg(REDIS_REPLY_ARRAY, 1, mkInt(5)),
            mkAgg(REDIS_REPLY_MAP, 2,
                mkStr(REDIS_REPLY_STATUS, "k", 1),
                mkStr(REDIS_REPLY_STRING, "z", 1)));
        redisReader *r = redisReaderCreate();
        A(redisReaderFeed(r, in, strlen(in)) == REDIS_OK);
        A(expectReply(r, want) == 0);
        freeNode(want);
        redisReaderFree(r);
    }

    /* Streamed blob inside streamed map inside streamed array. */
    {
        const char *in =
            "*?\r\n"
              "%?\r\n"
                "+k\r\n$?\r\n;2\r\nab\r\n;2\r\ncd\r\n;0\r\n"
              ".\r\n"
            ".\r\n";
        Node *want = mkAgg(REDIS_REPLY_ARRAY, 1,
            mkAgg(REDIS_REPLY_MAP, 2,
                mkStr(REDIS_REPLY_STATUS, "k", 1),
                mkStr(REDIS_REPLY_STRING, "abcd", 4)));
        redisReader *r = redisReaderCreate();
        A(redisReaderFeed(r, in, strlen(in)) == REDIS_OK);
        A(expectReply(r, want) == 0);
        freeNode(want);
        redisReaderFree(r);
    }

    /* Closing an inner stream must not advance the outer stream twice. */
    {
        const char *in =
            "*?\r\n"
              "*?\r\n:1\r\n.\r\n"   /* inner: one child, then closes */
              ":2\r\n"             /* outer continues at index 1 */
            ".\r\n";
        Node *want = mkAgg(REDIS_REPLY_ARRAY, 2,
            mkAgg(REDIS_REPLY_ARRAY, 1, mkInt(1)),
            mkInt(2));
        redisReader *r = redisReaderCreate();
        A(redisReaderFeed(r, in, strlen(in)) == REDIS_OK);
        A(expectReply(r, want) == 0);
        freeNode(want);
        redisReaderFree(r);
    }

    /* Pipeline: after the streamed root completes, the next message in the
     * same buffer must still be readable. */
    {
        const char *in = "*?\r\n*1\r\n:5\r\n.\r\n+NEXT\r\n";
        redisReader *r = redisReaderCreate();
        A(redisReaderFeed(r, in, strlen(in)) == REDIS_OK);
        Node *want1 = mkAgg(REDIS_REPLY_ARRAY, 1,
            mkAgg(REDIS_REPLY_ARRAY, 1, mkInt(5)));
        A(expectReply(r, want1) == 0);
        freeNode(want1);
        Node *want2 = mkStr(REDIS_REPLY_STATUS, "NEXT", 4);
        A(expectReply(r, want2) == 0);
        freeNode(want2);
        A(expectPending(r) == 0);
        redisReaderFree(r);
    }

    /* Empty streamed containers of every aggregate kind. */
    {
        const char *inArr = "*?\r\n.\r\n";
        const char *inMap = "%?\r\n.\r\n";
        const char *inSet = "~?\r\n.\r\n";
        redisReader *r = redisReaderCreate();
        A(redisReaderFeed(r, inArr, strlen(inArr)) == REDIS_OK);
        A(expectReply(r, mkAgg(REDIS_REPLY_ARRAY, 0)) == 0);
        A(redisReaderFeed(r, inMap, strlen(inMap)) == REDIS_OK);
        A(expectReply(r, mkAgg(REDIS_REPLY_MAP, 0)) == 0);
        A(redisReaderFeed(r, inSet, strlen(inSet)) == REDIS_OK);
        A(expectReply(r, mkAgg(REDIS_REPLY_SET, 0)) == 0);
        redisReaderFree(r);
    }

    /* Streamed and fixed encodings produce equivalent trees. */
    {
        const char *fixed = "*2\r\n:1\r\n:2\r\n";
        const char *stream = "*?\r\n:1\r\n:2\r\n.\r\n";
        redisReply *a = parseOne(fixed, strlen(fixed));
        redisReply *b = parseOne(stream, strlen(stream));
        Node *want = mkAgg(REDIS_REPLY_ARRAY, 2, mkInt(1), mkInt(2));
        A(replyEqualsNode(a, want));
        A(replyEqualsNode(b, want));
        freeNode(want);
        freeReplyObject(a);
        freeReplyObject(b);
    }

    return g_asserts;
}

/* ------------------------------------------------------------------ */
/* 3. Custom stream callbacks: ordering, grow, OOM, sentinel mode      */
/* ------------------------------------------------------------------ */

#define LOG_CAP 256
static char *g_log[LOG_CAP];
static int g_logn;

static void logEvent(const char *fmt, ...) {
    if (g_logn >= LOG_CAP) return;
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log[g_logn++] = hi_strdup(buf);
}

static void clearLog(void) {
    for (int i = 0; i < g_logn; i++) hi_free(g_log[i]);
    g_logn = 0;
}

typedef struct CObj {
    int type;
    long long ival;
    char *str;
    size_t len;
    struct CObj **kid;
    size_t cap, n;
} CObj;

/* Link a freshly built object into its parent aggregate's slot. The reader
 * guarantees growAggregate() already made room for task->idx, so both the
 * parent object and its privdata are usable at construction time. Returns
 * 0 on success, -1 on an invariant violation. */
static int cAttach(const redisReadTask *t, CObj *o) {
    if (!t->parent) return 0;
    CObj *p = t->parent->obj;
    if (p == NULL || (size_t)t->idx >= p->cap) return -1;
    p->kid[t->idx] = o;
    if ((size_t)(t->idx + 1) > p->n) p->n = (size_t)(t->idx + 1);
    return 0;
}

static void cobjFree(void *p) {
    CObj *o = p;
    if (!o) return;
    for (size_t i = 0; i < o->n; i++) cobjFree(o->kid[i]);
    hi_free(o->kid);
    hi_free(o->str);
    hi_free(o);
}

static void *cCreateString(const redisReadTask *t, char *s, size_t len) {
    logEvent("string idx=%d len=%zu", t->idx, len);
    CObj *o = hi_calloc(1, sizeof(*o));
    o->type = t->type; o->len = len;
    o->str = hi_malloc(len + 1);
    memcpy(o->str, s, len); o->str[len] = '\0';
    if (cAttach(t, o) != 0) { cobjFree(o); return NULL; }
    return o;
}

static void *cCreateArray(const redisReadTask *t, size_t elements) {
    logEvent("array idx=%d slots=%zu", t->idx, elements);
    CObj *o = hi_calloc(1, sizeof(*o));
    o->type = t->type; o->cap = elements;
    if (elements) o->kid = hi_calloc(elements, sizeof(*o->kid));
    if (cAttach(t, o) != 0) { cobjFree(o); return NULL; }
    return o;
}

static void *cCreateInteger(const redisReadTask *t, long long v) {
    logEvent("integer idx=%d", t->idx);
    CObj *o = hi_calloc(1, sizeof(*o));
    o->type = REDIS_REPLY_INTEGER; o->ival = v;
    if (cAttach(t, o) != 0) { cobjFree(o); return NULL; }
    return o;
}

static void *cCreateNil(const redisReadTask *t) {
    logEvent("nil idx=%d", t->idx);
    CObj *o = hi_calloc(1, sizeof(*o));
    o->type = REDIS_REPLY_NIL;
    if (cAttach(t, o) != 0) { cobjFree(o); return NULL; }
    return o;
}

static void *cCreateBool(const redisReadTask *t, int b) {
    logEvent("bool idx=%d", t->idx);
    CObj *o = hi_calloc(1, sizeof(*o));
    o->type = REDIS_REPLY_BOOL; o->ival = b;
    if (cAttach(t, o) != 0) { cobjFree(o); return NULL; }
    return o;
}

/* When non-zero, growAggregate fails (simulated OOM). */
static int g_growFail;

/* When armed, growAggregate records the task's privdata for the test to
 * verify the parent task's obj/privdata are usable at growth time. */
static int g_checkPriv;
static void *g_seenPriv;

static int cGrow(const redisReadTask *t, size_t slots) {
    logEvent("grow idx=%d slots=%zu", t->idx, slots);
    if (g_checkPriv)
        g_seenPriv = t->privdata;
    if (g_growFail)
        return REDIS_ERR;
    CObj *o = t->obj;
    if (slots <= o->cap)
        return REDIS_OK;
    size_t ncap = o->cap ? o->cap * 2 : 2;
    if (ncap < slots) ncap = slots;
    CObj **nk = hi_realloc(o->kid, ncap * sizeof(*nk));
    if (!nk) return REDIS_ERR;
    o->kid = nk; o->cap = ncap;
    return REDIS_OK;
}

static redisReplyObjectFunctions cObjFn = {
    cCreateString, cCreateArray, cCreateInteger, NULL,
    cCreateNil, cCreateBool, cobjFree
};
static redisReplyStreamCallbacks cStreamFn = { cGrow };

/* Allocator helper used by the OOM tests: every reallocation fails. */
static void *always_fail_realloc(void *ptr, size_t size);

static int expectLog(const char *const *want, int n) {
    A(g_logn == n);
    for (int i = 0; i < n; i++)
        A(strcmp(g_log[i], want[i]) == 0);
    return 0;
}

int contract_callbacks(void) {
    g_asserts = 0;

    /* growAggregate precedes every child; close reports the exact count. */
    {
        clearLog();
        redisReader *r = redisReaderCreateWithFunctions(&cObjFn);
        redisReaderSetStreamCallbacks(r, &cStreamFn);
        const char *in = "*?\r\n:10\r\n:20\r\n.\r\n";
        A(redisReaderFeed(r, in, strlen(in)) == REDIS_OK);
        void *p = NULL;
        A(redisReaderGetReply(r, &p) == REDIS_OK);
        CObj *o = p;
        A(o != NULL && o->type == REDIS_REPLY_ARRAY);
        A(o->n == 2 && o->cap >= 2);          /* exact final slot count */
        A(o->kid[0]->type == REDIS_REPLY_INTEGER && o->kid[0]->ival == 10);
        A(o->kid[1]->type == REDIS_REPLY_INTEGER && o->kid[1]->ival == 20);
        const char *want[] = {
            "array idx=-1 slots=0",
            "grow idx=-1 slots=1",
            "integer idx=0",
            "grow idx=-1 slots=2",
            "integer idx=1",
            "grow idx=-1 slots=2",
        };
        A(expectLog(want, (int)(sizeof(want)/sizeof(want[0]))) == 0);
        cobjFree(p);
        redisReaderFree(r);
    }

    /* Map growth: slots count key+value entries; final count is even. */
    {
        clearLog();
        redisReader *r = redisReaderCreateWithFunctions(&cObjFn);
        redisReaderSetStreamCallbacks(r, &cStreamFn);
        const char *in = "%?\r\n+a\r\n:1\r\n.\r\n";
        A(redisReaderFeed(r, in, strlen(in)) == REDIS_OK);
        void *p = NULL;
        A(redisReaderGetReply(r, &p) == REDIS_OK);
        const char *want[] = {
            "array idx=-1 slots=0",
            "grow idx=-1 slots=1",
            "string idx=0 len=1",
            "grow idx=-1 slots=2",
            "integer idx=1",
            "grow idx=-1 slots=2",
        };
        A(expectLog(want, (int)(sizeof(want)/sizeof(want[0]))) == 0);
        cobjFree(p);
        redisReaderFree(r);
    }

    /* Nested streamed containers: parent grow fires before the child
     * container is constructed, and task->obj/privdata stay usable. */
    {
        clearLog();
        static char privMarker;
        g_seenPriv = NULL;
        g_checkPriv = 1;
        redisReader *r = redisReaderCreateWithFunctions(&cObjFn);
        redisReaderSetStreamCallbacks(r, &cStreamFn);
        r->privdata = &privMarker;   /* set directly to avoid the macro's int cast */
        const char *in = "*?\r\n*?\r\n:1\r\n.\r\n.\r\n";
        A(redisReaderFeed(r, in, strlen(in)) == REDIS_OK);
        void *p = NULL;
        A(redisReaderGetReply(r, &p) == REDIS_OK);
        g_checkPriv = 0;
        A(g_seenPriv == (void*)&privMarker);  /* privdata usable during growth */
        const char *want[] = {
            "array idx=-1 slots=0",   /* outer created */
            "grow idx=-1 slots=1",    /* outer grows a slot for inner array */
            "array idx=0 slots=0",    /* inner created */
            "grow idx=0 slots=1",     /* inner grows a slot for its integer */
            "integer idx=0",
            "grow idx=0 slots=1",     /* inner closes: exact 1 slot */
            "grow idx=-1 slots=1",    /* outer closes: exact 1 slot */
        };
        A(expectLog(want, (int)(sizeof(want)/sizeof(want[0]))) == 0);
        cobjFree(p);
        redisReaderFree(r);
    }

    /* growAggregate returning REDIS_ERR is treated as OOM and is sticky. */
    {
        clearLog();
        g_growFail = 1;
        redisReader *r = redisReaderCreateWithFunctions(&cObjFn);
        redisReaderSetStreamCallbacks(r, &cStreamFn);
        const char *in = "*?\r\n:1\r\n.\r\n";
        A(redisReaderFeed(r, in, strlen(in)) == REDIS_OK);
        void *p = NULL;
        A(redisReaderGetReply(r, &p) == REDIS_ERR);
        A(r->err == REDIS_ERR_OOM);
        A(redisReaderGetReply(r, &p) == REDIS_ERR);
        A(r->err == REDIS_ERR_OOM);
        g_growFail = 0;
        redisReaderFree(r);   /* must release attached state without crash */
    }

    /* A custom table with no grow ability: fixed aggregates still work but
     * a streamed aggregate is a clear protocol error. */
    {
        redisReader *r = redisReaderCreateWithFunctions(&cObjFn);
        const char *fixed = "*1\r\n:1\r\n";
        A(redisReaderFeed(r, fixed, strlen(fixed)) == REDIS_OK);
        void *p = NULL;
        A(redisReaderGetReply(r, &p) == REDIS_OK);
        cobjFree(p);

        const char *stream = "*?\r\n:1\r\n.\r\n";
        A(redisReaderFeed(r, stream, strlen(stream)) == REDIS_OK);
        A(redisReaderGetReply(r, &p) == REDIS_ERR);
        A(r->err == REDIS_ERR_PROTOCOL);
        redisReaderFree(r);
    }

    /* fn == NULL sentinel mode consumes streamed structures and never
     * dereferences an object. */
    {
        redisReader *r = redisReaderCreateWithFunctions(NULL);
        const char *in =
            "*?\r\n%?\r\n+a\r\n:1\r\n.\r\n$?\r\n;1\r\nz\r\n;0\r\n.\r\n";
        A(redisReaderFeed(r, in, strlen(in)) == REDIS_OK);
        void *p = NULL;
        A(redisReaderGetReply(r, &p) == REDIS_OK);
        A(p == (void*)REDIS_REPLY_ARRAY);
        /* A second, simple reply still works. */
        A(redisReaderFeed(r, ":42\r\n", 5) == REDIS_OK);
        p = NULL;
        A(redisReaderGetReply(r, &p) == REDIS_OK);
        A(p == (void*)REDIS_REPLY_INTEGER);
        redisReaderFree(r);
    }

    /* OOM through the existing allocator interface: force realloc to fail.
     * Growing the default reader's streamed element vector needs realloc, so
     * this surfaces as a sticky OOM error. */
    {
        hiredisAllocFuncs saved = hiredisAllocFns;
        hiredisAllocFuncs ha = {
            .mallocFn = saved.mallocFn,
            .callocFn = saved.callocFn,
            .reallocFn = always_fail_realloc,
            .strdupFn = saved.strdupFn,
            .freeFn = saved.freeFn,
        };
        redisReader *r = redisReaderCreate();
        A(r != NULL);
        const char *in = "*?\r\n:1\r\n:2\r\n.\r\n";
        /* Buffer the input with working allocators, then arm the realloc
         * failure so it is the element-vector growth that hits OOM. */
        A(redisReaderFeed(r, in, strlen(in)) == REDIS_OK);
        hiredisSetAllocators(&ha);
        void *p = NULL;
        A(redisReaderGetReply(r, &p) == REDIS_ERR);
        A(r->err == REDIS_ERR_OOM);
        A(redisReaderGetReply(r, &p) == REDIS_ERR);
        A(r->err == REDIS_ERR_OOM);
        hiredisResetAllocators();
        redisReaderFree(r);
    }

    clearLog();
    return g_asserts;
}

/* Allocator helper used by the OOM tests: every reallocation fails. */
static void *always_fail_realloc(void *ptr, size_t size) {
    (void)ptr; (void)size;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 4. Failure modes: protocol errors, limits, sticky errors, cleanup   */
/* ------------------------------------------------------------------ */

static int expectProtoError(const char *in, size_t n) {
    redisReader *r = redisReaderCreate();
    A(redisReaderFeed(r, in, n) == REDIS_OK);
    void *p = NULL;
    A(redisReaderGetReply(r, &p) == REDIS_ERR);
    A(r->err == REDIS_ERR_PROTOCOL);
    /* Sticky: the same error is reported again. */
    A(redisReaderGetReply(r, &p) == REDIS_ERR);
    A(r->err == REDIS_ERR_PROTOCOL);
    /* Further feeds are rejected once the reader has failed. */
    A(redisReaderFeed(r, "x", 1) == REDIS_ERR);
    redisReaderFree(r);
    return 0;
}

int contract_failure(void) {
    g_asserts = 0;

    A(expectProtoError(".\r\n", strlen(".\r\n")) == 0);                  /* top level */
    A(expectProtoError("*1\r\n.\r\n", strlen("*1\r\n.\r\n")) == 0);      /* in fixed */
    A(expectProtoError("%?\r\n+a\r\n:1\r\n+b\r\n.\r\n",
                       strlen("%?\r\n+a\r\n:1\r\n+b\r\n.\r\n")) == 0);   /* odd map */
    A(expectProtoError("$?\r\n;-1\r\n;0\r\n",
                       strlen("$?\r\n;-1\r\n;0\r\n")) == 0);             /* neg chunk */
    A(expectProtoError("$?\r\n;2\r\nabX\n;0\r\n",
                       strlen("$?\r\n;2\r\nabX\n;0\r\n")) == 0);         /* bad CRLF */
    A(expectProtoError("$?\r\nx\r\n;0\r\n",
                       strlen("$?\r\nx\r\n;0\r\n")) == 0);               /* bad hdr */
    A(expectProtoError("|?\r\n.\r\n", strlen("|?\r\n.\r\n")) == 0);      /* ATTR */
    A(expectProtoError(">?\r\n.\r\n", strlen(">?\r\n.\r\n")) == 0);      /* PUSH */
    A(expectProtoError("=?\r\n;0\r\n", strlen("=?\r\n;0\r\n")) == 0);    /* VERB */

    /* A close marker whose CRLF is split must wait, then fail cleanly. */
    {
        redisReader *r = redisReaderCreate();
        A(redisReaderFeed(r, "*1\r\n.", 6) == REDIS_OK);
        A(expectPending(r) == 0);
        A(redisReaderFeed(r, "\r\n", 2) == REDIS_OK);
        void *p = NULL;
        A(redisReaderGetReply(r, &p) == REDIS_ERR);
        A(r->err == REDIS_ERR_PROTOCOL);
        redisReaderFree(r);
    }

    /* Incomplete input never publishes a half reply. */
    {
        const char *parts[] = {
            "$?", "$?\r", "$?\r\n;3", "$?\r\n;3\r",
            "*?", "*?\r", "*?\r\n:1\r", "%?\r\n+a\r\n"
        };
        for (size_t i = 0; i < sizeof(parts)/sizeof(parts[0]); i++) {
            redisReader *r = redisReaderCreate();
            A(redisReaderFeed(r, parts[i], strlen(parts[i])) == REDIS_OK);
            A(expectPending(r) == 0);
            redisReaderFree(r);
        }
    }

    /* maxdepth constrains streamed containers like fixed ones. */
    {
        redisReader *r = redisReaderCreate();
        r->maxdepth = 2;
        const char *in = "*?\r\n*?\r\n*?\r\n:1\r\n.\r\n.\r\n.\r\n";
        A(redisReaderFeed(r, in, strlen(in)) == REDIS_OK);
        void *p = NULL;
        A(redisReaderGetReply(r, &p) == REDIS_ERR);
        A(r->err == REDIS_ERR_PROTOCOL);
        redisReaderFree(r);
    }

    /* maxelements: array/set by child count, map by pair count. */
    {
        redisReader *r = redisReaderCreate();
        r->maxelements = 2;
        A(redisReaderFeed(r, "*?\r\n:1\r\n:2\r\n:3\r\n.\r\n",
                           strlen("*?\r\n:1\r\n:2\r\n:3\r\n.\r\n")) == REDIS_OK);
        void *p = NULL;
        A(redisReaderGetReply(r, &p) == REDIS_ERR);
        A(r->err == REDIS_ERR_PROTOCOL);
        redisReaderFree(r);

        r = redisReaderCreate();
        r->maxelements = 2;
        A(redisReaderFeed(r,
            "%?\r\n+a\r\n:1\r\n+b\r\n:2\r\n+c\r\n:3\r\n.\r\n",
            strlen("%?\r\n+a\r\n:1\r\n+b\r\n:2\r\n+c\r\n:3\r\n.\r\n"))
            == REDIS_OK);
        p = NULL;
        A(redisReaderGetReply(r, &p) == REDIS_ERR);
        A(r->err == REDIS_ERR_PROTOCOL);
        redisReaderFree(r);

        /* Exactly at the limit is accepted. */
        r = redisReaderCreate();
        r->maxelements = 2;
        A(redisReaderFeed(r, "*?\r\n:1\r\n:2\r\n.\r\n",
                           strlen("*?\r\n:1\r\n:2\r\n.\r\n")) == REDIS_OK);
        p = NULL;
        A(redisReaderGetReply(r, &p) == REDIS_OK);
        A(((redisReply*)p)->elements == 2);
        freeReplyObject(p);
        redisReaderFree(r);
    }

    /* Reader free while a stream is half-assembled releases everything
     * (verified under ASan in the test harness). */
    {
        redisReader *r = redisReaderCreate();
        /* Blob awaiting its terminating chunk; the outer array is also open. */
        const char *partial = "*?\r\n:1\r\n$?\r\n;3\r\nfoo\r\n";
        A(redisReaderFeed(r, partial, strlen(partial)) == REDIS_OK);
        A(expectPending(r) == 0);
        redisReaderFree(r);
    }

    /* After an OOM mid-blob, later GetReply keeps returning the same error. */
    {
        hiredisAllocFuncs saved = hiredisAllocFns;
        hiredisAllocFuncs ha = {
            .mallocFn = saved.mallocFn,
            .callocFn = saved.callocFn,
            .reallocFn = always_fail_realloc,
            .strdupFn = saved.strdupFn,
            .freeFn = saved.freeFn,
        };

        redisReader *r = redisReaderCreate();
        A(r != NULL);
        /* The second chunk forces an sds growth realloc that fails. Buffer
         * the input first, then arm the failing realloc before parsing. */
        const char *in = "$?\r\n;1\r\na\r\n;1\r\nb\r\n;0\r\n";
        A(redisReaderFeed(r, in, strlen(in)) == REDIS_OK);
        hiredisSetAllocators(&ha);
        void *p = NULL;
        A(redisReaderGetReply(r, &p) == REDIS_ERR);
        A(r->err == REDIS_ERR_OOM);
        A(redisReaderGetReply(r, &p) == REDIS_ERR);
        A(r->err == REDIS_ERR_OOM);
        hiredisResetAllocators();
        redisReaderFree(r);
    }

    return g_asserts;
}
