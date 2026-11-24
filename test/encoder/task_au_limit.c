#define _POSIX_C_SOURCE 200112L

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ixheaac_type_def.h"
#include "ixheaace_api.h"
#include "ixheaace_error_codes.h"
#include "task_au_limit.h"
#include "ixheaacd_apicmd_standards.h"
#include "ixheaacd_aac_config.h"
#include "task_support/check.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

IA_ERRORCODE ixheaacd_dec_api(pVOID p_obj, WORD32 cmd, WORD32 idx, pVOID value);

#define AU_TEST_FRAMES 48

typedef struct {
  ixheaace_input_config cfg;
  ixheaace_output_config out;
  int open;
  int channels;
  WORD32 lo;
  WORD32 hi;
} AuEnc;

typedef struct {
  unsigned char *data;
  size_t len;
  size_t cap;
  int lengths[AU_TEST_FRAMES * 2];
  int raw[AU_TEST_FRAMES * 2];
  int count;
} AuStream;

typedef struct {
  pVOID obj;
  pVOID tabs;
  pVOID allocations[8];
  int allocation_count;
  unsigned char *input;
  unsigned char *output;
  WORD32 input_size;
  WORD32 channels;
} AuDecoder;

static void *au_aligned_alloc(UWORD32 size, UWORD32 alignment) {
  void *ptr = NULL;
  if (alignment < sizeof(void *)) alignment = sizeof(void *);
  if (posix_memalign(&ptr, alignment, size ? size : 1) != 0) return NULL;
  return ptr;
}

static void au_aligned_free(pVOID ptr) { free(ptr); }

static int16_t au_sample(long sample_index, int channel, int sample_rate) {
  long frame = sample_index / 1024;
  long j = sample_index % 1024;
  int cycle = (int)(frame % 32);
  double t = (double)sample_index / (double)sample_rate;
  uint32_t bits = (uint32_t)sample_index * 2654435761u +
                  (uint32_t)(channel + 1) * 2246822519u + 0x9e3779b9u;
  bits ^= bits >> 15;
  bits *= 2246822519u;
  bits ^= bits >> 13;
  double independent_noise = (double)((int)(bits >> 17) - 16384) * 0.45;
  double value = independent_noise * 0.18; /* always-present decorrelated floor */

  if (cycle < 4) {
    value = 0.0; /* explicit silence */
  } else if (cycle < 12) {
    long impulse_pos = j % 96;
    double envelope = exp(-(double)impulse_pos / 13.0);
    double frequency = 520.0 + 180.0 * (frame % 5);
    value += envelope * (11500.0 * sin(2.0 * M_PI * frequency * t) +
                         independent_noise * 0.35);
  } else if (cycle < 20) {
    value += 6500.0 * sin(2.0 * M_PI * (330.0 + 110.0 * (channel + 1)) * t);
    value += 2600.0 * sin(2.0 * M_PI * 880.0 * t);
  } else {
    value += independent_noise * 1.35; /* strong independent L/R noise */
  }

  if (value > 32767.0) value = 32767.0;
  if (value < -32768.0) value = -32768.0;
  return (int16_t)lrint(value);
}

static void au_fill_pcm(AuEnc *enc, long frame_index) {
  int16_t *samples = (int16_t *)enc->out.mem_info_table[IA_MEMTYPE_INPUT].mem_ptr;
  int total_samples = enc->out.input_size / (int)sizeof(int16_t);
  int i;
  for (i = 0; i < total_samples; i++) {
    int channel = i % enc->channels;
    long sample_in_frame = i / enc->channels;
    long sample_index = frame_index * 1024 + sample_in_frame;
    samples[i] = au_sample(sample_index, channel, (int)enc->cfg.i_samp_freq);
  }
}

static WORD32 au_expected_min(int bitrate, int sample_rate) {
  WORD64 numerator = (WORD64)bitrate * 1024;
  WORD32 average = (WORD32)((numerator + 8LL * sample_rate - 1) /
                            (8LL * sample_rate));
  return average + 64;
}

static int au_enc_open_cap(AuEnc *enc, int channels, int sample_rate, int bitrate,
                           int dual_mono, int pce, WORD32 initial_cap) {
  IA_ERRORCODE err;
  memset(enc, 0, sizeof(*enc));
  enc->channels = channels;
  enc->cfg.ui_pcm_wd_sz = 16;
  enc->cfg.i_bitrate = bitrate;
  enc->cfg.aot = AOT_AAC_LC;
  enc->cfg.i_channels = channels;
  enc->cfg.i_samp_freq = sample_rate;
  enc->cfg.i_use_es = 1;
  enc->cfg.i_use_adts = 1;
  enc->cfg.frame_length = 1024;
  enc->cfg.frame_cmd_flag = 1;
  enc->cfg.write_program_config_element = pce;
  enc->cfg.max_au_bytes = initial_cap;
  enc->cfg.aac_config.bitrate = bitrate;
  enc->cfg.aac_config.inv_quant = 2;
  enc->cfg.aac_config.bitreservoir_size = 768;
  enc->cfg.aac_config.length = 64 * 1024;
  enc->cfg.aac_config.dual_mono = dual_mono;
  enc->out.malloc_xheaace = au_aligned_alloc;
  enc->out.free_xheaace = au_aligned_free;

  err = ixheaace_create(&enc->cfg, &enc->out);
  if (err != IA_NO_ERROR) return (int)err;
  enc->open = 1;
  enc->lo = au_expected_min(bitrate, sample_rate);
  enc->hi = 768 * channels;
  return 0;
}

