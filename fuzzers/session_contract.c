/* session_contract.c — structured fuzz entry for response-session contracts.
 *
 * What is exercised here is NOT the parser's single-message grammar (the
 * upstream test.c covers that) but the contract between a stream of
 * responses and the queue of requests a host has outstanding on one
 * connection:
 *
 *  - 1xx interim responses must not pop the request they precede.
 *  - A response to HEAD carries no body even when it declares
 *    Content-Length / Transfer-Encoding; that skip decision must neither
 *    close the connection nor leak into the next response.
 *  - A successful (2xx) response to CONNECT hands the connection off at
 *    the end of the header block; every byte after that position is
 *    returned to the caller untouched, whatever it looks like.
 *  - A non-2xx response to CONNECT is an ordinary response with an
 *    ordinary body.
 *
 * The expected behaviour is computed from an independent plan model
 * (method queue + per-response framing), never from the parser's return
 * values or its state machine.  The parser is only asked to agree.
 *
 * Feeding: every plan is replayed as one block, byte by byte, through
 * every single cut point, and through fixed-seed multi-chunk splits.
 * Bytes past the handoff position are never offered to the HTTP parser;
 * the unconsumed part of the handoff chunk plus all unfed chunks must
 * reassemble into the planned tail.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

#include "http_parser.h"
#include "baseline/session_driver.h"

#define MAX_REQ   8
#define MAX_RESP  16
#define MAX_WIRE  4096
#define MAX_TAIL  128

/* connection header to emit */
enum { C_NONE, C_CLOSE, C_KEEP };
/* framing to emit */
enum { F_NONE, F_LEN, F_CHUNK, F_TE_GZIP };

struct resp_spec {
  int req;            /* owning request index */
  int status;         /* HTTP status code */
  int minor;          /* HTTP/1.<minor> */
  int conn;           /* C_* */
  int framing;        /* F_* */
  size_t body_len;    /* decoded body bytes on the wire; for HEAD this is
                         only the Content-Length value *declared* by the
                         response — no body bytes are ever put on the wire */
  int nchunks;        /* chunk count for F_CHUNK */
  int trailer;        /* emit a trailer header after the last chunk */
  /* derived expectations (independent model) */
  int is_final;       /* last response of its request */
  size_t exp_body;    /* body bytes the host must observe */
  int exp_keep_h;     /* keep-alive sampled inside on_headers_complete */
  int exp_keep_c;     /* keep-alive sampled inside on_message_complete */
};

struct plan {
  uint64_t seed;
  int nreq;
  int req_method[MAX_REQ];
  int nresp;
  struct resp_spec resp[MAX_RESP];
  uint8_t tail[MAX_TAIL];   /* bytes after the handoff position */
  size_t tail_len;
  /* encoded */
  uint8_t wire[MAX_WIRE];
  size_t wire_len;
  size_t handoff;           /* offset where HTTP ends; == wire_len if none */
  int has_handoff;
  int eof_last;             /* last response body ends at EOF */
};

/* ---------------------------------------------------------------- model */

static int resp_is_final(const struct plan *pl, int i)
{
  return i == pl->nresp - 1 || pl->resp[i + 1].req != pl->resp[i].req;
}

static int resp_method(const struct plan *pl, int i)
{
  return pl->req_method[pl->resp[i].req];
}

/* Does the message need connection close to delimit it?
 * with_skip=0: as visible inside on_headers_complete before the host's
 *              return value is acted on (the skip-body decision is not
 *              included yet).
 * with_skip=1: as visible inside on_message_complete.
 */
static int model_needs_eof(const struct plan *pl, int i, int with_skip)
{
  const struct resp_spec *r = &pl->resp[i];
  int m;
  if (r->status / 100 == 1 || r->status == 204 || r->status == 304)
    return 0;
  m = resp_method(pl, i);
  if (with_skip) {
    if (m == HTTP_HEAD) return 0;
    if (m == HTTP_CONNECT && r->status / 100 == 2) return 0;
  }
  if (r->framing == F_LEN || r->framing == F_CHUNK) return 0;
  return 1; /* no length, or non-chunked Transfer-Encoding: read to EOF */
}

static int model_keep(const struct plan *pl, int i, int with_skip)
{
  const struct resp_spec *r = &pl->resp[i];
  int base;
  if (r->minor >= 1)
    base = (r->conn != C_CLOSE);    /* HTTP/1.1 keeps unless told otherwise */
  else
    base = (r->conn == C_KEEP);     /* HTTP/1.0 drops unless told otherwise */
  return base && !model_needs_eof(pl, i, with_skip);
}

/* deterministic body content owned by the plan, not by the parser */
static uint8_t body_byte(int resp, size_t i)
{
  return (uint8_t)(resp * 67 + (int)(i % 251) * 31 + (int)(i / 251) + 11);
}

/* ------------------------------------------------------------------ wire */

