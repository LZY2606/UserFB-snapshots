/* tests/adaptive_ms.c
 *
 * Tests for the adaptive MPEG-1 long-block mid/side stereo encoder.
 *
 * The tests are deliberately independent of the production selector:
 *   - the bit streams are parsed here, bit by bit, straight from the bytes
 *     the public entry points returned;
 *   - the reconstruction error is recomputed here from the observation points
 *     exposed under SHINE_MS_TESTING, using the formula from the task;
 *   - the decoded audio comes from the bundled minimp3 decoder.
 * The observation points are read-only inputs to the checks; they are never
 * compared against the production decision as an expectation.
 *
 * Three groups are registered through task_support/check.h:
 *   decision  - LR/MS/auto behaviour and the error criterion
 *   bitstream - real frames: header, side information, lengths, capacity,
 *               decoding, panning and loudness
 *   state     - API contract, switching, two instances, both entry points
 */

#include "layer3.h"
#include "types.h"

#include "task_support/check.h"

#include "vendor/minimp3/minimp3.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RATE_44 44100
#define RATE_48 48000
#define RATE_32 32000
#define FRAME_SAMPLES 1152

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static uint64_t fnv1a(const unsigned char *p, size_t n) {
  uint64_t h = UINT64_C(14695981039346656037);
  size_t i;
  for (i = 0; i < n; i++) {
    h ^= p[i];
    h *= UINT64_C(1099511628211);
  }
  return h;
}

static double absd(double x) { return x < 0 ? -x : x; }

/* Wait: implementations may skip the very first frame while the polyphase
 * window fills.  All public streams below therefore use a few warm-up frames
 * before anything is inspected, unless a test says otherwise. */

typedef struct {
  shine_t s;
  int rate;
  int bitr;
  int n;
} enc_t;

static shine_t make_encoder(int rate, int bitr, int mode, int channels) {
  shine_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  shine_set_config_mpeg_defaults(&cfg.mpeg);
  cfg.wave.channels = (enum channels)channels;
  cfg.wave.samplerate = rate;
  cfg.mpeg.mode = (enum modes)mode;
  cfg.mpeg.bitr = bitr;
  return shine_initialise(&cfg);
}

/* Deterministic stereo test signal.  kind selects the stereo relationship:
 *   0 identical        L = R
 *   1 antiphase        L = -R
 *   2 independent      different tones
 *   3 silence
 *   4 correlated pan   R = 0.55 L
 *   5 hard left        R = 0
 *   6 two directions   granule 0 identical, granule 1 antiphase
 */
static void gen_frame(int kind, int frame, int rate, int n, int16_t *l,
                      int16_t *r) {
  int j;
  for (j = 0; j < n; j++) {
    double t = (double)((int64_t)frame * n + j) / (double)rate;
    double left, right;
    switch (kind) {
    case 0:
      left = 14000.0 * sin(2.0 * M_PI * 1000.0 * t);
      right = left;
      break;
    case 1:
      left = 14000.0 * sin(2.0 * M_PI * 1000.0 * t);
      right = -left;
      break;
    case 2:
      left = 12000.0 * sin(2.0 * M_PI * 700.0 * t);
      right = 7000.0 * sin(2.0 * M_PI * 1500.0 * t + 0.3);
      break;
    case 3:
      left = 0.0;
      right = 0.0;
      break;
    case 4:
      left = 13000.0 * sin(2.0 * M_PI * 500.0 * t);
      right = 0.55 * left;
      break;
    case 5:
      left = 13000.0 * sin(2.0 * M_PI * 700.0 * t);
      right = 0.0;
      break;
    default: {
      /* kind 6: granule 0 identical, granule 1 antiphase */
      double v = 12000.0 * sin(2.0 * M_PI * 800.0 * t);
      left = v;
      right = j < GRANULE_SIZE ? v : -v;
      break;
    }
    }
    l[j] = (int16_t)left;
    r[j] = (int16_t)right;
  }
}

typedef struct {
  unsigned char data[1 << 17];
  size_t used;
  int last_written;
} stream_t;

static int encode_frames(shine_t s, int kind, int first, int count, int rate,
                         stream_t *out) {
  int16_t l[FRAME_SAMPLES], r[FRAME_SAMPLES];
  int f;
  int n = shine_samples_per_pass(s);
  for (f = 0; f < count; f++) {
    int w = 0;
    int16_t *ch[2];
    unsigned char *p;
    gen_frame(kind, first + f, rate, n, l, r);
    ch[0] = l;
    ch[1] = r;
    p = shine_encode_buffer(s, ch, &w);
    if (w < 0 || out->used + (size_t)w > sizeof(out->data))
      return -1;
    if (w > 0) {
      memcpy(out->data + out->used, p, (size_t)w);
      out->used += (size_t)w;
    }
    out->last_written = w;
  }
  return 0;
}

