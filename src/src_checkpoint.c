/*
** Copyright (c) 2024, libsamplerate contributors
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

/*
** Persistent checkpoint (de)serialisation for SRC_STATE.
**
** The on-disk/on-wire format described here is part of the public API and is
** stable across processes and host architectures:
**
**   * every integer field is stored fixed width, little endian;
**   * float and double fields are stored as their IEEE-754 bit patterns
**     (float as uint32, double as uint64), little endian;
**   * the format contains no pointers, no structure padding and no host
**     dependent types such as long;
**   * the stream is read and written byte by byte, so the buffer needs no
**     particular alignment.
**
** Format version 1, all fields in storage order:
**
**   offset  size        field
**   0       4           magic, the four ASCII bytes 'S' 'R' 'C' 'P'
**   4       2           uint16 format version (currently 1)
**   6       2           uint16 converter number (0..2, the three sinc converters)
**   8       2           uint16 channel count (>= 1)
**   10      2           uint16 mode (0 = SRC_MODE_PROCESS, 1 = SRC_MODE_CALLBACK)
**   12      8           double state last_ratio (0.0 for a never used state)
**   20      8           double state last_position, fractional position in [0, 1)
**   28      4           uint32 callback remainder frame count P (always 0 in
**                            process mode)
**   32      4*P*C       float callback remainder samples, interleaved, C channels
**   ...     4           uint32 converter payload length N in bytes
**   ...     N           converter payload (see src_sinc.c)
**
** A process mode checkpoint may only be restored with a NULL callback.
** A callback mode checkpoint may only be restored with a non-NULL callback.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdint.h>
#include <string.h>

#include "samplerate.h"
#include "common.h"

#define	SRC_CP_MAGIC_0		'S'
#define	SRC_CP_MAGIC_1		'R'
#define	SRC_CP_MAGIC_2		'C'
#define	SRC_CP_MAGIC_3		'P'

#define	SRC_CP_VERSION		1

#define	SRC_CP_HEADER_BYTES	32

/*----------------------------------------------------------------------------------------
** Byte order independent primitive writers and readers.
*/

LIBSAMPLERATE_DLL_PRIVATE void
src_cp_writer_init (SRC_CP_WRITER *writer, void *buffer, size_t bytes)
{
	writer->data = (unsigned char *) buffer ;
	writer->offset = 0 ;
	writer->capacity = bytes ;
} /* src_cp_writer_init */

LIBSAMPLERATE_DLL_PRIVATE void
src_cp_reader_init (SRC_CP_READER *reader, const void *buffer, size_t bytes)
{
	reader->data = (const unsigned char *) buffer ;
	reader->offset = 0 ;
	reader->length = bytes ;
} /* src_cp_reader_init */

static SRC_ERROR
cp_put (SRC_CP_WRITER *writer, const void *data, size_t bytes)
{
	if (writer->offset > writer->capacity || bytes > writer->capacity - writer->offset)
		return SRC_ERR_CHECKPOINT_TRUNCATED ;

	memcpy (writer->data + writer->offset, data, bytes) ;
	writer->offset += bytes ;
	return SRC_ERR_NO_ERROR ;
} /* cp_put */

static SRC_ERROR
cp_get (SRC_CP_READER *reader, void *data, size_t bytes)
{
	if (reader->offset > reader->length || bytes > reader->length - reader->offset)
		return SRC_ERR_CHECKPOINT_TRUNCATED ;

	memcpy (data, reader->data + reader->offset, bytes) ;
	reader->offset += bytes ;
	return SRC_ERR_NO_ERROR ;
} /* cp_get */

LIBSAMPLERATE_DLL_PRIVATE SRC_ERROR
src_cp_write_u16 (SRC_CP_WRITER *writer, uint16_t value)
{
	unsigned char bytes [2] ;

	bytes [0] = (unsigned char) (value & 0xFF) ;
	bytes [1] = (unsigned char) ((value >> 8) & 0xFF) ;

	return cp_put (writer, bytes, sizeof (bytes)) ;
} /* src_cp_write_u16 */

