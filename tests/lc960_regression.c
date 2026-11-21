/*
 * Regression tests for AAC-LC encoding with AACENC_GRANULE_LENGTH = 960.
 *
 * Exercises the production encoder/decoder API end to end: continuous
 * encode -> decode with a deterministic integer PCM generator, checking
 * frame length signalling (encoder info, AudioSpecificConfig frameLengthFlag,
 * decoded samples per frame), long/short window signalling parsed from the
 * raw bitstream, channel correspondence, flush/tail handling, chunked vs.
 * one-shot feeding equivalence and the LATM/LOAS transports. The 960/ADTS
 * combination must be rejected as a configuration error.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aacenc_lib.h"
#include "aacdecoder_lib.h"
#include "task_support/check.h"

#define LC960_SR 48000
#define LC960_FL 960
#define MAX_AU_BYTES 32768
#define MAX_STREAM (1 << 20)

static INT_PCM g_pcm[2 * 48 * 1024];        /* interleaved input */
static INT_PCM g_dec[2 * 32 * 1024];        /* decoder output */
static unsigned char g_au[MAX_AU_BYTES];    /* one access unit */
static unsigned char g_streamA[MAX_STREAM]; /* concatenated bitstream */
static unsigned char g_streamB[MAX_STREAM];
static long g_auOffA[4096], g_auOffB[4096]; /* access unit boundaries */
static int g_nAuA, g_nAuB;

/* ------------------------------------------------------------------ */
/* Deterministic integer PCM generator                                 */
/* ------------------------------------------------------------------ */

static double g_sine[512];
static int g_sine_ready = 0;

static void gen_init(void) {
  int i;
  if (g_sine_ready) return;
  for (i = 0; i < 512; i++) {
    g_sine[i] = sin(2.0 * 3.14159265358979323846 * (double)i / 512.0);
  }
  g_sine_ready = 1;
}

enum {
  GEN_NORMAL = 0, /* tones + texture + transient, per-channel difference */
  GEN_LEFT_ONLY,
  GEN_RIGHT_ONLY,
  GEN_TONE1K /* clean 1 kHz tone, both channels */
};

static INT_PCM gen_sample(unsigned t, int ch, int mode) {
  unsigned ph;
  int v;
  uint32_t lfsr = t * 2654435761u + (uint32_t)ch * 40503u + 1u;
  lfsr ^= lfsr << 13;
  lfsr ^= lfsr >> 17;
  lfsr ^= lfsr << 5;

  switch (mode) {
    case GEN_LEFT_ONLY:
      if (ch != 0) return 0;
      ph = (unsigned)((t * 53u) % 512u);
      return (INT_PCM)((g_sine[ph] * 9000.0) + (int)(lfsr % 801u) - 400);
    case GEN_RIGHT_ONLY:
      if (ch != 1) return 0;
      ph = (unsigned)((t * 41u) % 512u);
      return (INT_PCM)((g_sine[ph] * 9000.0) + (int)(lfsr % 801u) - 400);
    case GEN_TONE1K:
      /* 1000 Hz at 48 kHz: 512 * 1000 / 48000 tabs/sample */
      ph = (unsigned)(((uint64_t)t * 512u * 1000u / 48000u) % 512u);
      return (INT_PCM)(g_sine[ph] * 12000.0);
    default:
      break;
  }

  /* GEN_NORMAL: quiet intro, loud body with a click -> block switching. */
  ph = (unsigned)((t * (ch ? 37u : 23u)) % 512u);
  v = (int)(g_sine[ph] * (t < 4800 ? 600.0 : 10000.0));
  v += (int)(lfsr % 601u) - 300;
  if (t >= 12100 && t < 12106) {
    v += (t & 1) ? 14000 : -14000; /* isolated transient */
  }
  if (v > 30000) v = 30000;
  if (v < -30000) v = -30000;
  return (INT_PCM)v;
}

static void gen_pcm(INT_PCM *dst, unsigned firstSample, unsigned nSamplesPerCh,
                    int nCh, int mode) {
  unsigned i;
  int ch;
  gen_init();
  for (i = 0; i < nSamplesPerCh; i++) {
    for (ch = 0; ch < nCh; ch++) {
      dst[i * nCh + ch] = gen_sample(firstSample + i, ch, mode);
    }
  }
}

/* ------------------------------------------------------------------ */
/* Encoder / decoder helpers                                           */
/* ------------------------------------------------------------------ */

