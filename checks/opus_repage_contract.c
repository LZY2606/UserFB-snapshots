/* Contract tests for the Ogg Opus re-pager.
 *
 * Everything the tests check is derived from first principles: pages
 * are parsed byte by byte, CRCs are recomputed with a private bitwise
 * implementation, packet durations and stream clocks are hand-computed
 * from the Opus TOC rules, and expected page layouts are either
 * hard-coded per case or recomputed from the retained input packets.
 * The implementation under test is never used as its own reference.
 */

#include "contract.h"

#include <ogg/ogg.h>
#include <ogg/opus_repage.h>

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- */
/* little-endian helpers                                            */

static ogg_uint32_t get_le32(const unsigned char *p) {
  return (ogg_uint32_t)p[0] | ((ogg_uint32_t)p[1] << 8) |
         ((ogg_uint32_t)p[2] << 16) | ((ogg_uint32_t)p[3] << 24);
}

static ogg_int64_t get_le64(const unsigned char *p) {
  ogg_uint64_t v = 0;
  int i;
  for (i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return (ogg_int64_t)v;
}

static void put_le32(unsigned char *p, ogg_uint32_t v) {
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
  p[2] = (unsigned char)((v >> 16) & 0xff);
  p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void put_le64(unsigned char *p, ogg_int64_t v) {
  ogg_uint64_t u = (ogg_uint64_t)v;
  int i;
  for (i = 0; i < 8; ++i) {
    p[i] = (unsigned char)(u & 0xff);
    u >>= 8;
  }
}

/* ---------------------------------------------------------------- */
/* independent CRC: Ogg polynomial, bitwise, no table shared with
   the implementation under test                                    */

static ogg_uint32_t crc_update(ogg_uint32_t crc, const unsigned char *d, long n) {
  long i;
  int b;
  for (i = 0; i < n; ++i) {
    crc ^= (ogg_uint32_t)d[i] << 24;
    for (b = 0; b < 8; ++b)
      crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04c11db7u : (crc << 1);
  }
  return crc;
}

/* ---------------------------------------------------------------- */
/* input page construction (with valid CRCs)                        */

typedef struct {
  unsigned char *storage; /* header+body contiguous */
  size_t len;
  ogg_page page;
} tpage;

static void tpage_build(tpage *tp, ogg_uint32_t serial, ogg_uint32_t seq,
                        int flags, ogg_int64_t granule,
                        const unsigned char *lace, int nsegs,
                        const unsigned char *body) {
  long body_len = 0;
  int i;
  unsigned char *h;
  ogg_uint32_t crc;
  for (i = 0; i < nsegs; ++i) body_len += lace[i];
  tp->len = (size_t)(27 + nsegs + body_len);
  tp->storage = (unsigned char *)malloc(tp->len ? tp->len : 1);
  h = tp->storage;
  memcpy(h, "OggS", 4);
  h[4] = 0;
  h[5] = (unsigned char)flags;
  put_le64(h + 6, granule);
  put_le32(h + 14, serial);
  put_le32(h + 18, seq);
  h[22] = h[23] = h[24] = h[25] = 0;
  h[26] = (unsigned char)nsegs;
  if (nsegs) memcpy(h + 27, lace, nsegs);
  if (body_len) memcpy(h + 27 + nsegs, body, body_len);
  crc = crc_update(0, h, (long)tp->len);
  put_le32(h + 22, crc);
  tp->page.header = h;
  tp->page.header_len = 27 + nsegs;
  tp->page.body = h + 27 + nsegs;
  tp->page.body_len = body_len;
}

static void tpage_free(tpage *tp) {
  free(tp->storage);
  tp->storage = 0;
  tp->page.header = 0;
  tp->page.body = 0;
}

static void tpage_recalc(tpage *tp) {
  ogg_uint32_t crc;
  tp->storage[22] = tp->storage[23] = tp->storage[24] = tp->storage[25] = 0;
  crc = crc_update(0, tp->storage, (long)tp->len);
  put_le32(tp->storage + 22, crc);
}

static void tpage_set_granule(tpage *tp, ogg_int64_t g) {
  put_le64(tp->storage + 6, g);
  tpage_recalc(tp);
}

static void tpage_set_flag(tpage *tp, int bit) {
  tp->storage[5] |= (unsigned char)bit;
  tpage_recalc(tp);
}

static void tpage_clear_flag(tpage *tp, int bit) {
  tp->storage[5] &= (unsigned char)~bit;
  tpage_recalc(tp);
}

static void tpage_set_serial(tpage *tp, ogg_uint32_t s) {
  put_le32(tp->storage + 14, s);
  tpage_recalc(tp);
}

static void tpage_set_seq(tpage *tp, ogg_uint32_t q) {
  put_le32(tp->storage + 18, q);
  tpage_recalc(tp);
}

/* ---------------------------------------------------------------- */
/* output page parser (structure + CRC verified independently)      */

typedef struct {
  int flags;
  ogg_int64_t granule;
  ogg_uint32_t serial, seq;
  int nsegs;
  const unsigned char *lace;
  const unsigned char *body;
  long body_len;
} tpg;

static int parse_page(const unsigned char *p, size_t len, tpg *out) {
  static const unsigned char zero4[4] = {0, 0, 0, 0};
  long body_len = 0;
  int i;
  ogg_uint32_t crc;
  if (len < 27) return 0;
  if (memcmp(p, "OggS", 4) != 0 || p[4] != 0) return 0;
  out->nsegs = p[26];
  if (len < (size_t)(27 + out->nsegs)) return 0;
  out->lace = p + 27;
  for (i = 0; i < out->nsegs; ++i) body_len += out->lace[i];
  if (len != (size_t)(27 + out->nsegs) + (size_t)body_len) return 0;
  out->flags = p[5];
  out->granule = get_le64(p + 6);
  out->serial = get_le32(p + 14);
  out->seq = get_le32(p + 18);
  out->body = p + 27 + out->nsegs;
  out->body_len = body_len;
  crc = crc_update(0, p, 22);
  crc = crc_update(crc, zero4, 4);
  crc = crc_update(crc, p + 26, (long)len - 26);
  return crc == get_le32(p + 22);
}

/* ---------------------------------------------------------------- */
/* output collector                                                 */

typedef struct {
  unsigned char **pages;
  size_t *lens;
  int n, cap;
  int fail_at; /* 1-based output page index to reject; 0 = never */
} tcollect;

static int tcollect_cb(void *u, const ogg_page *og) {
  tcollect *c = (tcollect *)u;
  long len;
  unsigned char *copy;
  if (c->fail_at && c->n + 1 == c->fail_at) return -1;
  len = og->header_len + og->body_len;
  copy = (unsigned char *)malloc((size_t)len);
  memcpy(copy, og->header, og->header_len);
  memcpy(copy + og->header_len, og->body, og->body_len);
  if (c->n == c->cap) {
    c->cap = c->cap ? c->cap * 2 : 32;
    c->pages = (unsigned char **)realloc(c->pages, sizeof(*c->pages) * c->cap);
    c->lens = (size_t *)realloc(c->lens, sizeof(*c->lens) * c->cap);
  }
  c->pages[c->n] = copy;
  c->lens[c->n] = (size_t)len;
  c->n++;
  return 0;
}

static void tcollect_free(tcollect *c) {
  int i;
  for (i = 0; i < c->n; ++i) free(c->pages[i]);
  free(c->pages);
  free(c->lens);
  c->pages = 0;
  c->lens = 0;
  c->n = c->cap = 0;
}

/* ---------------------------------------------------------------- */
/* tracking allocator                                               */

typedef struct {
  size_t cur, peak;
  int live;
  int nalloc;
  int fail_after; /* fail the Nth allocation (1-based); 0 = never */
} talloc;

static void *talloc_alloc(void *u, size_t n) {
  talloc *t = (talloc *)u;
  size_t *p;
  t->nalloc++;
  if (t->fail_after && t->nalloc >= t->fail_after) return 0;
  p = (size_t *)malloc(n + sizeof(size_t));
  if (!p) return 0;
  *p = n;
  t->cur += n;
  if (t->cur > t->peak) t->peak = t->cur;
  t->live++;
  return p + 1;
}

static void talloc_release(void *u, void *ptr) {
  talloc *t = (talloc *)u;
  size_t *p;
  if (!ptr) return;
  p = ((size_t *)ptr) - 1;
  t->cur -= *p;
  t->live--;
  free(p);
}

/* ---------------------------------------------------------------- */
/* test's own Opus clock                                            */

static long tdur(unsigned toc, unsigned b2) {
  static const short d0[4] = {480, 960, 1920, 2880};
  static const short d1[4] = {120, 240, 480, 960};
  int cfg = (toc >> 3) & 0x1f, code = toc & 3;
  long frame, count;
  if (cfg <= 11)
    frame = d0[cfg & 3];
  else if (cfg <= 15)
    frame = (cfg & 1) ? 960 : 480;
  else
    frame = d1[cfg & 3];
  if (code == 0)
    count = 1;
  else if (code <= 2)
    count = 2;
  else
    count = b2 & 0x3f;
  return frame * count;
}

/* ---------------------------------------------------------------- */
/* source stream builder: packets -> input pages with a legal Ogg
   Opus layout and a linear audio clock                              */

typedef struct {
  const unsigned char *data;
  long len;
  long dur;
} tapkt;

typedef struct {
  tpage *pages;
  int n, cap;
  ogg_uint32_t serial;
  ogg_uint32_t seq;
  unsigned char lace[255];
  unsigned char body[65280];
  int nsegs;
  long body_len;
  int continued;    /* continued flag of the page under construction */
  int open;         /* a packet is currently unterminated */
  int open_on_page; /* the open packet has segments on this page */
  long src_cap;
  ogg_int64_t cum;      /* completed audio durations */
  ogg_int64_t page_dur; /* durations completed on the current page */
  ogg_int64_t anchor;
  ogg_int64_t first_granule;
  int anchor_set;
} tsrc;

static void tsrc_init(tsrc *s, ogg_uint32_t serial, long src_cap,
                      ogg_int64_t first_granule) {
  memset(s, 0, sizeof(*s));
  s->serial = serial;
  s->src_cap = src_cap;
  s->first_granule = first_granule;
}

static void tsrc_free(tsrc *s) {
  int i;
  for (i = 0; i < s->n; ++i) tpage_free(&s->pages[i]);
  free(s->pages);
  s->pages = 0;
  s->n = s->cap = 0;
}

static void tsrc_grow(tsrc *s) {
  if (s->n == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 16;
    s->pages = (tpage *)realloc(s->pages, sizeof(tpage) * s->cap);
  }
}

static void tsrc_flush(tsrc *s, int eos, ogg_int64_t granule) {
  int flags;
  tsrc_grow(s);
  flags = s->continued | (eos ? 4 : 0);
  tpage_build(&s->pages[s->n], s->serial, s->seq, flags, granule, s->lace,
              s->nsegs, s->body);
  s->n++;
  s->seq++;
  s->nsegs = 0;
  s->body_len = 0;
  s->page_dur = 0;
  s->continued = (s->open && s->open_on_page) ? 1 : 0;
  s->open_on_page = 0;
}

static void tsrc_flush_auto(tsrc *s) {
  ogg_int64_t g = -1;
  if (s->page_dur > 0) {
    if (!s->anchor_set) {
      s->anchor = s->first_granule - s->cum;
      s->anchor_set = 1;
    }
    g = s->anchor + s->cum;
  }
  tsrc_flush(s, 0, g);
}

/* append one whole packet, splitting across source pages as needed;
   dur is the packet's audio duration (0 for header packets) */
static void tsrc_packet(tsrc *s, const unsigned char *d, long len, long dur) {
  long off = 0;
  s->open = 1;
  for (;;) {
    long seg = (len - off >= 255) ? 255 : len - off;
    if (s->nsegs == 255 || s->body_len + seg > s->src_cap) tsrc_flush_auto(s);
    s->lace[s->nsegs++] = (unsigned char)seg;
    if (seg) memcpy(s->body + s->body_len, d + off, seg);
    s->body_len += seg;
    s->open_on_page = 1;
    off += seg;
    if (seg < 255) break;
  }
  s->open = 0;
  s->open_on_page = 0; /* the next packet has no segments here yet */
  s->cum += dur;
  s->page_dur += dur;
}

static void tsrc_head(tsrc *s, const unsigned char *head19) {
  unsigned char lace = 19;
  tsrc_grow(s);
  tpage_build(&s->pages[s->n], s->serial, s->seq, 2, 0, &lace, 1, head19);
  s->n++;
  s->seq++;
}

static void tsrc_tags(tsrc *s, const unsigned char *tags, long len) {
  tsrc_packet(s, tags, len, 0);
  tsrc_flush(s, 0, 0); /* comment header ends at a page boundary */
}

static void tsrc_end(tsrc *s, ogg_int64_t trim) {
  tsrc_flush(s, 1, s->anchor + s->cum - trim);
}

/* ---------------------------------------------------------------- */
/* stream material helpers                                          */

static void make_head(unsigned char h[19], int channels, unsigned preskip) {
  memcpy(h, "OpusHead", 8);
  h[8] = 1;
  h[9] = (unsigned char)channels;
  h[10] = (unsigned char)(preskip & 0xff);
  h[11] = (unsigned char)(preskip >> 8);
  h[12] = h[13] = h[14] = h[15] = 0; /* input sample rate */
  h[16] = h[17] = 0;                 /* output gain */
  h[18] = 0;                         /* mapping family */
}

static unsigned char *make_bytes(long len, const char *prefix) {
  unsigned char *p = (unsigned char *)malloc(len ? len : 1);
  long i;
  for (i = 0; i < len; ++i) p[i] = (unsigned char)(i * 31 + 7);
  if (prefix) {
    long n = (long)strlen(prefix);
    if (n > len) n = len;
    for (i = 0; i < n; ++i) p[i] = (unsigned char)prefix[i];
  }
  return p;
}

static unsigned char *make_packet(long len, unsigned toc, unsigned b2) {
  unsigned char *p = make_bytes(len, 0);
  if (len > 0) p[0] = (unsigned char)toc;
  if (len > 1) p[1] = (unsigned char)b2;
  return p;
}

/* ---------------------------------------------------------------- */
/* runner                                                           */

typedef struct {
  ogg_opus_repage *rp;
  tcollect col;
  talloc al;
  ogg_opus_repage_allocator alloc;
} trun;

static void trun_new(trun *r, size_t limit) {
  memset(r, 0, sizeof(*r));
  r->alloc.user = &r->al;
  r->alloc.alloc = talloc_alloc;
  r->alloc.release = talloc_release;
  r->rp = ogg_opus_repage_new(limit, &r->alloc, tcollect_cb, &r->col);
}

static int feed_all(ogg_opus_repage *rp, const tsrc *s) {
  int i;
  for (i = 0; i < s->n; ++i)
    if (ogg_opus_repage_pagein(rp, &s->pages[i].page)) return -1;
  return 0;
}

/* ---------------------------------------------------------------- */
/* full independent verification of a repaged stream                */

static void verify_stream(const tcollect *c, const unsigned char *head,
                          const unsigned char *tags, long tags_len,
                          const tapkt *audio, int naudio, long limit,
                          ogg_uint32_t serial, ogg_int64_t offset,
                          ogg_int64_t eos_granule) {
  tpg *pg;
  unsigned char *pkt = 0;
  size_t pkt_len = 0, pkt_cap = 0;
  ogg_int64_t cum = 0;
  int pkt_idx = 0;
  int open = 0;
  int tags_page = -1;
  int n = c->n;
  int i, k;

  contract_expect(n >= 3);
  pg = (tpg *)malloc(sizeof(tpg) * n);

  for (k = 0; k < n; ++k) {
    contract_expect(parse_page(c->pages[k], c->lens[k], &pg[k]));
    contract_expect(pg[k].seq == (ogg_uint32_t)k);
    contract_expect(pg[k].serial == serial);
    contract_expect(pg[k].nsegs >= 1 && pg[k].nsegs <= 255);
    contract_expect(27 + pg[k].nsegs + pg[k].body_len <= limit);
  }

  for (k = 0; k < n; ++k) {
    long dur_here = 0;
    int tags_here = 0;
    ogg_int64_t want;
    const unsigned char *bp = pg[k].body;

    contract_expect((pg[k].flags & 1) == (open ? 1 : 0));
    if (k == 0)
      contract_expect((pg[k].flags & 2) != 0);
    else
      contract_expect((pg[k].flags & 2) == 0);
    if (k == n - 1)
      contract_expect((pg[k].flags & 4) != 0);
    else
      contract_expect((pg[k].flags & 4) == 0);

    for (i = 0; i < pg[k].nsegs; ++i) {
      int L = pg[k].lace[i];
      if (pkt_len + L > pkt_cap) {
        pkt_cap = (pkt_len + L) * 2 + 64;
        pkt = (unsigned char *)realloc(pkt, pkt_cap);
      }
      if (L) memcpy(pkt + pkt_len, bp, L);
      pkt_len += L;
      bp += L;
      if (L < 255) {
        /* a packet completes: compare against the retained original */
        const unsigned char *expd;
        long expl;
        contract_expect(pkt_idx < naudio + 2);
        if (pkt_idx == 0) {
          expd = head;
          expl = 19;
          contract_expect(pkt_len == (size_t)expl);
          contract_expect(memcmp(pkt, expd, expl) == 0);
        } else if (pkt_idx == 1) {
          expd = tags;
          expl = tags_len;
          contract_expect(pkt_len == (size_t)expl);
          contract_expect(memcmp(pkt, expd, expl) == 0);
          tags_here = 1;
        } else {
          const tapkt *a = &audio[pkt_idx - 2];
          expd = a->data;
          expl = a->len;
          contract_expect(pkt_len == (size_t)expl);
          contract_expect(memcmp(pkt, expd, expl) == 0);
          cum += a->dur;
          dur_here += a->dur;
        }
        pkt_idx++;
        pkt_len = 0;
      }
    }

    if (k == n - 1)
      want = eos_granule;
    else if (k == 0)
      want = 0;
    else if (tags_here)
      want = 0;
    else if (dur_here > 0)
      want = offset + cum;
    else
      want = -1;
    contract_expect(pg[k].granule == want);

    if (tags_here) tags_page = k;
    open = (pg[k].lace[pg[k].nsegs - 1] == 255);
  }
  contract_expect(pkt_idx == naudio + 2);

  /* fixed paging rule: a non-forced page boundary only happens when
     the next lacing value could not have fit on the previous page */
  for (k = 0; k < n - 1; ++k) {
    int full;
    if (k == 0 || k == tags_page) continue; /* forced breaks */
    full = (pg[k].nsegs == 255) ||
           (27 + pg[k].nsegs + 1 + pg[k].body_len + pg[k + 1].lace[0] > limit);
    contract_expect(full);
  }

  free(pkt);
  free(pg);
}

/* exact hard-coded layout expectation */
typedef struct {
  int nsegs;
  long body_len;
  ogg_int64_t granule;
  int flags;
} texp;

static void check_layout(const tcollect *c, const texp *exp, int n) {
  int k;
  contract_expect(c->n == n);
  for (k = 0; k < n && k < c->n; ++k) {
    tpg pg;
    contract_expect(parse_page(c->pages[k], c->lens[k], &pg));
    contract_expect(pg.nsegs == exp[k].nsegs);
    contract_expect(pg.body_len == exp[k].body_len);
    contract_expect(pg.granule == exp[k].granule);
    contract_expect(pg.flags == exp[k].flags);
  }
}

/* build a complete source stream */
static void build_stream(tsrc *s, ogg_uint32_t serial, long src_cap,
                         int channels, unsigned preskip,
                         const unsigned char *tags, long tags_len,
                         const tapkt *audio, int naudio,
                         ogg_int64_t first_granule, ogg_int64_t trim,
                         unsigned char *head_out) {
  int i;
  make_head(head_out, channels, preskip);
  tsrc_init(s, serial, src_cap, first_granule);
  tsrc_head(s, head_out);
  tsrc_tags(s, tags, tags_len);
  for (i = 0; i < naudio; ++i)
    tsrc_packet(s, audio[i].data, audio[i].len, audio[i].dur);
  tsrc_end(s, trim);
}

/* ================================================================ */
/* paging group                                                     */

/* OpusHead alone on the BOS page, forced break after OpusTags. */
static void pg_head_tags_forced_breaks(void) {
  unsigned char head[19];
  unsigned char *tags = make_bytes(12, "OpusTags");
  unsigned char *pa = make_packet(100, 0x20, 0);
  unsigned char *pb = make_packet(100, 0x20, 0);
  unsigned char *pc = make_packet(100, 0x20, 0);
  tapkt audio[3];
  tsrc s;
  trun r;
  static const texp want[3] = {
    {1, 19, 0, 2},
    {1, 12, 0, 0},
    {3, 300, 1440, 4},
  };
  audio[0].data = pa; audio[0].len = 100; audio[0].dur = 480;
  audio[1].data = pb; audio[1].len = 100; audio[1].dur = 480;
  audio[2].data = pc; audio[2].len = 100; audio[2].dur = 480;
  build_stream(&s, 11, 4096, 2, 0, tags, 12, audio, 3, 1440, 0, head);

  trun_new(&r, 512);
  contract_expect(r.rp != 0);
  contract_expect(feed_all(r.rp, &s) == 0);
  contract_expect(ogg_opus_repage_finish(r.rp) == 0);
  check_layout(&r.col, want, 3);
  /* the tags page is a forced break: far below the 512 cap */
  {
    tpg pg;
    contract_expect(parse_page(r.col.pages[1], r.col.lens[1], &pg));
    contract_expect(27 + pg.nsegs + pg.body_len < 100);
  }
  verify_stream(&r.col, head, tags, 12, audio, 3, 512, 11, 0, 1440);
  ogg_opus_repage_free(r.rp);
  contract_expect(r.al.live == 0);
  contract_expect(r.al.peak <= 512 + 80000);
  tcollect_free(&r.col);
  tsrc_free(&s);
  free(tags); free(pa); free(pb); free(pc);
}

/* exact page layout at the 512 byte cap, hand-computed */
static void pg_limit_512_exact_layout(void) {
  unsigned char head[19];
  unsigned char *tags = make_bytes(12, "OpusTags");
  unsigned char *p0 = make_packet(300, 0x20, 0);
  unsigned char *p1 = make_packet(300, 0x20, 0);
  unsigned char *p2 = make_packet(100, 0x20, 0);
  tapkt audio[3];
  tsrc s;
  trun r;
  /* P0 [255,45] fills page 2 (329 of 512); P1's first segment would
     make 585, so page 2 closes.  Page 3 packs P1 [255,45] and P2
     [100] (430 of 512) and carries EOS. */
  static const texp want[4] = {
    {1, 19, 0, 2},
    {1, 12, 0, 0},
    {2, 300, 480, 0},
    {3, 400, 1440, 4},
  };
  audio[0].data = p0; audio[0].len = 300; audio[0].dur = 480;
  audio[1].data = p1; audio[1].len = 300; audio[1].dur = 480;
  audio[2].data = p2; audio[2].len = 100; audio[2].dur = 480;
  build_stream(&s, 12, 4096, 2, 0, tags, 12, audio, 3, 1440, 0, head);

  trun_new(&r, 512);
  contract_expect(r.rp != 0);
  contract_expect(feed_all(r.rp, &s) == 0);
  contract_expect(ogg_opus_repage_finish(r.rp) == 0);
  check_layout(&r.col, want, 4);
  verify_stream(&r.col, head, tags, 12, audio, 3, 512, 12, 0, 1440);
  ogg_opus_repage_free(r.rp);
  contract_expect(r.al.live == 0);
  tcollect_free(&r.col);
  tsrc_free(&s);
  free(tags); free(p0); free(p1); free(p2);
}

/* non-round page cap, hand-computed */
static void pg_odd_limit_1000(void) {
  unsigned char head[19];
  unsigned char *tags = make_bytes(12, "OpusTags");
  unsigned char *pa = make_packet(600, 0x20, 0);
  unsigned char *pb = make_packet(600, 0x20, 0);
  tapkt audio[2];
  tsrc s;
  trun r;
  /* A [255,255,90] = 630, then B's first 255 still fits (886) but not
     its second (1142): page 2 = [255,255,90,255].  Page 3 therefore
     starts in the middle of B and carries the continued flag. */
  static const texp want[4] = {
    {1, 19, 0, 2},
    {1, 12, 0, 0},
    {4, 855, 480, 0},
    {2, 345, 960, 5},
  };
  audio[0].data = pa; audio[0].len = 600; audio[0].dur = 480;
  audio[1].data = pb; audio[1].len = 600; audio[1].dur = 480;
  build_stream(&s, 13, 4096, 1, 0, tags, 12, audio, 2, 960, 0, head);

  trun_new(&r, 1000);
  contract_expect(r.rp != 0);
  contract_expect(feed_all(r.rp, &s) == 0);
  contract_expect(ogg_opus_repage_finish(r.rp) == 0);
  check_layout(&r.col, want, 4);
  verify_stream(&r.col, head, tags, 12, audio, 2, 1000, 13, 0, 960);
  ogg_opus_repage_free(r.rp);
  contract_expect(r.al.live == 0);
  tcollect_free(&r.col);
  tsrc_free(&s);
  free(tags); free(pa); free(pb);
}

/* a packet split across three pages: middle pages carry no
   completed packet and get granule -1 */
static void pg_mid_packet_split_continued(void) {
  unsigned char head[19];
  unsigned char *tags = make_bytes(12, "OpusTags");
  unsigned char *pc = make_packet(900, 0x20, 0);
  tapkt audio[1];
  tsrc s;
  trun r;
  static const texp want[5] = {
    {1, 19, 0, 2},
    {1, 12, 0, 0},
    {1, 255, -1, 0},
    {1, 255, -1, 1},
    {2, 390, 480, 5},
  };
  audio[0].data = pc; audio[0].len = 900; audio[0].dur = 480;
  build_stream(&s, 14, 4096, 2, 0, tags, 12, audio, 1, 480, 0, head);

  trun_new(&r, 512);
  contract_expect(r.rp != 0);
  contract_expect(feed_all(r.rp, &s) == 0);
  contract_expect(ogg_opus_repage_finish(r.rp) == 0);
  check_layout(&r.col, want, 5);
  verify_stream(&r.col, head, tags, 12, audio, 1, 512, 14, 0, 480);
  ogg_opus_repage_free(r.rp);
  contract_expect(r.al.live == 0);
  tcollect_free(&r.col);
  tsrc_free(&s);
  free(tags); free(pc);
}

/* packets whose length is a multiple of 255 keep their zero-length
   terminator and never turn into two packets */
static void pg_lace255_zero_terminator(void) {
  unsigned char head[19];
  unsigned char *tags = make_bytes(12, "OpusTags");
  unsigned char *pe = make_packet(510, 0x20, 0);
  unsigned char *pf = make_packet(255, 0x20, 0);
  unsigned char *pg2 = make_packet(255, 0x20, 0);
  tapkt audio[3];
  tsrc s;
  trun r;
  static const texp want[6] = {
    {1, 19, 0, 2},
    {1, 12, 0, 0},
    {1, 255, -1, 0},
    {2, 255, 480, 1},
    {2, 255, 960, 0},
    {2, 255, 1440, 4},
  };
  audio[0].data = pe; audio[0].len = 510; audio[0].dur = 480;
  audio[1].data = pf; audio[1].len = 255; audio[1].dur = 480;
  audio[2].data = pg2; audio[2].len = 255; audio[2].dur = 480;
  build_stream(&s, 15, 4096, 2, 0, tags, 12, audio, 3, 480, 0, head);

  trun_new(&r, 512);
  contract_expect(r.rp != 0);
  contract_expect(feed_all(r.rp, &s) == 0);
  contract_expect(ogg_opus_repage_finish(r.rp) == 0);
  check_layout(&r.col, want, 6);
  verify_stream(&r.col, head, tags, 12, audio, 3, 512, 15, 0, 1440);
  ogg_opus_repage_free(r.rp);
  contract_expect(r.al.live == 0);
  tcollect_free(&r.col);
  tsrc_free(&s);
  free(tags); free(pe); free(pf); free(pg2);
}

/* one page completes several packets and then carries part of the
   next one; its granule belongs to the last completed packet */
static void pg_multi_complete_and_partial(void) {
  unsigned char head[19];
  unsigned char *tags = make_bytes(12, "OpusTags");
  unsigned char *p[6];
  tapkt audio[6];
  tsrc s;
  trun r;
  int i;
  static const texp want[6] = {
    {1, 19, 0, 2},
    {1, 12, 0, 0},
    {4, 400, 1920, 0},
    {2, 355, 2400, 0},
    {1, 255, -1, 1},
    {2, 390, 2880, 5},
  };
  for (i = 0; i < 5; ++i) {
    p[i] = make_packet(100, 0x20, 0);
    audio[i].data = p[i]; audio[i].len = 100; audio[i].dur = 480;
  }
  p[5] = make_packet(900, 0x20, 0);
  audio[5].data = p[5]; audio[5].len = 900; audio[5].dur = 480;
  build_stream(&s, 16, 4096, 2, 0, tags, 12, audio, 6, 480, 0, head);

  trun_new(&r, 512);
  contract_expect(r.rp != 0);
  contract_expect(feed_all(r.rp, &s) == 0);
  contract_expect(ogg_opus_repage_finish(r.rp) == 0);
  check_layout(&r.col, want, 6);
  verify_stream(&r.col, head, tags, 12, audio, 6, 512, 16, 0, 2880);
  ogg_opus_repage_free(r.rp);
  contract_expect(r.al.live == 0);
  tcollect_free(&r.col);
  tsrc_free(&s);
  free(tags);
  for (i = 0; i < 6; ++i) free(p[i]);
}

/* input page boundaries must not trigger output: the same packets
   fed one segment per input page produce the same hard-coded output */
static void pg_input_boundaries_ignored(void) {
  unsigned char head[19];
  unsigned char *tags = make_bytes(12, "OpusTags");
  unsigned char *p0 = make_packet(300, 0x20, 0);
  unsigned char *p1 = make_packet(300, 0x20, 0);
  unsigned char *p2 = make_packet(100, 0x20, 0);
  tapkt audio[3];
  tsrc s;
  trun r;
  static const texp want[4] = {
    {1, 19, 0, 2},
    {1, 12, 0, 0},
    {2, 300, 480, 0},
    {3, 400, 1440, 4},
  };
  audio[0].data = p0; audio[0].len = 300; audio[0].dur = 480;
  audio[1].data = p1; audio[1].len = 300; audio[1].dur = 480;
  audio[2].data = p2; audio[2].len = 100; audio[2].dur = 480;
  /* src_cap 255: input pages carry one or two small segments each */
  build_stream(&s, 17, 255, 2, 0, tags, 12, audio, 3, 480, 0, head);
  contract_expect(s.n >= 6); /* fragmented input paging */

  trun_new(&r, 512);
  contract_expect(r.rp != 0);
  contract_expect(feed_all(r.rp, &s) == 0);
  contract_expect(ogg_opus_repage_finish(r.rp) == 0);
  check_layout(&r.col, want, 4);
  verify_stream(&r.col, head, tags, 12, audio, 3, 512, 17, 0, 1440);
  ogg_opus_repage_free(r.rp);
  contract_expect(r.al.live == 0);
  tcollect_free(&r.col);
  tsrc_free(&s);
  free(tags); free(p0); free(p1); free(p2);
}

/* a final packet ending exactly at the page cap: EOS rides on that
   page, no empty extra page is generated */
static void pg_eos_on_exact_full_page(void) {
  unsigned char head[19];
  unsigned char *tags = make_bytes(12, "OpusTags");
  unsigned char *pa = make_packet(483, 0x20, 0); /* 27+2+483 = 512 */
  tapkt audio[1];
  tsrc s;
  trun r;
  static const texp want[3] = {
    {1, 19, 0, 2},
    {1, 12, 0, 0},
    {2, 483, 480, 4},
  };
  audio[0].data = pa; audio[0].len = 483; audio[0].dur = 480;
  build_stream(&s, 18, 4096, 2, 0, tags, 12, audio, 1, 480, 0, head);

  trun_new(&r, 512);
  contract_expect(r.rp != 0);
  contract_expect(feed_all(r.rp, &s) == 0);
  contract_expect(ogg_opus_repage_finish(r.rp) == 0);
  check_layout(&r.col, want, 3); /* exactly 3 pages: no empty EOS page */
  {
    tpg pg;
    contract_expect(parse_page(r.col.pages[2], r.col.lens[2], &pg));
    contract_expect(27 + pg.nsegs + pg.body_len == 512);
  }
  verify_stream(&r.col, head, tags, 12, audio, 1, 512, 18, 0, 480);
  ogg_opus_repage_free(r.rp);
  contract_expect(r.al.live == 0);
  tcollect_free(&r.col);
  tsrc_free(&s);
  free(tags); free(pa);
}

/* unsupported or malformed headers are rejected */
static void pg_unsupported_heads_rejected(void) {
  unsigned char head[19];
  int i;
  for (i = 0; i < 7; ++i) {
    unsigned char lace[2] = {19, 0};
    unsigned char body[24];
    tpage tp;
    trun r;
    int nsegs = 1;
    make_head(head, 2, 0);
    memset(body, 0, sizeof(body));
    memcpy(body, head, 19);
    switch (i) {
    case 0: body[8] = 2; break;             /* version */
    case 1: body[9] = 0; break;             /* channels */
    case 2: body[9] = 3; break;             /* channels */
    case 3: body[18] = 1; break;            /* mapping family */
    case 4: lace[0] = 18; break;            /* wrong size */
    case 5: memcpy(body, "OpusHeap", 8); break;
    case 6: lace[1] = 5; nsegs = 2; break;  /* head not alone */
    default: break;
    }
    tpage_build(&tp, 21, 0, 2, 0, lace, nsegs, body);
    trun_new(&r, 512);
    contract_expect(r.rp != 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &tp.page) == -1);
    contract_expect(ogg_opus_repage_pagein(r.rp, &tp.page) == -1);
    contract_expect(ogg_opus_repage_finish(r.rp) == -1);
    ogg_opus_repage_free(r.rp);
    contract_expect(r.al.live == 0);
    tcollect_free(&r.col);
    tpage_free(&tp);
  }
  /* bad OpusTags magic */
  {
    unsigned char good_head[19];
    unsigned char *tags = make_bytes(12, "OpusTogs");
    tsrc s;
    trun r;
    make_head(good_head, 2, 0);
    tsrc_init(&s, 22, 4096, 0);
    tsrc_head(&s, good_head);
    tsrc_tags(&s, tags, 12);
    trun_new(&r, 512);
    contract_expect(r.rp != 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[0].page) == 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[1].page) == -1);
    contract_expect(ogg_opus_repage_finish(r.rp) == -1);
    ogg_opus_repage_free(r.rp);
    contract_expect(r.al.live == 0);
    tcollect_free(&r.col);
    tsrc_free(&s);
    free(tags);
  }
}

