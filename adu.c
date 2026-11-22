/*
 * libmad - MPEG audio decoder library
 *
 * ADU (Audio Data Unit) packetization for MPEG-1/2 Layer III, per the
 * frame definition of RFC 5219 section 4.1 (without the RTP transport).
 *
 * Physical frames are split into a prefix (4-byte header, optional CRC
 * word and side information), which is copied verbatim, and a main-data
 * slot.  All slots are concatenated into one continuous byte axis; frame
 * i owns the axis interval [C_i - B_i, C_{i+1} - B_{i+1}), where C_i is
 * the start of frame i's slot and B_i its main_data_begin value.
 */

# ifdef HAVE_CONFIG_H
#  include "config.h"
# endif

# include "global.h"

# include <stdlib.h>
# include <string.h>

# include "bit.h"
# include "adu.h"

/*
 * Layer III bitrates, copied from frame.c: index 0 would mean free
 * format (rejected here) and index 15 is forbidden.
 */
static
unsigned long const bitrate_iii[2][15] = {
  /* MPEG-1 Layer III */
  { 0,  32000,  40000,  48000,  56000,  64000,  80000,  96000,
     112000, 128000, 160000, 192000, 224000, 256000, 320000 },
  /* MPEG-2 LSF Layer III */
  { 0,   8000,  16000,  24000,  32000,  40000,  48000,  56000,
      64000,  80000,  96000, 112000, 128000, 144000, 160000 }
};

static
unsigned int const samplerate_iii[2][3] = {
  { 44100, 48000, 32000 },	/* MPEG-1 */
  { 22050, 24000, 16000 }	/* MPEG-2 LSF */
};

/* largest possible side information block, including header and CRC */
# define ADU_MAX_PREFIX		(4 + 2 + 32)

/* main_data_begin is at most 9 bits wide */
# define ADU_MAX_BACKREF	511

struct frame_info {
  int lsf;				/* MPEG-2 lower sampling freq. */
  int nch;				/* channels */
  int crc;				/* CRC word size in bytes */
  unsigned int si_len;			/* side information length */
  unsigned int prefix;			/* header + CRC + side info */
  unsigned long frame_len;		/* physical frame length N */
  unsigned int main_begin;		/* main_data_begin */
  unsigned int p23_bits;		/* sum of part2_3_length */
};

struct frame_rec {
  unsigned char prefix[ADU_MAX_PREFIX];
  unsigned int prefix_len;
  unsigned long c;			/* slot start on the axis */
  unsigned int main_begin;
  unsigned int p23_bits;
};

struct mad_adu_packer {
  int (*emit)(void *, unsigned char const *, unsigned long);
  void *user;

  int dead;				/* permanently stopped */
  int finished;			/* finish() succeeded */
  int finish_result;			/* status of the first finish() */

  int format_known;			/* first frame parsed */
  int lsf;
  int samplerate_index;
  int nch;

  unsigned char *partial;		/* current physical frame bytes */
  unsigned long partial_len, partial_cap;
  int header_known;
  struct frame_info cur;

  unsigned char *axis;			/* materialized slot axis */
  unsigned long axis_origin;		/* axis coordinate of axis[0] */
  unsigned long axis_len, axis_cap;

  struct frame_rec rec[2];		/* undelivered frame records */
  int nrec;

  unsigned char *out;			/* ADU assembly buffer */
  unsigned long out_cap;
};

static
int grow(unsigned char **buf, unsigned long *cap, unsigned long need)
{
  unsigned char *next;
  unsigned long ncap;

  if (need <= *cap)
    return 0;

  ncap = *cap ? *cap : 256;
  while (ncap < need) {
    if (ncap > (unsigned long) -1 / 2)
      return -1;
    ncap *= 2;
  }

  next = realloc(*buf, ncap);
  if (next == 0)
    return -1;

  *buf = next;
  *cap = ncap;
  return 0;
}

/*
 * Parse the 4-byte header and, once the whole side information block is
 * present, the side information of one physical frame.
 *
 * Returns 0 on success, 1 when more bytes are needed and -1 on a hard
 * format error.
 */