static const char *reason_phrase(int status)
{
  switch (status) {
    case 100: return "Continue";
    case 103: return "Early Hints";
    case 200: return "OK";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 304: return "Not Modified";
    case 407: return "Proxy Authentication Required";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default:  return "X";
  }
}

static int emit(struct plan *pl, const char *s)
{
  size_t n = strlen(s);
  if (pl->wire_len + n > MAX_WIRE) return -1;
  memcpy(pl->wire + pl->wire_len, s, n);
  pl->wire_len += n;
  return 0;
}

static int emitf(struct plan *pl, const char *fmt, ...)
{
  char buf[128];
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= sizeof buf) return -1;
  if (pl->wire_len + (size_t)n > MAX_WIRE) return -1;
  memcpy(pl->wire + pl->wire_len, buf, (size_t)n);
  pl->wire_len += (size_t)n;
  return 0;
}

static int encode_wire(struct plan *pl)
{
  int i;
  pl->wire_len = 0;
  pl->handoff = 0;
  for (i = 0; i < pl->nresp; i++) {
    struct resp_spec *r = &pl->resp[i];
    int m = resp_method(pl, i);
    int skip_wire_body =
        !r->is_final || m == HTTP_HEAD ||
        (m == HTTP_CONNECT && r->status / 100 == 2);
    if (emitf(pl, "HTTP/1.%d %d %s\r\n", r->minor, r->status,
              reason_phrase(r->status))) return -1;
    if (r->conn == C_CLOSE  && emit(pl, "Connection: close\r\n")) return -1;
    if (r->conn == C_KEEP   && emit(pl, "Connection: keep-alive\r\n")) return -1;
    switch (r->framing) {
      case F_LEN:
        if (emitf(pl, "Content-Length: %u\r\n", (unsigned)r->body_len))
          return -1;
        break;
      case F_CHUNK:
        if (emit(pl, "Transfer-Encoding: chunked\r\n")) return -1;
        break;
      case F_TE_GZIP:
        if (emit(pl, "Transfer-Encoding: gzip\r\n")) return -1;
        break;
      default: break;
    }
    if (emit(pl, "\r\n")) return -1;
    if (m == HTTP_CONNECT && r->is_final && r->status / 100 == 2) {
      /* the connection is handed off exactly here */
      pl->handoff = pl->wire_len;
      pl->has_handoff = 1;
      continue;
    }
    if (skip_wire_body) continue;
    if (r->framing == F_CHUNK) {
      size_t left = r->body_len;
      int nch = r->nchunks > 0 ? r->nchunks : 1;
      int k;
      size_t off = 0;
      if ((size_t)nch > left) nch = (int)left; /* no 0-size data chunks */
      for (k = 0; k < nch; k++) {
        size_t sz = left / (size_t)(nch - k);
        size_t j;
        if (emitf(pl, "%x%s\r\n", (unsigned)sz,
                  (k == 0 && (i & 1)) ? ";v=1" : "")) return -1;
        if (pl->wire_len + sz + 2 > MAX_WIRE) return -1;
        for (j = 0; j < sz; j++)
          pl->wire[pl->wire_len + j] = body_byte(i, off + j);
        pl->wire_len += sz;
        if (emit(pl, "\r\n")) return -1;
        left -= sz;
        off += sz;
      }
      if (emit(pl, "0\r\n")) return -1;
      if (r->trailer && emit(pl, "X-End: yes\r\n")) return -1;
      if (emit(pl, "\r\n")) return -1;
    } else {
      size_t j;
      if (pl->wire_len + r->body_len > MAX_WIRE) return -1;
      for (j = 0; j < r->body_len; j++)
        pl->wire[pl->wire_len + j] = body_byte(i, j);
      pl->wire_len += r->body_len;
    }
  }
  if (!pl->has_handoff) pl->handoff = pl->wire_len;
  if (pl->wire_len + pl->tail_len > MAX_WIRE) return -1;
  memcpy(pl->wire + pl->wire_len, pl->tail, pl->tail_len);
  pl->wire_len += pl->tail_len;
  return 0;
}

/* ------------------------------------------------------------- plan build */

static void plan_init(struct plan *pl, uint64_t seed)
{
  memset(pl, 0, sizeof *pl);
  pl->seed = seed;
}

static int plan_add_req(struct plan *pl, int method)
{
  if (pl->nreq >= MAX_REQ) return -1;
  pl->req_method[pl->nreq] = method;
  return pl->nreq++;
}

static int plan_add_resp(struct plan *pl, int req, int status, int minor,
                         int conn, int framing, size_t body_len,
                         int nchunks, int trailer)
{
  struct resp_spec *r;
  if (pl->nresp >= MAX_RESP || req < 0 || req >= pl->nreq) return -1;
  r = &pl->resp[pl->nresp++];
  memset(r, 0, sizeof *r);
  r->req = req;
  r->status = status;
  r->minor = minor;
  r->conn = conn;
  r->framing = framing;
  r->body_len = body_len;
  r->nchunks = nchunks;
  r->trailer = trailer;
  return 0;
}