/* broken sequence invariants are detected */
static void pg_sequence_breaks_rejected(void) {
  unsigned char head[19];
  unsigned char *tags = make_bytes(12, "OpusTags");
  unsigned char *pa = make_packet(100, 0x20, 0);
  tapkt audio[1];
  int which;
  audio[0].data = pa; audio[0].len = 100; audio[0].dur = 480;
  for (which = 0; which < 3; ++which) {
    tsrc s;
    trun r;
    build_stream(&s, 23, 4096, 2, 0, tags, 12, audio, 1, 480, 0, head);
    if (which == 0) tpage_set_flag(&s.pages[1], 1);        /* bad continuation */
    if (which == 1) tpage_set_serial(&s.pages[1], 999);    /* serial mismatch */
    if (which == 2) tpage_set_seq(&s.pages[1], 7);         /* page number skip */
    trun_new(&r, 512);
    contract_expect(r.rp != 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[0].page) == 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[1].page) == -1);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[2].page) == -1);
    contract_expect(ogg_opus_repage_finish(r.rp) == -1);
    ogg_opus_repage_free(r.rp);
    contract_expect(r.al.live == 0);
    tcollect_free(&r.col);
    tsrc_free(&s);
  }
  free(tags); free(pa);
}

/* ================================================================ */
/* timing group                                                     */

