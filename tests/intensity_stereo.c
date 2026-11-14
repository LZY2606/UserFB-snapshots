/* tests/intensity_stereo.c
 *
 * Tests for the per-frame MPEG-1 Layer III intensity stereo control added
 * through shine_set_intensity().  The frames are inspected with a small
 * independent bit reader (no encoder internals beyond the public API and
 * the published scalefactor band / slen tables) and, where loudspeaker
 * behaviour matters, decoded with the bundled minimp3 decoder.
 */

#include "../task_support/check.h"
#include "layer3.h"
#include "tables.h"
#include "types.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "../task_support/minimp3/minimp3.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TEST_FRAMES 8
#define STREAM_CAP 16384
#define SAMPLES_PER_FRAME (GRANULE_SIZE * 2)

/* ------------------------------------------------------------------ */
/* Independent MP3 bit reader                                         */
/* ------------------------------------------------------------------ */

typedef struct {
  const unsigned char *data;
  int byte_pos;
  int bit_pos; /* 0..7, MSB first */
} bit_reader_t;

static void br_init(bit_reader_t *r, const unsigned char *data) {
  r->data = data;
  r->byte_pos = 0;
  r->bit_pos = 0;
}

static unsigned br_get(bit_reader_t *r, int n) {
  unsigned value = 0;
  while (n-- > 0) {
    value <<= 1;
    value |= (unsigned)((r->data[r->byte_pos] >> (7 - r->bit_pos)) & 1);
    if (++r->bit_pos == 8) {
      r->bit_pos = 0;
      r->byte_pos++;
    }
  }
  return value;
}

struct parsed_granule {
  unsigned part2_3_length;
  unsigned big_values;
  unsigned global_gain;
  unsigned scalefac_compress;
  unsigned table_select[3];
  unsigned region0_count;
  unsigned region1_count;
  unsigned preflag;
  unsigned scalefac_scale;
  unsigned count1table_select;
  int scalefac[21];
};

struct parsed_frame {
  int version;
  int layer;
  int bitrate_index;
  int samplerate_index;
  int padding;
  int mode;
  int mode_ext;
  int scfsi[2][4];
  struct parsed_granule gr[2][2]; /* [granule][channel] */
  int frame_bytes;
};

/* Parse one MPEG-1 Layer III stereo frame, including every scalefactor. */
static int parse_frame(const unsigned char *data, int length,
                       struct parsed_frame *out) {
  bit_reader_t br;
  int gr, ch, p, sfb;
  static const int count[4] = {6, 5, 5, 5};

  memset(out, 0, sizeof(*out));
  br_init(&br, data);

  if (br_get(&br, 11) != 0x7ff)
    return -1;
  out->version = (int)br_get(&br, 2);
  out->layer = (int)br_get(&br, 2);
  br_get(&br, 1); /* protection */
  out->bitrate_index = (int)br_get(&br, 4);
  out->samplerate_index = (int)br_get(&br, 2);
  out->padding = (int)br_get(&br, 1);
  br_get(&br, 1); /* private */
  out->mode = (int)br_get(&br, 2);
  out->mode_ext = (int)br_get(&br, 2);
  br_get(&br, 1); /* copyright */
  br_get(&br, 1); /* original */
  br_get(&br, 2); /* emphasis */

  if (out->version != MPEG_I || out->layer != LAYER_III)
    return -1;

  /* MPEG-1 stereo side information: 32 bytes. */
  br_get(&br, 9); /* main_data_begin, always 0 from shine */
  br_get(&br, 3); /* private bits */
  for (ch = 0; ch < 2; ch++)
    for (p = 0; p < 4; p++)
      out->scfsi[ch][p] = (int)br_get(&br, 1);

  for (gr = 0; gr < 2; gr++)
    for (ch = 0; ch < 2; ch++) {
      struct parsed_granule *g = &out->gr[gr][ch];
      g->part2_3_length = br_get(&br, 12);
      g->big_values = br_get(&br, 9);
      g->global_gain = br_get(&br, 8);
      g->scalefac_compress = br_get(&br, 4);
      br_get(&br, 1); /* window switch flag, always 0 for long blocks */
      g->table_select[0] = br_get(&br, 5);
      g->table_select[1] = br_get(&br, 5);
      g->table_select[2] = br_get(&br, 5);
      g->region0_count = br_get(&br, 4);
      g->region1_count = br_get(&br, 3);
      g->preflag = br_get(&br, 1);
      g->scalefac_scale = br_get(&br, 1);
      g->count1table_select = br_get(&br, 1);
    }

  /* Main data starts after 4 header + 32 side info bytes.  Each
   * granule/channel block holds its scalefactors followed by its Huffman
   * data; part2_3_length spans both, so skip the Huffman portion to reach
   * the next block. */
  br.byte_pos = 36;
  br.bit_pos = 0;

  for (gr = 0; gr < 2; gr++)
    for (ch = 0; ch < 2; ch++) {
      struct parsed_granule *g = &out->gr[gr][ch];
      int slen[4];
      int part2_bits = 0;
      slen[0] = slen[1] = shine_slen1_tab[g->scalefac_compress];
      slen[2] = slen[3] = shine_slen2_tab[g->scalefac_compress];

      sfb = 0;
      for (p = 0; p < 4; p++) {
        if (gr == 1 && out->scfsi[ch][p]) {
          memcpy(&g->scalefac[sfb], &out->gr[0][ch].scalefac[sfb],
                 count[p] * sizeof(int));
        } else {
          int k;
          part2_bits += count[p] * slen[p];
          for (k = 0; k < count[p]; k++)
            g->scalefac[sfb + k] =
                slen[p] ? (int)br_get(&br, slen[p]) : 0;
        }
        sfb += count[p];
      }

      /* Skip the Huffman coded spectral values of this block. */
      if ((int)g->part2_3_length >= part2_bits) {
        int skip = (int)g->part2_3_length - part2_bits;
        br.byte_pos += (br.bit_pos + skip) >> 3;
        br.bit_pos = (br.bit_pos + skip) & 7;
      }
    }

  {
    int bitrate = bitrates[out->bitrate_index][MPEG_I];
    int samplerate = samplerates[out->samplerate_index];
    if (bitrate <= 0 || samplerate <= 0)
      return -1;
    out->frame_bytes = (144 * bitrate * 1000) / samplerate + out->padding;
  }

  return out->frame_bytes <= length ? 0 : -1;
}

