/*
 * Regression tests for RFC 5219 4.1 ADU packetization and direct ADU
 * decoding.  All MPEG fixtures are synthesized here in C; no external
 * audio files or third-party libraries are used.
 */

#include "mad.h"
#include "task_support/check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* bit writer (mirrors the style of task_support/baseline.c)          */
/* ------------------------------------------------------------------ */

static void bits_put(unsigned char *p, unsigned *at,
		     unsigned n, unsigned value)
{
  while (n--) {
    unsigned char mask = (unsigned char) (1u << (7 - (*at % 8)));
    if ((value >> n) & 1u)
      p[*at / 8] |= mask;
    else
      p[*at / 8] &= (unsigned char) ~mask;
    ++*at;
  }
}

/* ------------------------------------------------------------------ */
/* stream specifications and independently built fixtures             */
/* ------------------------------------------------------------------ */

#define MAX_FRAMES 8

struct spec {
  int lsf;				/* 0 = MPEG-1, 1 = MPEG-2 LSF */
  int crc;
  int nch;				/* 1 or 2 */
  unsigned sr_index;
  unsigned nframes;
  unsigned char bitrate_idx[MAX_FRAMES];
  unsigned char padding[MAX_FRAMES];
  unsigned short back[MAX_FRAMES];	/* main_data_begin per frame */
};

static unsigned long const br_iii[2][15] = {
  { 0,  32000,  40000,  48000,  56000,  64000,  80000,  96000,
     112000, 128000, 160000, 192000, 224000, 256000, 320000 },
  { 0,   8000,  16000,  24000,  32000,  40000,  48000,  56000,
      64000,  80000,  96000, 112000, 128000, 144000, 160000 }
};

static unsigned int const sr_iii[2][3] = {
  { 44100, 48000, 32000 },
  { 22050, 24000, 16000 }
};

struct built {
  struct spec spec;
  unsigned char *mp3;			/* physical frames + guard */
  unsigned long mp3_len;		/* without guard */
  unsigned long frame_off[MAX_FRAMES];
  unsigned long frame_len[MAX_FRAMES];
  unsigned long slot_off[MAX_FRAMES];	/* C_i on the slot axis */
  unsigned long slot_len[MAX_FRAMES];
  unsigned long axis_len;
  unsigned long marker_off[MAX_FRAMES];/* explicit ancillary markers */
  unsigned char *axis;			/* independently expected axis */
};

static unsigned long frame_length(struct spec const *s, unsigned f)
{
  unsigned long slots = s->lsf ? 72 : 144;
  return slots * br_iii[s->lsf][s->bitrate_idx[f]] /
    sr_iii[s->lsf][s->sr_index] + s->padding[f];
}

static unsigned sideinfo_len(int lsf, int nch)
{
  return lsf ? (nch == 1 ? 9 : 17) : (nch == 1 ? 17 : 32);
}

/*
 * Build a physical stream and, independently, the expected slot axis.
 * The audio payload is non-zero Huffman data (three bits per granule and
 * channel, as in the baseline fixture); each slot tail carries an
 * explicit ancillary marker pattern.
 */