/* every TOC duration class, one packet per output page, granules
   checked against a hand-computed cumulative sequence */
static void tm_toc_duration_grid(void) {
  unsigned char head[19];
  unsigned char *tags = make_bytes(12, "OpusTags");
  static const unsigned tocs[11] = {
    0x00, 0x08, 0x11, 0x1A, 0x60, 0x68, 0x79, 0x80, 0x9B, 0xFB, 0xA0
  };
  static const unsigned b2s[11] = {0, 0, 0, 0, 0, 0, 0, 0, 5, 6, 0};
  static const ogg_int64_t want_granule[11] = {
    480, 1440, 5280, 11040, 11520, 12480, 14400, 14520, 19320, 25080, 25200
  };
  unsigned char *p[11];
  tapkt audio[11];
  tsrc s;
  trun r;
  int i, k;
  for (i = 0; i < 11; ++i) {
    p[i] = make_packet(300, tocs[i], b2s[i]);
    audio[i].data = p[i];
    audio[i].len = 300;
    audio[i].dur = tdur(tocs[i], b2s[i]);
  }
  contract_expect(audio[0].dur == 480);   /* config 0, 1 frame */
  contract_expect(audio[1].dur == 960);   /* config 1 */
  contract_expect(audio[2].dur == 3840);  /* config 2, 2 frames */
  contract_expect(audio[3].dur == 5760);  /* config 3, 2 frames */
  contract_expect(audio[4].dur == 480);   /* config 12 */
  contract_expect(audio[5].dur == 960);   /* config 13 */
  contract_expect(audio[6].dur == 1920);  /* config 15, 2 frames */
  contract_expect(audio[7].dur == 120);   /* config 16 */
  contract_expect(audio[8].dur == 4800);  /* config 19, 5 frames */
  contract_expect(audio[9].dur == 5760);  /* config 31, 6 frames */
  contract_expect(audio[10].dur == 120);  /* config 20 */
  build_stream(&s, 31, 4096, 2, 0, tags, 12, audio, 11, 25200, 0, head);

  trun_new(&r, 512);
  contract_expect(r.rp != 0);
  contract_expect(feed_all(r.rp, &s) == 0);
  contract_expect(ogg_opus_repage_finish(r.rp) == 0);
  contract_expect(r.col.n == 13);
  /* pages 2..12 complete exactly one packet each */
  for (k = 0; k < 11; ++k) {
    tpg pg;
    contract_expect(parse_page(r.col.pages[2 + k], r.col.lens[2 + k], &pg));
    contract_expect(pg.granule == want_granule[k]);
    contract_expect(pg.nsegs == 2);
  }
  {
    tpg pg;
    contract_expect(parse_page(r.col.pages[12], r.col.lens[12], &pg));
    contract_expect((pg.flags & 4) != 0);
    contract_expect(pg.granule == 25200);
  }
  verify_stream(&r.col, head, tags, 12, audio, 11, 512, 31, 0, 25200);
  ogg_opus_repage_free(r.rp);
  contract_expect(r.al.live == 0);
  tcollect_free(&r.col);
  tsrc_free(&s);
  free(tags);
  for (i = 0; i < 11; ++i) free(p[i]);
}

