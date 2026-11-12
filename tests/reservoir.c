/* tests/reservoir.c
 *
 * Independent checks for shine_initialise_reservoir(). The tests parse
 * the real encoder output themselves: frame headers, side information
 * and the concatenated main-data stream built by stripping header and
 * side information from every physical frame. They verify the
 * cross-frame interval layout, recompute Huffman code lengths
 * independently to prove that borrowing frames really carry more
 * scalefactor/Huffman data than their own capacity (stuffing excluded),
 * and decode everything with the pinned minimp3 copy in
 * devcheck/vendor. No sound card, no external encoder.
 */

#include "layer3.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "huffman.h"
#include "tables.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "vendor/minimp3.h"

#define MAX_FRAMES 128
#define OUT_CAP (1 << 18)

static unsigned char buf_a[OUT_CAP];
static unsigned char buf_b[OUT_CAP];
static unsigned char buf_c[OUT_CAP];

/* ------------------------------------------------------------------ */
/* Bit-level reader                                                    */
/* ------------------------------------------------------------------ */

static unsigned bits_at(const unsigned char *p, long pos, int n) {
  unsigned v = 0;
  while (n--) {
    v = (v << 1) | ((p[pos >> 3] >> (7 - (pos & 7))) & 1);
    pos++;
  }
  return v;
}

/* ------------------------------------------------------------------ */
/* Frame header / side information parsing                             */
/* ------------------------------------------------------------------ */

typedef struct {
  long off; /* byte offset inside the stream */
  int version, mode, channels, padding, granules;
  int sr_idx;  /* 0..8 samplerate index */
  int rate, bitr;
  int frame_bytes, side_bytes, cap;
  int mdb;          /* main_data_begin, bytes */
  int p23[2][2];    /* part2_3_length per granule/channel, bits */
} frame_t;

static void parse_frame(const unsigned char *p, long off, frame_t *f) {
  int bi, si, base;

  memset(f, 0, sizeof(*f));
  f->off = off;
  assert(bits_at(p, 0, 11) == 0x7ff);       /* sync */
  f->version = (int)bits_at(p, 11, 2);
  assert(f->version == MPEG_I || f->version == MPEG_II ||
         f->version == MPEG_25);
  assert(bits_at(p, 13, 2) == LAYER_III);
  assert(bits_at(p, 15, 1) == 1);           /* no CRC */
  bi = (int)bits_at(p, 16, 4);
  si = (int)bits_at(p, 20, 2);
  f->padding = (int)bits_at(p, 22, 1);
  f->mode = (int)bits_at(p, 24, 2);
  f->channels = f->mode == MONO ? 1 : 2;

  base = f->version == MPEG_I ? 0 : f->version == MPEG_II ? 3 : 6;
  f->sr_idx = base + si;
  f->rate = samplerates[f->sr_idx];
  f->bitr = bitrates[bi][f->version];
  assert(f->bitr > 0);

  f->granules = f->version == MPEG_I ? 2 : 1;
  f->frame_bytes =
      (f->version == MPEG_I ? 144000 : 72000) * f->bitr / f->rate + f->padding;
  f->side_bytes = 4 + (f->version == MPEG_I ? (f->channels == 2 ? 32 : 17)
                                            : (f->channels == 2 ? 17 : 9));
  f->cap = f->frame_bytes - f->side_bytes;
  assert(f->cap > 0);

  f->mdb = (int)bits_at(p + 4, 0, f->version == MPEG_I ? 9 : 8);

  {
    int pos = f->version == MPEG_I
                  ? 9 + (f->channels == 2 ? 3 : 5) + 4 * f->channels
                  : 8 + (f->channels == 2 ? 2 : 1);
    int gr, ch;
    for (gr = 0; gr < f->granules; gr++)
      for (ch = 0; ch < f->channels; ch++) {
        f->p23[gr][ch] = (int)bits_at(p + 4, pos, 12);
        pos += f->version == MPEG_I ? 59 : 63;
      }
    assert(pos == (f->side_bytes - 4) * 8);
  }
}

