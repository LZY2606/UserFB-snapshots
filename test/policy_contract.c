/*
 *  policy_contract.c - deterministic byte-level contract tests for the
 *  chunked mutation engine. Links the real implementations in src/common
 *  (fd, fuzz, random, ranges) but never calls into the production RNG,
 *  range parser or mutation code from its reference model.
 *
 *  The reference below rebuilds every chunk flip mask from first principles
 *  (the documented chunk-seed arithmetic and the MINSTD generator), derives
 *  candidate bytes from pure xor/set/unset mathematics, and independently
 *  judges offset ranges, protect bytes and refuse bytes with its own
 *  grammar parsers. It is a semantic oracle, not a copy of fuzz.c's
 *  control flow.
 */

#define _POSIX_C_SOURCE 200809L

#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

#include "common/common.h"
#include "common/fd.h"
#include "common/fuzz.h"

/* ------------------------------------------------------------------ */
/* Independent deterministic reference                                */
/* ------------------------------------------------------------------ */

/* Chunk-seed mixing constants, restated as part of the mathematical
 * contract; the reference owns its own copies. */
#define REF_MAGIC1 0x33ea84f7u
#define REF_MAGIC2 0x783bc31fu
#define REF_MAGIC3 0x9b5da2fbu
#define REF_CB CHUNKBYTES

enum ref_mode { REF_XOR = 0, REF_SET, REF_UNSET };

/* Independent MINSTD (Park-Miller) step. The production generator in
 * random.c is the same Lehmer recurrence, but this implementation is
 * written locally and is the only RNG the oracle is allowed to use. */
static uint32_t ref_draw(uint32_t *state, uint32_t bound)
{
    unsigned long s = *state;
    long hi = (long)(s / 12773UL);
    long lo = (long)(s % 12773UL);
    long x = 16807L * lo - 2836L * hi;
    if (x <= 0)
        x += 0x7fffffffL;
    *state = (uint32_t)x;
    return (uint32_t)((unsigned long)x % (unsigned long)bound);
}

/* Rebuild one chunk's 1024-byte flip mask purely from (seed, ratio,
 * absolute chunk number). Repeated selection of the same bit toggles it
 * twice and therefore cancels; low-density dithering falls out of the
 * same integer rounding rule, no statistics involved. */
static void ref_chunk_mask(uint32_t seed, double ratio, int64_t chunk,
                           uint8_t mask[REF_CB])
{
    uint32_t cs = (uint32_t)chunk;
    cs ^= REF_MAGIC2;
    cs += (uint32_t)(ratio * REF_MAGIC1);
    cs ^= seed;
    cs += (uint32_t)(chunk * REF_MAGIC3);

    uint32_t state = cs ^ 0x12345678u;

    memset(mask, 0, REF_CB);
    int flips = (int)((ratio * (8.0 * REF_CB) * 1000000.0
                       + ref_draw(&state, 1000000u)) / 1000000.0);
    while (flips--)
    {
        unsigned idx = ref_draw(&state, REF_CB);
        uint8_t bit = (uint8_t)(1u << ref_draw(&state, 8u));
        mask[idx] ^= bit;
    }
}

/* Independent interval parser for _zz_bytes-style strings ("a-b,c,d-").
 * It deliberately does not use _zz_allocrange / _zz_isinrange. */
struct ref_interval { int64_t lo, hi; }; /* hi is inclusive */

static int ref_parse_intervals(const char *spec,
                               struct ref_interval *out, size_t maxn)
{
    size_t n = 0;
    const char *p = spec;
    while (*p && n < maxn)
    {
        int64_t lo = 0, hi = 0;
        int have_lo = 0, have_hi = 0, dash = 0;
        if (*p == '-') { dash = 1; p++; }
        if (*p >= '0' && *p <= '9')
        {
            have_lo = 1;
            while (*p >= '0' && *p <= '9') lo = lo * 10 + (*p++ - '0');
        }
        if (*p == '-') { dash = 1; p++; }
        if (*p >= '0' && *p <= '9')
        {
            have_hi = 1;
            while (*p >= '0' && *p <= '9') hi = hi * 10 + (*p++ - '0');
        }
        if (!dash)
            hi = lo;                /* bare number */
        else if (!have_lo)
            lo = 0;                 /* "-N" */
        else if (!have_hi)
            hi = (int64_t)1 << 40;  /* "N-" */
        out[n].lo = lo;
        out[n].hi = hi;
        n++;
        if (*p == ',') p++;
    }
    return (int)n;
}

static int ref_in_intervals(int64_t value, const struct ref_interval *iv,
                            size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (value >= iv[i].lo && value <= iv[i].hi)
            return 1;
    return 0;
}

/* Independent parser for protect/refuse character-set strings. Supports
 * literal characters, a-b ranges, \n \r \t, trailing \\, octal \ooo and
 * hex \xHH / \XHH. The grammar is reimplemented from the documented
 * escape rules rather than taken from fuzz.c. */
#define TOK_DASH -2
#define TOK_END  -1

static int ref_is_hexdigit(int q)
{
    return (q >= '0' && q <= '9') || (q >= 'a' && q <= 'f')
           || (q >= 'A' && q <= 'F');
}

static int ref_hexval(int q)
{
    if (q >= '0' && q <= '9') return q - '0';
    if (q >= 'a' && q <= 'f') return q - 'a' + 10;
    return q - 'A' + 10;
}

static int ref_next_token(const char **pp)
{
    const char *p = *pp;
    if (!*p) { *pp = p; return TOK_END; }
    if (*p == '-') { *pp = p + 1; return TOK_DASH; }
    if (*p != '\\') { int c = (unsigned char)*p; *pp = p + 1; return c; }

    if (!p[1]) { *pp = p + 1; return '\\'; }
    p++;
    if (*p == 'n') { *pp = p + 1; return '\n'; }
    if (*p == 'r') { *pp = p + 1; return '\r'; }
    if (*p == 't') { *pp = p + 1; return '\t'; }
    if (*p >= '0' && *p <= '7'
        && p[1] >= '0' && p[1] <= '7'
        && p[2] >= '0' && p[2] <= '7')
    {
        int c = (p[0] - '0') << 6 | (p[1] - '0') << 3 | (p[2] - '0');
        *pp = p + 3;
        return c & 0xff;
    }
    if ((*p == 'x' || *p == 'X') && ref_is_hexdigit(p[1])
        && ref_is_hexdigit(p[2]))
    {
        int c = (ref_hexval(p[1]) << 4) | ref_hexval(p[2]);
        *pp = p + 3;
        return c;
    }
    int c = (unsigned char)*p;
    *pp = p + 1;
    return c;
}

