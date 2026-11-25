/*
** Copyright (C) 2011-2016 Erik de Castro Lopo <erikd@mega-nerd.com>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU Lesser General Public License as published by
** the Free Software Foundation; either version 2.1 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU Lesser General Public License for more details.
**
** You should have received a copy of the GNU Lesser General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

#include	"sfconfig.h"

#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>
#include	<math.h>
#include	<errno.h>

#include	"sndfile.h"
#include	"sfendian.h"
#include	"common.h"
#include	"ALAC/alac_codec.h"
#include	"ALAC/ALACBitUtilities.h"

#define		ALAC_MAX_FRAME_SIZE		8192
#define		ALAC_BYTE_BUFFER_SIZE	0x20000
#define		ALAC_MAX_CHANNEL_COUNT	8	// Same as kALACMaxChannels in /ALACAudioTypes.h

typedef struct
{	uint32_t	byte_size ;
	uint32_t	frame_count ;	/* Per packet declared duration ; 0 for fixed rate tables. */
	uint64_t	byte_start ;	/* Byte interval start, relative to the data payload. */
	uint64_t	frame_start ;	/* Sample interval start on the common zero based axis. */
} PAKT_PACKET ;

typedef struct
{	uint32_t	current, count, allocated ;
	int			variable ;
	sf_count_t	total_frames ;
	PAKT_PACKET	packets [] ;
} PAKT_INFO ;

typedef struct
{	sf_count_t	input_data_pos ;

	PAKT_INFO	* pakt_info ;

	int			channels, final_write_block ;

	uint32_t	frames_this_block, partial_block_frames, frames_per_block ;
	uint32_t	bits_per_sample, kuki_size ;

	/* Latched once a variable packet fails its output frame count check. */
	int			read_error ;


	/* Can't have a decoder and an encoder at the same time so stick
	** them in a union.
	*/
	union
	{	ALAC_DECODER decoder ;
		ALAC_ENCODER encoder ;
	} u ;

	char enctmpname [512] ;
	FILE *enctmp ;

	uint8_t	byte_buffer [ALAC_MAX_CHANNEL_COUNT * ALAC_BYTE_BUFFER_SIZE] ;

	int	buffer	[] ;

} ALAC_PRIVATE ;

/*============================================================================================
*/

static int alac_reader_init (SF_PRIVATE *psf, const ALAC_DECODER_INFO * info) ;
static int alac_writer_init (SF_PRIVATE *psf) ;

static sf_count_t alac_reader_calc_frames (SF_PRIVATE *psf, ALAC_PRIVATE *plac) ;

static sf_count_t alac_read_s (SF_PRIVATE *psf, short *ptr, sf_count_t len) ;
static sf_count_t alac_read_i (SF_PRIVATE *psf, int *ptr, sf_count_t len) ;
static sf_count_t alac_read_f (SF_PRIVATE *psf, float *ptr, sf_count_t len) ;
static sf_count_t alac_read_d (SF_PRIVATE *psf, double *ptr, sf_count_t len) ;

static sf_count_t alac_write_s (SF_PRIVATE *psf, const short *ptr, sf_count_t len) ;
static sf_count_t alac_write_i (SF_PRIVATE *psf, const int *ptr, sf_count_t len) ;
static sf_count_t alac_write_f (SF_PRIVATE *psf, const float *ptr, sf_count_t len) ;
static sf_count_t alac_write_d (SF_PRIVATE *psf, const double *ptr, sf_count_t len) ;

static sf_count_t	alac_seek	(SF_PRIVATE *psf, int mode, sf_count_t offset) ;

static int	alac_close		(SF_PRIVATE *psf) ;
static int	alac_byterate	(SF_PRIVATE *psf) ;

static int alac_decode_block (SF_PRIVATE *psf, ALAC_PRIVATE *plac) ;
static int alac_encode_block (ALAC_PRIVATE *plac) ;

static uint32_t alac_kuki_read (SF_PRIVATE * psf, uint32_t kuki_offset, uint8_t * kuki, size_t kuki_maxlen) ;

static PAKT_INFO * alac_pakt_alloc (uint32_t initial_count) ;
static PAKT_INFO * alac_pakt_read_decode (SF_PRIVATE * psf, uint32_t pakt_offset) ;
static PAKT_INFO * alac_pakt_read_variable (SF_PRIVATE * psf, const ALAC_DECODER_INFO * info, uint32_t frame_cap) ;
static PAKT_INFO * alac_pakt_append (PAKT_INFO * info, uint32_t value) ;
static uint8_t * alac_pakt_encode (const SF_PRIVATE *psf, uint32_t * pakt_size) ;
static sf_count_t alac_pakt_block_offset (const PAKT_INFO *info, uint32_t block) ;
static sf_count_t alac_seek_variable (SF_PRIVATE *psf, ALAC_PRIVATE *plac, sf_count_t offset) ;

static const char * alac_error_string (int error) ;

/*============================================================================================
** ALAC Reader initialisation function.
*/

int
alac_init (SF_PRIVATE *psf, const ALAC_DECODER_INFO * info)
{	int error ;

	if ((psf->codec_data = calloc (1, sizeof (ALAC_PRIVATE) + psf->sf.channels * sizeof (int) * ALAC_MAX_FRAME_SIZE)) == NULL)
		return SFE_MALLOC_FAILED ;

	psf->codec_close = alac_close ;

	switch (psf->file.mode)
	{	case SFM_RDWR :
			return SFE_BAD_MODE_RW ;

		case SFM_READ :
			if ((error = alac_reader_init (psf, info)))
				return error ;
			break ;

		case SFM_WRITE :
			if ((error = alac_writer_init (psf)))
				return error ;
			break ;

		default :
			psf_log_printf (psf, "%s : Bad psf->file.mode.\n", __func__) ;
			return SFE_INTERNAL ;
		} ;

	psf->byterate = alac_byterate ;

	return 0 ;
} /* aiff_alac_init */

void
alac_get_desc_chunk_items (int subformat, uint32_t *fmt_flags, uint32_t *frames_per_packet)
{	switch (subformat)
	{	case SF_FORMAT_ALAC_16 :
			*fmt_flags = 1 ;
			break ;
		case SF_FORMAT_ALAC_20 :
			*fmt_flags = 2 ;
			break ;
		case SF_FORMAT_ALAC_24 :
			*fmt_flags = 3 ;
			break ;
		case SF_FORMAT_ALAC_32 :
			*fmt_flags = 4 ;
			break ;
		default :
			break ;
		} ;
	*frames_per_packet = ALAC_FRAME_LENGTH ;
} /* alac_get_desc_chunk_items */

