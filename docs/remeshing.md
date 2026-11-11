# Remeshing {#remeshing}

A common task in mesh processing is to improve the shape and size of the elements in a triangle mesh. Surface triangulations coming out of CAD programs, 3D modeling tools, or surface reconstruction can contain low quality elements not suitable for downstream processing. Low quality triangles typically include very long and skinny triangles or zero area faces.

Try for yourself:

\htmlonly
<iframe class="demo" src="/demos/remeshing.html"></iframe>
\endhtmlonly

PMP provides an implementation of the remeshing algorithm by Botsch and Kobbelt \cite botsch_2004_remeshing that improves the quality of the triangles in your mesh. The algorithm is based on applying a series of local mesh modifications through edge splits, collapses, flips, as well as tangential smoothing. In addition to the standard uniform remeshing, PMP also provides a curvature-adaptive remeshing based on the paper of Dunyach et al. \cite dunyach_2013_adaptive

In this section, you will learn the basics of the algorithm and how to use it effectively for remeshing tasks.

\note Remeshing only works on triangle meshes.

## Algorithm Basics

In this section, we provide a basic overview of the algorithm. Please refer to the original publications \cite botsch_2004_remeshing and \cite dunyach_2013_adaptive for details.

First of all, it is important to understand that our remeshing algorithm works in an incremental fashion. Starting from the original mesh the algorithm iteratively improves the shape and size of the triangles.

The optimization is performed by applying a series of local mesh modifications based on edge splits, edge collapse, edge flips, as well as tangential smoothing. An optional step projects mesh vertices back to the original surface to stay closer to the input shape.

In case of _uniform_ or _isotropic_ remeshing, the goal of the optimization are equally sized triangles as close as possible to regular triangles. Alternatively, _adaptive_ remeshing employs a spatially varying sizing field that refines the mesh in regions of high curvature while using larger elements in flat regions.

## Uniform Remeshing

Uniform remeshing creates a uniform (isotropic) triangulation with a given uniform target edge length for the whole mesh. The remeshing processing is controlled by three parameters:

- `edge_length`: The target length each edge in the mesh should have after successful operation.
- `iterations`: The number of iterations to perform.
- `use_projection`: Enable projection of vertices back to the original input surface.

A good default value for `edge_length` is the mean edge length which can be computed by the pmp::mean_edge_length() function.

## Adaptive Remeshing

Adaptive remeshing creates an adaptive mesh with varying element size depending on the local curvature of the input mesh. The remeshing is controlled by the following parameters:

- `min_edge_length`: The minimum edge length.
- `max_edge_length`: The maximum edge length.
- `approx_error`: The maximum approximation error.
- `iterations`: The number of iterations.
- `use_projection`: Enable projection of vertices back to the original input surface.

## Preserving Sharp Features

Both uniform and adaptive remeshing functions preserve feature edges present in the input mesh. They can be either detected by using the pmp::detect_features() function or by using the `v:feature` boolean vertex property and the `e:feature` boolean edge property.

## Selections

The remeshing can be applied restricted to a subset of the mesh. Use the `v:selected` boolean property to choose the vertices to which the remeshing will be applied.

## Preserving Texture Coordinates

Both remeshing functions accept an optional final parameter `preserve_texture`. If it is set to `true` and the mesh stores a halfedge property named `h:tex` of type TexCoord, the texture mapping is preserved throughout the remeshing. With `preserve_texture` set to `false`, or when the property is absent, behavior is identical to the non-textured algorithm.

Faces are grouped into *texture charts*: two faces belong to the same chart when they are connected through interior edges whose two corner UVs (the UV stored on each halfedge at the vertex it points to) agree within \(10^{-6}\). An edge whose corner UVs differ is a texture seam, and mesh boundaries also separate charts. Chart membership is carried through every edge split, collapse, valence flip, tangential smoothing step and the final garbage collection. Faces from different charts are never merged, and no face can ever span a seam.

Charts are remeshed individually but with the full edge split, collapse, flip and smoothing machinery — neither vertices are locked wholesale nor are flips or smoothing disabled. The geometry of seams is handled conservatively:

- the positions of all original seam and boundary vertices are preserved;
- seam and boundary polylines may be subdivided (long seams follow the same edge-length rules as the interior), but the output edges cover the same polyline without crossing a corner, drifting off the line, or opening cracks;
- at a seam junction one geometric vertex keeps several distinct corner UVs, since texture coordinates remain a halfedge property.

UVs are always recomputed from the original surface snapshot taken at the start of the call. For every output corner, the closest point on the closest original triangle *of the corner's own chart* is found (ties go to the smallest original face index), and the three original corner UVs of that triangle are interpolated with the corresponding barycentric coordinates. A global nearest-face search over the whole mesh is therefore never used, so nearby shells, folded regions or overlapping UVs cannot pull corners into another chart. When back-projection is enabled (`use_projection=true`), geometry projection is restricted to the vertex's chart in the same way; seam and boundary vertices are projected onto their original polylines instead. With `use_projection=false` no geometric projection is added, but corner UVs are still transferred with the chart-local nearest-point rule.

The existing `v:feature`, `e:feature` and `v:selected` constraints remain in force. Texture preservation never turns a previously false feature flag permanently true, and all temporary bookkeeping properties are removed when the call returns.

## Troubleshooting

- Our implementation is not perfect. If you spot a problem, please file an [issue](https://github.com/pmp-library/pmp-library/issues).
- Degenerate faces in the input and sharp feature detection can lead to unexpected results.
- We've had cases when the projection to the original surface causes trouble. Try disabling this step of the algorithm.
- If you run into trouble: try with a single iteration; disable individual stages of the algorithm to nail down where things go wrong.