LIBSAMPLERATE_DLL_PRIVATE SRC_ERROR
src_cp_write_u32 (SRC_CP_WRITER *writer, uint32_t value)
{
	unsigned char bytes [4] ;

	bytes [0] = (unsigned char) (value & 0xFF) ;
	bytes [1] = (unsigned char) ((value >> 8) & 0xFF) ;
	bytes [2] = (unsigned char) ((value >> 16) & 0xFF) ;
	bytes [3] = (unsigned char) ((value >> 24) & 0xFF) ;

	return cp_put (writer, bytes, sizeof (bytes)) ;
} /* src_cp_write_u32 */

LIBSAMPLERATE_DLL_PRIVATE SRC_ERROR
src_cp_write_u64 (SRC_CP_WRITER *writer, uint64_t value)
{
	unsigned char bytes [8] ;

	for (int k = 0 ; k < 8 ; k++)
		bytes [k] = (unsigned char) ((value >> (8 * k)) & 0xFF) ;

	return cp_put (writer, bytes, sizeof (bytes)) ;
} /* src_cp_write_u64 */

LIBSAMPLERATE_DLL_PRIVATE SRC_ERROR
src_cp_write_f32 (SRC_CP_WRITER *writer, float value)
{
	uint32_t bits ;

	memcpy (&bits, &value, sizeof (bits)) ;
	return src_cp_write_u32 (writer, bits) ;
} /* src_cp_write_f32 */

LIBSAMPLERATE_DLL_PRIVATE SRC_ERROR
src_cp_write_f64 (SRC_CP_WRITER *writer, double value)
{
	uint64_t bits ;

	memcpy (&bits, &value, sizeof (bits)) ;
	return src_cp_write_u64 (writer, bits) ;
} /* src_cp_write_f64 */

LIBSAMPLERATE_DLL_PRIVATE SRC_ERROR
src_cp_write_bytes (SRC_CP_WRITER *writer, const void *data, size_t bytes)
{
	return cp_put (writer, data, bytes) ;
} /* src_cp_write_bytes */

LIBSAMPLERATE_DLL_PRIVATE SRC_ERROR
src_cp_read_u16 (SRC_CP_READER *reader, uint16_t *value)
{
	unsigned char bytes [2] ;
	SRC_ERROR error ;

	if ((error = cp_get (reader, bytes, sizeof (bytes))) != 0)
		return error ;

	*value = (uint16_t) (bytes [0] | ((uint16_t) bytes [1] << 8)) ;
	return SRC_ERR_NO_ERROR ;
} /* src_cp_read_u16 */

LIBSAMPLERATE_DLL_PRIVATE SRC_ERROR
src_cp_read_u32 (SRC_CP_READER *reader, uint32_t *value)
{
	unsigned char bytes [4] ;
	SRC_ERROR error ;

	if ((error = cp_get (reader, bytes, sizeof (bytes))) != 0)
		return error ;

	*value = (uint32_t) bytes [0] | ((uint32_t) bytes [1] << 8) |
				((uint32_t) bytes [2] << 16) | ((uint32_t) bytes [3] << 24) ;
	return SRC_ERR_NO_ERROR ;
} /* src_cp_read_u32 */

LIBSAMPLERATE_DLL_PRIVATE SRC_ERROR
src_cp_read_u64 (SRC_CP_READER *reader, uint64_t *value)
{
	unsigned char bytes [8] ;
	SRC_ERROR error ;

	if ((error = cp_get (reader, bytes, sizeof (bytes))) != 0)
		return error ;

	*value = 0 ;
	for (int k = 0 ; k < 8 ; k++)
		*value |= ((uint64_t) bytes [k]) << (8 * k) ;
	return SRC_ERR_NO_ERROR ;
} /* src_cp_read_u64 */

LIBSAMPLERATE_DLL_PRIVATE SRC_ERROR
src_cp_read_f32 (SRC_CP_READER *reader, float *value)
{
	uint32_t bits ;
	SRC_ERROR error ;

	if ((error = src_cp_read_u32 (reader, &bits)) != 0)
		return error ;

	memcpy (value, &bits, sizeof (*value)) ;
	return SRC_ERR_NO_ERROR ;
} /* src_cp_read_f32 */

