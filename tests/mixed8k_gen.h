/*
 * m8k_gen.h - tiny in-tree MPEG-2/2.5 Layer III bitstream generator for the
 * 8 kHz mixed-block contract tests.
 *
 * This is test support code only; it is never part of the library build.
 * Everything is written in plain C99 using the same UIMSBF bit ordering as
 * the decoder, so the generated frames are legal bitstreams consumable by
 * the public mad_frame_decode() API.
 *
 * Supported envelope: one granule (LSF), mono or two-channel modes,
 * 8 kHz MPEG-2.5 or 16 kHz MPEG-2, 16 kb/s, no bit reservoir, Huffman
 * pair table 0/1 for big_values and zero stuffing for the count1 region.
 */

#ifndef M8K_GEN_H
#define M8K_GEN_H

#include <string.h>

/* LSF scalefactor band counts, copied from the ISO tables used by
   III_scalefactors_lsf() (layer3.c nsfb_table).  Rows 0..2 are the normal
   path, rows 3..5 the intensity-stereo right-channel path; columns are
   indexed by 0 = long, 1 = short, 2 = mixed. */
static const unsigned char m8k_nsfb[6][3][4] = {
  { {  6,  5,  5, 5 }, {  9,  9,  9, 9 }, {  6,  9,  9, 9 } },
  { {  6,  5,  7, 3 }, {  9,  9, 12, 6 }, {  6,  9, 12, 6 } },
  { { 11, 10,  0, 0 }, { 18, 18,  0, 0 }, { 15, 18,  0, 0 } },
  { {  7,  7,  7, 0 }, { 12, 12, 12, 0 }, {  6, 15, 12, 0 } },
  { {  6,  6,  6, 3 }, { 12,  9,  9, 6 }, {  6, 12,  9, 6 } },
  { {  8,  8,  5, 0 }, { 15, 12,  9, 0 }, {  6, 18,  9, 0 } }
};

struct m8k_channel {
  unsigned char block_type;		/* 0, 1, 2 or 3 */
  unsigned char mixed;			/* mixed_block_flag (block_type 2) */
  unsigned char global_gain;
  unsigned short scalefac_compress;
  unsigned char scalefac[39];
  unsigned char subblock_gain[3];
  int coef[576];			/* signed Huffman magnitudes, 0/1 here */
};

struct m8k_frame_spec {
  int id25;				/* 1 = MPEG-2.5, 0 = MPEG-2 LSF */
  int mode;				/* enum mad_mode value */
  int mode_extension;			/* 0..3 (I_STEREO = 1, MS = 2) */
  struct m8k_channel ch[2];
};

static void m8k_putbits(unsigned char *p, unsigned *bit,
			unsigned value, unsigned n)
{
  while (n--) {
    p[*bit / 8] |= ((value >> n) & 1) << (7 - *bit % 8);
    ++*bit;
  }
}

/* 32-bit header built field by field so the byte values do not have to be
   reverse engineered.  No CRC, 16 kb/s, no padding, one private bit. */
static void m8k_header(unsigned char *d, const struct m8k_frame_spec *spec)
{
  unsigned bit = 0;
  int sr_index = spec->id25 ? 2 : 2;	/* 8000 (2.5) or 16000 (2) */

  m8k_putbits(d, &bit, 0x7ff, 11);	/* syncword */
  m8k_putbits(d, &bit, spec->id25 ? 0 : 1, 1);	/* MPEG 2.5 indicator */
  m8k_putbits(d, &bit, 0, 1);		/* ID: LSF */
  m8k_putbits(d, &bit, 1, 2);		/* Layer III */
  m8k_putbits(d, &bit, 1, 1);		/* no CRC */
  m8k_putbits(d, &bit, 2, 4);		/* bitrate index 2 = 16 kb/s LSF */
  m8k_putbits(d, &bit, sr_index, 2);
  m8k_putbits(d, &bit, 0, 1);		/* padding */
  m8k_putbits(d, &bit, 0, 1);		/* private */
  m8k_putbits(d, &bit, 3 - spec->mode, 2);	/* bit field: 3 - mad_mode */
  m8k_putbits(d, &bit, spec->mode_extension, 2);
  m8k_putbits(d, &bit, 0, 1);		/* copyright */
  m8k_putbits(d, &bit, 1, 1);		/* original */
  m8k_putbits(d, &bit, 0, 2);		/* emphasis */
}

static unsigned m8k_frame_bytes(const struct m8k_frame_spec *spec)
{
  /* 72 slots per LSF Layer III frame at 16 kb/s; 8 kHz doubles it */
  return spec->id25 ? 144u : 72u;
}