static int au_enc_open(AuEnc *enc, int channels, int sample_rate, int bitrate,
                       int dual_mono, int pce) {
  return au_enc_open_cap(enc, channels, sample_rate, bitrate, dual_mono, pce, 0);
}

static void au_enc_close(AuEnc *enc) {
  if (enc->open) {
    ixheaace_delete(&enc->out);
    enc->open = 0;
  }
}

static int au_adts_header_length(const unsigned char *p, size_t len) {
  if (len < 7 || p[0] != 0xff || (p[1] & 0xf0) != 0xf0) return -1;
  return (p[1] & 0x01) ? 7 : 9;
}

static int au_adts_frame_length(const unsigned char *p) {
  return ((p[3] & 0x03) << 11) | (p[4] << 3) | (p[5] >> 5);
}

static int au_adts_channels(const unsigned char *p) {
  return ((p[2] & 0x01) << 2) | ((p[3] >> 6) & 0x03);
}

static int au_stream_append(AuStream *stream, const void *data, int bytes,
                            int max_au_bytes) {
  int header;
  if (stream->count >= AU_TEST_FRAMES * 2 || bytes <= 0) return 1;
  if (stream->len + (size_t)bytes + 16 > stream->cap) {
    size_t new_cap = stream->cap ? stream->cap * 2 : (1u << 20);
    unsigned char *next = realloc(stream->data, new_cap);
    if (!next) return 2;
    stream->data = next;
    stream->cap = new_cap;
  }
  memcpy(stream->data + stream->len, data, (size_t)bytes);
  stream->lengths[stream->count] = bytes;
  header = au_adts_header_length((const unsigned char *)data, (size_t)bytes);
  if (header < 0 || bytes < header) return 3;
  stream->raw[stream->count] = bytes - header;
  if (max_au_bytes > 0 && stream->raw[stream->count] > max_au_bytes) return 4;
  stream->len += (size_t)bytes;
  stream->count++;
  return 0;
}

static int au_decoder_open(AuDecoder *dec) {
  IA_ERRORCODE err;
  WORD32 api_size = 0;
  WORD32 tabs_size = 0;
  WORD32 i;
  WORD32 is_mp4 = 0;
  WORD32 pcm_bits = 16;
  WORD32 conceal = 0;

  memset(dec, 0, sizeof(*dec));
  err = ixheaacd_dec_api(NULL, IA_API_CMD_GET_API_SIZE, 0, &api_size);
  if (err || api_size <= 0) return 1;
  dec->obj = au_aligned_alloc((UWORD32)api_size, 16);
  if (!dec->obj) return 2;
  dec->allocations[dec->allocation_count++] = dec->obj;

  err = ixheaacd_dec_api(dec->obj, IA_API_CMD_INIT,
                         IA_CMD_TYPE_INIT_API_PRE_CONFIG_PARAMS, NULL);
  if (err) return 3;
  ixheaacd_dec_api(dec->obj, IA_API_CMD_SET_CONFIG_PARAM,
                   IA_ENHAACPLUS_DEC_CONFIG_PARAM_ISMP4, &is_mp4);
  ixheaacd_dec_api(dec->obj, IA_API_CMD_SET_CONFIG_PARAM,
                   IA_ENHAACPLUS_DEC_CONFIG_PARAM_PCM_WDSZ, &pcm_bits);
  ixheaacd_dec_api(dec->obj, IA_API_CMD_SET_CONFIG_PARAM,
                   IA_XHEAAC_DEC_CONFIG_ERROR_CONCEALMENT, &conceal);

  err = ixheaacd_dec_api(dec->obj, IA_API_CMD_GET_MEMTABS_SIZE, 0, &tabs_size);
  if (err || tabs_size <= 0) return 4;
  dec->tabs = au_aligned_alloc((UWORD32)tabs_size, 16);
  if (!dec->tabs) return 5;
  dec->allocations[dec->allocation_count++] = dec->tabs;
  err = ixheaacd_dec_api(dec->obj, IA_API_CMD_SET_MEMTABS_PTR, 0, dec->tabs);
  if (err) return 6;
  err = ixheaacd_dec_api(dec->obj, IA_API_CMD_INIT,
                         IA_CMD_TYPE_INIT_API_POST_CONFIG_PARAMS, NULL);
  if (err) return 7;

  for (i = 0; i < 4; i++) {
    WORD32 size = 0, alignment = 0, type = 0;
    void *raw;
    err = ixheaacd_dec_api(dec->obj, IA_API_CMD_GET_MEM_INFO_SIZE, i, &size);
    err |= ixheaacd_dec_api(dec->obj, IA_API_CMD_GET_MEM_INFO_ALIGNMENT, i,
                            &alignment);
    err |= ixheaacd_dec_api(dec->obj, IA_API_CMD_GET_MEM_INFO_TYPE, i, &type);
    if (err || size <= 0) return 8;
    raw = au_aligned_alloc((UWORD32)size, (UWORD32)alignment);
    if (!raw) return 9;
    if (dec->allocation_count < 8) dec->allocations[dec->allocation_count++] = raw;
    err = ixheaacd_dec_api(dec->obj, IA_API_CMD_SET_MEM_PTR, i, raw);
    if (err) return 10;
    if (type == IA_MEMTYPE_INPUT) {
      dec->input = raw;
      dec->input_size = size;
    } else if (type == IA_MEMTYPE_OUTPUT) {
      dec->output = raw;
    }
  }
  dec->channels = 2;
  return 0;
}