LIBSAMPLERATE_DLL_PRIVATE SRC_ERROR
src_cp_read_f64 (SRC_CP_READER *reader, double *value)
{
	uint64_t bits ;
	SRC_ERROR error ;

	if ((error = src_cp_read_u64 (reader, &bits)) != 0)
		return error ;

	memcpy (value, &bits, sizeof (*value)) ;
	return SRC_ERR_NO_ERROR ;
} /* src_cp_read_f64 */

LIBSAMPLERATE_DLL_PRIVATE SRC_ERROR
src_cp_read_bytes (SRC_CP_READER *reader, void *data, size_t bytes)
{
	return cp_get (reader, data, bytes) ;
} /* src_cp_read_bytes */

/*----------------------------------------------------------------------------------------
** Public API.
*/

LIBSAMPLERATE_DLL_PRIVATE SRC_ERROR
src_state_copy_pending (SRC_STATE *to, const SRC_STATE *from)
{
	size_t sample_bytes ;

	to->pending_data = NULL ;
	to->pending_frames = 0 ;
	to->saved_data = NULL ;
	to->saved_frames = 0 ;

	if (from->mode != SRC_MODE_CALLBACK || from->pending_frames <= 0)
		return SRC_ERR_NO_ERROR ;

	sample_bytes = (size_t) from->pending_frames * (size_t) from->channels * sizeof (float) ;

	to->pending_data = (float *) malloc (sample_bytes) ;
	if (to->pending_data == NULL)
		return SRC_ERR_MALLOC_FAILED ;

	memcpy (to->pending_data, from->pending_data, sample_bytes) ;
	to->pending_frames = from->pending_frames ;
	to->saved_data = to->pending_data ;
	to->saved_frames = to->pending_frames ;

	return SRC_ERR_NO_ERROR ;
} /* src_state_copy_pending */

static int
cp_state_supported (SRC_STATE *state)
{
	return state != NULL && state->vt != NULL && state->vt->checkpoint_size != NULL ;
} /* cp_state_supported */

static int
cp_converter_enabled (int converter)
{
	switch (converter)
	{
#ifdef ENABLE_SINC_BEST_CONVERTER
		case SRC_SINC_BEST_QUALITY :
			return 1 ;
#endif
#ifdef ENABLE_SINC_MEDIUM_CONVERTER
		case SRC_SINC_MEDIUM_QUALITY :
			return 1 ;
#endif
#ifdef ENABLE_SINC_FAST_CONVERTER
		case SRC_SINC_FASTEST :
			return 1 ;
#endif
		default :
			return 0 ;
	} ;
} /* cp_converter_enabled */

static size_t
cp_total_size (SRC_STATE *state)
{
	size_t payload, total ;

	payload = state->vt->checkpoint_size (state) ;
	if (payload == 0)
		return 0 ;

	/* Header, optional callback remainder and the payload length prefix. */
	total = SRC_CP_HEADER_BYTES + 4 + payload ;
	if (state->mode == SRC_MODE_CALLBACK)
		total += (size_t) state->pending_frames * (size_t) state->channels * sizeof (float) ;

	return total ;
} /* cp_total_size */

size_t
src_checkpoint_size (SRC_STATE *state)
{
	if (!cp_state_supported (state))
		return 0 ;

	if (state->channels < 1 || state->pending_frames < 0)
		return 0 ;

	return cp_total_size (state) ;
} /* src_checkpoint_size */