static int
alac_close	(SF_PRIVATE *psf)
{	ALAC_PRIVATE *plac ;
	BUF_UNION	ubuf ;

	plac = psf->codec_data ;

	if (psf->file.mode == SFM_WRITE)
	{	ALAC_ENCODER *penc = &plac->u.encoder ;
		SF_CHUNK_INFO chunk_info ;
		sf_count_t readcount ;
		uint8_t kuki_data [1024] ;
		uint32_t pakt_size = 0, saved_partial_block_frames ;

		plac->final_write_block = 1 ;
		saved_partial_block_frames = plac->partial_block_frames ;

		/*	If a block has been partially assembled, write it out as the final block. */
		if (plac->partial_block_frames && plac->partial_block_frames < plac->frames_per_block)
			alac_encode_block (plac) ;

		plac->partial_block_frames = saved_partial_block_frames ;

		alac_get_magic_cookie (penc, kuki_data, &plac->kuki_size) ;

		memset (&chunk_info, 0, sizeof (chunk_info)) ;
		chunk_info.id_size = snprintf (chunk_info.id, sizeof (chunk_info.id), "kuki") ;
		chunk_info.data = kuki_data ;
		chunk_info.datalen = plac->kuki_size ;
		psf_save_write_chunk (&psf->wchunks, &chunk_info) ;

		memset (&chunk_info, 0, sizeof (chunk_info)) ;
		chunk_info.id_size = snprintf (chunk_info.id, sizeof (chunk_info.id), "pakt") ;
		chunk_info.data = alac_pakt_encode (psf, &pakt_size) ;
		chunk_info.datalen = pakt_size ;
		psf_save_write_chunk (&psf->wchunks, &chunk_info) ;

		free (chunk_info.data) ;
		chunk_info.data = NULL ;

		psf->write_header (psf, 1) ;

		if (plac->enctmp != NULL)
		{	fseek (plac->enctmp, 0, SEEK_SET) ;

			while ((readcount = fread (ubuf.ucbuf, 1, sizeof (ubuf.ucbuf), plac->enctmp)) > 0)
				psf_fwrite (ubuf.ucbuf, 1, readcount, psf) ;
			fclose (plac->enctmp) ;
			remove (plac->enctmpname) ;
			} ;
		} ;

	if (plac->pakt_info)
		free (plac->pakt_info) ;
	plac->pakt_info = NULL ;

	return 0 ;
} /* alac_close */

static int
alac_byterate	(SF_PRIVATE *psf)
{
	if (psf->file.mode == SFM_READ && psf->sf.frames > 0)
		return (psf->datalength * psf->sf.samplerate) / psf->sf.frames ;

	return -1 ;
} /* alac_byterate */

/*============================================================================================
** ALAC initialisation Functions.
*/

static int
alac_reader_init (SF_PRIVATE *psf, const ALAC_DECODER_INFO * info)
{	ALAC_PRIVATE	*plac ;
	uint32_t		kuki_size ;
	int				error = ALAC_noErr, variable ;
	union			{ uint8_t kuki [512] ; uint32_t alignment ; } u ;

	if (info == NULL)
	{	psf_log_printf (psf, "%s : ALAC_DECODER_INFO is NULL.\n", __func__) ;
		return SFE_INTERNAL ;
		} ;

	/*
	** A CAF desc chunk with both bytes-per-packet and frames-per-packet set
	** to zero denotes a variable packet duration table in the pakt chunk,
	** where every packet is a (byte size, frame count) pair of vints.
	*/
	variable = info->frames_per_packet == 0 && info->bytes_per_packet == 0 ;

	if (variable && psf->sf.seekable != SF_TRUE)
	{	psf_log_printf (psf, "%s : variable packet duration CAF/ALAC needs a seekable stream.\n", __func__) ;
		return SFE_NOT_SEEKABLE ;
		} ;

	if (! variable && info->frames_per_packet > ALAC_FRAME_LENGTH)
	{	psf_log_printf (psf, "*** Error : frames_per_packet (%u) is too big. ***\n", info->frames_per_packet) ;
		return SFE_INTERNAL ;
		} ;

	plac = psf->codec_data ;

	plac->channels			= psf->sf.channels ;
	plac->frames_per_block	= variable ? ALAC_FRAME_LENGTH : info->frames_per_packet ;
	plac->bits_per_sample	= info->bits_per_sample ;

	if (! variable)
	{	if (plac->pakt_info != NULL)
			free (plac->pakt_info) ;
		plac->pakt_info = alac_pakt_read_decode (psf, info->pakt_offset) ;

		if (plac->pakt_info == NULL)
		{	psf_log_printf (psf, "%s : alac_pkt_read() returns NULL.\n", __func__) ;
			return SFE_INTERNAL ;
			} ;
		} ;

	/* Read in the ALAC cookie data and pass it to the init function. */
	kuki_size = alac_kuki_read (psf, info->kuki_offset, u.kuki, sizeof (u.kuki)) ;

	if (kuki_size == 0 ||
		(error = alac_decoder_init (&plac->u.decoder, u.kuki, kuki_size)) != ALAC_noErr)
	{	if (error == ALAC_noErr)
		{	psf_log_printf (psf, "*** %s : missing or empty 'kuki' chunk. ***\n", __func__) ;
			return SFE_MALFORMED_FILE ;
			} ;
		psf_log_printf (psf, "*** alac_decoder_init() returned %s. ***\n", alac_error_string (error)) ;
		return SFE_INTERNAL ;
		} ;


	if (plac->u.decoder.mNumChannels != (unsigned) psf->sf.channels)
	{	psf_log_printf (psf, "*** Initialized decoder has %u channels, but it should be %d. ***\n", plac->u.decoder.mNumChannels, psf->sf.channels) ;
		return SFE_INTERNAL ;
		} ;

	switch (info->bits_per_sample)
	{	case 16 :
		case 20 :
		case 24 :
		case 32 :
			psf->read_short		= alac_read_s ;
			psf->read_int		= alac_read_i ;
			psf->read_float		= alac_read_f ;
			psf->read_double	= alac_read_d ;
			break ;

		default :
			printf ("%s : info->bits_per_sample %u\n", __func__, info->bits_per_sample) ;
			return SFE_UNSUPPORTED_ENCODING ;
		} ;

	psf->codec_close	= alac_close ;
	psf->seek			= alac_seek ;

	if (variable)
	{	uint32_t frame_cap = plac->u.decoder.mConfig.frameLength ;

		if (frame_cap == 0 || frame_cap > ALAC_FRAME_LENGTH)
			frame_cap = ALAC_FRAME_LENGTH ;

		if (plac->pakt_info != NULL)
			free (plac->pakt_info) ;
		plac->pakt_info = alac_pakt_read_variable (psf, info, frame_cap) ;

		if (plac->pakt_info == NULL)
			return psf->error != SFE_NO_ERROR ? psf->error : SFE_MALFORMED_FILE ;

		/* Metadata only : the pakt table already gives the exact duration. */
		psf->sf.frames = plac->pakt_info->total_frames ;
		alac_seek (psf, SFM_READ, 0) ;
		return 0 ;
		} ;

	psf->sf.frames		= alac_reader_calc_frames (psf, plac) ;
	alac_seek (psf, SFM_READ, 0) ;

	return 0 ;
} /* alac_reader_init */

