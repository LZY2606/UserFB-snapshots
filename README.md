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

## shells in par_shapes

`par_shapes_create_shell(source, thickness)` turns an oriented, two-manifold
triangle surface into a solid shell by offsetting it by `thickness / 2` along
each vertex's unit normal.  It is meant for thin-walled models, not for large
offsets: the caller must keep the thickness small enough that the two skins and
their rims do not self-intersect.

The result is a `par_shapes_shell` owning a `par_shapes_mesh` plus two
`int` arrays of `mesh->npoints` entries.  `vertex_source` maps every render
vertex back to a source point and `vertex_layer` is `0` for the inner skin and
`1` for the outer skin.  These pairs are the topological identity of the shell:
merging render copies that share a `(vertex_source, vertex_layer)` pair yields
a closed, edge-manifold surface.  Render copies are deliberately duplicated
where normals or UVs differ (at rims), so do not weld by position.

The outer skin keeps the source winding and the inner skin reverses it.  Rims
are added only along genuine boundary loops, including holes; a closed source
yields two disconnected skins.  Skins copy their source points and UVs, while
all normals are recomputed from the offset triangles and rim facets use flat
per-triangle normals.  `par_shapes_free_shell` releases the whole result, and
the source mesh is never modified.

The input must be an oriented two-manifold with finite coordinates: zero-area
triangles, invalid indices, same-direction or over-shared edges, point-touching
face fans, a non-positive or non-finite thickness, and offset geometry that
degenerates are all rejected by returning `NULL`.  If the source has normals
they define the offset direction (normalized); otherwise the area-weighted
vertex normals are used.  A skin or rim triangle that collapses to zero area
after offsetting, or a referenced skin point whose offset normals cancel out,
is likewise rejected rather than silently producing a broken shell.

```c
par_shapes_mesh* surface = par_shapes_create_plane(4, 4);
par_shapes_shell* shell = par_shapes_create_shell(surface, 0.1f);
par_shapes_export(shell->mesh, "thick.obj");
par_shapes_free_shell(shell);
par_shapes_free_mesh(surface);
```

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

