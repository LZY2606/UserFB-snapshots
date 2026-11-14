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

## orienting solid shells (par_shapes.h)

`bool par_shapes_orient_solid(par_shapes_mesh* mesh, int* face_depth)` repairs
the winding of a triangle soup that is known to be a set of closed solid
boundary shells, e.g. output by an importer that glued several shells
together with inconsistent facing.

Geometric preconditions (validated where cheap, otherwise assumed):

- Connectivity is by vertex index only; coincident positions are never welded.
- Every edge is shared by exactly two triangles, the faces around each vertex
  form a single ring, and every connected component is orientable.  Invalid
  input (out-of-range indices, non-finite points, zero-area or duplicate
  faces, open or over-shared edges, multi-ring fans, non-orientable
  components) makes the function return `false`.
- The caller guarantees the shells are embedded (no self-intersections) and
  separated by positive gaps; intersections are not detected.

On success the winding of every shell is made consistent and each shell faces
according to its nesting depth: shells contained by an even number of other
shells (outer boundaries) face away from the bounded region they enclose,
while shells at odd depth (cavity walls) face into it.  Containment is
decided against the actual triangle surfaces, never bounding boxes.  Face
order and the first index of each face are preserved (a face is only ever
modified by swapping its second and third indices), points and texture
coordinates are never touched, and normals are recomputed from the final
winding if the mesh has them (area-weighted, unit length, zero for
unreferenced vertices).

`face_depth` may be null; otherwise it must point to `ntriangles` ints owned
by the caller, which receive the nesting depth of each face's shell (0 for
outermost).  It is written only on success.  On failure (invalid input or
allocation failure) the mesh and `face_depth` are left untouched and `false`
is returned.  An empty mesh is trivially valid and returns `true`.

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