static int
alac_writer_init (SF_PRIVATE *psf)
{	ALAC_PRIVATE	*plac ;
	uint32_t		alac_format_flags = 0 ;

	plac = psf->codec_data ;

	if (psf->file.mode != SFM_WRITE)
		return SFE_BAD_MODE_RW ;

	plac->channels	= psf->sf.channels ;
	plac->kuki_size = alac_get_magic_cookie_size (psf->sf.channels) ;

	psf->write_short	= alac_write_s ;
	psf->write_int		= alac_write_i ;
	psf->write_float	= alac_write_f ;
	psf->write_double	= alac_write_d ;

	switch (SF_CODEC (psf->sf.format))
	{	case SF_FORMAT_ALAC_16 :
			alac_format_flags	= 1 ;
			plac->bits_per_sample = 16 ;
			break ;

		case SF_FORMAT_ALAC_20 :
			alac_format_flags	= 2 ;
			plac->bits_per_sample = 20 ;
			break ;

		case SF_FORMAT_ALAC_24 :
			alac_format_flags	= 3 ;
			plac->bits_per_sample = 24 ;
			break ;

		case SF_FORMAT_ALAC_32 :
			alac_format_flags	= 4 ;
			plac->bits_per_sample = 32 ;
			break ;

		default :
			psf_log_printf (psf, "%s : Can't figure out bits per sample.\n", __func__) ;
			return SFE_UNIMPLEMENTED ;
		} ;

	plac->frames_per_block = ALAC_FRAME_LENGTH ;

	plac->pakt_info = alac_pakt_alloc (2000) ;

	if ((plac->enctmp = psf_open_tmpfile (plac->enctmpname, sizeof (plac->enctmpname))) == NULL)
	{	psf_log_printf (psf, "Error : Failed to open temp file '%s' : \n", plac->enctmpname, strerror (errno)) ;
		return SFE_ALAC_FAIL_TMPFILE ;
		} ;

	alac_encoder_init (&plac->u.encoder, psf->sf.samplerate, psf->sf.channels, alac_format_flags, ALAC_FRAME_LENGTH) ;

	return 0 ;
} /* alac_writer_init */

/*============================================================================================
** ALAC block decoder and encoder.
*/

static inline uint32_t
alac_reader_next_packet_size (PAKT_INFO * info)
{	if (info->current >= info->count)
		return 0 ;
	return info->packets [info->current++].byte_size ;
} /* alac_reader_next_packet_size */

static sf_count_t
alac_reader_calc_frames (SF_PRIVATE *psf, ALAC_PRIVATE *plac)
{	sf_count_t	frames = 0 ;
	uint32_t	current_pos = 1, blocks = 0 ;

	plac->pakt_info->current = 0 ;

	while (current_pos < psf->filelength && current_pos > 0)
	{	current_pos = alac_reader_next_packet_size (plac->pakt_info) ;
		blocks = current_pos > 0 ? blocks + 1 : blocks ;
		} ;

	if (blocks == 0)
		return 0 ;

	/* Only count full blocks. */
	frames = plac->frames_per_block * (blocks - 1) ;

	alac_seek (psf, SFM_READ, frames) ;
	alac_decode_block (psf, plac) ;
	frames += plac->frames_this_block ;

	plac->pakt_info->current = 0 ;

	return frames ;
} /* alac_reader_calc_frames */

static int
alac_decode_block (SF_PRIVATE *psf, ALAC_PRIVATE *plac)
{	ALAC_DECODER *pdec = &plac->u.decoder ;
	PAKT_INFO	*pakt = plac->pakt_info ;
	uint32_t	packet_size, packet_index = 0 ;
	uint64_t	packet_start = 0 ;
	BitBuffer	bit_buffer ;
	int32_t		error ;

	if (pakt->variable)
	{	if (pakt->current >= pakt->count)
			return 0 ;
		packet_index = pakt->current ;
		packet_size = pakt->packets [packet_index].byte_size ;
		packet_start = pakt->packets [packet_index].byte_start ;
		pakt->current ++ ;
		}
	else
		{
		packet_size = alac_reader_next_packet_size (pakt) ;
		} ;

	if (packet_size == 0)
	{	if (pakt->current < pakt->count)
			psf_log_printf (psf, "packet_size is 0 (%d of %d)\n", pakt->current, pakt->count) ;
		return 0 ;
		} ;

	if (pakt->variable)
	{	plac->input_data_pos = psf->dataoffset + (sf_count_t) packet_start ;
		plac->read_error = 0 ;
		} ;

	psf_fseek (psf, plac->input_data_pos, SEEK_SET) ;

	if (packet_size > sizeof (plac->byte_buffer))
	{	psf_log_printf (psf, "%s : bad packet_size (%u)\n", __func__, packet_size) ;
		if (pakt->variable)
		{	psf->error = SFE_MALFORMED_FILE ;
			plac->read_error = 1 ;
			} ;
		return 0 ;
		} ;

	if ((packet_size != psf_fread (plac->byte_buffer, 1, packet_size, psf)))
	{	if (pakt->variable)
		{	psf_log_printf (psf, "%s : short read of packet %u\n", __func__, packet_index) ;
			psf->error = SFE_MALFORMED_FILE ;
			plac->read_error = 1 ;
			} ;
		return 0 ;
		} ;

	BitBufferInit (&bit_buffer, plac->byte_buffer, packet_size) ;

	plac->input_data_pos += packet_size ;
	plac->frames_this_block = 0 ;
	if ((error = alac_decode (pdec, &bit_buffer, plac->buffer, plac->frames_per_block, &plac->frames_this_block)) != ALAC_noErr)
	{	psf_log_printf (psf, "*** alac_decode() returned %s. ***\n", alac_error_string (error)) ;
		plac->frames_this_block = 0 ;
		if (pakt->variable)
		{	/* Withhold this packet and make the failure observable. */
			psf->error = SFE_MALFORMED_FILE ;
			plac->read_error = 1 ;
			} ;
		return 0 ;
		} ;

	if (pakt->variable && plac->frames_this_block != pakt->packets [packet_index].frame_count)
	{	psf_log_printf (psf, "*** packet %u declares %u frames but decoder produced %u. ***\n",
				packet_index, pakt->packets [packet_index].frame_count, plac->frames_this_block) ;
		/* Do not publish the mismatching packet's PCM. */
		plac->frames_this_block = 0 ;
		psf->error = SFE_MALFORMED_FILE ;
		plac->read_error = 1 ;
		return 0 ;
		} ;

	plac->partial_block_frames = 0 ;

	return 1 ;
} /* alac_decode_block */