static void ref_parse_charset(const char *spec, unsigned char set[256])
{
    memset(set, 0, 256);
    int a = TOK_END, b = TOK_END;
    const char *p = spec;
    for (;;)
    {
        int ch = ref_next_token(&p);
        if (ch == TOK_END)
            break;
        if (a != TOK_END && b == TOK_DASH && a <= ch)
        {
            while (a <= ch) set[a++] = 1;
            a = b = TOK_END;
        }
        else
        {
            if (a != TOK_END) set[a] = 1;
            a = b;
            b = ch;
        }
    }
    if (a != TOK_END) set[a] = 1;
    if (b != TOK_END) set[b] = 1;
}

struct ref_policy
{
    enum ref_mode mode;
    const struct ref_interval *iv;
    size_t niv;
    const unsigned char *protect;
    const unsigned char *refuse;
};

/* Pure candidate arithmetic for the three documented modes. */
static uint8_t ref_candidate(enum ref_mode mode, uint8_t x, uint8_t m)
{
    switch (mode)
    {
    case REF_XOR:   return (uint8_t)(x ^ m);
    case REF_SET:   return (uint8_t)(x | m);
    default:        return (uint8_t)(x & (uint8_t)~m);
    }
}

/* Full oracle: expected output for len bytes read at absolute position
 * basepos, given a context bound to (seed, ratio) and a policy. */
static void ref_apply(const uint8_t *in, int64_t len, int64_t basepos,
                      uint32_t seed, double ratio,
                      const struct ref_policy *pol, uint8_t *out)
{
    uint8_t mask[REF_CB];
    int64_t cached = -1;
    for (int64_t k = 0; k < len; ++k)
    {
        int64_t pos = basepos + k;
        int64_t chunk = pos / REF_CB;
        if (chunk != cached)
        {
            ref_chunk_mask(seed, ratio, chunk, mask);
            cached = chunk;
        }

        uint8_t x = in[k];
        uint8_t m = mask[pos % REF_CB];
        uint8_t y = x;

        if (ref_in_intervals(pos, pol->iv, pol->niv)
            && !pol->protect[x] && m)
        {
            uint8_t c = ref_candidate(pol->mode, x, m);
            if (!pol->refuse[c])
                y = c;
        }
        out[k] = y;
    }
}

/* ------------------------------------------------------------------ */
/* Small helpers on top of the real, public production entry points   */
/* ------------------------------------------------------------------ */

static void prod_mode(enum ref_mode mode)
{
    _zz_fuzzing(mode == REF_XOR ? "xor"
                : mode == REF_SET ? "set" : "unset");
}

/* Run one real _zz_fuzz call on a freshly registered descriptor. */
static void prod_fuzz(int fd, int64_t pos, uint8_t *buf, int64_t len)
{
    _zz_setpos(fd, pos);
    _zz_fuzz(fd, buf, len);
}

static int buffers_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) return 0;
    return 1;
}

/* Locate, by brute force over the 256 possible input bytes, an offset
 * whose mask lets the requested protect/refuse category happen. */
struct category { int x_prot, x_ref, c_prot, c_ref; };

static int find_offset(uint32_t seed, double ratio, enum ref_mode mode,
                       int64_t start, int64_t stop,
                       const unsigned char *prot,
                       const unsigned char *refuse,
                       const struct category *want,
                       int64_t *pos_out, uint8_t *x_out, uint8_t *m_out)
{
    uint8_t mask[REF_CB];
    for (int64_t pos = start; pos < stop; ++pos)
    {
        ref_chunk_mask(seed, ratio, pos / REF_CB, mask);
        uint8_t m = mask[pos % REF_CB];
        if (!m) continue;
        for (int x = 0; x < 256; ++x)
        {
            uint8_t c = ref_candidate(mode, (uint8_t)x, m);
            if (!!prot[x] != want->x_prot || !!refuse[x] != want->x_ref)
                continue;
            if (!!prot[c] != want->c_prot || !!refuse[c] != want->c_ref)
                continue;
            *pos_out = pos;
            *x_out = (uint8_t)x;
            *m_out = m;
            return 1;
        }
    }
    return 0;
}

/* ================================================================== */
/* Group 1: mask_model                                                */
/* ================================================================== */

/* Fixed MINSTD outputs anchor the hand-written RNG to a small, auditable
 * sequence instead of trusting cross-comparison with production alone. */
static int case_rng_anchor(void)
{
    static const uint32_t expect[6] =
        { 16807u, 67796602u, 157910185u, 139045081u, 153603772u,
          107865829u };
    uint32_t state = 1;
    for (int i = 0; i < 6; ++i)
        TASK_CHECK(ref_draw(&state, 0x7fffffffu) == expect[i]);

    /* Dithering counts for seed 1812 / ratio 0.013 are themselves fixed
     * integers (nominal 106.496 flips per chunk plus one dither draw). */
    static const int want_todo[4] = { 106, 107, 107, 106 };
    static const struct { unsigned idx; uint8_t bit; } first_draw[4] =
        { { 228, 0x01 }, { 941, 0x40 }, { 292, 0x01 }, { 1020, 0x02 } };

    _zz_fd_init();
    zzuf_set_seed(1812);
    zzuf_set_ratio(0.013, 0.013);
    zzuf_protect_range("");
    zzuf_refuse_range("");

    for (int c = 0; c < 4; ++c)
    {
        uint8_t mine[REF_CB];
        ref_chunk_mask(1812, 0.013, c, mine);

        /* Rebuild the flip count and first draw independently as anchors. */
        uint32_t cs = (uint32_t)c;
        cs ^= REF_MAGIC2;
        cs += (uint32_t)(0.013 * REF_MAGIC1);
        cs ^= 1812u;
        cs += (uint32_t)((int64_t)c * REF_MAGIC3);
        uint32_t st = cs ^ 0x12345678u;
        int todo = (int)((0.013 * (8.0 * REF_CB) * 1000000.0
                          + ref_draw(&st, 1000000u)) / 1000000.0);
        TASK_CHECK(todo == want_todo[c]);
        unsigned idx = ref_draw(&st, REF_CB);
        uint8_t bit = (uint8_t)(1u << ref_draw(&st, 8u));
        TASK_CHECK(idx == first_draw[c].idx && bit == first_draw[c].bit);

        /* Same mask must appear in the real context's cached chunk. */
        int fd = 100 + c;
        _zz_register(fd);
        uint8_t scratch[1] = { 0 };
        prod_fuzz(fd, (int64_t)c * REF_CB, scratch, 1);
        fuzz_context_t *fz = _zz_getfuzz(fd);
        TASK_CHECK(fz != NULL && fz->cur == c);
        TASK_CHECK(buffers_equal(fz->data, mine, REF_CB));
        _zz_unregister(fd);
    }
    _zz_fd_fini();
    return 0;
}

