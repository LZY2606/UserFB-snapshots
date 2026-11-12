LodePNG
-------

PNG encoder and decoder in C and C++, without dependencies

Home page: http://lodev.org/lodepng/

### Documentation

Detailed documentation is included in a large comment in the second half of the
header file `lodepng.h`.

Source code examples using LodePNG can be found in the examples directory.

An FAQ can be found on http://lodev.org/lodepng/

### Building

Only two files are needed to encode and decode PNGs:

* `lodepng.cpp` (or renamed to `lodepng.c`)
* `lodepng.h`

All other files are just source code examples, tests, misc utilities, etc...,
which are normally not needed in projects using this.

You can include the files directly in your project's source tree and its
makefile, IDE project file, or other build system. No library is necessary.

In addition to C++, LodePNG also supports ANSI C (C89), with all the same
functionality: C++ only adds extra convenience API.

For C, rename `lodepng.cpp` to `lodepng.c`.

Consider using git submodules to include LodePNG in your project.

### Compiling in C++

If you have a hypothetical `your_program.cpp` that #includes and uses `lodepng.h`,
you can build as follows:

`g++ your_program.cpp lodepng.cpp -Wall -Wextra -pedantic -ansi -O3`

or:

`clang++ your_program.cpp lodepng.cpp -Wall -Wextra -pedantic -ansi -O3`

This shows compiler flags it was designed for, but normally one would use the
compiler or build system of their project instead of those commands, and other
C++ compilers are supported.

### Compiling in C

Rename `lodepng.cpp` to `lodepng.c` for this.

If you have a hypothetical your_program.c that #includes and uses lodepng.h,
you can build as follows:

`gcc your_program.c lodepng.c -ansi -pedantic -Wall -Wextra -O3`

or

`clang your_program.c lodepng.c -ansi -pedantic -Wall -Wextra -O3`

This shows compiler flags it was designed for, but normally one would use the
compiler or build system of their project instead of those commands, and other
C compilers are supported.

### Makefile

There is a Makefile, but this is not intended for using LodePNG itself since the
way to use that one is to include its source files in your program. The Makefile
only builds development and testing utilities. It can be used as follows:

`make -j`

### LZ77 suffix-array match index

For large sliding windows (`windowsize >= 8192`) the encoder used to find the
longest match by walking a full hash chain of previous positions, which on
images with long near-duplicate runs repeats the same byte comparisons over and
over. The encoder now builds, once per compression run, a suffix-array index of
the whole input and answers longest-match queries from it.

The index (`LZ77Index` in `lodepng.cpp`) contains:

* a doubling-algorithm suffix array and its inverse rank array,
* a Kasai longest-common-prefix (LCP) array with an iterative range-minimum
  segment tree for O(log n) LCP between any two positions,
* a position-keyed rank min/max segment tree that finds the newest in-window
  position whose suffix shares a given prefix, also in O(log n),
* a small boundary hash chain for the last two positions of each deflate block,
  whose stored hash uses lodepng's special end-of-block formula.

A query only ever returns candidate positions strictly before the current
position, inside the sliding window, and inside the current deflate block, so
reading the whole input up front never leaks future bytes into the dictionary.
The selected length/distance pairs are identical to the hash-chain result: the
same longest match, the same tie distance, the same `nicematch` early stop, the
same three-byte far-distance rule and the same lazy fallback. Small windows,
long zero runs and block-tail positions keep the original chain code. Long zero
runs still use the dedicated zero-chain acceleration. Inputs larger than
`INT_MAX` bytes fall back to the chain because the suffix array stores
positions in `int`.

Memory stays linear: roughly eleven `int` arrays and one byte array of input
length plus the fixed 65536-entry boundary hash table (on top of the unchanged
window-sized hash tables). No pairwise LCP table is built and nothing is cached
between compression runs. The production code remains plain C89 and uses no
STL.

When compiled with `-DLODEPNG_LZ77_TEST`, two counters are exposed:

* `lodepng_lz77test_byte_comparisons` - direct input byte equality tests,
  including the Kasai construction, boundary queries and chain fallback;
* `lodepng_lz77test_candidate_checks` - candidate match evaluations, including
  index queries, boundary lookups and fallback chain iterations.

On the fixed near-duplicate input in `verification/baseline.cpp`
(165120 bytes, window 32768, minmatch 3, nicematch 258, lazy on) the indexed
matcher produces the same 14163 internal token elements while reducing the
byte comparisons from 4254727288 to about 330000 and the candidate checks from
34742697 to about 153000, each comfortably below one quarter of the original.
Timing is measured by the verification but is not used as a pass condition.

### Verification

The added checks live in `tests/lz77_index.cpp`, which includes the production
source directly and the unmodified `verification/reference/` copy in a separate
namespace, comparing both internal LZ77 tokens and public zlib/PNG bytes.

Run the six registered groups (legacy-stream, lazy-window, block-history,
index-work, png-path, c-compatibility). The stock encoder binary is located
through the `STOCK_ENCODER` environment variable:

`bash verification/target.sh`

Run the baseline round-trip probe together with the full upstream lodepng unit
suite:

`bash verification/baseline.sh`

On a sandboxed system whose default temp directory is not writable, point
`TMPDIR` at a writable directory; the scripts use `mktemp -d` internally.

