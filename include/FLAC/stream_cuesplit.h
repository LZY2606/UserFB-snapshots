/* libFLAC - Free Lossless Audio Codec library
 * Copyright (C) 2025  Xiph.Org Foundation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * - Neither the name of the Xiph.org Foundation nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef FLAC__STREAM_CUESPLIT_H
#define FLAC__STREAM_CUESPLIT_H

#include "export.h"
#include "format.h"
#include "ordinals.h"
#include "stream_decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/** \file include/FLAC/stream_cuesplit.h
 *
 *  \brief
 *  This module splits a single native FLAC file carrying a CUESHEET
 *  into one independent FLAC file per music track, reading the source
 *  exactly once through a forward-only input channel.
 */

/** \defgroup flac_cuesplit FLAC/stream_cuesplit.h: cue sheet based stream splitter
 *  \ingroup flac
 *
 *  \brief
 *  Splits a cuesheeted native FLAC stream into per-track FLAC streams
 *  with a single sequential decoding pass.
 *
 *  The input must be a single native (non-Ogg) FLAC stream without an
 *  ID3 tag, with a known non-zero STREAMINFO total sample count and
 *  exactly one non-CD CUESHEET metadata block describing one or more
 *  audio tracks followed by the lead-out.  Every decoded input sample
 *  is routed to exactly one output stream; no PCM and no compressed
 *  frame data is buffered beyond the size of one decoded block.
 *
 *  Output is produced through the \link FLAC__CueSplitIO callbacks
 *  \endlink.  Only one output track is open at a time; the write
 *  callback receives absolute byte offsets so the STREAMINFO header of
 *  the current output may be rewritten in place when the track ends.
 *  The input side never seeks: the source decoder is initialized
 *  without seek, tell, length or EOF callbacks.
 *
 *  \{
 */

/** Outcome of a FLAC__stream_cuesplit() run.
 */
typedef enum {

	FLAC__CUESPLIT_STATUS_OK = 0,
	/**< Every input sample was split and every output track was
	 * finished and closed successfully. */

	FLAC__CUESPLIT_STATUS_INVALID_METADATA,
	/**< The input metadata or CUESHEET was missing, unsupported or
	 * inconsistent; no output was created. */

	FLAC__CUESPLIT_STATUS_DECODE_ERROR,
	/**< The source decoder reported an error or ended before all
	 * samples named by the CUESHEET had been decoded. */

	FLAC__CUESPLIT_STATUS_SOURCE_MD5_MISMATCH,
	/**< The source STREAMINFO carried a non-zero MD5 signature that
	 * does not match the PCM that was decoded.  This status is only
	 * determined after the whole stream has been consumed; tracks
	 * already finished with end(..., \c true) are not rolled back. */

	FLAC__CUESPLIT_STATUS_IO_ERROR
	/**< One of the read, begin or write callbacks failed. */

} FLAC__CueSplitStatus;

/** Selects which output owns the pregap (INDEX 00 area) of a track.
 */
typedef enum {

	FLAC__CUESPLIT_PREGAP_NEXT = 0,
	/**< Each track spans the half-open sample interval from its own
	 * track offset to the next track offset.  The pregap recorded at
	 * the start of a track is attached to that same track; the first
	 * output track starts at sample 0. */

	FLAC__CUESPLIT_PREGAP_PREVIOUS
	/**< Each track starts at its INDEX 01 point and runs to the next
	 * INDEX 01; the last track runs to the lead-out.  Pregap audio
	 * before the first INDEX 01, if any, is emitted as a separate
	 * output with track number 0. */

} FLAC__CueSplitPregapPolicy;

/** Read callback for the forward-only source.
 *
 *  Has the same short-read and end-of-stream semantics as the read
 *  callback of FLAC__stream_decoder_init_stream(): the callee may
 *  return fewer bytes than requested, must set the byte count to zero
 *  together with FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM only
 *  when no more input will ever be available, and uses
 *  FLAC__STREAM_DECODER_READ_STATUS_ABORT to signal an unrecoverable
 *  input error.
 */
typedef FLAC__StreamDecoderReadStatus (*FLAC__CueSplitReadCallback)(void *client_data, FLAC__byte buffer[], size_t *bytes);

/** Opens one output track.
 *
 *  Called before the first byte of track \a track is written.
 *  \a samples is the exact number of source samples that will be
 *  encoded into this output.  Return \c false to abort the whole run.
 */
typedef FLAC__bool (*FLAC__CueSplitBeginCallback)(void *client_data, uint32_t track, FLAC__uint64 samples);

/** Writes one chunk of the current output track.
 *
 *  \a byte_offset is the absolute byte position of \a buffer within
 *  this output track; it may be smaller than the previously reported
 *  position because the STREAMINFO header is rewritten when the track
 *  is finished.  The buffer must be consumed before the callback
 *  returns.  Return \c false to abort the whole run.
 */
typedef FLAC__bool (*FLAC__CueSplitWriteCallback)(void *client_data, uint32_t track, FLAC__uint64 byte_offset, const FLAC__byte buffer[], size_t bytes);

/** Closes the current output track.
 *
 *  Exactly one call is paired with every successful begin callback.
 *  \a complete is \c true when the track was encoded fully and its
 *  header was rewritten successfully, \c false when the run was
 *  aborted by a decode or I/O error first.
 */
typedef void (*FLAC__CueSplitEndCallback)(void *client_data, uint32_t track, FLAC__bool complete);

/** Callbacks and opaque client data used by FLAC__stream_cuesplit().
 */
typedef struct {

	void *client_data;
	/**< Passed unchanged to every callback. */

	FLAC__CueSplitReadCallback read;
	/**< Mandatory source input callback. */

	FLAC__CueSplitBeginCallback begin;
	/**< Mandatory output open callback. */

	FLAC__CueSplitWriteCallback write;
	/**< Mandatory output write callback. */

	FLAC__CueSplitEndCallback end;
	/**< Mandatory output close callback. */

} FLAC__CueSplitIO;

/** Splits one cuesheeted native FLAC stream into per-track FLAC
 *  streams.
 *
 *  The function runs synchronously and returns only after every
 *  output has been closed (or the run was aborted).  Apart from
 *  metadata it only allocates scratch memory proportional to the
 *  largest decoded block size and the channel count; the working set
 *  never grows with the total sample count or the length of a track.
 *
 * \param io      Source and sink callbacks; none may be \c NULL.
 * \param policy  Pregap ownership rule, see FLAC__CueSplitPregapPolicy.
 * \retval FLAC__CueSplitStatus
 *    See the individual status descriptions.
 */
FLAC_API FLAC__CueSplitStatus FLAC__stream_cuesplit(const FLAC__CueSplitIO *io, FLAC__CueSplitPregapPolicy policy);

/* \} */

#ifdef __cplusplus
}
#endif

#endif