static int
alac_encode_block (ALAC_PRIVATE *plac)
{	ALAC_ENCODER *penc = &plac->u.encoder ;
	PAKT_INFO *info ;
	uint32_t num_bytes = 0 ;

	alac_encode (penc, plac->partial_block_frames, plac->buffer, plac->byte_buffer, &num_bytes) ;

	if (fwrite (plac->byte_buffer, 1, num_bytes, plac->enctmp) != num_bytes)
		return 0 ;
	if ((info = alac_pakt_append (plac->pakt_info, num_bytes)) == NULL)
		return 0 ;
	plac->pakt_info = info ;

	plac->partial_block_frames = 0 ;

	return 1 ;
} /* alac_encode_block */

/*============================================================================================
** ALAC read functions.
*/

static sf_count_t
alac_read_s (SF_PRIVATE *psf, short *ptr, sf_count_t len)
{	ALAC_PRIVATE *plac ;
	int			*iptr ;
	int			k, readcount ;
	sf_count_t	total = 0 ;

	if ((plac = (ALAC_PRIVATE*) psf->codec_data) == NULL)
		return 0 ;

	if (plac->read_error)
	{	psf->error = SFE_MALFORMED_FILE ;
		return 0 ;
		} ;

	while (len > 0)
	{	if (plac->partial_block_frames >= plac->frames_this_block && alac_decode_block (psf, plac) == 0)
			break ;

		readcount = (plac->frames_this_block - plac->partial_block_frames) * plac->channels ;
		readcount = readcount > len ? (int) len : readcount ;

		iptr = plac->buffer + plac->partial_block_frames * plac->channels ;

		for (k = 0 ; k < readcount ; k++)
			ptr [total + k] = iptr [k] >> 16 ;

		plac->partial_block_frames += readcount / plac->channels ;
		total += readcount ;
		len -= readcount ;
		} ;

	return total ;
} /* alac_read_s */

static sf_count_t
alac_read_i (SF_PRIVATE *psf, int *ptr, sf_count_t len)
{	ALAC_PRIVATE *plac ;
	int			*iptr ;
	int			k, readcount ;
	sf_count_t	total = 0 ;

	if ((plac = (ALAC_PRIVATE*) psf->codec_data) == NULL)
		return 0 ;

	if (plac->read_error)
	{	psf->error = SFE_MALFORMED_FILE ;
		return 0 ;
		} ;

	while (len > 0)
	{	if (plac->partial_block_frames >= plac->frames_this_block && alac_decode_block (psf, plac) == 0)
			break ;

		readcount = (plac->frames_this_block - plac->partial_block_frames) * plac->channels ;
		readcount = readcount > len ? (int) len : readcount ;

		iptr = plac->buffer + plac->partial_block_frames * plac->channels ;

		for (k = 0 ; k < readcount ; k++)
			ptr [total + k] = iptr [k] ;

		plac->partial_block_frames += readcount / plac->channels ;
		total += readcount ;
		len -= readcount ;
		} ;

	return total ;
} /* alac_read_i */

static sf_count_t
alac_read_f (SF_PRIVATE *psf, float *ptr, sf_count_t len)
{	ALAC_PRIVATE *plac ;
	int			*iptr ;
	int			k, readcount ;
	sf_count_t	total = 0 ;
	float		normfact ;

	if ((plac = (ALAC_PRIVATE*) psf->codec_data) == NULL)
		return 0 ;

	normfact = (psf->norm_float == SF_TRUE) ? 1.0 / ((float) 0x80000000) : 1.0 ;

	if (plac->read_error)
	{	psf->error = SFE_MALFORMED_FILE ;
		return 0 ;
		} ;

	while (len > 0)
	{	if (plac->partial_block_frames >= plac->frames_this_block && alac_decode_block (psf, plac) == 0)
			break ;

		readcount = (plac->frames_this_block - plac->partial_block_frames) * plac->channels ;
		readcount = readcount > len ? (int) len : readcount ;

		iptr = plac->buffer + plac->partial_block_frames * plac->channels ;

		for (k = 0 ; k < readcount ; k++)
			ptr [total + k] = normfact * iptr [k] ;

		plac->partial_block_frames += readcount / plac->channels ;
		total += readcount ;
		len -= readcount ;
		} ;

	return total ;
} /* alac_read_f */

static sf_count_t
alac_read_d (SF_PRIVATE *psf, double *ptr, sf_count_t len)
{	ALAC_PRIVATE *plac ;
	int			*iptr ;
	int			k, readcount ;
	sf_count_t	total = 0 ;
	double		normfact ;

	if ((plac = (ALAC_PRIVATE*) psf->codec_data) == NULL)
		return 0 ;

	normfact = (psf->norm_double == SF_TRUE) ? 1.0 / ((float) 0x80000000) : 1.0 ;

	if (plac->read_error)
	{	psf->error = SFE_MALFORMED_FILE ;
		return 0 ;
		} ;

	while (len > 0)
	{	if (plac->partial_block_frames >= plac->frames_this_block && alac_decode_block (psf, plac) == 0)
			break ;

		readcount = (plac->frames_this_block - plac->partial_block_frames) * plac->channels ;
		readcount = readcount > len ? (int) len : readcount ;

		iptr = plac->buffer + plac->partial_block_frames * plac->channels ;

		for (k = 0 ; k < readcount ; k++)
			ptr [total + k] = normfact * iptr [k] ;

		plac->partial_block_frames += readcount / plac->channels ;
		total += readcount ;
		len -= readcount ;
		} ;

	return total ;
} /* alac_read_d */