/* Find the next MPEG frame sync at or after offset from.  At sample rates
 * whose frame size is not a multiple of four (e.g. 44.1 kHz) the encoder's
 * bit cache makes per-call byte counts wobble, so frame walks use sync
 * words rather than call boundaries. */
static int next_sync(const unsigned char *data, int length, int from) {
  int i;
  for (i = from; i + 3 < length; i++) {
    if (data[i] == 0xff && (data[i + 1] & 0xf0) == 0xf0 &&
        ((data[i + 1] >> 3) & 3) == MPEG_I &&
        ((data[i + 1] >> 1) & 3) == LAYER_III)
      return i;
  }
  return -1;
}

/* ------------------------------------------------------------------ */
/* Encoder / PCM helpers                                               */
/* ------------------------------------------------------------------ */

static shine_t make_encoder(int samplerate, int mode) {
  shine_config_t cfg;
  shine_set_config_mpeg_defaults(&cfg.mpeg);
  cfg.wave.channels = PCM_STEREO;
  cfg.wave.samplerate = samplerate;
  cfg.mpeg.mode = mode;
  return shine_initialise(&cfg);
}

typedef void (*configure_fn)(shine_t enc, int frame);
typedef void (*pcm_fn)(int16_t *pcm, int frame, int samplerate);

/* Independent sinusoids per channel, both with high frequency content. */
static void gen_broadband(int16_t *pcm, int frame, int samplerate) {
  int i;
  for (i = 0; i < SAMPLES_PER_FRAME; i++) {
    long t = (long)frame * SAMPLES_PER_FRAME + i;
    pcm[2 * i] = (int16_t)(11000 * sin(2.0 * M_PI * 9000.0 * t / samplerate));
    pcm[2 * i + 1] =
        (int16_t)(9000 * sin(2.0 * M_PI * 11000.0 * t / samplerate));
  }
}

/* Same loud tone on both channels so the intensity pan is unambiguous. */
static void gen_identical_hf(int16_t *pcm, int frame, int samplerate) {
  int i;
  for (i = 0; i < SAMPLES_PER_FRAME; i++) {
    long t = (long)frame * SAMPLES_PER_FRAME + i;
    int16_t v = (int16_t)(12000 * sin(2.0 * M_PI * 9000.0 * t / samplerate));
    pcm[2 * i] = v;
    pcm[2 * i + 1] = v;
  }
}

/* Left-only low frequency tone below the intensity boundary. */
static void gen_low_left_only(int16_t *pcm, int frame, int samplerate) {
  int i;
  for (i = 0; i < SAMPLES_PER_FRAME; i++) {
    long t = (long)frame * SAMPLES_PER_FRAME + i;
    pcm[2 * i] = (int16_t)(14000 * sin(2.0 * M_PI * 400.0 * t / samplerate));
    pcm[2 * i + 1] = 0;
  }
}

/* Left-only high frequency tone: the right channel spectrum is all zero
 * even in the merged region. */
static void gen_silent_right_hf(int16_t *pcm, int frame, int samplerate) {
  int i;
  for (i = 0; i < SAMPLES_PER_FRAME; i++) {
    long t = (long)frame * SAMPLES_PER_FRAME + i;
    pcm[2 * i] = (int16_t)(14000 * sin(2.0 * M_PI * 9000.0 * t / samplerate));
    pcm[2 * i + 1] = 0;
  }
}

/* Encode TEST_FRAMES frames.  configure() runs before every frame so tests
 * can toggle intensity per frame; planar selects the planar entry point. */
