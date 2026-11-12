/* Multichannel pitch-period checks for the Sonic library.
   Copyright 2026
   This file is part of the Sonic Library.

   This file is licensed under the Apache 2.0 license.

   The test includes sonic.c directly so that the private analysis routines can
   be exercised through a test build; no public API is added.  It verifies that
   pitch-period decisions use every channel independently, that the arithmetic
   survives full-scale samples, that history and buffer reconfiguration behave,
   and that the public streaming entry points preserve polarity and channel
   relationships. */

#include "sonic.c"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TEST_SAMPLE_RATE 48000
#define TEST_WINDOW 4096
#define MAX_CHANNELS 6

static int casesRun = 0;
static int casesFailed = 0;
static int firstFailureLogged = 0;

#define FAIL(...)                                                         \
  do {                                                                    \
    casesFailed++;                                                        \
    if (!firstFailureLogged) {                                            \
      firstFailureLogged = 1;                                            \
      fprintf(stderr, "FIRST_DEVIATION [C]: ");                          \
      fprintf(stderr, __VA_ARGS__);                                       \
      fputc('\n', stderr);                                                \
    }                                                                     \
  } while (0)

#define CHECK(cond, ...)                                                  \
  do {                                                                    \
    casesRun++;                                                           \
    if (!(cond)) {                                                        \
      FAIL(__VA_ARGS__);                                                  \
      return;                                                             \
    }                                                                     \
  } while (0)

/* Absolute difference of two samples.  Kept as int: the delta of two 16-bit
   values can reach 65535 and must never be narrowed back to short. */
static int absSampleDelta(short a, short b) {
  int d = (int)a - (int)b;
  return d < 0 ? -d : d;
}

/* Independent, deliberately simple AMDF reference.  Scores one candidate
   period over all channels with 64-bit accumulation.  It shares no code with
   the implementation under test: no down-sampling, no heuristics, just the
   raw per-channel absolute differences summed across channels. */
static long long referencePeriodDiff(const short* samples, int numChannels,
                                     int period) {
  long long diff = 0;
  int c, i;

  for (c = 0; c < numChannels; c++) {
    for (i = 0; i < period; i++) {
      diff += absSampleDelta(samples[i * numChannels + c],
                             samples[(i + period) * numChannels + c]);
    }
  }
  return diff;
}

/* Reference argmin over a candidate range.  Ties resolve to the shorter
   period, matching the implementation's strict comparison rule. */
static int referenceBestPeriod(const short* samples, int numChannels,
                               int minPeriod, int maxPeriod) {
  long long bestDiff = 0;
  int bestPeriod = 0;
  int p;

  for (p = minPeriod; p <= maxPeriod; p++) {
    long long diff = referencePeriodDiff(samples, numChannels, p);
    if (bestPeriod == 0 || diff * bestPeriod < bestDiff * p) {
      bestDiff = diff;
      bestPeriod = p;
    }
  }
  return bestPeriod;
}

/* Deterministic LCG noise, identical when reimplemented in Java using long
   arithmetic. */
static long lcgState = 1;

static void lcgReset(void) { lcgState = 1; }

static short lcgSample(int amplitude) {
  long long state;
  lcgState = (lcgState * 1103515245L + 12345L) & 0x7fffffffL;
  state = lcgState;
  return (short)((((state >> 16) & 0x7fff) - 16384) * amplitude / 16384);
}

/* Integer triangular harmonic probe.  A 32-bit phase accumulator drives a
   fundamental plus two harmonics, so the values are bit-reproducible in C and
   Java without depending on libm rounding. */
static unsigned int phaseAcc = 0;
static unsigned int phaseInc = 0;

static void probeReset(int frequency, int sampleRate) {
  phaseAcc = 0;
  phaseInc = (unsigned int)((4294967296.0 * frequency) / sampleRate + 0.5);
}

static short probeSample(int amplitude) {
  int tri1, tri2, tri3, value;
  phaseAcc += phaseInc;
  tri1 = ((int)(phaseAcc >> 15) & 0xffff) - 32768;
  tri2 = ((int)((phaseAcc << 1) >> 15) & 0xffff) - 32768;
  tri3 = ((int)((phaseAcc << 2) >> 15) & 0xffff) - 32768;
  value = (int)(0.6 * tri1 + 0.3 * tri2 + 0.1 * tri3);
  if (value > 32767) {
    value = 32767;
  } else if (value < -32767) {
    value = -32767;
  }
  return (short)((long)value * amplitude / 32767);
}

/* Step through a buffer the way changeSpeed does, recording the period
   decisions so the prevPeriod/prevMinDiff history is exercised. */
#define SEQ_CAP 512