static int enc_open(HANDLE_AACENCODER *pEnc, int aot, int sampleRate, int nCh,
                    int bitrate, int tpType, int granule) {
  HANDLE_AACENCODER enc = NULL;
  if (aacEncOpen(&enc, 0, nCh) != AACENC_OK) return 0;
  if (aacEncoder_SetParam(enc, AACENC_AOT, aot) != AACENC_OK) return 0;
  if (aacEncoder_SetParam(enc, AACENC_SAMPLERATE, sampleRate) != AACENC_OK)
    return 0;
  if (aacEncoder_SetParam(enc, AACENC_CHANNELMODE,
                          (nCh == 1) ? MODE_1 : MODE_2) != AACENC_OK)
    return 0;
  if (aacEncoder_SetParam(enc, AACENC_CHANNELORDER, 1) != AACENC_OK) return 0;
  if (aacEncoder_SetParam(enc, AACENC_BITRATE, bitrate) != AACENC_OK) return 0;
  if (tpType != TT_UNKNOWN &&
      aacEncoder_SetParam(enc, AACENC_TRANSMUX, tpType) != AACENC_OK)
    return 0;
  if (granule != 0 &&
      aacEncoder_SetParam(enc, AACENC_GRANULE_LENGTH, granule) != AACENC_OK)
    return 0;
  *pEnc = enc;
  return 1;
}

/* Feed up to nInSamples (interleaved) from pcm; the encoder consumes at
 * most one frame per call. Output bytes (0 or 1 access unit) are left in
 * g_au and returned. *consumed receives oa.numInSamples.
 * Returns number of output bytes, -1 on error, -2 on EOF. */
static int enc_feed(HANDLE_AACENCODER enc, const INT_PCM *pcm, int nInSamples,
                    int *consumed) {
  AACENC_BufDesc ib = {0}, ob = {0};
  AACENC_InArgs ia = {0};
  AACENC_OutArgs oa = {0};
  int iid = IN_AUDIO_DATA, oid = OUT_BITSTREAM_DATA;
  int is = (nInSamples > 0) ? nInSamples * (int)sizeof(INT_PCM) : 0;
  int os = MAX_AU_BYTES, ie = sizeof(INT_PCM), oe = 1;
  void *ip = (void *)pcm, *op = g_au;
  AACENC_ERROR rc;

  ib.numBufs = ob.numBufs = 1;
  ib.bufs = &ip;
  ob.bufs = &op;
  ib.bufferIdentifiers = &iid;
  ob.bufferIdentifiers = &oid;
  ib.bufSizes = &is;
  ob.bufSizes = &os;
  ib.bufElSizes = &ie;
  ob.bufElSizes = &oe;
  ia.numInSamples = nInSamples;

  rc = aacEncEncode(enc, &ib, &ob, &ia, &oa);
  if (rc == AACENC_ENCODE_EOF) return -2;
  if (rc != AACENC_OK) return -1;
  if (consumed) *consumed = oa.numInSamples;
  return oa.numOutBytes;
}

/* Append the access unit currently in g_au to a stream, recording its
 * boundary (RAW transport is not self-delimiting, so the decoder must be
 * fed one access unit at a time). */
static long stream_put(unsigned char *stream, long *offs, int *nAu,
                       long streamLen, int out) {
  if (out > 0 && stream) {
    memcpy(stream + streamLen, g_au, (size_t)out);
    if (offs && *nAu < 4096) offs[(*nAu)++] = streamLen;
    streamLen += out;
  }
  return streamLen;
}

/* Feed nInSamples starting at pcm, looping until everything is consumed.
 * Appends all output to stream. Returns new stream length or -1. */
static long enc_feed_all(HANDLE_AACENCODER enc, const INT_PCM *pcm,
                         int nInSamples, unsigned char *stream, long *offs,
                         int *nAu, long streamLen, int *consumedTotal) {
  int fed = 0, guard = 0;
  while (fed < nInSamples) {
    int cons = 0;
    int out = enc_feed(enc, pcm + fed, nInSamples - fed, &cons);
    if (out < 0) return -1;
    if (cons <= 0) return -1;
    fed += cons;
    if (consumedTotal) *consumedTotal += cons;
    streamLen = stream_put(stream, offs, nAu, streamLen, out);
    if (++guard > 100000) return -1;
  }
  return streamLen;
}

/* Flush encoder until EOF, appending output to stream. */
static long enc_flush(HANDLE_AACENCODER enc, unsigned char *stream, long *offs,
                      int *nAu, long streamLen) {
  int guard = 0;
  for (;;) {
    int out = enc_feed(enc, NULL, -1, NULL);
    if (out == -2) return streamLen; /* AACENC_ENCODE_EOF */
    if (out < 0) return -1;
    streamLen = stream_put(stream, offs, nAu, streamLen, out);
    if (++guard > 1000) return -1;
  }
}

static HANDLE_AACDECODER dec_open(int tpType, const AACENC_InfoStruct *info) {
  HANDLE_AACDECODER dec = aacDecoder_Open((TRANSPORT_TYPE)tpType, 1);
  if (!dec) return NULL;
  if (info && info->confSize > 0) {
    UCHAR *p = (UCHAR *)info->confBuf;
    UINT n = info->confSize;
    if (aacDecoder_ConfigRaw(dec, &p, &n) != AAC_DEC_OK) {
      aacDecoder_Close(dec);
      return NULL;
    }
  }
  aacDecoder_SetParam(dec, AAC_PCM_LIMITER_ENABLE, 0);
  aacDecoder_SetParam(dec, AAC_CONCEAL_METHOD, 1);
  return dec;
}

/* Decode all complete frames found in stream[0..len). If auOff/nAu are
 * given, the stream is fed to the decoder one access unit at a time
 * (required for the RAW transport). Returns the number of decoded frames
 * or -1 on error. */
