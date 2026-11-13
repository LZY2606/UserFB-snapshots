/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE Ogg CONTAINER SOURCE CODE.              *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE Ogg CONTAINER SOURCE CODE IS (C) COPYRIGHT 1994-2026         *
 * by the Xiph.Org Foundation http://www.xiph.org/                  *
 *                                                                  *
 ********************************************************************

 function: streaming re-pager for single-track Ogg Opus recordings

 Pages of one complete Ogg Opus stream are consumed one at a time and
 re-emitted under a hard page byte cap.  Packet payloads flow through
 a single output-page assembly buffer; nothing is decoded, nothing is
 collected whole, and no global or file-backed storage is used.

 ********************************************************************/

#include <stdlib.h>
#include <string.h>
#include <ogg/ogg.h>
#include <ogg/opus_repage.h>

#define OPR_MIN_PAGE_BYTES 512
#define OPR_MAX_PAGE_BYTES 65307  /* 27 + 255 + 255*255 */

enum { OPR_PHASE_HEAD, OPR_PHASE_TAGS, OPR_PHASE_AUDIO };

struct ogg_opus_repage {
  ogg_opus_repage_allocator al;
  ogg_opus_repage_emit emit;
  void *emit_user;
  size_t max_page_bytes;

  /* output page under construction; body holds at most
     max_page_bytes bytes (the segment table and header live in the
     fixed-size members below) */
  unsigned char *body;
  unsigned char lacing[255];
  unsigned char header[282];
  long body_len;
  int nsegs;

  ogg_uint32_t serial;
  ogg_int64_t pageno;      /* next output page number */
  ogg_int64_t in_pageno;   /* next expected input page number */

  int phase;
  int error;               /* terminal error latch */
  int eos_seen;
  int finished;
  int out_continued;       /* continued flag for the next emitted page */

  /* assembly of the packet currently crossing pages */
  long packet_pos;         /* bytes of the open packet consumed so far */
  int have_toc;
  int have_toc2;
  unsigned char toc;       /* first packet byte (Opus TOC) */
  unsigned char toc2;      /* second packet byte (frame count for code 3) */

  /* OpusHead fields needed later */
  unsigned pre_skip;
  int channels;

  /* audio clock, in 48 kHz samples per channel */
  int offset_set;
  ogg_int64_t offset;      /* stream time at the first anchored packet */
  ogg_int64_t cum_dur;     /* total duration of completed audio packets */
  int have_prev_completed;
  ogg_int64_t prev_completed_granule;

  /* granule to stamp if the page under construction were emitted now;
     -1 while no packet has completed on it */
  ogg_int64_t page_granule;
};

static void *opr_default_alloc(void *user, size_t bytes) {
  (void)user;
  return malloc(bytes);
}

static void opr_default_release(void *user, void *ptr) {
  (void)user;
  free(ptr);
}

static int opr_fail(ogg_opus_repage *st) {
  st->error = 1;
  return -1;
}

static ogg_uint32_t read_le32(const unsigned char *p) {
  return (ogg_uint32_t)p[0] | ((ogg_uint32_t)p[1] << 8) |
         ((ogg_uint32_t)p[2] << 16) | ((ogg_uint32_t)p[3] << 24);
}

