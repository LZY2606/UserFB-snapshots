/* intensity.c
 *
 * MPEG-1 Layer III intensity stereo for long-block, two-channel streams.
 *
 * When enabled, the L and R MDCT spectra at and above a configurable
 * scalefactor band are merged into a single (L+R) spectrum carried by the
 * left channel, while the right channel transmits no spectral data in that
 * region.  The right channel scalefactor fields are reused as MPEG-1
 * intensity position values (ISO 11172-3, 2.4.2.7), one position per
 * granule.
 */

#include "intensity.h"
#include "l3loop.h"
#include "layer3.h"
#include "tables.h"

#include <stdint.h>

/* Enable or disable MPEG-1 intensity stereo on this encoder.
 *
 * first_sfb == -1 disables it (positions are ignored).  Otherwise first_sfb
 * must be in 11..20 and both granule positions in 0..6.  Only MPEG-I
 * (32/44.1/48 kHz), two-channel instances initialized in STEREO or
 * JOINT_STEREO mode accept the setting.  A rejected call leaves the
 * previous configuration untouched. */
int shine_set_intensity(shine_t s, int first_sfb, int position0,
                        int position1) {
  if (s == NULL)
    return -1;

  if (s->wave.channels != 2 || s->mpeg.version != MPEG_I ||
      (s->initial_mode != STEREO && s->initial_mode != JOINT_STEREO))
    return -1;

  if (first_sfb == -1) {
    s->intensity_on = 0;
    s->intensity_first_sfb = -1;
    return 0;
  }

  if (first_sfb < 11 || first_sfb > 20)
    return -1;

  if (position0 < 0 || position0 > 6 || position1 < 0 || position1 > 6)
    return -1;

  s->intensity_on = 1;
  s->intensity_first_sfb = first_sfb;
  s->intensity_position[0] = position0;
  s->intensity_position[1] = position1;

  return 0;
}

/* Switch the frame header to joint/intensity stereo and back.  The encoder
 * never produces M/S stereo, so mode_ext bit 1 stays zero. */
void shine_intensity_set_header(shine_global_config *config) {
  if (config->intensity_on) {
    config->mpeg.mode = JOINT_STEREO;
    config->mpeg.mode_ext = 1; /* intensity stereo on, M/S off */
  } else {
    config->mpeg.mode = config->initial_mode;
    config->mpeg.mode_ext = 0;
  }
}

/* Merge L and R spectra at and above the start band into a single L+R
 * spectrum on the left channel and clear the right channel there.
 * The sum is evaluated in 64 bit and saturated to int32 range, so no
 * signed overflow can happen regardless of the two channel levels. */
void shine_intensity_merge(shine_global_config *config) {
  int gr, i;
  const int *band;
  int start;

  if (!config->intensity_on)
    return;

  band = &shine_scale_fact_band_index[config->mpeg.samplerate_index][0];
  start = band[config->intensity_first_sfb];

  for (gr = 0; gr < config->mpeg.granules_per_frame; gr++) {
    for (i = start; i < GRANULE_SIZE; i++) {
      int64_t sum = (int64_t)config->mdct_freq[0][gr][i] +
                    (int64_t)config->mdct_freq[1][gr][i];

      if (sum > INT32_MAX)
        sum = INT32_MAX;
      else if (sum < INT32_MIN)
        sum = INT32_MIN;

      config->mdct_freq[0][gr][i] = (int32_t)sum;
      config->mdct_freq[1][gr][i] = 0;
    }
  }
}

/* Fill the right channel scalefactors of one granule with intensity
 * information.
 *
 * Bands below the actual intensity boundary carry the invalid position
 * value 7 so the decoder keeps normal LR coding there even when the right
 * channel quantized spectrum is empty; from the boundary up to band 20 the
 * granule's requested intensity position is transmitted (the implicit top
 * band 21 follows band 20 on the decoder).
 *
 * scalefac_compress is forced to 13 (3 bits per scalefactor) before the
 * quantization loop, which makes the Huffman budget reserve the position
 * bits explicitly.  A granule whose whole spectrum is zero skips that loop,
 * so its part2 accounting is completed here instead. */
void shine_intensity_finish_granule(shine_global_config *config, int gr) {
  gr_info *gi = &config->side_info.gr[gr].ch[1].tt;
  const int *band =
      &shine_scale_fact_band_index[config->mpeg.samplerate_index][0];
  int boundary = config->intensity_first_sfb;
  int sfb, i, last_nonzero = -1;

  /* Highest band still holding right channel spectral energy.  The MDCT
   * spectrum is fully recomputed every frame, so this cannot see stale
   * quantized values; the merged region above the requested start band is
   * zero by construction, so the boundary never moves past first_sfb. */
  for (sfb = 0; sfb < 21; sfb++) {
    for (i = band[sfb]; i < band[sfb + 1]; i++) {
      if (config->mdct_freq[1][gr][i] != 0) {
        last_nonzero = sfb;
        break;
      }
    }
  }

  if (last_nonzero + 1 > boundary)
    boundary = last_nonzero + 1;

  for (sfb = 0; sfb < 21; sfb++)
    config->scalefactor.l[gr][1][sfb] =
        (sfb < boundary) ? SHINE_INTENSITY_OFF
                         : config->intensity_position[gr];

  /* No quantization ran on an all-zero right spectrum: the position bits
   * are the only main data this granule contributes. */
  if (config->l3loop.xrmax == 0) {
    gi->part2_length = shine_part2_length(gr, 1, config);
    gi->part2_3_length = gi->part2_length;
  }
}
