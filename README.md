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

`par_shapes.h` can also tessellate a `par_shapes_fn` callback with an adaptive quadtree instead of a uniform slices × stacks grid, which is useful when curvature is concentrated in a small region of the parameter domain:

```c
par_shapes_adaptive* a = par_shapes_create_adaptive(fn, userdata,
    0.002f /* tolerance */, 7 /* max_depth (2..8) */,
    true /* wrap_u */, true /* wrap_v */);
if (!a) {
    // Failure: invalid arguments, max_depth exhausted, non-finite
    // callback output, allocation failure, or degenerate triangles.
    // All intermediate allocations were released; nothing to free.
}
par_shapes_mesh* mesh = a->mesh;             // owned by you
par_shapes_adaptive_leaf* leaves = a->leaves; // a->nleaves entries
par_shapes_free_adaptive(a); // frees mesh, leaves, and the struct; NULL-safe
```

Each leaf records its quadtree cell (`depth`, `ix`, `iy` spanning `[ix,ix+1]/2^depth × [iy,iy+1]/2^depth`) and the index of its center vertex; every output triangle belongs to the fan of exactly one leaf center.  The per-cell error estimate is the maximum distance between five parametric samples (the four edge midpoints and the center) and the bilinear interpolation of the four corner positions.  It is only a sampling estimate, not a guaranteed bound on the distance to the true surface for arbitrary callbacks.  Leaves are balanced so that cells sharing an edge differ in depth by at most one, so a local bump does not force the whole domain to the deepest level.

With `wrap_u` / `wrap_v`, the caller promises that the two ends of the axis are the same smooth surface.  The subdivision is kept in sync across the seam and the mesh keeps distinct UV 0 and UV 1 render vertices, but positions are evaluated at the canonical parameter copy and smooth normals are accumulated by parametric identity, so seam duplicates get identical positions and normals.  Points that are merely close in world space are never welded together.  On failure the function returns NULL and releases every intermediate allocation; ownership of a successful result passes to the caller.  See `examples/adaptive.c` for a complete program.

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