/* Picking the same (index, bit) twice must cancel; different bits on the
 * same index combine into a multi-bit mask byte. */
static int case_repeat_cancels(void)
{
    _zz_fd_init();
    zzuf_set_seed(1812);
    zzuf_set_ratio(0.013, 0.013);
    zzuf_protect_range("");
    zzuf_refuse_range("");
    prod_mode(REF_XOR);

    uint8_t mine[REF_CB];
    ref_chunk_mask(1812, 0.013, 3, mine);

    /* Probe-derived fixed facts about chunk 3: index 34 draws bit 0x40
     * twice (cancels to zero), index 736 also cancels, while index 229
     * accumulates 0x09 from two distinct bits. */
    TASK_CHECK(mine[34] == 0x00);
    TASK_CHECK(mine[736] == 0x00);
    TASK_CHECK(mine[229] == 0x09);
    TASK_CHECK(mine[266] == 0x22);

    int fd = 110;
    _zz_register(fd);
    static uint8_t buf[REF_CB];
    memset(buf, 0, sizeof buf);
    prod_fuzz(fd, 3 * REF_CB, buf, REF_CB);
    fuzz_context_t *fz = _zz_getfuzz(fd);
    TASK_CHECK(buffers_equal(fz->data, mine, REF_CB));

    /* End-to-end: zero input can only change where the mask survived. */
    TASK_CHECK(buf[34] == 0x00);       /* cancelled flip leaves byte alone */
    TASK_CHECK(buf[736] == 0x00);
    TASK_CHECK(buf[229] == 0x09);      /* combined bits are applied */
    _zz_unregister(fd);
    _zz_fd_fini();
    return 0;
}

/* Nominal density below one flip per chunk. The integer dither decides,
 * chunk by chunk, whether zero or one flip happens; the pattern of those
 * decisions and the exact resulting bytes are fully deterministic. */
static int case_low_density_rounding(void)
{
    const double r = 0.00006; /* 0.49152 nominal flips per 1024-byte chunk */
    static const int want_todo[8] = { 0, 0, 0, 1, 0, 1, 0, 0 };

    _zz_fd_init();
    zzuf_set_seed(7);
    zzuf_set_ratio(r, r);
    zzuf_protect_range("");
    zzuf_refuse_range("");
    prod_mode(REF_XOR);

    int total_flips = 0;
    for (int c = 0; c < 8; ++c)
    {
        uint8_t mine[REF_CB];
        ref_chunk_mask(7, r, c, mine);
        int bits = 0;
        for (int i = 0; i < REF_CB; ++i)
            for (int b = 0; b < 8; ++b)
                bits += !!(mine[i] & (1 << b));
        TASK_CHECK(bits == want_todo[c]);
        total_flips += bits;
    }
    TASK_CHECK(total_flips == 2);

    /* Whole-stream production run: exactly two bytes may differ from the
     * all-zero input, at absolute offsets 3947 and 5611. */
    int fd = 120;
    _zz_register(fd);
    static uint8_t buf[8 * REF_CB];
    memset(buf, 0, sizeof buf);
    prod_fuzz(fd, 0, buf, sizeof buf);
    size_t changed = 0;
    for (size_t i = 0; i < sizeof buf; ++i)
        if (buf[i])
        {
            TASK_CHECK(i == 3947 || i == 5611);
            changed++;
        }
    TASK_CHECK(changed == 2);
    TASK_CHECK(buf[3947] == 0x40); /* chunk 3, index 875 */
    TASK_CHECK(buf[5611] == 0x80); /* chunk 5, index 491 */
    _zz_unregister(fd);
    _zz_fd_fini();
    return 0;
}

/* ================================================================== */
/* Group 2: policy_composition                                        */
/* ================================================================== */

/* Hex, octal, newline and range escapes parsed by the independent
 * charset oracle, then exercised through the public config functions. */
static int case_charset_escapes(void)
{
    /* C string "\x41-\x43\n\101\132\\" -> tokens 0x41 '-' 0x43 '\n'
     * 0o101(65) 0o132(90) '\' */
    const char *spec = "\\x41-\\x43\\n\\101\\132\\\\";
    unsigned char mine[256];
    ref_parse_charset(spec, mine);

    static const unsigned char present[] = { 10, 65, 66, 67, 90, 92 };
    static const unsigned char absent[] = { 9, 13, 64, 68, 89, 91, 93 };
    for (size_t i = 0; i < sizeof present; ++i)
        TASK_CHECK(mine[present[i]] == 1);
    for (size_t i = 0; i < sizeof absent; ++i)
        TASK_CHECK(mine[absent[i]] == 0);

    _zz_fd_init();
    zzuf_set_seed(1812);
    zzuf_set_ratio(0.013, 0.013);
    zzuf_refuse_range("");
    zzuf_protect_range(spec);
    prod_mode(REF_XOR);
    _zz_bytes("0-100000");

    int fd = 130;
    _zz_register(fd);
    static uint8_t in[2 * REF_CB], out[2 * REF_CB], expect[2 * REF_CB];
    for (size_t i = 0; i < sizeof in; ++i)
        in[i] = (uint8_t)((i * 31 + 7) & 0xff);
    memcpy(out, in, sizeof in);

    struct ref_interval iv[1] = { { 0, 100000 } };
    struct ref_policy pol = { REF_XOR, iv, 1, mine,
                              (const unsigned char[256]){ 0 } };
    ref_apply(in, sizeof in, 0, 1812, 0.013, &pol, expect);
    prod_fuzz(fd, 0, out, sizeof out);
    TASK_CHECK(buffers_equal(out, expect, sizeof out));

    /* Behavioural proof that a protected original byte is kept even when
     * the flip mask would mutate it. */
    size_t kept = 0, mutated = 0;
    for (size_t i = 0; i < sizeof in; ++i)
    {
        if (mine[in[i]]) { TASK_CHECK(out[i] == in[i]); kept++; }
        else if (out[i] != in[i]) mutated++;
    }
    TASK_CHECK(kept > 0 && mutated > 0);
    _zz_unregister(fd);
    _zz_fd_fini();
    return 0;
}

