/* l3ms.c
 *
 * Adaptive MPEG-1 long-block mid/side stereo.
 *
 * JOINT_STEREO used to be a header-only decoration: the encoder always coded
 * the left and right spectra.  This module prepares two complete candidates
 * for every frame -- the historical LR quantisation and an M/S quantisation --
 * and lets the caller pick one, keeping the physical CBR frame within its
 * capacity and the reservoir untouched (main_data_begin stays 0).
 *
 * M and S are the natural mid/side pair
 *
 *     M = (L + R) / sqrt(2)      S = (L - R) / sqrt(2)
 *
 * so the inverse transform in the decoder (L = (M + S)/sqrt2, R = (M - S)/
 * sqrt2) is orthogonal and the level is preserved.  All combinations use 64
 * bit intermediates with compensated rounding; nothing can overflow, nothing
 * is saturated and the loudness is never silently halved.
 */

#include "l3ms.h"
#include "layer3.h"
#include "l3loop.h"
#include "types.h"

/* Candidate indices. */
#define MS_LR 0
#define MS_MS 1

/* sqrt(1/2) in Q15. */
#define MS_SQRT1_2_Q15 23170

/* The fixed point quantiser must never leave its comfort zone.  A power of two
 * pre-scale is applied to M/S when needed and undone through global_gain,
 * which the bitstream carries as 4 * log2(step), so a factor 2^k becomes +4k
 * and the decoded level is unchanged. */
#define MS_SAFE_MAGNITUDE ((int32_t)0x30000000)
#define MS_MAX_FLOOR_LOG2 4

void shine_ms_initialise(shine_global_config *config) {
  memset(&config->ms, 0, sizeof(config->ms));
  config->ms.mode = 0;
  config->ms.base_mode = config->mpeg.mode;
  config->ms.floor_log2 = 0;
}

/* Compensated divide by a power of two, rounding to nearest. */
static int32_t shift_div_round(int64_t value, int shift) {
  if (shift <= 0)
    return (int32_t)value;
  return (int32_t)((value + ((int64_t)1 << (shift - 1))) >> shift);
}

/* M = (L + R) / sqrt(2), S = (L - R) / sqrt(2), with an optional power of two
 * pre-scale.  The intermediate is 64 bit. */
static int32_t ms_combine(int32_t l, int32_t r, int subtract, int floor_log2) {
  int64_t sum = subtract ? ((int64_t)l - (int64_t)r) : ((int64_t)l + (int64_t)r);
  return shift_div_round(sum * MS_SQRT1_2_Q15, 15 + floor_log2);
}

static void ms_build_spectra(shine_global_config *config, int floor_log2) {
  int ch, gr, i;

  for (ch = 0; ch < 2; ch++)
    for (gr = 0; gr < config->mpeg.granules_per_frame; gr++)
      for (i = 0; i < GRANULE_SIZE; i++)
        config->ms.ms_freq[ch][gr][i] =
            ch ? ms_combine(config->mdct_freq[0][gr][i],
                            config->mdct_freq[1][gr][i], 1, floor_log2)
               : ms_combine(config->mdct_freq[0][gr][i],
                            config->mdct_freq[1][gr][i], 0, floor_log2);
}

/* Pick the power-of-two pre-scale that keeps M/S inside the fixed point
 * range.  The estimate works on the M/S spectra themselves, so it cannot drift
 * away from what ms_build_spectra() actually produces. */
static int ms_choose_floor_log2(shine_global_config *config) {
  int shift;

  for (shift = 0; shift < MS_MAX_FLOOR_LOG2; shift++) {
    int32_t max = 0;
    int ch, gr, i;
    int too_large = 0;

    for (ch = 0; ch < 2 && !too_large; ch++)
      for (gr = 0; gr < config->mpeg.granules_per_frame && !too_large; gr++)
        for (i = 0; i < GRANULE_SIZE; i++) {
          int32_t v = ms_combine(config->mdct_freq[0][gr][i],
                                 config->mdct_freq[1][gr][i], ch, shift);
          if (v < 0)
            v = -v;
          if (v >= MS_SAFE_MAGNITUDE) {
            too_large = 1;
            break;
          }
          if (v > max)
            max = v;
        }
    if (!too_large)
      return shift;
  }
  return MS_MAX_FLOOR_LOG2;
}