/* non-zero initial offset from the first anchored source page */
static void tm_nonzero_initial_offset(void) {
  unsigned char head[19];
  unsigned char *tags = make_bytes(12, "OpusTags");
  unsigned char *p[4];
  tapkt audio[4];
  tsrc s;
  trun r;
  int i, k;
  static const ogg_int64_t want_granule[4] = {4040, 5000, 5960, 6920};
  for (i = 0; i < 4; ++i) {
    p[i] = make_packet(300, 0x08, 0); /* 960 each */
    audio[i].data = p[i]; audio[i].len = 300; audio[i].dur = 960;
  }
  /* two packets per source page; first anchored page says 5000 for
     1920 samples of audio, so the offset is 3080 */
  build_stream(&s, 32, 600, 2, 0, tags, 12, audio, 4, 5000, 0, head);

  trun_new(&r, 512);
  contract_expect(r.rp != 0);
  contract_expect(feed_all(r.rp, &s) == 0);
  contract_expect(ogg_opus_repage_finish(r.rp) == 0);
  contract_expect(r.col.n == 6);
  for (k = 0; k < 4; ++k) {
    tpg pg;
    contract_expect(parse_page(r.col.pages[2 + k], r.col.lens[2 + k], &pg));
    contract_expect(pg.granule == want_granule[k]);
  }
  verify_stream(&r.col, head, tags, 12, audio, 4, 512, 32, 3080, 6920);
  ogg_opus_repage_free(r.rp);
  contract_expect(r.al.live == 0);
  tcollect_free(&r.col);
  tsrc_free(&s);
  free(tags);
  for (i = 0; i < 4; ++i) free(p[i]);
}