/* All three modes against an offset interval that straddles the chunk
 * boundary; every output byte is judged by the pure oracle. */
static int case_modes_and_ranges(void)
{
    static const enum ref_mode modes[3] = { REF_XOR, REF_SET, REF_UNSET };
    struct ref_interval iv[2];
    int niv = ref_parse_intervals("1000-2100,50-60", iv, 2);
    TASK_CHECK(niv == 2 && iv[0].lo == 1000 && iv[0].hi == 2100
               && iv[1].lo == 50 && iv[1].hi == 60);

    _zz_fd_init();
    zzuf_set_seed(4242);
    zzuf_set_ratio(0.021, 0.021);
    zzuf_protect_range("");
    zzuf_refuse_range("");
    _zz_bytes("1000-2100,50-60");

    const int64_t len = 3 * REF_CB + 37;
    static uint8_t in[3 * REF_CB + 37], out[3 * REF_CB + 37],
        expect[3 * REF_CB + 37];
    for (size_t i = 0; i < sizeof in; ++i)
        in[i] = (uint8_t)((i * 19 + 11) & 0xff);

    for (int m = 0; m < 3; ++m)
    {
        prod_mode(modes[m]);
        int fd = 140 + m;
        _zz_register(fd);
        memcpy(out, in, sizeof in);
        struct ref_policy pol = { modes[m], iv, (size_t)niv,
                                  (const unsigned char[256]){ 0 },
                                  (const unsigned char[256]){ 0 } };
        ref_apply(in, len, 0, 4242, 0.021, &pol, expect);
        prod_fuzz(fd, 0, out, len);
        TASK_CHECK(buffers_equal(out, expect, (size_t)len));

        /* Offsets outside every interval are provably untouched despite
         * nonzero masks; the 1000-2100 interval crosses the 1024 border. */
        int outside_kept = 0, inside_changed = 0, boundary_changed = 0;
        for (int64_t p = 0; p < len; ++p)
        {
            if (!ref_in_intervals(p, iv, (size_t)niv))
            {
                TASK_CHECK(out[p] == in[p]);
                outside_kept++;
            }
            else if (out[p] != in[p])
                inside_changed++;
            if (p >= 1000 && p <= 1048 && out[p] != in[p])
                boundary_changed++;
        }
        TASK_CHECK(outside_kept > 0 && inside_changed > 0
                   && boundary_changed > 0);
        _zz_unregister(fd);
    }
    _zz_fd_fini();
    return 0;
}

/* Protect judges the original byte, refuse judges the candidate byte;
 * overlapping memberships must not let the two be interchanged. */
static int case_protect_refuse_order(void)
{
    const char *pspec = "\\x41-\\x43\\n\\132\\\\";   /* {10,65,66,67,90,92} */
    const char *rspec = "\\x5a-\\x5c\\x7a";          /* {90,91,92,122} */
    unsigned char prot[256], refuse[256];
    ref_parse_charset(pspec, prot);
    ref_parse_charset(rspec, refuse);
    TASK_CHECK(prot[90] && refuse[90]);     /* overlap value */
    TASK_CHECK(prot[92] && refuse[92]);     /* overlap value */

    _zz_fd_init();
    zzuf_set_seed(909);
    zzuf_set_ratio(0.03, 0.03);
    prod_mode(REF_XOR);
    zzuf_protect_range(pspec);
    zzuf_refuse_range(rspec);
    _zz_bytes("0-100000");

    struct ref_interval iv[1] = { { 0, 100000 } };
    struct ref_policy pol = { REF_XOR, iv, 1, prot, refuse };
    const int64_t span = 4 * REF_CB;

    /* (1) original refused, candidate protected: mutation must still be
     * applied - protect only ever looks at the original byte. */
    {
        int64_t pos; uint8_t x, m;
        struct category want = { 0, 1, 1, 0 };
        TASK_CHECK(find_offset(909, 0.03, REF_XOR, 0, span, prot, refuse,
                               &want, &pos, &x, &m));
        uint8_t c = (uint8_t)(x ^ m);
        TASK_CHECK(refuse[x] && prot[c] && !refuse[c]);
        int fd = 150;
        _zz_register(fd);
        uint8_t buf[16], model[16];
        memset(buf, 0, sizeof buf); buf[0] = x;
        ref_apply(buf, 1, pos, 909, 0.03, &pol, model);
        prod_fuzz(fd, pos, buf, 1);
        TASK_CHECK(buf[0] == c && model[0] == c);
        _zz_unregister(fd);
    }
    /* (2) original protected: nothing can move it, even if the candidate
     * would itself be a refused value. */
    {
        int64_t pos; uint8_t x, m;
        struct category want = { 1, 0, 0, 1 };
        TASK_CHECK(find_offset(909, 0.03, REF_XOR, 0, span, prot, refuse,
                               &want, &pos, &x, &m));
        uint8_t c = (uint8_t)(x ^ m);
        TASK_CHECK(prot[x] && refuse[c]);
        int fd = 151;
        _zz_register(fd);
        uint8_t buf[1] = { x }, model[1];
        ref_apply(buf, 1, pos, 909, 0.03, &pol, model);
        prod_fuzz(fd, pos, buf, 1);
        TASK_CHECK(buf[0] == x && model[0] == x);
        _zz_unregister(fd);
    }
    /* (3) plain refusal: unprotected original, candidate in refuse, the
     * original byte is retained. */
    {
        int64_t pos; uint8_t x, m;
        struct category want = { 0, 0, 0, 1 };
        TASK_CHECK(find_offset(909, 0.03, REF_XOR, 0, span, prot, refuse,
                               &want, &pos, &x, &m));
        int fd = 152;
        _zz_register(fd);
        uint8_t buf[1] = { x }, model[1];
        ref_apply(buf, 1, pos, 909, 0.03, &pol, model);
        prod_fuzz(fd, pos, buf, 1);
        TASK_CHECK(buf[0] == x && model[0] == x);
        _zz_unregister(fd);
    }
    /* (4) full sweep over three chunks matches the oracle byte for byte,
     * including positions where the two sets overlap. */
    {
        int fd = 153;
        _zz_register(fd);
        static uint8_t in[3 * REF_CB], out[3 * REF_CB],
            expect[3 * REF_CB];
        for (size_t i = 0; i < sizeof in; ++i)
            in[i] = (uint8_t)((i * 7 + 5) & 0xff);
        memcpy(out, in, sizeof in);
        ref_apply(in, sizeof in, 0, 909, 0.03, &pol, expect);
        prod_fuzz(fd, 0, out, sizeof out);
        TASK_CHECK(buffers_equal(out, expect, sizeof out));
        _zz_unregister(fd);
    }
    _zz_fd_fini();
    return 0;
}

