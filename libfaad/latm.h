/*
** FAAD2 - Freeware Advanced Audio (AAC) Decoder including SBR decoding
** Copyright (C) 2003-2005 M. Bakker, Nero AG, http://www.nero.com
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
**
** Any non-GPL usage of this software or parts of this software is strictly
** forbidden.
**
** Commercial non-GPL licensing of this software is possible.
** For more info contact Nero AG through Mpeg4AAClicense@nero.com.
**
** LOAS/LATM transport support (audioMuxVersion == 0,
** allStreamsSameTimeFraming == 1, single program, single layer,
** frameLengthType == 0, plain AAC-LC payload).
**/

#ifndef __LATM_H__
#define __LATM_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the decoder from the first complete LOAS frame.
** Parses the StreamMuxConfig carried by the frame, validates it against
** the supported subset and sets up the decoder core from the ASC.
** Returns 0 on success (the same frame must still be passed to
** NeAACDecDecode), negative on error.
*/
long faad_latm_init(NeAACDecStruct *hDecoder,
                    unsigned char *buffer,
                    unsigned long buffer_size,
                    unsigned long *samplerate,
                    unsigned char *channels);

/* Decode exactly one complete LOAS frame.  Never consumes bytes beyond
** the current frame (3 byte LOAS header + audioMuxLengthBytes), even if
** the input buffer holds the next frame already.  All access units of
** the frame are decoded in order on the same decoder history and the
** resulting PCM is concatenated.
*/
void *faad_latm_decode(NeAACDecStruct *hDecoder,
                       NeAACDecFrameInfo *hInfo,
                       unsigned char *buffer,
                       unsigned long buffer_size,
                       void **sample_buffer2,
                       unsigned long sample_buffer_size);

/* defined in decoder.c */
void create_channel_config(NeAACDecStruct *hDecoder, NeAACDecFrameInfo *hInfo);

#ifdef __cplusplus
}
#endif
#endif
