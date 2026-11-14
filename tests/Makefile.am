## librdfa/tests/Makefile.am

noinst_PROGRAMS = \
	rdfa2n3 \
	rdfastring2n3 \
	curies \
	speed \
	speed2 \
	property_copy_example

property_copy_example_SOURCES = property_copy_example.c

EXTRA_DIST = \
	test-cases/fetch-tests \
	property_copy_target.c

AM_CPPFLAGS = \
	-I$(top_srcdir)/c \
	$(LIBXML2_CFLAGS)

LDADD = \
	$(top_builddir)/c/librdfa.la \
	$(LIBXML2_LIBS)

## end librdfa/tests/Makefile.am