static void au_decoder_close(AuDecoder *dec) {
  int i;
  for (i = dec->allocation_count - 1; i >= 0; i--) free(dec->allocations[i]);
  memset(dec, 0, sizeof(*dec));
}

static int au_decode_stream(AuStream *stream, int expected_channels,
                            long *decoded_frames, long long *channel_energy) {
  AuDecoder dec;
  IA_ERRORCODE err;
  WORD32 init_bytes;
  WORD32 init_done = 0;
  WORD32 out_bytes = 0;
  WORD32 consumed = 0;
  WORD32 cfg_channels = 0;
  size_t offset = 0;
  int packet;
  int16_t *pcm_copy = NULL;
  size_t pcm_bytes = 0;

  if (au_decoder_open(&dec)) return 100;
  if (stream->count <= 0 || (WORD32)stream->lengths[0] > dec.input_size) {
    au_decoder_close(&dec);
    return 101;
  }

  {
    int init_packets = stream->count < 4 ? stream->count : 4;
    int init_packet;
    size_t init_len = 0;
    int init_try;
    for (init_packet = 0; init_packet < init_packets; init_packet++) {
      init_len += (size_t)stream->lengths[init_packet];
    }
    if (init_len > (size_t)dec.input_size) {
      au_decoder_close(&dec);
      return 109;
    }
    memcpy(dec.input, stream->data, init_len);
    for (init_try = 0; init_try < 8 && !init_done; init_try++) {
      init_bytes = (WORD32)init_len;
      ixheaacd_dec_api(dec.obj, IA_API_CMD_SET_INPUT_BYTES, 0, &init_bytes);
      err = ixheaacd_dec_api(dec.obj, IA_API_CMD_INIT,
                             IA_CMD_TYPE_INIT_PROCESS, NULL);
      if (err & IA_FATAL_ERROR) {
        au_decoder_close(&dec);
        return 102;
      }
      ixheaacd_dec_api(dec.obj, IA_API_CMD_INIT,
                       IA_CMD_TYPE_INIT_DONE_QUERY, &init_done);
    }
  }
  if (!init_done) {
    au_decoder_close(&dec);
    return 110;
  }
  ixheaacd_dec_api(dec.obj, IA_API_CMD_GET_CONFIG_PARAM,
                   IA_ENHAACPLUS_DEC_CONFIG_PARAM_NUM_CHANNELS, &cfg_channels);
  if (cfg_channels > 0) dec.channels = cfg_channels;
  if (dec.channels != expected_channels) {
    fprintf(stderr, "decoder channel mismatch cfg=%ld expected=%d init_done=%u err=0x%x\n",
            (long)cfg_channels, expected_channels, init_done, (unsigned)err);
    au_decoder_close(&dec);
    return 103;
  }

  for (packet = 0; packet < stream->count; packet++) {
    int frame_bytes = stream->lengths[packet];
    WORD32 use_bytes = frame_bytes;
    WORD32 done = 0;
    if (frame_bytes <= 0 || frame_bytes > dec.input_size) {
      au_decoder_close(&dec);
      free(pcm_copy);
      return 104;
    }
    memcpy(dec.input, stream->data + offset, (size_t)frame_bytes);
    err = ixheaacd_dec_api(dec.obj, IA_API_CMD_SET_INPUT_BYTES, 0, &use_bytes);
    if (err) {
      au_decoder_close(&dec);
      free(pcm_copy);
      return 105;
    }
    err = ixheaacd_dec_api(dec.obj, IA_API_CMD_EXECUTE,
                           IA_CMD_TYPE_DO_EXECUTE, NULL);
    if (err != IA_NO_ERROR) {
      au_decoder_close(&dec);
      free(pcm_copy);
      return 106;
    }
    ixheaacd_dec_api(dec.obj, IA_API_CMD_EXECUTE, IA_CMD_TYPE_DONE_QUERY, &done);
    ixheaacd_dec_api(dec.obj, IA_API_CMD_GET_OUTPUT_BYTES, 0, &out_bytes);
    ixheaacd_dec_api(dec.obj, IA_API_CMD_GET_CURIDX_INPUT_BUF, 0, &consumed);
    if (consumed <= 0 || consumed > (WORD32)frame_bytes) {
      au_decoder_close(&dec);
      free(pcm_copy);
      return 107;
    }
    if (out_bytes > 0) {
      int16_t *grown = realloc(pcm_copy, pcm_bytes + out_bytes);
      if (!grown) {
        au_decoder_close(&dec);
        free(pcm_copy);
        return 108;
      }
      pcm_copy = grown;
      memcpy((unsigned char *)pcm_copy + pcm_bytes, dec.output, out_bytes);
      pcm_bytes += out_bytes;
    }
    offset += (size_t)frame_bytes;
  }

  *decoded_frames = (long)(pcm_bytes / (sizeof(int16_t) * dec.channels));
  if (channel_energy) {
    long total_samples = (long)(pcm_bytes / sizeof(int16_t));
    long i;
    channel_energy[0] = channel_energy[1] = 0;
    for (i = 0; i < total_samples; i++) {
      int ch = (int)(i % dec.channels);
      int32_t s = pcm_copy[i];
      if (ch < 2) channel_energy[ch] += (long long)s * s;
    }
  }
  free(pcm_copy);
  au_decoder_close(&dec);
  return 0;
}