static void periodSequence(sonicStream stream, const short* samples,
                           int frames, int* seq, int* seqLen) {
  int position = 0;
  int count = 0;

  while (position + stream->maxRequired <= frames && count < SEQ_CAP) {
    int period = findPitchPeriod(stream,
                                 (short*)(samples + position * stream->numChannels),
                                 1);
    seq[count++] = period;
    position += period;
  }
  *seqLen = count;
}

static void printSequence(const char* tag, const char* name, int quality,
                          const int* seq, int seqLen) {
  int i;
  printf("XL HISTORY name=%s quality=%d seq=", name, quality);
  for (i = 0; i < seqLen; i++) {
    printf("%s%d", i ? "," : "", seq[i]);
  }
  printf(" tag=%s\n", tag);
}

/* Compare the implementation's per-candidate cost against the independent
   reference for every candidate period, on several channel layouts. */
static void testCostTableAgainstReference(void) {
  static short data[TEST_WINDOW * MAX_CHANNELS];
  sonicStream stream = sonicCreateStream(TEST_SAMPLE_RATE, 2);
  int p, c, i;

  sonicSetQuality(stream, 1);

  /* Harmonic probe, antiphase stereo. */
  probeReset(220, TEST_SAMPLE_RATE);
  for (i = 0; i < TEST_WINDOW; i++) {
    short v = probeSample(30000);
    data[2 * i] = v;
    data[2 * i + 1] = (short)-v;
  }
  for (p = stream->minPeriod; p <= stream->maxPeriod; p++) {
    long long expected = referencePeriodDiff(data, 2, p);
    long long actual = findPeriodDiff(data, 2, p);
    casesRun++;
    if (expected != actual) {
      FAIL("antiphase cost mismatch at p=%d expected=%lld actual=%lld", p,
           expected, actual);
      sonicDestroyStream(stream);
      return;
    }
  }

  /* First channel silent: its zero contribution must not change how the
     voiced channel is scored. */
  probeReset(220, TEST_SAMPLE_RATE);
  for (i = 0; i < TEST_WINDOW; i++) {
    data[2 * i] = 0;
    data[2 * i + 1] = probeSample(30000);
  }
  for (p = stream->minPeriod; p <= stream->maxPeriod; p++) {
    long long expected = referencePeriodDiff(data, 2, p);
    long long actual = findPeriodDiff(data, 2, p);
    casesRun++;
    if (expected != actual) {
      FAIL("silent-first cost mismatch at p=%d expected=%lld actual=%lld", p,
           expected, actual);
      sonicDestroyStream(stream);
      return;
    }
  }

  /* Six channels, including an all-zero channel. */
  sonicDestroyStream(stream);
  stream = sonicCreateStream(TEST_SAMPLE_RATE, 6);
  sonicSetQuality(stream, 1);
  probeReset(220, TEST_SAMPLE_RATE);
  for (i = 0; i < TEST_WINDOW; i++) {
    short v = probeSample(20000);
    for (c = 0; c < 5; c++) {
      data[6 * i + c] = (c & 1) ? (short)-v : v;
    }
    data[6 * i + 5] = 0;
  }
  for (p = stream->minPeriod; p <= stream->maxPeriod; p++) {
    long long expected = referencePeriodDiff(data, 6, p);
    long long actual = findPeriodDiff(data, 6, p);
    casesRun++;
    if (expected != actual) {
      FAIL("six-channel cost mismatch at p=%d expected=%lld actual=%lld", p,
           expected, actual);
      sonicDestroyStream(stream);
      return;
    }
  }
  sonicDestroyStream(stream);
}

/* A silent first channel must not dominate the decision: a first-channel-only
   estimator would return the silence minimum instead of the voiced period. */