static int dec_stream(HANDLE_AACDECODER dec, const unsigned char *stream,
                      long len, const long *auOff, int nAu, long *energyL,
                      long *energyR, int *frameSize, int *nChOut) {
  int frames = 0, au = 0;
  long pos = 0;
  do {
    UCHAR *p;
    UINT bytes;
    if (auOff) {
      long end = (au + 1 < nAu) ? auOff[au + 1] : len;
      p = (UCHAR *)stream + auOff[au];
      bytes = (UINT)(end - auOff[au]);
      au++;
    } else {
      p = (UCHAR *)stream + pos;
      bytes = (UINT)(len - pos);
    }
    {
      UINT valid = bytes;
      UCHAR *pf = p;
      if (aacDecoder_Fill(dec, &pf, &bytes, &valid) != AAC_DEC_OK) return -1;
      if (valid != 0) return -1;
      pos += (long)(pf - p);
    }
    for (;;) {
      CStreamInfo *si;
      int i;
      AAC_DECODER_ERROR dr = aacDecoder_DecodeFrame(
          dec, g_dec, (int)(sizeof(g_dec) / sizeof(g_dec[0])), 0);
      if (dr == AAC_DEC_NOT_ENOUGH_BITS) break;
      if (dr != AAC_DEC_OK) return -1;
      si = aacDecoder_GetStreamInfo(dec);
      if (!si || si->frameSize <= 0 || si->numChannels <= 0) return -1;
      if (frameSize && *frameSize != -1 && *frameSize != si->frameSize)
        return -1;
      if (frameSize) *frameSize = si->frameSize;
      if (nChOut) *nChOut = si->numChannels;
      for (i = 0; i < si->frameSize; i++) {
        int l = g_dec[i * si->numChannels];
        if (energyL) *energyL += (l < 0) ? -l : l;
        if (energyR && si->numChannels > 1) {
          int r = g_dec[i * si->numChannels + 1];
          *energyR += (r < 0) ? -r : r;
        }
      }
      frames++;
    }
  } while (auOff ? (au < nAu) : (pos < len));
  return frames;
}

/* ------------------------------------------------------------------ */
/* Minimal raw_block bitstream parser for window signalling            */
/* ------------------------------------------------------------------ */

typedef struct {
  const unsigned char *p;
  unsigned bitpos;
} BITRD;

static unsigned rd_bits(BITRD *b, int n) {
  unsigned v = 0;
  while (n-- > 0) {
    v = (v << 1) | ((b->p[b->bitpos >> 3] >> (7 - (b->bitpos & 7))) & 1u);
    b->bitpos++;
  }
  return v;
}

/* Parse the first individual_channel_stream info of one raw_data_block.
 * Returns 1 on success. Expects a single SCE or CPE per frame. */
