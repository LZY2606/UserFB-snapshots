/* Independent scalar reference for the true-peak regression tests.
 *
 * This reference deliberately does not include ebur128.h and does not reuse
 * the library's polyphase table, ring buffer or internal helpers.  It performs
 * explicit zero insertion followed by a time-domain FIR convolution using a
 * Hann-windowed sinc prototype, exactly as described in true_peak_reference.c.
 */

#ifndef TRUE_PEAK_REFERENCE_H_
#define TRUE_PEAK_REFERENCE_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  TP_REF_SHORT = 0,
  TP_REF_INT,
  TP_REF_FLOAT,
  TP_REF_DOUBLE
} tp_ref_type;

typedef struct tp_reference tp_reference;

/* samplerate selects interpolation just like the library: 4x below 96 kHz,
 * 2x below 192 kHz, and no interpolation at 192 kHz and above. */
tp_reference* tp_reference_create(unsigned long samplerate, unsigned int channels);
void tp_reference_destroy(tp_reference** ref);

/* Process one add_frames()-equivalent call.  For integer types src holds the
 * *already quantized* samples; the reference derives its own normalization
 * from those quantized values.  prev_* statistics describe only this call,
 * while global_* statistics accumulate over all calls. */
int tp_reference_add_frames(tp_reference* ref, tp_ref_type type,
                            const void* src, size_t frames);

void tp_reference_prev_true_peak(const tp_reference* ref, unsigned int channel,
                                 double* out);
void tp_reference_prev_sample_peak(const tp_reference* ref, unsigned int channel,
                                   double* out);
void tp_reference_true_peak(const tp_reference* ref, unsigned int channel,
                            double* out);
void tp_reference_sample_peak(const tp_reference* ref, unsigned int channel,
                              double* out);

#ifdef __cplusplus
}
#endif

#endif /* TRUE_PEAK_REFERENCE_H_ */