static void testFirstChannelSilent(void) {
  static short mono[TEST_WINDOW];
  static short stereo[TEST_WINDOW * 2];
  static short three[TEST_WINDOW * 3];
  sonicStream monoStream, stereoStream, threeStream, fresh;
  int monoPeriod, stereoPeriod, threePeriod, silencePeriod;
  int i;

  probeReset(220, TEST_SAMPLE_RATE);
  for (i = 0; i < TEST_WINDOW; i++) {
    mono[i] = probeSample(30000);
  }
  probeReset(220, TEST_SAMPLE_RATE);
  for (i = 0; i < TEST_WINDOW; i++) {
    short v = probeSample(30000);
    stereo[2 * i] = 0;
    stereo[2 * i + 1] = v;
    three[3 * i] = v;
    three[3 * i + 1] = 0;
    three[3 * i + 2] = (short)-v;
  }

  monoStream = sonicCreateStream(TEST_SAMPLE_RATE, 1);
  stereoStream = sonicCreateStream(TEST_SAMPLE_RATE, 2);
  threeStream = sonicCreateStream(TEST_SAMPLE_RATE, 3);
  sonicSetQuality(monoStream, 1);
  sonicSetQuality(stereoStream, 1);
  sonicSetQuality(threeStream, 1);

  monoPeriod = findPitchPeriod(monoStream, mono, 1);
  stereoPeriod = findPitchPeriod(stereoStream, stereo, 1);
  threePeriod = findPitchPeriod(threeStream, three, 1);
  silencePeriod = stereoStream->minPeriod;

  CHECK(monoPeriod != silencePeriod,
        "mono probe period %d equals the silence minimum %d", monoPeriod,
        silencePeriod);
  CHECK(stereoPeriod == monoPeriod,
        "silent-first stereo picked %d, mono picked %d", stereoPeriod,
        monoPeriod);
  CHECK(threePeriod == monoPeriod,
        "silent-middle 3-channel picked %d, mono picked %d", threePeriod,
        monoPeriod);
  CHECK(referenceBestPeriod(stereo, 2, stereoStream->minPeriod,
                            stereoStream->maxPeriod) == stereoPeriod,
        "silent-first stereo disagrees with reference");

  /* Fully silent window followed by a voiced one must not poison history. */
  {
    static short silence[TEST_WINDOW * 2];
    static short voiced[TEST_WINDOW * 2];
    sonicStream poisoned = sonicCreateStream(TEST_SAMPLE_RATE, 2);
    sonicSetQuality(poisoned, 1);
    memset(silence, 0, sizeof(silence));
    probeReset(220, TEST_SAMPLE_RATE);
    for (i = 0; i < TEST_WINDOW; i++) {
      short v = probeSample(30000);
      voiced[2 * i] = 0;
      voiced[2 * i + 1] = v;
    }
    findPitchPeriod(poisoned, silence, 1);
    findPitchPeriod(poisoned, voiced, 1);
    fresh = sonicCreateStream(TEST_SAMPLE_RATE, 2);
    sonicSetQuality(fresh, 1);
    findPitchPeriod(fresh, voiced, 1);
    CHECK(poisoned->prevPeriod == fresh->prevPeriod,
          "silence window poisoned next period: %d vs fresh %d",
          poisoned->prevPeriod, fresh->prevPeriod);
    sonicDestroyStream(fresh);
    sonicDestroyStream(poisoned);
  }

  sonicDestroyStream(monoStream);
  sonicDestroyStream(stereoStream);
  sonicDestroyStream(threeStream);
}

/* Channels whose individual best periods differ.  Each channel repeats a
   fixed random pattern at its own period, so its single-channel AMDF minimum
   is exactly that period.  The two quieter channels together outweigh the
   loud one in the summed score, which means the combined answer is neither
   the first-channel nor the loudest-channel answer. */
static void testChannelsPreferDifferentPeriods(void) {
  static short data[TEST_WINDOW * 3];
  static short firstOnly[TEST_WINDOW];
  static short secondOnly[TEST_WINDOW];
  static short loudPattern[150];
  static short quietPattern[233];
  const int loudPeriod = 150;
  const int quietPeriod = 233;
  const int loudAmp = 15000;
  const int quietAmp = 9000;
  sonicStream stream;
  int combined, firstChannel, secondChannel, reference, i;

  lcgReset();
  for (i = 0; i < loudPeriod; i++) {
    loudPattern[i] = lcgSample(loudAmp);
  }
  lcgReset();
  for (i = 0; i < quietPeriod; i++) {
    quietPattern[i] = lcgSample(quietAmp);
  }
  for (i = 0; i < TEST_WINDOW; i++) {
    short loudValue = loudPattern[i % loudPeriod];
    short quietValue = quietPattern[i % quietPeriod];
    data[3 * i] = loudValue;
    data[3 * i + 1] = quietValue;
    data[3 * i + 2] = quietValue;
    firstOnly[i] = loudValue;
    secondOnly[i] = quietValue;
  }

  stream = sonicCreateStream(TEST_SAMPLE_RATE, 3);
  sonicSetQuality(stream, 1);

  firstChannel = referenceBestPeriod(firstOnly, 1, stream->minPeriod,
                                     stream->maxPeriod);
  secondChannel = referenceBestPeriod(secondOnly, 1, stream->minPeriod,
                                      stream->maxPeriod);
  CHECK(firstChannel == loudPeriod,
        "loud channel best period %d, expected %d", firstChannel, loudPeriod);
  CHECK(secondChannel == quietPeriod,
        "quiet channel best period %d, expected %d", secondChannel,
        quietPeriod);
  CHECK(firstChannel != secondChannel,
        "per-channel best periods do not differ: both %d", firstChannel);

  reference = referenceBestPeriod(data, 3, stream->minPeriod,
                                  stream->maxPeriod);
  combined = findPitchPeriod(stream, data, 1);
  CHECK(combined == reference,
        "combined period %d differs from reference %d", combined, reference);
  CHECK(combined != firstChannel,
        "combined period %d equals first/loudest-channel-only answer %d",
        combined, firstChannel);
  printf("XL CANDIDATE name=diffperiod period=%d firstOnly=%d secondOnly=%d\n",
         combined, firstChannel, secondChannel);
  sonicDestroyStream(stream);

  /* The same patterns reordered, keeping three channels, still match the
     reference and ignore channel ordering. */
  for (i = 0; i < TEST_WINDOW; i++) {
    short a = data[3 * i], b = data[3 * i + 1], c = data[3 * i + 2];
    data[3 * i] = c;
    data[3 * i + 1] = a;
    data[3 * i + 2] = b;
  }
  stream = sonicCreateStream(TEST_SAMPLE_RATE, 3);
  sonicSetQuality(stream, 1);
  reference = referenceBestPeriod(data, 3, stream->minPeriod,
                                  stream->maxPeriod);
  combined = findPitchPeriod(stream, data, 1);
  CHECK(combined == reference,
        "permuted combined %d differs from reference %d", combined, reference);
  CHECK(combined != loudPeriod,
        "permuted combined period %d equals loudest-channel answer", combined);
  sonicDestroyStream(stream);
}