/* pre-skip spanning several packets: the offset goes negative and
   early output pages keep the formula's exact (negative) times */
static void tm_preskip_spanning_packets(void) {
  unsigned char head[19];
  unsigned char *tags = make_bytes(12, "OpusTags");
  unsigned char *p[6];
  tapkt audio[6];
  tsrc s;
  trun r;
  int i, k;
  static const ogg_int64_t want_granule[6] = {
    -1540, -580, 380, 1340, 2300, 3260
  };
  for (i = 0; i < 6; ++i) {
    p[i] = make_packet(300, 0x08, 0); /* 960 each */
    audio[i].data = p[i]; audio[i].len = 300; audio[i].dur = 960;
  }
  /* pre-skip 2500 spans more than two 960-sample packets; the first
     source page completes three of them and reads 380 */
  build_stream(&s, 33, 900, 2, 2500, tags, 12, audio, 6, 380, 0, head);

  trun_new(&r, 512);
  contract_expect(r.rp != 0);
  contract_expect(feed_all(r.rp, &s) == 0);
  contract_expect(ogg_opus_repage_finish(r.rp) == 0);
  contract_expect(r.col.n == 8);
  for (k = 0; k < 6; ++k) {
    tpg pg;
    contract_expect(parse_page(r.col.pages[2 + k], r.col.lens[2 + k], &pg));
    contract_expect(pg.granule == want_granule[k]);
  }
  verify_stream(&r.col, head, tags, 12, audio, 6, 512, 33, -2500, 3260);
  ogg_opus_repage_free(r.rp);
  contract_expect(r.al.live == 0);
  tcollect_free(&r.col);
  tsrc_free(&s);
  free(tags);
  for (i = 0; i < 6; ++i) free(p[i]);
}

/* EOS tail trim: the output EOS page keeps the trimmed granule */
static void tm_eos_tail_trim(void) {
  unsigned char head[19];
  unsigned char *tags = make_bytes(12, "OpusTags");
  unsigned char *p[5];
  tapkt audio[5];
  tsrc s;
  trun r;
  int i, k;
  static const ogg_int64_t want_granule[5] = {960, 1920, 2880, 3840, 4300};
  for (i = 0; i < 5; ++i) {
    p[i] = make_packet(300, 0x08, 0);
    audio[i].data = p[i]; audio[i].len = 300; audio[i].dur = 960;
  }
  build_stream(&s, 34, 4096, 2, 0, tags, 12, audio, 5, 4800, 500, head);

  trun_new(&r, 512);
  contract_expect(r.rp != 0);
  contract_expect(feed_all(r.rp, &s) == 0);
  contract_expect(ogg_opus_repage_finish(r.rp) == 0);
  contract_expect(r.col.n == 7);
  for (k = 0; k < 5; ++k) {
    tpg pg;
    contract_expect(parse_page(r.col.pages[2 + k], r.col.lens[2 + k], &pg));
    contract_expect(pg.granule == want_granule[k]);
  }
  verify_stream(&r.col, head, tags, 12, audio, 5, 512, 34, 0, 4300);
  ogg_opus_repage_free(r.rp);
  contract_expect(r.al.live == 0);
  tcollect_free(&r.col);
  tsrc_free(&s);
  free(tags);
  for (i = 0; i < 5; ++i) free(p[i]);
}

