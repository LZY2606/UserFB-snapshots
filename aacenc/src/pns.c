#include <math.h>
#include <stdint.h>
#include <string.h>

#include "pns.h"
#include "psy_const.h"

#if defined(__SIZEOF_INT128__)
typedef unsigned __int128 pns_u128;
#else
typedef unsigned long long pns_u128;
#endif

static uint64_t abs_spectrum(Word32 x)
{
  uint32_t u = (uint32_t)x;
  if (u & 0x80000000u)
    return 0x100000000ULL - (uint64_t)u;
  return u;
}

static int lower_edge_at_least_4k(Word16 offset, Word32 sampleRate, int physicalLines)
{
  /* MDCT bin k has frequency k*Fs/(2*physicalLines). */
  return (int64_t)offset * (int64_t)sampleRate >=
         4000LL * 2LL * (int64_t)physicalLines;
}

static Word16 noise_exponent(double averageBandEnergy, Word16 mdctScale)
{
  long double amplitude;
  long double scaledLog;
  long k;

  if (!(averageBandEnergy > 0.0))
    return -128;

  /*
   * The fixed point transform leaves the spectrum scaled by 2^(15-scale)
   * relative to the decoder's MDCT coefficients.  A PNS scalefactor is an
   * amplitude in quarter power-of-two steps.
   */
  amplitude = sqrtl((long double)averageBandEnergy) *
              ldexpl(1.0L, (int)mdctScale - 15);
  scaledLog = 4.0L * log2l(amplitude);

  /* ceil(x - 1/2) rounds to nearest and puts an exact tie on the lower level. */
  k = (long)ceill(scaledLog - 0.5L);
  return (Word16)k;
}

void PNS_ResetChannel(PSY_OUT_CHANNEL *psyOut, Word16 enable)
{
  if (!psyOut)
    return;

  psyOut->pnsEnable = enable ? 1 : 0;
  memset(psyOut->pnsFlag, 0, sizeof(psyOut->pnsFlag));
  memset(psyOut->pnsNoiseExp, 0, sizeof(psyOut->pnsNoiseExp));
  memset(psyOut->pnsPhysOk, 0, sizeof(psyOut->pnsPhysOk));
  memset(psyOut->pnsPhysEnergy, 0, sizeof(psyOut->pnsPhysEnergy));
  memset(psyOut->pnsBandEnergy, 0, sizeof(psyOut->pnsBandEnergy));
  memset(psyOut->pnsObsSpectrum, 0, sizeof(psyOut->pnsObsSpectrum));
}

static int band_is_noise_like(const Word32 *spectrum, int start, int width)
{
  pns_u128 energy = 0;
  pns_u128 maxSquare = 0;
  int i;

  if (width < 8)
    return 0;

  for (i = 0; i < width; i++) {
    uint64_t a = abs_spectrum(spectrum[start + i]);
    pns_u128 square = (pns_u128)a * (pns_u128)a;
    energy += square;
    if (square > maxSquare)
      maxSquare = square;
  }

  if (energy == 0)
    return 0;

  return (pns_u128)width * maxSquare <= (pns_u128)8 * energy;
}

void PNS_SelectLong(const Word32 *spectrum,
                    const Word16 *sfbOffset,
                    Word16 sfbCnt,
                    Word32 sampleRate,
                    Word16 mdctScale,
                    PSY_OUT_CHANNEL *psyOut)
{
  Word16 sfb;

  if (!psyOut || !psyOut->pnsEnable || !spectrum || !sfbOffset)
    return;

  memcpy(psyOut->pnsObsSpectrum, spectrum,
         FRAME_LEN_LONG * sizeof(psyOut->pnsObsSpectrum[0]));

  for (sfb = 0; sfb < sfbCnt && sfb < MAX_GROUPED_SFB; sfb++) {
    int start = sfbOffset[sfb];
    int width = sfbOffset[sfb + 1] - start;

    psyOut->pnsFlag[sfb] = 0;
    psyOut->pnsBandEnergy[sfb] = 0.0;

    if (!lower_edge_at_least_4k(sfbOffset[sfb], sampleRate, FRAME_LEN_LONG))
      continue;
    if (!band_is_noise_like(spectrum, start, width))
      continue;

    {
      pns_u128 energy = 0;
      int i;
      for (i = 0; i < width; i++) {
        uint64_t a = abs_spectrum(spectrum[start + i]);
        energy += (pns_u128)a * (pns_u128)a;
      }
      psyOut->pnsBandEnergy[sfb] = (double)energy;
      psyOut->pnsNoiseExp[sfb] = noise_exponent((double)energy, mdctScale);
      psyOut->pnsFlag[sfb] = 1;
    }
  }
}