/* Near full-scale samples, including SHRT_MIN: the 64-bit scorer must match
   the wide reference instead of wrapping in 32-bit arithmetic. */
static void testFullScaleArithmetic(void) {
  static short data[TEST_WINDOW * 2];
  static short mono[TEST_WINDOW];
  sonicStream stream;
  int p, periodQ0, periodQ1, reference, i;

  for (i = 0; i < TEST_WINDOW; i++) {
    short a = (i & 1) ? SHRT_MAX : SHRT_MIN;
    short b = (i & 1) ? SHRT_MIN : SHRT_MAX;
    data[2 * i] = a;
    data[2 * i + 1] = b;
  }
  stream = sonicCreateStream(TEST_SAMPLE_RATE, 2);
  for (p = stream->minPeriod; p <= stream->maxPeriod; p++) {
    long long expected = referencePeriodDiff(data, 2, p);
    long long actual = findPeriodDiff(data, 2, p);
    casesRun++;
    if (expected != actual) {
      FAIL("full-scale cost mismatch at p=%d expected=%lld actual=%lld", p,
           expected, actual);
      sonicDestroyStream(stream);
      return;
    }
  }
  sonicDestroyStream(stream);

  /* Full-scale periodic signal: both quality paths must agree and match the
     reference argmin. */
  probeReset(220, TEST_SAMPLE_RATE);
  for (i = 0; i < TEST_WINDOW; i++) {
    mono[i] = probeSample(32767);
  }
  stream = sonicCreateStream(TEST_SAMPLE_RATE, 1);
  sonicSetQuality(stream, 1);
  reference = referenceBestPeriod(mono, 1, stream->minPeriod,
                                  stream->maxPeriod);
  periodQ1 = findPitchPeriod(stream, mono, 1);
  sonicDestroyStream(stream);

  stream = sonicCreateStream(TEST_SAMPLE_RATE, 1);
  sonicSetQuality(stream, 0);
  periodQ0 = findPitchPeriod(stream, mono, 1);
  sonicDestroyStream(stream);

  CHECK(periodQ1 == reference,
        "full-scale q1 period %d differs from reference %d", periodQ1,
        reference);
  CHECK(periodQ0 == periodQ1,
        "full-scale q0 period %d differs from q1 period %d", periodQ0,
        periodQ1);
  printf("XL CANDIDATE name=fullscale period=%d\n", periodQ1);
}

/* Pure silence always yields a legal period and never divides by zero. */
static void testSilence(void) {
  static short silence[TEST_WINDOW * MAX_CHANNELS];
  int channelsList[] = {1, 2, 6};
  int k;

  memset(silence, 0, sizeof(silence));
  for (k = 0; k < 3; k++) {
    int channels = channelsList[k];
    sonicStream stream = sonicCreateStream(TEST_SAMPLE_RATE, channels);
    int minDiff, maxDiff;
    int period;
    sonicSetQuality(stream, 1);
    period = findPitchPeriodInRange(silence, channels, stream->minPeriod,
                                    stream->maxPeriod, &minDiff, &maxDiff);
    CHECK(period >= stream->minPeriod && period <= stream->maxPeriod,
          "silence period %d out of range %d..%d for %d channels", period,
          stream->minPeriod, stream->maxPeriod, channels);
    CHECK(minDiff == 0 && maxDiff == 0,
          "silence diffs not zero: min=%d max=%d", minDiff, maxDiff);
    sonicDestroyStream(stream);
  }
}

