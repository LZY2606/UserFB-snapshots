/* See COPYING file for copyright and license details. */

#ifndef EBUR128_H_
#define EBUR128_H_

/** \file ebur128.h
 *  \brief libebur128 - a library for loudness measurement according to
 *         the EBU R128 standard.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define EBUR128_VERSION_MAJOR 1
#define EBUR128_VERSION_MINOR 2
#define EBUR128_VERSION_PATCH 6

#include <stddef.h> /* for size_t */

/** \enum channel
 *  Use these values when setting the channel map with ebur128_set_channel().
 *  See definitions in ITU R-REC-BS 1770-4
 */
enum channel {
  EBUR128_UNUSED = 0,         /**< unused channel (for example LFE channel) */
  EBUR128_LEFT = 1,           /**<           */
  EBUR128_Mp030 = 1,          /**< itu M+030 */
  EBUR128_RIGHT = 2,          /**<           */
  EBUR128_Mm030 = 2,          /**< itu M-030 */
  EBUR128_CENTER = 3,         /**<           */
  EBUR128_Mp000 = 3,          /**< itu M+000 */
  EBUR128_LEFT_SURROUND = 4,  /**<           */
  EBUR128_Mp110 = 4,          /**< itu M+110 */
  EBUR128_RIGHT_SURROUND = 5, /**<           */
  EBUR128_Mm110 = 5,          /**< itu M-110 */
  EBUR128_DUAL_MONO,          /**< a channel that is counted twice */
  EBUR128_MpSC,               /**< itu M+SC  */
  EBUR128_MmSC,               /**< itu M-SC  */
  EBUR128_Mp060,              /**< itu M+060 */
  EBUR128_Mm060,              /**< itu M-060 */
  EBUR128_Mp090,              /**< itu M+090 */
  EBUR128_Mm090,              /**< itu M-090 */
  EBUR128_Mp135,              /**< itu M+135 */
  EBUR128_Mm135,              /**< itu M-135 */
  EBUR128_Mp180,              /**< itu M+180 */
  EBUR128_Up000,              /**< itu U+000 */
  EBUR128_Up030,              /**< itu U+030 */
  EBUR128_Um030,              /**< itu U-030 */
  EBUR128_Up045,              /**< itu U+045 */
  EBUR128_Um045,              /**< itu U-030 */
  EBUR128_Up090,              /**< itu U+090 */
  EBUR128_Um090,              /**< itu U-090 */
  EBUR128_Up110,              /**< itu U+110 */
  EBUR128_Um110,              /**< itu U-110 */
  EBUR128_Up135,              /**< itu U+135 */
  EBUR128_Um135,              /**< itu U-135 */
  EBUR128_Up180,              /**< itu U+180 */
  EBUR128_Tp000,              /**< itu T+000 */
  EBUR128_Bp000,              /**< itu B+000 */
  EBUR128_Bp045,              /**< itu B+045 */
  EBUR128_Bm045               /**< itu B-045 */
};

/** \enum error
 *  Error return values.
 */
enum error {
  EBUR128_SUCCESS = 0,
  EBUR128_ERROR_NOMEM,
  EBUR128_ERROR_INVALID_MODE,
  EBUR128_ERROR_INVALID_CHANNEL_INDEX,
  EBUR128_ERROR_NO_CHANGE,
  EBUR128_ERROR_INVALID_ARGUMENT
};

/** \enum mode
 *  Use these values in ebur128_init (or'ed). Try to use the lowest possible
 *  modes that suit your needs, as performance will be better.
 */