static
int parse_frame(unsigned char const *frame, unsigned long avail,
		struct frame_info *info, int *format_known,
		int *known_lsf, int *known_sr_index, int *known_nch)
{
  unsigned char const *h;
  unsigned int bitrate_index, samplerate_index, padding, mode_field;
  unsigned long bitrate, slots;
  struct mad_bitptr ptr;
  unsigned int ngr, gr, ch, priv_bitlen, si_off;

  if (avail < 4)
    return 1;

  h = frame;

  if (!(h[0] == 0xff && (h[1] & 0xe0) == 0xe0))
    return -1;					/* lost synchronization */

  /* the MPEG 2.5 indicator is really part of the sync word */
  if ((h[1] & 0x10) == 0)
    return -1;					/* MPEG 2.5 unsupported */

  info->lsf = (h[1] & 0x08) == 0;

  if (((h[1] >> 1) & 0x03) != 0x01)
    return -1;					/* only Layer III */

  info->crc = (h[1] & 0x01) ? 0 : 2;

  bitrate_index = h[2] >> 4;
  if (bitrate_index == 0 || bitrate_index == 15)
    return -1;					/* free format/forbidden */

  samplerate_index = (h[2] >> 2) & 0x03;
  if (samplerate_index == 3)
    return -1;

  padding = (h[2] >> 1) & 0x01;
  mode_field = h[3] >> 6;
  info->nch = (mode_field == 3) ? 1 : 2;

  if (*format_known) {
    if (info->lsf != *known_lsf ||
	samplerate_index != (unsigned int) *known_sr_index ||
	info->nch != *known_nch)
      return -1;				/* format changed mid-stream */
  }
  else {
    *format_known = 1;
    *known_lsf = info->lsf;
    *known_sr_index = (int) samplerate_index;
    *known_nch = info->nch;
  }

  bitrate = bitrate_iii[info->lsf][bitrate_index];
  slots = info->lsf ? 72 : 144;
  info->frame_len =
    slots * bitrate / samplerate_iii[info->lsf][samplerate_index] + padding;

  info->si_len = info->lsf ?
    (info->nch == 1 ? 9 : 17) : (info->nch == 1 ? 17 : 32);
  info->prefix = 4 + info->crc + info->si_len;

  if (info->frame_len < info->prefix)
    return -1;
  if (avail < info->prefix)
    return 1;					/* truncated side info */

  /* side information */

  si_off = 4 + info->crc;
  mad_bit_init(&ptr, frame + si_off);

  info->main_begin =
    mad_bit_read(&ptr, info->lsf ? 8 : 9);

  priv_bitlen = info->lsf ?
    (info->nch == 1 ? 1 : 2) : (info->nch == 1 ? 5 : 3);
  mad_bit_skip(&ptr, priv_bitlen);

  if (!info->lsf)
    mad_bit_skip(&ptr, 4 * info->nch);		/* scfsi */

  info->p23_bits = 0;
  ngr = info->lsf ? 1 : 2;

  for (gr = 0; gr < ngr; ++gr) {
    for (ch = 0; ch < (unsigned int) info->nch; ++ch) {
      unsigned int big_values;

      info->p23_bits += mad_bit_read(&ptr, 12);	/* part2_3_length */
      big_values = mad_bit_read(&ptr, 9);
      if (big_values > 288)
	return -1;

      mad_bit_skip(&ptr, 8);			/* global_gain */
      mad_bit_skip(&ptr, info->lsf ? 9 : 4);	/* scalefac_compress */

      if (mad_bit_read(&ptr, 1)) {		/* window_switching_flag */
	mad_bit_skip(&ptr, 2);			/* block_type */
	mad_bit_skip(&ptr, 1);			/* mixed_block_flag */
	mad_bit_skip(&ptr, 5 + 5);		/* table_select */
	mad_bit_skip(&ptr, 3 + 3 + 3);		/* subblock_gain */
      }
      else {
	mad_bit_skip(&ptr, 5 + 5 + 5);		/* table_select */
	mad_bit_skip(&ptr, 4);			/* region0_count */
	mad_bit_skip(&ptr, 3);			/* region1_count */
      }

      /* scalefac_scale and count1table_select (plus preflag for MPEG-1) */
      mad_bit_skip(&ptr, info->lsf ? 2 : 3);
    }
  }

  return 0;
}

static
int emit_adu(struct mad_adu_packer *packer, struct frame_rec const *rec,
	     unsigned long body_begin, unsigned long body_end)
{
  unsigned long body_len, total;
  unsigned char *p;

  body_len = body_end - body_begin;
  total = rec->prefix_len + body_len;

  if (grow(&packer->out, &packer->out_cap, total) == -1)
    return -1;

  p = packer->out;
  memcpy(p, rec->prefix, rec->prefix_len);
  p += rec->prefix_len;
  memcpy(p, packer->axis + (body_begin - packer->axis_origin), body_len);

  if (packer->emit(packer->user, packer->out, total) != 0)
    return -1;

  return 0;
}

/*
 * A complete physical frame sits at the front of the partial buffer.
 * Append its slot to the axis and, once the following frame is known,
 * emit the frame that is now fully bounded.
 */