/*============================================================================================
*/

static sf_count_t
alac_seek_variable (SF_PRIVATE *psf, ALAC_PRIVATE *plac, sf_count_t offset)
{	PAKT_INFO	*pakt = plac->pakt_info ;
	uint32_t	lo, hi, packet_index ;
	sf_count_t	sample_offset ;

	if (offset < 0 || offset > pakt->total_frames)
	{	psf->error = SFE_BAD_SEEK ;
		return PSF_SEEK_ERROR ;
		} ;

	/* A new seek gives a (possibly good) packet another chance. */
	plac->read_error = 0 ;

	if (offset == 0)
	{	psf_fseek (psf, psf->dataoffset, SEEK_SET) ;
		plac->frames_this_block = 0 ;
		plac->partial_block_frames = 0 ;
		plac->input_data_pos = psf->dataoffset ;
		pakt->current = 0 ;
		return 0 ;
		} ;

	if (offset == pakt->total_frames)
	{	/* The total frame count is a legal tail position. */
		psf_fseek (psf, psf->dataoffset + psf->datalength, SEEK_SET) ;
		plac->frames_this_block = 0 ;
		plac->partial_block_frames = 0 ;
		plac->input_data_pos = psf->dataoffset + psf->datalength ;
		pakt->current = pakt->count ;
		return offset ;
		} ;

	/* Find the last packet whose frame interval starts at or before offset. */
	lo = 0 ;
	hi = pakt->count ;
	while (lo < hi)
	{	uint32_t mid = lo + (hi - lo) / 2 ;
		if ((sf_count_t) pakt->packets [mid].frame_start <= offset)
			lo = mid + 1 ;
		else
			hi = mid ;
		} ;

	packet_index = lo - 1 ;
	sample_offset = offset - (sf_count_t) pakt->packets [packet_index].frame_start ;

	pakt->current = packet_index ;
	plac->input_data_pos = psf->dataoffset + (sf_count_t) pakt->packets [packet_index].byte_start ;

	if (alac_decode_block (psf, plac) == 0)
		return PSF_SEEK_ERROR ;

	plac->partial_block_frames = (uint32_t) sample_offset ;

	return offset ;
} /* alac_seek_variable */

static sf_count_t
alac_seek (SF_PRIVATE *psf, int mode, sf_count_t offset)
{	ALAC_PRIVATE *plac ;
	int			newblock, newsample ;

	if (! psf->codec_data)
		return 0 ;
	plac = (ALAC_PRIVATE*) psf->codec_data ;

	if (psf->datalength < 0 || psf->dataoffset < 0)
	{	psf->error = SFE_BAD_SEEK ;
		return PSF_SEEK_ERROR ;
		} ;

	if (plac->pakt_info != NULL && plac->pakt_info->variable)
	{	if (mode != SFM_READ)
		{	psf->error = SFE_BAD_SEEK ;
			return PSF_SEEK_ERROR ;
			} ;
		return alac_seek_variable (psf, plac, offset) ;
		} ;

	if (offset == 0)
	{	psf_fseek (psf, psf->dataoffset, SEEK_SET) ;

		plac->frames_this_block = 0 ;
		plac->input_data_pos = psf->dataoffset ;
		plac->pakt_info->current = 0 ;
		return 0 ;
		} ;

	if (offset < 0 || offset > plac->pakt_info->count * plac->frames_per_block)
	{	psf->error = SFE_BAD_SEEK ;
		return	PSF_SEEK_ERROR ;
		} ;

	newblock	= offset / plac->frames_per_block ;
	newsample	= offset % plac->frames_per_block ;

	if (mode == SFM_READ)
	{	plac->input_data_pos = psf->dataoffset + alac_pakt_block_offset (plac->pakt_info, newblock) ;

		plac->pakt_info->current = newblock ;
		alac_decode_block (psf, plac) ;
		plac->partial_block_frames = newsample ;
		}
	else
	{	/* What to do about write??? */
		psf->error = SFE_BAD_SEEK ;
		return	PSF_SEEK_ERROR ;
		} ;

	return newblock * plac->frames_per_block + newsample ;
} /* alac_seek */

/*==========================================================================================
** ALAC Write Functions.
*/

static sf_count_t
alac_write_s (SF_PRIVATE *psf, const short *ptr, sf_count_t len)
{	ALAC_PRIVATE *plac ;
	int			*iptr ;
	int			k, writecount ;
	sf_count_t	total = 0 ;

	if ((plac = (ALAC_PRIVATE*) psf->codec_data) == NULL)
		return 0 ;

	while (len > 0)
	{	writecount = (plac->frames_per_block - plac->partial_block_frames) * plac->channels ;
		writecount = (writecount == 0 || writecount > len) ? (int) len : writecount ;

		iptr = plac->buffer + plac->partial_block_frames * plac->channels ;

		for (k = 0 ; k < writecount ; k++)
			iptr [k] = arith_shift_left (ptr [k], 16) ;

		plac->partial_block_frames += writecount / plac->channels ;
		total += writecount ;
		len -= writecount ;
		ptr += writecount ;

		if (plac->partial_block_frames >= plac->frames_per_block)
			alac_encode_block (plac) ;
		} ;

	return total ;
} /* alac_write_s */

static sf_count_t
alac_write_i (SF_PRIVATE *psf, const int *ptr, sf_count_t len)
{	ALAC_PRIVATE *plac ;
	int			*iptr ;
	int			k, writecount ;
	sf_count_t	total = 0 ;

	if ((plac = (ALAC_PRIVATE*) psf->codec_data) == NULL)
		return 0 ;

	while (len > 0)
	{	writecount = (plac->frames_per_block - plac->partial_block_frames) * plac->channels ;
		writecount = (writecount == 0 || writecount > len) ? (int) len : writecount ;

		iptr = plac->buffer + plac->partial_block_frames * plac->channels ;

		for (k = 0 ; k < writecount ; k++)
			iptr [k] = ptr [k] ;

		plac->partial_block_frames += writecount / plac->channels ;
		total += writecount ;
		len -= writecount ;
		ptr += writecount ;

		if (plac->partial_block_frames >= plac->frames_per_block)
			alac_encode_block (plac) ;
		} ;

	return total ;
} /* alac_write_i */

