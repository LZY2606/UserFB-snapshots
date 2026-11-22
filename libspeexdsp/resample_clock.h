/* Copyright (C) 2026 Xiph.Org Foundation and contributors

   File: resample_clock.h
   Precise rational clock for the arbitrary resampler.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

   1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   3. The name of the author may not be used to endorse or promote products
   derived from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
   IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
   OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
   DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
   INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
   (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
   HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
   STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
   ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
   POSSIBILITY OF SUCH DAMAGE.
*/

/* The precise clock maintains the nominal source cursor U of the next
   output frame as an exact fixed-point number: U = whole + fraction*2^-64,
   measured in input frames.  Each produced output frame advances U by the
   current ratio num/den.  The accumulation is carried out in exact 64.64
   fixed point (128-bit intermediate products), so changing the ratio never
   rescales or truncates a position that has already accumulated.  The same
   64.64 position drives the integer sample index and the fractional
   interpolation phase of the FIR resampler; the clock is not a separate
   bookkeeping ledger. */

#ifndef RESAMPLE_CLOCK_H
#define RESAMPLE_CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SpeexResampleClock_ SpeexResampleClock;

/* Allocate and zero a clock for nb_channels independent cursors. */
SpeexResampleClock *speex_resample_clock_init(unsigned nb_channels);

/* Free a clock. */
void speex_resample_clock_destroy(SpeexResampleClock *clock);

/* Reset every cursor to U = 0 (used by speex_resampler_reset_mem). */
void speex_resample_clock_reset(SpeexResampleClock *clock);

/* Set the ratio used for all subsequent advances.  The fraction is stored
   verbatim (already reduced by the caller); it must satisfy
   num <= 1000000, den <= 1000000 and 1/2 <= num/den <= 2.  This never
   touches an already accumulated cursor. */
void speex_resample_clock_set_ratio(SpeexResampleClock *clock,
                                    uint32_t num, uint32_t den);

/* Exact cursor U = *whole + *fraction * 2^-64 for one channel. */
void speex_resample_clock_get(const SpeexResampleClock *clock, unsigned channel,
                              uint64_t *whole, uint64_t *fraction);

/* Fractional phase in [0,1), using one double rounding only at the point the
   interpolator consumes it.  The exact cursor itself stays integer. */
double speex_resample_clock_phase_one(const SpeexResampleClock *clock,
                                      unsigned channel);

/* Integer source frame index of the current read point. */
int32_t speex_resample_clock_whole(const SpeexResampleClock *clock,
                                   unsigned channel);

/* Advance one channel by one output frame, i.e. U += num/den, and return the
   carry (number of whole input frames crossed, 0, 1 or 2 for ratios in
   [1/2, 2]).  The old phase (before the advance) must be read beforehand
   with the get/phase accessors. */
unsigned speex_resample_clock_advance(SpeexResampleClock *clock,
                                      unsigned channel);

#ifdef __cplusplus
}
#endif

#endif /* RESAMPLE_CLOCK_H */