static ogg_int64_t read_le64(const unsigned char *p) {
  ogg_uint64_t v = 0;
  int i;
  for (i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return (ogg_int64_t)v;
}

static void write_le32(unsigned char *p, ogg_uint32_t v) {
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
  p[2] = (unsigned char)((v >> 16) & 0xff);
  p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void write_le64(unsigned char *p, ogg_int64_t v) {
  ogg_uint64_t u = (ogg_uint64_t)v;
  int i;
  for (i = 0; i < 8; ++i) {
    p[i] = (unsigned char)(u & 0xff);
    u >>= 8;
  }
}

/* Duration of one Opus packet in 48 kHz samples per channel, derived
   from the TOC byte (and the frame count byte for packet code 3).
   Returns 0 on success, -1 for an invalid frame count or a total over
   5760 samples.  Padding contributes nothing. */
static int opr_packet_duration(unsigned toc, int have_toc2, unsigned toc2,
                               long *out) {
  static const unsigned short dur_single[4] = {480, 960, 1920, 2880};
  static const unsigned short dur_celt[4] = {120, 240, 480, 960};
  int config = (toc >> 3) & 0x1f;
  int code = toc & 3;
  long frame, count, total;

  if (config <= 11)
    frame = dur_single[config & 3];
  else if (config <= 15)
    frame = (config & 1) ? 960 : 480;
  else
    frame = dur_celt[config & 3];

  if (code == 0) {
    count = 1;
  } else if (code == 1 || code == 2) {
    count = 2;
  } else {
    if (!have_toc2) return -1;
    count = toc2 & 0x3f;
    if (count == 0) return -1;
  }

  total = frame * count;
  if (total > 5760) return -1;
  *out = total;
  return 0;
}

/* Emit the page under construction and reset the assembly state.
   The emitted page borrows st->header/st->body and is only valid
   until the callback returns. */
static int opr_emit_page(ogg_opus_repage *st, int eos,
                         ogg_int64_t eos_granule) {
  unsigned char *h = st->header;
  ogg_int64_t granule = eos ? eos_granule : st->page_granule;
  ogg_page og;
  int flags = 0, r;

  if (st->out_continued) flags |= 1;
  if (st->pageno == 0) flags |= 2;
  if (eos) flags |= 4;

  memcpy(h, "OggS", 4);
  h[4] = 0;
  h[5] = (unsigned char)flags;
  write_le64(h + 6, granule);
  write_le32(h + 14, st->serial);
  write_le32(h + 18, (ogg_uint32_t)st->pageno);
  h[22] = h[23] = h[24] = h[25] = 0;
  h[26] = (unsigned char)st->nsegs;
  if (st->nsegs) memcpy(h + 27, st->lacing, st->nsegs);

  og.header = h;
  og.header_len = 27 + st->nsegs;
  og.body = st->body;
  og.body_len = st->body_len;
  ogg_page_checksum_set(&og);

  r = st->emit(st->emit_user, &og);

  st->nsegs = 0;
  st->body_len = 0;
  st->page_granule = -1;
  st->pageno++;
  st->out_continued = (st->packet_pos > 0);

  if (r) return opr_fail(st);
  return 0;
}

ogg_opus_repage *ogg_opus_repage_new(
    size_t max_page_bytes,
    const ogg_opus_repage_allocator *allocator,
    ogg_opus_repage_emit emit,
    void *emit_user) {
  ogg_opus_repage_allocator al;
  ogg_opus_repage *st;

  if (max_page_bytes < OPR_MIN_PAGE_BYTES ||
      max_page_bytes > OPR_MAX_PAGE_BYTES)
    return NULL;
  if (!emit) return NULL;
  if (allocator) {
    if (!allocator->alloc || !allocator->release) return NULL;
    al = *allocator;
  } else {
    al.user = NULL;
    al.alloc = opr_default_alloc;
    al.release = opr_default_release;
  }

  st = (ogg_opus_repage *)al.alloc(al.user, sizeof(*st));
  if (!st) return NULL;
  memset(st, 0, sizeof(*st));
  st->body = (unsigned char *)al.alloc(al.user, max_page_bytes);
  if (!st->body) {
    al.release(al.user, st);
    return NULL;
  }

  st->al = al;
  st->emit = emit;
  st->emit_user = emit_user;
  st->max_page_bytes = max_page_bytes;
  st->page_granule = -1;
  return st;
}

void ogg_opus_repage_free(ogg_opus_repage *st) {
  ogg_opus_repage_allocator al;
  if (!st) return;
  al = st->al;
  if (st->body) al.release(al.user, st->body);
  al.release(al.user, st);
}

/* First page: the 19 byte OpusHead packet alone on the BOS page. */
static int opr_process_head(ogg_opus_repage *st, int flags,
                            ogg_int64_t granule,
                            const unsigned char *lace, long nsegs,
                            const unsigned char *body, long body_len) {
  if (flags & 4) return opr_fail(st);        /* EOS cannot be first */
  if (granule != 0) return opr_fail(st);
  if (nsegs != 1 || lace[0] != 19 || body_len != 19)
    return opr_fail(st);
  if (memcmp(body, "OpusHead", 8) != 0) return opr_fail(st);
  if (body[8] != 1) return opr_fail(st);     /* version */
  if (body[9] != 1 && body[9] != 2) return opr_fail(st);
  if (body[18] != 0) return opr_fail(st);    /* mapping family */

  st->channels = body[9];
  st->pre_skip = (unsigned)(body[10] | (body[11] << 8));

  /* the header packet becomes the first output page on its own */
  st->lacing[0] = 19;
  memcpy(st->body, body, 19);
  st->body_len = 19;
  st->nsegs = 1;
  st->page_granule = 0;
  if (opr_emit_page(st, 0, 0)) return -1;
  st->phase = OPR_PHASE_TAGS;
  return 0;
}

/* First pass over a non-head input page: validate the header packet
   boundaries or accumulate the audio clock anchors, without copying
   anything.  Mirrors the packet walk performed by the second pass. */
static int opr_analyze_page(ogg_opus_repage *st, int eos,
                            ogg_int64_t granule,
                            const unsigned char *lace, long nsegs,
                            const unsigned char *body) {
  long ppos = st->packet_pos;
  int have_toc = st->have_toc, have_toc2 = st->have_toc2;
  unsigned char toc = st->toc, toc2 = st->toc2;
  long off = 0, i;
  ogg_int64_t dur_sum = 0;
  long last_dur = 0;
  int ncomplete = 0;
  int tags_done = 0;

  for (i = 0; i < nsegs; ++i) {
    int lace_i = lace[i];
    if (ppos == 0) {
      if (lace_i > 0) {
        toc = body[off];
        have_toc = 1;
        if (lace_i > 1) {
          toc2 = body[off + 1];
          have_toc2 = 1;
        } else {
          have_toc2 = 0;
        }
      } else {
        have_toc = 0;
        have_toc2 = 0;
      }
    } else if (ppos == 1 && !have_toc2 && lace_i > 0) {
      toc2 = body[off];
      have_toc2 = 1;
    }
    ppos += lace_i;
    if (lace_i < 255) {
      /* a packet completes on this page */
      if (st->phase == OPR_PHASE_TAGS) {
        if (i != nsegs - 1) return opr_fail(st); /* tags must end the page */
        tags_done = 1;
      } else {
        long d;
        if (!have_toc) return opr_fail(st);    /* empty audio packet */
        if (opr_packet_duration(toc, have_toc2, toc2, &d))
          return opr_fail(st);
        dur_sum += d;
        last_dur = d;
        ncomplete++;
      }
      ppos = 0;
      have_toc = 0;
      have_toc2 = 0;
    }
    off += lace_i;
  }

  /* EOS must land on a packet boundary */
  if (eos && ppos != 0) return opr_fail(st);

  if (st->phase == OPR_PHASE_TAGS) {
    if (tags_done) {
      if (granule != 0) return opr_fail(st);
    } else {
      if (granule != -1) return opr_fail(st);
      if (eos) return opr_fail(st);            /* stream ended inside tags */
    }
    return 0;
  }

  /* audio phase clock anchoring */
  if (ncomplete) {
    if (!st->offset_set) {
      if (eos) {
        if (granule <= dur_sum) {
          ogg_int64_t trim = dur_sum - granule;
          if (trim > 0 && trim >= last_dur) return opr_fail(st);
          st->offset = 0;
        } else {
          st->offset = granule - dur_sum;
        }
      } else {
        st->offset = granule - dur_sum;
      }
      st->offset_set = 1;
    } else {
      ogg_int64_t expected = st->prev_completed_granule + dur_sum;
      if (eos) {
        ogg_int64_t trim;
        if (granule > expected) return opr_fail(st);
        trim = expected - granule;
        if (trim > 0 && trim >= last_dur) return opr_fail(st);
      } else if (granule != expected) {
        return opr_fail(st);
      }
    }
    st->prev_completed_granule = granule;
    st->have_prev_completed = 1;
  } else {
    if (eos) {
      if (st->have_prev_completed && granule != st->prev_completed_granule)
        return opr_fail(st);
    } else if (granule != -1) {
      return opr_fail(st);
    }
  }
  if (eos && granule < (ogg_int64_t)st->pre_skip) return opr_fail(st);
  return 0;
}

/* Second pass: append the page's segments to the output page under
   construction, emitting completed pages as the fixed rule requires. */
static int opr_append_page(ogg_opus_repage *st, int eos,
                           ogg_int64_t granule,
                           const unsigned char *lace, long nsegs,
                           const unsigned char *body) {
  static const char tags_magic[8] = {'O','p','u','s','T','a','g','s'};
  long off = 0, i;
  int eos_emitted = 0;

  for (i = 0; i < nsegs; ++i) {
    int lace_i = lace[i];
    const unsigned char *ptr = body + off;

    /* fixed paging rule: emit the current page when the next lacing
       value would push it past the byte cap or 255 segments */
    if (st->nsegs == 255 ||
        27 + st->nsegs + 1 + st->body_len + lace_i >
            (long)st->max_page_bytes) {
      if (opr_emit_page(st, 0, 0)) return -1;
    }

    if (st->phase == OPR_PHASE_TAGS && st->packet_pos < 8) {
      long k, n = 8 - st->packet_pos;
      if (n > lace_i) n = lace_i;
      for (k = 0; k < n; ++k)
        if (ptr[k] != (unsigned char)tags_magic[st->packet_pos + k])
          return opr_fail(st);
    }

    if (st->packet_pos == 0 && lace_i > 0) {
      st->toc = ptr[0];
      st->have_toc = 1;
      if (lace_i > 1) {
        st->toc2 = ptr[1];
        st->have_toc2 = 1;
      } else {
        st->have_toc2 = 0;
      }
    } else if (st->packet_pos == 1 && !st->have_toc2 && lace_i > 0) {
      st->toc2 = ptr[0];
      st->have_toc2 = 1;
    }

    st->lacing[st->nsegs++] = (unsigned char)lace_i;
    if (lace_i) memcpy(st->body + st->body_len, ptr, lace_i);
    st->body_len += lace_i;
    st->packet_pos += lace_i;

    if (lace_i < 255) {
      /* packet completed */
      if (st->phase == OPR_PHASE_TAGS) {
        if (st->packet_pos < 8) return opr_fail(st);
        st->page_granule = 0;
        st->phase = OPR_PHASE_AUDIO;
        st->packet_pos = 0;
        st->have_toc = 0;
        st->have_toc2 = 0;
        /* forced page break at the end of the comment header */
        if (opr_emit_page(st, eos, granule)) return -1;
        if (eos) eos_emitted = 1;
      } else {
        long d;
        if (opr_packet_duration(st->toc, st->have_toc2, st->toc2, &d))
          return opr_fail(st);
        st->cum_dur += d;
        st->page_granule = st->offset + st->cum_dur;
        st->packet_pos = 0;
        st->have_toc = 0;
        st->have_toc2 = 0;
      }
    }
    off += lace_i;
  }

  if (eos) {
    st->eos_seen = 1;
    if (!eos_emitted) {
      /* the page under construction is the last one; it carries EOS
         rather than being followed by an empty page */
      if (opr_emit_page(st, 1, granule)) return -1;
    }
  }
  return 0;
}

int ogg_opus_repage_pagein(ogg_opus_repage *st, const ogg_page *og) {
  const unsigned char *h, *lace, *body;
  long nsegs, body_len, i;
  int flags, eos;
  ogg_int64_t granule;

  if (!st || !og || !og->header) return -1;
  if (st->error) return -1;
  if (st->finished || st->eos_seen) return opr_fail(st);

  h = og->header;
  if (og->header_len < 27) return opr_fail(st);
  if (memcmp(h, "OggS", 4) != 0 || h[4] != 0) return opr_fail(st);
  nsegs = h[26];
  if (og->header_len != 27 + nsegs) return opr_fail(st);
  lace = h + 27;
  body_len = 0;
  for (i = 0; i < nsegs; ++i) body_len += lace[i];
  if (og->body_len != body_len) return opr_fail(st);
  body = og->body;
  if (nsegs && !body) return opr_fail(st);

  flags = h[5];
  granule = read_le64(h + 6);
  eos = flags & 4;

  if (st->in_pageno == 0) {
    st->serial = read_le32(h + 14);
    if (!(flags & 2)) return opr_fail(st);   /* first page must be BOS */
  } else {
    if (read_le32(h + 14) != st->serial) return opr_fail(st);
    if (flags & 2) return opr_fail(st);      /* BOS appears only once */
  }
  if (read_le32(h + 18) != (ogg_uint32_t)st->in_pageno)
    return opr_fail(st);
  if ((flags & 1) != (st->packet_pos > 0 ? 1 : 0))
    return opr_fail(st);                     /* continuation mismatch */
  st->in_pageno++;

  if (st->phase == OPR_PHASE_HEAD)
    return opr_process_head(st, flags, granule, lace, nsegs, body, body_len);

  if (opr_analyze_page(st, eos, granule, lace, nsegs, body)) return -1;
  return opr_append_page(st, eos, granule, lace, nsegs, body);
}

int ogg_opus_repage_finish(ogg_opus_repage *st) {
  if (!st) return -1;
  if (st->error) return -1;
  if (st->finished) return 0;
  if (!st->eos_seen) return opr_fail(st);    /* missing EOS */
  st->finished = 1;
  return 0;
}