int
src_checkpoint_write (SRC_STATE *state, void *buffer, size_t bytes)
{
	SRC_CP_WRITER writer ;
	SRC_ERROR error ;
	uint32_t pending_frames ;

	if (!cp_state_supported (state))
		return SRC_ERR_CHECKPOINT_UNSUPPORTED ;

	if (state->channels < 1 || state->pending_frames < 0)
		return SRC_ERR_BAD_INTERNAL_STATE ;

	if (buffer == NULL)
		return SRC_ERR_CHECKPOINT_TRUNCATED ;

	if (bytes < cp_total_size (state))
		return SRC_ERR_CHECKPOINT_TRUNCATED ;

	src_cp_writer_init (&writer, buffer, bytes) ;

	/* Header. */
	unsigned char magic [4] = { SRC_CP_MAGIC_0, SRC_CP_MAGIC_1, SRC_CP_MAGIC_2, SRC_CP_MAGIC_3 } ;
	if ((error = src_cp_write_bytes (&writer, magic, sizeof (magic))) != 0)
		return error ;
	if ((error = src_cp_write_u16 (&writer, SRC_CP_VERSION)) != 0)
		return error ;
	if ((error = src_cp_write_u16 (&writer, (uint16_t) state->converter_type)) != 0)
		return error ;
	if ((error = src_cp_write_u16 (&writer, (uint16_t) state->channels)) != 0)
		return error ;
	if ((error = src_cp_write_u16 (&writer, (uint16_t) state->mode)) != 0)
		return error ;
	if ((error = src_cp_write_f64 (&writer, state->last_ratio)) != 0)
		return error ;
	if ((error = src_cp_write_f64 (&writer, state->last_position)) != 0)
		return error ;

	/* Callback remainder. Process mode states never hold any. */
	if (state->mode == SRC_MODE_PROCESS)
		pending_frames = 0 ;
	else
		pending_frames = (uint32_t) state->pending_frames ;

	if ((error = src_cp_write_u32 (&writer, pending_frames)) != 0)
		return error ;

	if (pending_frames > 0)
	{
		if (state->pending_data == NULL)
			return SRC_ERR_BAD_INTERNAL_STATE ;

		error = src_cp_write_bytes (&writer, state->pending_data,
						(size_t) pending_frames * (size_t) state->channels * sizeof (float)) ;
		if (error != 0)
			return error ;
	} ;

	/* Converter specific payload, length prefixed. */
	size_t payload_bytes = state->vt->checkpoint_size (state) ;
	if (payload_bytes == 0)
		return SRC_ERR_CHECKPOINT_UNSUPPORTED ;

	if ((error = src_cp_write_u32 (&writer, (uint32_t) payload_bytes)) != 0)
		return error ;
	if ((error = state->vt->checkpoint_write (state, &writer)) != 0)
		return error ;

	if (writer.offset != cp_total_size (state))
		return SRC_ERR_BAD_INTERNAL_STATE ;

	return SRC_ERR_NO_ERROR ;
} /* src_checkpoint_write */

static SRC_ERROR
cp_validate_ratio (double ratio, int allow_zero)
{
	/* Reject NaN and infinity; serialised state must stay plain IEEE values. */
	if (ratio != ratio || ratio - ratio != 0.0)
		return SRC_ERR_CHECKPOINT_BAD_FORMAT ;

	if (allow_zero && ratio == 0.0)
		return SRC_ERR_NO_ERROR ;

	if (is_bad_src_ratio (ratio))
		return SRC_ERR_CHECKPOINT_BAD_FORMAT ;

	return SRC_ERR_NO_ERROR ;
} /* cp_validate_ratio */

