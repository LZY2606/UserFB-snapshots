/*
** Copyright (C) 2026 Erik de Castro Lopo <erikd@mega-nerd.com>
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
*/

/* caf_alac_cut : copy a window of a CAF/ALAC file without re-encoding.
**
** Usage : caf_alac_cut <input.caf> <output.caf> <start> <frames>
**
** start is measured in audible frames from the start of the input, frames is
** the length of the half-open window [start, start + frames). Packets that
** intersect the window are copied verbatim; only the 'pakt' framing changes.
*/

#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>
#include	<errno.h>

#include	<sndfile.h>

static void
usage (const char *name)
{	printf ("Usage : %s <input.caf> <output.caf> <start> <frames>\n", name) ;
	printf ("  Copy the [start, start + frames) window of a CAF/ALAC file\n") ;
	printf ("  without re-encoding. Both counts are in frames.\n") ;
} /* usage */

int
main (int argc, char *argv [])
{	sf_count_t start, frames ;
	char *end ;

	if (argc != 5)
	{	usage (argv [0]) ;
		return 1 ;
		} ;

	errno = 0 ;
	start = strtoll (argv [3], &end, 10) ;
	if (errno != 0 || end == argv [3] || *end != 0 || start < 0)
	{	fprintf (stderr, "Bad start value '%s'.\n", argv [3]) ;
		return 1 ;
		} ;

	errno = 0 ;
	frames = strtoll (argv [4], &end, 10) ;
	if (errno != 0 || end == argv [4] || *end != 0 || frames <= 0)
	{	fprintf (stderr, "Bad frames value '%s'.\n", argv [4]) ;
		return 1 ;
		} ;

	if (sf_caf_alac_cut (argv [1], argv [2], start, frames) != 0)
	{	fprintf (stderr, "Failed to cut '%s' into '%s'.\n", argv [1], argv [2]) ;
		return 1 ;
		} ;

	return 0 ;
} /* main */