/* Rate/channel grid: polarity flips, channel permutations and added silent
   channels must all produce the same period sequence, and the chosen periods
   stay legal.  Emits cross-language comparison lines. */
static void testRateChannelGrid(void) {
  static const int rates[] = {8000, 16000, 48000};
  static const int channelCounts[] = {1, 2, 6};
  static short mono[TEST_WINDOW];
  static short data[TEST_WINDOW * MAX_CHANNELS];
  int r, c, q, variant, i, ch;

  for (r = 0; r < 3; r++) {
    int rate = rates[r];
    probeReset(220, rate);
    for (i = 0; i < TEST_WINDOW; i++) {
      mono[i] = probeSample(20000);
    }
    for (c = 0; c < 3; c++) {
      int channels = channelCounts[c];
      for (q = 0; q <= 1; q++) {
        int referenceSeq[SEQ_CAP];
        int referenceLen = 0;
        for (variant = 0; variant < 4; variant++) {
          sonicStream stream = sonicCreateStream(rate, channels);
          int seq[SEQ_CAP];
          int seqLen, k;
          sonicSetQuality(stream, q);
          for (i = 0; i < TEST_WINDOW; i++) {
            for (ch = 0; ch < channels; ch++) {
              short value;
              switch (variant) {
                case 0: /* in phase */
                  value = mono[i];
                  break;
                case 1: /* alternating polarity */
                  value = (ch & 1) ? (short)-mono[i] : mono[i];
                  break;
                case 2: /* permuted channels */
                  value = mono[i];
                  break;
                default: /* extra silent channels */
                  value = (ch < 2) ? ((ch & 1) ? (short)-mono[i] : mono[i])
                                   : 0;
              }
              data[i * channels + ch] = value;
            }
            if (variant == 2 && channels > 1) {
              short saved = data[i * channels];
              for (ch = 0; ch < channels - 1; ch++) {
                data[i * channels + ch] = data[i * channels + ch + 1];
              }
              data[i * channels + channels - 1] = saved;
            }
          }
          periodSequence(stream, data, TEST_WINDOW, seq, &seqLen);
          CHECK(seqLen > 0, "no periods generated rate=%d ch=%d q=%d v=%d",
                rate, channels, q, variant);
          for (k = 0; k < seqLen; k++) {
            CHECK(seq[k] >= stream->minPeriod && seq[k] <= stream->maxPeriod,
                  "period %d out of range rate=%d ch=%d q=%d v=%d", seq[k],
                  rate, channels, q, variant);
          }
          if (variant == 0) {
            memcpy(referenceSeq, seq, seqLen * sizeof(int));
            referenceLen = seqLen;
          } else {
            CHECK(seqLen == referenceLen,
                  "sequence length differs rate=%d ch=%d q=%d v=%d: %d vs %d",
                  rate, channels, q, variant, seqLen, referenceLen);
            for (k = 0; k < seqLen; k++) {
              CHECK(seq[k] == referenceSeq[k],
                    "sequence differs rate=%d ch=%d q=%d v=%d at %d: %d vs %d",
                    rate, channels, q, variant, k, seq[k], referenceSeq[k]);
            }
          }
          if (variant == 0 || variant == 1) {
            printf("XL GRID rate=%d ch=%d q=%d variant=%d seq=", rate,
                   channels, q, variant);
            for (k = 0; k < seqLen; k++) {
              printf("%s%d", k ? "," : "", seq[k]);
            }
            printf("\n");
          }
          sonicDestroyStream(stream);
        }
      }
    }
  }
}

/* Cross-language history vectors: integer-generated signals, both qualities. */
static void testCrossLanguageVectors(void) {
  static short data[TEST_WINDOW * 2];
  const char* names[2] = {"probe", "silentfirst"};
  int v, q, i;

  for (v = 0; v < 2; v++) {
    probeReset(220, TEST_SAMPLE_RATE);
    for (i = 0; i < TEST_WINDOW; i++) {
      short value = probeSample(30000);
      if (v == 0) {
        data[2 * i] = value;
        data[2 * i + 1] = (short)-value;
      } else {
        data[2 * i] = 0;
        data[2 * i + 1] = value;
      }
    }
    for (q = 0; q <= 1; q++) {
      sonicStream stream = sonicCreateStream(TEST_SAMPLE_RATE, 2);
      int seq[SEQ_CAP];
      int seqLen;
      sonicSetQuality(stream, q);
      periodSequence(stream, data, TEST_WINDOW, seq, &seqLen);
      printSequence("C", names[v], q, seq, seqLen);
      if (q == 1) {
        int minDiff, maxDiff;
        int candidate = findPitchPeriodInRange(
            data, 2, stream->minPeriod, stream->maxPeriod, &minDiff, &maxDiff);
        printf("XL CANDIDATE name=%s period=%d\n", names[v], candidate);
      }
      sonicDestroyStream(stream);
    }
  }
}