/* True when a granule/channel pair carries no coefficient at all: it costs
 * nothing and its share of the granule is free for the other channel. */
static int ms_channel_empty(const int32_t *xr) {
  int i;
  for (i = GRANULE_SIZE; i--;)
    if (xr[i])
      return 0;
  return 1;
}

/* Take a deep snapshot of the encoder state produced by a candidate.
 *
 * The loop produces magnitudes only; the stream sign is the sign of the
 * spectrum line it quantised.  The snapshot therefore stores the signed
 * spectrum, which is also what the error measurement needs. */
static void ms_capture(shine_global_config *config, int which) {
  shine_ms_trial_t *t = &config->ms.trial[which];
  const int32_t (*spectrum)[MAX_GRANULES][GRANULE_SIZE] =
      which == MS_MS ? config->ms.ms_freq : config->ms.lr_freq;
  int ch, gr, i;

  t->side = config->side_info;
  t->bits = 0;
  for (ch = 0; ch < 2; ch++)
    for (gr = 0; gr < config->mpeg.granules_per_frame; gr++) {
      gr_info *gi = &config->side_info.gr[gr].ch[ch].tt;
      for (i = 0; i < GRANULE_SIZE; i++) {
        int q = config->l3_enc[ch][gr][i];
        if (q > 0 && spectrum[ch][gr][i] < 0)
          q = -q;
        t->l3_enc[ch][gr][i] = q;
      }
      t->gain[ch][gr] = (int)gi->global_gain;
      t->part2_3[ch][gr] = (int)gi->part2_3_length;
      t->bits += (int)gi->part2_3_length;
    }
  t->valid = 1;
}

/* Squared error of a candidate after reconstruction into the original LR
 * domain, with
 *   x  = mdct_freq / 2^31
 *   q' = sign(q) * |q|^(4/3) * 2^((global_gain - 210) / 4)
 * for the integer spectrum q written to the stream.  The MS candidate is
 * mapped back with the orthogonal inverse transform.
 *
 * Both candidates are compared at the level the decoder reproduces.  Any
 * internal power-of-two pre-scale of the M/S spectra is already carried by
 * global_gain, so the stream gain is exactly what the formula uses.  Both
 * granules and both channels of the frame are accumulated. */
static double ms_candidate_error(shine_global_config *config, int which) {
  const shine_ms_trial_t *t = &config->ms.trial[which];
  double total = 0;
  int gr, i;

  for (gr = 0; gr < config->mpeg.granules_per_frame; gr++) {
    const int *q0 = t->l3_enc[0][gr];
    const int *q1 = t->l3_enc[1][gr];
    int g0 = t->gain[0][gr];
    int g1 = t->gain[1][gr];

    if (which == MS_LR) {
      int ch;
      for (ch = 0; ch < 2; ch++) {
        const int *q = t->l3_enc[ch][gr];
        int gg = t->gain[ch][gr];
        double gain = pow(2.0, (gg - 210) / 4.0);
        for (i = 0; i < GRANULE_SIZE; i++) {
          double x = config->ms.lr_freq[ch][gr][i] * (1.0 / 2147483648.0);
          double r = 0;
          if (q[i])
            r = (q[i] > 0 ? 1.0 : -1.0) *
                pow(fabs((double)q[i]), 4.0 / 3.0) * gain;
          total += (r - x) * (r - x);
        }
      }
    } else {
      double gain0 = pow(2.0, (g0 - 210) / 4.0);
      double gain1 = pow(2.0, (g1 - 210) / 4.0);
      for (i = 0; i < GRANULE_SIZE; i++) {
        double m = 0, s = 0;
        double l, r, xl, xr;
        if (q0[i])
          m = (q0[i] > 0 ? 1.0 : -1.0) * pow(fabs((double)q0[i]), 4.0 / 3.0) *
              gain0;
        if (q1[i])
          s = (q1[i] > 0 ? 1.0 : -1.0) * pow(fabs((double)q1[i]), 4.0 / 3.0) *
              gain1;
        l = (m + s) * 0.70710678118654752;
        r = (m - s) * 0.70710678118654752;
        xl = config->ms.lr_freq[0][gr][i] * (1.0 / 2147483648.0);
        xr = config->ms.lr_freq[1][gr][i] * (1.0 / 2147483648.0);
        total += (l - xl) * (l - xl) + (r - xr) * (r - xr);
      }
    }
  }
  return total;
}