static int parse_ics(const unsigned char *au, int expectCpe, int *seq,
                     int *commonWindow, int *maxSfbShort) {
  BITRD b;
  unsigned id;
  b.p = au;
  b.bitpos = 0;
  id = rd_bits(&b, 3);
  if (id != (expectCpe ? (unsigned)ID_CPE : (unsigned)ID_SCE)) return 0;
  rd_bits(&b, 4); /* element_instance_tag */
  if (expectCpe) {
    *commonWindow = (int)rd_bits(&b, 1);
  } else {
    *commonWindow = 0;
  }
  rd_bits(&b, 1); /* ics_reserved_bit */
  *seq = (int)rd_bits(&b, 2);
  rd_bits(&b, 1); /* window_shape */
  if (*seq == 2) { /* EIGHT_SHORT_SEQUENCE */
    *maxSfbShort = (int)rd_bits(&b, 4);
    rd_bits(&b, 7); /* scale_factor_grouping */
  } else {
    rd_bits(&b, 6); /* max_sfb */
    rd_bits(&b, 1); /* predictor_present */
    *maxSfbShort = -1;
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* frame group                                                         */
/* ------------------------------------------------------------------ */

static int case_frame_length_raw(void) {
  HANDLE_AACENCODER enc = NULL;
  HANDLE_AACDECODER dec;
  AACENC_InfoStruct info = {0};
  long streamLen = 0, energyL = 0, energyR = 0;
  int consumed = 0, frames, frameSize = -1, nChOut = 0, t;

  TASK_CHECK(enc_open(&enc, AOT_AAC_LC, LC960_SR, 2, 128000, TT_MP4_RAW, 960));
  TASK_CHECK(aacEncEncode(enc, NULL, NULL, NULL, NULL) == AACENC_OK);
  TASK_CHECK(aacEncInfo(enc, &info) == AACENC_OK);
  TASK_CHECK(info.frameLength == LC960_FL);
  TASK_CHECK(info.nDelay == 2 * LC960_FL);
  TASK_CHECK(aacEncoder_GetParam(enc, AACENC_GRANULE_LENGTH) == LC960_FL);

  /* AudioSpecificConfig: AOT(5) srIdx(4) chanConfig(4) frameLengthFlag(1) */
  TASK_CHECK(info.confSize == 2);
  TASK_CHECK((info.confBuf[0] >> 3) == AOT_AAC_LC);
  TASK_CHECK((info.confBuf[1] & 0x04) != 0); /* frameLengthFlag = 1 -> 960 */

  dec = dec_open(TT_MP4_RAW, &info);
  TASK_CHECK(dec != NULL);

  gen_pcm(g_pcm, 0, 20 * LC960_FL, 2, GEN_NORMAL);
  g_nAuA = 0;
  for (t = 0; t < 20; t++) {
    int cons = 0, out = enc_feed(enc, g_pcm + t * LC960_FL * 2, LC960_FL * 2,
                                 &cons);
    TASK_CHECK(out >= 0);
    TASK_CHECK(cons == LC960_FL * 2); /* 960 samples per channel per AU */
    consumed += cons;
    streamLen = stream_put(g_streamA, g_auOffA, &g_nAuA, streamLen, out);
  }
  TASK_CHECK(consumed == 20 * LC960_FL * 2);
  streamLen = enc_flush(enc, g_streamA, g_auOffA, &g_nAuA, streamLen);
  TASK_CHECK(streamLen > 0);

  frames = dec_stream(dec, g_streamA, streamLen, g_auOffA, g_nAuA, &energyL,
                      &energyR, &frameSize, &nChOut);
  TASK_CHECK(frames >= 20);
  TASK_CHECK(frameSize == LC960_FL); /* decoded samples per channel */
  TASK_CHECK(nChOut == 2);
  TASK_CHECK(energyL > 0 && energyR > 0); /* not silent */

  aacDecoder_Close(dec);
  aacEncClose(&enc);
  return 0;
}

static int case_frame_flush_tail(void) {
  HANDLE_AACENCODER enc = NULL;
  HANDLE_AACDECODER dec;
  AACENC_InfoStruct info = {0};
  long streamLen = 0, energyL = 0, energyR = 0;
  int consumed = 0, frames, frameSize = -1;
  const int nFull = 12, tail = 300; /* tail < one frame */

  TASK_CHECK(enc_open(&enc, AOT_AAC_LC, LC960_SR, 2, 128000, TT_MP4_RAW, 960));
  TASK_CHECK(aacEncEncode(enc, NULL, NULL, NULL, NULL) == AACENC_OK);
  TASK_CHECK(aacEncInfo(enc, &info) == AACENC_OK);
  dec = dec_open(TT_MP4_RAW, &info);
  TASK_CHECK(dec != NULL);

  gen_pcm(g_pcm, 0, nFull * LC960_FL + tail, 2, GEN_NORMAL);
  g_nAuA = 0;
  streamLen = enc_feed_all(enc, g_pcm, (nFull * LC960_FL + tail) * 2,
                           g_streamA, g_auOffA, &g_nAuA, 0, &consumed);
  TASK_CHECK(streamLen >= 0);
  /* Every input sample, including the incomplete trailing frame, must be
   * consumed by the encoder. */
  TASK_CHECK(consumed == (nFull * LC960_FL + tail) * 2);

  streamLen = enc_flush(enc, g_streamA, g_auOffA, &g_nAuA, streamLen);
  TASK_CHECK(streamLen > 0);

  frames = dec_stream(dec, g_streamA, streamLen, g_auOffA, g_nAuA, &energyL,
                      &energyR, &frameSize, NULL);
  TASK_CHECK(frameSize == LC960_FL);
  /* Tail frames cover the padded remainder of the partial frame. */
  TASK_CHECK(frames > nFull);
  TASK_CHECK(frames * LC960_FL >= nFull * LC960_FL + tail);
  TASK_CHECK(energyL > 0 && energyR > 0);

  aacDecoder_Close(dec);
  aacEncClose(&enc);
  return 0;
}

static int case_frame_rates_channels(void) {
  static const int rates[3] = {32000, 44100, 48000};
  int r, nCh;
  for (r = 0; r < 3; r++) {
    for (nCh = 1; nCh <= 2; nCh++) {
      HANDLE_AACENCODER enc = NULL;
      HANDLE_AACDECODER dec;
      AACENC_InfoStruct info = {0};
      long streamLen = 0, energyL = 0, energyR = 0;
      int frames, frameSize = -1, nChOut = 0;

      TASK_CHECK(enc_open(&enc, AOT_AAC_LC, rates[r], nCh,
                          nCh == 1 ? 64000 : 96000, TT_MP4_RAW, 960));
      TASK_CHECK(aacEncEncode(enc, NULL, NULL, NULL, NULL) == AACENC_OK);
      TASK_CHECK(aacEncInfo(enc, &info) == AACENC_OK);
      TASK_CHECK(info.frameLength == LC960_FL);
      TASK_CHECK((info.confBuf[1] & 0x04) != 0);
      dec = dec_open(TT_MP4_RAW, &info);
      TASK_CHECK(dec != NULL);

      gen_pcm(g_pcm, 0, 16 * LC960_FL, nCh, GEN_NORMAL);
      g_nAuA = 0;
      streamLen = enc_feed_all(enc, g_pcm, 16 * LC960_FL * nCh, g_streamA,
                               g_auOffA, &g_nAuA, 0, NULL);
      TASK_CHECK(streamLen >= 0);
      streamLen = enc_flush(enc, g_streamA, g_auOffA, &g_nAuA, streamLen);
      TASK_CHECK(streamLen > 0);
      frames = dec_stream(dec, g_streamA, streamLen, g_auOffA, g_nAuA,
                          &energyL, &energyR, &frameSize, &nChOut);
      TASK_CHECK(frames >= 16);
      TASK_CHECK(frameSize == LC960_FL);
      TASK_CHECK(nChOut == nCh);
      TASK_CHECK(energyL > 0);
      if (nCh == 2) TASK_CHECK(energyR > 0);

      aacDecoder_Close(dec);
      aacEncClose(&enc);
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* windows group                                                       */
/* ------------------------------------------------------------------ */

static int case_window_signaling(void) {
  HANDLE_AACENCODER enc = NULL;
  AACENC_InfoStruct info = {0};
  int seenLong = 0, seenStart = 0, seenShort = 0, seenStop = 0;
  int shortMaxSfbOk = 1, commonOk = 1, parseOk = 1, nAus = 0;
  int t, out;

  TASK_CHECK(enc_open(&enc, AOT_AAC_LC, LC960_SR, 2, 128000, TT_MP4_RAW, 960));
  TASK_CHECK(aacEncEncode(enc, NULL, NULL, NULL, NULL) == AACENC_OK);
  TASK_CHECK(aacEncInfo(enc, &info) == AACENC_OK);

  gen_pcm(g_pcm, 0, 24 * LC960_FL, 2, GEN_NORMAL);
  for (t = 0; t < 24 + 4; t++) { /* 4 extra flush calls for the delay */
    int seq = -1, common = -1, maxSfbShort = -1;
    if (t < 24) {
      out = enc_feed(enc, g_pcm + t * LC960_FL * 2, LC960_FL * 2, NULL);
    } else {
      out = enc_feed(enc, NULL, -1, NULL);
      if (out == -2) break;
    }
    TASK_CHECK(out >= 0);
    if (out == 0) continue; /* encoder delay: no AU yet */
    nAus++;
    if (!parse_ics(g_au, 1, &seq, &common, &maxSfbShort)) {
      parseOk = 0;
      continue;
    }
    if (common != 1) commonOk = 0; /* stereo must share one window */
    switch (seq) {
      case 0:
        seenLong = 1;
        break;
      case 1:
        seenStart = 1;
        break;
      case 2:
        seenShort = 1;
        /* 8 short windows of 120 lines: the short SFB count must fit the
         * 120-line tables (15 is the 1024-path limit, 120 lines need less
         * and never more). */
        if (maxSfbShort < 0 || maxSfbShort + 1 > 15) shortMaxSfbOk = 0;
        break;
      case 3:
        seenStop = 1;
        break;
      default:
        parseOk = 0;
    }
  }
  TASK_CHECK(nAus >= 24);
  TASK_CHECK(parseOk);
  TASK_CHECK(commonOk);
  TASK_CHECK(seenLong);
  TASK_CHECK(seenShort); /* transient must trigger eight_short_sequence */
  TASK_CHECK(seenStart || seenStop); /* real long<->short transitions */
  TASK_CHECK(shortMaxSfbOk);

  aacEncClose(&enc);
  return 0;
}

static int case_window_pitch_960(void) {
  /* A 1 kHz tone keeps its pitch only if a true 960-point transform with
   * correct band layout is used (no relabeling, no resampling). */
  HANDLE_AACENCODER enc = NULL;
  HANDLE_AACDECODER dec;
  AACENC_InfoStruct info = {0};
  long streamLen = 0, energyL = 0;
  int frames, frameSize = -1, crossings = 0, nSamples = 0;
  double freq;

  TASK_CHECK(enc_open(&enc, AOT_AAC_LC, LC960_SR, 1, 64000, TT_MP4_RAW, 960));
  TASK_CHECK(aacEncEncode(enc, NULL, NULL, NULL, NULL) == AACENC_OK);
  TASK_CHECK(aacEncInfo(enc, &info) == AACENC_OK);
  dec = dec_open(TT_MP4_RAW, &info);
  TASK_CHECK(dec != NULL);

  gen_pcm(g_pcm, 0, 24 * LC960_FL, 1, GEN_TONE1K);
  g_nAuA = 0;
  streamLen = enc_feed_all(enc, g_pcm, 24 * LC960_FL, g_streamA, g_auOffA,
                           &g_nAuA, 0, NULL);
  TASK_CHECK(streamLen >= 0);
  streamLen = enc_flush(enc, g_streamA, g_auOffA, &g_nAuA, streamLen);
  TASK_CHECK(streamLen > 0);
  frames = dec_stream(dec, g_streamA, streamLen, g_auOffA, g_nAuA, &energyL,
                      NULL, &frameSize, NULL);
  TASK_CHECK(frames >= 24);
  TASK_CHECK(frameSize == LC960_FL);
  TASK_CHECK(energyL > 0);

  /* Decode again, measuring the zero crossing rate of the steady part. */
  aacDecoder_Close(dec);
  dec = dec_open(TT_MP4_RAW, &info);
  TASK_CHECK(dec != NULL);
  {
    int au = 0, prevSign = 0, i;
    for (au = 0; au < g_nAuA; au++) {
      long end = (au + 1 < g_nAuA) ? g_auOffA[au + 1] : streamLen;
      UCHAR *p = g_streamA + g_auOffA[au];
      UINT bytes = (UINT)(end - g_auOffA[au]);
      UINT valid = bytes;
      TASK_CHECK(aacDecoder_Fill(dec, &p, &bytes, &valid) == AAC_DEC_OK &&
                 valid == 0);
      for (;;) {
        CStreamInfo *si;
        AAC_DECODER_ERROR dr = aacDecoder_DecodeFrame(
            dec, g_dec, (int)(sizeof(g_dec) / sizeof(g_dec[0])), 0);
        if (dr == AAC_DEC_NOT_ENOUGH_BITS) break;
        TASK_CHECK(dr == AAC_DEC_OK);
        si = aacDecoder_GetStreamInfo(dec);
        for (i = 0; i < si->frameSize; i++) {
          int s = (g_dec[i] > 0) ? 1 : ((g_dec[i] < 0) ? -1 : 0);
          nSamples++;
          if (nSamples > 4 * LC960_FL) { /* skip codec delay onset */
            if (s != 0 && prevSign != 0 && s != prevSign) crossings++;
            if (s != 0) prevSign = s;
          }
        }
      }
    }
    TASK_CHECK(nSamples > 8 * LC960_FL);
    freq = (double)crossings / 2.0 * (double)LC960_SR /
           (double)(nSamples - 4 * LC960_FL);
    TASK_CHECK(freq > 980.0 && freq < 1020.0);
  }

  aacDecoder_Close(dec);
  aacEncClose(&enc);
  return 0;
}

static int case_window_compare_1024(void) {
  HANDLE_AACENCODER enc960 = NULL, enc1024 = NULL;
  HANDLE_AACDECODER dec;
  AACENC_InfoStruct info960 = {0}, info1024 = {0};
  long len960 = 0, len1024 = 0, e960 = 0, e1024 = 0;
  int frames960, frames1024, fs960 = -1, fs1024 = -1;
  const int total = 20 * 1024 * 2; /* interleaved samples */

  TASK_CHECK(enc_open(&enc960, AOT_AAC_LC, LC960_SR, 2, 128000, TT_MP4_RAW,
                      960));
  TASK_CHECK(enc_open(&enc1024, AOT_AAC_LC, LC960_SR, 2, 128000, TT_MP4_RAW,
                      0)); /* default 1024 path */
  TASK_CHECK(aacEncEncode(enc960, NULL, NULL, NULL, NULL) == AACENC_OK);
  TASK_CHECK(aacEncEncode(enc1024, NULL, NULL, NULL, NULL) == AACENC_OK);
  TASK_CHECK(aacEncInfo(enc960, &info960) == AACENC_OK);
  TASK_CHECK(aacEncInfo(enc1024, &info1024) == AACENC_OK);
  TASK_CHECK(info960.frameLength == 960 && info1024.frameLength == 1024);
  TASK_CHECK((info960.confBuf[1] & 0x04) != 0);  /* 960: flag set */
  TASK_CHECK((info1024.confBuf[1] & 0x04) == 0); /* 1024: flag clear */

  gen_pcm(g_pcm, 0, 20 * 1024, 2, GEN_NORMAL);
  g_nAuA = 0;
  g_nAuB = 0;
  len960 = enc_feed_all(enc960, g_pcm, (total / (960 * 2)) * (960 * 2),
                        g_streamA, g_auOffA, &g_nAuA, 0, NULL);
  TASK_CHECK(len960 >= 0);
  len1024 = enc_feed_all(enc1024, g_pcm, (total / (1024 * 2)) * (1024 * 2),
                         g_streamB, g_auOffB, &g_nAuB, 0, NULL);
  TASK_CHECK(len1024 >= 0);
  len960 = enc_flush(enc960, g_streamA, g_auOffA, &g_nAuA, len960);
  len1024 = enc_flush(enc1024, g_streamB, g_auOffB, &g_nAuB, len1024);
  TASK_CHECK(len960 > 0 && len1024 > 0);

  /* The 960 stream must not be a relabeled 1024 stream. */
  TASK_CHECK(len960 != len1024 ||
             memcmp(g_streamA, g_streamB, (size_t)len960) != 0);

  dec = dec_open(TT_MP4_RAW, &info960);
  TASK_CHECK(dec != NULL);
  frames960 = dec_stream(dec, g_streamA, len960, g_auOffA, g_nAuA, &e960, NULL,
                         &fs960, NULL);
  aacDecoder_Close(dec);
  dec = dec_open(TT_MP4_RAW, &info1024);
  TASK_CHECK(dec != NULL);
  frames1024 = dec_stream(dec, g_streamB, len1024, g_auOffB, g_nAuB, &e1024,
                          NULL, &fs1024, NULL);
  aacDecoder_Close(dec);

  TASK_CHECK(frames960 > 0 && frames1024 > 0);
  TASK_CHECK(fs960 == 960 && fs1024 == 1024);
  TASK_CHECK(e960 > 0 && e1024 > 0); /* neither path degenerates to silence */
  /* Same source: decoded energy per sample must be in the same ballpark. */
  {
    double a = (double)e960 / ((double)frames960 * 960.0);
    double b = (double)e1024 / ((double)frames1024 * 1024.0);
    TASK_CHECK(a > 0.5 * b && a < 2.0 * b);
  }

  aacEncClose(&enc960);
  aacEncClose(&enc1024);
  return 0;
}

/* ------------------------------------------------------------------ */
/* stream group                                                        */
/* ------------------------------------------------------------------ */

static int case_stream_chunked_identical(void) {
  /* Feeding the same PCM in different chunk sizes must yield the exact
   * same bitstream and sample consumption. */
  static const int chunks[] = {960 * 2, 100, 2000, 333 * 2, 78, 4096, 16};
  HANDLE_AACENCODER enc = NULL;
  long lenA = 0, lenB = 0;
  int consumedA = 0, consumedB = 0, t;
  const int totalSamples = 20 * 960 * 2;

  gen_pcm(g_pcm, 0, 20 * 960, 2, GEN_NORMAL);

  TASK_CHECK(enc_open(&enc, AOT_AAC_LC, LC960_SR, 2, 128000, TT_MP4_RAW, 960));
  TASK_CHECK(aacEncEncode(enc, NULL, NULL, NULL, NULL) == AACENC_OK);
  for (t = 0; t < 20; t++) {
    int cons = 0, out = enc_feed(enc, g_pcm + t * 960 * 2, 960 * 2, &cons);
    TASK_CHECK(out >= 0);
    TASK_CHECK(cons == 960 * 2);
    consumedA += cons;
    lenA = stream_put(g_streamA, NULL, NULL, lenA, out);
  }
  lenA = enc_flush(enc, g_streamA, NULL, NULL, lenA);
  TASK_CHECK(lenA > 0);
  aacEncClose(&enc);
  enc = NULL;

  TASK_CHECK(enc_open(&enc, AOT_AAC_LC, LC960_SR, 2, 128000, TT_MP4_RAW, 960));
  TASK_CHECK(aacEncEncode(enc, NULL, NULL, NULL, NULL) == AACENC_OK);
  {
    int fed = 0, ci = 0;
    while (fed < totalSamples) {
      int target = fed + chunks[ci++ % (int)(sizeof(chunks) / sizeof(chunks[0]))];
      if (target > totalSamples) target = totalSamples;
      while (fed < target) {
        int cons = 0, out = enc_feed(enc, g_pcm + fed, target - fed, &cons);
        TASK_CHECK(out >= 0);
        TASK_CHECK(cons > 0);
        fed += cons;
        consumedB += cons;
        lenB = stream_put(g_streamB, NULL, NULL, lenB, out);
      }
    }
  }
  lenB = enc_flush(enc, g_streamB, NULL, NULL, lenB);
  TASK_CHECK(lenB > 0);
  aacEncClose(&enc);

  TASK_CHECK(consumedA == totalSamples);
  TASK_CHECK(consumedB == totalSamples);
  TASK_CHECK(lenA == lenB);
  TASK_CHECK(memcmp(g_streamA, g_streamB, (size_t)lenA) == 0);
  return 0;
}

static int case_stream_latm_loas(void) {
  static const int tps[2] = {TT_MP4_LOAS, TT_MP4_LATM_MCP0};
  int k;
  for (k = 0; k < 2; k++) {
    HANDLE_AACENCODER enc = NULL;
    HANDLE_AACDECODER dec;
    AACENC_InfoStruct info = {0};
    long streamLen = 0, energyL = 0, energyR = 0;
    int frames, frameSize = -1, nChOut = 0;

    TASK_CHECK(enc_open(&enc, AOT_AAC_LC, LC960_SR, 2, 128000, tps[k], 960));
    TASK_CHECK(aacEncEncode(enc, NULL, NULL, NULL, NULL) == AACENC_OK);
    TASK_CHECK(aacEncInfo(enc, &info) == AACENC_OK);
    TASK_CHECK(info.frameLength == LC960_FL);
    TASK_CHECK(info.confSize > 2); /* LATM/LOAS carries the full config */
    dec = dec_open(tps[k], &info);
    TASK_CHECK(dec != NULL);

    gen_pcm(g_pcm, 0, 16 * LC960_FL, 2, GEN_NORMAL);
    g_nAuA = 0;
    streamLen = enc_feed_all(enc, g_pcm, 16 * LC960_FL * 2, g_streamA,
                             g_auOffA, &g_nAuA, 0, NULL);
    TASK_CHECK(streamLen >= 0);
    streamLen = enc_flush(enc, g_streamA, g_auOffA, &g_nAuA, streamLen);
    TASK_CHECK(streamLen > 0);
    frames = dec_stream(dec, g_streamA, streamLen, g_auOffA, g_nAuA, &energyL,
                        &energyR, &frameSize, &nChOut);
    TASK_CHECK(frames >= 16);
    TASK_CHECK(frameSize == LC960_FL);
    TASK_CHECK(nChOut == 2);
    TASK_CHECK(energyL > 0 && energyR > 0);

    aacDecoder_Close(dec);
    aacEncClose(&enc);
  }
  return 0;
}

static int case_stream_adts_rejected(void) {
  HANDLE_AACENCODER enc = NULL;
  AACENC_ERROR rc;

  /* ADTS cannot signal a 960 samples frame length: must be a config error,
   * not a seemingly valid stream. */
  TASK_CHECK(enc_open(&enc, AOT_AAC_LC, LC960_SR, 2, 128000, TT_MP4_ADTS, 960));
  rc = aacEncEncode(enc, NULL, NULL, NULL, NULL);
  TASK_CHECK(rc == AACENC_INVALID_CONFIG);
  aacEncClose(&enc);
  enc = NULL;

  /* Same when ADTS is selected implicitly (default transport). */
  TASK_CHECK(enc_open(&enc, AOT_AAC_LC, LC960_SR, 2, 128000, TT_UNKNOWN, 960));
  rc = aacEncEncode(enc, NULL, NULL, NULL, NULL);
  TASK_CHECK(rc == AACENC_INVALID_CONFIG);
  aacEncClose(&enc);
  enc = NULL;

  /* SBR/PS object types are not extended to 960 in this step. */
  TASK_CHECK(enc_open(&enc, AOT_SBR, LC960_SR, 2, 64000, TT_MP4_RAW, 960));
  rc = aacEncEncode(enc, NULL, NULL, NULL, NULL);
  TASK_CHECK(rc == AACENC_INVALID_CONFIG);
  aacEncClose(&enc);
  enc = NULL;

  /* Sanity: 1024 + ADTS keeps working (default path compatibility). */
  TASK_CHECK(enc_open(&enc, AOT_AAC_LC, LC960_SR, 2, 128000, TT_MP4_ADTS, 0));
  TASK_CHECK(aacEncEncode(enc, NULL, NULL, NULL, NULL) == AACENC_OK);
  aacEncClose(&enc);
  return 0;
}

static int case_stream_channel_mapping(void) {
  int dir;
  for (dir = 0; dir < 2; dir++) {
    HANDLE_AACENCODER enc = NULL;
    HANDLE_AACDECODER dec;
    AACENC_InfoStruct info = {0};
    long streamLen = 0, energyL = 0, energyR = 0;
    int frames, frameSize = -1;

    TASK_CHECK(enc_open(&enc, AOT_AAC_LC, LC960_SR, 2, 128000, TT_MP4_RAW,
                        960));
    TASK_CHECK(aacEncEncode(enc, NULL, NULL, NULL, NULL) == AACENC_OK);
    TASK_CHECK(aacEncInfo(enc, &info) == AACENC_OK);
    dec = dec_open(TT_MP4_RAW, &info);
    TASK_CHECK(dec != NULL);

    gen_pcm(g_pcm, 0, 16 * LC960_FL, 2,
            dir == 0 ? GEN_LEFT_ONLY : GEN_RIGHT_ONLY);
    g_nAuA = 0;
    streamLen = enc_feed_all(enc, g_pcm, 16 * LC960_FL * 2, g_streamA,
                             g_auOffA, &g_nAuA, 0, NULL);
    TASK_CHECK(streamLen >= 0);
    streamLen = enc_flush(enc, g_streamA, g_auOffA, &g_nAuA, streamLen);
    TASK_CHECK(streamLen > 0);
    frames = dec_stream(dec, g_streamA, streamLen, g_auOffA, g_nAuA, &energyL,
                        &energyR, &frameSize, NULL);
    TASK_CHECK(frames >= 16);
    TASK_CHECK(frameSize == LC960_FL);
    /* The active channel must dominate: no channel swap, no mono fold-down. */
    if (dir == 0) {
      TASK_CHECK(energyL > 4 * energyR);
    } else {
      TASK_CHECK(energyR > 4 * energyL);
    }

    aacDecoder_Close(dec);
    aacEncClose(&enc);
  }
  return 0;
}

/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    {"frame", "lc960_frame_length_raw", case_frame_length_raw},
    {"frame", "lc960_flush_tail", case_frame_flush_tail},
    {"frame", "lc960_rates_channels", case_frame_rates_channels},
    {"windows", "lc960_window_signaling", case_window_signaling},
    {"windows", "lc960_tone_pitch_960", case_window_pitch_960},
    {"windows", "lc960_compare_1024", case_window_compare_1024},
    {"stream", "lc960_chunked_identical", case_stream_chunked_identical},
    {"stream", "lc960_latm_loas", case_stream_latm_loas},
    {"stream", "lc960_adts_rejected", case_stream_adts_rejected},
    {"stream", "lc960_channel_mapping", case_stream_channel_mapping},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