static int plan_set_tail(struct plan *pl, const void *bytes, size_t n)
{
  if (n > MAX_TAIL) return -1;
  memcpy(pl->tail, bytes, n);
  pl->tail_len = n;
  return 0;
}

/* validate the plan and derive the independent expectations */
static int plan_finalize(struct plan *pl)
{
  int i;
  if (pl->nreq < 1 || pl->nresp < 1) return -1;
  for (i = 0; i < pl->nresp; i++) {
    struct resp_spec *r = &pl->resp[i];
    int m = resp_method(pl, i);
    r->is_final = resp_is_final(pl, i);
    if (!r->is_final) {
      /* interim responses: 1xx, no framing, no body, never pop */
      if (r->status != 100 && r->status != 103) return -1;
      if (r->framing != F_NONE || r->body_len != 0) return -1;
    }
    if (m == HTTP_CONNECT && r->is_final && r->status / 100 == 2) {
      if (i != pl->nresp - 1) return -1;  /* handoff ends the HTTP stream */
      if (r->framing != F_NONE) return -1;
      pl->has_handoff = 1;
    }
    r->exp_body = (!r->is_final || m == HTTP_HEAD ||
                   (m == HTTP_CONNECT && r->status / 100 == 2))
                      ? 0 : r->body_len;
    r->exp_keep_h = model_keep(pl, i, 0);
    r->exp_keep_c = model_keep(pl, i, 1);
    /* a response after which the model drops the connection must be last */
    if (i != pl->nresp - 1 && !r->exp_keep_c) return -1;
  }
  pl->eof_last = !pl->has_handoff && model_needs_eof(pl, pl->nresp - 1, 1);
  return encode_wire(pl);
}

/* ------------------------------------------------------------ observation */

struct host {
  const struct plan *pl;
  http_parser p;
  int qhead;                 /* host request queue: index of oldest
                                unanswered request */
  int seen;                  /* on_headers_complete count */
  int done;                  /* on_message_complete count */
  int status[MAX_RESP];
  int keep_h[MAX_RESP];      /* keep-alive sampled in on_headers_complete */
  int keep_c[MAX_RESP];      /* keep-alive sampled in on_message_complete */
  size_t blen[MAX_RESP];
  int bbad[MAX_RESP];
  int assoc_bad;
  int handed;                /* upgrade reported at message_complete */
  int err;                   /* first parser errno recorded */
  size_t consumed;           /* bytes accepted by the HTTP parser */
  uint8_t leftover[MAX_WIRE];
  size_t leftover_len;       /* bytes never offered to / refused by HTTP */
};

static int hs_headers(http_parser *p)
{
  struct host *h = (struct host *)p->data;
  const struct plan *pl = h->pl;
  int i = h->seen;
  int method, st;
  if (i < MAX_RESP) {
    h->status[i] = (int)p->status_code;
    /* sampled before our return value can influence the parser: this
     * cannot yet include the skip-body decision we are about to make */
    h->keep_h[i] = http_should_keep_alive(p);
  }
  h->seen++;
  if (h->qhead >= pl->nreq) { h->assoc_bad = 1; return 0; }
  method = pl->req_method[h->qhead];   /* method comes from the host queue */
  st = (int)p->status_code;
  if (st >= 100 && st < 200) return 0; /* interim: request stays queued */
  if (method == HTTP_HEAD) return 1;   /* HEAD: never a body on the wire */
  if (method == HTTP_CONNECT && st / 100 == 2) return 2; /* hand off */
  return 0;
}

static int hs_body(http_parser *p, const char *at, size_t n)
{
  struct host *h = (struct host *)p->data;
  int i = h->seen - 1;   /* body of the response currently in flight */
  size_t k;
  if (i < 0 || i >= MAX_RESP) return 0;
  for (k = 0; k < n; k++) {
    if (h->blen[i] + k >= MAX_WIRE ||
        (uint8_t)at[k] != body_byte(i, h->blen[i] + k))
      h->bbad[i]++;
  }
  h->blen[i] += n;
  return 0;
}

static int hs_complete(http_parser *p)
{
  struct host *h = (struct host *)p->data;
  const struct plan *pl = h->pl;
  int i = h->done;
  if (i < MAX_RESP)
    h->keep_c[i] = http_should_keep_alive(p);
  h->done++;
  if (i < pl->nresp) {
    if (pl->resp[i].req != h->qhead) h->assoc_bad = 1;
    if (pl->resp[i].is_final) h->qhead++;  /* only final responses pop */
  } else {
    h->assoc_bad = 1;
  }
  if (p->upgrade) h->handed = 1;
  return 0;
}

/* -------------------------------------------------------------- reporting */

static void dump_hex(const uint8_t *b, size_t n)
{
  size_t i;
  for (i = 0; i < n; i++) fprintf(stderr, "%02x", b[i]);
  if (n == 0) fprintf(stderr, "(empty)");
  fprintf(stderr, "\n");
}