static int au_cap_for_phase(const AuEnc *enc, int frame) {
  static const int numerator[6] = {0, 1, 2, 4, 8, 0};
  int phase = (frame / 4) % 6;
  if (numerator[phase] == 0) return 0;
  if (numerator[phase] == 1) return enc->lo;
  if (numerator[phase] == 2) return enc->lo + 16;
  if (numerator[phase] == 4) return (enc->lo + enc->hi) / 2;
  return enc->hi;
}

static int au_run_decoded_profile(int channels, int sample_rate, int bitrate,
                                  int dual_mono, int pce) {
  AuEnc enc;
  AuStream stream;
  long frames = 0;
  long long energy[2] = {0, 0};
  int f;
  int rc;
  IA_ERRORCODE err;

  memset(&stream, 0, sizeof(stream));
  rc = au_enc_open(&enc, channels, sample_rate, bitrate, dual_mono, pce);
  if (rc) return rc;

  for (f = 0; f < AU_TEST_FRAMES; f++) {
    int cap = au_cap_for_phase(&enc, f);
    enc.cfg.max_au_bytes = cap;
    au_fill_pcm(&enc, f);
    err = ixheaace_process(enc.out.pv_ia_process_api_obj, &enc.cfg, &enc.out);
    if (err != IA_NO_ERROR && (err & IA_FATAL_ERROR)) {
      au_enc_close(&enc);
      free(stream.data);
      return 200 + (int)err;
    }
    if (enc.out.i_out_bytes <= 0) {
      au_enc_close(&enc);
      free(stream.data);
      return 201;
    }
    rc = au_stream_append(&stream,
                          enc.out.mem_info_table[IA_MEMTYPE_OUTPUT].mem_ptr,
                          enc.out.i_out_bytes, cap);
    if (rc) {
      fprintf(stderr, "append fail profile ch=%d sr=%d f=%d cap=%d out=%d err=0x%x rc=%d\n",
              channels, sample_rate, f, cap, enc.out.i_out_bytes, (unsigned)err, rc);
      au_enc_close(&enc);
      free(stream.data);
      return 202 + rc;
    }
  }

  au_enc_close(&enc);
  rc = au_decode_stream(&stream, channels, &frames, energy);
  if (rc) {
    free(stream.data);
    return 300 + rc;
  }

  if (frames < AU_TEST_FRAMES * 1024L - 2048L) rc = 400;
  if (energy[0] < 100000000LL) rc = 401;
  if (channels == 2 && energy[1] < 100000000LL) {
    fprintf(stderr, "low second channel frames=%ld e0=%lld e1=%lld\n", frames,
            energy[0], energy[1]);
    rc = 402;
  }
  free(stream.data);
  return rc;
}

/* ------------------------------ capacity group --------------------------- */

