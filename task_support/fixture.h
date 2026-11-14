#ifndef TASK_FIXTURE_H
#define TASK_FIXTURE_H
#include <stddef.h>
#include <stdint.h>
/* Basic MPEG-1 intra-only material; no decoder or expected target output. */
typedef struct { uint8_t bytes[65536]; size_t bit; } fixture_t;
void fixture_bits(fixture_t *, unsigned value, unsigned count);
void fixture_start(fixture_t *, unsigned code);
void fixture_sequence(fixture_t *, unsigned width, unsigned height, unsigned rate_code, unsigned aspect_code);
void fixture_intra(fixture_t *, unsigned width, unsigned height, unsigned temporal_reference, unsigned gray);
void fixture_end(fixture_t *);
size_t fixture_size(const fixture_t *);
void fixture_ps(fixture_t *out, const fixture_t *es);
#endif
