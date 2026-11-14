librdfa - The Fastest RDFa Processor on the Internet
====================================================

librdfa is a SAX-based [RDFa] processor written in C for XML and HTML 
family languages. It currently supports XML+RDFa, XHTML+RDFa, SVG+RDFa,
HTML4+RDFa and HTML5+RDFa for both RDFa 1.0 and RDFa 1.1.

Building and Running librdfa
----------------------------

Make sure that you have the following software installed on your system
before attempting to build librdfa:

    autoconf
    automake
    gcc
    make
    pkg-config
    libtool
    libxml2-dev

Make sure you have the following packages installed if you're attempting
to build the Python RDFa libraries:

    g++
    python-dev 
    swig

Make sure you have the following packages installed if you are attempting
to build the Debian/Ubuntu packages:

    dpkg-buildpackage
    dpkg-dev
    python-all-dev 
    dh-buildinfo 
    debhelper

Change into the librdfa source directory and run the following:

    ./autogen.sh
    ./configure
    make
    ./tests/speed 

If everything goes well, the speed test should be able to crank through
documents at 32 MB/sec and generate roughly 6.5k triples/sec.

Optionally, you can build the Python bindings with the following:

    ./configure --enable-python
    make

HTML+RDFa 1.1 Property Copying
------------------------------

HTML+RDFa 1.1 lets documents reuse attributes by declaring a resource
with `rdfa:Pattern` and pointing at it with `rdfa:copy`.  librdfa
supports that processing as an optional mode, which is disabled by
default:

    rdfacontext* context = rdfa_create_context(base_uri);
    rdfa_set_default_graph_triple_handler(context, &default_graph_triple);
    rdfa_set_property_copying(context, 1);
    rdfa_parse(context);

The option must be set on the root context before `rdfa_parse_start()`
(or `rdfa_parse()`).  When it is enabled the default graph is buffered
for the whole document instead of being delivered as it is produced.
After a successful end of input the `rdfa:copy` relations are applied to
a fixed point, so chained and mutually recursive patterns converge
regardless of declaration order, and then the copy relations and the
copied pattern definitions are removed before each remaining triple is
delivered exactly once.  Namespace and processor graph notifications are
unaffected.  With the option disabled the parser keeps its streaming
behaviour.

See `tests/property_copy_example.c` for a complete example.

Development
-----------

pkg-config support is provided to setup cflags and libs. The .pc file is
installed along with development files. Add the librdfa source dir to
the `PKG_CONFIG_PATH` environment var to use the uninstalled library.

Building a simple program using librdfa pkg-config support:

    cc `pkg-config --libs --cflags librdfa` -o test test.c

Installation
------------

Use configure options to setup install directories.  For more information:

    ./configure --help

To install:

    make install

[RDFa]: http://rdfa.info/