static int build_fixture(struct spec const *s, struct built *b)
{
  unsigned f;
  unsigned long total = 0, axis = 0;
  unsigned char *mp3, *tape;
  unsigned si_len, prefix;

  b->spec = *s;

  for (f = 0; f < s->nframes; ++f)
    total += frame_length(s, f);

  mp3 = calloc(total + MAD_BUFFER_GUARD, 1);
  tape = calloc(total, 1);		/* never smaller than the axis */
  if (mp3 == 0 || tape == 0) {
    free(mp3);
    free(tape);
    return -1;
  }

  si_len = sideinfo_len(s->lsf, s->nch);
  prefix = 4 + (s->crc ? 2 : 0) + si_len;

  for (f = 0; f < s->nframes; ++f) {
    unsigned long n = frame_length(s, f);
    unsigned long slot = n - prefix;
    unsigned j;

    b->frame_off[f] = (f == 0) ? 0 : b->frame_off[f - 1] + b->frame_len[f - 1];
    b->frame_len[f] = n;
    b->slot_off[f] = axis;
    b->slot_len[f] = slot;

    /* deterministic slot filling, then the ancillary signature */
    for (j = 0; j < slot; ++j)
      tape[axis + j] = (unsigned char) ((axis + j) * 37u + 19u);
    if (slot >= 6) {
      static unsigned char const sig[6] = { 0xA5, 0x5A, 0xA5, 0x5A, 0xA5, 0x5A };
      b->marker_off[f] = axis + slot - 6;
      memcpy(tape + b->marker_off[f], sig, 6);
    }
    else
      b->marker_off[f] = (unsigned long) -1;

    axis += slot;
  }
  b->axis_len = axis;
  b->axis = tape;

  /* non-zero Huffman audio on the axis at each frame's body origin */
  for (f = 0; f < s->nframes; ++f) {
    unsigned gr, ch, ngr = s->lsf ? 1 : 2;
    unsigned long start = b->slot_off[f] - s->back[f];
    unsigned at = 0;

    for (gr = 0; gr < ngr; ++gr)
      for (ch = 0; ch < (unsigned) s->nch; ++ch) {
	bits_put(tape + start, &at, 2, 1);
	bits_put(tape + start, &at, 1, (f + gr + ch) & 1u);
      }
  }

  /* physical frames */
  for (f = 0; f < s->nframes; ++f) {
    unsigned char *p = mp3 + b->frame_off[f];
    unsigned at = 0;

    p[0] = 0xff;
    p[1] = (unsigned char) (0xe0 | 0x10 | (s->lsf ? 0x00 : 0x08) |
			    (0x01 << 1) | (s->crc ? 0x00 : 0x01));
    p[2] = (unsigned char) ((s->bitrate_idx[f] << 4) |
			    (s->sr_index << 2) |
			    (s->padding[f] << 1));
    p[3] = (s->nch == 1) ? 0xc0 : 0x00;

    /* side information */
    bits_put(p + 4 + (s->crc ? 2 : 0), &at, s->lsf ? 8 : 9, s->back[f]);
    bits_put(p + 4 + (s->crc ? 2 : 0), &at,
	     s->lsf ? (s->nch == 1 ? 1 : 2) : (s->nch == 1 ? 5 : 3), 0);
    if (!s->lsf)
      bits_put(p + 4 + (s->crc ? 2 : 0), &at, 4 * s->nch, 0);

    {
      unsigned gr, ch, ngr = s->lsf ? 1 : 2;
      for (gr = 0; gr < ngr; ++gr)
	for (ch = 0; ch < (unsigned) s->nch; ++ch) {
	  bits_put(p + 4 + (s->crc ? 2 : 0), &at, 12, 3);  /* part2_3 */
	  bits_put(p + 4 + (s->crc ? 2 : 0), &at, 9, 1);   /* big_values */
	  bits_put(p + 4 + (s->crc ? 2 : 0), &at, 8,
		   ch ? 210 : 216);			     /* global_gain */
	  bits_put(p + 4 + (s->crc ? 2 : 0), &at,
		   s->lsf ? 9 : 4, 0);			     /* scf compress */
	  bits_put(p + 4 + (s->crc ? 2 : 0), &at, 1, 0);   /* no win switch */
	  bits_put(p + 4 + (s->crc ? 2 : 0), &at, 5, 1);   /* table_select 0 */
	  bits_put(p + 4 + (s->crc ? 2 : 0), &at, 5, 0);
	  bits_put(p + 4 + (s->crc ? 2 : 0), &at, 5, 0);
	  bits_put(p + 4 + (s->crc ? 2 : 0), &at, 4, 0);   /* region0 */
	  bits_put(p + 4 + (s->crc ? 2 : 0), &at, 3, 0);   /* region1 */
	  /* LSF has no preflag: scalefac_scale + count1table_select only */
	  bits_put(p + 4 + (s->crc ? 2 : 0), &at,
		   s->lsf ? 2 : 3, 0);			     /* flags */
	}
    }

    /* CRC over header bytes 2-3 and the complete side information */
    if (s->crc) {
      struct mad_bitptr bp;
      unsigned short crc;
      mad_bit_init(&bp, p + 2);
      crc = mad_bit_crc(bp, 16, 0xffff);
      mad_bit_init(&bp, p + 6);
      crc = mad_bit_crc(bp, si_len * 8, crc);
      p[4] = (unsigned char) (crc >> 8);
      p[5] = (unsigned char) (crc & 0xff);
    }

    memcpy(p + prefix, tape + b->slot_off[f], b->slot_len[f]);
  }

  b->mp3 = mp3;
  b->mp3_len = total;
  return 0;
}

static void built_free(struct built *b)
{
  free(b->mp3);
  free(b->axis);
  b->mp3 = 0;
  b->axis = 0;
}

/* ------------------------------------------------------------------ */
/* ADU collector                                                      */
/* ------------------------------------------------------------------ */

struct collected {
  unsigned char **pkts;
  unsigned long *lens;
  unsigned count, cap;
  unsigned fail_after;		/* 0 = never fail */
  unsigned calls;
};

static void collector_init(struct collected *c, unsigned fail_after)
{
  memset(c, 0, sizeof(*c));
  c->fail_after = fail_after;
}

static int collector_emit(void *user, unsigned char const *adu,
			  unsigned long length)
{
  struct collected *c = user;
  unsigned char *copy;

  ++c->calls;
  if (c->fail_after && c->calls > c->fail_after)
    return -1;

  if (c->count == c->cap) {
    unsigned ncap = c->cap ? c->cap * 2 : 8;
    unsigned char **np = realloc(c->pkts, ncap * sizeof(*np));
    unsigned long *nl = realloc(c->lens, ncap * sizeof(*nl));
    if (np == 0 || nl == 0) {
      free(np);
      free(nl);
      return -1;
    }
    c->pkts = np;
    c->lens = nl;
    c->cap = ncap;
  }

  copy = malloc(length ? length : 1);
  if (copy == 0)
    return -1;
  memcpy(copy, adu, length);
  c->pkts[c->count] = copy;
  c->lens[c->count] = length;
  ++c->count;
  return 0;
}