static int encode_stream(int samplerate, int mode, int planar,
                         configure_fn configure, pcm_fn gen,
                         unsigned char *stream) {
  shine_t enc = make_encoder(samplerate, mode);
  int16_t interleaved[SAMPLES_PER_FRAME * 2];
  int16_t left[SAMPLES_PER_FRAME], right[SAMPLES_PER_FRAME];
  int16_t *planes[2] = {left, right};
  int total = 0, frame, written, i;

  if (!enc)
    return -1;

  for (frame = 0; frame < TEST_FRAMES; frame++) {
    unsigned char *out;
    gen(interleaved, frame, samplerate);
    if (configure)
      configure(enc, frame);
    if (planar) {
      for (i = 0; i < SAMPLES_PER_FRAME; i++) {
        left[i] = interleaved[2 * i];
        right[i] = interleaved[2 * i + 1];
      }
      out = shine_encode_buffer(enc, planes, &written);
    } else {
      out = shine_encode_buffer_interleaved(enc, interleaved, &written);
    }
    if (!out || written <= 0 || total + written > STREAM_CAP) {
      shine_close(enc);
      return -1;
    }
    memcpy(stream + total, out, written);
    total += written;
  }

  written = 0;
  {
    unsigned char *tail = shine_flush(enc, &written);
    if (written) {
      if (total + written > STREAM_CAP) {
        shine_close(enc);
        return -1;
      }
      memcpy(stream + total, tail, written);
      total += written;
    }
  }
  shine_close(enc);
  return total;
}

struct decoded {
  int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
  mp3dec_frame_info_t info;
  int samples;
};

static int decode_stream(const unsigned char *stream, int length,
                         struct decoded *out, int max_frames) {
  mp3dec_t dec;
  int at = 0, count = 0;

  mp3dec_init(&dec);
  while (at < length && count < max_frames) {
    out[count].samples = mp3dec_decode_frame(
        &dec, stream + at, length - at, out[count].pcm, &out[count].info);
    if (out[count].info.frame_bytes <= 0)
      return -1;
    at += out[count].info.frame_bytes;
    count++;
  }
  return count;
}

static long long channel_energy(const struct decoded *d, int channel,
                                int start, int count) {
  long long energy = 0;
  int i;
  for (i = 0; i < count; i++) {
    int v = d->pcm[2 * (start + i) + channel];
    energy += (long long)v * v;
  }
  return energy;
}

/* ------------------------------------------------------------------ */
/* syntax group                                                        */
/* ------------------------------------------------------------------ */

static void cfg_enable_11(shine_t enc, int frame) {
  (void)frame;
  shine_set_intensity(enc, 11, 1, 5);
}

static int case_header_and_mode_ext(void) {
  unsigned char on[STREAM_CAP], off[STREAM_CAP];
  struct parsed_frame f;
  int len_on, len_off;

  len_on = encode_stream(48000, STEREO, 0, cfg_enable_11, gen_broadband, on);
  TASK_CHECK(len_on > 0);
  TASK_CHECK(parse_frame(on, len_on, &f) == 0);
  TASK_CHECK(f.mode == JOINT_STEREO);
  TASK_CHECK(f.mode_ext == 1); /* intensity bit set, M/S bit clear */
  TASK_CHECK(f.padding == 0);  /* 48 kHz CBR has no padding */

  len_off = encode_stream(48000, STEREO, 0, NULL, gen_broadband, off);
  TASK_CHECK(len_off > 0);
  TASK_CHECK(parse_frame(off, len_off, &f) == 0);
  TASK_CHECK(f.mode == STEREO);
  TASK_CHECK(f.mode_ext == 0);
  return 0;
}

static int case_initial_joint_mode_restored(void) {
  unsigned char stream[STREAM_CAP];
  struct parsed_frame f;
  int len = encode_stream(48000, JOINT_STEREO, 0, NULL, gen_broadband, stream);
  TASK_CHECK(len > 0);
  TASK_CHECK(parse_frame(stream, len, &f) == 0);
  TASK_CHECK(f.mode == JOINT_STEREO);
  TASK_CHECK(f.mode_ext == 0); /* no intensity without an enable call */
  return 0;
}

static int case_scalefactor_layout(void) {
  unsigned char stream[STREAM_CAP];
  struct parsed_frame f;
  int len, gr, sfb;

  len = encode_stream(48000, STEREO, 0, cfg_enable_11, gen_broadband, stream);
  TASK_CHECK(len > 0);
  TASK_CHECK(parse_frame(stream, len, &f) == 0);

  for (gr = 0; gr < 2; gr++) {
    struct parsed_granule *r = &f.gr[gr][1];
    int expected_position = gr == 0 ? 1 : 5;
    TASK_CHECK(r->scalefac_compress == 13); /* 3 bit slen1 and slen2 */
    for (sfb = 0; sfb < 21; sfb++) {
      if (sfb < 11)
        TASK_CHECK(r->scalefac[sfb] == 7); /* LR band, intensity off */
      else
        TASK_CHECK(r->scalefac[sfb] == expected_position);
    }
    TASK_CHECK(f.scfsi[1][0] == 0 && f.scfsi[1][1] == 0 &&
               f.scfsi[1][2] == 0 && f.scfsi[1][3] == 0);
  }
  return 0;
}