static void dump_plan(const struct plan *pl)
{
  int i, j;
  fprintf(stderr, "plan seed=%llu nreq=%d nresp=%d handoff=%zu tail=%zu\n",
          (unsigned long long)pl->seed, pl->nreq, pl->nresp,
          pl->has_handoff ? pl->handoff : (size_t)-1, pl->tail_len);
  for (i = 0; i < pl->nreq; i++) {
    fprintf(stderr, "  req#%d method=%s resps=", i,
            http_method_str((enum http_method)pl->req_method[i]));
    for (j = 0; j < pl->nresp; j++)
      if (pl->resp[j].req == i) fprintf(stderr, "%d ", j);
    fprintf(stderr, "\n");
  }
  for (j = 0; j < pl->nresp; j++) {
    const struct resp_spec *r = &pl->resp[j];
    fprintf(stderr,
            "  resp#%d req=%d status=%d http/1.%d conn=%d framing=%d "
            "body=%zu final=%d keep_h=%d keep_c=%d\n",
            j, r->req, r->status, r->minor, r->conn, r->framing,
            r->body_len, r->is_final, r->exp_keep_h, r->exp_keep_c);
  }
  fprintf(stderr, "wire(%zu)=", pl->wire_len);
  dump_hex(pl->wire, pl->wire_len);
}

static const char *g_case = "?";

static int deviate(const struct plan *pl, const char *feed,
                   const char *kind, const char *detail)
{
  fprintf(stderr, "SESSION_DEVIATION case=%s kind=%s feed=%s detail=%s\n",
          g_case, kind, feed, detail);
  dump_plan(pl);
  return -1;
}

/* ---------------------------------------------------------------- feeding */

static int exec_plan(const struct plan *pl, const char *feed,
                     const size_t *off, const size_t *ln, size_t nchunk)
{
  struct host h;
  http_parser_settings st;
  size_t c;
  int i;
  char detail[160];

  memset(&h, 0, sizeof h);
  h.pl = pl;
  http_parser_settings_init(&st);
  st.on_headers_complete = hs_headers;
  st.on_body = hs_body;
  st.on_message_complete = hs_complete;
  http_parser_init(&h.p, HTTP_RESPONSE);
  h.p.data = &h;

  for (c = 0; c < nchunk; c++) {
    const char *d = (const char *)pl->wire + off[c];
    size_t n = ln[c];
    if (n == 0) continue;      /* an empty execute would signal EOF */
    if (h.handed || h.err) {
      /* past the handoff (or dead): these bytes are not HTTP anymore */
      if (h.leftover_len + n <= sizeof h.leftover) {
        memcpy(h.leftover + h.leftover_len, d, n);
        h.leftover_len += n;
      }
      continue;
    }
    {
      size_t np = http_parser_execute(&h.p, &st, d, n);
      h.consumed += np;
      if (HTTP_PARSER_ERRNO(&h.p) != HPE_OK)
        h.err = (int)HTTP_PARSER_ERRNO(&h.p);
      if (np < n && h.leftover_len + (n - np) <= sizeof h.leftover) {
        memcpy(h.leftover + h.leftover_len, d + np, n - np);
        h.leftover_len += n - np;
      }
    }
  }
  if (!h.err && !h.handed && pl->eof_last) {
    http_parser_execute(&h.p, &st, NULL, 0);   /* signal EOF */
    if (HTTP_PARSER_ERRNO(&h.p) != HPE_OK)
      h.err = (int)HTTP_PARSER_ERRNO(&h.p);
  }

  /* The checks below are ordered so that a regression in response
   * chaining surfaces as content/completion/tail evidence before any
   * bare keep-alive boolean is compared. */

  if (h.done != pl->nresp || h.seen != pl->nresp || h.qhead != pl->nreq) {
    snprintf(detail, sizeof detail,
             "completions done=%d seen=%d qhead=%d want nresp=%d nreq=%d",
             h.done, h.seen, h.qhead, pl->nresp, pl->nreq);
    return deviate(pl, feed, "completion", detail);
  }
  for (i = 0; i < pl->nresp; i++) {
    if (h.status[i] != pl->resp[i].status) {
      snprintf(detail, sizeof detail, "resp#%d status=%d want=%d",
               i, h.status[i], pl->resp[i].status);
      return deviate(pl, feed, "association", detail);
    }
  }
  if (h.assoc_bad)
    return deviate(pl, feed, "association", "request queue misaligned");
  for (i = 0; i < pl->nresp; i++) {
    if (h.blen[i] != pl->resp[i].exp_body || h.bbad[i]) {
      snprintf(detail, sizeof detail, "resp#%d body=%zu bad=%d want=%zu",
               i, h.blen[i], h.bbad[i], pl->resp[i].exp_body);
      return deviate(pl, feed, "body", detail);
    }
  }
  if (pl->has_handoff && !h.handed)
    return deviate(pl, feed, "handoff", "upgrade never reported");
  if (!pl->has_handoff && h.handed)
    return deviate(pl, feed, "handoff", "unexpected upgrade");
  if (h.consumed != pl->handoff ||
      h.leftover_len != pl->tail_len ||
      memcmp(h.leftover, pl->tail, pl->tail_len) != 0) {
    snprintf(detail, sizeof detail,
             "consumed=%zu leftover=%zu want handoff=%zu tail=%zu",
             h.consumed, h.leftover_len, pl->handoff, pl->tail_len);
    return deviate(pl, feed, "tail", detail);
  }
  for (i = 0; i < pl->nresp; i++) {
    if (h.keep_c[i] != pl->resp[i].exp_keep_c) {
      snprintf(detail, sizeof detail, "resp#%d keep@complete=%d want=%d",
               i, h.keep_c[i], pl->resp[i].exp_keep_c);
      return deviate(pl, feed, "keepalive", detail);
    }
  }
  for (i = 0; i < pl->nresp; i++) {
    if (h.keep_h[i] != pl->resp[i].exp_keep_h) {
      snprintf(detail, sizeof detail, "resp#%d keep@headers=%d want=%d",
               i, h.keep_h[i], pl->resp[i].exp_keep_h);
      return deviate(pl, feed, "keepalive", detail);
    }
  }
  if (h.err) {
    snprintf(detail, sizeof detail, "parser error=%s",
             http_errno_name((enum http_errno)h.err));
    return deviate(pl, feed, "error", detail);
  }
  return 0;
}

