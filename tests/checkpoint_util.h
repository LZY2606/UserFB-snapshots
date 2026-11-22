/*
** Shared deterministic signal generation for the checkpoint tests and the
** checkpoint child helper. No Python, no external data files: the same seed
** reproduces the same PCM in another process.
*/

#ifndef CHECKPOINT_UTIL_H
#define CHECKPOINT_UTIL_H

#include <stdint.h>
#include <math.h>

#define CKPT_SIGNAL_FRAMES	20000
#define CKPT_CHUNK_FRAMES	2500

/* Callback mode needs batches larger than the sinc ring (which can hold
** roughly 112k frames for the best quality converter). The callback source
** therefore uses a longer signal and hands back a single oversized batch.
*/
#define CKPT_CB_SIGNAL_FRAMES	300000
#define CKPT_CB_BATCH		200000

/* Non periodic, bounded, deterministic interleaved PCM: LCG noise plus
** isolated impulses so that filter history and ring wraparound both see
** distinctive samples on every channel.
*/
static inline void
ckpt_gen_signal (float *buffer, long frames, int channels, uint32_t seed)
{
	uint32_t r = seed ? seed : 0x9e3779b9u ;

	for (long f = 0 ; f < frames ; f++)
	{
		for (int c = 0 ; c < channels ; c++)
		{
			float pulse, noise ;

			r = r * 1664525u + 1013904223u ;
			noise = ((float) ((int) (r >> 16) - 32768) / 32768.0f) * 0.4f ;

			pulse = 0.0f ;
			if (f % 97 == 3)
				pulse = (c == 0) ? 0.9f : (0.35f + 0.05f * c) ;
			else if (f % 211 == 100)
				pulse = -0.7f ;

			buffer [f * channels + c] = noise + pulse ;
		} ;
	} ;
} /* ckpt_gen_signal */

static inline const int *
ckpt_output_caps (int *count)
{
	static const int caps [] = { 1, 7, 17 } ;

	*count = (int) (sizeof (caps) / sizeof (caps [0])) ;
	return caps ;
} /* ckpt_output_caps */

static inline int
ckpt_cap_at (long step)
{
	int count ;
	const int *caps = ckpt_output_caps (&count) ;

	return caps [step % count] ;
} /* ckpt_cap_at */

#endif
