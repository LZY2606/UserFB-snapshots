/*
 *  dv.h
 *
 *     Copyright (C) Charles 'Buck' Krasic - April 2000
 *     Copyright (C) Erik Walthinsen - April 2000
 *
 *  This file is part of libdv, a free DV (IEC 61834/SMPTE 314M)
 *  codec.
 *
 *  libdv is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU Lesser Public License as published by
 *  the Free Software Foundation; either version 2.1, or (at your
 *  option) any later version.
 *   
 *  libdv is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser Public License for more details.
 *   
 *  You should have received a copy of the GNU Lesser Public License
 *  along with libdv; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA. 
 *
 *  The libdv homepage is http://libdv.sourceforge.net/.  
 */

/** @file
 *  @ingroup decoder
 *  @ingroup encoder
 *  @brief   Interface for @link decoder DV Decoder @endlink
 */

/** @addtogroup decoder DV Decoder
 *  @{
 */

#ifndef DV_H
#define DV_H


#include <libdv/dv_types.h>

#include <stdio.h>
#include <inttypes.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Main API */
extern dv_decoder_t *dv_decoder_new     (int ignored, int clamp_luma,
	                  int clamp_chroma);
extern void         dv_decoder_free     (dv_decoder_t*);
extern void         dv_init             (int clamp_luma, int clamp_chroma);
extern void         dv_cleanup          (void);
extern void         dv_reconfigure      (int clamp_luma, int clamp_chroma); 
extern int          dv_parse_header     (dv_decoder_t *dv, const uint8_t *buffer);
extern void         dv_parse_packs      (dv_decoder_t *dv, const uint8_t *buffer);
extern void         dv_decode_full_frame(dv_decoder_t *dv, 
					  const uint8_t *buffer, dv_color_space_t color_space,
					  uint8_t **pixels, int *pitches);
extern int          dv_decode_full_audio(dv_decoder_t *dv, 
					  const uint8_t *buffer, int16_t **outbufs);
extern int          dv_set_audio_correction (dv_decoder_t *dv, int method);            
extern FILE         *dv_set_error_log (dv_decoder_t *dv, FILE *errfile);            
extern void         dv_report_video_error (dv_decoder_t *dv, uint8_t *data);

#define LIBDV_HAS_SAMPLE_CALCULATOR
extern int          dv_calculate_samples( dv_encoder_t *, int frequency, 
					  int frame_count );

/*@}*/

/** @addtogroup encoder
 *  @{
 */

extern dv_encoder_t *dv_encoder_new     (int rem_ntsc_setup, int clamp_luma,
	                  int clamp_chroma);
extern void         dv_encoder_free     (dv_encoder_t* dv_enc);
extern int          dv_encode_full_frame(dv_encoder_t *dv_enc, uint8_t **in,
					  dv_color_space_t color_space, uint8_t *out);
	
extern int          dv_encode_full_audio(dv_encoder_t *dv_enc, int16_t **pcm,
					  int channels, int frequency, uint8_t *frame);
extern void         dv_encode_metadata(uint8_t *target, int isPAL, int is16x9,
					  time_t *datetime, int frame);
extern void         dv_encode_timecode(uint8_t *target, int isPAL, int frame);

/*@}*/

/** @addtogroup encoder
 *  @{
 */

/** @brief Opaque state for streaming four channel DV audio muxing.
 *
 * A dv_audio_stream_t accepts a continuous flow of 32 kHz four channel
 * interleaved 16 bit PCM and muxes it, one DV frame at a time, into
 * caller supplied DV frames via dv_encode_full_audio().  Video, timecode
 * and all non-audio DIF content of the supplied frames is preserved.
 *
 * The per-frame sample budget is counted in samples per channel.  PAL
 * frames always carry 1280 samples.  NTSC frame k (counted from 0)
 * carries floor((k+1)*32000*1001/30000) - floor(k*32000*1001/30000)
 * samples; the frame counter only advances when a frame is committed.
 * The AAUX sample count of a committed frame always matches the budget
 * it actually carries, for both channel pairs.
 *
 * Tail convention: PCM that does not yet fill a complete frame budget
 * is held inside the stream; the caller's output frame is left
 * completely untouched until a full budget is available.  Feeding zero
 * input frames never inserts silence, and an incomplete tail keeps
 * waiting for more input - there is no automatic pad-and-flush.  Call
 * dv_audio_stream_reset() to explicitly discard a pending tail and
 * restart the frame counter at zero.
 *
 * The stream only keeps one audio frame worth of PCM plus a fixed
 * workspace; memory use does not grow with the length of the recording.
 * Between feeds the caller may hand in a different output frame (of the
 * same video standard); pending audio belongs to whichever frame is
 * committed next.  Once a frame is committed the caller owns it and may
 * immediately reuse or free it.
 */
typedef struct dv_audio_stream_s dv_audio_stream_t;