static size_t mkchunks(const struct plan *pl, int mode, size_t cut,
                       uint64_t cseed, size_t *off, size_t *ln)
{
  size_t n = pl->wire_len, pos = 0, nc = 0;
  if (mode == 0) {            /* whole */
    off[0] = 0; ln[0] = n; return 1;
  }
  if (mode == 1) {            /* byte by byte */
    while (pos < n) { off[nc] = pos; ln[nc] = 1; nc++; pos++; }
    return nc;
  }
  if (mode == 2) {            /* single cut */
    off[0] = 0;   ln[0] = cut;
    off[1] = cut; ln[1] = n - cut;
    return 2;
  }
  /* fixed-seed multi-chunk split */
  {
    uint64_t rs = cseed ? cseed : 0x9e3779b97f4a7c15ULL;
    while (pos < n) {
      size_t sz;
      rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
      sz = 1 + (size_t)(rs % 37);
      if (sz > n - pos) sz = n - pos;
      off[nc] = pos; ln[nc] = sz; nc++; pos += sz;
    }
    return nc;
  }
}

static int run_all(const struct plan *pl, const char *casename)
{
  static size_t off[MAX_WIRE + 2], ln[MAX_WIRE + 2];
  size_t nc, cut;
  char label[64];
  int rc;

  g_case = casename;
  nc = mkchunks(pl, 0, 0, 0, off, ln);
  if ((rc = exec_plan(pl, "whole", off, ln, nc))) return rc;

  nc = mkchunks(pl, 1, 0, 0, off, ln);
  if ((rc = exec_plan(pl, "bytes", off, ln, nc))) return rc;

  for (cut = 1; cut < pl->wire_len; cut++) {
    snprintf(label, sizeof label, "cut=%zu", cut);
    nc = mkchunks(pl, 2, cut, 0, off, ln);
    if ((rc = exec_plan(pl, label, off, ln, nc))) return rc;
  }

  {
    uint64_t seeds[2];
    int s;
    seeds[0] = pl->seed * 2 + 1;
    seeds[1] = 0xdeadbeefcafef00dULL ^ pl->seed;
    for (s = 0; s < 2; s++) {
      snprintf(label, sizeof label, "chunks=%llu",
               (unsigned long long)seeds[s]);
      nc = mkchunks(pl, 3, 0, seeds[s], off, ln);
      if ((rc = exec_plan(pl, label, off, ln, nc))) return rc;
    }
  }
  return 0;
}

/* ------------------------------------------------------------- fixed cases */

#define CASE_OK  1
#define CASE_BAD (-1)

static int run_case(struct plan *pl, const char *name)
{
  if (plan_finalize(pl)) {
    fprintf(stderr, "invalid plan in case %s\n", name);
    return CASE_BAD;
  }
  return run_all(pl, name) ? CASE_BAD : CASE_OK;
}

/* -- association: interim responses and method/response correspondence -- */

static int case_assoc_interim_head_get(void)
{
  struct plan pl;
  int r;
  plan_init(&pl, 0xA001);
  r = plan_add_req(&pl, HTTP_HEAD);
  plan_add_resp(&pl, r, 100, 1, C_NONE, F_NONE, 0, 0, 0);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_LEN, 8, 0, 0); /* declares CL:8 */
  r = plan_add_req(&pl, HTTP_GET);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_LEN, 4, 0, 0);
  return run_case(&pl, "assoc_interim_head_get");
}

