[![Build Status](https://travis-ci.org/prideout/par.svg?branch=master)](https://travis-ci.org/prideout/par)

## par

Single-file C libraries under the MIT license, mostly graphics related.
Documentation can be found at the top of each header file, but some libraries have an accompanying blog post.
The most useful ones are listed in the following table.

library    | description  | link
------------------- | ---- | ---
**par_camera_control.h** | orbit controller, or pan-and-zoom like Google Maps | [demo project](https://github.com/prideout/camera_demo)
**par_octasphere.h** | malloc-free mesh gen for spheres and rounded cuboids | [blog post](https://prideout.net/blog/octasphere)
**par_streamlines.h** | triangulate wide lines and curves | [blog post](https://prideout.net/blog/par_streamlines/)
**par_string_blocks.h** | string manager for snippets of Lua or GLSL |
**par_shapes.h** | generate parametric surfaces and other simple shapes | [blog post](https://prideout.net/shapes)

There are more libraries too but they're probably less useful; scroll to the bottom of this README.

## tests

To run tests, you need CMake and pkg-config.  On macOS, these can be installed with homebrew:

```bash
$ brew install cmake pkg-config
```

Here's how you can tell CMake to use the CMakeLists in the `test` folder, placing all the messy stuff in a new folder called `build`.

```bash
$ cmake test -Bbuild   # Create makefiles
$ cmake --build build  # Invoke the build
```

The tests are executed by simply running the programs:
```bash
$ build/test_bubbles
$ build/test_shapes
$ build/test_octasphere
```

## rounded convex meshes

`par_shapes_create_rounded(source, radius, max_error)` builds the closed
triangle boundary of the Minkowski sum of a convex polyhedron and a sphere.
The source must be a connected, outward-oriented, closed, non-degenerate convex
triangle mesh; its vertices must be true extreme points.  Coplanar triangle
diagonals are merged before constructing planar, cylindrical edge and spherical
corner patches, so triangulation of a polygonal face does not create false
rounded seams.

Let `L` be the source bounding-box diagonal.  The documented geometry range is
`0.1 <= L <= 100`, `0.02 <= radius/L <= 1`, and
`0.001 <= max_error/radius <= 0.1`.  Invalid topology, reversed faces,
non-convex or degenerate input, non-finite numbers, non-positive parameters and
unsupported ranges return `NULL`.  `max_error` is the world-space two-sided
Hausdorff budget for the complete output surface; subdivision uses the actual
edge normal angles.

The returned mesh is an indexed closed shell with allocated analytic normals
and no texture coordinates.  Each normal points from the nearest point on the
original polyhedron toward the rounded surface, making planar, edge and corner
normals continuous at shared indices.  The result owns independent storage, so
the source can be freed immediately.  Use `par_shapes_free_mesh` to release it.

The task test and example can be built and run with:

```bash
$ bash support/run.sh target
$ cc -std=c99 -DPAR_SHAPES_IMPLEMENTATION -I. examples/rounded.c -lm -o rounded
```

## code formatting

This library's code style is strictly enforced to be vertically dense (no consecutive newlines) and 100 columns or less.

The `tools/format.py` script invokes a two-step code formatting process:

1. Runs `uncrustify` with our custom configuration.  This auto-formats all code in the root folder, up to a point.
1. Checks for violations that are not otherwise enforced with uncrustify.

The aforementioned Python script is also invoked from Travis, but using the `--check` option, which checks for conformance without editing the code.

Beyond what our uncrustify configuration enforces, the Python script does the following:

- Checks that no lines are more than 100 chars.
- Checks for extra newlines before an end brace.

## other libraries

library    | description  | link
------------------- | ---- | ---
**par_bluenoise.h** | generate progressive 2D point sequences | [blog post](https://prideout.net/recursive-wang-tiles)
**par_bubbles.h** | pack circles into hierarchical diagrams | [blog post](https://prideout.net/bubbles)
**par_easings.h** | Robert Penner's easing functions |
**par_easycurl.h** | simple HTTP requests using libcurl |
**par_filecache.h** | LRU caching on your device's filesystem |
**par_sprune.h** | efficient broad-phase collision detection in 2D | [web demo](https://prideout.net/d3cpp/)
**par_msquares.h** | unmaintained marching squares library (do not use) | [blog post](https://prideout.net/marching-squares)