static void collector_free(struct collected *c)
{
  unsigned i;
  for (i = 0; i < c->count; ++i)
    free(c->pkts[i]);
  free(c->pkts);
  free(c->lens);
  memset(c, 0, sizeof(*c));
}

static int collect_all(struct built const *b, struct collected *out,
		       int bytewise)
{
  struct mad_adu_packer *packer;
  unsigned long i;
  int rc = 0;

  collector_init(out, 0);
  packer = mad_adu_packer_new(collector_emit, out);
  TASK_CHECK(packer != 0);

  if (bytewise) {
    for (i = 0; i < b->mp3_len; ++i) {
      if (mad_adu_packer_feed(packer, b->mp3 + i, 1) == -1) {
	rc = -1;
		goto done;
      }
    }
  }
  else {
    /* deliberately awkward fragment sizes plus zero-length feeds */
    unsigned long pos = 0;
    static unsigned char const chunks[] = { 1, 7, 3, 16, 5, 64, 2, 11 };
    unsigned k = 0;
    while (pos < b->mp3_len) {
      unsigned long n = chunks[k++ % (sizeof chunks)];
      if (n > b->mp3_len - pos)
	n = b->mp3_len - pos;
      TASK_CHECK(mad_adu_packer_feed(packer, 0, 0) == 0);
      if (mad_adu_packer_feed(packer, b->mp3 + pos, n) == -1) {
	rc = -1;
	goto done;
      }
      pos += n;
    }
    TASK_CHECK(mad_adu_packer_feed(packer, 0, 0) == 0);
  }

  if (mad_adu_packer_finish(packer) == -1)
    rc = -1;

done:
  mad_adu_packer_free(packer);
  return rc;
}

/* ------------------------------------------------------------------ */
/* independent expected interval for frame f                          */
/* ------------------------------------------------------------------ */

static void body_interval(struct built const *b, unsigned f,
			  unsigned long *begin, unsigned long *end)
{
  *begin = b->slot_off[f] - b->spec.back[f];
  if (f + 1 < b->spec.nframes)
    *end = b->slot_off[f + 1] - b->spec.back[f + 1];
  else
    *end = b->axis_len;
}

static unsigned prefix_len(struct built const *b)
{
  return 4 + (b->spec.crc ? 2 : 0) +
    sideinfo_len(b->spec.lsf, b->spec.nch);
}

/* ------------------------------------------------------------------ */
/* packetize group                                                    */
/* ------------------------------------------------------------------ */

static int check_packets(struct built const *b, struct collected const *c)
{
  unsigned f, pre;
  unsigned long prev_begin = 0;
  int saw_cross = 0, saw_long = 0;

  TASK_CHECK(c->count == b->spec.nframes);
  pre = prefix_len(b);

  for (f = 0; f < b->spec.nframes; ++f) {
    unsigned long begin, end, body;
    unsigned char const *pkt = c->pkts[f];

    body_interval(b, f, &begin, &end);
    body = end - begin;

    /* ordering and one-to-one correspondence with physical frames */
    TASK_CHECK(c->lens[f] == pre + body);
    TASK_CHECK(memcmp(pkt, b->mp3 + b->frame_off[f], pre) == 0);
    TASK_CHECK(memcmp(pkt + pre, b->axis + begin, body) == 0);
    TASK_CHECK(pkt[0] == 0xff);		/* sync word preserved */

    if (f == 0)
      TASK_CHECK(b->spec.back[f] == 0 && begin == 0);
    TASK_CHECK(begin >= prev_begin);	/* origins never move backwards */
    prev_begin = begin;

    /* part2_3_length (3 bits per granule/channel) must fit the body */
    {
      unsigned ngr = b->spec.lsf ? 1 : 2;
      unsigned p23 = ngr * b->spec.nch * 3;
      TASK_CHECK(p23 <= body * 8);
    }

    if (b->spec.back[f] > 0)
      saw_cross = 1;			/* body reaches a previous slot */
    if (body > b->slot_len[f])
      saw_long = 1;			/* body longer than its own slot */

    /* explicit ancillary marker, located independently */
    if (b->marker_off[f] != (unsigned long) -1) {
      unsigned long m = b->marker_off[f];
      if (m >= begin && m + 6 <= end) {
	TASK_CHECK(memcmp(pkt + pre + (m - begin),
			  b->axis + m, 6) == 0);
	TASK_CHECK(pkt[pre + (m - begin)] == 0xA5);
      }
    }
  }

  TASK_CHECK(saw_cross);
  TASK_CHECK(saw_long);
  return 0;
}

static struct spec const spec_a = {
  0, 1, 2, 1, 6,
  { 9, 8, 10, 9, 8, 10 }, { 0, 1, 0, 1, 0, 1 },
  { 0, 120, 260, 80, 240, 160 }
};

static struct spec const spec_b = {
  1, 1, 1, 0, 5,
  { 8, 8, 8, 8, 8 }, { 1, 0, 1, 0, 1 },
  { 0, 60, 120, 40, 90 }
};

