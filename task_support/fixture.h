#ifndef TASK_FIXTURE_H
#define TASK_FIXTURE_H
#include <stddef.h>
#include <stdint.h>
/* Basic MPEG-1 intra-only material; no decoder or expected target output. */
typedef struct { uint8_t bytes[65536]; size_t bit; } fixture_t;
void fixture_bits(fixture_t *, unsigned value, unsigned count);
void fixture_start(fixture_t *, unsigned code);
void fixture_sequence(fixture_t *, unsigned width, unsigned height, unsigned rate_code, unsigned aspect_code);
/* Sequence header with explicit quant matrix selection. When the corresponding
 * custom flag is set, a fixed recognizable matrix is written; otherwise the
 * header announces that the default matrix is used. */
void fixture_sequence_quant(fixture_t *, unsigned width, unsigned height,
	unsigned rate_code, unsigned aspect_code,
	int custom_intra, int custom_non_intra);
void fixture_intra(fixture_t *, unsigned width, unsigned height, unsigned temporal_reference, unsigned gray);
/* Predictive picture: motion-compensated copy with a uniform luma DC residual
 * added to every 8x8 luma block. level 0 produces a plain copy of the forward
 * reference. */
void fixture_predictive(fixture_t *, unsigned width, unsigned height, unsigned temporal_reference, int level);
/* Bidirective picture: zero-motion average of the forward and backward refs. */
void fixture_bidirect(fixture_t *, unsigned width, unsigned height, unsigned temporal_reference);
void fixture_end(fixture_t *);
size_t fixture_size(const fixture_t *);
void fixture_ps(fixture_t *out, const fixture_t *es);
#endif