static int case_capacity_bounds(void) {
  const struct { int ch, sr, br, dual; } profiles[] = {
      {1, 44100, 64000, 0}, {1, 48000, 64000, 0},
      {2, 44100, 96000, 0}, {2, 48000, 96000, 0},
      {2, 44100, 96000, 1}, {2, 48000, 96000, 1}};
  size_t p;
  for (p = 0; p < sizeof(profiles) / sizeof(profiles[0]); p++) {
    AuEnc enc;
    WORD32 lo = au_expected_min(profiles[p].br, profiles[p].sr);
    WORD32 hi = 768 * profiles[p].ch;
    TASK_CHECK(au_enc_open(&enc, profiles[p].ch, profiles[p].sr,
                           profiles[p].br, profiles[p].dual, 0) == 0);
    TASK_CHECK(enc.lo == lo);
    TASK_CHECK(enc.hi == hi);
    TASK_CHECK(lo <= hi);
    if (profiles[p].dual) {
      TASK_CHECK(ixheaace_task_au_get_num_elements(
                     enc.out.pv_ia_process_api_obj) == 2);
      TASK_CHECK(ixheaace_task_au_get_element_type(
                     enc.out.pv_ia_process_api_obj, 0) == 0 /* ID_SCE */);
      TASK_CHECK(ixheaace_task_au_get_element_type(
                     enc.out.pv_ia_process_api_obj, 1) == 0 /* ID_SCE */);
      TASK_CHECK(ixheaace_task_au_get_element_tag(
                     enc.out.pv_ia_process_api_obj, 0) == 0);
      TASK_CHECK(ixheaace_task_au_get_element_tag(
                     enc.out.pv_ia_process_api_obj, 1) == 1);
    } else if (profiles[p].ch == 1) {
      TASK_CHECK(ixheaace_task_au_get_element_type(
                     enc.out.pv_ia_process_api_obj, 0) == 0 /* ID_SCE */);
    } else {
      TASK_CHECK(ixheaace_task_au_get_element_type(
                     enc.out.pv_ia_process_api_obj, 0) == 1 /* ID_CPE */);
    }

    enc.cfg.max_au_bytes = lo;
    au_fill_pcm(&enc, 1);
    TASK_CHECK(ixheaace_process(enc.out.pv_ia_process_api_obj, &enc.cfg,
                                &enc.out) == 0);
    TASK_CHECK(enc.out.i_out_bytes > 7);
    TASK_CHECK(enc.out.i_out_bytes - 7 <= lo);
    if (profiles[p].dual) {
      const unsigned char *raw =
          (const unsigned char *)enc.out.mem_info_table[IA_MEMTYPE_OUTPUT]
              .mem_ptr + 7;
      /* ID_SCE (0) and element instance tag 0 occupy the first 7 bits. */
      TASK_CHECK((raw[0] & 0xfe) == 0);
    }

    enc.cfg.max_au_bytes = hi;
    au_fill_pcm(&enc, 2);
    TASK_CHECK(ixheaace_process(enc.out.pv_ia_process_api_obj, &enc.cfg,
                                &enc.out) == 0);
    TASK_CHECK(enc.out.i_out_bytes - 7 <= hi);
    au_enc_close(&enc);

    TASK_CHECK(au_enc_open_cap(&enc, profiles[p].ch, profiles[p].sr,
                               profiles[p].br, profiles[p].dual, 0, lo) == 0);
    au_enc_close(&enc);
    TASK_CHECK(au_enc_open_cap(&enc, profiles[p].ch, profiles[p].sr,
                               profiles[p].br, profiles[p].dual, 0, hi) == 0);
    au_enc_close(&enc);
  }
  return 0;
}

static int case_capacity_invalid(void) {
  AuEnc enc;
  IA_ERRORCODE err;
  TASK_CHECK(au_enc_open(&enc, 2, 44100, 96000, 0, 0) == 0);

  au_fill_pcm(&enc, 7);
  enc.cfg.max_au_bytes = -1;
  enc.out.i_out_bytes = 1234;
  enc.out.i_bytes_consumed = 99;
  err = ixheaace_process(enc.out.pv_ia_process_api_obj, &enc.cfg, &enc.out);
  TASK_CHECK(err == IA_EXHEAACE_CONFIG_FATAL_MAX_AU_BYTES_NEGATIVE);
  TASK_CHECK(enc.out.i_out_bytes == 0);
  TASK_CHECK(enc.out.i_bytes_consumed == 0);

  enc.cfg.max_au_bytes = enc.lo - 1;
  err = ixheaace_process(enc.out.pv_ia_process_api_obj, &enc.cfg, &enc.out);
  TASK_CHECK(err == IA_EXHEAACE_CONFIG_FATAL_MAX_AU_BYTES_OUT_OF_RANGE);
  TASK_CHECK(enc.out.i_out_bytes == 0);

  enc.cfg.max_au_bytes = enc.hi + 1;
  err = ixheaace_process(enc.out.pv_ia_process_api_obj, &enc.cfg, &enc.out);
  TASK_CHECK(err == IA_EXHEAACE_CONFIG_FATAL_MAX_AU_BYTES_OUT_OF_RANGE);
  TASK_CHECK(enc.out.i_out_bytes == 0);
  au_enc_close(&enc);

  TASK_CHECK(au_enc_open_cap(&enc, 2, 44100, 96000, 0, 0, -8) ==
             IA_EXHEAACE_CONFIG_FATAL_MAX_AU_BYTES_NEGATIVE);
  au_enc_close(&enc);
  TASK_CHECK(au_enc_open_cap(&enc, 2, 44100, 96000, 0, 0, 10) ==
             IA_EXHEAACE_CONFIG_FATAL_MAX_AU_BYTES_OUT_OF_RANGE);
  au_enc_close(&enc);
  TASK_CHECK(au_enc_open_cap(&enc, 2, 44100, 96000, 0, 0, 2000) ==
             IA_EXHEAACE_CONFIG_FATAL_MAX_AU_BYTES_OUT_OF_RANGE);
  au_enc_close(&enc);
  return 0;
}