/* derive slen[4] for a scalefac_compress value in the normal path */
static void m8k_slen(unsigned short compress, unsigned slen[4],
		     unsigned *row)
{
  if (compress < 400) {
    *row = 0;
    slen[0] = (compress >> 4) / 5;
    slen[1] = (compress >> 4) % 5;
    slen[2] = (compress % 16) >> 2;
    slen[3] = compress % 4;
  }
  else if (compress < 500) {
    unsigned c = compress - 400;
    *row = 1;
    slen[0] = (c >> 2) / 5;
    slen[1] = (c >> 2) % 5;
    slen[2] = c % 4;
    slen[3] = 0;
  }
  else {
    unsigned c = compress - 500;
    *row = 2;
    slen[0] = c / 3;
    slen[1] = c % 3;
    slen[2] = 0;
    slen[3] = 0;
  }
}

/* intensity-stereo slen table for the right channel */
static void m8k_slen_is(unsigned short compress, unsigned slen[4],
			unsigned *row)
{
  compress >>= 1;
  if (compress < 180) {
    *row = 3;
    slen[0] = compress / 36;
    slen[1] = (compress % 36) / 6;
    slen[2] = (compress % 36) % 6;
    slen[3] = 0;
  }
  else if (compress < 244) {
    unsigned c = compress - 180;
    *row = 4;
    slen[0] = (c % 64) >> 4;
    slen[1] = (c % 16) >> 2;
    slen[2] = c % 4;
    slen[3] = 0;
  }
  else {
    unsigned c = compress - 244;
    *row = 5;
    slen[0] = c / 3;
    slen[1] = c % 3;
    slen[2] = 0;
    slen[3] = 0;
  }
}

/* serialize one channel's scalefactors; returns the number of bits */
static unsigned m8k_scalefactors(unsigned char *d, unsigned *bit,
				 const struct m8k_channel *c,
				 int is_right_intensity, int mixed4)
{
  unsigned slen[4], row, part, i, n = 0;
  unsigned index;
  const unsigned char *nsfb;
  unsigned start = *bit;

  if (c->block_type != 2)
    index = 0;
  else if (mixed4)
    index = 0;				/* option: long counts for 8 kHz */
  else
    index = c->mixed ? 2 : 1;

  if (is_right_intensity) {
    m8k_slen_is(c->scalefac_compress, slen, &row);
    nsfb = m8k_nsfb[row][index];
  }
  else {
    m8k_slen(c->scalefac_compress, slen, &row);
    nsfb = m8k_nsfb[row][index];
  }

  for (part = 0; part < 4; ++part) {
    for (i = 0; i < nsfb[part]; ++i) {
      m8k_putbits(d, bit, c->scalefac[n++], slen[part]);
    }
  }
  return *bit - start;
}

/* encode big_values pairs with Huffman pair table 1 (magnitudes 0/1) and
   terminate the count1 region with zero stuffing */
static unsigned m8k_huffman(unsigned char *d, unsigned *bit,
			    const int coef[576], unsigned big_values)
{
  unsigned start = *bit, p;

  for (p = 0; p < big_values; ++p) {
    int x = coef[2 * p], y = coef[2 * p + 1];
    int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;

    if (ax == 1 && ay == 1) {
      m8k_putbits(d, bit, 0, 3);
      m8k_putbits(d, bit, x < 0 ? 1 : 0, 1);
      m8k_putbits(d, bit, y < 0 ? 1 : 0, 1);
    }
    else if (ax == 0 && ay == 1) {
      m8k_putbits(d, bit, 1, 3);
      m8k_putbits(d, bit, y < 0 ? 1 : 0, 1);
    }
    else if (ax == 1 && ay == 0) {
      /* table 1: (1,0) is the 2-bit codeword "01", then the x sign */
      m8k_putbits(d, bit, 1, 2);
      m8k_putbits(d, bit, x < 0 ? 1 : 0, 1);
    }
    else {
      /* (0,0): table 1 encodes this as the single-bit codeword "1" */
      m8k_putbits(d, bit, 1, 1);
    }
  }

  /* Pad part3 to a byte boundary with '1' bits.  In the count1 region the
     decoder uses Huffman quad table A, where a single '1' bit encodes the
     all-zero quadruple (hlen 1).  Byte-aligning main_data (which itself
     starts on a byte boundary) keeps the decoder's bitcache from reaching
     for fake zero bits at the end of the last big_values codeword, and the
     padding ones are consumed harmlessly as zero count1 samples.  The
     padding is included in part2_3_length. */
  while (*bit % 8)
    m8k_putbits(d, bit, 1, 1);

  return *bit - start;
}

