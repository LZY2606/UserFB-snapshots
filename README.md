# Introduction

[![build](https://github.com/pmp-library/pmp-library/workflows/build/badge.svg)](https://github.com/pmp-library/pmp-library/actions?query=workflow%3Abuild)
[![Coverage Status](https://coveralls.io/repos/github/pmp-library/pmp-library/badge.svg?branch=master)](https://coveralls.io/github/pmp-library/pmp-library?branch=main)
[![Latest Release](https://img.shields.io/github/v/release/pmp-library/pmp-library?sort=semver)](https://github.com/pmp-library/pmp-library/releases/latest)
[![DOI](https://zenodo.org/badge/105374301.svg)](https://zenodo.org/doi/10.5281/zenodo.10866531)

The Polygon Mesh Processing Library is a modern C++ open-source library for processing and visualizing polygon surface meshes. Its main features are:

- An efficient and easy-to-use mesh data structure
- Standard algorithms such as decimation, remeshing, subdivision, or smoothing
- Ready-to-use viewers
- Seamless cross-compilation to JavaScript ([demo](https://www.pmp-library.org/demos/mpview.html))

## Get Started

Clone the repository:

```sh
git clone https://github.com/pmp-library/pmp-library.git
```

Configure and build:

```sh
cd pmp-library && mkdir build && cd build && cmake .. && make
```

Run the mesh processing app:

```sh
./mpview ../data/off/bunny.off
```

Build your own tool:

```cpp
#include <pmp/surface_mesh.h>
#include <pmp/io/io.h>

int main()
{
    pmp::SurfaceMesh mesh;
    pmp::read(mesh,"input.obj");
    // .. do awesome things with your mesh
    pmp::write(mesh,"output.obj");
}
```

## Intrinsic Transfer of Scalar Fields

The `pmp::IntrinsicTransfer` class (in
`src/pmp/algorithms/intrinsic_transfer.h`) provides an intrinsic edge-flip
editing session for continuous piecewise linear scalar fields living on a
triangle surface, e.g. painted thickness values.

The session owns a private deep copy of the input mesh. Its intrinsic
metric is defined by the 3D edge lengths of the input mesh: flipping an
edge unfolds the two incident triangles into the plane and replaces the
diagonal of the resulting quadrilateral, so the new edge length is the
unfolded distance, not the 3D chord of the new endpoints. Vertex
identities, positions, and custom properties are preserved.

A source field is given by one value per initial vertex slot
(`vertices_size()` of the input mesh) and is always interpreted as the
piecewise linear field on the initial surface. `project()` transfers it
onto the current triangulation by solving the consistent mass matrix
system `M_current u = b`, where `b` integrates the source field against
the current hat functions through the exact common refinement of the
initial and current triangulations. Repeated calls do not accumulate, and
flipping an edge back restores the original field.

```cpp
#include <pmp/algorithms/intrinsic_transfer.h>

pmp::IntrinsicTransfer session(mesh);
auto e = mesh.find_edge(v0, v1);
if (session.flip(e))
    std::cout << "new intrinsic length: " << session.edge_length(e) << "\n";
// transfer per-vertex source values onto the current triangulation
std::vector<double> u = session.project(source_values);
```

## Read the Docs

The [user guide](https://www.pmp-library.org/guide.html) contains all you need to get started using PMP, including a [tutorial](https://www.pmp-library.org/tutorial.html) covering mesh processing basics.

## Contribute

Contributions to PMP are welcome! There are many ways you can help: Report any [issues](https://github.com/pmp-library/pmp-library/issues) you find, help to improve the documentation, join our [discussions](https://github.com/pmp-library/pmp-library/discussions) forum, or [contribute](https://www.pmp-library.org/contributing.html) new code.

## Acknowledge

If you are using PMP for research projects, please acknowledge its use by referencing

```tex
@software{pmp23,
  author = {Sieger, Daniel and Botsch, Mario},
  title = {{The Polygon Mesh Processing Library}},
  year = {2023},
  month = aug,
  version = {3.0.0},
  doi = {10.5281/zenodo.10866532},
  url = {https://github.com/pmp-library/pmp-library}
}
```

We acknowledge that PMP evolved from our previous work on [Surface_mesh](http://dx.doi.org/10.1007/978-3-642-24734-7_29) and [OpenMesh](https://pub.uni-bielefeld.de/record/1961694).

## License

PMP is provided under a [MIT license](https://github.com/pmp-library/pmp-library/blob/master/LICENSE.txt), allowing for both open-source and commercial usage.
