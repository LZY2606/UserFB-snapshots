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

## As-rigid-as-possible deformation

`pmp::ArapDeformation` (in `pmp/algorithms/arap.h`) implements the
local/global ARAP surface deformation of Sorkine and Alexa. Handles are
hard position constraints and every local rotation is restricted to
proper 3D rotations (determinant +1). The minimized energy is

```
E(x,R) = sum_i sum_{j neighbor of i} w_ij *
         || (x_i - x_j) - R_i (p_i - p_j) ||^2
```

where `p` are the coordinates recorded as the rest pose when the
`ArapDeformation` is constructed. The edge weight `w_ij` is half the sum
of the cotangents opposite to the edge in its incident triangles
(boundary edges use one side), clamped to non-negative values. The
weights are computed once from the rest pose and never recomputed.

```cpp
#include <pmp/algorithms/arap.h>

pmp::ArapDeformation arap(mesh); // stores the current coordinates as rest pose

pmp::ArapHandle handle{vertex, target_position};
arap.set_handles({handle});      // replaces the whole handle set

// energies at the starting state and after each global update
std::vector<double> energies = arap.deform(10);

// the same object supports repeated drags; the rest pose never changes,
// and an unchanged free vertex set reuses the sparse factorization
arap.set_handles({handle, another_handle});
energies = arap.deform(10);
size_t factorizations = arap.factorization_count();
```

Only finite coordinates, oriented 2-manifold triangle meshes with
non-zero area faces are accepted; invalid input raises
`pmp::InvalidInputException`, while a failed factorization or solve
raises `pmp::SolverException` without writing partial coordinates.
After constructing an `ArapDeformation`, the caller must not change the
mesh topology and must leave all coordinate edits to the object, which
is intended to be short-lived and must not outlive the mesh.

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