/* Restore a candidate's side information and quantised spectra.
 *
 * The sign of every quantised line is part of the stream, so it is taken from
 * the spectrum of the representation that is being restored: the LR
 * candidate carries the original left/right spectra, the MS candidate the
 * mid/side spectra.  The magnitude of |q| is what the error formula uses. */
static void ms_restore(shine_global_config *config, int which) {
  const shine_ms_trial_t *t = &config->ms.trial[which];
  const int32_t (*spectrum)[MAX_GRANULES][GRANULE_SIZE] =
      which == MS_MS ? config->ms.ms_freq : config->ms.lr_freq;
  int ch, gr, i;

  config->side_info = t->side;
  for (ch = 0; ch < 2; ch++)
    for (gr = 0; gr < config->mpeg.granules_per_frame; gr++)
      for (i = 0; i < GRANULE_SIZE; i++) {
        int q = t->l3_enc[ch][gr][i];
        if (q > 0 && spectrum[ch][gr][i] < 0)
          q = -q;
        config->l3_enc[ch][gr][i] = q;
      }
}

/* Trial one representation and store the result for the decision.
 *
 * The M/S candidate codes a channel that cannot produce any nonzero
 * coefficient at the cheapest possible gain (all zero, 0 bits), and hands its
 * half of the granule capacity to the channel that still carries signal. */
static void ms_trial(shine_global_config *config, int which) {
  shine_ms_t *ms = &config->ms;
  int floor_log2 = which == MS_MS ? ms->floor_log2 : 0;

  if (which == MS_LR) {
    shine_iteration_frame(config, ms->lr_freq, 0);
  } else {
    int empty[MAX_CHANNELS][MAX_GRANULES];
    int ch, gr;
    for (ch = 0; ch < 2; ch++)
      for (gr = 0; gr < config->mpeg.granules_per_frame; gr++)
        empty[ch][gr] = ms_channel_empty(ms->ms_freq[ch][gr]);
    shine_iteration_frame_ms(config, ms->ms_freq, floor_log2, empty);
  }

  ms_capture(config, which);
  ms->trial[which].error = ms_candidate_error(config, which);

#ifdef SHINE_MS_TESTING
  {
    int ch, gr, i;
    for (ch = 0; ch < 2; ch++)
      for (gr = 0; gr < config->mpeg.granules_per_frame; gr++) {
        const int32_t *src =
            which == MS_MS ? ms->ms_freq[ch][gr] : ms->lr_freq[ch][gr];
        for (i = 0; i < GRANULE_SIZE; i++) {
          ms->obs_q[which][ch][gr][i] = ms->trial[which].l3_enc[ch][gr][i];
          if (which == MS_MS)
            ms->obs_ms_spectrum[ch][gr][i] = src[i];
          else
            ms->obs_lr_spectrum[ch][gr][i] = src[i];
        }
        ms->obs_gain[which][ch][gr] = ms->trial[which].gain[ch][gr];
        ms->obs_part2_3[which][ch][gr] = ms->trial[which].part2_3[ch][gr];
      }
    ms->obs_bits[which] = ms->trial[which].bits;
    ms->obs_floor_log2[which] = floor_log2;
    ms->obs_valid[which] = ms->trial[which].valid;
  }
#endif
}

void shine_ms_frame(shine_global_config *config) {
  shine_ms_t *ms = &config->ms;
  int saved_resv = config->ResvSize;
  int saved_drain = config->side_info.resvDrain;
  unsigned saved_scfsi[MAX_CHANNELS][4];
  int choose_ms;
  int ch, gr, i;

  if (config->mpeg.version != MPEG_I || config->wave.channels != 2)
    return;

  /* 0 is the historical path: leave no trace of the adaptive decision. */
  if (ms->mode == 0) {
    config->mpeg.mode = ms->base_mode;
    config->mpeg.mode_ext = 0;
    ms->selected_ms = 0;
#ifdef SHINE_MS_TESTING
    ms->obs_valid[0] = ms->obs_valid[1] = 0;
    ms->obs_selected = 0;
    ms->obs_mode = (int)config->mpeg.mode;
    ms->obs_mode_ext = 0;
#endif
    return;
  }

  for (ch = 0; ch < 2; ch++)
    for (i = 0; i < 4; i++)
      saved_scfsi[ch][i] = config->side_info.scfsi[ch][i];

  /* 1. Analyse the frame only once and keep the alias-reduced LR spectrum.
   *    The M/S spectra are derived from that same copy. */
  for (ch = 0; ch < 2; ch++)
    memcpy(ms->lr_freq[ch], config->mdct_freq[ch], sizeof(ms->lr_freq[ch]));

  ms->floor_log2 = ms_choose_floor_log2(config);
  ms_build_spectra(config, ms->floor_log2);

  /* 2. LR candidate: the historical quantisation. */
  ms_trial(config, MS_LR);

  /* 3. MS candidate: same spectra history and same physical capacity.  The
   *    losing trial must not leak a reservoir balance or scfsi into it. */
  config->ResvSize = saved_resv;
  config->side_info.resvDrain = saved_drain;
  for (ch = 0; ch < 2; ch++)
    for (i = 0; i < 4; i++)
      config->side_info.scfsi[ch][i] = saved_scfsi[ch][i];
  ms_trial(config, MS_MS);

  /* 4. Pick one representation for the whole frame (both granules). */
  if (ms->mode == 0)
    choose_ms = 0;
  else if (ms->mode == 1)
    choose_ms = 1;
  else {
    double dlr = ms->trial[MS_LR].error;
    double dms = ms->trial[MS_MS].error;
    double scale = fabs(dlr) > fabs(dms) ? fabs(dlr) : fabs(dms);
    if (scale < 1.0)
      scale = 1.0;
    choose_ms = dms < dlr - 1e-12 * scale;
  }

  /* 5. Leave only the winner behind. */
  config->ResvSize = saved_resv;
  config->side_info.resvDrain = saved_drain;
  ms_restore(config, choose_ms);
  ms->selected_ms = choose_ms;

  for (ch = 0; ch < 2; ch++)
    for (gr = 0; gr < config->mpeg.granules_per_frame; gr++)
      memcpy(config->mdct_freq[ch][gr],
             choose_ms ? ms->ms_freq[ch][gr] : ms->lr_freq[ch][gr],
             GRANULE_SIZE * sizeof(int32_t));

  config->mpeg.mode = choose_ms ? JOINT_STEREO : ms->base_mode;
  config->mpeg.mode_ext = choose_ms ? 2 : 0;

#ifdef SHINE_MS_TESTING
  ms->obs_selected = choose_ms;
  ms->obs_mode = (int)config->mpeg.mode;
  ms->obs_mode_ext = (int)config->mpeg.mode_ext;
#endif
}