static struct spec const spec_c = {
  1, 0, 2, 1, 4,
  { 8, 10, 8, 12 }, { 0, 0, 0, 0 },
  { 0, 100, 30, 150 }
};

static struct spec const spec_d = {
  0, 0, 1, 2, 4,
  { 8, 9, 10, 9 }, { 0, 0, 1, 0 },
  { 0, 80, 150, 60 }
};

static int case_pack_mpeg1_stereo_crc_vbr(void)
{
  struct built b;
  struct collected bulk, onebyte;
  TASK_CHECK(build_fixture(&spec_a, &b) == 0);
  TASK_CHECK(collect_all(&b, &bulk, 0) == 0);
  TASK_CHECK(check_packets(&b, &bulk) == 0);
  TASK_CHECK(collect_all(&b, &onebyte, 1) == 0);
  TASK_CHECK(onebyte.count == bulk.count);
  {
    unsigned i;
    for (i = 0; i < bulk.count; ++i) {
      TASK_CHECK(onebyte.lens[i] == bulk.lens[i]);
      TASK_CHECK(memcmp(onebyte.pkts[i], bulk.pkts[i],
			bulk.lens[i]) == 0);
    }
  }
  collector_free(&bulk);
  collector_free(&onebyte);
  built_free(&b);
  return 0;
}

static int case_pack_mpeg2_mono_crc_padding(void)
{
  struct built b;
  struct collected c;
  TASK_CHECK(build_fixture(&spec_b, &b) == 0);
  TASK_CHECK(collect_all(&b, &c, 0) == 0);
  TASK_CHECK(check_packets(&b, &c) == 0);
  collector_free(&c);
  built_free(&b);
  return 0;
}

static int case_pack_mpeg2_stereo_vbr(void)
{
  struct built b;
  struct collected c;
  TASK_CHECK(build_fixture(&spec_c, &b) == 0);
  TASK_CHECK(collect_all(&b, &c, 1) == 0);
  TASK_CHECK(check_packets(&b, &c) == 0);
  collector_free(&c);
  built_free(&b);
  return 0;
}

static int case_pack_mpeg1_mono(void)
{
  struct built b;
  struct collected c;
  TASK_CHECK(build_fixture(&spec_d, &b) == 0);
  TASK_CHECK(collect_all(&b, &c, 0) == 0);
  TASK_CHECK(check_packets(&b, &c) == 0);
  collector_free(&c);
  built_free(&b);
  return 0;
}

static int case_pack_empty_and_idempotent_finish(void)
{
  struct collected c;
  struct mad_adu_packer *p;

  collector_init(&c, 0);
  p = mad_adu_packer_new(collector_emit, &c);
  TASK_CHECK(p != 0);
  TASK_CHECK(mad_adu_packer_finish(p) == 0);
  TASK_CHECK(c.calls == 0 && c.count == 0);
  TASK_CHECK(mad_adu_packer_finish(p) == 0);
  TASK_CHECK(c.calls == 0);
  TASK_CHECK(mad_adu_packer_feed(p, (unsigned char const *) "x", 1) == -1);
  mad_adu_packer_free(p);
  collector_free(&c);
  return 0;
}

static int case_pack_truncated(void)
{
  struct built b;
  struct mad_adu_packer *p;
  struct collected c;
  unsigned long cut;

  TASK_CHECK(build_fixture(&spec_a, &b) == 0);
  collector_init(&c, 0);
  p = mad_adu_packer_new(collector_emit, &c);
  TASK_CHECK(p != 0);

  /* a few complete frames, then half of the next one */
  cut = b.frame_off[3] + b.frame_len[3] / 2;
  TASK_CHECK(mad_adu_packer_feed(p, b.mp3, cut) == 0);
  TASK_CHECK(c.count == 2);		/* frame 2's packet waits for frame 3 */
  TASK_CHECK(mad_adu_packer_finish(p) == -1);
  TASK_CHECK(c.count == 2);		/* nothing invented or withdrawn */
  TASK_CHECK(mad_adu_packer_finish(p) == -1);
  TASK_CHECK(mad_adu_packer_feed(p, b.mp3, 1) == -1);
  mad_adu_packer_free(p);
  collector_free(&c);

  /* three stray bytes can never be a complete frame */
  collector_init(&c, 0);
  p = mad_adu_packer_new(collector_emit, &c);
  TASK_CHECK(p != 0);
  TASK_CHECK(mad_adu_packer_feed(p, b.mp3, 3) == 0);
  TASK_CHECK(mad_adu_packer_finish(p) == -1);
  mad_adu_packer_free(p);
  collector_free(&c);
  built_free(&b);
  return 0;
}