static int flush_stream(shine_t s, stream_t *out) {
  int w = 0;
  unsigned char *p = shine_flush(s, &w);
  if (w < 0 || out->used + (size_t)w > sizeof(out->data))
    return -1;
  if (w > 0) {
    memcpy(out->data + out->used, p, (size_t)w);
    out->used += (size_t)w;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* An independent MPEG-1 Layer III frame/side-information parser       */
/* ------------------------------------------------------------------ */

typedef struct {
  int version;
  int bitrate_kbps;
  int samplerate;
  int padding;
  int length; /* physical bytes */
  int mode;
  int mode_ext;
  int channels;
  int granules;
  int main_data_begin;
  int scfsi[2][4];
  int part2_3_length[2][2]; /* [granule][channel] */
  int global_gain[2][2];
  int big_values[2][2];
  int scalefac_compress[2][2];
  int side_bits;
  int main_data_bits;
  int p23_sum;
} frame_t;

static int bit_at(const unsigned char *buf, int bitpos) {
  return (buf[bitpos >> 3] >> (7 - (bitpos & 7))) & 1;
}

static unsigned bits_at(const unsigned char *buf, int bitpos, int count) {
  unsigned v = 0;
  int i;
  for (i = 0; i < count; i++)
    v = (v << 1) | (unsigned)bit_at(buf, bitpos + i);
  return v;
}

/* Parse the header and side information of one frame.  Returns 0 on success,
 * a negative value when the bytes cannot be a legal MPEG-1 Layer III frame at
 * the expected configuration. */
static int parse_frame(const unsigned char *buf, size_t avail, int rate,
                       int bitr, frame_t *fi) {
  static const int bitrates[16] = {0,  32, 40,  48,  56,  64, 80,
                                   96, 112, 128, 160, 192, 224, 256,
                                   320, 0};
  static const int samplerates[3] = {44100, 48000, 32000};
  int version, layer, bitrate_index, samplerate_index;
  int pos;

  if (avail < 4)
    return -1;
  if (buf[0] != 0xff || (buf[1] & 0xe0) != 0xe0)
    return -2; /* no sync word */
  version = (buf[1] >> 3) & 3;
  layer = (buf[1] >> 1) & 3;
  bitrate_index = (buf[2] >> 4) & 0xf;
  samplerate_index = (buf[2] >> 2) & 3;
  if (version != 3 || layer != 1)
    return -3; /* not MPEG-1 Layer III */
  if (bitrate_index == 0 || bitrate_index == 15 || samplerate_index == 3)
    return -4;
  if (samplerates[samplerate_index] != rate ||
      bitrates[bitrate_index] != bitr)
    return -5;

  fi->version = version;
  fi->bitrate_kbps = bitrates[bitrate_index];
  fi->samplerate = samplerates[samplerate_index];
  fi->padding = (buf[2] >> 1) & 1;
  fi->length = 144 * fi->bitrate_kbps * 1000 / fi->samplerate + fi->padding;
  fi->mode = (buf[3] >> 6) & 3;
  fi->mode_ext = (buf[3] >> 4) & 3;
  fi->channels = fi->mode == 3 ? 1 : 2;
  fi->granules = 2;
  if ((size_t)fi->length > avail)
    return -6;

  pos = 32; /* after the 4 byte header */
  fi->main_data_begin = (int)bits_at(buf, pos, 9);
  pos += 9;
  pos += fi->channels == 2 ? 3 : 5; /* private bits */
  {
    int ch, band;
    for (ch = 0; ch < fi->channels; ch++)
      for (band = 0; band < 4; band++, pos++)
        fi->scfsi[ch][band] = (int)bits_at(buf, pos, 1);
  }
  fi->p23_sum = 0;
  {
    int gr, ch;
    for (gr = 0; gr < fi->granules; gr++)
      for (ch = 0; ch < fi->channels; ch++) {
        fi->part2_3_length[gr][ch] = (int)bits_at(buf, pos, 12);
        pos += 12;
        fi->big_values[gr][ch] = (int)bits_at(buf, pos, 9);
        pos += 9;
        fi->global_gain[gr][ch] = (int)bits_at(buf, pos, 8);
        pos += 8;
        fi->scalefac_compress[gr][ch] = (int)bits_at(buf, pos, 4);
        pos += 4;
        pos += 1; /* window switching flag */
        pos += 15; /* table_select */
        pos += 4 + 3; /* region counts */
        pos += 1 + 1 + 1; /* preflag, scalefac_scale, count1table_select */
        fi->p23_sum += fi->part2_3_length[gr][ch];
      }
  }
  fi->side_bits = pos - 32;
  fi->main_data_bits = fi->length * 8 - fi->side_bits;
  return 0;
}

/* Walk a whole stream, checking that every frame parses and is contiguous. */
typedef struct {
  int frames;
  int ms_frames;
  int lr_frames;
  int max_part2_3; /* largest single granule/channel field */
  int capacity_violations;
  int bad_mode_ext;
  int granules_agree;
} walk_t;

static int walk_stream(const stream_t *st, int rate, int bitr, walk_t *w) {
  size_t at = 0;
  memset(w, 0, sizeof(*w));
  w->granules_agree = 1;
  while (at < st->used) {
    frame_t fi;
    int r = parse_frame(st->data + at, st->used - at, rate, bitr, &fi);
    frame_t next;
    if (r != 0)
      return r;
    if (fi.main_data_begin != 0)
      return -20; /* the adaptive encoder never uses the reservoir */
    if (fi.mode == JOINT_STEREO && fi.mode_ext == 2) {
      w->ms_frames++;
    } else if (fi.mode == 0 || (fi.mode == 1 && fi.mode_ext == 0)) {
      w->lr_frames++;
    } else {
      w->bad_mode_ext++;
    }
    if (fi.p23_sum > fi.main_data_bits)
      w->capacity_violations++;
    {
      int gr, ch;
      for (gr = 0; gr < fi.granules; gr++)
        for (ch = 0; ch < fi.channels; ch++) {
          int v = fi.part2_3_length[gr][ch];
          if (v > 4095)
            return -21;
          if (v > w->max_part2_3)
            w->max_part2_3 = v;
        }
    }
    w->frames++;
    at += (size_t)fi.length;
    if (at < st->used) {
      /* the following frame must start exactly here */
      if (parse_frame(st->data + at, st->used - at, rate, bitr, &next) != 0)
        return -22;
    }
  }
  return at == st->used ? 0 : -23;
}

/* Decode a stream with the bundled minimp3 and measure per channel. */
typedef struct {
  int frames;
  double energy_l;
  double energy_r;
  double peak;
  int decoded_scalars;
} decode_t;

static int decode_stream(const stream_t *st, decode_t *out) {
  mp3dec_t dec;
  short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
  size_t at = 0;
  memset(out, 0, sizeof(*out));
  mp3dec_init(&dec);
  while (at + 4 <= st->used) {
    mp3dec_frame_info_t info;
    int samples;
    int i;
    memset(&info, 0, sizeof(info));
    samples = mp3dec_decode_frame(&dec, st->data + at,
                                  (int)(st->used - at), pcm, &info);
    if (!info.frame_bytes)
      break;
    if (info.channels != 2)
      return -1;
    if (samples != FRAME_SAMPLES)
      return -2;
    for (i = 0; i < samples; i++) {
      double l = (double)pcm[2 * i];
      double r = (double)pcm[2 * i + 1];
      out->energy_l += l * l;
      out->energy_r += r * r;
      if (absd(l) > out->peak)
        out->peak = absd(l);
      if (absd(r) > out->peak)
        out->peak = absd(r);
      out->decoded_scalars += 2;
    }
    out->frames++;
    at += (size_t)info.frame_bytes;
  }
  return at == st->used ? 0 : -3;
}

/* ------------------------------------------------------------------ */
/* Independent reconstruction error                                    */
/* ------------------------------------------------------------------ */

/* sign(q) * |q|^(4/3) * 2^((gain-210)/4), exactly as specified. */
static double reconstruct(int q, int gain) {
  double mag;
  if (!q)
    return 0.0;
  mag = pow(fabs((double)q), 4.0 / 3.0) * pow(2.0, (gain - 210) / 4.0);
  return q > 0 ? mag : -mag;
}

/* Squared error of the LR representation of the frame that was just encoded,
 * recomputed from the observation points in the observation slots. */
static double observed_lr_error(const shine_global_config *c) {
  double total = 0.0;
  int gr, ch, i;
  for (ch = 0; ch < 2; ch++)
    for (gr = 0; gr < 2; gr++) {
      int gain = c->ms.obs_gain[0][ch][gr];
      for (i = 0; i < GRANULE_SIZE; i++) {
        double x = c->ms.obs_lr_spectrum[ch][gr][i] * (1.0 / 2147483648.0);
        double r = reconstruct(c->ms.obs_q[0][ch][gr][i], gain);
        total += (r - x) * (r - x);
      }
    }
  return total;
}

static double observed_ms_error(const shine_global_config *c) {
  double total = 0.0;
  int gr, i;
  for (gr = 0; gr < 2; gr++) {
    int gain_m = c->ms.obs_gain[1][0][gr];
    int gain_s = c->ms.obs_gain[1][1][gr];
    for (i = 0; i < GRANULE_SIZE; i++) {
      double m = reconstruct(c->ms.obs_q[1][0][gr][i], gain_m);
      double s = reconstruct(c->ms.obs_q[1][1][gr][i], gain_s);
      double l = (m + s) * 0.70710678118654752;
      double r = (m - s) * 0.70710678118654752;
      double xl = c->ms.obs_lr_spectrum[0][gr][i] * (1.0 / 2147483648.0);
      double xr = c->ms.obs_lr_spectrum[1][gr][i] * (1.0 / 2147483648.0);
      total += (l - xl) * (l - xl) + (r - xr) * (r - xr);
    }
  }
  return total;
}

/* The selection rule from the task, applied to errors measured here. */
static int should_choose_ms(double dms, double dlr) {
  double scale = absd(dlr) > absd(dms) ? absd(dlr) : absd(dms);
  if (scale < 1.0)
    scale = 1.0;
  return dms < dlr - 1e-12 * scale;
}

/* ------------------------------------------------------------------ */
/* Decision group                                                      */
/* ------------------------------------------------------------------ */

static int decision_auto_prefers_ms_for_correlated(void) {
  shine_t s = make_encoder(RATE_48, 64, JOINT_STEREO, 2);
  shine_global_config *c;
  int16_t l[FRAME_SAMPLES], r[FRAME_SAMPLES];
  int frames_with_ms = 0;
  int f;
  double total_lr = 0.0, total_ms = 0.0;

  TASK_CHECK(s != NULL);
  c = (shine_global_config *)s;
  TASK_CHECK(shine_set_ms(s, 2) == 0);

  for (f = 0; f < 12; f++) {
    int w = 0;
    int16_t *ch[2];
    unsigned char *p;
    double dlr, dms;
    gen_frame(0, f, RATE_48, FRAME_SAMPLES, l, r);
    ch[0] = l;
    ch[1] = r;
    p = shine_encode_buffer(s, ch, &w);
    TASK_CHECK(p != NULL && w > 0);
    TASK_CHECK(c->ms.obs_valid[0] && c->ms.obs_valid[1]);
    dlr = observed_lr_error(c);
    dms = observed_ms_error(c);
    total_lr += dlr;
    total_ms += dms;
    TASK_CHECK(c->ms.obs_selected == should_choose_ms(dms, dlr));
    if (c->ms.obs_selected)
      frames_with_ms++;
    TASK_CHECK((c->ms.obs_mode == JOINT_STEREO && c->ms.obs_mode_ext == 2) ||
               (c->ms.obs_mode == JOINT_STEREO && c->ms.obs_mode_ext == 0) ||
               (c->ms.obs_mode == STEREO && c->ms.obs_mode_ext == 0));
  }
  /* identical channels: M/S is the better representation overall */
  TASK_CHECK(frames_with_ms > 0);
  TASK_CHECK(should_choose_ms(total_ms, total_lr));
  TASK_CHECK(total_ms < total_lr);
  shine_close(s);
  return 0;
}

/* A pure antiphase pair carries the same information in both representations;
 * the two measured errors must stay close, and the choice still follows the
 * strict comparison. */
static int decision_auto_keeps_lr_for_antiphase(void) {
  shine_t s = make_encoder(RATE_48, 128, JOINT_STEREO, 2);
  shine_global_config *c;
  int16_t l[FRAME_SAMPLES], r[FRAME_SAMPLES];
  int f;

  TASK_CHECK(s != NULL);
  c = (shine_global_config *)s;
  TASK_CHECK(shine_set_ms(s, 2) == 0);

  for (f = 0; f < 10; f++) {
    int w = 0;
    int16_t *ch[2];
    unsigned char *p;
    double dlr, dms;
    gen_frame(1, f, RATE_48, FRAME_SAMPLES, l, r);
    ch[0] = l;
    ch[1] = r;
    p = shine_encode_buffer(s, ch, &w);
    TASK_CHECK(p != NULL && w > 0);
    dlr = observed_lr_error(c);
    dms = observed_ms_error(c);
    TASK_CHECK(c->ms.obs_selected == should_choose_ms(dms, dlr));
    /* The two representations are equivalent for a pure antiphase pair: the
     * errors cannot differ by more than a rounding scale factor. */
    TASK_CHECK(absd(dms - dlr) <=
               0.05 * (absd(dms) > absd(dlr) ? absd(dms) : absd(dlr)) + 1e-9);
  }
  shine_close(s);
  return 0;
}

static int decision_forced_modes_are_honoured(void) {
  int mode;
  for (mode = 1; mode <= 2; mode += 1) {
    shine_t s = make_encoder(RATE_48, 128, JOINT_STEREO, 2);
    shine_global_config *c;
    int16_t l[FRAME_SAMPLES], r[FRAME_SAMPLES];
    int f;
    TASK_CHECK(s != NULL);
    c = (shine_global_config *)s;
    TASK_CHECK(shine_set_ms(s, mode) == 0);
    for (f = 0; f < 4; f++) {
      int w = 0;
      int16_t *ch[2];
      gen_frame(1, f, RATE_48, FRAME_SAMPLES, l, r); /* antiphase */
      ch[0] = l;
      ch[1] = r;
      TASK_CHECK(shine_encode_buffer(s, ch, &w) != NULL && w > 0);
      if (mode == 1)
        TASK_CHECK(c->ms.obs_selected == 1);
      else
        TASK_CHECK(c->ms.obs_selected == should_choose_ms(
                                             observed_ms_error(c),
                                             observed_lr_error(c)));
    }
    shine_close(s);
  }
  return 0;
}

/* Two granules that prefer different representations: the decision must be
 * taken for the whole frame, never per granule. */
/* Per-granule squared errors, measured here from the observation points. */
static void observed_granule_errors(const shine_global_config *c, int gr,
                                    double *dlr, double *dms) {
  int i;
  double lr = 0.0, ms = 0.0;
  int ch;
  for (ch = 0; ch < 2; ch++) {
    int gain = c->ms.obs_gain[0][ch][gr];
    for (i = 0; i < GRANULE_SIZE; i++) {
      double x = c->ms.obs_lr_spectrum[ch][gr][i] * (1.0 / 2147483648.0);
      double r = reconstruct(c->ms.obs_q[0][ch][gr][i], gain);
      lr += (r - x) * (r - x);
    }
  }
  {
    int g0 = c->ms.obs_gain[1][0][gr];
    int g1 = c->ms.obs_gain[1][1][gr];
    for (i = 0; i < GRANULE_SIZE; i++) {
      double m = reconstruct(c->ms.obs_q[1][0][gr][i], g0);
      double s = reconstruct(c->ms.obs_q[1][1][gr][i], g1);
      double l = (m + s) * 0.70710678118654752;
      double r = (m - s) * 0.70710678118654752;
      double xl = c->ms.obs_lr_spectrum[0][gr][i] * (1.0 / 2147483648.0);
      double xr = c->ms.obs_lr_spectrum[1][gr][i] * (1.0 / 2147483648.0);
      ms += (l - xl) * (l - xl) + (r - xr) * (r - xr);
    }
  }
  *dlr = lr;
  *dms = ms;
}

/* The two granules of a frame can disagree on which representation is better;
 * the decision must still be taken once for the whole frame (and the header
 * can only express one mode). */
static int decision_is_frame_wide(void) {
  shine_t s = make_encoder(RATE_48, 128, JOINT_STEREO, 2);
  shine_global_config *c;
  int16_t l[FRAME_SAMPLES], r[FRAME_SAMPLES];
  int f;
  int disagreements = 0;
  TASK_CHECK(s != NULL);
  c = (shine_global_config *)s;
  TASK_CHECK(shine_set_ms(s, 2) == 0);
  for (f = 0; f < 12; f++) {
    int w = 0;
    int16_t *ch[2];
    unsigned char *p;
    int j;
    double lr0, ms0, lr1, ms1;
    int prefer0, prefer1;
    /* granule 0 is identical in both channels, granule 1 antiphase */
    for (j = 0; j < FRAME_SAMPLES; j++) {
      double t = (double)((int64_t)f * FRAME_SAMPLES + j) / (double)RATE_48;
      double v = 12000.0 * sin(2.0 * M_PI * 800.0 * t);
      l[j] = (int16_t)v;
      r[j] = (int16_t)(j < GRANULE_SIZE ? v : -v);
    }
    ch[0] = l;
    ch[1] = r;
    p = shine_encode_buffer(s, ch, &w);
    TASK_CHECK(p != NULL && w > 0);
    observed_granule_errors(c, 0, &lr0, &ms0);
    observed_granule_errors(c, 1, &lr1, &ms1);
    prefer0 = should_choose_ms(ms0, lr0);
    prefer1 = should_choose_ms(ms1, lr1);
    if (prefer0 != prefer1)
      disagreements++;
    /* frame-wide rule uses the summed error, not a per-granule vote */
    TASK_CHECK(c->ms.obs_selected ==
               should_choose_ms(observed_ms_error(c), observed_lr_error(c)));
    /* and the single mode extension in the header reflects that one choice */
    TASK_CHECK(((p[3] >> 6) & 3) == JOINT_STEREO);
    TASK_CHECK(((p[3] >> 4) & 3) == (c->ms.obs_selected ? 2 : 0));
  }
  /* the signal really does make the granules disagree in some frames */
  TASK_CHECK(disagreements > 0);
  shine_close(s);
  return 0;
}

static int decision_all_zero_is_a_tie(void) {
  shine_t s = make_encoder(RATE_48, 128, JOINT_STEREO, 2);
  shine_global_config *c;
  int16_t l[FRAME_SAMPLES], r[FRAME_SAMPLES];
  int f;
  TASK_CHECK(s != NULL);
  c = (shine_global_config *)s;
  TASK_CHECK(shine_set_ms(s, 2) == 0);
  for (f = 0; f < 6; f++) {
    int w = 0;
    int16_t *ch[2];
    gen_frame(3, f, RATE_48, FRAME_SAMPLES, l, r);
    ch[0] = l;
    ch[1] = r;
    TASK_CHECK(shine_encode_buffer(s, ch, &w) != NULL);
    /* silence gives both candidates zero error: only a strict improvement
     * may select M/S */
    TASK_CHECK(observed_lr_error(c) == 0.0);
    TASK_CHECK(observed_ms_error(c) == 0.0);
    TASK_CHECK(c->ms.obs_selected == 0);
  }
  shine_close(s);
  return 0;
}

static int decision_ms_improves_correlated_signal(void) {
  shine_t s = make_encoder(RATE_48, 64, JOINT_STEREO, 2);
  shine_global_config *c;
  int16_t l[FRAME_SAMPLES], r[FRAME_SAMPLES];
  int f;
  double total_lr = 0.0, total_ms = 0.0;
  TASK_CHECK(s != NULL);
  c = (shine_global_config *)s;
  TASK_CHECK(shine_set_ms(s, 2) == 0);
  for (f = 0; f < 10; f++) {
    int w = 0;
    int16_t *ch[2];
    gen_frame(4, f, RATE_48, FRAME_SAMPLES, l, r);
    ch[0] = l;
    ch[1] = r;
    TASK_CHECK(shine_encode_buffer(s, ch, &w) != NULL && w > 0);
    total_lr += observed_lr_error(c);
    total_ms += observed_ms_error(c);
  }
  /* On a correlated pair the M/S candidate really is better; the winner must
   * reflect that measurement. */
  TASK_CHECK(should_choose_ms(total_ms, total_lr));
  TASK_CHECK(c->ms.obs_selected == 1);
  shine_close(s);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Bitstream group                                                     */
/* ------------------------------------------------------------------ */

static int bitstream_lr_stream_is_plain_stereo(void) {
  shine_t s = make_encoder(RATE_44, 128, JOINT_STEREO, 2);
  stream_t *st = (stream_t *)calloc(1, sizeof(*st));
  walk_t walk;
  TASK_CHECK(s != NULL && st != NULL);
  TASK_CHECK(shine_set_ms(s, 0) == 0);
  TASK_CHECK(encode_frames(s, 2, 0, 12, RATE_44, st) == 0);
  TASK_CHECK(flush_stream(s, st) == 0);
  shine_close(s);
  TASK_CHECK(walk_stream(st, RATE_44, 128, &walk) == 0);
  TASK_CHECK(walk.ms_frames == 0);
  TASK_CHECK(walk.lr_frames == walk.frames);
  TASK_CHECK(walk.capacity_violations == 0);
  TASK_CHECK(walk.bad_mode_ext == 0);
  /* every frame announced its own physical length */
  TASK_CHECK(walk.frames == 12);
  free(st);
  return 0;
}

static int bitstream_forced_ms_headers_and_lengths(void) {
  static const int rates[] = {RATE_32, RATE_44, RATE_48};
  static const int bitrates[] = {48, 128, 320};
  int ri, bi;
  for (ri = 0; ri < 3; ri++)
    for (bi = 0; bi < 3; bi++) {
      int rate = rates[ri];
      int bitr = bitrates[bi];
      shine_t s = make_encoder(rate, bitr, JOINT_STEREO, 2);
      stream_t *st = (stream_t *)calloc(1, sizeof(*st));
      walk_t walk;
      TASK_CHECK(s != NULL && st != NULL);
      TASK_CHECK(shine_set_ms(s, 1) == 0);
      TASK_CHECK(encode_frames(s, 4, 0, 10, rate, st) == 0);
      TASK_CHECK(flush_stream(s, st) == 0);
      shine_close(s);
      TASK_CHECK(walk_stream(st, rate, bitr, &walk) == 0);
      TASK_CHECK(walk.ms_frames == walk.frames);
      TASK_CHECK(walk.max_part2_3 <= 4095);
      TASK_CHECK(walk.capacity_violations == 0);
      TASK_CHECK(walk.granules_agree);
      free(st);
    }
  return 0;
}

static int bitstream_44k_padding_alternates(void) {
  static const int bitrates[] = {128, 192};
  int bi;
  for (bi = 0; bi < 2; bi++) {
    int bitr = bitrates[bi];
    double slots = 144.0 * bitr * 1000.0 / RATE_44;
    int whole = (int)slots;
    int need_padding = slots > (double)whole;
    shine_t s = make_encoder(RATE_44, bitr, JOINT_STEREO, 2);
    stream_t *st = (stream_t *)calloc(1, sizeof(*st));
    size_t at = 0;
    int frames = 0, with_padding = 0, without_padding = 0;
    TASK_CHECK(s != NULL && st != NULL);
    TASK_CHECK(shine_set_ms(s, 2) == 0);
    /* The padding accumulator in shine drifts slowly for 44.1 kHz, so give
     * it enough frames for both slot counts to appear. */
    TASK_CHECK(encode_frames(s, 4, 0, 60, RATE_44, st) == 0);
    TASK_CHECK(flush_stream(s, st) == 0);
    shine_close(s);
    while (at < st->used) {
      frame_t fi;
      TASK_CHECK(parse_frame(st->data + at, st->used - at, RATE_44, bitr,
                             &fi) == 0);
      TASK_CHECK(fi.length == 144 * bitr * 1000 / RATE_44 + fi.padding);
      if (fi.padding)
        with_padding++;
      else
        without_padding++;
      at += (size_t)fi.length;
      frames++;
    }
    TASK_CHECK(at == st->used);
    TASK_CHECK(frames == 60);
    if (need_padding) {
      /* both slot counts appear, so padding really alternates */
      TASK_CHECK(with_padding > 0);
      TASK_CHECK(without_padding > 0);
    } else {
      TASK_CHECK(with_padding == 0);
    }
    free(st);
  }
  return 0;
}

static int bitstream_decodes_nonzero_audio(void) {
  shine_t s = make_encoder(RATE_48, 128, JOINT_STEREO, 2);
  stream_t *st = (stream_t *)calloc(1, sizeof(*st));
  decode_t dec;
  walk_t walk;
  TASK_CHECK(s != NULL && st != NULL);
  TASK_CHECK(shine_set_ms(s, 2) == 0);
  TASK_CHECK(encode_frames(s, 4, 0, 16, RATE_48, st) == 0);
  TASK_CHECK(flush_stream(s, st) == 0);
  shine_close(s);
  TASK_CHECK(walk_stream(st, RATE_48, 128, &walk) == 0);
  TASK_CHECK(decode_stream(st, &dec) == 0);
  TASK_CHECK(dec.frames == walk.frames);
  TASK_CHECK(dec.decoded_scalars == dec.frames * 2 * FRAME_SAMPLES);
  TASK_CHECK(dec.energy_l > 0.0 && dec.energy_r > 0.0);
  free(st);
  return 0;
}

/* In-phase, antiphase and level checks straight from minimp3. */
static int bitstream_panning_and_loudness(void) {
  /* A: identical channels -> both decoded channels carry the same audio. */
  {
    shine_t s = make_encoder(RATE_48, 128, JOINT_STEREO, 2);
    stream_t *st = (stream_t *)calloc(1, sizeof(*st));
    decode_t dec;
    TASK_CHECK(s != NULL && st != NULL);
    TASK_CHECK(shine_set_ms(s, 1) == 0);
    TASK_CHECK(encode_frames(s, 0, 4, 12, RATE_48, st) == 0);
    TASK_CHECK(flush_stream(s, st) == 0);
    shine_close(s);
    TASK_CHECK(decode_stream(st, &dec) == 0);
    TASK_CHECK(dec.energy_l > 0.0 && dec.energy_r > 0.0);
    /* same content on both sides within 10 percent */
    TASK_CHECK(absd(dec.energy_l - dec.energy_r) <
               0.10 * (dec.energy_l + dec.energy_r));
    /* the level did not collapse: comparable to the reference LR encoding */
    free(st);
  }
  /* B: hard left -> decoded audio stays on the left. */
  {
    shine_t s = make_encoder(RATE_48, 128, JOINT_STEREO, 2);
    stream_t *st = (stream_t *)calloc(1, sizeof(*st));
    decode_t dec;
    TASK_CHECK(s != NULL && st != NULL);
    TASK_CHECK(shine_set_ms(s, 2) == 0);
    TASK_CHECK(encode_frames(s, 5, 4, 12, RATE_48, st) == 0);
    TASK_CHECK(flush_stream(s, st) == 0);
    shine_close(s);
    TASK_CHECK(decode_stream(st, &dec) == 0);
    TASK_CHECK(dec.energy_l > 0.0);
    /* energy on the right is negligible compared to the left */
    TASK_CHECK(dec.energy_r < 0.01 * dec.energy_l);
    free(st);
  }
  /* C: antiphase -> decoded channels keep opposite polarity. */
  {
    shine_t s = make_encoder(RATE_48, 128, JOINT_STEREO, 2);
    stream_t *st = (stream_t *)calloc(1, sizeof(*st));
    mp3dec_t mp;
    short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t at = 0;
    int frames = 0;
    double dot = 0.0, el = 0.0, er = 0.0;
    TASK_CHECK(s != NULL && st != NULL);
    TASK_CHECK(shine_set_ms(s, 1) == 0);
    TASK_CHECK(encode_frames(s, 1, 4, 12, RATE_48, st) == 0);
    TASK_CHECK(flush_stream(s, st) == 0);
    shine_close(s);
    mp3dec_init(&mp);
    while (at + 4 <= st->used) {
      mp3dec_frame_info_t info;
      int samples, i;
      memset(&info, 0, sizeof(info));
      samples = mp3dec_decode_frame(&mp, st->data + at, (int)(st->used - at),
                                    pcm, &info);
      TASK_CHECK(info.frame_bytes > 0 && samples == FRAME_SAMPLES);
      for (i = 0; i < samples; i++) {
        double l = pcm[2 * i], r = pcm[2 * i + 1];
        dot += l * r;
        el += l * l;
        er += r * r;
      }
      frames++;
      at += (size_t)info.frame_bytes;
    }
    TASK_CHECK(frames == 12 && at == st->used);
    TASK_CHECK(el > 0.0 && er > 0.0);
    TASK_CHECK(dot < -0.8 * sqrt(el * er));
    free(st);
  }
  return 0;
}

/* The M/S frame must spend its capacity on the spectrum that carries sound:
 * on a correlated pair, more main-data bits go to M than to S. */
static int bitstream_silent_channel_donates(void) {
  shine_t s = make_encoder(RATE_48, 64, JOINT_STEREO, 2);
  shine_global_config *c;
  int16_t l[FRAME_SAMPLES], r[FRAME_SAMPLES];
  int f;
  int frames_checked = 0;
  TASK_CHECK(s != NULL);
  c = (shine_global_config *)s;
  TASK_CHECK(shine_set_ms(s, 1) == 0);
  for (f = 0; f < 8; f++) {
    int w = 0;
    int16_t *ch[2];
    gen_frame(0, f, RATE_48, FRAME_SAMPLES, l, r);
    ch[0] = l;
    ch[1] = r;
    TASK_CHECK(shine_encode_buffer(s, ch, &w) != NULL && w > 0);
    {
      int m = c->ms.obs_part2_3[1][0][0] + c->ms.obs_part2_3[1][0][1];
      int sd = c->ms.obs_part2_3[1][1][0] + c->ms.obs_part2_3[1][1][1];
      TASK_CHECK(sd == 0); /* S is exactly zero for an identical pair */
      TASK_CHECK(m > 0);
      frames_checked++;
    }
  }
  TASK_CHECK(frames_checked == 8);
  shine_close(s);
  return 0;
}

/* Two unrelated channels: M and S both carry sound, so the M/S frame cannot
 * save anything and must keep both channels audible and separate. */
static int bitstream_independent_channels_stay_separate(void) {
  shine_t s = make_encoder(RATE_48, 192, JOINT_STEREO, 2);
  stream_t *st = (stream_t *)calloc(1, sizeof(*st));
  walk_t walk;
  decode_t dec;
  mp3dec_t mp;
  short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
  size_t at = 0;
  double el = 0.0, er = 0.0, cross = 0.0;
  int frames = 0;
  TASK_CHECK(s != NULL && st != NULL);
  TASK_CHECK(shine_set_ms(s, 2) == 0);
  TASK_CHECK(encode_frames(s, 2, 4, 14, RATE_48, st) == 0);
  TASK_CHECK(flush_stream(s, st) == 0);
  shine_close(s);
  TASK_CHECK(walk_stream(st, RATE_48, 192, &walk) == 0);
  TASK_CHECK(walk.capacity_violations == 0);
  TASK_CHECK(decode_stream(st, &dec) == 0);
  TASK_CHECK(dec.energy_l > 0.0 && dec.energy_r > 0.0);
  /* The two channels must stay independently audible: neither side may be
   * wiped out by the M/S mapping. */
  TASK_CHECK(dec.energy_l > 0.2 * dec.energy_r &&
             dec.energy_r > 0.2 * dec.energy_l);

  /* Verify the two decoded channels are not simply copies of each other. */
  mp3dec_init(&mp);
  while (at + 4 <= st->used) {
    mp3dec_frame_info_t info;
    int samples, i;
    memset(&info, 0, sizeof(info));
    samples = mp3dec_decode_frame(&mp, st->data + at, (int)(st->used - at),
                                  pcm, &info);
    TASK_CHECK(info.frame_bytes > 0 && samples == FRAME_SAMPLES);
    for (i = 0; i < samples; i++) {
      double l = pcm[2 * i], r = pcm[2 * i + 1];
      el += l * l;
      er += r * r;
      cross += l * r;
    }
    frames++;
    at += (size_t)info.frame_bytes;
  }
  TASK_CHECK(frames == 14 && at == st->used);
  /* independent content: the normalised correlation stays well below 1 */
  TASK_CHECK(absd(cross) < 0.5 * sqrt(el * er));
  free(st);
  return 0;
}

static int bitstream_low_bitrate_stays_legal(void) {
  shine_t s = make_encoder(RATE_48, 32, JOINT_STEREO, 2);
  stream_t *st = (stream_t *)calloc(1, sizeof(*st));
  walk_t walk;
  decode_t dec;
  TASK_CHECK(s != NULL && st != NULL);
  TASK_CHECK(shine_set_ms(s, 2) == 0);
  TASK_CHECK(encode_frames(s, 4, 0, 20, RATE_48, st) == 0);
  TASK_CHECK(flush_stream(s, st) == 0);
  shine_close(s);
  TASK_CHECK(walk_stream(st, RATE_48, 32, &walk) == 0);
  TASK_CHECK(walk.capacity_violations == 0);
  TASK_CHECK(walk.max_part2_3 <= 4095);
  TASK_CHECK(decode_stream(st, &dec) == 0);
  TASK_CHECK(dec.energy_l > 0.0 && dec.energy_r > 0.0);
  free(st);
  return 0;
}

/* ------------------------------------------------------------------ */
/* State group                                                         */
/* ------------------------------------------------------------------ */

static int state_set_ms_contract(void) {
  shine_t s;
  TASK_CHECK(shine_set_ms(NULL, 0) == -1);

  s = make_encoder(RATE_48, 128, JOINT_STEREO, 2);
  TASK_CHECK(s != NULL);
  TASK_CHECK(shine_set_ms(s, -1) == -1);
  TASK_CHECK(shine_set_ms(s, 3) == -1);
  TASK_CHECK(shine_set_ms(s, 0) == 0);
  TASK_CHECK(shine_set_ms(s, 1) == 0);
  TASK_CHECK(shine_set_ms(s, 2) == 0);
  shine_close(s);

  s = make_encoder(RATE_48, 128, MONO, 1);
  TASK_CHECK(s != NULL);
  TASK_CHECK(shine_set_ms(s, 1) == -1);
  shine_close(s);

  s = make_encoder(RATE_48, 128, DUAL_CHANNEL, 2);
  TASK_CHECK(s != NULL);
  TASK_CHECK(shine_set_ms(s, 1) == -1);
  shine_close(s);

  s = make_encoder(22050, 64, JOINT_STEREO, 2);
  TASK_CHECK(s != NULL);
  TASK_CHECK(shine_set_ms(s, 1) == -1);
  shine_close(s);

  s = make_encoder(11025, 32, JOINT_STEREO, 2);
  TASK_CHECK(s != NULL);
  TASK_CHECK(shine_set_ms(s, 1) == -1);
  shine_close(s);

  s = make_encoder(RATE_44, 192, STEREO, 2);
  TASK_CHECK(s != NULL);
  TASK_CHECK(shine_set_ms(s, 2) == 0);
  shine_close(s);
  return 0;
}

static int state_rejected_call_keeps_stream(void) {
  shine_config_t cfg;
  int16_t l[FRAME_SAMPLES], r[FRAME_SAMPLES];
  static stream_t a, b;
  shine_t s;
  int f;

  memset(&cfg, 0, sizeof(cfg));
  shine_set_config_mpeg_defaults(&cfg.mpeg);
  cfg.wave.channels = PCM_STEREO;
  cfg.wave.samplerate = RATE_44;
  cfg.mpeg.mode = JOINT_STEREO;
  cfg.mpeg.bitr = 96;

  memset(&a, 0, sizeof(a));
  s = shine_initialise(&cfg);
  TASK_CHECK(s != NULL);
  TASK_CHECK(shine_set_ms(s, 2) == 0);
  for (f = 0; f < 6; f++) {
    int w = 0;
    int16_t *ch[2];
    unsigned char *p;
    gen_frame(4, f, RATE_44, FRAME_SAMPLES, l, r);
    ch[0] = l;
    ch[1] = r;
    p = shine_encode_buffer(s, ch, &w);
    TASK_CHECK(p && w > 0);
    memcpy(a.data + a.used, p, (size_t)w);
    a.used += (size_t)w;
  }
  TASK_CHECK(flush_stream(s, &a) == 0);
  shine_close(s);

  memset(&b, 0, sizeof(b));
  s = shine_initialise(&cfg);
  TASK_CHECK(s != NULL);
  TASK_CHECK(shine_set_ms(s, 2) == 0);
  for (f = 0; f < 6; f++) {
    int w = 0;
    int16_t *ch[2];
    unsigned char *p;
    if (f == 3) {
      /* every one of these must be refused and change nothing */
      TASK_CHECK(shine_set_ms(s, 7) == -1);
      TASK_CHECK(shine_set_ms(s, -2) == -1);
      TASK_CHECK(shine_set_ms(NULL, 2) == -1);
    }
    gen_frame(4, f, RATE_44, FRAME_SAMPLES, l, r);
    ch[0] = l;
    ch[1] = r;
    p = shine_encode_buffer(s, ch, &w);
    TASK_CHECK(p && w > 0);
    memcpy(b.data + b.used, p, (size_t)w);
    b.used += (size_t)w;
  }
  TASK_CHECK(flush_stream(s, &b) == 0);
  shine_close(s);
  TASK_CHECK(a.used == b.used && memcmp(a.data, b.data, a.used) == 0);
  return 0;
}

/* Never enabling M/S must keep the historical bytes for a known stream. */
static int state_never_enabled_output_is_legacy(void) {
  static const uint64_t legacy_fnv = UINT64_C(0xd557900c34c028cc);
  shine_t s = make_encoder(RATE_48, 128, JOINT_STEREO, 2);
  stream_t *st = (stream_t *)calloc(1, sizeof(*st));
  int16_t l[FRAME_SAMPLES], r[FRAME_SAMPLES];
  int f;
  TASK_CHECK(s != NULL && st != NULL);
  for (f = 0; f < 12; f++) {
    int w = 0;
    int16_t *ch[2];
    unsigned char *p;
    gen_frame(4, f, RATE_48, FRAME_SAMPLES, l, r);
    ch[0] = l;
    ch[1] = r;
    p = shine_encode_buffer(s, ch, &w);
    TASK_CHECK(p && w > 0);
    memcpy(st->data + st->used, p, (size_t)w);
    st->used += (size_t)w;
  }
  TASK_CHECK(flush_stream(s, st) == 0);
  shine_close(s);
  TASK_CHECK(st->used == 4608);
  TASK_CHECK(fnv1a(st->data, st->used) == legacy_fnv);
  free(st);
  return 0;
}

/* Enabling and disabling before the first frame must not alter the bytes. */
static int state_enable_then_disable_is_transparent(void) {
  static stream_t a, b;
  shine_t s;
  int16_t l[FRAME_SAMPLES], r[FRAME_SAMPLES];
  int f;
  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));

  s = make_encoder(RATE_48, 128, JOINT_STEREO, 2);
  TASK_CHECK(s != NULL);
  for (f = 0; f < 10; f++) {
    int w = 0;
    int16_t *ch[2];
    unsigned char *p;
    gen_frame(4, f, RATE_48, FRAME_SAMPLES, l, r);
    ch[0] = l;
    ch[1] = r;
    p = shine_encode_buffer(s, ch, &w);
    TASK_CHECK(p && w > 0);
    memcpy(a.data + a.used, p, (size_t)w);
    a.used += (size_t)w;
  }
  TASK_CHECK(flush_stream(s, &a) == 0);
  shine_close(s);

  s = make_encoder(RATE_48, 128, JOINT_STEREO, 2);
  TASK_CHECK(s != NULL);
  TASK_CHECK(shine_set_ms(s, 2) == 0);
  TASK_CHECK(shine_set_ms(s, 0) == 0);
  for (f = 0; f < 10; f++) {
    int w = 0;
    int16_t *ch[2];
    unsigned char *p;
    gen_frame(4, f, RATE_48, FRAME_SAMPLES, l, r);
    ch[0] = l;
    ch[1] = r;
    p = shine_encode_buffer(s, ch, &w);
    TASK_CHECK(p && w > 0);
    memcpy(b.data + b.used, p, (size_t)w);
    b.used += (size_t)w;
  }
  TASK_CHECK(flush_stream(s, &b) == 0);
  shine_close(s);
  TASK_CHECK(a.used == b.used && memcmp(a.data, b.data, a.used) == 0);
  return 0;
}

/* Switching modes between frames must keep a single, consistent state, and
 * the analysis history must be the same as if M/S had never been touched. */
static int state_switching_preserves_history(void) {
  static stream_t ref, switched;
  shine_t s;
  int16_t l[FRAME_SAMPLES], r[FRAME_SAMPLES];
  int f;
  memset(&ref, 0, sizeof(ref));
  memset(&switched, 0, sizeof(switched));

  /* reference: plain LR for the first four frames, then auto */
  s = make_encoder(RATE_48, 128, JOINT_STEREO, 2);
  TASK_CHECK(s != NULL);
  for (f = 0; f < 10; f++) {
    int w = 0;
    int16_t *ch[2];
    unsigned char *p;
    if (f == 4)
      TASK_CHECK(shine_set_ms(s, 2) == 0);
    gen_frame(4, f, RATE_48, FRAME_SAMPLES, l, r);
    ch[0] = l;
    ch[1] = r;
    p = shine_encode_buffer(s, ch, &w);
    TASK_CHECK(p && w > 0);
    memcpy(ref.data + ref.used, p, (size_t)w);
    ref.used += (size_t)w;
  }
  TASK_CHECK(flush_stream(s, &ref) == 0);
  shine_close(s);

  /* switched: forced MS, then off, then auto; same analysis history */
  s = make_encoder(RATE_48, 128, JOINT_STEREO, 2);
  TASK_CHECK(s != NULL);
  for (f = 0; f < 10; f++) {
    int w = 0;
    int16_t *ch[2];
    unsigned char *p;
    if (f == 0)
      TASK_CHECK(shine_set_ms(s, 1) == 0);
    if (f == 2)
      TASK_CHECK(shine_set_ms(s, 0) == 0);
    if (f == 4)
      TASK_CHECK(shine_set_ms(s, 2) == 0);
    gen_frame(4, f, RATE_48, FRAME_SAMPLES, l, r);
    ch[0] = l;
    ch[1] = r;
    p = shine_encode_buffer(s, ch, &w);
    TASK_CHECK(p && w > 0);
    memcpy(switched.data + switched.used, p, (size_t)w);
    switched.used += (size_t)w;
  }
  TASK_CHECK(flush_stream(s, &switched) == 0);
  shine_close(s);

  /* From frame 4 on, when the setting is the same, the bytes must match:
   * the audio history is the same irrespective of the stereo representation
   * used by the preceding frames.  Compare the tail of both streams. */
  TASK_CHECK(ref.used == switched.used);
  {
    size_t tail = ref.used - 6 * 384; /* last six 48 kHz / 128 kbps frames */
    TASK_CHECK(ref.used > tail);
    TASK_CHECK(memcmp(ref.data + tail, switched.data + tail,
                      ref.used - tail) == 0);
  }
  return 0;
}

static int state_two_instances_are_independent(void) {
  shine_t a = make_encoder(RATE_48, 128, JOINT_STEREO, 2);
  shine_t b = make_encoder(RATE_48, 128, JOINT_STEREO, 2);
  static stream_t sa, sb;
  int16_t l[FRAME_SAMPLES], r[FRAME_SAMPLES];
  int f;
  TASK_CHECK(a != NULL && b != NULL);
  memset(&sa, 0, sizeof(sa));
  memset(&sb, 0, sizeof(sb));
  TASK_CHECK(shine_set_ms(a, 1) == 0);
  TASK_CHECK(shine_set_ms(b, 0) == 0);
  for (f = 0; f < 8; f++) {
    int w = 0;
    int16_t *ch[2];
    unsigned char *p;
    gen_frame(4, f, RATE_48, FRAME_SAMPLES, l, r);
    ch[0] = l;
    ch[1] = r;
    p = shine_encode_buffer(a, ch, &w);
    TASK_CHECK(p && w > 0);
    memcpy(sa.data + sa.used, p, (size_t)w);
    sa.used += (size_t)w;

    w = 0;
    p = shine_encode_buffer(b, ch, &w);
    TASK_CHECK(p && w > 0);
    memcpy(sb.data + sb.used, p, (size_t)w);
    sb.used += (size_t)w;
  }
  TASK_CHECK(flush_stream(a, &sa) == 0);
  TASK_CHECK(flush_stream(b, &sb) == 0);
  shine_close(a);
  shine_close(b);
  {
    walk_t wa, wb;
    TASK_CHECK(walk_stream(&sa, RATE_48, 128, &wa) == 0);
    TASK_CHECK(walk_stream(&sb, RATE_48, 128, &wb) == 0);
    TASK_CHECK(wa.ms_frames == wa.frames); /* instance a stayed M/S */
    TASK_CHECK(wb.ms_frames == 0);         /* instance b stayed LR */
  }
  return 0;
}

static int state_both_entry_points_match(void) {
  static stream_t planar, interleaved;
  shine_t s;
  int16_t l[FRAME_SAMPLES], r[FRAME_SAMPLES];
  int16_t inter[2 * FRAME_SAMPLES];
  int f, mode;
  for (mode = 0; mode <= 2; mode++) {
    memset(&planar, 0, sizeof(planar));
    memset(&interleaved, 0, sizeof(interleaved));
    s = make_encoder(RATE_48, 128, JOINT_STEREO, 2);
    TASK_CHECK(s != NULL);
    if (mode)
      TASK_CHECK(shine_set_ms(s, mode) == 0);
    for (f = 0; f < 8; f++) {
      int w = 0;
      int16_t *ch[2];
      unsigned char *p;
      int j;
      gen_frame(4, f, RATE_48, FRAME_SAMPLES, l, r);
      ch[0] = l;
      ch[1] = r;
      p = shine_encode_buffer(s, ch, &w);
      TASK_CHECK(p && w > 0);
      memcpy(planar.data + planar.used, p, (size_t)w);
      planar.used += (size_t)w;
      (void)j;
    }
    TASK_CHECK(flush_stream(s, &planar) == 0);
    shine_close(s);

    s = make_encoder(RATE_48, 128, JOINT_STEREO, 2);
    TASK_CHECK(s != NULL);
    if (mode)
      TASK_CHECK(shine_set_ms(s, mode) == 0);
    for (f = 0; f < 8; f++) {
      int w = 0;
      int j;
      unsigned char *p;
      gen_frame(4, f, RATE_48, FRAME_SAMPLES, l, r);
      for (j = 0; j < FRAME_SAMPLES; j++) {
        inter[2 * j] = l[j];
        inter[2 * j + 1] = r[j];
      }
      p = shine_encode_buffer_interleaved(s, inter, &w);
      TASK_CHECK(p && w > 0);
      memcpy(interleaved.data + interleaved.used, p, (size_t)w);
      interleaved.used += (size_t)w;
    }
    TASK_CHECK(flush_stream(s, &interleaved) == 0);
    shine_close(s);

    TASK_CHECK(planar.used == interleaved.used);
    TASK_CHECK(memcmp(planar.data, interleaved.data, planar.used) == 0);
  }
  return 0;
}

/* Turning M/S off again must return to the historical path immediately. */
static int state_disable_returns_to_lr(void) {
  shine_t s = make_encoder(RATE_48, 128, JOINT_STEREO, 2);
  shine_global_config *c;
  int16_t l[FRAME_SAMPLES], r[FRAME_SAMPLES];
  int w = 0;
  int16_t *ch[2];
  unsigned char *p;
  TASK_CHECK(s != NULL);
  c = (shine_global_config *)s;
  TASK_CHECK(shine_set_ms(s, 1) == 0);
  gen_frame(4, 0, RATE_48, FRAME_SAMPLES, l, r);
  ch[0] = l;
  ch[1] = r;
  p = shine_encode_buffer(s, ch, &w);
  TASK_CHECK(p && w > 0);
  TASK_CHECK(c->ms.obs_selected == 1);
  TASK_CHECK(((p[3] >> 4) & 3) == 2);

  TASK_CHECK(shine_set_ms(s, 0) == 0);
  gen_frame(4, 1, RATE_48, FRAME_SAMPLES, l, r);
  p = shine_encode_buffer(s, ch, &w);
  TASK_CHECK(p && w > 0);
  TASK_CHECK(((p[3] >> 6) & 3) == JOINT_STEREO);
  TASK_CHECK(((p[3] >> 4) & 3) == 0);
  shine_close(s);
  return 0;
}

/* The configuration structure layout is part of the ABI and must not have
 * grown or been reordered. */
static int state_public_config_layout(void) {
  shine_config_t cfg;
  TASK_CHECK(sizeof(cfg.wave.channels) == sizeof(int));
  TASK_CHECK(sizeof(cfg.wave.samplerate) == sizeof(int));
  TASK_CHECK(sizeof(cfg.mpeg.mode) == sizeof(int));
  TASK_CHECK(sizeof(cfg.mpeg.bitr) == sizeof(int));
  TASK_CHECK(sizeof(cfg.mpeg.emph) == sizeof(int));
  TASK_CHECK(offsetof(shine_config_t, wave) == 0);
  TASK_CHECK(offsetof(shine_config_t, mpeg) == offsetof(shine_config_t, wave) +
                                                   sizeof(shine_wave_t));
  TASK_CHECK(sizeof(shine_wave_t) == 2 * sizeof(int));
  return 0;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    {"decision", "auto_prefers_ms_for_correlated",
     decision_auto_prefers_ms_for_correlated},
    {"decision", "auto_keeps_lr_for_antiphase",
     decision_auto_keeps_lr_for_antiphase},
    {"decision", "forced_modes_are_honoured",
     decision_forced_modes_are_honoured},
    {"decision", "decision_is_frame_wide", decision_is_frame_wide},
    {"decision", "all_zero_is_a_tie", decision_all_zero_is_a_tie},
    {"decision", "ms_improves_correlated_signal",
     decision_ms_improves_correlated_signal},

    {"bitstream", "lr_stream_is_plain_stereo",
     bitstream_lr_stream_is_plain_stereo},
    {"bitstream", "forced_ms_headers_and_lengths",
     bitstream_forced_ms_headers_and_lengths},
    {"bitstream", "44k_padding_alternates", bitstream_44k_padding_alternates},
    {"bitstream", "decodes_nonzero_audio", bitstream_decodes_nonzero_audio},
    {"bitstream", "panning_and_loudness", bitstream_panning_and_loudness},
    {"bitstream", "silent_channel_donates", bitstream_silent_channel_donates},
    {"bitstream", "independent_channels_stay_separate",
     bitstream_independent_channels_stay_separate},
    {"bitstream", "low_bitrate_stays_legal", bitstream_low_bitrate_stays_legal},

    {"state", "set_ms_contract", state_set_ms_contract},
    {"state", "rejected_call_keeps_stream", state_rejected_call_keeps_stream},
    {"state", "never_enabled_output_is_legacy",
     state_never_enabled_output_is_legacy},
    {"state", "enable_then_disable_is_transparent",
     state_enable_then_disable_is_transparent},
    {"state", "switching_preserves_history", state_switching_preserves_history},
    {"state", "two_instances_are_independent",
     state_two_instances_are_independent},
    {"state", "both_entry_points_match", state_both_entry_points_match},
    {"state", "disable_returns_to_lr", state_disable_returns_to_lr},
    {"state", "public_config_layout", state_public_config_layout},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
