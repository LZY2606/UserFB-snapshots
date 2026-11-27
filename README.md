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

## solid orientation in par_shapes

`par_shapes_orient_solid(mesh, face_depth)` repairs the winding of a mesh
that holds one or more closed triangle shells, as produced by importers that
glue shells together with arbitrary per-face winding.  Connectivity is
determined by vertex indices only; positions are never welded.  The caller
guarantees that every shell is a self-intersection-free embedded surface and
that distinct shells neither intersect nor touch.

The function makes the two triangles on either side of every shared edge
wind oppositely, then assigns each connected shell a nesting depth using
containment tests against the actual triangle surfaces: 0 for shells not
enclosed by any other, 1 for the walls of cavities inside a solid, 2 for
solids inside those cavities, and so on.  Shells at even depth are wound so
their front faces point away from the bounded region they enclose; shells at
odd depth point into it.  Face order and the first index of every face are
preserved — only the second and third index of a face may be swapped — and
points, texture coordinates, and their counts are never modified.  If the
mesh has normals they are recomputed from the final winding; otherwise they
stay null.  If `face_depth` is not null it must point to `ntriangles` ints
owned by the caller, which receive the per-face depth on success.

The function returns false without modifying the mesh or `face_depth` when
the input is invalid: out-of-range indices, non-finite positions, zero-area
or duplicated faces, edges shared by other than two faces, vertex fans that
do not form a single ring, or topology that cannot be oriented consistently.
A null mesh or negative counts return false; an empty mesh returns true.

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