static int case_assoc_interim_connect(void)
{
  struct plan pl;
  int r;
  plan_init(&pl, 0xA002);
  r = plan_add_req(&pl, HTTP_CONNECT);
  plan_add_resp(&pl, r, 103, 1, C_NONE, F_NONE, 0, 0, 0);
  plan_add_resp(&pl, r, 407, 1, C_NONE, F_LEN, 5, 0, 0);
  r = plan_add_req(&pl, HTTP_GET);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_CHUNK, 7, 2, 1);
  return run_case(&pl, "assoc_interim_connect");
}

static int case_assoc_get_eof_tail(void)
{
  struct plan pl;
  int r;
  plan_init(&pl, 0xA003);
  r = plan_add_req(&pl, HTTP_GET);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_LEN, 3, 0, 0);
  r = plan_add_req(&pl, HTTP_GET);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_NONE, 4, 0, 0); /* body till EOF */
  return run_case(&pl, "assoc_get_eof_tail");
}

static int case_assoc_multi_interim(void)
{
  struct plan pl;
  int r;
  plan_init(&pl, 0xA004);
  r = plan_add_req(&pl, HTTP_GET);
  plan_add_resp(&pl, r, 100, 1, C_NONE, F_NONE, 0, 0, 0);
  plan_add_resp(&pl, r, 100, 1, C_NONE, F_NONE, 0, 0, 0);
  plan_add_resp(&pl, r, 103, 1, C_NONE, F_NONE, 0, 0, 0);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_CHUNK, 9, 3, 1);
  r = plan_add_req(&pl, HTTP_HEAD);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_TE_GZIP, 0, 0, 0);
  r = plan_add_req(&pl, HTTP_GET);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_LEN, 0, 0, 0);
  return run_case(&pl, "assoc_multi_interim");
}

static int case_assoc_get_te_gzip_eof(void)
{
  struct plan pl;
  int r;
  plan_init(&pl, 0xA005);
  r = plan_add_req(&pl, HTTP_GET);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_TE_GZIP, 6, 0, 0);
  return run_case(&pl, "assoc_get_te_gzip_eof");
}

/* -- head_reuse: the connection must survive HEAD responses ------------ */

static int case_head_no_length_then_get(void)
{
  struct plan pl;
  int r;
  plan_init(&pl, 0xB001);
  r = plan_add_req(&pl, HTTP_HEAD);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_NONE, 0, 0, 0);
  r = plan_add_req(&pl, HTTP_GET);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_LEN, 5, 0, 0);
  return run_case(&pl, "head_no_length_then_get");
}

static int case_head_te_gzip_then_get(void)
{
  struct plan pl;
  int r;
  plan_init(&pl, 0xB002);
  r = plan_add_req(&pl, HTTP_HEAD);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_TE_GZIP, 0, 0, 0);
  r = plan_add_req(&pl, HTTP_GET);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_LEN, 3, 0, 0);
  return run_case(&pl, "head_te_gzip_then_get");
}

static int case_head_cl_then_get(void)
{
  struct plan pl;
  int r;
  plan_init(&pl, 0xB003);
  r = plan_add_req(&pl, HTTP_HEAD);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_LEN, 9, 0, 0); /* declares CL:9 */
  r = plan_add_req(&pl, HTTP_GET);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_CHUNK, 6, 2, 1);
  return run_case(&pl, "head_cl_then_get");
}

static int case_head_chunked_then_get(void)
{
  struct plan pl;
  int r;
  plan_init(&pl, 0xB004);
  r = plan_add_req(&pl, HTTP_HEAD);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_CHUNK, 0, 0, 0);
  r = plan_add_req(&pl, HTTP_GET);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_LEN, 4, 0, 0);
  return run_case(&pl, "head_chunked_then_get");
}

static int case_head_close(void)
{
  struct plan pl;
  int r;
  plan_init(&pl, 0xB005);
  r = plan_add_req(&pl, HTTP_HEAD);
  plan_add_resp(&pl, r, 200, 1, C_CLOSE, F_NONE, 0, 0, 0);
  return run_case(&pl, "head_close");
}

static int case_head_http10_keep(void)
{
  struct plan pl;
  int r;
  plan_init(&pl, 0xB006);
  r = plan_add_req(&pl, HTTP_HEAD);
  plan_add_resp(&pl, r, 200, 0, C_KEEP, F_NONE, 0, 0, 0);
  r = plan_add_req(&pl, HTTP_GET);
  plan_add_resp(&pl, r, 200, 0, C_KEEP, F_LEN, 2, 0, 0);
  return run_case(&pl, "head_http10_keep");
}

static int case_head_http10_close(void)
{
  struct plan pl;
  int r;
  plan_init(&pl, 0xB007);
  r = plan_add_req(&pl, HTTP_HEAD);
  plan_add_resp(&pl, r, 200, 0, C_NONE, F_NONE, 0, 0, 0);
  return run_case(&pl, "head_http10_close");
}