static int case_part23_and_constant_frame(void) {
  unsigned char stream[STREAM_CAP];
  struct parsed_frame f;
  int len, at, frames = 0;

  len = encode_stream(48000, STEREO, 0, cfg_enable_11, gen_broadband, stream);
  TASK_CHECK(len > 0);

  at = 0;
  while (at < len) {
    TASK_CHECK(parse_frame(stream + at, len - at, &f) == 0);
    TASK_CHECK(f.frame_bytes == 384); /* 128 kbps, 48 kHz */
    TASK_CHECK(f.gr[0][1].part2_3_length >= 21 * 3);
    TASK_CHECK(f.gr[1][1].part2_3_length >= 21 * 3);
    TASK_CHECK(f.gr[0][1].part2_3_length <= 4095);
    TASK_CHECK(f.gr[1][1].part2_3_length <= 4095);
    at += f.frame_bytes;
    frames++;
  }
  TASK_CHECK(frames == TEST_FRAMES);
  TASK_CHECK(len == TEST_FRAMES * 384);
  return 0;
}

static int case_silent_right_granule(void) {
  unsigned char stream[STREAM_CAP];
  struct parsed_frame f;
  struct decoded decoded[TEST_FRAMES];
  int len, frames, gr;

  len = encode_stream(48000, STEREO, 0, cfg_enable_11, gen_silent_right_hf,
                      stream);
  TASK_CHECK(len > 0);
  TASK_CHECK(parse_frame(stream, len, &f) == 0);

  for (gr = 0; gr < 2; gr++) {
    struct parsed_granule *r = &f.gr[gr][1];
    TASK_CHECK(r->big_values == 0);
    TASK_CHECK(r->part2_3_length == 21 * 3); /* only the position bits */
    TASK_CHECK(r->scalefac[10] == 7);
    TASK_CHECK(r->scalefac[11] == (gr == 0 ? 1 : 5));
  }

  frames = decode_stream(stream, len, decoded, TEST_FRAMES);
  TASK_CHECK(frames == TEST_FRAMES);
  TASK_CHECK(decoded[3].info.channels == 2);
  TASK_CHECK(decoded[3].samples == SAMPLES_PER_FRAME);
  return 0;
}

static int case_rejected_settings(void) {
  shine_config_t cfg;
  shine_t enc, lowrate, dual;

  enc = make_encoder(48000, STEREO);
  TASK_CHECK(enc != NULL);
  TASK_CHECK(shine_set_intensity(enc, 11, 0, 6) == 0);
  /* Out of range scalefactor bands and positions. */
  TASK_CHECK(shine_set_intensity(enc, 10, 0, 6) == -1);
  TASK_CHECK(shine_set_intensity(enc, 21, 0, 6) == -1);
  TASK_CHECK(shine_set_intensity(enc, 11, 7, 6) == -1);
  TASK_CHECK(shine_set_intensity(enc, 11, 0, -1) == -1);
  TASK_CHECK(shine_set_intensity(NULL, 11, 0, 6) == -1);
  shine_close(enc);

  /* Mono instance. */
  shine_set_config_mpeg_defaults(&cfg.mpeg);
  cfg.wave.channels = PCM_MONO;
  cfg.wave.samplerate = 48000;
  cfg.mpeg.mode = MONO;
  enc = shine_initialise(&cfg);
  TASK_CHECK(enc != NULL);
  TASK_CHECK(shine_set_intensity(enc, 11, 0, 6) == -1);
  shine_close(enc);

  /* MPEG-II sample rate. */
  shine_set_config_mpeg_defaults(&cfg.mpeg);
  cfg.wave.channels = PCM_STEREO;
  cfg.wave.samplerate = 22050;
  cfg.mpeg.mode = STEREO;
  lowrate = shine_initialise(&cfg);
  TASK_CHECK(lowrate != NULL);
  TASK_CHECK(shine_set_intensity(lowrate, 11, 0, 6) == -1);
  shine_close(lowrate);

  /* Dual channel mode. */
  shine_set_config_mpeg_defaults(&cfg.mpeg);
  cfg.wave.channels = PCM_STEREO;
  cfg.wave.samplerate = 48000;
  cfg.mpeg.mode = DUAL_CHANNEL;
  dual = shine_initialise(&cfg);
  TASK_CHECK(dual != NULL);
  TASK_CHECK(shine_set_intensity(dual, 11, 0, 6) == -1);
  shine_close(dual);
  return 0;
}

static int reject_seen; /* set by cfg_reject_mid_stream */

static void cfg_reject_mid_stream(shine_t enc, int frame) {
  if (frame == 0)
    shine_set_intensity(enc, 12, 2, 4);
  if (frame == 3 && shine_set_intensity(enc, 5, 0, 0) != -1)
    reject_seen = 0;
}

static int case_rejected_call_keeps_state(void) {
  unsigned char stream[STREAM_CAP];
  struct parsed_frame f;
  int len, at = 0, frame = 0, sfb;

  reject_seen = 1;
  len = encode_stream(48000, STEREO, 0, cfg_reject_mid_stream, gen_broadband,
                      stream);
  TASK_CHECK(len > 0);
  TASK_CHECK(reject_seen);
  while (at < len) {
    TASK_CHECK(parse_frame(stream + at, len - at, &f) == 0);
    TASK_CHECK(f.mode == JOINT_STEREO && f.mode_ext == 1);
    TASK_CHECK(f.gr[0][1].scalefac_compress == 13);
    for (sfb = 12; sfb < 21; sfb++) {
      TASK_CHECK(f.gr[0][1].scalefac[sfb] == 2);
      TASK_CHECK(f.gr[1][1].scalefac[sfb] == 4);
    }
    at += f.frame_bytes;
    frame++;
  }
  TASK_CHECK(frame == TEST_FRAMES);
  return 0;
}