/* several packets completing on a single source page anchor the
   clock with their summed duration */
static void tm_multi_complete_source_page(void) {
  unsigned char head[19];
  unsigned char *tags = make_bytes(12, "OpusTags");
  unsigned char *p[6];
  tapkt audio[6];
  tsrc s;
  trun r;
  int i;
  static const texp want[4] = {
    {1, 19, 0, 2},
    {1, 12, 0, 0},
    {4, 400, 1920, 0},
    {2, 200, 2880, 4},
  };
  for (i = 0; i < 6; ++i) {
    p[i] = make_packet(100, 0x20, 0); /* 480 each */
    audio[i].data = p[i]; audio[i].len = 100; audio[i].dur = 480;
  }
  build_stream(&s, 35, 4096, 2, 0, tags, 12, audio, 6, 2880, 0, head);

  trun_new(&r, 512);
  contract_expect(r.rp != 0);
  contract_expect(feed_all(r.rp, &s) == 0);
  contract_expect(ogg_opus_repage_finish(r.rp) == 0);
  check_layout(&r.col, want, 4);
  verify_stream(&r.col, head, tags, 12, audio, 6, 512, 35, 0, 2880);
  ogg_opus_repage_free(r.rp);
  contract_expect(r.al.live == 0);
  tcollect_free(&r.col);
  tsrc_free(&s);
  free(tags);
  for (i = 0; i < 6; ++i) free(p[i]);
}

/* clock and EOS inconsistencies are rejected */
static void tm_clock_errors_rejected(void) {
  unsigned char head[19];
  unsigned char *tags = make_bytes(12, "OpusTags");
  unsigned char *pa = make_packet(300, 0x08, 0);
  unsigned char *pb = make_packet(300, 0x08, 0);
  unsigned char *pc = make_packet(300, 0x08, 0);
  unsigned char *pd = make_packet(300, 0x08, 0);
  tapkt audio4[4];
  int i;
  for (i = 0; i < 4; ++i) {
    unsigned char *q = i == 0 ? pa : i == 1 ? pb : i == 2 ? pc : pd;
    audio4[i].data = q; audio4[i].len = 300; audio4[i].dur = 960;
  }

  /* granule jump on a middle source page */
  {
    tsrc s;
    trun r;
    /* one packet per source page: pages 2..5 carry 960 each and must
       read 960, 1920, 2880, 3840 */
    build_stream(&s, 41, 300, 2, 0, tags, 12, audio4, 4, 960, 0, head);
    contract_expect(s.n == 6);
    tpage_set_granule(&s.pages[3], 1921); /* should be 1920 */
    trun_new(&r, 512);
    contract_expect(r.rp != 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[0].page) == 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[1].page) == 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[2].page) == 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[3].page) == -1);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[4].page) == -1);
    contract_expect(ogg_opus_repage_finish(r.rp) == -1);
    ogg_opus_repage_free(r.rp);
    contract_expect(r.al.live == 0);
    tcollect_free(&r.col);
    tsrc_free(&s);
  }
  /* EOS granule beyond the linear extrapolation */
  {
    tsrc s;
    trun r;
    build_stream(&s, 42, 600, 2, 0, tags, 12, audio4, 4, 1920, 0, head);
    tpage_set_granule(&s.pages[3], 3841); /* EOS may only end early */
    trun_new(&r, 512);
    contract_expect(r.rp != 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[0].page) == 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[1].page) == 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[2].page) == 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[3].page) == -1);
    contract_expect(ogg_opus_repage_finish(r.rp) == -1);
    ogg_opus_repage_free(r.rp);
    contract_expect(r.al.live == 0);
    tcollect_free(&r.col);
    tsrc_free(&s);
  }
  /* tail trim as large as the last packet is unsupported */
  {
    tsrc s;
    trun r;
    tapkt a2[2];
    a2[0] = audio4[0];
    a2[1] = audio4[1];
    build_stream(&s, 43, 4096, 2, 0, tags, 12, a2, 2, 1920, 1920, head);
    trun_new(&r, 512);
    contract_expect(r.rp != 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[0].page) == 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[1].page) == 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[2].page) == -1);
    contract_expect(ogg_opus_repage_finish(r.rp) == -1);
    ogg_opus_repage_free(r.rp);
    contract_expect(r.al.live == 0);
    tcollect_free(&r.col);
    tsrc_free(&s);
  }
  /* EOS granule below the OpusHead pre-skip */
  {
    tsrc s;
    trun r;
    tapkt a1[1];
    unsigned char *p120 = make_packet(300, 0x80, 0); /* 120 samples */
    a1[0].data = p120; a1[0].len = 300; a1[0].dur = 120;
    build_stream(&s, 44, 4096, 2, 312, tags, 12, a1, 1, 100, 0, head);
    trun_new(&r, 512);
    contract_expect(r.rp != 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[0].page) == 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[1].page) == 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[2].page) == -1);
    contract_expect(ogg_opus_repage_finish(r.rp) == -1);
    ogg_opus_repage_free(r.rp);
    contract_expect(r.al.live == 0);
    tcollect_free(&r.col);
    tsrc_free(&s);
    free(p120);
  }
  free(tags); free(pa); free(pb); free(pc); free(pd);
}

/* invalid packet timing data is rejected */
static void tm_invalid_toc_rejected(void) {
  unsigned char head[19];
  unsigned char *tags = make_bytes(12, "OpusTags");
  int which;
  for (which = 0; which < 3; ++which) {
    tsrc s;
    trun r;
    tapkt a1[1];
    unsigned char *pkt;
    if (which == 0) pkt = make_packet(300, 0xFB, 0); /* zero frame count */
    else if (which == 1) pkt = make_packet(300, 0xFB, 7); /* 6720 > 5760 */
    else pkt = make_packet(0, 0, 0); /* zero-length audio packet */
    a1[0].data = pkt;
    a1[0].len = which == 2 ? 0 : 300;
    a1[0].dur = 0;
    make_head(head, 2, 0);
    tsrc_init(&s, 45, 4096, 0);
    tsrc_head(&s, head);
    tsrc_tags(&s, tags, 12);
    tsrc_packet(&s, a1[0].data, a1[0].len, 0);
    tsrc_end(&s, 0);
    trun_new(&r, 512);
    contract_expect(r.rp != 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[0].page) == 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[1].page) == 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[2].page) == -1);
    contract_expect(ogg_opus_repage_finish(r.rp) == -1);
    ogg_opus_repage_free(r.rp);
    contract_expect(r.al.live == 0);
    tcollect_free(&r.col);
    tsrc_free(&s);
    free(pkt);
  }
  free(tags);
}

/* an EOS page arriving in the middle of a packet is rejected */
static void tm_unfinished_packet_rejected(void) {
  unsigned char head[19];
  unsigned char *tags = make_bytes(12, "OpusTags");
  unsigned char *pkt = make_packet(510, 0x20, 0);
  tpage pages[4];
  unsigned char lace1 = 19, lace2 = 12, lace3 = 255, lace4 = 255;
  trun r;
  make_head(head, 2, 0);
  tpage_build(&pages[0], 46, 0, 2, 0, &lace1, 1, head);
  tpage_build(&pages[1], 46, 1, 0, 0, &lace2, 1, tags);
  tpage_build(&pages[2], 46, 2, 0, -1, &lace3, 1, pkt);
  tpage_build(&pages[3], 46, 3, 1 | 4, 480, &lace4, 1, pkt + 255);
  trun_new(&r, 512);
  contract_expect(r.rp != 0);
  contract_expect(ogg_opus_repage_pagein(r.rp, &pages[0].page) == 0);
  contract_expect(ogg_opus_repage_pagein(r.rp, &pages[1].page) == 0);
  contract_expect(ogg_opus_repage_pagein(r.rp, &pages[2].page) == 0);
  /* EOS lands with the packet still unterminated */
  contract_expect(ogg_opus_repage_pagein(r.rp, &pages[3].page) == -1);
  contract_expect(ogg_opus_repage_finish(r.rp) == -1);
  ogg_opus_repage_free(r.rp);
  contract_expect(r.al.live == 0);
  tcollect_free(&r.col);
  tpage_free(&pages[0]);
  tpage_free(&pages[1]);
  tpage_free(&pages[2]);
  tpage_free(&pages[3]);
  free(tags);
  free(pkt);
}