/** @brief Create an audio stream for one video standard.
 *
 * @param is_pal 1 for PAL (625/50), 0 for NTSC (525/60).  Any other
 *        value is rejected.
 * @return a new stream, or NULL on invalid argument or out of memory.
 */
extern dv_audio_stream_t *dv_audio_stream_new(int is_pal);

/** @brief Destroy an audio stream.  NULL is accepted and ignored. */
extern void  dv_audio_stream_free(dv_audio_stream_t *stream);

/** @brief Discard any buffered PCM and restart the frame counter at
 *         zero.  NULL is accepted and ignored. */
extern void  dv_audio_stream_reset(dv_audio_stream_t *stream);

/** @brief Feed interleaved four channel PCM and maybe commit one DV frame.
 *
 * @param stream The audio stream.
 * @param interleaved 32 kHz signed 16 bit PCM, four channels interleaved.
 *        May be NULL only when \c input_frames is zero.
 * @param input_frames Number of input frames (samples per channel) offered.
 * @param used Receives the number of input frames actually consumed by
 *        this call; the caller is responsible for re-offering the rest.
 *        Set to zero on rejected calls.
 * @param frame Output DV frame; every byte stays untouched unless a
 *        complete frame is committed by this call.
 * @param frame_bytes Size of \c frame: 144000 for PAL, 120000 for NTSC,
 *        matching both the stream standard and the frame's own standard
 *        marker.
 * @return 1 when a DV frame was committed into \c frame, 0 when more
 *        PCM is needed, -1 for invalid calls (negative or missing input,
 *        missing \c used/\c frame, or a frame size/standard mismatch).
 *        Invalid calls consume no PCM and leave the stream state and the
 *        output frame unchanged.  At most one frame is committed per
 *        call.
 */
extern int   dv_audio_stream_feed(dv_audio_stream_t *stream,
				  const int16_t *interleaved,
				  long input_frames, long *used,
				  uint8_t *frame, size_t frame_bytes);

/*@}*/

/** @addtogroup decoder
 *  @{
 */

/* Low level API */
extern int dv_parse_video_segment(dv_videosegment_t *seg, unsigned int quality);
extern void dv_decode_video_segment(dv_decoder_t *dv, dv_videosegment_t *seg, unsigned int quality);

extern void dv_render_video_segment_rgb(dv_decoder_t *dv, dv_videosegment_t *seg,
					uint8_t **pixels, int *pitches);

extern void dv_render_video_segment_yuv(dv_decoder_t *dv, dv_videosegment_t *seg, 
					uint8_t **pixels, int *pitches);

extern int dv_encode_videosegment( dv_encoder_t *dv_enc,
					dv_videosegment_t *videoseg, uint8_t *vsbuffer);

/* ---------------------------------------------------------------------------
 */
extern int dv_set_quality (dv_decoder_t *dv, int quality),
           dv_is_PAL (dv_decoder_t *dv);

/* ---------------------------------------------------------------------------
 * functions based on vaux data
 * return value: <0 unknown, 0 no, >0 yes
 */
extern int dv_frame_changed (dv_decoder_t *dv),
           dv_frame_is_color (dv_decoder_t *dv),
           dv_system_50_fields (dv_decoder_t *dv),
           dv_format_normal (dv_decoder_t *dv),
           dv_format_wide (dv_decoder_t *dv),
           dv_format_letterbox (dv_decoder_t *dv),
           dv_is_progressive (dv_decoder_t *dv),
           dv_get_vaux_pack (dv_decoder_t *dv, uint8_t pack_id, uint8_t *pack_data);

/* ---------------------------------------------------------------------------
 * functions based on ssyb data
 * return value: 0 not-present, >0 ok
 */
extern int dv_get_timestamp (dv_decoder_t *dv, char *tstprt),
           dv_get_recording_datetime (dv_decoder_t *dv, char *dtptr),
           dv_get_timestamp_int (dv_decoder_t *dv, int *timestamp),
           dv_get_recording_datetime_tm (dv_decoder_t *dv, struct tm *rec_dt),
           dv_get_ssyb_pack (dv_decoder_t *dv, uint8_t pack_id, uint8_t *pack_data);

/* ---------------------------------------------------------------------------
 * functions based on aaux data
 */
extern int dv_is_new_recording (dv_decoder_t *dv, const uint8_t *buffer),
		   dv_is_normal_speed (dv_decoder_t *dv),
           dv_set_mixing_level (dv_decoder_t *dv, int new_value);

/* ---------------------------------------------------------------------------
 * audio query functions
 */
extern int dv_get_num_samples (dv_decoder_t *dv),
           dv_get_num_channels (dv_decoder_t *dv),
           dv_get_raw_samples (dv_decoder_t *dv, int chan),
           dv_is_4ch (dv_decoder_t *dv),
           dv_get_frequency (dv_decoder_t *dv);

#ifdef __cplusplus
}
#endif

#endif // DV_H 

/*@}*/