void PNS_EvaluateShortWindow(const Word32 *spectrum,
                             const Word16 *sfbOffset,
                             Word16 sfbCnt,
                             Word32 sampleRate,
                             Word16 mdctScale,
                             Word16 window,
                             PSY_OUT_CHANNEL *psyOut)
{
  Word16 sfb;

  (void)mdctScale; /* The grouped exponent is chosen after averaging windows. */

  if (!psyOut || !psyOut->pnsEnable || window < 0 || window >= TRANS_FAC)
    return;

  memcpy(psyOut->pnsObsSpectrum + window * FRAME_LEN_SHORT,
         spectrum, FRAME_LEN_SHORT * sizeof(psyOut->pnsObsSpectrum[0]));

  for (sfb = 0; sfb < sfbCnt && sfb < MAX_SFB_SHORT; sfb++) {
    int start = sfbOffset[sfb];
    int width = sfbOffset[sfb + 1] - start;

    psyOut->pnsPhysOk[window][sfb] = 0;
    psyOut->pnsPhysEnergy[window][sfb] = 0.0;

    if (!lower_edge_at_least_4k(sfbOffset[sfb], sampleRate, FRAME_LEN_SHORT))
      continue;
    if (!band_is_noise_like(spectrum, start, width))
      continue;

    {
      pns_u128 energy = 0;
      int i;
      for (i = 0; i < width; i++) {
        uint64_t a = abs_spectrum(spectrum[start + i]);
        energy += (pns_u128)a * (pns_u128)a;
      }
      psyOut->pnsPhysOk[window][sfb] = 1;
      psyOut->pnsPhysEnergy[window][sfb] = (double)energy;
    }
  }
}

void PNS_BuildShortGroups(Word16 noOfGroups,
                          const Word16 *groupLen,
                          Word16 shortSfbCnt,
                          Word16 mdctScale,
                          PSY_OUT_CHANNEL *psyOut)
{
  Word16 grp, sfb;
  Word16 windowBase = 0;

  if (!psyOut || !psyOut->pnsEnable || !groupLen)
    return;

  for (grp = 0; grp < noOfGroups; grp++) {
    Word16 len = groupLen[grp];

    for (sfb = 0; sfb < shortSfbCnt; sfb++) {
      int idx = grp * shortSfbCnt + sfb;
      int allOk = 1;
      double energySum = 0.0;
      Word16 w;

      if (idx >= MAX_GROUPED_SFB)
        return;

      for (w = 0; w < len; w++) {
        Word16 win = windowBase + w;
        if (win >= TRANS_FAC || !psyOut->pnsPhysOk[win][sfb])
          allOk = 0;
        else
          energySum += psyOut->pnsPhysEnergy[win][sfb];
      }

      psyOut->pnsFlag[idx] = 0;
      psyOut->pnsBandEnergy[idx] = 0.0;
      psyOut->pnsNoiseExp[idx] = -128;

      if (allOk && len > 0) {
        double averageEnergy = energySum / (double)len;
        psyOut->pnsFlag[idx] = 1;
        psyOut->pnsBandEnergy[idx] = averageEnergy;
        psyOut->pnsNoiseExp[idx] =
          noise_exponent(averageEnergy, mdctScale);
      }
    }
    windowBase += len;
  }
}

void PNS_ResolveDeltas(PSY_OUT_CHANNEL *psyOut, Word16 groupedSfbCnt)
{
  int idx;
  int haveLast = 0;
  Word16 lastExp = 0;

  if (!psyOut || !psyOut->pnsEnable)
    return;

  for (idx = 0; idx < groupedSfbCnt && idx < MAX_GROUPED_SFB; idx++) {
    Word16 exp = psyOut->pnsNoiseExp[idx];

    if (!psyOut->pnsFlag[idx])
      continue;

    if (exp < -100 || exp > 155) {
      psyOut->pnsFlag[idx] = 0;
      continue;
    }

    if (haveLast) {
      int delta = (int)exp - (int)lastExp;
      if (delta > CODE_BOOK_PNS_LAV || delta < -CODE_BOOK_PNS_LAV) {
        /* This exact noise level cannot follow on the independent chain. */
        psyOut->pnsFlag[idx] = 0;
        continue;
      }
    }

    lastExp = exp;
    haveLast = 1;
  }
}

void PNS_ClearQuantizedBands(const PSY_OUT_CHANNEL *psyOut,
                             QC_OUT_CHANNEL *qcOut)
{
  Word16 sfb;

  if (!psyOut || !qcOut || !psyOut->pnsEnable)
    return;

  for (sfb = 0; sfb < psyOut->sfbCnt && sfb < MAX_GROUPED_SFB; sfb++) {
    int line;
    int start = psyOut->sfbOffsets[sfb];
    int stop = psyOut->sfbOffsets[sfb + 1];

    if (!psyOut->pnsFlag[sfb])
      continue;

    for (line = start; line < stop && line < FRAME_LEN_LONG; line++)
      qcOut->quantSpec[line] = 0;
    qcOut->maxValueInSfb[sfb] = 0;
  }
}

void PNS_FinalizeBitstreamChannel(const PSY_OUT_CHANNEL *psyOut,
                                  QC_OUT_CHANNEL *qcOut)
{
  Word32 logNorm;
  Word32 globalGainField;
  Word16 sfb;

  if (!psyOut || !qcOut || !psyOut->pnsEnable)
    return;

  logNorm = LOG_NORM_PCM - (psyOut->mdctScale + 1);
  globalGainField = (Word32)qcOut->globalGain -
                    qcOut->scf[qcOut->sectionData.firstScf] +
                    100 - (logNorm << 2);
  globalGainField &= 0xff;

  for (sfb = 0; sfb < psyOut->sfbCnt && sfb < MAX_GROUPED_SFB; sfb++) {
    if (psyOut->pnsFlag[sfb]) {
      qcOut->pnsNrg[sfb] =
        (Word16)((Word32)psyOut->pnsNoiseExp[sfb] - globalGainField +
                 PNS_NOISE_OFFSET);
    } else {
      qcOut->pnsNrg[sfb] = 0;
    }
  }
}
