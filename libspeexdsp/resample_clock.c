/* Copyright (C) 2026 Xiph.Org Foundation and contributors

   File: resample_clock.c
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <math.h>

#include "resample_clock.h"

/* One per-channel cursor: U = whole + fraction * 2^-64 input frames. */
typedef struct {
   uint64_t whole;
   uint64_t fraction;
} SpeexClockCursor;

struct SpeexResampleClock_ {
   unsigned nb_channels;
   uint32_t ratio_num;
   uint32_t ratio_den;
   SpeexClockCursor *cursor;
};

SpeexResampleClock *speex_resample_clock_init(unsigned nb_channels)
{
   SpeexResampleClock *clock;
   if (nb_channels == 0)
      return NULL;
   clock = (SpeexResampleClock *)calloc(1, sizeof(*clock));
   if (!clock)
      return NULL;
   clock->cursor = (SpeexClockCursor *)calloc(nb_channels, sizeof(*clock->cursor));
   if (!clock->cursor)
   {
      free(clock);
      return NULL;
   }
   clock->nb_channels = nb_channels;
   /* Valid ratios are always positive; 1/1 is a harmless default until the
      first real ratio is installed. */
   clock->ratio_num = 1;
   clock->ratio_den = 1;
   return clock;
}

void speex_resample_clock_destroy(SpeexResampleClock *clock)
{
   if (!clock)
      return;
   free(clock->cursor);
   free(clock);
}

void speex_resample_clock_reset(SpeexResampleClock *clock)
{
   unsigned i;
   if (!clock)
      return;
   for (i = 0; i < clock->nb_channels; i++)
   {
      clock->cursor[i].whole = 0;
      clock->cursor[i].fraction = 0;
   }
}

void speex_resample_clock_set_ratio(SpeexResampleClock *clock,
                                    uint32_t num, uint32_t den)
{
   clock->ratio_num = num;
   clock->ratio_den = den;
}

void speex_resample_clock_get(const SpeexResampleClock *clock, unsigned channel,
                              uint64_t *whole, uint64_t *fraction)
{
   const SpeexClockCursor *c = &clock->cursor[channel];
   *whole = c->whole;
   *fraction = c->fraction;
}

double speex_resample_clock_phase_one(const SpeexResampleClock *clock,
                                      unsigned channel)
{
   return ldexp((double)clock->cursor[channel].fraction, -64);
}

int32_t speex_resample_clock_whole(const SpeexResampleClock *clock,
                                   unsigned channel)
{
   return (int32_t)clock->cursor[channel].whole;
}

unsigned speex_resample_clock_advance(SpeexResampleClock *clock,
                                      unsigned channel)
{
   SpeexClockCursor *c = &clock->cursor[channel];
   const uint32_t num = clock->ratio_num;
   const uint32_t den = clock->ratio_den;
   const uint64_t old_whole = c->whole;
#if defined(__SIZEOF_INT128__) && !defined(RESAMPLE_CLOCK_FORCE_EMULATION)
   unsigned __int128 step = ((unsigned __int128)num << 64) / den;
   unsigned __int128 pos = ((unsigned __int128)c->whole << 64) | c->fraction;
   pos += step;
   c->whole = (uint64_t)(pos >> 64);
   c->fraction = (uint64_t)pos;
#else
   /* step = num/den * 2^64, split into a whole part and a 64-bit fraction. */
   uint64_t step_hi = (uint64_t)(num / den);
   uint32_t rem = num % den;
   uint64_t step_lo;
   uint64_t lo;
   /* (rem << 64) / den, rem and den both below 2^32: evaluate in two 32-bit
      chunks so nothing overflows a uint64_t. */
   uint64_t a = ((uint64_t)rem << 32) / den;
   uint64_t b = (((uint64_t)rem << 32) % den);
   step_lo = (a << 32) + (b << 32) / den;
   lo = c->fraction + step_lo;
   c->whole = c->whole + step_hi + (lo < c->fraction);
   c->fraction = lo;
#endif
   /* Ratios are constrained to [1/2, 2], so a single-frame advance crosses at
      most two integer input frames. */
   return (unsigned)(c->whole - old_whole);
}
