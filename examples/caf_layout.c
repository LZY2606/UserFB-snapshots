/*
** Copyright (C) 2026 The libsndfile authors
**
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**
**     * Redistributions of source code must retain the above copyright
**       notice, this list of conditions and the following disclaimer.
**     * Redistributions in binary form must reproduce the above copyright
**       notice, this list of conditions and the following disclaimer in
**       the documentation and/or other materials provided with the
**       distribution.
**     * Neither the author nor the names of any contributors may be used
**       to endorse or promote products derived from this software without
**       specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
** TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
** PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
** CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
** EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
** PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
** OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
** WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
** OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
** ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
**	Write a CAF file whose three channels are laid out as right, left, center.
**	That ordering has no predefined CAF layout tag, so libsndfile stores it as
**	a UseChannelDescriptions array. The samples are written exactly in the
**	order the channel map declares: right first, then left, then center.
*/

#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>

#include	<sndfile.h>

#define		SAMPLE_RATE			8000
#define		FRAME_COUNT			2

int
main (void)
{	SNDFILE	*file ;
	SF_INFO	sfinfo ;
	int		channel_map [3] = { SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_CENTER } ;
	short	pcm [FRAME_COUNT * 3] = { 10, 20, 30, -40, 50, -60 } ;

	memset (&sfinfo, 0, sizeof (sfinfo)) ;

	sfinfo.samplerate	= SAMPLE_RATE ;
	sfinfo.channels		= 3 ;
	sfinfo.format		= (SF_FORMAT_CAF | SF_FORMAT_PCM_16) ;

	if (! (file = sf_open ("caf_layout.caf", SFM_WRITE, &sfinfo)))
	{	printf ("Error : Not able to open output file : %s\n", sf_strerror (NULL)) ;
		return 1 ;
		} ;

	/* Must be set before the first frame is written. */
	if (sf_command (file, SFC_SET_CHANNEL_MAP_INFO, channel_map, sizeof (channel_map)) != SF_TRUE)
	{	printf ("Error : Channel map rejected : %s\n", sf_strerror (file)) ;
		sf_close (file) ;
		return 1 ;
		} ;

	if (sf_writef_short (file, pcm, FRAME_COUNT) != FRAME_COUNT)
		puts (sf_strerror (file)) ;

	sf_close (file) ;
	return 0 ;
} /* main */