/* Whole-buffer fuzzing, cross-boundary fragment fuzzing and out-of-order
 * chunk re-reads must converge on the same bytes. */
static int case_whole_fragment_reread(void)
{
    const int64_t len = 3 * REF_CB + 50;
    static uint8_t in[3 * REF_CB + 50], whole[3 * REF_CB + 50],
        frag[3 * REF_CB + 50], reread[3 * REF_CB + 50],
        expect[3 * REF_CB + 50];
    for (size_t i = 0; i < sizeof in; ++i)
        in[i] = (uint8_t)((i * 19 + 3) & 0xff);

    _zz_fd_init();
    zzuf_set_seed(1812);
    zzuf_set_ratio(0.013, 0.013);
    zzuf_protect_range("");
    zzuf_refuse_range("");
    prod_mode(REF_XOR);
    _zz_bytes("0-100000");
    struct ref_interval iv[1] = { { 0, 100000 } };
    struct ref_policy pol = { REF_XOR, iv, 1,
                              (const unsigned char[256]){ 0 },
                              (const unsigned char[256]){ 0 } };
    ref_apply(in, len, 0, 1812, 0.013, &pol, expect);

    /* One shot. */
    int fd_w = 160;
    _zz_register(fd_w);
    memcpy(whole, in, sizeof in);
    prod_fuzz(fd_w, 0, whole, len);
    TASK_CHECK(buffers_equal(whole, expect, (size_t)len));
    _zz_unregister(fd_w);

    /* Fragments, one deliberately straddling the 1024-byte border. */
    int fd_f = 161;
    _zz_register(fd_f);
    memcpy(frag, in, sizeof frag);
    static const int64_t cuts[4] = { 0, 700, 1400, 3 * REF_CB + 50 };
    for (int k = 0; k < 3; ++k)
        prod_fuzz(fd_f, cuts[k], frag + cuts[k], cuts[k + 1] - cuts[k]);
    TASK_CHECK(buffers_equal(frag, expect, (size_t)len));
    _zz_unregister(fd_f);

    /* Out-of-order re-reads: chunk 2, then 0, then 1, then the tail. */
    int fd_r = 162;
    _zz_register(fd_r);
    memcpy(reread, in, sizeof reread);
    prod_fuzz(fd_r, 2 * REF_CB, reread + 2 * REF_CB, REF_CB);
    prod_fuzz(fd_r, 0, reread, REF_CB);
    prod_fuzz(fd_r, REF_CB, reread + REF_CB, REF_CB);
    prod_fuzz(fd_r, 3 * REF_CB, reread + 3 * REF_CB, 50);
    TASK_CHECK(buffers_equal(reread, expect, (size_t)len));
    TASK_CHECK(buffers_equal(reread, whole, (size_t)len));
    _zz_unregister(fd_r);
    _zz_fd_fini();
    return 0;
}

/* ================================================================== */
/* Group 3: context_lifecycle                                         */
/* ================================================================== */

/* Seed and ratio are bound at registration time; later global changes
 * must not leak into an existing context. */
static int case_bind_at_register(void)
{
    _zz_fd_init();
    zzuf_set_seed(11);
    zzuf_set_ratio(0.02, 0.02);
    int fd_a = 200;
    _zz_register(fd_a);

    zzuf_set_seed(22);
    zzuf_set_ratio(0.05, 0.05);
    int fd_b = 201;
    _zz_register(fd_b);

    fuzz_context_t *fa = _zz_getfuzz(fd_a);
    fuzz_context_t *fb = _zz_getfuzz(fd_b);
    TASK_CHECK(fa->seed == 11 && fa->ratio == 0.02);
    TASK_CHECK(fb->seed == 22 && fb->ratio == 0.05);

    zzuf_protect_range("");
    zzuf_refuse_range("");
    prod_mode(REF_XOR);
    _zz_bytes("0-100000");
    struct ref_interval iv[1] = { { 0, 100000 } };
    struct ref_policy pol = { REF_XOR, iv, 1,
                              (const unsigned char[256]){ 0 },
                              (const unsigned char[256]){ 0 } };
    const int64_t len = 2 * REF_CB;
    static uint8_t in[2 * REF_CB], oa[2 * REF_CB], ob[2 * REF_CB],
        ea[2 * REF_CB], eb[2 * REF_CB];
    for (size_t i = 0; i < sizeof in; ++i)
        in[i] = (uint8_t)((i * 13 + 1) & 0xff);
    memcpy(oa, in, sizeof in);
    memcpy(ob, in, sizeof in);
    ref_apply(in, len, 0, 11, 0.02, &pol, ea);
    ref_apply(in, len, 0, 22, 0.05, &pol, eb);

    prod_fuzz(fd_a, 0, oa, len);
    prod_fuzz(fd_b, 0, ob, len);
    TASK_CHECK(buffers_equal(oa, ea, (size_t)len));
    TASK_CHECK(buffers_equal(ob, eb, (size_t)len));
    TASK_CHECK(!buffers_equal(oa, ob, (size_t)len));
    _zz_unregister(fd_a);
    _zz_unregister(fd_b);
    _zz_fd_fini();
    return 0;
}