static sf_count_t
alac_write_f (SF_PRIVATE *psf, const float *ptr, sf_count_t len)
{	ALAC_PRIVATE *plac ;
	void		(*convert) (const float *, int *t, int, int) ;
	int			*iptr ;
	int			writecount ;
	sf_count_t	total = 0 ;

	if ((plac = (ALAC_PRIVATE*) psf->codec_data) == NULL)
		return 0 ;

	convert = (psf->add_clipping) ? psf_f2i_clip_array : psf_f2i_array ;

	while (len > 0)
	{	writecount = (plac->frames_per_block - plac->partial_block_frames) * plac->channels ;
		writecount = (writecount == 0 || writecount > len) ? (int) len : writecount ;

		iptr = plac->buffer + plac->partial_block_frames * plac->channels ;

		convert (ptr, iptr, writecount, psf->norm_float) ;

		plac->partial_block_frames += writecount / plac->channels ;
		total += writecount ;
		len -= writecount ;
		ptr += writecount ;

		if (plac->partial_block_frames >= plac->frames_per_block)
			alac_encode_block (plac) ;
		} ;

	return total ;
} /* alac_write_f */

static sf_count_t
alac_write_d (SF_PRIVATE *psf, const double *ptr, sf_count_t len)
{	ALAC_PRIVATE *plac ;
	void		(*convert) (const double *, int *t, int, int) ;
	int			*iptr ;
	int			writecount ;
	sf_count_t	total = 0 ;

	if ((plac = (ALAC_PRIVATE*) psf->codec_data) == NULL)
		return 0 ;

	convert = (psf->add_clipping) ? psf_d2i_clip_array : psf_d2i_array ;

	while (len > 0)
	{	writecount = (plac->frames_per_block - plac->partial_block_frames) * plac->channels ;
		writecount = (writecount == 0 || writecount > len) ? (int) len : writecount ;

		iptr = plac->buffer + plac->partial_block_frames * plac->channels ;

		convert (ptr, iptr, writecount, psf->norm_float) ;

		plac->partial_block_frames += writecount / plac->channels ;
		total += writecount ;
		len -= writecount ;
		ptr += writecount ;

		if (plac->partial_block_frames >= plac->frames_per_block)
			alac_encode_block (plac) ;
		} ;

	return total ;
} /* alac_write_d */

/*==============================================================================
** PAKT_INFO handling.
*/

static PAKT_INFO *
alac_pakt_alloc (uint32_t initial_count)
{	PAKT_INFO * info ;

	if ((info = calloc (1, sizeof (PAKT_INFO) + initial_count * sizeof (info->packets [0]))) == NULL)
		return NULL ;

	info->allocated = initial_count ;
	info->current = 0 ;
	info->count = 0 ;

	return info ;
} /* alac_pakt_alloc */

static PAKT_INFO *
alac_pakt_append (PAKT_INFO * info, uint32_t value)
{
	if (info->count >= info->allocated)
	{	PAKT_INFO * temp ;
		uint32_t newcount = info->allocated + info->allocated / 2 ;

		if ((temp = realloc (info, sizeof (PAKT_INFO) + newcount * sizeof (info->packets [0]))) == NULL)
			return NULL ;

		info = temp ;
		info->allocated = newcount ;
		} ;

	info->packets [info->count].byte_size = value ;
	info->packets [info->count].frame_count = 0 ;
	info->packets [info->count].byte_start = 0 ;
	info->packets [info->count].frame_start = 0 ;
	info->count ++ ;
	return info ;
} /* alac_pakt_append */

static PAKT_INFO *
alac_pakt_read_decode (SF_PRIVATE * psf, uint32_t UNUSED (pakt_offset))
{	SF_CHUNK_INFO chunk_info ;
	PAKT_INFO * info = NULL, * temp ;
	uint8_t *pakt_data = NULL ;
	uint32_t bcount, value = 1, pakt_size ;
	SF_CHUNK_ITERATOR * chunk_iterator ;


	memset (&chunk_info, 0, sizeof (chunk_info)) ;
	snprintf (chunk_info.id, sizeof (chunk_info.id), "pakt") ;
	chunk_info.id_size = 4 ;

	if ((chunk_iterator = psf_get_chunk_iterator (psf, chunk_info.id)) == NULL)
	{	psf_log_printf (psf, "%s : no chunk iterator found\n", __func__) ;
		free (chunk_info.data) ;
		chunk_info.data = NULL ;
		return NULL ;
		} ;

	psf->get_chunk_size (psf, chunk_iterator, &chunk_info) ;

	pakt_size = chunk_info.datalen ;
	if (pakt_size > UINT32_MAX-5 )
		return NULL;
	chunk_info.data = pakt_data = malloc (pakt_size + 5) ;
	if (!chunk_info.data)
		return NULL ;

	if ((bcount = psf->get_chunk_data (psf, chunk_iterator, &chunk_info)) != SF_ERR_NO_ERROR)
	{	while (chunk_iterator)
			chunk_iterator = psf->next_chunk_iterator (psf, chunk_iterator) ;
		free (chunk_info.data) ;
		chunk_info.data = NULL ;
		return NULL ;
		} ;

	while (chunk_iterator)
		chunk_iterator = psf->next_chunk_iterator (psf, chunk_iterator) ;

	info = alac_pakt_alloc (pakt_size / 4) ;

	/* Start at 24 bytes in, skipping over the 'pakt' chunks header. */
	for (bcount = 24 ; bcount < pakt_size && value != 0 ; )
	{	uint8_t byte ;
		int32_t count = 0 ;

		value = 0 ;
		do
		{	byte = pakt_data [bcount + count] ;
			value = (value << 7) + (byte & 0x7F) ;

			count ++ ;
			if (count > 5 || bcount + count > pakt_size)
			{	printf ("%s %d : Ooops! count %" PRIi32 "    bcount %" PRIu32 "\n", __func__, __LINE__, count, bcount) ;
				value = 0 ;
				break ;
				} ;
			}
			while (byte & 0x80) ;

		bcount += count ;

		if ((temp = alac_pakt_append (info, value)) == NULL)
			goto FreeExit ;
		info = temp ;
		} ;

	free (pakt_data) ;

	return info ;

FreeExit :
	free (pakt_data) ;
	free (info) ;
	return NULL ;
} /* alac_pakt_read_decode */