static int case_pack_callback_failure(void)
{
  struct built b;
  struct collected c;
  struct mad_adu_packer *p;

  TASK_CHECK(build_fixture(&spec_a, &b) == 0);
  collector_init(&c, 2);			/* fail on the third ADU */
  p = mad_adu_packer_new(collector_emit, &c);
  TASK_CHECK(p != 0);
  TASK_CHECK(mad_adu_packer_feed(p, b.mp3, b.mp3_len) == -1);
  TASK_CHECK(c.calls == 3 && c.count == 2);
  TASK_CHECK(mad_adu_packer_feed(p, b.mp3, 1) == -1);
  TASK_CHECK(mad_adu_packer_finish(p) == -1);
  TASK_CHECK(c.calls == 3);			/* permanently silent */
  mad_adu_packer_free(p);
  collector_free(&c);
  built_free(&b);
  return 0;
}

static int case_pack_malformed(void)
{
  struct mad_adu_packer *p;
  struct collected c;
  unsigned char junk[8];
  struct built b;
  struct mad_adu_packer *q;
  struct collected d;

  collector_init(&c, 0);
  p = mad_adu_packer_new(collector_emit, &c);
  TASK_CHECK(p != 0);

  /* no sync word */
  memset(junk, 0x00, sizeof junk);
  TASK_CHECK(mad_adu_packer_feed(p, junk, sizeof junk) == -1);
  mad_adu_packer_free(p);
  collector_free(&c);

  /* MPEG-2.5 */
  collector_init(&c, 0);
  p = mad_adu_packer_new(collector_emit, &c);
  TASK_CHECK(p != 0);
  junk[0] = 0xff; junk[1] = 0xe3; junk[2] = 0x90; junk[3] = 0x00;
  TASK_CHECK(mad_adu_packer_feed(p, junk, 4) == -1);
  mad_adu_packer_free(p);
  collector_free(&c);

  /* free format (bitrate index 0) */
  collector_init(&c, 0);
  p = mad_adu_packer_new(collector_emit, &c);
  TASK_CHECK(p != 0);
  junk[0] = 0xff; junk[1] = 0xfb; junk[2] = 0x00; junk[3] = 0x00;
  TASK_CHECK(mad_adu_packer_feed(p, junk, 4) == -1);
  mad_adu_packer_free(p);
  collector_free(&c);

  /* format change mid stream: MPEG-1 stereo head, then an LSF header */
  TASK_CHECK(build_fixture(&spec_a, &b) == 0);
  collector_init(&d, 0);
  q = mad_adu_packer_new(collector_emit, &d);
  TASK_CHECK(q != 0);
  TASK_CHECK(mad_adu_packer_feed(q, b.mp3, b.frame_len[0]) == 0);
  TASK_CHECK(d.count == 0);			/* first packet waits */
  {
    unsigned char *flip = b.mp3 + b.frame_off[1];
    flip[1] = (unsigned char) (flip[1] & ~0x08);	/* clear ID bit */
    TASK_CHECK(mad_adu_packer_feed(q, flip, b.frame_len[1]) == -1);
  }
  mad_adu_packer_free(q);
  collector_free(&d);
  built_free(&b);
  return 0;
}

/* ------------------------------------------------------------------ */
/* decode group                                                       */
/* ------------------------------------------------------------------ */

struct pcm_tape {
  mad_fixed_t *samples;
  unsigned long count;		/* per-channel samples */
  unsigned channels;
};

static int decode_physical(struct built const *b, struct pcm_tape *out)
{
  struct mad_stream stream;
  struct mad_frame frame;
  struct mad_synth synth;
  unsigned frames = 0, i, ch;
  unsigned long cap = 0;

  memset(out, 0, sizeof(*out));
  mad_stream_init(&stream);
  mad_frame_init(&frame);
  mad_synth_init(&synth);
  mad_stream_buffer(&stream, b->mp3, b->mp3_len + MAD_BUFFER_GUARD);

  while (frames < b->spec.nframes) {
    if (mad_frame_decode(&frame, &stream) == -1) {
      fprintf(stderr, "physical decode failed frame %u: %s\n", frames,
	      mad_stream_errorstr(&stream));
      goto fail;
    }
    mad_synth_frame(&synth, &frame);
    TASK_CHECK(synth.pcm.channels == (unsigned short) b->spec.nch);
    TASK_CHECK(synth.pcm.length == (b->spec.lsf ? 576 : 1152));

    if (out->count + 1152 > cap) {
      unsigned long ncap = cap ? cap * 2 : 1152 * 2;
      mad_fixed_t *n = realloc(out->samples,
			       ncap * 2 * sizeof(mad_fixed_t));
      TASK_CHECK(n != 0);
      out->samples = n;
      cap = ncap;
    }
    for (i = 0; i < synth.pcm.length; ++i)
      for (ch = 0; ch < synth.pcm.channels; ++ch)
	out->samples[(out->count + i) * 2 + ch] =
	  synth.pcm.samples[ch][i];
    out->count += synth.pcm.length;
    out->channels = synth.pcm.channels;
    ++frames;
  }

  mad_frame_finish(&frame);
  mad_stream_finish(&stream);
  return 0;

fail:
  mad_frame_finish(&frame);
  mad_stream_finish(&stream);
  free(out->samples);
  memset(out, 0, sizeof(*out));
  return -1;
}