static int case_entry_points_identical(void) {
  unsigned char planar[STREAM_CAP], interleaved[STREAM_CAP];
  int len_p, len_i;

  len_p = encode_stream(48000, STEREO, 1, cfg_enable_11, gen_broadband,
                        planar);
  len_i = encode_stream(48000, STEREO, 0, cfg_enable_11, gen_broadband,
                        interleaved);
  TASK_CHECK(len_p > 0 && len_p == len_i);
  TASK_CHECK(memcmp(planar, interleaved, (size_t)len_p) == 0);
  return 0;
}

static int case_padding_44k(void) {
  unsigned char stream[STREAM_CAP];
  struct parsed_frame f;
  int len, at, frames = 0, saw_padding = 0, total = 0;

  len = encode_stream(44100, STEREO, 0, cfg_enable_11, gen_broadband, stream);
  TASK_CHECK(len > 0);

  at = 0;
  while (at < len) {
    TASK_CHECK(parse_frame(stream + at, len - at, &f) == 0);
    TASK_CHECK(f.samplerate_index == 0);
    TASK_CHECK(f.frame_bytes == 417 + f.padding);
    TASK_CHECK(f.mode == JOINT_STEREO && f.mode_ext == 1);
    if (f.padding)
      saw_padding = 1;
    total += f.frame_bytes;
    frames++;
    if (at + f.frame_bytes < len)
      TASK_CHECK(next_sync(stream, len, at + f.frame_bytes) ==
                 at + f.frame_bytes);
    at += f.frame_bytes;
  }
  TASK_CHECK(frames == TEST_FRAMES);
  TASK_CHECK(total == len);
  /* 128 kbps at 44.1 kHz needs a padding slot roughly every 25 frames. */
  TASK_CHECK(saw_padding);
  return 0;
}

static int case_supported_rates(void) {
  shine_t e32 = make_encoder(32000, STEREO);
  shine_t e44 = make_encoder(44100, JOINT_STEREO);
  shine_t e48 = make_encoder(48000, STEREO);
  TASK_CHECK(e32 && e44 && e48);
  TASK_CHECK(shine_set_intensity(e32, 11, 0, 6) == 0);
  TASK_CHECK(shine_set_intensity(e44, 20, 6, 0) == 0);
  TASK_CHECK(shine_set_intensity(e48, 15, 3, 3) == 0);
  shine_close(e32);
  shine_close(e44);
  shine_close(e48);
  return 0;
}

static int case_disabled_bytes_unchanged(void) {
  unsigned char never[STREAM_CAP], toggled[STREAM_CAP];
  int len_never, len_toggled;

  /* Never enabled: plain LR stream. */
  len_never = encode_stream(48000, STEREO, 0, NULL, gen_broadband, never);
  TASK_CHECK(len_never > 0);

  /* Only disable / rejected calls: output must stay byte identical. */
  {
    shine_t enc = make_encoder(48000, STEREO);
    int16_t pcm[SAMPLES_PER_FRAME * 2];
    int total = 0, frame, written;
    TASK_CHECK(enc != NULL);
    TASK_CHECK(shine_set_intensity(enc, -1, 0, 0) == 0);
    TASK_CHECK(shine_set_intensity(enc, 99, 0, 0) == -1);
    for (frame = 0; frame < TEST_FRAMES; frame++) {
      unsigned char *out;
      gen_broadband(pcm, frame, 48000);
      out = shine_encode_buffer_interleaved(enc, pcm, &written);
      TASK_CHECK(out && written > 0);
      memcpy(toggled + total, out, written);
      total += written;
    }
    shine_close(enc);
    len_toggled = total;
  }

  TASK_CHECK(len_toggled == len_never);
  TASK_CHECK(memcmp(toggled, never, (size_t)len_never) == 0);
  return 0;
}

/* ------------------------------------------------------------------ */
/* pan group                                                           */
/* ------------------------------------------------------------------ */

/* configure_fn callbacks with a fixed position for both granules. */
#define PAN_CALLBACK(name, POS)                                              \
  static void name(shine_t enc, int frame) {                                 \
    (void)frame;                                                              \
    shine_set_intensity(enc, 11, POS, POS);                                  \
  }

PAN_CALLBACK(cfg_pan0, 0)
PAN_CALLBACK(cfg_pan1, 1)
PAN_CALLBACK(cfg_pan3, 3)
PAN_CALLBACK(cfg_pan5, 5)
PAN_CALLBACK(cfg_pan6, 6)

static int decode_with(configure_fn cfg, pcm_fn gen, struct decoded *out) {
  unsigned char stream[STREAM_CAP];
  int len = encode_stream(48000, STEREO, 0, cfg, gen, stream);
  int frames;
  if (len <= 0)
    return -1;
  frames = decode_stream(stream, len, out, TEST_FRAMES);
  return frames == TEST_FRAMES ? 0 : -1;
}