enum mode {
  /** can call ebur128_loudness_momentary */
  EBUR128_MODE_M = (1 << 0),
  /** can call ebur128_loudness_shortterm */
  EBUR128_MODE_S = (1 << 1) | EBUR128_MODE_M,
  /** can call ebur128_loudness_global_* and ebur128_relative_threshold */
  EBUR128_MODE_I = (1 << 2) | EBUR128_MODE_M,
  /** can call ebur128_loudness_range */
  EBUR128_MODE_LRA = (1 << 3) | EBUR128_MODE_S,
  /** can call ebur128_sample_peak */
  EBUR128_MODE_SAMPLE_PEAK = (1 << 4) | EBUR128_MODE_M,
  /** can call ebur128_true_peak */
  EBUR128_MODE_TRUE_PEAK = (1 << 5) | EBUR128_MODE_M | EBUR128_MODE_SAMPLE_PEAK,
  /** uses histogram algorithm to calculate loudness */
  EBUR128_MODE_HISTOGRAM = (1 << 6),
  /** can call ebur128_loudness_mix to preview arbitrary linear downmixes.
   *  Keeps per-block channel covariance data so that the integrated loudness
   *  of any linear mix of the physical input channels can be queried without
   *  replaying the audio. Cannot be combined with EBUR128_MODE_HISTOGRAM;
   *  ebur128_init() will return NULL if both are requested. */
  EBUR128_MODE_MIX_PREVIEW = (1 << 7) | EBUR128_MODE_I
};

/** forward declaration of ebur128_state_internal */
struct ebur128_state_internal;

/** \brief Contains information about the state of a loudness measurement.
 *
 *  You should not need to modify this struct directly.
 */
typedef struct {
  int mode;                         /**< The current mode. */
  unsigned int channels;            /**< The number of channels. */
  unsigned long samplerate;         /**< The sample rate. */
  struct ebur128_state_internal* d; /**< Internal state. */
} ebur128_state;

/** \brief Get library version number. Do not pass null pointers here.
 *
 *  @param major major version number of library
 *  @param minor minor version number of library
 *  @param patch patch version number of library
 */
void ebur128_get_version(int* major, int* minor, int* patch);

/** \brief Initialize library state.
 *
 *  @param channels the number of channels.
 *  @param samplerate the sample rate.
 *  @param mode see the mode enum for possible values.
 *  @return an initialized library state, or NULL on error.
 */
ebur128_state*
ebur128_init(unsigned int channels, unsigned long samplerate, int mode);

/** \brief Destroy library state.
 *
 *  @param st pointer to a library state.
 */
void ebur128_destroy(ebur128_state** st);

/** \brief Set channel type.
 *
 *  The default is:
 *  - 0 -> EBUR128_LEFT
 *  - 1 -> EBUR128_RIGHT
 *  - 2 -> EBUR128_CENTER
 *  - 3 -> EBUR128_UNUSED
 *  - 4 -> EBUR128_LEFT_SURROUND
 *  - 5 -> EBUR128_RIGHT_SURROUND
 *
 *  @param st library state.
 *  @param channel_number zero based channel index.
 *  @param value channel type from the "channel" enum.
 *  @return
 *    - EBUR128_SUCCESS on success.
 *    - EBUR128_ERROR_INVALID_CHANNEL_INDEX if invalid channel index.
 */
int ebur128_set_channel(ebur128_state* st,
                        unsigned int channel_number,
                        int value);

/** \brief Change library parameters.
 *
 *  Note that the channel map will be reset when setting a different number of
 *  channels. The current unfinished block will be lost.
 *
 *  @param st library state.
 *  @param channels new number of channels.
 *  @param samplerate new sample rate.
 *  @return
 *    - EBUR128_SUCCESS on success.
 *    - EBUR128_ERROR_NOMEM on memory allocation error. The state will be
 *      invalid and must be destroyed.
 *    - EBUR128_ERROR_NO_CHANGE if channels and sample rate were not changed.
 */
int ebur128_change_parameters(ebur128_state* st,
                              unsigned int channels,
                              unsigned long samplerate);

/** \brief Set the maximum window duration.
 *
 *  Set the maximum duration that will be used for ebur128_loudness_window().
 *  Note that this destroys the current content of the audio buffer.
 *
 *  @param st library state.
 *  @param window duration of the window in ms.
 *  @return
 *    - EBUR128_SUCCESS on success.
 *    - EBUR128_ERROR_NOMEM on memory allocation error. The state will be
 *      invalid and must be destroyed.
 *    - EBUR128_ERROR_NO_CHANGE if window duration not changed.
 */
int ebur128_set_max_window(ebur128_state* st, unsigned long window);