/* The same descriptor number, unregistered and re-registered, must start
 * from a fresh configuration with no inherited chunk cache. */
static int case_reregister_no_cache(void)
{
    _zz_fd_init();
    zzuf_set_seed(11);
    zzuf_set_ratio(0.02, 0.02);
    zzuf_protect_range("");
    zzuf_refuse_range("");
    prod_mode(REF_XOR);
    _zz_bytes("0-100000");

    int fd = 210;
    _zz_register(fd);
    static uint8_t first[REF_CB];
    memset(first, 0, sizeof first);
    prod_fuzz(fd, 0, first, REF_CB); /* populates chunk 0 cache */
    fuzz_context_t *f1 = _zz_getfuzz(fd);
    TASK_CHECK(f1->cur == 0 && f1->seed == 11);
    _zz_unregister(fd);

    zzuf_set_seed(22);
    zzuf_set_ratio(0.05, 0.05);
    _zz_register(fd);
    fuzz_context_t *f2 = _zz_getfuzz(fd);
    TASK_CHECK(f2->cur == -1);                 /* cache reset */
    TASK_CHECK(f2->seed == 22 && f2->ratio == 0.05);

    static uint8_t second[REF_CB], expect[REF_CB];
    memset(second, 0, sizeof second);
    struct ref_interval iv[1] = { { 0, 100000 } };
    struct ref_policy pol = { REF_XOR, iv, 1,
                              (const unsigned char[256]){ 0 },
                              (const unsigned char[256]){ 0 } };
    static uint8_t zeros[REF_CB];
    ref_apply(zeros, REF_CB, 0, 22, 0.05, &pol, expect);
    prod_fuzz(fd, 0, second, REF_CB);
    TASK_CHECK(buffers_equal(second, expect, REF_CB));
    TASK_CHECK(!buffers_equal(first, second, REF_CB));
    _zz_unregister(fd);
    _zz_fd_fini();
    return 0;
}

/* Several real contexts driven in an interleaved schedule, ending with a
 * newly registered context under a changed global policy that revisits a
 * chunk while character refusal is active: block revisit + refusal + new
 * registration exercised together. */
static int case_interleaved_contexts(void)
{
    _zz_fd_init();
    zzuf_protect_range("");
    zzuf_refuse_range("");
    prod_mode(REF_XOR);
    _zz_bytes("0-100000");
    struct ref_interval iv[1] = { { 0, 100000 } };
    unsigned char no_set[256];
    memset(no_set, 0, sizeof no_set);

    static const uint32_t seeds[3] = { 301, 302, 303 };
    static const double ratios[3] = { 0.011, 0.017, 0.023 };
    int fds[3] = { 220, 221, 222 };
    static uint8_t in[2 * REF_CB], store[3][2 * REF_CB],
        model[2 * REF_CB];
    for (size_t i = 0; i < sizeof in; ++i)
        in[i] = (uint8_t)((i * 23 + 9) & 0xff);

    for (int k = 0; k < 3; ++k)
    {
        zzuf_set_seed((int32_t)seeds[k]);
        zzuf_set_ratio(ratios[k], ratios[k]);
        _zz_register(fds[k]);
        memcpy(store[k], in, sizeof in);
    }

    /* Interleave: chunk 1 of A, chunk 0 of B, chunk 1 of C, then chunk 0
     * of A (revisit after its cache was left on chunk 1). */
    prod_fuzz(fds[0], REF_CB, store[0] + REF_CB, REF_CB);
    prod_fuzz(fds[1], 0, store[1], REF_CB);
    prod_fuzz(fds[2], REF_CB, store[2] + REF_CB, REF_CB);
    prod_fuzz(fds[0], 0, store[0], REF_CB);
    prod_fuzz(fds[1], REF_CB, store[1] + REF_CB, REF_CB);
    prod_fuzz(fds[2], 0, store[2], REF_CB);

    for (int k = 0; k < 3; ++k)
    {
        struct ref_policy pol = { REF_XOR, iv, 1, no_set, no_set };
        ref_apply(in, 2 * REF_CB, 0, seeds[k], ratios[k], &pol, model);
        TASK_CHECK(buffers_equal(store[k], model, 2 * REF_CB));
    }

    /* New registration with different globals; refuse is switched on and
     * the context revisits chunk 0 after first touching chunk 1. */
    const char *rspec = "\\x5a-\\x5c"; /* 90,91,92 */
    unsigned char refuse[256];
    ref_parse_charset(rspec, refuse);
    zzuf_refuse_range(rspec);
    zzuf_set_seed(777);
    zzuf_set_ratio(0.029, 0.029);
    int fd_new = 223;
    _zz_register(fd_new);

    static uint8_t fresh[2 * REF_CB], expect_f[2 * REF_CB];
    memcpy(fresh, in, sizeof fresh);
    prod_fuzz(fd_new, REF_CB, fresh + REF_CB, REF_CB);
    prod_fuzz(fd_new, 0, fresh, REF_CB);           /* revisit chunk 0 */
    struct ref_policy pol_r = { REF_XOR, iv, 1, no_set, refuse };
    ref_apply(in, 2 * REF_CB, 0, 777, 0.029, &pol_r, expect_f);
    TASK_CHECK(buffers_equal(fresh, expect_f, sizeof fresh));

    int refusals = 0;
    for (size_t i = 0; i < sizeof fresh; ++i)
        if (fresh[i] == in[i] && expect_f[i] == in[i])
        {
            uint8_t m;
            uint8_t tmp[REF_CB];
            ref_chunk_mask(777, 0.029, (int64_t)i / REF_CB, tmp);
            m = tmp[i % REF_CB];
            if (m && refuse[(uint8_t)(in[i] ^ m)]) refusals++;
        }
    TASK_CHECK(refusals > 0);

    for (int k = 0; k < 3; ++k) _zz_unregister(fds[k]);
    _zz_unregister(fd_new);
    _zz_fd_fini();
    return 0;
}

/* Real CLI path: the same oracle validates .task-build/zzuf-stdin used
 * purely as a finite byte filter, across BUFSIZ reads and with a -B cap
 * ending inside a chunk. No child program, no preload. */