static int case_head_interim_then_get(void)
{
  struct plan pl;
  int r;
  plan_init(&pl, 0xB008);
  r = plan_add_req(&pl, HTTP_HEAD);
  plan_add_resp(&pl, r, 103, 1, C_NONE, F_NONE, 0, 0, 0);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_LEN, 7, 0, 0);
  r = plan_add_req(&pl, HTTP_GET);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_LEN, 3, 0, 0);
  return run_case(&pl, "head_interim_then_get");
}

static int case_head_then_get_eof(void)
{
  struct plan pl;
  int r;
  plan_init(&pl, 0xB009);
  r = plan_add_req(&pl, HTTP_HEAD);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_LEN, 5, 0, 0);
  r = plan_add_req(&pl, HTTP_GET);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_NONE, 6, 0, 0); /* EOF-delimited */
  return run_case(&pl, "head_then_get_eof");
}

/* -- handoff: CONNECT 2xx stops HTTP at the header boundary ------------- */

static int case_handoff_nul_tail(void)
{
  struct plan pl;
  int r;
  static const uint8_t tail[] = {0x00, 0x01, 0x00, 0x02};
  plan_init(&pl, 0xC001);
  r = plan_add_req(&pl, HTTP_GET);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_LEN, 2, 0, 0);
  r = plan_add_req(&pl, HTTP_CONNECT);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_NONE, 0, 0, 0);
  plan_set_tail(&pl, tail, sizeof tail);
  return run_case(&pl, "handoff_nul_tail");
}

static int case_handoff_crlf_tail(void)
{
  struct plan pl;
  int r;
  plan_init(&pl, 0xC002);
  r = plan_add_req(&pl, HTTP_CONNECT);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_NONE, 0, 0, 0);
  plan_set_tail(&pl, "\r\n\r\n", 4);
  return run_case(&pl, "handoff_crlf_tail");
}

static int case_handoff_status_tail(void)
{
  struct plan pl;
  int r;
  static const char tail[] = "HTTP/1.1 500 Nope\r\nContent-Length: 9\r\n\r\n";
  plan_init(&pl, 0xC003);
  r = plan_add_req(&pl, HTTP_HEAD);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_LEN, 4, 0, 0);
  r = plan_add_req(&pl, HTTP_CONNECT);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_NONE, 0, 0, 0);
  plan_set_tail(&pl, tail, sizeof(tail) - 1);
  return run_case(&pl, "handoff_status_tail");
}

static int case_handoff_after_chunked(void)
{
  struct plan pl;
  int r;
  static const char tail[] = "GET / HTTP/1.1\r\n\r\n";
  plan_init(&pl, 0xC004);
  r = plan_add_req(&pl, HTTP_GET);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_CHUNK, 8, 3, 1);
  r = plan_add_req(&pl, HTTP_CONNECT);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_NONE, 0, 0, 0);
  plan_set_tail(&pl, tail, sizeof(tail) - 1);
  return run_case(&pl, "handoff_after_chunked");
}

static int case_handoff_non2xx_connect(void)
{
  struct plan pl;
  int r;
  plan_init(&pl, 0xC005);
  r = plan_add_req(&pl, HTTP_CONNECT);
  plan_add_resp(&pl, r, 503, 1, C_NONE, F_LEN, 6, 0, 0);
  r = plan_add_req(&pl, HTTP_GET);
  plan_add_resp(&pl, r, 200, 1, C_NONE, F_LEN, 2, 0, 0);
  return run_case(&pl, "handoff_non2xx_connect");
}

/* ------------------------------------------------------- generated plans */

static uint64_t g_rs;
static uint32_t rnd32(void)
{
  g_rs ^= g_rs << 13; g_rs ^= g_rs >> 7; g_rs ^= g_rs << 17;
  return (uint32_t)(g_rs >> 11);
}

/* Bounded model: <= 8 requests, <= 16 responses, <= 4096 wire bytes.
 * Any response after which the model would drop or hand off the
 * connection is forced to be the last one. */