/** \brief Set the maximum history.
 *
 *  Set the maximum history that will be stored for loudness integration.
 *  More history provides more accurate results, but requires more resources.
 *
 *  Applies to ebur128_loudness_range() and ebur128_loudness_global() when
 *  EBUR128_MODE_HISTOGRAM is not set.
 *
 *  Default is ULONG_MAX (at least ~50 days).
 *  Minimum is 3000ms for EBUR128_MODE_LRA and 400ms for EBUR128_MODE_M.
 *
 *  @param st library state.
 *  @param history duration of history in ms.
 *  @return
 *    - EBUR128_SUCCESS on success.
 *    - EBUR128_ERROR_NO_CHANGE if history not changed.
 */
int ebur128_set_max_history(ebur128_state* st, unsigned long history);

/** \brief Add frames to be processed.
 *
 *  @param st library state.
 *  @param src array of source frames. Channels must be interleaved.
 *  @param frames number of frames. Not number of samples!
 *  @return
 *    - EBUR128_SUCCESS on success.
 *    - EBUR128_ERROR_NOMEM on memory allocation error.
 */
int ebur128_add_frames_short(ebur128_state* st,
                             const short* src,
                             size_t frames);
/** \brief See \ref ebur128_add_frames_short */
int ebur128_add_frames_int(ebur128_state* st, const int* src, size_t frames);
/** \brief See \ref ebur128_add_frames_short */
int ebur128_add_frames_float(ebur128_state* st,
                             const float* src,
                             size_t frames);
/** \brief See \ref ebur128_add_frames_short */
int ebur128_add_frames_double(ebur128_state* st,
                              const double* src,
                              size_t frames);

/** \brief Get global integrated loudness in LUFS.
 *
 *  @param st library state.
 *  @param out integrated loudness in LUFS. -HUGE_VAL if result is negative
 *             infinity.
 *  @return
 *    - EBUR128_SUCCESS on success.
 *    - EBUR128_ERROR_INVALID_MODE if mode "EBUR128_MODE_I" has not been set.
 */
int ebur128_loudness_global(ebur128_state* st, double* out);
/** \brief Get global integrated loudness in LUFS across multiple instances.
 *
 *  @param sts array of library states.
 *  @param size length of sts
 *  @param out integrated loudness in LUFS. -HUGE_VAL if result is negative
 *             infinity.
 *  @return
 *    - EBUR128_SUCCESS on success.
 *    - EBUR128_ERROR_INVALID_MODE if mode "EBUR128_MODE_I" has not been set.
 */
int ebur128_loudness_global_multiple(ebur128_state** sts,
                                     size_t size,
                                     double* out);

/** \brief Get integrated loudness in LUFS of a linear mix of the input
 *         channels, without replaying the audio.
 *
 *  Requires EBUR128_MODE_MIX_PREVIEW. The library stores, for every complete
 *  400ms gating block (produced every 100ms, like in mode I), the covariance
 *  matrix of the K-weighted physical input channels. This includes blocks
 *  below the -70 LUFS absolute gate and channels mapped to EBUR128_UNUSED,
 *  so any linear combination of the physical inputs can be evaluated later.
 *  The raw PCM is not kept.
 *
 *  The mix is described by a matrix in row-major order: matrix[r * channels
 *  + c] is the coefficient of physical input channel c in output channel r.
 *  The output channels are mixed linearly first; the energy of output
 *  channel r is then multiplied by weights[r]. Allowed weights are exactly
 *  0.0, 1.0, 1.41 and 2.0. Matrix coefficients may be positive or negative
 *  but must be finite. rows must be between 1 and 32. The channel map does
 *  not apply to the preview: matrix columns always refer to physical input
 *  channels and the only weights are those passed in weights.
 *
 *  Gating is performed on the mixed block energies: first the absolute
 *  -70 LUFS gate is applied to the mixed energy of each block, then the
 *  relative gate is computed from the blocks that passed, exactly like
 *  ebur128_loudness_global() does for the unmixed signal.
 *
 *  The number of blocks kept is controlled by ebur128_set_max_history():
 *  the most recent history/100 blocks (after the usual clamping of history,
 *  rounded down) are retained, counted by their time position; silent blocks
 *  count as well. The first block still needs 400ms of audio.
 *
 *  This function does not advance filtering, time or any gating statistics;
 *  querying multiple matrices interleaved with add_frames calls does not
 *  affect any measurement.
 *
 *  @param st library state.
 *  @param matrix row-major mix matrix, rows * channels finite coefficients.
 *  @param rows number of mix output channels, 1 to 32.
 *  @param weights per-output-channel energy weights, each 0.0, 1.0, 1.41 or
 *         2.0.
 *  @param out integrated loudness of the mix in LUFS. -HUGE_VAL if no block
 *         passes the gates. Not modified on error.
 *  @return
 *    - EBUR128_SUCCESS on success.
 *    - EBUR128_ERROR_INVALID_MODE if mode "EBUR128_MODE_MIX_PREVIEW" has not
 *      been set.
 *    - EBUR128_ERROR_INVALID_ARGUMENT if matrix, weights or out is NULL,
 *      rows is out of range, a matrix coefficient is not finite, or a
 *      weight is not one of the allowed values.
 */