static int decode_adu_path(struct collected const *c,
			   unsigned expected_ch, int lsf,
			   struct pcm_tape *out)
{
  struct mad_stream stream;
  struct mad_frame frame;
  struct mad_synth synth;
  unsigned f, i, ch;
  unsigned long cap = 0;

  memset(out, 0, sizeof(*out));
  mad_stream_init(&stream);
  mad_frame_init(&frame);
  mad_synth_init(&synth);

  for (f = 0; f < c->count; ++f) {
    unsigned char *before;
    int err;

    before = malloc(c->lens[f]);
    TASK_CHECK(before != 0);
    memcpy(before, c->pkts[f], c->lens[f]);

    err = mad_frame_decode_adu(&frame, &stream, c->pkts[f], c->lens[f]);
    if (err == -1) {
      fprintf(stderr, "adu decode failed packet %u: %s\n", f,
	      mad_stream_errorstr(&stream));
      free(before);
      goto fail;
    }
    TASK_CHECK(stream.error == MAD_ERROR_NONE);
    TASK_CHECK(memcmp(before, c->pkts[f], c->lens[f]) == 0);
    free(before);

    mad_synth_frame(&synth, &frame);
    TASK_CHECK(synth.pcm.channels == expected_ch);
    TASK_CHECK(synth.pcm.length == (lsf ? 576u : 1152u));

    if (out->count + 1152 > cap) {
      unsigned long ncap = cap ? cap * 2 : 1152 * 2;
      mad_fixed_t *n = realloc(out->samples,
			       ncap * 2 * sizeof(mad_fixed_t));
      TASK_CHECK(n != 0);
      out->samples = n;
      cap = ncap;
    }
    for (i = 0; i < synth.pcm.length; ++i)
      for (ch = 0; ch < synth.pcm.channels; ++ch)
	out->samples[(out->count + i) * 2 + ch] =
	  synth.pcm.samples[ch][i];
    out->count += synth.pcm.length;
    out->channels = synth.pcm.channels;
  }

  mad_frame_finish(&frame);
  mad_stream_finish(&stream);
  return 0;

fail:
  mad_frame_finish(&frame);
  mad_stream_finish(&stream);
  free(out->samples);
  memset(out, 0, sizeof(*out));
  return -1;
}

static void pcm_free(struct pcm_tape *p)
{
  free(p->samples);
  memset(p, 0, sizeof(*p));
}

static int case_decode_matches_physical(struct spec const *s)
{
  struct built b;
  struct collected c;
  struct pcm_tape ref, got;
  unsigned long i;
  unsigned ch;
  unsigned long nonzero = 0;

  TASK_CHECK(build_fixture(s, &b) == 0);
  TASK_CHECK(collect_all(&b, &c, 0) == 0);
  TASK_CHECK(decode_physical(&b, &ref) == 0);
  TASK_CHECK(decode_adu_path(&c, s->nch, s->lsf, &got) == 0);
  TASK_CHECK(got.count == ref.count);
  TASK_CHECK(got.channels == ref.channels);

  /* compare only the populated channel slots (mono leaves ch 1 unused) */
  for (i = 0; i < got.count; ++i) {
    for (ch = 0; ch < got.channels; ++ch) {
      TASK_CHECK(got.samples[i * 2 + ch] == ref.samples[i * 2 + ch]);
      if (got.samples[i * 2 + ch])
	++nonzero;
    }
  }
  TASK_CHECK(nonzero > 0);		/* real audio, not silence */

  pcm_free(&ref);
  pcm_free(&got);
  collector_free(&c);
  built_free(&b);
  return 0;
}

static int case_decode_mpeg1_stereo(void)
{
  return case_decode_matches_physical(&spec_a);
}

static int case_decode_mpeg2_mono(void)
{
  return case_decode_matches_physical(&spec_b);
}

static int case_decode_mpeg2_stereo(void)
{
  return case_decode_matches_physical(&spec_c);
}

static int case_decode_mpeg1_mono(void)
{
  return case_decode_matches_physical(&spec_d);
}

static int case_decode_isolated_reservoir_packet(void)
{
  struct built b;
  struct collected c;
  struct mad_stream stream;
  struct mad_frame frame;
  unsigned f;

  TASK_CHECK(build_fixture(&spec_a, &b) == 0);
  TASK_CHECK(collect_all(&b, &c, 0) == 0);

  /* find a packet carrying a non-zero main_data_begin */
  for (f = 1; f < c.count; ++f) {
    if (b.spec.back[f] != 0)
      break;
  }
  TASK_CHECK(f < c.count);

  mad_stream_init(&stream);
  mad_frame_init(&frame);
  TASK_CHECK(mad_frame_decode_adu(&frame, &stream,
				  c.pkts[f], c.lens[f]) == 0);
  TASK_CHECK(stream.error == MAD_ERROR_NONE);
  mad_frame_finish(&frame);
  mad_stream_finish(&stream);

  collector_free(&c);
  built_free(&b);
  return 0;
}