/* Recreating, resizing and flushing a stream must leave no stale layout or
   history behind. */
static void testReconfigureAndReuse(void) {
  static short data[TEST_WINDOW * MAX_CHANNELS];
  static short silence[2048 * MAX_CHANNELS];
  sonicStream stream, fresh;
  int seqA[SEQ_CAP], seqB[SEQ_CAP], lenA, lenB, i, c, k;

  stream = sonicCreateStream(48000, 2);
  probeReset(220, 48000);
  for (i = 0; i < TEST_WINDOW; i++) {
    short v = probeSample(20000);
    data[2 * i] = v;
    data[2 * i + 1] = (short)-v;
  }
  sonicSetQuality(stream, 1);
  findPitchPeriod(stream, data, 1);

  sonicSetSampleRate(stream, 16000);
  sonicSetNumChannels(stream, 6);
  CHECK(sonicGetSampleRate(stream) == 16000, "sample rate not reset");
  CHECK(sonicGetNumChannels(stream) == 6, "channel count not reset");
  memset(silence, 0, sizeof(silence));
  CHECK(sonicWriteShortToStream(stream, silence, 2048), "write after reset");
  CHECK(sonicFlushStream(stream), "flush after reset");
  CHECK(sonicSamplesAvailable(stream) >= 0, "bad available count");

  sonicSetSampleRate(stream, 8000);
  sonicSetNumChannels(stream, 1);
  probeReset(220, 8000);
  for (i = 0; i < TEST_WINDOW; i++) {
    data[i] = probeSample(20000);
  }
  sonicSetQuality(stream, 1);
  periodSequence(stream, data, TEST_WINDOW, seqA, &lenA);

  fresh = sonicCreateStream(8000, 1);
  sonicSetQuality(fresh, 1);
  periodSequence(fresh, data, TEST_WINDOW, seqB, &lenB);
  CHECK(lenA == lenB, "reused stream sequence length %d vs fresh %d", lenA,
        lenB);
  for (k = 0; k < lenA; k++) {
    CHECK(seqA[k] == seqB[k], "reused stream period %d vs fresh %d at %d",
          seqA[k], seqB[k], k);
  }
  sonicDestroyStream(fresh);

  /* Repeated create/reset/flush cycles keep working and stay in range. */
  for (c = 1; c <= 6; c++) {
    sonicStream cycled = sonicCreateStream(48000, c);
    sonicSetNumChannels(cycled, c);
    sonicSetSampleRate(cycled, 48000);
    memset(silence, 0, (size_t)2048 * c * sizeof(short));
    CHECK(sonicWriteShortToStream(cycled, silence, 2048), "cycle write ch=%d",
          c);
    CHECK(sonicFlushStream(cycled), "cycle flush ch=%d", c);
    sonicDestroyStream(cycled);
  }
  sonicDestroyStream(stream);
}

/* Growable interleaved output buffer. */
typedef struct {
  short* data;
  int frames;
  int capacity;
  int channels;
} PcmBuffer;

static void pcmInit(PcmBuffer* buf, int channels) {
  buf->channels = channels;
  buf->frames = 0;
  buf->capacity = 4096;
  buf->data = (short*)malloc((size_t)buf->capacity * channels * sizeof(short));
}

static void pcmDrain(sonicStream stream, PcmBuffer* buf) {
  short chunk[2048 * MAX_CHANNELS];
  int read;
  do {
    read = sonicReadShortFromStream(stream, chunk, 2048);
    if (read > 0) {
      if (buf->frames + read > buf->capacity) {
        while (buf->frames + read > buf->capacity) {
          buf->capacity *= 2;
        }
        buf->data = (short*)realloc(
            buf->data, (size_t)buf->capacity * buf->channels * sizeof(short));
      }
      memcpy(buf->data + (size_t)buf->frames * buf->channels, chunk,
             (size_t)read * buf->channels * sizeof(short));
      buf->frames += read;
    }
  } while (read > 0);
}