int ebur128_loudness_mix(ebur128_state* st,
                         const double* matrix,
                         size_t rows,
                         const double* weights,
                         double* out);

/** \brief Get momentary loudness (last 400ms) in LUFS.
 *
 *  @param st library state.
 *  @param out momentary loudness in LUFS. -HUGE_VAL if result is negative
 *             infinity.
 *  @return
 *    - EBUR128_SUCCESS on success.
 */
int ebur128_loudness_momentary(ebur128_state* st, double* out);
/** \brief Get short-term loudness (last 3s) in LUFS.
 *
 *  @param st library state.
 *  @param out short-term loudness in LUFS. -HUGE_VAL if result is negative
 *             infinity.
 *  @return
 *    - EBUR128_SUCCESS on success.
 *    - EBUR128_ERROR_INVALID_MODE if mode "EBUR128_MODE_S" has not been set.
 */
int ebur128_loudness_shortterm(ebur128_state* st, double* out);

/** \brief Get loudness of the specified window in LUFS.
 *
 *  window must not be larger than the current window set in st.
 *  The current window can be changed by calling ebur128_set_max_window().
 *
 *  @param st library state.
 *  @param window window in ms to calculate loudness.
 *  @param out loudness in LUFS. -HUGE_VAL if result is negative infinity.
 *  @return
 *    - EBUR128_SUCCESS on success.
 *    - EBUR128_ERROR_INVALID_MODE if window larger than current window in st.
 */
int ebur128_loudness_window(ebur128_state* st,
                            unsigned long window,
                            double* out);

/** \brief Get loudness range (LRA) of programme in LU.
 *
 *  Calculates loudness range according to EBU 3342.
 *
 *  @param st library state.
 *  @param out loudness range (LRA) in LU. Will not be changed in case of
 *             error. EBUR128_ERROR_NOMEM or EBUR128_ERROR_INVALID_MODE will be
 *             returned in this case.
 *  @return
 *    - EBUR128_SUCCESS on success.
 *    - EBUR128_ERROR_NOMEM in case of memory allocation error.
 *    - EBUR128_ERROR_INVALID_MODE if mode "EBUR128_MODE_LRA" has not been set.
 */
int ebur128_loudness_range(ebur128_state* st, double* out);
/** \brief Get loudness range (LRA) in LU across multiple instances.
 *
 *  Calculates loudness range according to EBU 3342.
 *
 *  @param sts array of library states.
 *  @param size length of sts
 *  @param out loudness range (LRA) in LU. Will not be changed in case of
 *             error. EBUR128_ERROR_NOMEM or EBUR128_ERROR_INVALID_MODE will be
 *             returned in this case.
 *  @return
 *    - EBUR128_SUCCESS on success.
 *    - EBUR128_ERROR_NOMEM in case of memory allocation error.
 *    - EBUR128_ERROR_INVALID_MODE if mode "EBUR128_MODE_LRA" has not been set.
 */
int ebur128_loudness_range_multiple(ebur128_state** sts,
                                    size_t size,
                                    double* out);