/* ================================================================ */
/* streaming group                                                  */

/* multi-MiB OpusTags across many pages: bounded memory, output
   during pagein, input pages released immediately */
static void st_huge_tags_streaming(void) {
  unsigned char head[19];
  long tags_len = 3 * 1024 * 1024 + 123;
  unsigned char *tags = make_bytes(tags_len, "OpusTags");
  unsigned char *pa = make_packet(300, 0x08, 0);
  unsigned char *pb = make_packet(300, 0x08, 0);
  tapkt audio[2];
  tsrc s;
  trun r;
  int i;
  audio[0].data = pa; audio[0].len = 300; audio[0].dur = 960;
  audio[1].data = pb; audio[1].len = 300; audio[1].dur = 960;
  build_stream(&s, 51, 60000, 2, 0, tags, tags_len, audio, 2, 1920, 0, head);
  contract_expect(s.n > 50);

  trun_new(&r, 512);
  contract_expect(r.rp != 0);
  for (i = 0; i < s.n; ++i) {
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[i].page) == 0);
    tpage_free(&s.pages[i]); /* released while the stream continues */
    if (i == 1) contract_expect(r.col.n > 0); /* output during pagein */
  }
  contract_expect(r.col.n > 1000); /* all emitted before finish */
  contract_expect(ogg_opus_repage_finish(r.rp) == 0);
  verify_stream(&r.col, head, tags, tags_len, audio, 2, 512, 51, 0, 1920);
  ogg_opus_repage_free(r.rp);
  contract_expect(r.al.live == 0);
  contract_expect(r.al.cur == 0);
  contract_expect(r.al.peak <= 512 + 80000);
  contract_expect(r.al.nalloc == 2); /* no steady-state allocation */
  tcollect_free(&r.col);
  tsrc_free(&s);
  free(tags); free(pa); free(pb);
}

/* one audio packet spanning many input and output pages */
static void st_spanning_audio_packet(void) {
  unsigned char head[19];
  unsigned char *tags = make_bytes(12, "OpusTags");
  unsigned char *big = make_packet(200000, 0x20, 0);
  tapkt audio[1];
  tsrc s;
  trun r;
  int k, saw_middle;
  audio[0].data = big; audio[0].len = 200000; audio[0].dur = 480;
  build_stream(&s, 52, 4096, 2, 0, tags, 12, audio, 1, 480, 0, head);
  contract_expect(s.n > 40);

  trun_new(&r, 512);
  contract_expect(r.rp != 0);
  contract_expect(feed_all(r.rp, &s) == 0);
  contract_expect(ogg_opus_repage_finish(r.rp) == 0);
  contract_expect(r.col.n > 300);
  saw_middle = 0;
  for (k = 2; k < r.col.n - 1; ++k) {
    tpg pg;
    contract_expect(parse_page(r.col.pages[k], r.col.lens[k], &pg));
    contract_expect(pg.granule == -1);
    if (k > 2) contract_expect((pg.flags & 1) != 0);
    saw_middle = 1;
  }
  contract_expect(saw_middle);
  verify_stream(&r.col, head, tags, 12, audio, 1, 512, 52, 0, 480);
  ogg_opus_repage_free(r.rp);
  contract_expect(r.al.live == 0);
  contract_expect(r.al.peak <= 512 + 80000);
  tcollect_free(&r.col);
  tsrc_free(&s);
  free(tags); free(big);
}

/* minimal stream: a single audio completion page, default allocator */
static void st_short_single_audio_page(void) {
  unsigned char head[19];
  unsigned char *tags = make_bytes(12, "OpusTags");
  unsigned char *pa = make_packet(300, 0x08, 0);
  tapkt audio[1];
  tsrc s;
  tcollect col;
  ogg_opus_repage *rp;
  static const texp want[3] = {
    {1, 19, 0, 2},
    {1, 12, 0, 0},
    {2, 300, 960, 4},
  };
  audio[0].data = pa; audio[0].len = 300; audio[0].dur = 960;
  build_stream(&s, 53, 4096, 1, 0, tags, 12, audio, 1, 960, 0, head);

  memset(&col, 0, sizeof(col));
  rp = ogg_opus_repage_new(512, 0, tcollect_cb, &col); /* malloc/free */
  contract_expect(rp != 0);
  contract_expect(feed_all(rp, &s) == 0);
  contract_expect(col.n == 3); /* everything emitted before finish */
  contract_expect(ogg_opus_repage_finish(rp) == 0);
  contract_expect(col.n == 3);
  check_layout(&col, want, 3);
  verify_stream(&col, head, tags, 12, audio, 1, 512, 53, 0, 960);
  ogg_opus_repage_free(rp);
  tcollect_free(&col);
  tsrc_free(&s);
  free(tags); free(pa);
}

/* two instances interleaved stay independent */
static void st_two_instances_interleaved(void) {
  unsigned char head_a[19], head_b[19];
  unsigned char *tags_a = make_bytes(12, "OpusTags");
  unsigned char *tags_b = make_bytes(20, "OpusTags");
  unsigned char *pa0 = make_packet(300, 0x20, 0);
  unsigned char *pa1 = make_packet(300, 0x20, 0);
  unsigned char *pa2 = make_packet(100, 0x20, 0);
  unsigned char *pb0 = make_packet(600, 0x08, 0);
  unsigned char *pb1 = make_packet(600, 0x08, 0);
  tapkt audio_a[3], audio_b[2];
  tsrc sa, sb;
  trun ra, rb;
  int i, n;
  static const texp want_a[4] = {
    {1, 19, 0, 2},
    {1, 12, 0, 0},
    {2, 300, 480, 0},
    {3, 400, 1440, 4},
  };
  static const texp want_b[4] = {
    {1, 19, 0, 2},
    {1, 20, 0, 0},
    {4, 855, 960, 0},
    {2, 345, 1820, 5},
  };
  audio_a[0].data = pa0; audio_a[0].len = 300; audio_a[0].dur = 480;
  audio_a[1].data = pa1; audio_a[1].len = 300; audio_a[1].dur = 480;
  audio_a[2].data = pa2; audio_a[2].len = 100; audio_a[2].dur = 480;
  audio_b[0].data = pb0; audio_b[0].len = 600; audio_b[0].dur = 960;
  audio_b[1].data = pb1; audio_b[1].len = 600; audio_b[1].dur = 960;
  build_stream(&sa, 100, 4096, 2, 0, tags_a, 12, audio_a, 3, 1440, 0, head_a);
  build_stream(&sb, 200, 4096, 1, 0, tags_b, 20, audio_b, 2, 1920, 100, head_b);

  trun_new(&ra, 512);
  trun_new(&rb, 1000);
  contract_expect(ra.rp != 0 && rb.rp != 0);
  n = sa.n > sb.n ? sa.n : sb.n;
  for (i = 0; i < n; ++i) {
    if (i < sa.n) contract_expect(ogg_opus_repage_pagein(ra.rp, &sa.pages[i].page) == 0);
    if (i < sb.n) contract_expect(ogg_opus_repage_pagein(rb.rp, &sb.pages[i].page) == 0);
  }
  contract_expect(ogg_opus_repage_finish(ra.rp) == 0);
  contract_expect(ogg_opus_repage_finish(rb.rp) == 0);
  check_layout(&ra.col, want_a, 4);
  check_layout(&rb.col, want_b, 4);
  verify_stream(&ra.col, head_a, tags_a, 12, audio_a, 3, 512, 100, 0, 1440);
  verify_stream(&rb.col, head_b, tags_b, 20, audio_b, 2, 1000, 200, 0, 1820);
  ogg_opus_repage_free(ra.rp);
  ogg_opus_repage_free(rb.rp);
  contract_expect(ra.al.live == 0);
  contract_expect(rb.al.live == 0);
  tcollect_free(&ra.col);
  tcollect_free(&rb.col);
  tsrc_free(&sa);
  tsrc_free(&sb);
  free(tags_a); free(tags_b);
  free(pa0); free(pa1); free(pa2); free(pb0); free(pb1);
}

/* callback failure terminates the stream without leaks */
static void st_emit_failure_terminal(void) {
  unsigned char head[19];
  unsigned char *tags = make_bytes(12, "OpusTags");
  unsigned char *pa = make_packet(300, 0x20, 0);
  tapkt audio[1];
  tsrc s;
  trun r;
  audio[0].data = pa; audio[0].len = 300; audio[0].dur = 480;
  build_stream(&s, 55, 4096, 2, 0, tags, 12, audio, 1, 480, 0, head);

  trun_new(&r, 512);
  contract_expect(r.rp != 0);
  r.col.fail_at = 2; /* reject the tags page */
  contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[0].page) == 0);
  contract_expect(r.col.n == 1);
  contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[1].page) == -1);
  contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[2].page) == -1);
  contract_expect(ogg_opus_repage_finish(r.rp) == -1);
  ogg_opus_repage_free(r.rp);
  contract_expect(r.al.live == 0);
  contract_expect(r.al.cur == 0);
  tcollect_free(&r.col);
  tsrc_free(&s);
  free(tags); free(pa);
}