static void runPublicStream(int channels, float speed, const short* input,
                            int inputFrames, int alternatingChunks,
                            PcmBuffer* output) {
  sonicStream stream = sonicCreateStream(TEST_SAMPLE_RATE, channels);
  int position = 0;
  int chunkSizes[3] = {1, 7, 113};
  int chunkIndex = 0;

  sonicSetSpeed(stream, speed);
  sonicSetPitch(stream, 1.0f);
  sonicSetRate(stream, 1.0f);
  pcmInit(output, channels);
  while (position < inputFrames) {
    int count;
    if (alternatingChunks) {
      count = chunkSizes[chunkIndex++ % 3];
      if (count > inputFrames - position) {
        count = inputFrames - position;
      }
    } else {
      count = inputFrames - position;
    }
    CHECK(sonicWriteShortToStream(
              stream, input + (size_t)position * channels, count) != 0,
          "public write failed");
    pcmDrain(stream, output);
    position += count;
  }
  CHECK(sonicFlushStream(stream), "public flush failed");
  pcmDrain(stream, output);
  sonicDestroyStream(stream);
}

/* Independent frequency check using autocorrelation.  Period-splicing adds
   amplitude-modulation sidebands that bias a single-bin DFT, so the steady
   state is instead identified by the lag with the strongest correlation over
   the interior (steady-state) portion of the output. */
static int dominantFrequency(const PcmBuffer* buf, int channel) {
  int start = buf->frames / 5;
  int end = buf->frames - buf->frames / 5;
  int lag, bestLag = 0;
  double bestCorrelation = -1.0;

  for (lag = 180; lag <= 260; lag++) {
    double correlation = 0.0;
    int n;
    for (n = start; n + lag < end; n++) {
      correlation += (double)buf->data[n * buf->channels + channel] *
                     buf->data[(n + lag) * buf->channels + channel];
    }
    if (correlation > bestCorrelation) {
      bestCorrelation = correlation;
      bestLag = lag;
    }
  }
  return (int)(TEST_SAMPLE_RATE / (double)bestLag + 0.5);
}

static int maxSampleDelta(const PcmBuffer* a, const PcmBuffer* b) {
  int frames, i, worst = 0;
  if (a->frames < b->frames) {
    frames = a->frames;
  } else {
    frames = b->frames;
  }
  for (i = 0; i < frames * a->channels; i++) {
    int delta = (int)a->data[i] - (int)b->data[i];
    if (delta < 0) {
      delta = -delta;
    }
    if (delta > worst) {
      worst = delta;
    }
  }
  return worst;
}

/* Per-channel inverse transform used to undo a polarity flip or channel
   permutation.  refChannel < 0 means the channel is expected to stay silent. */
typedef struct {
  int refChannel;
  int sign;
} ChannelXform;

/* Compare an output against the in-phase reference after undoing the input's
   polarity/channel transform.  Lengths are checked separately. */
static int transformedMaxDelta(const PcmBuffer* ref, const PcmBuffer* got,
                               const ChannelXform* xform) {
  int frames = ref->frames < got->frames ? ref->frames : got->frames;
  int frame, ch, worst = 0;

  for (frame = 0; frame < frames; frame++) {
    for (ch = 0; ch < got->channels; ch++) {
      int expected = 0;
      int actual = got->data[frame * got->channels + ch];
      int delta;
      if (xform[ch].refChannel >= 0) {
        expected = xform[ch].sign *
                   ref->data[frame * ref->channels + xform[ch].refChannel];
      }
      delta = actual - expected;
      if (delta < 0) {
        delta = -delta;
      }
      if (delta > worst) {
        worst = delta;
      }
    }
  }
  return worst;
}

/* Public short-stream entry points: polarity/channel transforms of one source
   must keep output lengths aligned and samples within 2 units, and the steady
   state must stay at the original frequency. */