/** \brief Get maximum sample peak from all frames that have been processed.
 *
 *  The equation to convert to dBFS is: 20 * log10(out)
 *
 *  @param st library state
 *  @param channel_number channel to analyse
 *  @param out maximum sample peak in float format (1.0 is 0 dBFS)
 *  @return
 *    - EBUR128_SUCCESS on success.
 *    - EBUR128_ERROR_INVALID_MODE if mode "EBUR128_MODE_SAMPLE_PEAK" has not
 *      been set.
 *    - EBUR128_ERROR_INVALID_CHANNEL_INDEX if invalid channel index.
 */
int ebur128_sample_peak(ebur128_state* st,
                        unsigned int channel_number,
                        double* out);

/** \brief Get maximum sample peak from the last call to add_frames().
 *
 *  The equation to convert to dBFS is: 20 * log10(out)
 *
 *  @param st library state
 *  @param channel_number channel to analyse
 *  @param out maximum sample peak in float format (1.0 is 0 dBFS)
 *  @return
 *    - EBUR128_SUCCESS on success.
 *    - EBUR128_ERROR_INVALID_MODE if mode "EBUR128_MODE_SAMPLE_PEAK" has not
 *      been set.
 *    - EBUR128_ERROR_INVALID_CHANNEL_INDEX if invalid channel index.
 */
int ebur128_prev_sample_peak(ebur128_state* st,
                             unsigned int channel_number,
                             double* out);

/** \brief Get maximum true peak from all frames that have been processed.
 *
 *  Uses an implementation defined algorithm to calculate the true peak. Do not
 *  try to compare resulting values across different versions of the library,
 *  as the algorithm may change.
 *
 *  The current implementation uses a custom polyphase FIR interpolator to
 *  calculate true peak. Will oversample 4x for sample rates < 96000 Hz, 2x for
 *  sample rates < 192000 Hz and leave the signal unchanged for 192000 Hz.
 *
 *  The equation to convert to dBTP is: 20 * log10(out)
 *
 *  @param st library state
 *  @param channel_number channel to analyse
 *  @param out maximum true peak in float format (1.0 is 0 dBTP)
 *  @return
 *    - EBUR128_SUCCESS on success.
 *    - EBUR128_ERROR_INVALID_MODE if mode "EBUR128_MODE_TRUE_PEAK" has not
 *      been set.
 *    - EBUR128_ERROR_INVALID_CHANNEL_INDEX if invalid channel index.
 */
int ebur128_true_peak(ebur128_state* st,
                      unsigned int channel_number,
                      double* out);

/** \brief Get maximum true peak from the last call to add_frames().
 *
 *  Uses an implementation defined algorithm to calculate the true peak. Do not
 *  try to compare resulting values across different versions of the library,
 *  as the algorithm may change.
 *
 *  The current implementation uses a custom polyphase FIR interpolator to
 *  calculate true peak. Will oversample 4x for sample rates < 96000 Hz, 2x for
 *  sample rates < 192000 Hz and leave the signal unchanged for 192000 Hz.
 *
 *  The equation to convert to dBTP is: 20 * log10(out)
 *
 *  @param st library state
 *  @param channel_number channel to analyse
 *  @param out maximum true peak in float format (1.0 is 0 dBTP)
 *  @return
 *    - EBUR128_SUCCESS on success.
 *    - EBUR128_ERROR_INVALID_MODE if mode "EBUR128_MODE_TRUE_PEAK" has not
 *      been set.
 *    - EBUR128_ERROR_INVALID_CHANNEL_INDEX if invalid channel index.
 */
int ebur128_prev_true_peak(ebur128_state* st,
                           unsigned int channel_number,
                           double* out);

/** \brief Get relative threshold in LUFS.
 *
 *  @param st library state
 *  @param out relative threshold in LUFS.
 *  @return
 *    - EBUR128_SUCCESS on success.
 *    - EBUR128_ERROR_INVALID_MODE if mode "EBUR128_MODE_I" has not
 *      been set.
 */
int ebur128_relative_threshold(ebur128_state* st, double* out);

#ifdef __cplusplus
}
#endif

#endif /* EBUR128_H_ */