static
int accept_frame(struct mad_adu_packer *packer)
{
  struct frame_info const *cur = &packer->cur;
  unsigned long slot_len, c, body_begin;
  struct frame_rec *rec;
  unsigned int idx;

  slot_len = cur->frame_len - cur->prefix;
  c = packer->axis_origin + packer->axis_len;

  if (packer->nrec == 0 && cur->main_begin != 0)
    return -1;					/* first frame must start at 0 */

  if (cur->main_begin > c)
    return -1;					/* back reference off the axis */

  body_begin = c - cur->main_begin;

  if (packer->nrec > 0) {
    struct frame_rec const *prev = &packer->rec[0];
    unsigned long prev_begin = prev->c - prev->main_begin;

    if (body_begin < prev_begin)
      return -1;				/* body starts moved backwards */
  }

  if (grow(&packer->axis, &packer->axis_cap,
	   packer->axis_len + slot_len) == -1)
    return -1;

  memcpy(packer->axis + packer->axis_len,
	 packer->partial + cur->prefix, slot_len);
  packer->axis_len += slot_len;

  idx = packer->nrec;				/* always 0 or 1 */
  rec = &packer->rec[idx];
  memcpy(rec->prefix, packer->partial, cur->prefix);
  rec->prefix_len = cur->prefix;
  rec->c = c;
  rec->main_begin = cur->main_begin;
  rec->p23_bits = cur->p23_bits;
  packer->nrec++;

  if (packer->nrec == 2) {
    struct frame_rec const *old = &packer->rec[0];
    unsigned long old_begin = old->c - old->main_begin;
    unsigned long old_end = body_begin;
    unsigned long old_body = old_end - old_begin;

    if (old->p23_bits > old_body * 8)
      return -1;				/* body cannot hold the audio */

    if (emit_adu(packer, old, old_begin, old_end) == -1)
      return -1;

    /* drop axis bytes no longer reachable and rotate the records */
    memmove(packer->axis,
	    packer->axis + (old_end - packer->axis_origin),
	    packer->axis_len - (old_end - packer->axis_origin));
    packer->axis_len -= old_end - packer->axis_origin;
    packer->axis_origin = old_end;
    packer->rec[0] = packer->rec[1];
    packer->nrec = 1;
  }

  /* remove the consumed frame, keep any bytes of the next one */
  memmove(packer->partial,
	  packer->partial + cur->frame_len,
	  packer->partial_len - cur->frame_len);
  packer->partial_len -= cur->frame_len;
  packer->header_known = 0;

  return 0;
}

static
int pump(struct mad_adu_packer *packer)
{
  for (;;) {
    if (!packer->header_known) {
      if (packer->partial_len < 4)
	return 0;

      switch (parse_frame(packer->partial, packer->partial_len,
			  &packer->cur, &packer->format_known,
			  &packer->lsf, &packer->samplerate_index,
			  &packer->nch)) {
      case 0:
	break;
      case 1:
	return 0;				/* wait for more bytes */
      default:
	return -1;
      }
      packer->header_known = 1;
    }

    if (packer->partial_len < packer->cur.frame_len)
      return 0;				/* wait for the rest of the frame */

    if (accept_frame(packer) == -1)
      return -1;
  }
}

struct mad_adu_packer *mad_adu_packer_new(
    int (*emit)(void *, unsigned char const *, unsigned long),
    void *user)
{
  struct mad_adu_packer *packer;

  if (emit == 0)
    return 0;

  packer = malloc(sizeof(*packer));
  if (packer == 0)
    return 0;

  memset(packer, 0, sizeof(*packer));
  packer->emit = emit;
  packer->user = user;

  return packer;
}

int mad_adu_packer_feed(struct mad_adu_packer *packer,
			unsigned char const *data, unsigned long length)
{
  if (packer == 0 || packer->dead || packer->finished)
    return -1;

  if (length == 0)
    return 0;
  if (data == 0)
    return -1;

  if (grow(&packer->partial, &packer->partial_cap,
	   packer->partial_len + length) == -1) {
    packer->dead = 1;
    return -1;
  }

  memcpy(packer->partial + packer->partial_len, data, length);
  packer->partial_len += length;

  if (pump(packer) == -1) {
    packer->dead = 1;
    return -1;
  }

  return 0;
}

int mad_adu_packer_finish(struct mad_adu_packer *packer)
{
  unsigned long axis_end, body_begin, body_len;

  if (packer == 0 || packer->dead)
    return -1;

  if (packer->finished)
    return packer->finish_result;

  /* any unparsed bytes mean a truncated final frame */
  if (packer->partial_len != 0) {
    packer->dead = 1;
    return -1;
  }

  if (packer->nrec == 0) {
    packer->finished = 1;
    packer->finish_result = 0;
    return 0;
  }

  /* flush the last frame up to the end of the last physical slot */
  {
    struct frame_rec const *last = &packer->rec[0];

    body_begin = last->c - last->main_begin;
    axis_end = packer->axis_origin + packer->axis_len;
    body_len = axis_end - body_begin;

    if (body_begin < packer->axis_origin ||
	last->p23_bits > body_len * 8) {
      packer->dead = 1;
      return -1;
    }

    if (emit_adu(packer, last, body_begin, axis_end) == -1) {
      packer->dead = 1;
      return -1;
    }
  }

  packer->nrec = 0;
  packer->finished = 1;
  packer->finish_result = 0;

  return 0;
}

void mad_adu_packer_free(struct mad_adu_packer *packer)
{
  if (packer == 0)
    return;

  free(packer->partial);
  free(packer->axis);
  free(packer->out);
  free(packer);
}