/*
** Read one big endian, 7 bit continuation integer from a bounded buffer.
** Returns 1 on success, 0 on truncation and -1 on overflow.
*/
static int
alac_vint_read (const uint8_t * data, uint32_t data_len, uint32_t * position, uint32_t * value_out)
{	uint64_t	value = 0 ;
	uint32_t	pos = *position ;
	int			count = 0 ;
	uint8_t		byte ;

	do
	{	if (pos >= data_len)
			return 0 ;
		byte = data [pos ++] ;
		value = (value << 7) | (byte & 0x7f) ;
		count ++ ;
		if (count > 5 || value > UINT32_MAX)
			return -1 ;
		}
	while (byte & 0x80) ;

	*position = pos ;
	*value_out = (uint32_t) value ;
	return 1 ;
} /* alac_vint_read */

static PAKT_INFO *
alac_pakt_read_variable (SF_PRIVATE * psf, const ALAC_DECODER_INFO * info, uint32_t frame_cap)
{	SF_CHUNK_INFO chunk_info ;
	SF_CHUNK_ITERATOR * chunk_iterator ;
	PAKT_INFO	*pakt = NULL ;
	uint8_t		*pakt_data = NULL ;
	uint32_t	pakt_size, position, packet_index ;
	uint32_t	priming, remainder ;
	uint64_t	header_packets, header_frames ;
	uint64_t	sum_bytes = 0, sum_frames = 0 ;
	int			vint_result ;

	memset (&chunk_info, 0, sizeof (chunk_info)) ;
	snprintf (chunk_info.id, sizeof (chunk_info.id), "pakt") ;
	chunk_info.id_size = 4 ;

	if ((chunk_iterator = psf_get_chunk_iterator (psf, chunk_info.id)) == NULL)
	{	psf_log_printf (psf, "%s : no chunk iterator found\n", __func__) ;
		psf->error = SFE_MALFORMED_FILE ;
		return NULL ;
		} ;

	psf->get_chunk_size (psf, chunk_iterator, &chunk_info) ;
	pakt_size = chunk_info.datalen ;

	if (pakt_size < 26)
	{	psf_log_printf (psf, "*** variable 'pakt' chunk %u is too short for header plus a packet pair. ***\n", pakt_size) ;
		psf->error = SFE_MALFORMED_FILE ;
		return NULL ;
		} ;

	if ((pakt_data = malloc (pakt_size)) == NULL)
	{	psf->error = SFE_MALLOC_FAILED ;
		return NULL ;
		} ;

	chunk_info.data = pakt_data ;

	if (psf->get_chunk_data (psf, chunk_iterator, &chunk_info) != SF_ERR_NO_ERROR)
	{	while (chunk_iterator)
			chunk_iterator = psf->next_chunk_iterator (psf, chunk_iterator) ;
		free (pakt_data) ;
		psf->error = SFE_MALFORMED_FILE ;
		return NULL ;
		} ;

	while (chunk_iterator)
		chunk_iterator = psf->next_chunk_iterator (psf, chunk_iterator) ;

	header_packets = psf_get_be64 (pakt_data, 0) ;
	header_frames = psf_get_be64 (pakt_data, 8) ;
	priming = psf_get_be32 (pakt_data, 16) ;
	remainder = psf_get_be32 (pakt_data, 20) ;

	if ((uint64_t) info->packets != header_packets || (uint64_t) info->valid_frames != header_frames ||
			(uint32_t) info->priming_frames != priming || (uint32_t) info->remainder_frames != remainder)
	{	psf_log_printf (psf, "*** 'pakt' header disagrees with the CAF chunk data. ***\n") ;
		goto Malformed ;
		} ;

	if (priming != 0 || remainder != 0)
	{	psf_log_printf (psf, "*** variable packet tables with priming/remainder are not supported. ***\n") ;
		goto Malformed ;
		} ;

	/* Every packet needs at least two one byte vints. */
	if (header_packets == 0 || header_packets > (uint64_t) (pakt_size - 24) / 2 || header_packets > UINT32_MAX)
	{	psf_log_printf (psf, "*** 'pakt' packet count %" PRIu64 " does not fit the table of %u bytes. ***\n",
				header_packets, pakt_size - 24) ;
		goto Malformed ;
		} ;

	if (header_frames > SF_COUNT_MAX)
	{	psf_log_printf (psf, "*** 'pakt' valid frame count %" PRIu64 " is too large. ***\n", header_frames) ;
		goto Malformed ;
		} ;

	pakt = alac_pakt_alloc ((uint32_t) header_packets) ;
	if (pakt == NULL)
	{	free (pakt_data) ;
		psf->error = SFE_MALLOC_FAILED ;
		return NULL ;
		} ;

	pakt->variable = 1 ;
	position = 24 ;

	for (packet_index = 0 ; packet_index < header_packets ; packet_index ++)
	{	uint32_t byte_count = 0, frame_count = 0 ;

		vint_result = alac_vint_read (pakt_data, pakt_size, &position, &byte_count) ;
		if (vint_result != 1)
		{	psf_log_printf (psf, "*** 'pakt' byte size vint for packet %u is %s. ***\n",
					packet_index, vint_result == 0 ? "truncated" : "an overflowing integer") ;
			goto Malformed ;
			} ;

		vint_result = alac_vint_read (pakt_data, pakt_size, &position, &frame_count) ;
		if (vint_result != 1)
		{	psf_log_printf (psf, "*** 'pakt' frame count vint for packet %u is %s. ***\n",
					packet_index, vint_result == 0 ? "truncated" : "an overflowing integer") ;
			goto Malformed ;
			} ;

		if (byte_count == 0 || frame_count == 0)
		{	psf_log_printf (psf, "*** 'pakt' packet %u has a zero byte or frame length. ***\n", packet_index) ;
			goto Malformed ;
			} ;

		if (frame_count > frame_cap)
		{	psf_log_printf (psf, "*** 'pakt' packet %u declares %u frames, above the cookie maximum %u. ***\n",
					packet_index, frame_count, frame_cap) ;
			goto Malformed ;
			} ;

		if (sum_bytes > UINT64_MAX - byte_count || sum_frames > UINT64_MAX - frame_count)
		{	psf_log_printf (psf, "*** 'pakt' cumulative counters overflow. ***\n") ;
			goto Malformed ;
			} ;

		pakt->packets [packet_index].byte_size = byte_count ;
		pakt->packets [packet_index].frame_count = frame_count ;
		pakt->packets [packet_index].byte_start = sum_bytes ;
		pakt->packets [packet_index].frame_start = sum_frames ;
		pakt->count = packet_index + 1 ;

		sum_bytes += byte_count ;
		sum_frames += frame_count ;
		} ;

	/* The declared packet count must consume the whole table, no more and no less. */
	if (position != pakt_size)
	{	psf_log_printf (psf, "*** 'pakt' table has %u trailing bytes after %" PRIu64 " packets. ***\n",
				pakt_size - position, header_packets) ;
		goto Malformed ;
		} ;

	if (sum_frames != header_frames)
	{	psf_log_printf (psf, "*** 'pakt' cumulative frames %" PRIu64 " != valid frames %" PRIu64 ". ***\n",
				sum_frames, header_frames) ;
		goto Malformed ;
		} ;

	if (psf->datalength < 0 || sum_bytes != (uint64_t) psf->datalength)
	{	psf_log_printf (psf, "*** 'pakt' cumulative bytes %" PRIu64 " != data payload %D. ***\n",
				sum_bytes, psf->datalength) ;
		goto Malformed ;
		} ;

	pakt->total_frames = (sf_count_t) sum_frames ;

	free (pakt_data) ;
	return pakt ;

Malformed :
	free (pakt_data) ;
	if (pakt != NULL)
		free (pakt) ;
	psf->error = SFE_MALFORMED_FILE ;
	return NULL ;
} /* alac_pakt_read_variable */

