# OPENJPEG Library and Applications

## Unmaintained repository

This repository should be considered unmaintained. Commits may appear from time
to time depending on committer interest and mood, but no committer currently feel
responsible to regularly review tickets or pull requests.

## What is OpenJPEG ? 

OpenJPEG is an open-source JPEG 2000 codec written in C language. It has been developed in order to promote the use of [JPEG 2000](http://www.jpeg.org/jpeg2000), a still-image compression standard from the Joint Photographic Experts Group ([JPEG](http://www.jpeg.org)).  Since April 2015, it is officially recognized by ISO/IEC and ITU-T as a [JPEG 2000 Reference Software](http://www.itu.int/rec/T-REC-T.804-201504-I!Amd2).

## Who can use the code ?
[![badge-license]][link-license]

Anyone. As the OpenJPEG code is released under the [BSD 2-clause "Simplified" License][link-license], anyone can use or modify the code, even for commercial applications. The only restriction is to retain the copyright in the sources or in the binaries documentation. Of course, if you modified the code in a way that might be of interest for other users, you are encouraged to share it (through a [github pull request](https://github.com/uclouvain/openjpeg/pulls) or by filling an [issue](https://github.com/uclouvain/openjpeg/issues)) but this is not a requirement.

## How to install and use OpenJPEG ?
API Documentation needs a major refactoring. Meanwhile, you can check [installation](https://github.com/uclouvain/openjpeg/wiki/Installation) instructions and [codec documentation](https://github.com/uclouvain/openjpeg/wiki/DocJ2KCodec).
    
## Current Status
[![badge-build]][link-build]

[![badge-msvc-build]][link-msvc-build] 

[![badge-coverity]][link-coverity]

## Who are the developers ?

The library is developed and maintained by the Image and Signal Processing Group ([ISPGroup](http://sites.uclouvain.be/ispgroup/)), in the Université catholique de Louvain ([UCL](http://www.uclouvain.be/en-index.html), with the support of the [CNES](https://cnes.fr/), the [CS](http://www.c-s.fr/) company and the [intoPIX](http://www.intopix.com) company. The JPWL module has been developed by the Digital Signal Processing Lab ([DSPLab](http://dsplab.diei.unipg.it/)) of the University of Perugia, Italy ([UNIPG](http://www.unipg.it/)).

## Details on folders hierarchy

* src
  * lib
    * openjp2: contains the sources of the openjp2 library (Part 1 & 2)
    * openjpip: complete client-server architecture for remote browsing of jpeg 2000 images.
  * bin: contains all applications that use the openjpeg library
    * common: common files to all applications
    * jp2: a basic codec
    * jpip: OpenJPIP applications (server and dec server)
      * java: a Java client viewer for JPIP
    * wx
      * OPJViewer: gui for displaying j2k files (based on wxWidget)
* wrapping
  * java: java jni to use openjpeg in a java program
* thirdparty: thirdparty libraries used by some applications. These libraries will be built only if there are not found on the system. Note that libopenjpeg itself does not have any dependency.
* doc: doxygen documentation setup file and man pages
* tests: configuration files and utilities for the openjpeg test suite. All test images are located in [openjpeg-data](https://github.com/uclouvain/openjpeg-data) repository.
* cmake: cmake related files
* scripts: scripts for developers

See [LICENSE][link-license] for license and copyright information.

See [INSTALL](https://github.com/uclouvain/openjpeg/blob/master/INSTALL.md) for installation procedures.

See [NEWS](https://github.com/uclouvain/openjpeg/blob/master/NEWS.md) for user visible changes in successive releases.

## Deprecated components

The openjpwl, openjp3d and openmj2 components have been removed after the
2.4.0 release. Their building and working state is unknown. People interested
in them should start from the 2.4.0 tag.

## API/ABI

An API/ABI timeline is automatically updated [here][link-api-timeline].

OpenJPEG strives to provide a stable API/ABI for your applications. As such it
only exposes a limited subset of its functions.  It uses a mechanism of
exporting/hiding functions. If you are unsure which functions you can use in
your applications, you should compile OpenJPEG using something similar to gcc:
`-fvisibility=hidden` compilation flag.
See also: http://gcc.gnu.org/wiki/Visibility

On windows, MSVC directly supports export/hiding function and as such the only
API available is the one supported by OpenJPEG.

## Reversible 5/3 DWT regression verification

The `verification/` directory contains a self-contained quality gate for the
lossless reversible 5/3 wavelet path. It needs only C and a POSIX shell: no
test images are downloaded, the command line codec tools are not built, and no
PNG/TIFF or other third-party library is used.

* `verification/baseline.sh` builds the static library and checks the
  five-sample forward and inverse lifting vectors documented in the code.
* `tests/dwt_reference.[ch]` is an independent 64-bit integer reference written
  directly from the JPEG 2000 reversible 5/3 predict/update definitions. It
  implements floor-towards-negative-infinity division explicitly, symmetric
  boundary extension, the LL/HL/LH/HH subband layout and multilevel 2-D
  decomposition. It never calls the OpenJPEG lifting, deinterleave or DWT code
  and does not copy the production unrolled loops.
* `tests/dwt_contract.c` registers six groups with `verification/driver.h`:
  `coefficient-grid`, `inverse-grid`, `multilevel`, `roi-reduce`, `threads`
  and `codec-roundtrip`. Each group returns success only after every assertion
  passes; there are no skip branches, and every mismatch reports the direction,
  decomposition level, subband, global coordinate and first differing integer.

The two layers prove different properties, so neither replaces the other:

* The **independent reference** proves the production coefficients are the
  mathematically correct 5/3 coefficients: production forward output is
  compared sample by sample with the reference over all four global x0/y0
  parity combinations, sizes 1, 2, 3, 5 and 17x19 (which crosses the batched
  column boundary), one to five resolution levels (four decomposition levels,
  with illegal level counts for a shape filtered out), negative inputs,
  asymmetric ramps, edge pulses and deterministic aperiodic patterns. The
  odd-origin singleton has its own expected `x2` result. Reference-generated
  coefficients are then fed to the production inverse and must reproduce the
  exact original image; a production forward/inverse pair merely cancelling
  itself out would not pass. Multilevel LL decomposition keeps the real
  component coordinates at every level instead of restarting at zero, and
  thread counts 0 and 2 must produce identical coefficients and pixels.
* The **public codec integration** proves the same guarantees through the
  documented `opj_*` API: a programmatically generated signed-component image
  with a non-zero origin, several tiles, different component dx/dy, the
  reversible transform and MCT disabled is encoded to an in-memory J2K stream.
  Full decoding is sample-exact at `cp_reduce=0`; for `cp_reduce` 0/1/2,
  `opj_set_decode_area()` windows crossing tile edges are compared with the
  crop of the same-level full decode on each component grid, using a fresh
  decoder per query and explicit window-to-component coordinate assertions.
  The reduced-resolution full decode is also checked against the independent
  per-tile LL reference, rather than against naive subsampling.

Run `bash verification/target.sh` to build the static library, execute all six
groups and then the mutation check. The measured matrix on this target is
3483 coefficient-grid combinations, 3480 inverse-grid combinations, 32
multilevel combinations, 7 roi/reduce decode combinations (one full decode plus
two windows over three reduction levels), 16 thread comparisons and 1 public
codec roundtrip, i.e. 7019 executed combinations plus the six group
registrations.

`verification/mutations.sh` builds three independent temporary source copies,
each with one production mutation (forward transform ignoring horizontal
origin parity, inverse update dropping the `+2` rounding correction, and the
odd-origin singleton forward transform missing its `x2` scaling). Each copy
must compile and then fail with a reported numeric mismatch; build errors,
crashes, timeouts or runs without executed cases are rejected. All three
mutations print `MUTATION_NUMERIC_PASS origin-parity`,
`MUTATION_NUMERIC_PASS inverse-rounding` and
`MUTATION_NUMERIC_PASS singleton` respectively, and the working tree is left
untouched.

[comment-license]: https://img.shields.io/github/license/uclouvain/openjpeg.svg "https://img.shields.io/badge/license-BSD--2--Clause-blue.svg"
[badge-license]: https://img.shields.io/badge/license-BSD--2--Clause-blue.svg "BSD 2-clause \"Simplified\" License"
[link-license]: https://github.com/uclouvain/openjpeg/blob/master/LICENSE "BSD 2-clause \"Simplified\" License"
[badge-build]: https://github.com/uclouvain/openjpeg/actions/workflows/build.yml/badge.svg "Build Status"
[link-build]: https://github.com/uclouvain/openjpeg/actions/workflows/build.yml "Build Status"
[badge-msvc-build]: https://ci.appveyor.com/api/projects/status/github/uclouvain/openjpeg?branch=master&svg=true "Windows Build Status"
[link-msvc-build]: https://ci.appveyor.com/project/detonin/openjpeg/branch/master "Windows Build Status"
[badge-coverity]: https://scan.coverity.com/projects/6383/badge.svg "Coverity Scan Build Status"
[link-coverity]: https://scan.coverity.com/projects/uclouvain-openjpeg "Coverity Scan Build Status"
[link-api-timeline]: http://www.openjpeg.org/abi-check/timeline/openjpeg "OpenJPEG API/ABI timeline"
