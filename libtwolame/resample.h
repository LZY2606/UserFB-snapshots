/*
 *  TwoLAME: an optimized MPEG Audio Layer Two encoder
 *
 *  Copyright (C) 2004-2018 The TwoLAME Project
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#ifndef TWOLAME_RESAMPLE_H
#define TWOLAME_RESAMPLE_H

#ifdef __cplusplus
extern "C" {
#endif

/** \file resample.h
 *  Continuous band-limited sample rate converter.
 *
 *  All channels share a single output sample schedule, while each
 *  channel keeps its own filter history. The converter is streaming:
 *  output samples that need input samples not yet supplied are withheld
 *  until more input arrives. twolame_resampler_flush() zero-extends the
 *  source at both ends and completes the logical output interval
 *  ceil(N * out_rate / in_rate).
 */

/** Opaque resampler state. */
typedef struct twolame_resampler twolame_resampler;

/** Create a resampler.
 *
 *  \param in_rate        input sample rate in Hz (8000..96000)
 *  \param out_rate       output sample rate in Hz
 *  \param num_channels   1 or 2 channels, each with independent history
 *  \return NULL on failure (bad arguments or out of memory)
 */
twolame_resampler *twolame_resampler_create(int in_rate, int out_rate, int num_channels);

/** Free a resampler created by twolame_resampler_create(). */
void twolame_resampler_destroy(twolame_resampler *rs);

/** Feed planar (one buffer per channel) 16-bit samples.
 *
 *  All num_samples input samples per channel are consumed. Resampled
 *  samples are returned through out[] (valid until the next call on the
 *  same resampler). Output may be deferred when the filter needs future
 *  input, so *out_n can be zero.
 *
 *  \return 0 on success, -1 on failure
 */
int twolame_resampler_process(twolame_resampler *rs,
                              const short *const in[],
                              int num_samples,
                              short *out[],
                              int *out_n);

/** Feed interleaved 16-bit samples (channels stored frame by frame).
 *  Behaves identically to twolame_resampler_process() otherwise. */
int twolame_resampler_process_interleaved(twolame_resampler *rs,
                                          const short pcm[],
                                          int num_samples,
                                          int num_channels,
                                          short *out[],
                                          int *out_n);

/** Flush the stream.
 *
 *  The source is zero-extended at both ends and every remaining sample
 *  of the logical output interval is produced. Repeated calls return no
 *  samples. out[] is valid until the next call.
 *
 *  \return 0 on success, -1 on failure
 */
int twolame_resampler_flush(twolame_resampler *rs, short *out[], int *out_n);

/** Number of resampled frames produced so far (the shared schedule
 *  position k). After flush this equals the logical output length. */
unsigned long long twolame_resampler_count(const twolame_resampler *rs);

/** Access an internal per-channel scratch buffer of at least need shorts.
 *  Used by the float front ends to stage the short conversion. */
short *twolame_resampler_scratch(twolame_resampler *rs, int channel, int need);

#ifdef __cplusplus
}
#endif

#endif                          /* TWOLAME_RESAMPLE_H */

// vim:ts=4:sw=4:nowrap:
