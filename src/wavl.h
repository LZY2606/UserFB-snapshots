/*
** Copyright (C) 2024
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

/*
** Read-only support for WAVE files whose audio is a single RIFF LIST
** chunk of type 'wavl', holding interleaved 'data' (IMA-ADPCM) and
** 'slnt' (silence/hold) subchunks. The expanded audio is presented as
** one continuous frame axis.
**
** Scope: seekable little-endian RIFF/WAVE, mono or stereo IMA-ADPCM,
** fmt chunk before the audio LIST, one audio LIST/wavl, no top level
** data chunk. Every non-empty data subchunk is a whole number of
** complete IMA blocks, each block header restarting the per channel
** predictor. RF64, pipes, nested wavl, other codecs and writing are not
** supported.
**
** The directory grows with the number of LIST entries; the decode
** workspaces grow only with the fixed block geometry. Held frames are
** synthesised on demand from the last compressed block of the nearest
** preceding data segment, so the I/O needed for a seek never grows with
** the recording length or the slnt count.
*/

#ifndef WAVL_H_INCLUDED
#define WAVL_H_INCLUDED

/*
** This header is force-included for every library translation unit in
** the task build. Only this marker macro is visible there; the full
** declarations below expand solely when common.h has already been
** included, so units such as the GSM codec stay untouched. The entity
** block has its own guard so a later explicit include still works.
*/
#define TASK_BUILD_WAVL 1

#endif /* WAVL_H_INCLUDED */

#ifdef SNDFILE_COMMON_H
#ifndef WAVL_TYPES_DEFINED
#define WAVL_TYPES_DEFINED 1

/* Shared WAV style IMA-ADPCM block decoder implemented in ima_adpcm.c.
** It decodes one block already present in the input buffer and never
** touches file IO or codec-private state.
*/
void	wavlike_ima_decode_block_data (const unsigned char *block, short *samples, int channels,
	int blocksize, int samplesperblock) ;

/* List entry kinds. */
enum
{	WAVL_SEG_DATA	= 1,
	WAVL_SEG_SLNT	= 2
} ;

typedef struct
{	/* WAVL_SEG_DATA: absolute file offset and byte length of the payload. */
	sf_count_t	offset ;
	sf_count_t	length ;

	/* WAVL_SEG_SLNT: number of held frames (always > 0 for stored entries). */
	sf_count_t	frames ;

	/* First logical frame covered by this entry and one past the last. */
	sf_count_t	start ;
	sf_count_t	end ;

	/* WAVL_SEG_DATA only: compressed block index range, file wide. */
	int			first_block ;
	int			last_block ;

	/* Index of the nearest preceding non-empty data segment, or -1. */
	int			prev_data ;

	int			kind ;
} WAVL_SEGMENT ;

typedef struct WAVL_PRIVATE_tag
{	WAVL_SEGMENT	*segs ;
	int				seg_count ;
	int				seg_cap ;

	/* Total number of compressed IMA blocks across all data entries. */
	int				block_count ;

	/* Block geometry copied from the fmt chunk. */
	int				blockalign ;
	int				samplesperblock ;
	int				channels ;

	/* Decodable frames in data entries, frames added by slnt entries. */
	sf_count_t		data_frames ;
	sf_count_t		slnt_frames ;

	/* Expanded logical length in frames. */
	sf_count_t		total_frames ;

	/* Absolute file offsets covered by the wavl LIST body. */
	sf_count_t		list_start ;
	sf_count_t		list_end ;

	/* Reusable one-block input buffer and decoded block workspace. */
	unsigned char	*block ;
	short			*samples ;

	/* Cache for the most recently decoded physical block. */
	int				cache_seg ;
	int				cache_phys ;
} WAVL_PRIVATE ;

/*
** Parse one LIST/wavl body. On entry the file position is right after
** the four byte 'wavl' form tag. On success *pwavl holds a new directory
** and the file position is at the first byte after the LIST body. Empty
** data and zero length slnt entries are consumed but add no segment and
** never reset the hold value. Returns an SFE_* error code on failure.
*/
int		wavl_parse_list (SF_PRIVATE *psf, WAVL_PRIVATE **pwavl, uint32_t list_chunk_size, sf_count_t form_tag_pos) ;

/*
** Validate the directory against the optional fact chunk and publish the
** expanded frame axis on the SF_PRIVATE handle.
*/
int		wavl_finalize (SF_PRIVATE *psf, WAVL_PRIVATE *wavl, uint32_t fact_frames, int have_fact) ;

/* Release the directory and its workspaces. */
void	wavl_free (WAVL_PRIVATE *wavl) ;

/* True when this handle is backed by a wavl directory. */
int		wavl_active (SF_PRIVATE *psf) ;

/*
** Install the wavl read/seek/close hooks on top of an initialised IMA
** codec. The ordinary IMA block decoder is reused block by block through
** wavl_fill_block(); no whole-segment PCM copy is ever materialised.
*/
int		wavl_codec_init (SF_PRIVATE *psf) ;

/*
** Decode logical block logical_block (frame range
** [logical_block * samplesperblock, ...)) into out, zero filling any
** part beyond the end of the expanded axis. A slnt frame is the last
** decoded frame of the nearest preceding data segment (zero before any
** audio exists) and is recovered directly from that compressed block.
*/
void	wavl_fill_block (SF_PRIVATE *psf, int logical_block, short *out) ;

#endif /* WAVL_TYPES_DEFINED */
#endif /* SNDFILE_COMMON_H */