static int case_capacity_unsupported(void) {
  ixheaace_input_config cfg;
  ixheaace_output_config out;
  IA_ERRORCODE err;

  /* AAC-LD/512 is outside the first supported scope. */
  memset(&cfg, 0, sizeof(cfg));
  memset(&out, 0, sizeof(out));
  cfg.ui_pcm_wd_sz = 16;
  cfg.i_bitrate = 96000;
  cfg.aot = AOT_AAC_LD;
  cfg.i_channels = 2;
  cfg.i_samp_freq = 48000;
  cfg.i_use_es = 1;
  cfg.i_use_adts = 1;
  cfg.frame_length = 512;
  cfg.frame_cmd_flag = 1;
  cfg.max_au_bytes = 200;
  cfg.aac_config.bitrate = 96000;
  cfg.aac_config.inv_quant = 2;
  cfg.aac_config.bitreservoir_size = 768;
  cfg.aac_config.length = 64 * 1024;
  out.malloc_xheaace = au_aligned_alloc;
  out.free_xheaace = au_aligned_free;
  err = ixheaace_create(&cfg, &out);
  TASK_CHECK(err == IA_EXHEAACE_CONFIG_FATAL_MAX_AU_BYTES_UNSUPPORTED_MODE);
  ixheaace_delete(&out);

  /* The same unsupported mode may be created with limit zero and must reject
     a positive value on a later process call without advancing state. */
  cfg.max_au_bytes = 0;
  memset(&out, 0, sizeof(out));
  out.malloc_xheaace = au_aligned_alloc;
  out.free_xheaace = au_aligned_free;
  err = ixheaace_create(&cfg, &out);
  TASK_CHECK(err == IA_NO_ERROR);
  cfg.max_au_bytes = 200;
  out.i_out_bytes = 777;
  err = ixheaace_process(out.pv_ia_process_api_obj, &cfg, &out);
  TASK_CHECK(err == IA_EXHEAACE_CONFIG_FATAL_MAX_AU_BYTES_UNSUPPORTED_MODE);
  TASK_CHECK(out.i_out_bytes == 0);
  TASK_CHECK(out.i_bytes_consumed == 0);
  ixheaace_delete(&out);

  /* AAC-LC/960 is valid encoder configuration but outside the 1024 scope. */
  memset(&cfg, 0, sizeof(cfg));
  memset(&out, 0, sizeof(out));
  cfg.ui_pcm_wd_sz = 16;
  cfg.i_bitrate = 96000;
  cfg.aot = AOT_AAC_LC;
  cfg.i_channels = 2;
  cfg.i_samp_freq = 48000;
  cfg.i_use_es = 1;
  cfg.i_use_adts = 1;
  cfg.frame_length = 960;
  cfg.frame_cmd_flag = 1;
  cfg.max_au_bytes = 300;
  cfg.aac_config.bitrate = 96000;
  cfg.aac_config.inv_quant = 2;
  cfg.aac_config.bitreservoir_size = 768;
  cfg.aac_config.length = 64 * 1024;
  out.malloc_xheaace = au_aligned_alloc;
  out.free_xheaace = au_aligned_free;
  err = ixheaace_create(&cfg, &out);
  TASK_CHECK(err == IA_EXHEAACE_CONFIG_FATAL_MAX_AU_BYTES_UNSUPPORTED_MODE);
  ixheaace_delete(&out);
  return 0;
}

static int case_capacity_mono_44(void) {
  int rc = au_run_decoded_profile(1, 44100, 64000, 0, 0);
  if (rc) fprintf(stderr, "mono decode profile rc=%d\n", rc);
  TASK_CHECK(rc == 0);
  return 0;
}
static int case_capacity_cpe_48(void) {
  int rc = au_run_decoded_profile(2, 48000, 96000, 0, 0);
  if (rc) fprintf(stderr, "cpe48 profile rc=%d\n", rc);
  TASK_CHECK(rc == 0);
  return 0;
}
static int case_capacity_cpe_pce_48(void) {
  int rc = au_run_decoded_profile(2, 48000, 128000, 0, 1);
  if (rc) fprintf(stderr, "cpe-pce48 profile rc=%d\n", rc);
  TASK_CHECK(rc == 0);
  return 0;
}
static int case_capacity_dual_44(void) {
  int rc = au_run_decoded_profile(2, 44100, 96000, 1, 1);
  if (rc) fprintf(stderr, "dual44 profile rc=%d\n", rc);
  TASK_CHECK(rc == 0);
  return 0;
}
static int case_capacity_dual_48_pce(void) {
  TASK_CHECK(au_run_decoded_profile(2, 48000, 128000, 1, 1) == 0);
  return 0;
}

/* ------------------------------- history group --------------------------- */

static int au_outputs_equal(const AuEnc *a, const AuEnc *b) {
  if (a->out.i_out_bytes != b->out.i_out_bytes) return 0;
  return memcmp(a->out.mem_info_table[IA_MEMTYPE_OUTPUT].mem_ptr,
                 b->out.mem_info_table[IA_MEMTYPE_OUTPUT].mem_ptr,
                 (size_t)a->out.i_out_bytes) == 0;
}