static int case_pan_extremes_and_center(void) {
  struct decoded d0[TEST_FRAMES], d3[TEST_FRAMES], d6[TEST_FRAMES];
  long long el, er;

  TASK_CHECK(decode_with(cfg_pan0, gen_identical_hf, d0) == 0);
  TASK_CHECK(decode_with(cfg_pan3, gen_identical_hf, d3) == 0);
  TASK_CHECK(decode_with(cfg_pan6, gen_identical_hf, d6) == 0);

  /* Position 0: fully right. */
  el = channel_energy(&d0[4], 0, 0, SAMPLES_PER_FRAME);
  er = channel_energy(&d0[4], 1, 0, SAMPLES_PER_FRAME);
  TASK_CHECK(er > 1000000);
  TASK_CHECK(er > 25 * (el + 1));

  /* Position 6: fully left. */
  el = channel_energy(&d6[4], 0, 0, SAMPLES_PER_FRAME);
  er = channel_energy(&d6[4], 1, 0, SAMPLES_PER_FRAME);
  TASK_CHECK(el > 1000000);
  TASK_CHECK(el > 25 * (er + 1));

  /* Position 3: equal split. */
  el = channel_energy(&d3[4], 0, 0, SAMPLES_PER_FRAME);
  er = channel_energy(&d3[4], 1, 0, SAMPLES_PER_FRAME);
  TASK_CHECK(el > 1000000 && er > 1000000);
  TASK_CHECK(el * 2 > er && er * 2 > el);
  return 0;
}

static int case_tangent_positions(void) {
  struct decoded d1[TEST_FRAMES], d5[TEST_FRAMES];
  long long l1, r1, l5, r5;
  double share1, share5;

  TASK_CHECK(decode_with(cfg_pan1, gen_identical_hf, d1) == 0);
  TASK_CHECK(decode_with(cfg_pan5, gen_identical_hf, d5) == 0);

  l1 = channel_energy(&d1[4], 0, 0, SAMPLES_PER_FRAME);
  r1 = channel_energy(&d1[4], 1, 0, SAMPLES_PER_FRAME);
  l5 = channel_energy(&d5[4], 0, 0, SAMPLES_PER_FRAME);
  r5 = channel_energy(&d5[4], 1, 0, SAMPLES_PER_FRAME);
  TASK_CHECK(l1 > 100000 && r1 > 100000 && l5 > 100000 && r5 > 100000);

  /* Standard tangent ratios: position 1 ~= 0.211 left, position 5 ~= 0.789. */
  share1 = (double)l1 / (double)(l1 + r1);
  share5 = (double)l5 / (double)(l5 + r5);
  TASK_CHECK(share1 > 0.05 && share1 < 0.45);
  TASK_CHECK(share5 > 0.55 && share5 < 0.95);
  TASK_CHECK(share5 > share1);
  return 0;
}

static void cfg_granules_differ(shine_t enc, int frame) {
  (void)frame;
  shine_set_intensity(enc, 11, 0, 6);
}

static int case_granule_pan_differs(void) {
  unsigned char stream[STREAM_CAP];
  struct parsed_frame f;
  struct decoded d[TEST_FRAMES];
  long long l0, r0, l1, r1;
  double share0, share1;
  int len;

  /* Bitstream: the two granules must carry distinct positions. */
  len = encode_stream(48000, STEREO, 0, cfg_granules_differ,
                      gen_identical_hf, stream);
  TASK_CHECK(len > 0);
  TASK_CHECK(parse_frame(stream + 4 * 384, len - 4 * 384, &f) == 0);
  TASK_CHECK(f.gr[0][1].scalefac[11] == 0);
  TASK_CHECK(f.gr[1][1].scalefac[11] == 6);

  /* Decoded audio: the two time halves must lean to opposite speakers.
   * The synthesis polyphase filter bank has a group delay close to one
   * granule, so with per-granule pan alternation the nominal half ordering
   * is delayed; the robust invariant is that the two halves have opposite,
   * clearly separated L/R balances rather than a fixed time mapping. */
  TASK_CHECK(decode_stream(stream, len, d, TEST_FRAMES) == TEST_FRAMES);
  l0 = channel_energy(&d[4], 0, 0, GRANULE_SIZE);
  r0 = channel_energy(&d[4], 1, 0, GRANULE_SIZE);
  l1 = channel_energy(&d[4], 0, GRANULE_SIZE, GRANULE_SIZE);
  r1 = channel_energy(&d[4], 1, GRANULE_SIZE, GRANULE_SIZE);
  TASK_CHECK(l0 + r0 > 1000000 && l1 + r1 > 1000000);
  share0 = (double)l0 / (double)(l0 + r0);
  share1 = (double)l1 / (double)(l1 + r1);
  TASK_CHECK((share0 > 0.6 && share1 < 0.4) ||
             (share0 < 0.4 && share1 > 0.6));
  TASK_CHECK(fabs(share0 - share1) > 0.3);
  return 0;
}