/* allocation failures and bad arguments are handled without leaks */
static void st_alloc_failure_and_args(void) {
  /* constructor argument validation */
  contract_expect(ogg_opus_repage_new(511, 0, tcollect_cb, 0) == 0);
  contract_expect(ogg_opus_repage_new(65308, 0, tcollect_cb, 0) == 0);
  contract_expect(ogg_opus_repage_new(512, 0, 0, 0) == 0);
  {
    ogg_opus_repage_allocator bad;
    bad.user = 0;
    bad.alloc = 0;
    bad.release = talloc_release;
    contract_expect(ogg_opus_repage_new(512, &bad, tcollect_cb, 0) == 0);
    bad.alloc = talloc_alloc;
    bad.release = 0;
    contract_expect(ogg_opus_repage_new(512, &bad, tcollect_cb, 0) == 0);
  }
  ogg_opus_repage_free(0); /* NULL is accepted */

  /* boundary limits are accepted */
  {
    ogg_opus_repage *rp = ogg_opus_repage_new(512, 0, tcollect_cb, 0);
    contract_expect(rp != 0);
    ogg_opus_repage_free(rp);
    rp = ogg_opus_repage_new(65307, 0, tcollect_cb, 0);
    contract_expect(rp != 0);
    ogg_opus_repage_free(rp);
  }

  /* first allocation fails */
  {
    talloc al;
    ogg_opus_repage_allocator a;
    memset(&al, 0, sizeof(al));
    al.fail_after = 1;
    a.user = &al; a.alloc = talloc_alloc; a.release = talloc_release;
    contract_expect(ogg_opus_repage_new(512, &a, tcollect_cb, 0) == 0);
    contract_expect(al.live == 0);
    contract_expect(al.cur == 0);
  }
  /* second allocation fails; the first is rolled back */
  {
    talloc al;
    ogg_opus_repage_allocator a;
    memset(&al, 0, sizeof(al));
    al.fail_after = 2;
    a.user = &al; a.alloc = talloc_alloc; a.release = talloc_release;
    contract_expect(ogg_opus_repage_new(512, &a, tcollect_cb, 0) == 0);
    contract_expect(al.live == 0);
    contract_expect(al.cur == 0);
  }
  /* exactly two allocations ever happen, at construction time */
  {
    unsigned char head[19];
    unsigned char *tags = make_bytes(12, "OpusTags");
    unsigned char *pa = make_packet(300, 0x20, 0);
    tapkt audio[1];
    tsrc s;
    trun r;
    audio[0].data = pa; audio[0].len = 300; audio[0].dur = 480;
    build_stream(&s, 56, 4096, 2, 0, tags, 12, audio, 1, 480, 0, head);
    trun_new(&r, 512);
    r.al.fail_after = 3; /* would fail a third allocation */
    contract_expect(r.rp != 0);
    contract_expect(feed_all(r.rp, &s) == 0);
    contract_expect(ogg_opus_repage_finish(r.rp) == 0);
    contract_expect(r.al.nalloc == 2);
    ogg_opus_repage_free(r.rp);
    contract_expect(r.al.live == 0);
    tcollect_free(&r.col);
    tsrc_free(&s);
    free(tags); free(pa);
  }
}

/* a stream without EOS fails at finish; resources are still freed */
static void st_missing_eos_cleanup(void) {
  unsigned char head[19];
  unsigned char *tags = make_bytes(12, "OpusTags");
  unsigned char *pa = make_packet(300, 0x20, 0);
  tapkt audio[1];
  tsrc s;
  trun r;
  audio[0].data = pa; audio[0].len = 300; audio[0].dur = 480;
  build_stream(&s, 57, 4096, 2, 0, tags, 12, audio, 1, 480, 0, head);
  tpage_clear_flag(&s.pages[s.n - 1], 4); /* strip EOS */

  trun_new(&r, 512);
  contract_expect(r.rp != 0);
  contract_expect(feed_all(r.rp, &s) == 0);
  contract_expect(r.col.n == 2); /* final page still pending */
  contract_expect(ogg_opus_repage_finish(r.rp) == -1);
  contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[0].page) == -1);
  contract_expect(ogg_opus_repage_finish(r.rp) == -1);
  ogg_opus_repage_free(r.rp);
  contract_expect(r.al.live == 0);
  contract_expect(r.al.cur == 0);
  tcollect_free(&r.col);
  tsrc_free(&s);
  free(tags); free(pa);
}

/* calls after EOS/finish are rejected */
static void st_after_eos_and_finish(void) {
  unsigned char head[19];
  unsigned char *tags = make_bytes(12, "OpusTags");
  unsigned char *pa = make_packet(300, 0x20, 0);
  tapkt audio[1];
  audio[0].data = pa; audio[0].len = 300; audio[0].dur = 480;

  /* pagein after a successful finish */
  {
    tsrc s;
    trun r;
    build_stream(&s, 58, 4096, 2, 0, tags, 12, audio, 1, 480, 0, head);
    trun_new(&r, 512);
    contract_expect(r.rp != 0);
    contract_expect(feed_all(r.rp, &s) == 0);
    contract_expect(ogg_opus_repage_finish(r.rp) == 0);
    contract_expect(ogg_opus_repage_finish(r.rp) == 0); /* idempotent */
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[0].page) == -1);
    ogg_opus_repage_free(r.rp);
    contract_expect(r.al.live == 0);
    tcollect_free(&r.col);
    tsrc_free(&s);
  }
  /* pagein after the EOS page */
  {
    tsrc s;
    trun r;
    build_stream(&s, 59, 4096, 2, 0, tags, 12, audio, 1, 480, 0, head);
    trun_new(&r, 512);
    contract_expect(r.rp != 0);
    contract_expect(feed_all(r.rp, &s) == 0);
    contract_expect(ogg_opus_repage_pagein(r.rp, &s.pages[0].page) == -1);
    contract_expect(ogg_opus_repage_finish(r.rp) == -1);
    ogg_opus_repage_free(r.rp);
    contract_expect(r.al.live == 0);
    tcollect_free(&r.col);
    tsrc_free(&s);
  }
  free(tags); free(pa);
}

/* ================================================================ */

void register_contract(void) {
  contract_add("paging", "head_tags_forced_breaks", pg_head_tags_forced_breaks);
  contract_add("paging", "limit_512_exact_layout", pg_limit_512_exact_layout);
  contract_add("paging", "odd_limit_1000", pg_odd_limit_1000);
  contract_add("paging", "mid_packet_split_continued", pg_mid_packet_split_continued);
  contract_add("paging", "lace255_zero_terminator", pg_lace255_zero_terminator);
  contract_add("paging", "multi_complete_and_partial", pg_multi_complete_and_partial);
  contract_add("paging", "input_boundaries_ignored", pg_input_boundaries_ignored);
  contract_add("paging", "eos_on_exact_full_page", pg_eos_on_exact_full_page);
  contract_add("paging", "unsupported_heads_rejected", pg_unsupported_heads_rejected);
  contract_add("paging", "sequence_breaks_rejected", pg_sequence_breaks_rejected);

  contract_add("timing", "toc_duration_grid", tm_toc_duration_grid);
  contract_add("timing", "nonzero_initial_offset", tm_nonzero_initial_offset);
  contract_add("timing", "preskip_spanning_packets", tm_preskip_spanning_packets);
  contract_add("timing", "eos_tail_trim", tm_eos_tail_trim);
  contract_add("timing", "multi_complete_source_page", tm_multi_complete_source_page);
  contract_add("timing", "clock_errors_rejected", tm_clock_errors_rejected);
  contract_add("timing", "invalid_toc_rejected", tm_invalid_toc_rejected);
  contract_add("timing", "unfinished_packet_rejected", tm_unfinished_packet_rejected);

  contract_add("streaming", "huge_tags_streaming", st_huge_tags_streaming);
  contract_add("streaming", "spanning_audio_packet", st_spanning_audio_packet);
  contract_add("streaming", "short_single_audio_page", st_short_single_audio_page);
  contract_add("streaming", "two_instances_interleaved", st_two_instances_interleaved);
  contract_add("streaming", "emit_failure_terminal", st_emit_failure_terminal);
  contract_add("streaming", "alloc_failure_and_args", st_alloc_failure_and_args);
  contract_add("streaming", "missing_eos_cleanup", st_missing_eos_cleanup);
  contract_add("streaming", "after_eos_and_finish", st_after_eos_and_finish);
}