static int case_history_rejection_no_consume(void) {
  AuEnc guarded, control;
  int f;
  TASK_CHECK(au_enc_open(&guarded, 2, 48000, 96000, 0, 0) == 0);
  TASK_CHECK(au_enc_open(&control, 2, 48000, 96000, 0, 0) == 0);

  for (f = 0; f < 20; f++) {
    int cap = (f >= 8 && f < 14) ? guarded.lo : 0;
    au_fill_pcm(&guarded, f);
    au_fill_pcm(&control, f);

    if (f == 9 || f == 10) {
      IA_ERRORCODE err;
      WORD32 level_before = ixheaace_task_au_get_reservoir_level(
          guarded.out.pv_ia_process_api_obj, 0);
      guarded.cfg.max_au_bytes = (f == 9) ? -2 : guarded.hi + 9;
      err = ixheaace_process(guarded.out.pv_ia_process_api_obj, &guarded.cfg,
                             &guarded.out);
      TASK_CHECK(err != 0);
      TASK_CHECK(guarded.out.i_out_bytes == 0);
      TASK_CHECK(guarded.out.i_bytes_consumed == 0);
      TASK_CHECK(ixheaace_task_au_get_reservoir_level(
                     guarded.out.pv_ia_process_api_obj, 0) == level_before);
    }

    guarded.cfg.max_au_bytes = cap;
    control.cfg.max_au_bytes = cap;
    TASK_CHECK(ixheaace_process(guarded.out.pv_ia_process_api_obj,
                                &guarded.cfg, &guarded.out) == 0);
    TASK_CHECK(ixheaace_process(control.out.pv_ia_process_api_obj,
                                &control.cfg, &control.out) == 0);
    TASK_CHECK(au_outputs_equal(&guarded, &control));
  }

  /* Recovery after a final illegal value must remain deterministic. */
  guarded.cfg.max_au_bytes = -1;
  TASK_CHECK(ixheaace_process(guarded.out.pv_ia_process_api_obj, &guarded.cfg,
                              &guarded.out) ==
             IA_EXHEAACE_CONFIG_FATAL_MAX_AU_BYTES_NEGATIVE);
  guarded.cfg.max_au_bytes = guarded.lo + 8;
  au_fill_pcm(&guarded, 20);
  au_fill_pcm(&control, 20);
  control.cfg.max_au_bytes = guarded.lo + 8;
  TASK_CHECK(ixheaace_process(guarded.out.pv_ia_process_api_obj, &guarded.cfg,
                              &guarded.out) == 0);
  TASK_CHECK(ixheaace_process(control.out.pv_ia_process_api_obj,
                              &control.cfg, &control.out) == 0);
  TASK_CHECK(au_outputs_equal(&guarded, &control));

  au_enc_close(&guarded);
  au_enc_close(&control);
  return 0;
}

static int case_history_reservoir_retained(void) {
  AuEnc limited, normal;
  WORD32 limited_level_after_tight;
  WORD32 normal_level_after_tight;
  WORD32 limited_level_after_relax;
  int f;

  TASK_CHECK(au_enc_open(&limited, 2, 44100, 96000, 0, 0) == 0);
  TASK_CHECK(au_enc_open(&normal, 2, 44100, 96000, 0, 0) == 0);

  for (f = 0; f < 24; f++) {
    int tight = (f >= 8 && f < 16);
    limited.cfg.max_au_bytes = tight ? limited.lo : 0;
    normal.cfg.max_au_bytes = 0;
    au_fill_pcm(&limited, f);
    au_fill_pcm(&normal, f);
    TASK_CHECK(ixheaace_process(limited.out.pv_ia_process_api_obj,
                                &limited.cfg, &limited.out) == 0);
    TASK_CHECK(ixheaace_process(normal.out.pv_ia_process_api_obj,
                                &normal.cfg, &normal.out) == 0);
  }
  limited_level_after_tight = ixheaace_task_au_get_reservoir_level(
      limited.out.pv_ia_process_api_obj, 0);
  normal_level_after_tight = ixheaace_task_au_get_reservoir_level(
      normal.out.pv_ia_process_api_obj, 0);
  TASK_CHECK(limited_level_after_tight >= 0);
  TASK_CHECK(limited_level_after_tight <=
             ixheaace_task_au_get_reservoir_max(
                 limited.out.pv_ia_process_api_obj, 0));
  TASK_CHECK(limited_level_after_tight != normal_level_after_tight);

  limited.cfg.max_au_bytes = 0;
  au_fill_pcm(&limited, 24);
  TASK_CHECK(ixheaace_process(limited.out.pv_ia_process_api_obj, &limited.cfg,
                              &limited.out) == 0);
  limited_level_after_relax = ixheaace_task_au_get_reservoir_level(
      limited.out.pv_ia_process_api_obj, 0);
  TASK_CHECK(limited_level_after_relax >= 0);
  TASK_CHECK(limited_level_after_relax != limited_level_after_tight);

  au_enc_close(&limited);
  au_enc_close(&normal);
  return 0;
}