static int case_high_frequency_present(void) {
  struct decoded d[TEST_FRAMES];
  long long energy;
  TASK_CHECK(decode_with(cfg_pan3, gen_identical_hf, d) == 0);
  energy = channel_energy(&d[4], 0, 0, SAMPLES_PER_FRAME) +
           channel_energy(&d[4], 1, 0, SAMPLES_PER_FRAME);
  TASK_CHECK(energy > 10000000);
  TASK_CHECK(d[4].info.hz == 48000 && d[4].info.channels == 2);
  return 0;
}

static int case_lr_below_boundary_preserved(void) {
  struct decoded d[TEST_FRAMES];
  long long left, right;

  /* Position 0 would pan high frequencies fully right, but the only signal
   * is a low frequency left-only tone below scalefactor band 11. */
  TASK_CHECK(decode_with(cfg_pan0, gen_low_left_only, d) == 0);
  left = channel_energy(&d[4], 0, 0, SAMPLES_PER_FRAME);
  right = channel_energy(&d[4], 1, 0, SAMPLES_PER_FRAME);
  TASK_CHECK(left > 1000000);
  TASK_CHECK(left > 20 * (right + 1));
  return 0;
}

static int case_silent_right_decodes(void) {
  struct decoded d[TEST_FRAMES];
  long long right_hf;

  TASK_CHECK(decode_with(cfg_pan3, gen_silent_right_hf, d) == 0);
  /* The merged high frequency spectrum must still reach both speakers. */
  right_hf = channel_energy(&d[4], 1, 0, SAMPLES_PER_FRAME);
  TASK_CHECK(right_hf > 100000);
  return 0;
}

/* ------------------------------------------------------------------ */
/* transitions group                                                   */
/* ------------------------------------------------------------------ */

static void cfg_toggle_every_frame(shine_t enc, int frame) {
  if (frame & 1)
    shine_set_intensity(enc, -1, 0, 0);
  else
    shine_set_intensity(enc, 11, 2, 4);
}

static int case_frame_by_frame_toggle(void) {
  unsigned char stream[STREAM_CAP];
  struct parsed_frame f;
  int len, at = 0, frame = 0, sfb;

  len = encode_stream(48000, STEREO, 0, cfg_toggle_every_frame,
                      gen_broadband, stream);
  TASK_CHECK(len > 0);
  while (at < len) {
    TASK_CHECK(parse_frame(stream + at, len - at, &f) == 0);
    if (frame & 1) {
      TASK_CHECK(f.mode == STEREO && f.mode_ext == 0);
    } else {
      TASK_CHECK(f.mode == JOINT_STEREO && f.mode_ext == 1);
      for (sfb = 11; sfb < 21; sfb++) {
        TASK_CHECK(f.gr[0][1].scalefac[sfb] == 2);
        TASK_CHECK(f.gr[1][1].scalefac[sfb] == 4);
      }
    }
    at += f.frame_bytes;
    frame++;
  }
  TASK_CHECK(frame == TEST_FRAMES);
  return 0;
}

static void cfg_change_boundary(shine_t enc, int frame) {
  if (frame == 0)
    shine_set_intensity(enc, 11, 3, 3);
  if (frame == 3)
    shine_set_intensity(enc, 16, 3, 3);
  if (frame == 6)
    shine_set_intensity(enc, -1, 0, 0);
}

static int case_boundary_changes(void) {
  unsigned char stream[STREAM_CAP];
  struct parsed_frame f;
  int len, at = 0, frame = 0, sfb;

  len = encode_stream(48000, STEREO, 0, cfg_change_boundary, gen_broadband,
                      stream);
  TASK_CHECK(len > 0);
  while (at < len) {
    TASK_CHECK(parse_frame(stream + at, len - at, &f) == 0);
    if (frame <= 2) {
      for (sfb = 0; sfb < 21; sfb++)
        TASK_CHECK(f.gr[0][1].scalefac[sfb] == (sfb < 11 ? 7 : 3));
    } else if (frame <= 5) {
      for (sfb = 0; sfb < 21; sfb++)
        TASK_CHECK(f.gr[0][1].scalefac[sfb] == (sfb < 16 ? 7 : 3));
    } else {
      TASK_CHECK(f.mode == STEREO && f.mode_ext == 0);
    }
    at += f.frame_bytes;
    frame++;
  }
  TASK_CHECK(frame == TEST_FRAMES);
  return 0;
}

static void cfg_position_sequence(shine_t enc, int frame) {
  static const int pos[TEST_FRAMES][2] = {
      {0, 6}, {1, 5}, {2, 4}, {3, 3}, {4, 2}, {5, 1}, {6, 0}, {0, 6}};
  shine_set_intensity(enc, 13, pos[frame][0], pos[frame][1]);
}

static int case_position_transitions(void) {
  unsigned char stream[STREAM_CAP];
  struct parsed_frame f;
  static const int pos[TEST_FRAMES][2] = {
      {0, 6}, {1, 5}, {2, 4}, {3, 3}, {4, 2}, {5, 1}, {6, 0}, {0, 6}};
  int len, at = 0, frame = 0;

  len = encode_stream(48000, STEREO, 0, cfg_position_sequence, gen_broadband,
                      stream);
  TASK_CHECK(len > 0);
  while (at < len) {
    TASK_CHECK(parse_frame(stream + at, len - at, &f) == 0);
    TASK_CHECK(f.gr[0][1].scalefac[13] == pos[frame][0]);
    TASK_CHECK(f.gr[1][1].scalefac[13] == pos[frame][1]);
    at += f.frame_bytes;
    frame++;
  }
  TASK_CHECK(frame == TEST_FRAMES);
  return 0;
}