static int parse_stream(const unsigned char *out, size_t len, frame_t *fr,
                        int maxf) {
  size_t off = 0;
  int n = 0;
  while (off < len) {
    assert(n < maxf);
    parse_frame(out + off, (long)off, &fr[n]);
    off += (size_t)fr[n].frame_bytes;
    n++;
  }
  assert(off == len);
  return n;
}

/* Full side information of one granule/channel, for the independent
 * code-length recomputation. */
typedef struct {
  unsigned p23, big_values, scalefac_compress, table_select[3];
  unsigned region0_count, region1_count, count1table_select;
} gside_t;

static void parse_side(const frame_t *f, const unsigned char *frame,
                       int scfsi[2][4], gside_t gs[2][2]) {
  const unsigned char *s = frame + 4;
  int pos = f->version == MPEG_I ? 9 : 8;
  int gr, ch, b, r;

  pos += f->channels == 2 ? (f->version == MPEG_I ? 3 : 2)
                          : (f->version == MPEG_I ? 5 : 1);
  if (f->version == MPEG_I)
    for (ch = 0; ch < f->channels; ch++)
      for (b = 0; b < 4; b++)
        scfsi[ch][b] = (int)bits_at(s, pos++, 1);
  else
    for (ch = 0; ch < f->channels; ch++)
      for (b = 0; b < 4; b++)
        scfsi[ch][b] = 0;

  for (gr = 0; gr < f->granules; gr++)
    for (ch = 0; ch < f->channels; ch++) {
      gside_t *g = &gs[gr][ch];
      int scfc_bits = f->version == MPEG_I ? 4 : 9;
      int q = 29 + scfc_bits; /* window_switching bit position */
      g->p23 = (unsigned)bits_at(s, pos, 12);
      g->big_values = (unsigned)bits_at(s, pos + 12, 9);
      g->scalefac_compress = (unsigned)bits_at(s, pos + 29, scfc_bits);
      for (r = 0; r < 3; r++)
        g->table_select[r] = (unsigned)bits_at(s, pos + q + 1 + 5 * r, 5);
      g->region0_count = (unsigned)bits_at(s, pos + q + 16, 4);
      g->region1_count = (unsigned)bits_at(s, pos + q + 20, 3);
      g->count1table_select =
          (unsigned)bits_at(s, pos + q + (f->version == MPEG_I ? 25 : 24), 1);
      pos += f->version == MPEG_I ? 59 : 63;
    }
}

/* ------------------------------------------------------------------ */
/* Layout verification                                                 */
/*                                                                     */
/* Concatenate the main-data areas of all physical frames into one     */
/* byte stream. Frame i's data must occupy [8*(C_i - B_i), that plus   */
/* L_i), with C_i the cumulated capacity before it, B_i its            */
/* main_data_begin and L_i its summed part2_3_length.                  */
/* ------------------------------------------------------------------ */

static void check_layout(const frame_t *fr, int n, int max_bytes,
                         int allow_drain) {
  long long C = 0, prev_end = 0;
  int i, gr, ch;

  assert(fr[0].mdb == 0); /* first frame points nowhere */
  for (i = 0; i < n; i++) {
    long L = 0;
    long long S, gap;

    for (gr = 0; gr < fr[i].granules; gr++)
      for (ch = 0; ch < fr[i].channels; ch++) {
        assert(fr[i].p23[gr][ch] <= 4095);
        L += fr[i].p23[gr][ch];
      }

    assert(fr[i].mdb <= max_bytes);
    S = C - 8LL * fr[i].mdb;
    assert(S >= 0);           /* not before the stream head */
    assert((S & 7) == 0);     /* byte aligned start */
    assert(S + L <= C + 8LL * fr[i].cap); /* not past its own frame */

    gap = S - prev_end; /* bits no granule claims (ancillary drain) */
    assert(gap >= 0);   /* intervals must not overlap */
    if (!allow_drain)
      assert(gap == 0);

    prev_end = S + L;
    C += 8LL * fr[i].cap;
  }
}

/* ------------------------------------------------------------------ */
/* Independent granule parsing and code-length recomputation           */
/* ------------------------------------------------------------------ */

typedef struct {
  const unsigned char *p;
  long pos, limit;
} brd_t;