static unsigned m8k_last_nonzero(const int coef[576])
{
  int l;
  for (l = 575; l >= 0; --l)
    if (coef[l] != 0)
      return (unsigned) l;
  return 0;
}

/*
 * Build one complete frame at d (caller guarantees at least frame_bytes+8).
 * mixed4 selects the scalefactor encoding the new option expects; it only
 * changes how many scalefactor bits are emitted for an 8 kHz mixed block.
 * Returns the frame length in bytes (guard bytes left zero by caller).
 */
static unsigned m8k_build(unsigned char *d,
			  const struct m8k_frame_spec *spec, int mixed4)
{
  unsigned bytes = m8k_frame_bytes(spec);
  unsigned nch = spec->mode == 0 ? 1 : 2;
  unsigned bit, main_bit, ch;
  unsigned p23[2], part2[2], part3[2], big_values[2];

  memset(d, 0, bytes + 8);
  m8k_header(d, spec);

  /* side information starts at byte 4 */
  bit = 32;
  m8k_putbits(d, &bit, 0, 8);		/* main_data_begin = 0 */
  m8k_putbits(d, &bit, 0, nch == 1 ? 1 : 2);	/* private bits */

  for (ch = 0; ch < nch; ++ch) {
    const struct m8k_channel *c = &spec->ch[ch];
    unsigned last = m8k_last_nonzero(c->coef);
    int any = 0;
    unsigned k;

    for (k = 0; k < 576; ++k)
      if (c->coef[k]) any = 1;
    big_values[ch] = any ? (last / 2) + 1 : 0;

    /* placeholders, patched after main_data length is known */
    p23[ch] = bit;
    m8k_putbits(d, &bit, 0, 12);
    m8k_putbits(d, &bit, 2 * big_values[ch], 9);
    m8k_putbits(d, &bit, c->global_gain, 8);
    m8k_putbits(d, &bit, c->scalefac_compress, 9);

    if (c->block_type == 1 || c->block_type == 2 || c->block_type == 3) {
      m8k_putbits(d, &bit, 1, 1);	/* window_switching_flag */
      m8k_putbits(d, &bit, c->block_type, 2);
      m8k_putbits(d, &bit, c->block_type == 2 ? c->mixed : 0, 1);
      m8k_putbits(d, &bit, 1, 5);	/* table_select 0 = table 1 */
      m8k_putbits(d, &bit, 1, 5);	/* table_select 1 = table 1 */
      m8k_putbits(d, &bit, c->subblock_gain[0], 3);
      m8k_putbits(d, &bit, c->subblock_gain[1], 3);
      m8k_putbits(d, &bit, c->subblock_gain[2], 3);
    }
    else {
      m8k_putbits(d, &bit, 0, 1);	/* window_switching_flag */
      m8k_putbits(d, &bit, 1, 5);	/* table_select 0 = table 1 */
      m8k_putbits(d, &bit, 1, 5);	/* table_select 1 = table 1 */
      m8k_putbits(d, &bit, 1, 5);	/* table_select 2 = table 1 */
      m8k_putbits(d, &bit, 0, 4);	/* region0_count */
      m8k_putbits(d, &bit, 0, 3);	/* region1_count */
    }

    m8k_putbits(d, &bit, 0, 1);		/* scalefac_scale */
    m8k_putbits(d, &bit, 0, 1);		/* count1table_select */
  }

  /* main_data begins immediately after side information */
  main_bit = bit;

  for (ch = 0; ch < nch; ++ch) {
    const struct m8k_channel *c = &spec->ch[ch];
    int intensity = (spec->mode == 2 && (spec->mode_extension & 1) &&
		     ch == 1);

    part2[ch] = m8k_scalefactors(d, &main_bit, c, intensity,
				 mixed4 && spec->id25);
    part3[ch] = m8k_huffman(d, &main_bit, c->coef, big_values[ch]);
  }

  /* patch part2_3_length fields */
  for (ch = 0; ch < nch; ++ch) {
    unsigned k, v = part2[ch] + part3[ch], pos = p23[ch];
    for (k = 0; k < 12; ++k) {
      unsigned bitv = (v >> (11 - k)) & 1;
      d[(pos + k) / 8] =
	(d[(pos + k) / 8] & ~(1u << (7 - (pos + k) % 8))) |
	(bitv << (7 - (pos + k) % 8));
    }
  }

  return bytes;
}

#endif