static int case_cbr_through_transitions(void) {
  unsigned char stream[STREAM_CAP];
  struct parsed_frame f;
  int len, at = 0, frames = 0;

  len = encode_stream(48000, STEREO, 0, cfg_toggle_every_frame,
                      gen_broadband, stream);
  TASK_CHECK(len > 0);
  while (at < len) {
    TASK_CHECK(parse_frame(stream + at, len - at, &f) == 0);
    TASK_CHECK(f.frame_bytes == 384);
    at += f.frame_bytes;
    frames++;
  }
  TASK_CHECK(frames == TEST_FRAMES && len == TEST_FRAMES * 384);
  return 0;
}

static void cfg_enable_then_disable(shine_t enc, int frame) {
  if (frame == 2)
    shine_set_intensity(enc, 11, 1, 5);
  if (frame == 5)
    shine_set_intensity(enc, -1, 0, 0);
}

static int case_disable_restores_bytes(void) {
  unsigned char plain[STREAM_CAP], toggled[STREAM_CAP];
  int len_plain, len_toggled;

  len_plain = encode_stream(48000, STEREO, 0, NULL, gen_broadband, plain);
  len_toggled = encode_stream(48000, STEREO, 0, cfg_enable_then_disable,
                              gen_broadband, toggled);
  TASK_CHECK(len_plain > 0 && len_plain == len_toggled);

  /* Frames after the disable (index 5..7) must match a never-enabled
   * encoder because intensity merging never touches analysis history. */
  {
    struct parsed_frame fp, ft;
    int at_p = 5 * 384, at_t = 5 * 384, k;
    for (k = 0; k < 3; k++) {
      TASK_CHECK(parse_frame(plain + at_p, len_plain - at_p, &fp) == 0);
      TASK_CHECK(parse_frame(toggled + at_t, len_toggled - at_t, &ft) == 0);
      TASK_CHECK(memcmp(plain + at_p, toggled + at_t,
                        (size_t)fp.frame_bytes) == 0);
      at_p += fp.frame_bytes;
      at_t += ft.frame_bytes;
    }
  }
  return 0;
}

static int case_invalid_then_continue_encoding(void) {
  unsigned char stream[STREAM_CAP];
  struct decoded decoded[TEST_FRAMES];
  struct parsed_frame f;
  int len, frames, at = 0;

  len = encode_stream(48000, STEREO, 0, cfg_reject_mid_stream, gen_broadband,
                      stream);
  TASK_CHECK(len > 0);
  frames = decode_stream(stream, len, decoded, TEST_FRAMES);
  TASK_CHECK(frames == TEST_FRAMES);
  while (at < len) {
    TASK_CHECK(parse_frame(stream + at, len - at, &f) == 0);
    TASK_CHECK(f.frame_bytes == 384);
    at += f.frame_bytes;
  }
  TASK_CHECK(decoded[TEST_FRAMES - 1].samples == SAMPLES_PER_FRAME);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Case registry                                                       */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    /* syntax */
    {"syntax", "header_and_mode_ext", case_header_and_mode_ext},
    {"syntax", "initial_joint_mode_restored", case_initial_joint_mode_restored},
    {"syntax", "scalefactor_layout", case_scalefactor_layout},
    {"syntax", "part23_and_constant_frame", case_part23_and_constant_frame},
    {"syntax", "silent_right_granule", case_silent_right_granule},
    {"syntax", "rejected_settings", case_rejected_settings},
    {"syntax", "rejected_call_keeps_state", case_rejected_call_keeps_state},
    {"syntax", "entry_points_identical", case_entry_points_identical},
    {"syntax", "padding_44k", case_padding_44k},
    {"syntax", "supported_rates", case_supported_rates},
    {"syntax", "disabled_bytes_unchanged", case_disabled_bytes_unchanged},
    /* pan */
    {"pan", "extremes_and_center", case_pan_extremes_and_center},
    {"pan", "tangent_positions", case_tangent_positions},
    {"pan", "granule_pan_differs", case_granule_pan_differs},
    {"pan", "high_frequency_present", case_high_frequency_present},
    {"pan", "lr_below_boundary_preserved", case_lr_below_boundary_preserved},
    {"pan", "silent_right_decodes", case_silent_right_decodes},
    /* transitions */
    {"transitions", "frame_by_frame_toggle", case_frame_by_frame_toggle},
    {"transitions", "boundary_changes", case_boundary_changes},
    {"transitions", "position_transitions", case_position_transitions},
    {"transitions", "cbr_through_transitions", case_cbr_through_transitions},
    {"transitions", "disable_restores_bytes", case_disable_restores_bytes},
    {"transitions", "invalid_then_continue", case_invalid_then_continue_encoding},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