static int case_decode_bad_lengths(void)
{
  struct built b;
  struct collected c;
  struct mad_stream stream;
  struct mad_frame frame;
  unsigned char tiny[4] = { 0xff, 0xfb, 0x90, 0x00 };
  unsigned char *evil;
  unsigned pre;

  TASK_CHECK(build_fixture(&spec_a, &b) == 0);
  TASK_CHECK(collect_all(&b, &c, 0) == 0);
  pre = prefix_len(&b);

  mad_stream_init(&stream);
  mad_frame_init(&frame);

  TASK_CHECK(mad_frame_decode_adu(&frame, &stream, 0, 0) == -1);
  TASK_CHECK(stream.error == MAD_ERROR_BUFLEN);
  TASK_CHECK(mad_frame_decode_adu(&frame, &stream, tiny, 2) == -1);
  TASK_CHECK(stream.error == MAD_ERROR_BUFLEN);

  /* prefix truncated: not enough side information for a real frame */
  TASK_CHECK(mad_frame_decode_adu(&frame, &stream, c.pkts[0],
				  pre - 1) == -1);
  TASK_CHECK(stream.error == MAD_ERROR_BADFRAMELEN);

  /* structurally intact packet whose part2_3_length exceeds its body */
  evil = malloc(c.lens[0]);
  TASK_CHECK(evil != 0);
  memcpy(evil, c.pkts[0], c.lens[0]);
  /* first part2_3_length field sits at side-info bit offset 9+3+8=20
     (MPEG-1 stereo: main_data_begin 9, private 3, scfsi 8) */
  evil[4 + 2] = (unsigned char) ((evil[4 + 2] & 0x0f) | 0xf0);
  evil[4 + 3] = (unsigned char) ((evil[4 + 3] & 0xf0) | 0x0f);
  TASK_CHECK(mad_frame_decode_adu(&frame, &stream, evil,
				  c.lens[0]) == -1);
  TASK_CHECK(stream.error != MAD_ERROR_NONE);
  free(evil);

  mad_frame_finish(&frame);
  mad_stream_finish(&stream);
  collector_free(&c);
  built_free(&b);
  return 0;
}

static int case_decode_crc_and_ignore(void)
{
  struct built b;
  struct collected c;
  struct mad_stream s1, s2;
  struct mad_frame f1, f2;
  unsigned char *bad;

  TASK_CHECK(build_fixture(&spec_b, &b) == 0);
  TASK_CHECK(collect_all(&b, &c, 0) == 0);

  /* corrupt only the CRC word: side information and main data stay
     decodable, so IGNORECRC must still succeed */
  bad = malloc(c.lens[1]);
  TASK_CHECK(bad != 0);
  memcpy(bad, c.pkts[1], c.lens[1]);
  bad[4] ^= 0xff;
  bad[5] ^= 0xff;

  mad_stream_init(&s1);
  mad_frame_init(&f1);
  TASK_CHECK(mad_frame_decode_adu(&f1, &s1, bad, c.lens[1]) == -1);
  TASK_CHECK(s1.error == MAD_ERROR_BADCRC);
  mad_frame_finish(&f1);
  mad_stream_finish(&s1);

  mad_stream_init(&s2);
  mad_stream_options(&s2, MAD_OPTION_IGNORECRC);
  mad_frame_init(&f2);
  TASK_CHECK(mad_frame_decode_adu(&f2, &s2, bad, c.lens[1]) == 0);
  TASK_CHECK(s2.error == MAD_ERROR_NONE);
  mad_frame_finish(&f2);
  mad_stream_finish(&s2);

  free(bad);
  collector_free(&c);
  built_free(&b);
  return 0;
}

/* ------------------------------------------------------------------ */
/* lifecycle group: drive the actual example binary                   */
/* ------------------------------------------------------------------ */

#define LC_MP3	".task-build/lifecycle.mp3"
#define LC_ADU	".task-build/lifecycle.adu"
#define LC_PCM	".task-build/lifecycle.pcm"
#define LC_BIN	".task-build/mad_adu_example"

static signed int scale16(mad_fixed_t sample)
{
  sample += (1L << (MAD_F_FRACBITS - 16));
  if (sample >= MAD_F_ONE)
    sample = MAD_F_ONE - 1;
  else if (sample < -MAD_F_ONE)
    sample = -MAD_F_ONE;
  return sample >> (MAD_F_FRACBITS + 1 - 16);
}

