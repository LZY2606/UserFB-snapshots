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

## adaptive parametric surfaces

`par_shapes_create_adaptive` tessellates a `par_shapes_fn` parametric surface
with local subdivision instead of one uniform `slices x stacks` grid, so a
small bump no longer forces the whole parameter domain to the finest level.

```c
par_shapes_adaptive* result = par_shapes_create_adaptive(
    my_surface, userdata,
    0.002f,  // tolerance
    7,       // max depth, must be in [2, 8]
    true,    // wrap_u: the u = 0 and u = 1 sides are the same position
    true);   // wrap_v: likewise for v
if (!result) {
    // generation failed; nothing was allocated for the caller to free
}
par_shapes_mesh* mesh = result->mesh;
for (int i = 0; i < result->nleaves; i++) {
    par_shapes_adaptive_leaf leaf = result->leaves[i];
    // leaf covers [ix/2^d,(ix+1)/2^d] x [iy/2^d,(iy+1)/2^d];
    // leaf.center is the mesh point at its true surface center.
}
par_shapes_free_adaptive(result); // frees the mesh, leaves and struct
```

How it works, and what to rely on:

- **Error estimate.** The domain starts as a 4x4 grid of depth-2 cells. Each
  cell samples its four corners, four edge midpoints and center. The error
  `eta` is the largest Euclidean distance between the five interior *true*
  points and the bilinear surface through the four corners. A cell with
  `eta > tolerance` splits into four equal children, down to `max_depth`.
  This is a sampling estimate, not a bound over the whole surface: a callback
  that oscillates between the nine sample points is not detected.
- **2:1 balance.** Splitting propagates across shared edges so neighbouring
  leaves differ in depth by at most one; periodic sides count as neighbours.
  A fine/coarse boundary shares its edge midpoint, so there are no
  T-junctions, duplicate faces or zero-area triangles.
- **Periodic topology.** When `wrap_u`/`wrap_v` is set the caller guarantees
  matching positions and tangents across that axis. Refinement stays
  synchronized on both sides, the 0 and 1 UV seam vertices remain distinct
  render vertices but share one position and one smooth normal, and the four
  corner copies of a doubly periodic surface are handled. Non-periodic points
  are never welded even when their world positions coincide.
- **Normals.** Area-weighted normals are accumulated by parameter identity
  (so seam replicas agree) and unitized; every referenced vertex gets a
  unit normal, and close opposing sheets keep independent normals.
- **Failure and ownership.** The returned struct, its `mesh` and `leaves` are
  independently owned by the caller. Invalid arguments, depth exhaustion,
  non-finite callback output, allocation or index-capacity overflow, and
  degenerate triangles all return `NULL` after releasing every intermediate
  allocation; the function never silently relaxes `tolerance` or raises
  `max_depth`. `par_shapes_free_adaptive(NULL)` is safe.

See `examples/adaptive.c` for a complete program that writes an OBJ and
prints the leaf-depth distribution.

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