static int case_history_dual_shared_budget(void) {
  AuEnc enc;
  AuStream stream;
  long frames = 0;
  long long energy[2] = {0, 0};
  int f;
  memset(&stream, 0, sizeof(stream));
  TASK_CHECK(au_enc_open(&enc, 2, 48000, 96000, 1, 1) == 0);
  for (f = 0; f < 32; f++) {
    int cap = (f >= 8 && f < 20) ? enc.lo : 0;
    int rc;
    au_fill_pcm(&enc, f);
    TASK_CHECK(ixheaace_process(enc.out.pv_ia_process_api_obj, &enc.cfg,
                                &enc.out) == 0);
    rc = au_stream_append(&stream,
                          enc.out.mem_info_table[IA_MEMTYPE_OUTPUT].mem_ptr,
                          enc.out.i_out_bytes, cap);
    TASK_CHECK(rc == 0);
  }
  au_enc_close(&enc);
  TASK_CHECK(au_decode_stream(&stream, 2, &frames, energy) == 0);
  TASK_CHECK(frames >= 32 * 1024 - 2048);
  TASK_CHECK(energy[0] > 50000000LL && energy[1] > 50000000LL);
  free(stream.data);
  return 0;
}

/* ---------------------------- compatibility group ------------------------ */

static int case_compat_upper_limit(void) {
  const struct { int ch, sr, br, dual; } profiles[] = {
      {1, 44100, 64000, 0}, {2, 48000, 96000, 0},
      {2, 44100, 96000, 0}, {2, 48000, 128000, 0}};
  size_t p;
  int f;
  for (p = 0; p < sizeof(profiles) / sizeof(profiles[0]); p++) {
    AuEnc zero, wide;
    TASK_CHECK(au_enc_open(&zero, profiles[p].ch, profiles[p].sr,
                           profiles[p].br, profiles[p].dual, 0) == 0);
    TASK_CHECK(au_enc_open(&wide, profiles[p].ch, profiles[p].sr,
                           profiles[p].br, profiles[p].dual, 0) == 0);
    wide.cfg.max_au_bytes = wide.hi;
    for (f = 0; f < 32; f++) {
      au_fill_pcm(&zero, f);
      au_fill_pcm(&wide, f);
      zero.cfg.max_au_bytes = 0;
      TASK_CHECK(ixheaace_process(zero.out.pv_ia_process_api_obj, &zero.cfg,
                                  &zero.out) == 0);
      TASK_CHECK(ixheaace_process(wide.out.pv_ia_process_api_obj, &wide.cfg,
                                  &wide.out) == 0);
      TASK_CHECK(au_outputs_equal(&zero, &wide));
    }
    au_enc_close(&zero);
    au_enc_close(&wide);
  }
  return 0;
}

static int case_compat_adts_headers(void) {
  AuEnc enc;
  int f;
  TASK_CHECK(au_enc_open(&enc, 2, 44100, 96000, 0, 0) == 0);
  for (f = 0; f < 12; f++) {
    const unsigned char *p;
    int hlen, flen;
    enc.cfg.max_au_bytes = (f >= 4 && f < 8) ? enc.lo : enc.hi;
    au_fill_pcm(&enc, f);
    TASK_CHECK(ixheaace_process(enc.out.pv_ia_process_api_obj, &enc.cfg,
                                &enc.out) == 0);
    p = enc.out.mem_info_table[IA_MEMTYPE_OUTPUT].mem_ptr;
    hlen = au_adts_header_length(p, (size_t)enc.out.i_out_bytes);
    TASK_CHECK(hlen == 7);
    flen = au_adts_frame_length(p);
    TASK_CHECK(flen == enc.out.i_out_bytes);
    TASK_CHECK(au_adts_channels(p) == 2);
    TASK_CHECK(flen - hlen <= enc.cfg.max_au_bytes);
  }
  au_enc_close(&enc);
  return 0;
}

const struct task_case task_cases[] = {
    {"capacity", "bounds", case_capacity_bounds},
    {"capacity", "invalid_values", case_capacity_invalid},
    {"capacity", "unsupported_modes", case_capacity_unsupported},
    {"capacity", "mono_44k_decode", case_capacity_mono_44},
    {"capacity", "cpe_48k_decode", case_capacity_cpe_48},
    {"capacity", "cpe_pce_48k_decode", case_capacity_cpe_pce_48},
    {"capacity", "dual_mono_44k_decode", case_capacity_dual_44},
    {"capacity", "dual_mono_48k_pce_decode", case_capacity_dual_48_pce},
    {"history", "rejection_does_not_consume", case_history_rejection_no_consume},
    {"history", "reservoir_retained", case_history_reservoir_retained},
    {"history", "dual_sce_shared_budget", case_history_dual_shared_budget},
    {"compatibility", "upper_limit_byte_identical", case_compat_upper_limit},
    {"compatibility", "adts_headers", case_compat_adts_headers},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