SRC_STATE *
src_checkpoint_read (const void *buffer, size_t bytes,
			src_callback_t callback, void *cb_data, int *error)
{
	SRC_CP_READER reader ;
	SRC_STATE *state = NULL ;
	SRC_ERROR err ;
	unsigned char magic [4] ;
	uint16_t version, converter, channels, mode ;
	uint32_t pending_frames, payload_bytes ;
	double last_ratio, last_position ;

	if (error != NULL)
		*error = SRC_ERR_NO_ERROR ;

	if (buffer == NULL)
	{
		if (error != NULL)
			*error = SRC_ERR_CHECKPOINT_TRUNCATED ;
		return NULL ;
	} ;

	src_cp_reader_init (&reader, buffer, bytes) ;

	if ((err = src_cp_read_bytes (&reader, magic, sizeof (magic))) != 0)
		goto fail ;

	unsigned char want_magic [4] = { SRC_CP_MAGIC_0, SRC_CP_MAGIC_1, SRC_CP_MAGIC_2, SRC_CP_MAGIC_3 } ;
	if (memcmp (magic, want_magic, sizeof (magic)) != 0)
	{	err = SRC_ERR_CHECKPOINT_BAD_FORMAT ;
		goto fail ;
		} ;

	if ((err = src_cp_read_u16 (&reader, &version)) != 0)
		goto fail ;
	if (version != SRC_CP_VERSION)
	{	err = SRC_ERR_CHECKPOINT_BAD_VERSION ;
		goto fail ;
		} ;

	if ((err = src_cp_read_u16 (&reader, &converter)) != 0)
		goto fail ;
	if ((err = src_cp_read_u16 (&reader, &channels)) != 0)
		goto fail ;
	if ((err = src_cp_read_u16 (&reader, &mode)) != 0)
		goto fail ;

	/* Field self-consistency: only the three sinc converters are valid and
	** each has to be enabled in this build.
	*/
	if (!cp_converter_enabled ((int) converter))
	{	err = SRC_ERR_CHECKPOINT_BAD_FORMAT ;
		goto fail ;
		} ;

	if (channels < 1)
	{	err = SRC_ERR_CHECKPOINT_BAD_FORMAT ;
		goto fail ;
		} ;

	if (mode != SRC_MODE_PROCESS && mode != SRC_MODE_CALLBACK)
	{	err = SRC_ERR_CHECKPOINT_BAD_FORMAT ;
		goto fail ;
		} ;

	/* The callback argument has to match the serialised mode exactly. */
	if (mode == SRC_MODE_PROCESS && callback != NULL)
	{	err = SRC_ERR_BAD_CALLBACK ;
		goto fail ;
		} ;
	if (mode == SRC_MODE_CALLBACK && callback == NULL)
	{	err = SRC_ERR_NULL_CALLBACK ;
		goto fail ;
		} ;

	if ((err = src_cp_read_f64 (&reader, &last_ratio)) != 0)
		goto fail ;
	if ((err = src_cp_read_f64 (&reader, &last_position)) != 0)
		goto fail ;

	if ((err = cp_validate_ratio (last_ratio, 1)) != 0)
		goto fail ;
	if (last_position != last_position || last_position < 0.0 || last_position >= 1.0)
	{	err = SRC_ERR_CHECKPOINT_BAD_FORMAT ;
		goto fail ;
		} ;

	if ((err = src_cp_read_u32 (&reader, &pending_frames)) != 0)
		goto fail ;
	if (mode == SRC_MODE_PROCESS && pending_frames != 0)
	{	err = SRC_ERR_CHECKPOINT_BAD_FORMAT ;
		goto fail ;
		} ;

	/* Guard against frame counts that cannot be represented by size_t math
	** on this platform or that contradict the remaining buffer length.
	*/
	if (pending_frames > (uint32_t) (bytes / sizeof (float)) / channels)
	{	err = SRC_ERR_CHECKPOINT_BAD_FORMAT ;
		goto fail ;
		} ;

	state = sinc_state_new ((int) converter, (int) channels, &err) ;
	if (state == NULL)
		goto fail ;

	if (pending_frames > 0)
	{
		size_t sample_bytes = (size_t) pending_frames * (size_t) channels * sizeof (float) ;

		state->pending_data = (float *) malloc (sample_bytes) ;
		if (state->pending_data == NULL)
		{	err = SRC_ERR_MALLOC_FAILED ;
			goto fail ;
			} ;

		if ((err = src_cp_read_bytes (&reader, state->pending_data, sample_bytes)) != 0)
			goto fail ;

		state->pending_frames = (long) pending_frames ;
		state->saved_data = state->pending_data ;
		state->saved_frames = state->pending_frames ;
	} ;

	if ((err = src_cp_read_u32 (&reader, &payload_bytes)) != 0)
		goto fail ;

	/* The payload must end exactly at the end of the supplied buffer. */
	if ((size_t) payload_bytes != bytes - reader.offset)
	{	err = SRC_ERR_CHECKPOINT_BAD_FORMAT ;
		goto fail ;
		} ;

	if ((err = state->vt->checkpoint_read (state, &reader)) != 0)
		goto fail ;

	if (reader.offset != reader.length)
	{	err = SRC_ERR_CHECKPOINT_BAD_FORMAT ;
		goto fail ;
		} ;

	state->converter_type = (int) converter ;
	state->last_ratio = last_ratio ;
	state->last_position = last_position ;

	if (mode == SRC_MODE_CALLBACK)
	{
		state->mode = SRC_MODE_CALLBACK ;
		state->callback_func = callback ;
		state->user_callback_data = cb_data ;
	} ;

	return state ;

fail:
	if (state != NULL)
		src_delete (state) ;
	if (error != NULL)
		*error = (int) err ;
	return NULL ;
} /* src_checkpoint_read */
