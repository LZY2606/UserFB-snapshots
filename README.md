# Mesh voxelizer
_Header only mesh voxelizer in ANSI C_

![](images/capture.png)

**About**

Converts meshes and performs triangle-cube intersection to output a voxelized mesh.

**How to use**

Include the header only file as following once:
```c
#define VOXELIZER_IMPLEMENTATION
#include "voxelizer.h"
```

To generate a voxelized mesh, create an instance of `vx_mesh_t` and initialize its
vertices and indices like this:

```c
vx_mesh_t* mesh;
vx_mesh_t* result;

mesh = vx_mesh_alloc(nvertices, nindices);

// Add vertices and indices from the original mesh you want to voxelize
// [...]

// Precision factor to reduce "holes" artifact
float precision = 0.01;

// Run voxelization
result = vx_voxelize(mesh, 0.025, 0.025, 0.025, precision);

vx_mesh_free(result);
vx_mesh_free(mesh);
```

**Geometry rules**

Voxel centers sit on the integer lattice `(kx*sx, ky*sy, kz*sz)` anchored at
the origin, with positive per-axis voxel sizes. A cell is occupied if and only
if a positive-area input triangle shares at least one point with the closed
box centered on the cell whose per-axis half width is `size/2 + precision`
(`precision` is a non-negative world-space length and may exceed one voxel).
Contact on a single edge or point already occupies the cell. Truly degenerate
triangles (collinear or repeated vertices, zero area) are skipped, but any
positive-area triangle is kept no matter how small it is, and distinct lattice
cells are never merged. As a result the occupancy set does not depend on
triangle order, corner rotation or winding, and is preserved when a surface is
re-triangulated through coplanar interior points.

**Color rules**

Per-vertex colors define a linear color field over each triangle. For every
cell, each surface contributes the area `A` of the triangle clipped against
the cell's closed intersection box together with the integral `I` of its
linear color field over that clipped region (clipped vertices inherit the
interpolated field). The cell color is `sum(I) / sum(A)` over all surfaces:
coincident surfaces are weighted independently (no occlusion), no color space
conversion is applied, and finite colors outside `[0, 1]` are integrated
without clamping. Cells touched only on an edge or point have zero total area
and are still emitted, with a black color. The point cloud cell colors and the
eight cube vertices of `vx_voxelize` use the same accumulation.

**Uncolored meshes**

A mesh with `colors == NULL` produces the same occupancy set as the colored
path for the same geometry, and the outputs keep `colors == NULL`.