static int gen_plan(struct plan *pl, uint64_t seed)
{
  int nreq, i, end_after = 0;
  plan_init(pl, seed);
  g_rs = seed ? seed : 0x9e3779b97f4a7c15ULL;
  nreq = 1 + (int)(rnd32() % MAX_REQ);
  for (i = 0; i < nreq && !end_after; i++) {
    static const int k_method[3] = {HTTP_GET, HTTP_HEAD, HTTP_CONNECT};
    int mid = (int)(rnd32() % 3);    /* 0=GET 1=HEAD 2=CONNECT */
    int method = k_method[mid];
    int req = plan_add_req(pl, method);
    int ninterim, k, minor, conn, framing, status;
    size_t body;
    if (req < 0) return -1;
    ninterim = (int)(rnd32() % 3);
    for (k = 0; k < ninterim && pl->nresp < MAX_RESP - 1; k++)
      plan_add_resp(pl, req, (rnd32() & 1) ? 100 : 103, 1, C_NONE,
                    F_NONE, 0, 0, 0);
    minor = (rnd32() % 4 == 0) ? 0 : 1;
    conn = C_NONE;
    if (minor == 0) conn = (rnd32() & 1) ? C_KEEP : C_NONE;
    else if (rnd32() % 8 == 0) conn = C_CLOSE;
    if (conn == C_CLOSE || (minor == 0 && conn != C_KEEP))
      end_after = 1;                 /* connection ends with this response */
    status = 200;
    framing = F_LEN;
    body = rnd32() % 49;
    if (mid == 1) {                  /* HEAD */
      framing = (int)(rnd32() % 4);  /* NONE/LEN/CHUNK/TE_GZIP all legal */
      /* F_LEN only *declares* a length; no body bytes go on the wire */
      body = (framing == F_LEN) ? rnd32() % 49 : 0;
    } else if (mid == 2) {           /* CONNECT */
      if (rnd32() & 1) {
        framing = F_NONE;            /* 2xx: hand off after the headers */
        body = 0;
        end_after = 1;
      } else {
        status = (rnd32() & 1) ? 407 : 503;   /* ordinary response */
        framing = F_LEN;
        body = 1 + rnd32() % 16;
      }
    } else {                         /* GET */
      switch (rnd32() % 4) {
        case 0: framing = F_CHUNK; break;
        case 1: framing = F_NONE;    end_after = 1; break; /* EOF body */
        case 2: framing = F_TE_GZIP; end_after = 1; break; /* EOF body */
        default: framing = F_LEN; break;
      }
    }
    if (pl->nresp >= MAX_RESP) return -1;
    plan_add_resp(pl, req, status, minor, conn, framing, body,
                  framing == F_CHUNK ? 1 + (int)(rnd32() % 3) : 0,
                  framing == F_CHUNK ? (int)(rnd32() & 1) : 0);
    if (mid == 2 && status / 100 == 2) {
      static const char tails[3][40] = {
        "\0\1\2\3",
        "\r\n\r\n",
        "HTTP/1.1 404 Looks Real\r\n\r\n",
      };
      static const size_t tlens[3] = {4, 4, 29};
      int t = (int)(rnd32() % 3);
      plan_set_tail(pl, tails[t], tlens[t]);
    }
  }
  if (pl->nreq < 1 || pl->nresp < 1) return -1;
  return plan_finalize(pl);
}

/* -------------------------------------------------------------- registry */

static const struct session_case k_cases[] = {
  {"assoc_interim_head_get",  ASSOCIATION, case_assoc_interim_head_get},
  {"assoc_interim_connect",   ASSOCIATION, case_assoc_interim_connect},
  {"assoc_get_eof_tail",      ASSOCIATION, case_assoc_get_eof_tail},
  {"assoc_multi_interim",     ASSOCIATION, case_assoc_multi_interim},
  {"assoc_get_te_gzip_eof",   ASSOCIATION, case_assoc_get_te_gzip_eof},
  {"head_no_length_then_get", HEAD_REUSE,  case_head_no_length_then_get},
  {"head_te_gzip_then_get",   HEAD_REUSE,  case_head_te_gzip_then_get},
  {"head_cl_then_get",        HEAD_REUSE,  case_head_cl_then_get},
  {"head_chunked_then_get",   HEAD_REUSE,  case_head_chunked_then_get},
  {"head_close",              HEAD_REUSE,  case_head_close},
  {"head_http10_keep",        HEAD_REUSE,  case_head_http10_keep},
  {"head_http10_close",       HEAD_REUSE,  case_head_http10_close},
  {"head_interim_then_get",   HEAD_REUSE,  case_head_interim_then_get},
  {"head_then_get_eof",       HEAD_REUSE,  case_head_then_get_eof},
  {"handoff_nul_tail",        HANDOFF,     case_handoff_nul_tail},
  {"handoff_crlf_tail",       HANDOFF,     case_handoff_crlf_tail},
  {"handoff_status_tail",     HANDOFF,     case_handoff_status_tail},
  {"handoff_after_chunked",   HANDOFF,     case_handoff_after_chunked},
  {"handoff_non2xx_connect",  HANDOFF,     case_handoff_non2xx_connect},
};

const struct session_case *session_cases(size_t *count)
{
  *count = sizeof(k_cases) / sizeof(k_cases[0]);
  return k_cases;
}

/* ------------------------------------------------------------ fuzz entry */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  struct plan pl;
  uint64_t h = 1469598103934665603ULL;
  size_t i;
  for (i = 0; i < size; i++) {
    h ^= data[i];
    h *= 1099511628211ULL;
  }
  h ^= (uint64_t)size;
  h *= 1099511628211ULL;
  if (gen_plan(&pl, h)) return 0;   /* out-of-model input: mapped to skip */
  if (run_all(&pl, "fuzz")) {
    fprintf(stderr, "fuzzer input(%zu)=", size);
    dump_hex(data, size);
    return 1;
  }
  return 0;
}