static void testPublicStreaming(void) {
  static const float speeds[2] = {0.7f, 1.6f};
  static short mono[8192];
  static short inPhase[8192 * 2];
  static short antiPhase[8192 * 2];
  static short swapped[8192 * 2];
  static short sixChannel[8192 * 6];
  const int inputFrames = 8192;
  int s, chunkMode, i, frequency;

  probeReset(220, TEST_SAMPLE_RATE);
  for (i = 0; i < inputFrames; i++) {
    mono[i] = probeSample(28000);
  }
  for (i = 0; i < inputFrames; i++) {
    inPhase[2 * i] = mono[i];
    inPhase[2 * i + 1] = mono[i];
    antiPhase[2 * i] = mono[i];
    antiPhase[2 * i + 1] = (short)-mono[i];
    swapped[2 * i] = (short)-mono[i];
    swapped[2 * i + 1] = mono[i];
    sixChannel[6 * i] = mono[i];
    sixChannel[6 * i + 1] = (short)-mono[i];
    sixChannel[6 * i + 2] = 0;
    sixChannel[6 * i + 3] = 0;
    sixChannel[6 * i + 4] = 0;
    sixChannel[6 * i + 5] = 0;
  }

  for (s = 0; s < 2; s++) {
    for (chunkMode = 0; chunkMode < 2; chunkMode++) {
      PcmBuffer outIn, outAnti, outSwap, outSix;
      ChannelXform identity[2] = {{0, 1}, {1, 1}};
      ChannelXform antiXform[2] = {{0, 1}, {1, -1}};
      ChannelXform swapXform[2] = {{0, -1}, {1, 1}};
      ChannelXform sixXform[6] = {{0, 1}, {1, -1}, {-1, 0}, {-1, 0},
                                  {-1, 0}, {-1, 0}};
      int antiDelta, swapDelta, sixDelta;
      runPublicStream(2, speeds[s], inPhase, inputFrames, chunkMode, &outIn);
      runPublicStream(2, speeds[s], antiPhase, inputFrames, chunkMode,
                      &outAnti);
      runPublicStream(2, speeds[s], swapped, inputFrames, chunkMode, &outSwap);
      runPublicStream(6, speeds[s], sixChannel, inputFrames, chunkMode,
                      &outSix);

      CHECK(outIn.frames > 1000, "degenerate output length %d", outIn.frames);
      CHECK(outIn.frames == outAnti.frames,
            "antiphase length %d vs in-phase %d (speed=%.2f chunks=%d)",
            outAnti.frames, outIn.frames, speeds[s], chunkMode);
      CHECK(outIn.frames == outSwap.frames,
            "swapped length %d vs in-phase %d (speed=%.2f chunks=%d)",
            outSwap.frames, outIn.frames, speeds[s], chunkMode);
      CHECK(outSix.frames == outIn.frames,
            "6-channel length %d vs stereo %d (speed=%.2f chunks=%d)",
            outSix.frames, outIn.frames, speeds[s], chunkMode);
      /* Undo each polarity/channel transform before comparing, so an inverted
         input is measured against an inverted (but otherwise identical)
         output. */
      CHECK(transformedMaxDelta(&outIn, &outIn, identity) == 0,
            "identity transform comparator broken");
      antiDelta = transformedMaxDelta(&outIn, &outAnti, antiXform);
      swapDelta = transformedMaxDelta(&outIn, &outSwap, swapXform);
      sixDelta = transformedMaxDelta(&outIn, &outSix, sixXform);
      CHECK(antiDelta <= 2,
            "antiphase sample delta %d exceeds 2 (speed=%.2f chunks=%d)",
            antiDelta, speeds[s], chunkMode);
      CHECK(swapDelta <= 2,
            "swapped sample delta %d exceeds 2 (speed=%.2f chunks=%d)",
            swapDelta, speeds[s], chunkMode);
      CHECK(sixDelta <= 2,
            "6-channel sample delta %d exceeds 2 (speed=%.2f chunks=%d)",
            sixDelta, speeds[s], chunkMode);
      frequency = dominantFrequency(&outAnti, 0);
      CHECK(abs(frequency - 220) <= 3,
            "antiphase dominant frequency %d not within 3 Hz of 220",
            frequency);
      frequency = dominantFrequency(&outAnti, 1);
      CHECK(abs(frequency - 220) <= 3,
            "antiphase channel 1 frequency %d not within 3 Hz of 220",
            frequency);
      printf(
          "XL STREAM impl=C speed=%.2f chunks=%d frames=%d maxdelta=%d\n",
          speeds[s], chunkMode, outIn.frames, antiDelta);
      free(outIn.data);
      free(outAnti.data);
      free(outSwap.data);
      free(outSix.data);
    }
  }

  /* speed=pitch=rate=1 must pass samples through unchanged, including when
     writes arrive in small alternating chunks. */
  for (chunkMode = 0; chunkMode < 2; chunkMode++) {
    PcmBuffer out;
    PcmBuffer input;
    runPublicStream(2, 1.0f, antiPhase, inputFrames, chunkMode, &out);
    CHECK(out.frames == inputFrames, "passthrough length %d vs %d",
          out.frames, inputFrames);
    input.data = antiPhase;
    input.frames = inputFrames;
    input.channels = 2;
    CHECK(maxSampleDelta(&input, &out) == 0,
          "passthrough modified samples (chunks=%d)", chunkMode);
    free(out.data);
  }
}

int main(void) {
  testCostTableAgainstReference();
  testFirstChannelSilent();
  testChannelsPreferDifferentPeriods();
  testFullScaleArithmetic();
  testSilence();
  testRateChannelGrid();
  testCrossLanguageVectors();
  testReconfigureAndReuse();
  testPublicStreaming();

  printf("C_SUMMARY cases=%d failures=%d\n", casesRun, casesFailed);
  if (casesRun == 0 || casesFailed != 0) {
    return 1;
  }
  return 0;
}