static int rd_bit(brd_t *b) {
  int v;
  if (b->pos >= b->limit)
    return -1;
  v = (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
  b->pos++;
  return v;
}

static int rd_bits(brd_t *b, int n) {
  int v = 0;
  while (n--) {
    int bit = rd_bit(b);
    if (bit < 0)
      return -1;
    v = (v << 1) | bit;
  }
  return v;
}

/* Decode one Huffman codeword by table lookup; -1 if the bit budget of
 * the granule is exhausted before any codeword completes. */
static int huff_sym(brd_t *b, const struct huffcodetab *h) {
  unsigned code = 0, n = h->xlen * h->ylen, i;
  int len = 0;
  for (;;) {
    int bit = rd_bit(b);
    if (bit < 0)
      return -1;
    code = (code << 1) | (unsigned)bit;
    len++;
    for (i = 0; i < n; i++)
      if (h->hlen[i] == len && h->table[i] == code)
        return (int)i;
    assert(len <= 32);
  }
}

static int huff_pair(brd_t *b, const struct huffcodetab *h, int *x, int *y) {
  int idx = huff_sym(b, h);
  if (idx < 0)
    return -1;
  *x = idx / (int)h->ylen;
  *y = idx % (int)h->ylen;
  if (h->linbits) {
    if (*x == 15) {
      int v = rd_bits(b, (int)h->linbits);
      if (v < 0)
        return -1;
      *x += v;
    }
    if (*y == 15) {
      int v = rd_bits(b, (int)h->linbits);
      if (v < 0)
        return -1;
      *y += v;
    }
  }
  if (*x && rd_bit(b) < 0)
    return -1;
  if (*y && rd_bit(b) < 0)
    return -1;
  return 0;
}

static int huff_quad(brd_t *b, const struct huffcodetab *h, int *v) {
  int idx = huff_sym(b, h), k;
  if (idx < 0)
    return -1;
  for (k = 0; k < 4; k++) {
    v[k] = (idx >> k) & 1;
    if (v[k] && rd_bit(b) < 0)
      return -1;
  }
  return 0;
}

/* Parse one granule/channel of scalefactor + Huffman data from the
 * concatenated main-data stream, then recompute the number of bits the
 * coded data really needs from the decoded spectral values, excluding
 * any stuffing. Returns the true bit count; the granule must consume
 * exactly its part2_3_length. */
static long granule_true_bits(const unsigned char *md, long start,
                              const gside_t *g, const int *scfsi, int gr,
                              int sr_idx, int *ix) {
  brd_t b;
  const int *sfb = shine_scale_fact_band_index[sr_idx];
  int slen1, slen2, band, sfb_i, i, ci;
  long part2 = 0, true_bits;
  int r1, r2, npairs, count1, big_values;

  assert(g->scalefac_compress < 16);
  slen1 = shine_slen1_tab[g->scalefac_compress];
  slen2 = shine_slen2_tab[g->scalefac_compress];

  b.p = md;
  b.pos = start;
  b.limit = start + (long)g->p23;

  /* scalefactors */
  {
    static const int nb[4] = {6, 5, 5, 5};
    int sfb0 = 0;
    for (band = 0; band < 4; band++) {
      int sl = band < 2 ? slen1 : slen2;
      if (!(gr == 1 && scfsi[band]))
        for (sfb_i = 0; sfb_i < nb[band]; sfb_i++) {
          if (sl && rd_bits(&b, sl) < 0)
            assert(0);
          part2 += sl;
        }
      sfb0 += nb[band];
    }
    (void)sfb0;
  }

  /* big_values region */
  r1 = sfb[g->region0_count + 1];
  r2 = sfb[g->region0_count + 1 + g->region1_count + 1];
  npairs = (int)g->big_values * 2;
  for (i = 0; i < npairs; i += 2) {
    int t = (int)g->table_select[(i >= r1) + (i >= r2)];
    if (!t) {
      ix[i] = ix[i + 1] = 0;
      continue;
    }
    if (huff_pair(&b, &shine_huffman_table[t], &ix[i], &ix[i + 1]) < 0)
      assert(0);
  }

  /* count1 region, then stuffing (all ones, decodes as zero quads) */
  ci = npairs;
  for (;;) {
    long save;
    int v[4];
    if (b.pos == b.limit)
      break;
    save = b.pos;
    if (ci + 4 > 576 ||
        huff_quad(&b, &shine_huffman_table[32 + g->count1table_select], v) < 0) {
      /* Past the real count1 data only stuffing ones remain; decoders
       * stop at 576 values and skip the rest of the granule budget. */
      long p;
      for (p = save; p < b.limit; p++)
        assert((int)bits_at(md, p, 1) == 1); /* stuffing is all ones */
      b.pos = b.limit;
      break;
    }
    for (i = 0; i < 4; i++)
      ix[ci + i] = v[i];
    ci += 4;
  }
  assert(b.pos == b.limit);
  for (i = ci; i < 576; i++)
    ix[i] = 0;

  /* Independent code-length recomputation from the decoded values. */
  for (i = 576; i > 1; i -= 2)
    if (ix[i - 1] || ix[i - 2])
      break;
  count1 = 0;
  for (; i > 3; i -= 4)
    if (ix[i - 1] <= 1 && ix[i - 2] <= 1 && ix[i - 3] <= 1 && ix[i - 4] <= 1)
      count1++;
    else
      break;
  big_values = i >> 1;
  assert(big_values == (int)g->big_values);

  true_bits = part2;
  for (i = 0; i < 2 * big_values; i += 2) {
    int t = (int)g->table_select[(i >= r1) + (i >= r2)];
    const struct huffcodetab *h;
    int x, y, cx, cy;
    if (!t)
      continue;
    h = &shine_huffman_table[t];
    x = ix[i];
    y = ix[i + 1];
    cx = x > 14 ? 15 : x;
    cy = y > 14 ? 15 : y;
    true_bits += h->hlen[cx * (int)h->ylen + cy];
    if (h->linbits) {
      if (x > 14)
        true_bits += h->linbits;
      if (y > 14)
        true_bits += h->linbits;
    }
    if (x)
      true_bits++;
    if (y)
      true_bits++;
  }
  {
    const struct huffcodetab *h = &shine_huffman_table[32 + g->count1table_select];
    for (i = 2 * big_values; i < 2 * big_values + 4 * count1; i += 4) {
      int p = ix[i] + 2 * ix[i + 1] + 4 * ix[i + 2] + 8 * ix[i + 3];
      int k;
      true_bits += h->hlen[p];
      for (k = 0; k < 4; k++)
        if (ix[i + k])
          true_bits++;
    }
  }

  assert(true_bits <= (long)g->p23); /* stuffing may not be negative */
  return true_bits;
}

/* ------------------------------------------------------------------ */
/* minimp3 decode check                                                */
/* ------------------------------------------------------------------ */

static void check_decode(const unsigned char *out, size_t len,
                         const frame_t *fr, int nfr) {
  mp3dec_t dec;
  size_t off = 0;
  long samples = 0;
  uint64_t energy = 0;
  int spf = fr[0].granules * 576;
  int i, j;

  mp3dec_init(&dec);
  for (i = 0; i < nfr; i++) {
    mp3dec_frame_info_t info;
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int n = mp3dec_decode_frame(&dec, out + off, (int)(len - off), pcm, &info);
    assert(info.frame_offset == 0);
    assert(info.frame_bytes == fr[i].frame_bytes);
    assert(n == spf);
    assert(info.hz == fr[i].rate);
    assert(info.channels == fr[i].channels);
    for (j = 0; j < n * fr[i].channels; j++)
      energy += (uint64_t)((int64_t)pcm[j] * pcm[j]);
    off += (size_t)info.frame_bytes;
    samples += n;
  }
  assert(off == len);
  assert(samples == (long)nfr * spf);
  assert(energy > 1000000); /* nonzero sound must survive */
  printf("  decode: frames=%d rate=%d channels=%d samples=%ld energy=%llu\n",
         nfr, fr[0].rate, fr[0].channels, samples,
         (unsigned long long)energy);
}

/* ------------------------------------------------------------------ */
/* Encoder drivers                                                     */
/* ------------------------------------------------------------------ */

static shine_t make_encoder(int rate, int bitr, int channels, int max_bytes) {
  shine_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  shine_set_config_mpeg_defaults(&cfg.mpeg);
  cfg.wave.channels = channels == 1 ? PCM_MONO : PCM_STEREO;
  cfg.wave.samplerate = rate;
  cfg.mpeg.bitr = bitr;
  cfg.mpeg.mode = channels == 1 ? MONO : STEREO;
  return max_bytes >= 0 ? shine_initialise_reservoir(&cfg, max_bytes)
                        : shine_initialise(&cfg);
}

static void gen_block(uint32_t *rng, int n, int channels, int noisy,
                      int16_t *l, int16_t *r, int16_t *inter) {
  int i;
  for (i = 0; i < n; i++) {
    *rng = 1664525u * *rng + 1013904223u;
    l[i] = noisy ? (int16_t)((int)(*rng >> 18) - 8192) : 0;
    *rng = 1664525u * *rng + 1013904223u;
    r[i] = noisy ? (int16_t)((int)(*rng >> 18) - 8192) : 0;
    inter[channels * i] = l[i];
    if (channels == 2)
      inter[2 * i + 1] = r[i];
  }
}

/* Encode `frames` PCM blocks; blocks [noise_start, noise_end) carry the
 * LCRNG noise, the rest is digital silence. max_bytes < 0 selects the
 * legacy shine_initialise entry point. */
static size_t encode_all(int rate, int bitr, int channels, int max_bytes,
                         int frames, int noise_start, int noise_end,
                         int planar, unsigned char *out) {
  shine_t s = make_encoder(rate, bitr, channels, max_bytes);
  int n, f;
  int16_t inter[2 * SHINE_MAX_SAMPLES], l[SHINE_MAX_SAMPLES],
      r[SHINE_MAX_SAMPLES];
  int16_t *planes[2] = {l, r};
  uint32_t rng = 0x12345678u;
  size_t total = 0;

  assert(s);
  n = shine_samples_per_pass(s);
  for (f = 0; f < frames; f++) {
    int w = 0;
    unsigned char *p;
    gen_block(&rng, n, channels, f >= noise_start && f < noise_end, l, r,
              inter);
    p = planar ? shine_encode_buffer(s, planes, &w)
               : shine_encode_buffer_interleaved(s, inter, &w);
    assert(w >= 0 && total + (size_t)w <= OUT_CAP);
    if (w)
      memcpy(out + total, p, (size_t)w);
    total += (size_t)w;
  }
  {
    int w = 0;
    unsigned char *p = shine_flush(s, &w);
    assert(w >= 0 && total + (size_t)w <= OUT_CAP);
    if (w)
      memcpy(out + total, p, (size_t)w);
    total += (size_t)w;
  }
  shine_close(s);
  return total;
}

/* ------------------------------------------------------------------ */
/* Test groups                                                         */
/* ------------------------------------------------------------------ */

/* The new entry point with max_bytes=0 must be byte-identical to the
 * original entry point. */
static void case_legacy(void) {
  static const int rates[4] = {48000, 44100, 24000, 8000};
  static const int bitrs[4] = {128, 128, 64, 32};
  static const int chs[4] = {2, 2, 2, 1};
  int k;

  for (k = 0; k < 4; k++) {
    size_t la = encode_all(rates[k], bitrs[k], chs[k], -1, 40, 8, 32, 0, buf_a);
    size_t lb = encode_all(rates[k], bitrs[k], chs[k], 0, 40, 8, 32, 0, buf_b);
    assert(la == lb);
    assert(!memcmp(buf_a, buf_b, la));
    printf("  legacy: rate=%d channels=%d bytes=%zu identical\n", rates[k],
           chs[k], la);
  }
  puts("RESERVOIR_CASE legacy PASS");
}

/* The specified borrowing scenario: quiet frames must bank capacity and
 * at least one complex frame must really spend more than its own
 * main-data capacity, proven with an independent code-length
 * recomputation (stuffing excluded). */
static void case_borrow(void) {
  frame_t fr[MAX_FRAMES];
  unsigned char *md;
  int scfsi[2][4];
  gside_t gs[2][2];
  int ix[576];
  size_t len;
  long long C = 0, mdlen = 0;
  int n, i, gr, ch, borrowed = 0;

  len = encode_all(48000, 128, 2, 511, 40, 8, 32, 0, buf_a);
  n = parse_stream(buf_a, len, fr, MAX_FRAMES);
  assert(n == 40);
  check_layout(fr, n, 511, 0);
  check_decode(buf_a, len, fr, n);

  /* Build the concatenated main-data stream ourselves. */
  md = malloc(len);
  assert(md);
  for (i = 0; i < n; i++) {
    memcpy(md + mdlen, buf_a + fr[i].off + fr[i].side_bytes,
           (size_t)fr[i].cap);
    mdlen += fr[i].cap;
  }

  for (i = 0; i < n; i++) {
    long L = 0;
    for (gr = 0; gr < fr[i].granules; gr++)
      for (ch = 0; ch < fr[i].channels; ch++)
        L += fr[i].p23[gr][ch];

    if (L > 8L * fr[i].cap) {
      long long S = C - 8LL * fr[i].mdb;
      long pos = (long)S, true_total = 0;
      parse_side(&fr[i], buf_a + fr[i].off, scfsi, gs);
      for (gr = 0; gr < fr[i].granules; gr++)
        for (ch = 0; ch < fr[i].channels; ch++) {
          true_total += granule_true_bits(md, pos, &gs[gr][ch], scfsi[ch], gr,
                                          fr[i].sr_idx, ix);
          pos += gs[gr][ch].p23;
        }
      assert(pos == (long)(S + L));
      /* Not just a fat part2_3_length: the actual scalefactor/Huffman
       * data, stuffing excluded, must exceed this frame's own capacity
       * for at least one of the frames that claim more than their
       * capacity. */
      if (true_total > 8L * fr[i].cap) {
        borrowed++;
        printf("  borrow: frame %d claims %ld of %d bits, true coded %ld\n",
               i, L, 8 * fr[i].cap, true_total);
      }
    }
    C += 8LL * fr[i].cap;
  }
  assert(borrowed >= 1);
  free(md);
  puts("RESERVOIR_CASE borrow PASS");
}

/* Silence (reservoir charges), noise (reservoir drains), silence again
 * (reservoir recharges): check every frame's interval and the tail. */
static void case_layout(void) {
  frame_t fr[MAX_FRAMES];
  size_t len;
  int n, i, maxb = 0, minb_noise = 511;

  len = encode_all(48000, 128, 2, 511, 40, 12, 28, 0, buf_a);
  n = parse_stream(buf_a, len, fr, MAX_FRAMES);
  assert(n == 40);
  check_layout(fr, n, 511, 0);
  check_decode(buf_a, len, fr, n);

  for (i = 0; i < 12; i++)
    if (fr[i].mdb > maxb)
      maxb = fr[i].mdb;
  assert(maxb == 511); /* silence fills the reservoir to its cap */

  for (i = 12; i < 28; i++)
    if (fr[i].mdb < minb_noise)
      minb_noise = fr[i].mdb;
  assert(minb_noise < 511); /* noise actually spends the savings */

  assert(fr[n - 1].mdb == 511); /* trailing silence refills it */
  assert(fr[n - 1].off + fr[n - 1].frame_bytes == (long)len); /* exact tail */
  printf("  layout: charged=%d drained=%d recharged=%d frames=%d\n", maxb,
         minb_noise, fr[n - 1].mdb, n);
  puts("RESERVOIR_CASE layout PASS");
}

/* Output lag bound, whole-frame delivery, both feed entries, two
 * interleaved instances, repeated flush. */
static void case_streaming(void) {
  shine_t s, s1, s2;
  int16_t inter[2 * SHINE_MAX_SAMPLES], l[SHINE_MAX_SAMPLES],
      r[SHINE_MAX_SAMPLES];
  int16_t *planes[2] = {l, r};
  uint32_t rng;
  size_t delivered, total, la, lb;
  int n, f, w;

  /* Hand-run one encoder, checking the backlog bound after every call.
   * 48 kHz / 128 kbps / stereo: 384-byte frames, 348-byte main-data
   * capacity, so at most max_bytes + one frame of capacity may be
   * undelivered: pending*348 <= 511 + 348. */
  s = make_encoder(48000, 128, 2, 511);
  assert(s);
  n = shine_samples_per_pass(s);
  assert(n == 1152);
  rng = 0x12345678u;
  delivered = total = 0;
  for (f = 0; f < 40; f++) {
    unsigned char *p;
    long pending;
    gen_block(&rng, n, 2, f >= 8 && f < 32, l, r, inter);
    w = 0;
    p = shine_encode_buffer_interleaved(s, inter, &w);
    assert(w >= 0 && w % 384 == 0); /* whole physical frames only */
    if (w)
      memcpy(buf_c + total, p, (size_t)w);
    total += (size_t)w;
    delivered += (size_t)w;
    pending = (long)(f + 1) * 384 - (long)delivered;
    assert(pending >= 0 && pending % 384 == 0);
    assert((pending / 384) * 348 <= 511 + 348);
  }
  w = 0;
  {
    unsigned char *p = shine_flush(s, &w);
    assert(w >= 0 && w % 384 == 0);
    if (w)
      memcpy(buf_c + total, p, (size_t)w);
    total += (size_t)w;
  }
  assert(total == 40 * 384); /* everything delivered after flush */
  w = 1;
  shine_flush(s, &w);
  assert(w == 0); /* repeated flush returns nothing */
  shine_close(s);

  /* Same PCM, planar and interleaved entries: identical streams. */
  la = encode_all(48000, 128, 2, 511, 40, 8, 32, 0, buf_a);
  lb = encode_all(48000, 128, 2, 511, 40, 8, 32, 1, buf_b);
  assert(la == lb && !memcmp(buf_a, buf_b, la));
  assert(la == total && !memcmp(buf_a, buf_c, la));

  /* Two instances driven alternately must not share audio or pending
   * output state: each stream equals its solo run. */
  s1 = make_encoder(48000, 128, 2, 511);
  s2 = make_encoder(48000, 128, 2, 511);
  assert(s1 && s2);
  rng = 0x12345678u;
  {
    size_t t1 = 0, t2 = 0;
    for (f = 0; f < 40; f++) {
      unsigned char *p;
      gen_block(&rng, n, 2, f >= 8 && f < 32, l, r, inter);
      w = 0;
      p = shine_encode_buffer_interleaved(s1, inter, &w);
      if (w)
        memcpy(buf_a + t1, p, (size_t)w);
      t1 += (size_t)w;
      w = 0;
      p = shine_encode_buffer(s2, planes, &w);
      if (w)
        memcpy(buf_b + t2, p, (size_t)w);
      t2 += (size_t)w;
    }
    w = 0;
    {
      unsigned char *p = shine_flush(s1, &w);
      if (w)
        memcpy(buf_a + t1, p, (size_t)w);
      t1 += (size_t)w;
    }
    w = 0;
    {
      unsigned char *p = shine_flush(s2, &w);
      if (w)
        memcpy(buf_b + t2, p, (size_t)w);
      t2 += (size_t)w;
    }
    assert(t1 == la && t2 == lb);
    shine_close(s1);
    shine_close(s2);
  }
  printf("  streaming: bytes=%zu lag<=2 frames, dual entry and instances "
         "identical\n",
         total);
  puts("RESERVOIR_CASE streaming PASS");
}

/* MPEG-I padding, MPEG-II/2.5, mono/stereo, illegal capacities, and
 * ancillary filling beyond the part2_3_length range. */
static void case_versions(void) {
  frame_t fr[MAX_FRAMES];
  shine_config_t cfg;
  shine_t s;
  size_t len;
  int n, i, seen_pad0, seen_pad1;

  /* 44.1 kHz stereo: both padding values must occur. */
  len = encode_all(44100, 128, 2, 511, 40, 8, 32, 0, buf_a);
  n = parse_stream(buf_a, len, fr, MAX_FRAMES);
  assert(n == 40);
  check_layout(fr, n, 511, 0);
  check_decode(buf_a, len, fr, n);
  seen_pad0 = seen_pad1 = 0;
  for (i = 0; i < n; i++) {
    assert(fr[i].frame_bytes == 417 + fr[i].padding);
    if (fr[i].padding)
      seen_pad1 = 1;
    else
      seen_pad0 = 1;
  }
  assert(seen_pad0 && seen_pad1);
  printf("  versions: 44.1kHz padding 0 and 1 both present\n");

  /* 24 kHz stereo (MPEG-II) and 8 kHz mono (MPEG-2.5). */
  len = encode_all(24000, 64, 2, 255, 40, 8, 32, 0, buf_a);
  n = parse_stream(buf_a, len, fr, MAX_FRAMES);
  assert(n == 40 && fr[0].version == MPEG_II && fr[0].channels == 2);
  check_layout(fr, n, 255, 0);
  check_decode(buf_a, len, fr, n);

  len = encode_all(8000, 32, 1, 255, 40, 8, 32, 0, buf_a);
  n = parse_stream(buf_a, len, fr, MAX_FRAMES);
  assert(n == 40 && fr[0].version == MPEG_25 && fr[0].channels == 1);
  check_layout(fr, n, 255, 0);
  check_decode(buf_a, len, fr, n);

  /* 32 kHz / 320 kbps mono: a frame's main-data capacity (11352 bits)
   * exceeds what two granules can express (2*4095 bits). The remaining
   * capacity must still be filled legally as ancillary bits. */
  len = encode_all(32000, 320, 1, 511, 40, 8, 32, 0, buf_a);
  n = parse_stream(buf_a, len, fr, MAX_FRAMES);
  assert(n == 40);
  check_layout(fr, n, 511, 1);
  check_decode(buf_a, len, fr, n);
  {
    long long C = 0;
    int drains = 0;
    for (i = 0; i + 1 < n; i++) {
      long L = fr[i].p23[0][0] + fr[i].p23[1][0];
      long long S = C - 8LL * fr[i].mdb;
      long long Sn = C + 8LL * fr[i].cap - 8LL * fr[i + 1].mdb;
      assert(L <= 2 * 4095);
      if (Sn - (S + L) > 0)
        drains++;
      C += 8LL * fr[i].cap;
    }
    assert(drains > 0); /* ancillary filling beyond part2_3 happened */
    printf("  versions: 32kHz/320kbps mono ancillary-fill frames=%d\n",
           drains);
  }

  /* Illegal and boundary reservoir capacities. */
  memset(&cfg, 0, sizeof(cfg));
  shine_set_config_mpeg_defaults(&cfg.mpeg);
  cfg.wave.channels = PCM_STEREO;
  cfg.wave.samplerate = 48000;
  cfg.mpeg.bitr = 128;
  cfg.mpeg.mode = STEREO;
  assert(shine_initialise_reservoir(&cfg, -1) == NULL);
  assert(shine_initialise_reservoir(&cfg, 512) == NULL);
  s = shine_initialise_reservoir(&cfg, 511);
  assert(s);
  shine_close(s);
  s = shine_initialise_reservoir(&cfg, 1);
  assert(s);
  shine_close(s);
  s = shine_initialise_reservoir(&cfg, 0);
  assert(s);
  shine_close(s);

  cfg.wave.samplerate = 24000;
  cfg.mpeg.bitr = 64;
  assert(shine_initialise_reservoir(&cfg, 256) == NULL);
  s = shine_initialise_reservoir(&cfg, 255);
  assert(s);
  shine_close(s);

  cfg.wave.channels = PCM_MONO;
  cfg.wave.samplerate = 8000;
  cfg.mpeg.bitr = 32;
  cfg.mpeg.mode = MONO;
  assert(shine_initialise_reservoir(&cfg, 256) == NULL);
  s = shine_initialise_reservoir(&cfg, 255);
  assert(s);
  shine_close(s);

  cfg.wave.samplerate = 12345; /* unsupported samplerate */
  assert(shine_initialise_reservoir(&cfg, 100) == NULL);

  puts("RESERVOIR_CASE versions PASS");
}

int main(void) {
  case_legacy();
  case_borrow();
  case_layout();
  case_streaming();
  case_versions();
  puts("SHINE_RESERVOIR_OK cases=5");
  return 0;
}
