#  example_c_cuesplit_file - CUESHEET stream splitter example
#  Copyright (C) 2025  Xiph.Org Foundation
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.

EXTRA_DIST = \
	CMakeLists.txt

AM_CPPFLAGS = -I$(top_builddir) -I$(srcdir)/include -I$(top_srcdir)/include
noinst_PROGRAMS = example_c_cuesplit_file
example_c_cuesplit_file_LDADD = \
	$(top_builddir)/src/libFLAC/libFLAC.la \
	@OGG_LIBS@ \
	-lm

example_c_cuesplit_file_SOURCES = main.c

CLEANFILES = example_c_cuesplit_file.exe