static int run_stdin_filter(const char *in_path, const char *out_path,
                            const char *protect_arg,
                            const char *refuse_arg, int maxbytes)
{
    /* Flush inherited stdio buffers before forking: otherwise the child's
     * freopen() would re-emit the parent's buffered prefix into our log. */
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0)
    {
        /* Plain stdio redirection onto the temp files; no fd aliasing. */
        if (!freopen(in_path, "r", stdin)
            || !freopen(out_path, "w", stdout))
            _exit(97);
        char bbuf[32];
        snprintf(bbuf, sizeof bbuf, "%d", maxbytes);
        execl(".task-build/zzuf-stdin", "zzuf-stdin",
              "-s", "1812", "-r", "0.013", "-f", "xor",
              "-b", "100-2600",
              "-P", protect_arg,
              "-R", refuse_arg,
              "-B", bbuf, (char *)NULL);
        _exit(98);
    }
    int status = 0;
    TASK_CHECK(waitpid(pid, &status, 0) == pid);
    TASK_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return 0;
}

static int case_cli_stdin_filter(void)
{
    TASK_CHECK(access(".task-build/zzuf-stdin", X_OK) == 0);

    const char *pspec = "\\x41-\\x43\\n\\132\\\\"; /* {10,65,66,67,90,92} */
    const char *rspec = "\\x5a-\\x5c\\x7a";        /* {90,91,92,122} */
    unsigned char prot[256], refuse[256];
    ref_parse_charset(pspec, prot);
    ref_parse_charset(rspec, refuse);
    TASK_CHECK(prot[90] && refuse[90] && prot[92] && refuse[92]);

    const size_t n = (size_t)(2 * BUFSIZ + 37);
    uint8_t *in = malloc(n), *expect = malloc(n);
    TASK_CHECK(in && expect);
    for (size_t i = 0; i < n; ++i)
        in[i] = (uint8_t)((i * 31 + 7) & 0xff);

    struct ref_interval iv[1];
    int niv = ref_parse_intervals("100-2600", iv, 1);
    TASK_CHECK(niv == 1);
    struct ref_policy pol = { REF_XOR, iv, 1, prot, refuse };
    ref_apply(in, (int64_t)n, 0, 1812, 0.013, &pol, expect);

    /* Guard: the policy really bites on this input. */
    size_t prot_kept = 0, ref_kept = 0, changed = 0;
    for (size_t i = 0; i < n; ++i)
    {
        if (expect[i] != in[i]) changed++;
        if (prot[in[i]]) prot_kept++;
        uint8_t tmp[REF_CB];
        ref_chunk_mask(1812, 0.013, (int64_t)i / REF_CB, tmp);
        uint8_t m = tmp[i % REF_CB];
        if (m && !prot[in[i]] && refuse[(uint8_t)(in[i] ^ m)]
            && ref_in_intervals((int64_t)i, iv, 1))
            ref_kept++;
    }
    TASK_CHECK(changed > 0 && prot_kept > 0 && ref_kept > 0);

    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !*tmpdir) tmpdir = "/tmp";
    char in_path[256], out_path[256];
    snprintf(in_path, sizeof in_path, "%s/pc-in.XXXXXX", tmpdir);
    snprintf(out_path, sizeof out_path, "%s/pc-out.XXXXXX", tmpdir);
    int fi = mkstemp(in_path), fo = mkstemp(out_path);
    TASK_CHECK(fi >= 0 && fo >= 0);
    close(fo);
    TASK_CHECK(write(fi, in, n) == (ssize_t)n);
    close(fi);

    /* Full stream: -B above input length, several BUFSIZ reads. */
    TASK_CHECK(run_stdin_filter(in_path, out_path, pspec, rspec,
                                (int)n + 100) == 0);
    int fd = open(out_path, O_RDONLY);
    TASK_CHECK(fd >= 0);
    uint8_t *got = malloc(n + 16);
    TASK_CHECK(got);
    ssize_t total = 0, r;
    while ((r = read(fd, got + total, n + 16 - (size_t)total)) > 0)
        total += r;
    close(fd);
    TASK_CHECK(total == (ssize_t)n);
    TASK_CHECK(buffers_equal(got, expect, n));

    /* Limited read ending inside a chunk (not on a 1024-byte border). */
    int cap = BUFSIZ + 137;
    TASK_CHECK(cap % REF_CB != 0);
    TASK_CHECK(run_stdin_filter(in_path, out_path, pspec, rspec, cap) == 0);
    fd = open(out_path, O_RDONLY);
    TASK_CHECK(fd >= 0);
    total = 0;
    while ((r = read(fd, got + total, (size_t)cap - (size_t)total)) > 0)
        total += r;
    close(fd);
    TASK_CHECK(total == cap);
    TASK_CHECK(buffers_equal(got, expect, (size_t)cap));

    unlink(in_path);
    unlink(out_path);
    free(in); free(expect); free(got);
    return 0;
}

/* ================================================================== */
/* Group 4: pushback_schedule                                         */
/* ================================================================== */

/* upos hit: the single fallback byte overrides the mutated result once
 * and the flag is consumed. */