static int case_lifecycle_example_roundtrip(void)
{
  struct built b;
  struct collected c;
  struct pcm_tape ref;
  FILE *fp;
  unsigned char hdr[4];
  unsigned long records = 0, pcm_bytes = 0, i;
  int ch;

  TASK_CHECK(build_fixture(&spec_a, &b) == 0);
  TASK_CHECK(collect_all(&b, &c, 0) == 0);
  TASK_CHECK(decode_physical(&b, &ref) == 0);

  fp = fopen(LC_MP3, "wb");
  TASK_CHECK(fp != 0);
  TASK_CHECK(fwrite(b.mp3, 1, b.mp3_len, fp) == b.mp3_len);
  TASK_CHECK(fclose(fp) == 0);

  TASK_CHECK(system(LC_BIN " pack " LC_MP3 " " LC_ADU) == 0);
  TASK_CHECK(system(LC_BIN " decode " LC_ADU " " LC_PCM) == 0);

  /* verify the on-disk record framing independently */
  fp = fopen(LC_ADU, "rb");
  TASK_CHECK(fp != 0);
  while (fread(hdr, 1, 4, fp) == 4) {
    unsigned long len = ((unsigned long) hdr[0] << 24) |
			((unsigned long) hdr[1] << 16) |
			((unsigned long) hdr[2] << 8) |
			(unsigned long) hdr[3];
    unsigned char *buf = malloc(len);
    TASK_CHECK(buf != 0);
    TASK_CHECK(fread(buf, 1, len, fp) == len);
    TASK_CHECK(records < c.count);
    TASK_CHECK(len == c.lens[records]);
    TASK_CHECK(memcmp(buf, c.pkts[records], len) == 0);
    free(buf);
    ++records;
  }
  TASK_CHECK(feof(fp));
  TASK_CHECK(records == c.count);
  TASK_CHECK(fclose(fp) == 0);

  /* verify the example's PCM against independently converted samples */
  fp = fopen(LC_PCM, "rb");
  TASK_CHECK(fp != 0);
  for (i = 0; i < ref.count; ++i) {
    for (ch = 0; ch < (int) ref.channels; ++ch) {
      unsigned char pair[2];
      signed int expect = scale16(ref.samples[i * 2 + ch]);
      TASK_CHECK(fread(pair, 1, 2, fp) == 2);
      TASK_CHECK((int) (pair[0] | (pair[1] << 8)) == (expect & 0xffff));
      pcm_bytes += 2;
    }
  }
  TASK_CHECK(fgetc(fp) == EOF);
  TASK_CHECK(fclose(fp) == 0);
  TASK_CHECK(pcm_bytes == ref.count * ref.channels * 2);

  pcm_free(&ref);
  collector_free(&c);
  built_free(&b);
  return 0;
}

static int case_lifecycle_empty_file(void)
{
  FILE *fp;

  fp = fopen(LC_MP3, "wb");
  TASK_CHECK(fp != 0);
  TASK_CHECK(fclose(fp) == 0);

  TASK_CHECK(system(LC_BIN " pack " LC_MP3 " " LC_ADU) == 0);
  TASK_CHECK(system(LC_BIN " decode " LC_ADU " " LC_PCM) == 0);

  fp = fopen(LC_ADU, "rb");
  TASK_CHECK(fp != 0);
  TASK_CHECK(fgetc(fp) == EOF);
  TASK_CHECK(fclose(fp) == 0);

  fp = fopen(LC_PCM, "rb");
  TASK_CHECK(fp != 0);
  TASK_CHECK(fgetc(fp) == EOF);
  TASK_CHECK(fclose(fp) == 0);
  return 0;
}

static int case_lifecycle_truncated_file_fails(void)
{
  struct built b;
  FILE *fp;

  TASK_CHECK(build_fixture(&spec_b, &b) == 0);
  fp = fopen(LC_MP3, "wb");
  TASK_CHECK(fp != 0);
  TASK_CHECK(fwrite(b.mp3, 1, b.mp3_len - 7, fp) == b.mp3_len - 7);
  TASK_CHECK(fclose(fp) == 0);

  TASK_CHECK(system(LC_BIN " pack " LC_MP3 " " LC_ADU) != 0);
  built_free(&b);
  return 0;
}

/* ------------------------------------------------------------------ */
/* case registry                                                      */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
  { "packetize", "mpeg1_stereo_crc_vbr",	case_pack_mpeg1_stereo_crc_vbr },
  { "packetize", "mpeg2_mono_crc_padding",	case_pack_mpeg2_mono_crc_padding },
  { "packetize", "mpeg2_stereo_vbr",		case_pack_mpeg2_stereo_vbr },
  { "packetize", "mpeg1_mono",			case_pack_mpeg1_mono },
  { "packetize", "empty_and_finish_idempotent",	case_pack_empty_and_idempotent_finish },
  { "packetize", "truncated",			case_pack_truncated },
  { "packetize", "callback_failure",		case_pack_callback_failure },
  { "packetize", "malformed",			case_pack_malformed },

  { "decode", "mpeg1_stereo_pcm_match",		case_decode_mpeg1_stereo },
  { "decode", "mpeg2_mono_pcm_match",		case_decode_mpeg2_mono },
  { "decode", "mpeg2_stereo_pcm_match",		case_decode_mpeg2_stereo },
  { "decode", "mpeg1_mono_pcm_match",		case_decode_mpeg1_mono },
  { "decode", "isolated_reservoir_packet",	case_decode_isolated_reservoir_packet },
  { "decode", "bad_lengths",			case_decode_bad_lengths },
  { "decode", "crc_and_ignorecrc",		case_decode_crc_and_ignore },

  { "lifecycle", "example_roundtrip",		case_lifecycle_example_roundtrip },
  { "lifecycle", "empty_file",			case_lifecycle_empty_file },
  { "lifecycle", "truncated_file_fails",	case_lifecycle_truncated_file_fails }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