static uint8_t *
alac_pakt_encode (const SF_PRIVATE *psf, uint32_t * pakt_size_out)
{	const ALAC_PRIVATE *plac ;
	const PAKT_INFO *info ;
	uint8_t	*data ;
	uint32_t k, allocated, pakt_size ;

	plac = psf->codec_data ;
	info = plac->pakt_info ;

	allocated = 100 + 4 * info->count ;
	if ((data = calloc (1, allocated)) == NULL)
		return NULL ;

	psf_put_be64 (data, 0, info->count) ;
	psf_put_be64 (data, 8, psf->sf.frames) ;
	psf_put_be32 (data, 20, kALACDefaultFramesPerPacket - plac->partial_block_frames) ;

	/* Real 'pakt' data starts after 24 byte header. */
	pakt_size = 24 ;

	for (k = 0 ; k < info->count ; k++)
	{	int32_t value = info->packets [k].byte_size ;

		if ((value & 0x7f) == value)
		{	data [pakt_size++] = value ;
			continue ;
			} ;

		if ((value & 0x3fff) == value)
		{	data [pakt_size++] = (value >> 7) | 0x80 ;
			data [pakt_size++] = value & 0x7f ;
			continue ;
			} ;

		if ((value & 0x1fffff) == value)
		{	data [pakt_size++] = (value >> 14) | 0x80 ;
			data [pakt_size++] = ((value >> 7) & 0x7f) | 0x80 ;
			data [pakt_size++] = value & 0x7f ;
			continue ;
		} ;

		if ((value & 0x0fffffff) == value)
		{	data [pakt_size++] = (value >> 21) | 0x80 ;
			data [pakt_size++] = ((value >> 14) & 0x7f) | 0x80 ;
			data [pakt_size++] = ((value >> 7) & 0x7f) | 0x80 ;
			data [pakt_size++] = value & 0x7f ;
			continue ;
			} ;

		*pakt_size_out = 0 ;
		free (data) ;
		return NULL ;
		} ;

	*pakt_size_out = pakt_size ;
	return data ;
} /* alac_pakt_encode */

static sf_count_t
alac_pakt_block_offset (const PAKT_INFO *info, uint32_t block)
{	sf_count_t offset = 0 ;
	uint32_t k ;

	for (k = 0 ; k < block ; k++)
		offset += info->packets [k].byte_size ;

	return offset ;
} /* alac_pakt_block_offset */

static uint32_t
alac_kuki_read (SF_PRIVATE * psf, uint32_t kuki_offset, uint8_t * kuki, size_t kuki_maxlen)
{	uint32_t marker ;
	uint64_t kuki_size ;

	if (psf_fseek (psf, kuki_offset, SEEK_SET) != kuki_offset)
		return 0 ;

	psf_fread (&marker, 1, sizeof (marker), psf) ;
	if (marker != MAKE_MARKER ('k', 'u', 'k', 'i'))
		return 0 ;

	psf_fread (&kuki_size, 1, sizeof (kuki_size), psf) ;
	kuki_size = BE2H_64 (kuki_size) ;

	if (kuki_size == 0 || kuki_size > kuki_maxlen)
	{	psf_log_printf (psf, "%s : Bad size (%D) of 'kuki' chunk.\n", __func__, kuki_size) ;
		return 0 ;
		} ;

	psf_fread (kuki, 1, kuki_size, psf) ;

	return kuki_size ;
} /* alac_kuki_read */

#define CASE_NAME(x)	case x : return #x ; break ;

static const char *
alac_error_string (int error)
{	static char errstr [128] ;
	switch (error)
	{	CASE_NAME (kALAC_UnimplementedError) ;
		CASE_NAME (kALAC_FileNotFoundError) ;
		CASE_NAME (kALAC_ParamError) ;
		CASE_NAME (kALAC_MemFullError) ;
		CASE_NAME (fALAC_FrameLengthError) ;

		/* Added for libsndfile */
		CASE_NAME (kALAC_BadBitWidth) ;
		CASE_NAME (kALAC_IncompatibleVersion) ;
		CASE_NAME (kALAC_BadSpecificConfigSize) ;
		CASE_NAME (kALAC_ZeroChannelCount) ;
		CASE_NAME (kALAC_NumSamplesTooBig) ;
		CASE_NAME (kALAC_UnsupportedElement) ;
		default :
			break ;
		} ;

	snprintf (errstr, sizeof (errstr), "Unknown error %d", error) ;
	return errstr ;
} /* alac_error_string */