static int case_pushback_hit(void)
{
    _zz_fd_init();
    zzuf_set_seed(1812);
    zzuf_set_ratio(0.013, 0.013);
    zzuf_protect_range("");
    zzuf_refuse_range("");
    prod_mode(REF_XOR);
    _zz_bytes("0-100000");

    int fd = 300;
    _zz_register(fd);
    /* Pick, deterministically, a chunk-0 offset whose mask really flips. */
    uint8_t mask[REF_CB];
    ref_chunk_mask(1812, 0.013, 0, mask);
    int64_t pos = -1;
    for (int i = 0; i < REF_CB; ++i)
        if (mask[i]) { pos = i; break; }
    TASK_CHECK(pos >= 0);
    uint8_t m = mask[pos];
    TASK_CHECK(m != 0);
    uint8_t x = 0x5a;
    TASK_CHECK((uint8_t)(x ^ m) != x); /* mutation would be visible */

    uint8_t buf[8], model[8];
    memset(buf, x, sizeof buf);
    fuzz_context_t *fz = _zz_getfuzz(fd);
    fz->uflag = 1;
    fz->upos = pos;
    fz->uchar = 0x77;
    struct ref_interval iv[1] = { { 0, 100000 } };
    struct ref_policy pol = { REF_XOR, iv, 1,
                              (const unsigned char[256]){ 0 },
                              (const unsigned char[256]){ 0 } };
    ref_apply(buf, sizeof buf, pos, 1812, 0.013, &pol, model);
    TASK_CHECK(model[0] != x); /* oracle confirms the mutation existed */

    prod_fuzz(fd, pos, buf, sizeof buf);
    TASK_CHECK(buf[0] == 0x77);          /* fallback wins over mutation */
    TASK_CHECK(fz->uflag == 0);          /* one-shot consumption */
    for (size_t i = 1; i < sizeof buf; ++i)
        TASK_CHECK(buf[i] == model[i]);  /* rest follows the normal path */

    /* A second legal read no longer overrides anything. */
    memset(buf, x, sizeof buf);
    prod_fuzz(fd, pos, buf, sizeof buf);
    TASK_CHECK(buf[0] == model[0]);
    _zz_unregister(fd);
    _zz_fd_fini();
    return 0;
}

/* upos miss: the flag is still consumed exactly once, but the fallback
 * byte is not inserted. */
static int case_pushback_miss(void)
{
    _zz_fd_init();
    zzuf_set_seed(1812);
    zzuf_set_ratio(0.013, 0.013);
    zzuf_protect_range("");
    zzuf_refuse_range("");
    prod_mode(REF_XOR);
    _zz_bytes("0-100000");

    int fd = 301;
    _zz_register(fd);
    const int64_t pos = 100;
    uint8_t buf[4], model[4];
    memset(buf, 0x33, sizeof buf);
    fuzz_context_t *fz = _zz_getfuzz(fd);
    fz->uflag = 1;
    fz->upos = pos + 5; /* mismatch */
    fz->uchar = 0x77;

    struct ref_interval iv[1] = { { 0, 100000 } };
    struct ref_policy pol = { REF_XOR, iv, 1,
                              (const unsigned char[256]){ 0 },
                              (const unsigned char[256]){ 0 } };
    ref_apply(buf, sizeof buf, pos, 1812, 0.013, &pol, model);
    prod_fuzz(fd, pos, buf, sizeof buf);
    TASK_CHECK(fz->uflag == 0);
    TASK_CHECK(buf[0] != 0x77);
    TASK_CHECK(buffers_equal(buf, model, sizeof buf));
    _zz_unregister(fd);
    _zz_fd_fini();
    return 0;
}

/* Pushback state belongs to one context; a flag set on A cannot affect B,
 * and a chunk-cache revisit after pushback still matches the oracle. */
static int case_pushback_isolation(void)
{
    _zz_fd_init();
    zzuf_set_seed(555);
    zzuf_set_ratio(0.017, 0.017);
    zzuf_protect_range("");
    zzuf_refuse_range("");
    prod_mode(REF_XOR);
    _zz_bytes("0-100000");

    int fd_a = 310, fd_b = 311;
    _zz_register(fd_a);
    _zz_register(fd_b);

    uint8_t a[REF_CB], b[REF_CB], expect[REF_CB], plain[REF_CB];
    memset(a, 0x44, sizeof a);
    memset(b, 0x44, sizeof b);
    memset(plain, 0x44, sizeof plain);
    struct ref_interval iv[1] = { { 0, 100000 } };
    struct ref_policy pol = { REF_XOR, iv, 1,
                              (const unsigned char[256]){ 0 },
                              (const unsigned char[256]){ 0 } };
    ref_apply(plain, REF_CB, REF_CB, 555, 0.017, &pol, expect);

    /* Arm pushback on A only. */
    fuzz_context_t *fa = _zz_getfuzz(fd_a);
    fa->uflag = 1;
    fa->upos = REF_CB;
    fa->uchar = 0x2e;

    /* B reads the same position: must follow the plain oracle. */
    prod_fuzz(fd_b, REF_CB, b, REF_CB);
    TASK_CHECK(b[0] == expect[0] && b[0] != 0x2e);
    TASK_CHECK(_zz_getfuzz(fd_b)->uflag == 0);

    /* A reads: fallback on byte 0, normal mutation everywhere else. */
    prod_fuzz(fd_a, REF_CB, a, REF_CB);
    TASK_CHECK(a[0] == 0x2e);
    for (size_t i = 1; i < REF_CB; ++i)
        TASK_CHECK(a[i] == expect[i]);

    /* Revisit the chunk on A; cache stays valid and no fallback remains. */
    uint8_t expect_zero[REF_CB], zero_in[REF_CB];
    memset(zero_in, 0, sizeof zero_in);
    ref_apply(zero_in, REF_CB, REF_CB, 555, 0.017, &pol, expect_zero);
    memset(a, 0, sizeof a);
    prod_fuzz(fd_a, REF_CB, a, REF_CB);
    TASK_CHECK(buffers_equal(a, expect_zero, REF_CB));
    _zz_unregister(fd_a);
    _zz_unregister(fd_b);
    _zz_fd_fini();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Registration per task_support/check.h                              */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] =
{
    { "mask_model",         "rng_anchor",              case_rng_anchor },
    { "mask_model",         "repeat_bit_cancels",      case_repeat_cancels },
    { "mask_model",         "low_density_rounding",    case_low_density_rounding },

    { "policy_composition", "charset_escapes",         case_charset_escapes },
    { "policy_composition", "modes_and_ranges",        case_modes_and_ranges },
    { "policy_composition", "protect_refuse_order",    case_protect_refuse_order },
    { "policy_composition", "whole_fragment_reread",   case_whole_fragment_reread },

    { "context_lifecycle",  "bind_at_register",        case_bind_at_register },
    { "context_lifecycle",  "reregister_no_cache",     case_reregister_no_cache },
    { "context_lifecycle",  "interleaved_contexts",    case_interleaved_contexts },
    { "context_lifecycle",  "cli_stdin_filter",        case_cli_stdin_filter },

    { "pushback_schedule",  "pushback_hit",            case_pushback_hit },
    { "pushback_schedule",  "pushback_miss",           case_pushback_miss },
    { "pushback_schedule",  "pushback_isolation",      case_pushback_isolation },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
