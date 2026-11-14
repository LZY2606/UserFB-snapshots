# CinoLib
![MacOS](https://github.com/mlivesu/cinolib/actions/workflows/macos-build.yml/badge.svg?event=push)
![Ubuntu](https://github.com/mlivesu/cinolib/actions/workflows/ubuntu-build.yml/badge.svg?event=push)
![Linux](https://github.com/mlivesu/cinolib/actions/workflows/windows-build.yml/badge.svg?event=push)

CinoLib is a C++ library for processing polygonal and polyhedral meshes. It supports surface meshes made of triangles, quads or general polygons as well as volumetric meshes made of tetrahedra, hexahedra or general polyhedra. 

A distinctive feature of the library is that all supported meshes inherit from a unique base class that implements their common traits, permitting to deploy algorithms that operate on _abstract_ meshes that may be any of the above. This allows to implement algorithms just once and run the same code on any possible mesh, thus avoiding code duplication and reducing the debugging effort.

:boom:***CinoLib received the [Symposium on Geometry Processing Software Award](http://awards.geometryprocessing.org) in 2024***:boom:

<p align="center"><img src="cinolib_rep_image.png" width="500"></p>

## Positioning
Github hosts a whole variety of great academic libraries for mesh processing. If you do mainly geometry processing on triangle meshes, then tools like [libigl](https://libigl.github.io), [GeometryCentral](https://geometry-central.net) or [VCG](https://github.com/cnr-isti-vclab/vcglib) may be what you want. If you are interested in rendering, [Yocto/GL](https://github.com/xelatihy/yocto-gl) is extremely fast and implements many relevant algorithms. [OpenMesh](https://www.graphics.rwth-aachen.de/software/openmesh/), [CGoGN2](https://github.com/cgogn/CGoGN_2/) and [PMP](http://www.pmp-library.org) have a slightly broader scope and can handle general polygonal surfaces. For volumes, tiny portions of [libigl](https://libigl.github.io) and [GeometryCentral](https://geometry-central.net) offer rudimentary support for specific solid elements such as tetrahedra or hexahedra, but most of the library is focused on surfaces. Conversely, [OpenVolumeMesh](https://www.graphics.rwth-aachen.de/software/openvolumemesh/) and [CGoGN3](https://github.com/cgogn/CGoGN_3/) are focused on volumes and can operate on general polyhedral elements, but they do not support surface meshes. To the best of my knowledge, only [Geogram](https://github.com/BrunoLevy/geogram) has a unified data structure that can host both surface and volume elements, but it only supports hexahedra, tetrahedra, prisms and pyramids as volume cells. Differently from all these alternatives, CinoLib has a unique data structure that is designed to host any type of surface and volumetric element. If this comes handy to you, I am not aware of any existing alternative. Data structures based on combinatorial maps have the potential to offer similar capabilities, but these are not fully implemented yet ([#171](https://github.com/mlivesu/cinolib/issues/171)). Note that CinoLib trades generality for efficiency, hence all this flexibilty comes at a cost. Many optimizations that are possible when one operates on a restricted set of mesh elements cannot be applied here, especially memory-wise, where generic elements with an unpredictable number of vertices edges and faces demand the use of dynamic allocators. For this reason, in some cases CinoLib may be sligthly less efficient than the aforementioned alternatives.


## Getting started
CinoLib is header only. It does not need to be installed, all you have to do is to clone the repo with
```
git clone https://github.com/mlivesu/cinolib.git
```
and include in your C++ application the header files you need. For small projects this could already be done by instructing the compiler on where to find the library sources, e.g. with the `-I` option. For more convoluted projects it is suggested to rely on a building system such as [CMake](https://cmake.org), that can also handle optional external dependencies and compilation flags or symbols.

## Build a sample project (with CMake)
Here is an example of a toy program that reads a triangle mesh and displays it on a window
```c++
#include <cinolib/meshes/drawable_trimesh.h>
#include <cinolib/gl/glcanvas.h>

int main()
{
    using namespace cinolib;
    DrawableTrimesh<> m("bunny.obj");
    GLcanvas gui;
    gui.push(&m);
    return gui.launch();
}
```
and this is the `CMakeLists.txt` that can be used to compile it
```cmake
cmake_minimum_required(VERSION 3.2)
project(cinolib_demo)
add_executable(${PROJECT_NAME} main.cpp)
set(CINOLIB_USES_OPENGL_GLFW_IMGUI ON)
find_package(cinolib REQUIRED)
target_link_libraries(${PROJECT_NAME} cinolib)
```
Compiling should be as easy as opening a terminal in the folder containing the two files above and type
```
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -Dcinolib_DIR=<absolute-path-to-cinolib>
make
```
Note that for the rendering part CinoLib uses [GLFW](https://www.glfw.org), which will be automatically installed and linked by the script `cinolib-config.cmake`, contained in the main directory of the library. The same script can automatically download and install any other external dependency, meaning that if you want to access a functionality that depends on some external library `XXX`, all you have to do is setting to `ON` a cmake variable that looks like `CINOLIB_USES_XXX`. 
Valid options are:
* `CINOLIB_USES_OPENGL_GLFW_IMGUI`, used for rendering with OpenGL
* `CINOLIB_USES_TRIANGLE`, used for polygon triangulation
* `CINOLIB_USES_TETGEN`, used for tetrahedralization
* `CINOLIB_USES_SHEWCHUK_PREDICATES`, used for exact geometric tests on input points
* `CINOLIB_USES_INDIRECT_PREDICATES`, used for exact geometric tests on implicit points
* `CINOLIB_USES_GRAPH_CUT`, used for graph clustering
* `CINOLIB_USES_BOOST`, used for 2D polygon operations (e.g. thickening, clipping, 2D booleans...)
* `CINOLIB_USES_VTK`, used just to support VTK file formats
* `CINOLIB_USES_SPECTRA`, used for matrix eigendecomposition
* `CINOLIB_USES_CGAL_GMP_MPFR`, used for rational numbers with a lazy kernel

## GUI
CinoLib is designed for researchers in computer graphics and geometry processing that need to quickly realize software prototypes that demonstate a novel algorithm or technique. In this context a simple OpenGL window and a side bar containing a few buttons and sliders are often more than enough. The library uses [ImGui](https://github.com/ocornut/imgui) for the GUI and [GLFW](https://www.glfw.org) for OpenGL rendering. Typical visual controls for the rendering of a mesh (e.g. shading, wireframe, texturing, planar slicing, ecc) are all encoded in two classes `cinolib::SurfaceMeshControls` and `cinolib::VolumeMeshControls`, that operate on surface and volume meshes respectively. To add a side bar that displays all such controls one can modify the sample progam above as follows:
```c++
#include <cinolib/meshes/drawable_trimesh.h>
#include <cinolib/gl/glcanvas.h>
#include <cinolib/gl/surface_mesh_controls.h>

int main()
{
    using namespace cinolib;
    DrawableTrimesh<> m("bunny.obj");
    GLcanvas gui;
    SurfaceMeshControls<DrawableTrimesh<>> mesh_controls(&m, &gui);
    gui.push(&m);
    gui.push(&mesh_controls);
    return gui.launch();
}
```
The canvas can host multiple mesh controls, ideally one of each mesh in the scene. Additional GUI elements that may be necessary to control the application (e.g. the parameters of your algorithm) can be added by implementing a dedicated callback:
```c++
#include <cinolib/meshes/drawable_trimesh.h>
#include <cinolib/gl/glcanvas.h>
#include <cinolib/gl/surface_mesh_controls.h>

int main()
{
    using namespace cinolib;
    DrawableTrimesh<> m("bunny.obj");
    GLcanvas gui;
    SurfaceMeshControls<DrawableTrimesh<>> mesh_controls(&m, &gui);
    gui.push(&m);
    gui.push(&mesh_controls);
    float val = 1.0, min = 0.0, max = 10.0;
    gui.callback_app_controls = [&]()
    {
        if(ImGui::Button("MyButton"))
        {
            // button clicked: do something
        }
        if(ImGui::SliderFloat("MySlider", &val, min, max))
        {
            // slider moved: do something
        }       
    };
    return gui.launch();
}
```
The full list of callbacks exposed by `GLcanvas` to interact with user events (e.g. for scene navigation, mouse picking, ecc) are:
* `callback_key_pressed(int key, int modifiers)`
* `callback_mouse_left_click(int modifiers)`
* `callback_mouse_left_click2(int modifiers)` => double click
* `callback_mouse_right_click(int modifiers)` 
* `callback_mouse_right_click2(int modifiers)` => double click
* `callback_mouse_moved(double x_pos, double y_pos)`
* `callback_mouse_scroll(double x_offset, double y_offset)`
* `callback_app_controls(void)`


## Other examples
A tutorial with detailed info on how to use the library is under developement. In the meanwhile, you can explore the [**examples**](https://github.com/mlivesu/cinolib/tree/master/examples#examples)  folder, which contains a constantly growing number of sample projects that showcase the core features of the library, and will be the backbone of the forthcoming tutorial.

## Hexahedral sheet subdivision

`Hexmesh::subdivide_sheets(const std::vector<uint> & marked_edges)` refines a hexahedral mesh along topological sheets instead of globally refining every cell. A fully worked headless sample is in `examples/49_hex_sheet_subdivision`.

### Sheet propagation

Each marked edge seeds a sheet. From a selected edge the selection propagates across every incident quadrilateral to its **topologically opposite edge** (the unique quad edge that shares no vertex with it), and the process repeats until the closure is reached. The relation is purely combinatorial: world-space parallelism is never used, so it works with curved meshes, rotated local eight-vertex numberings, singular interior edges and sheets that return to the same cell through a different local direction. Repeated edge ids count once, an empty selection leaves the mesh unchanged, and an out-of-range id throws `std::invalid_argument`. Selection order and duplicates never change the result.

A hexahedron hit by one, two or three sheet directions is split into respectively **two, four or eight** sub hexahedra, arranged as the corresponding 1×2×2, 2×2×1 or 2×2×2 midpoint grid. Cells not hit by any sheet are kept untouched, including whole connected components: a component is never uniformly octasplit just because another component was refined.

### Returned mapping

The call returns a `HexSheetSubdivision`:

* `parent_poly` has one entry per output hexahedron, giving the id of the cell it was refined from.
* `vertex_sources` has one entry per output vertex. Each entry is a list of `(input vertex id, non negative weight)` pairs whose weights sum to one. Original vertices map to a single source of weight `1`; new edge midpoint nodes have two sources of weight `1/2`, quad face center nodes four sources of weight `1/4`, and cell center nodes eight sources of weight `1/8`. The actual coordinates (and the interpolated `uvw`/`color` fields) are exactly the weighted sum of the pre-call vertices.

All ids in both structures refer to the numbering in effect **before** the call; the mesh itself is rewritten in place. Original vertices keep their ids, positions and all attributes, and new vertices are appended after them in deterministic order (edge nodes by input edge id, then face centers by input face id, then cell centers by input cell id). Parents are processed in ascending id and children are emitted in lexicographic grid order. Shared edges, quads and interfaces always reuse the same topological vertex and face (no coordinate welding, no hanging nodes); the two sides of every interior face receive opposite windings.

### Attribute propagation

Each child hexahedron copies the user attributes of its parent; derived quality is recomputed. Sub faces created on an input face copy that face's user attributes, while newly created internal faces use the attribute type defaults. New vertices interpolate `uvw` and `color` with the same weights as their coordinates; every other vertex field (including fields added by custom attribute types) keeps its default value. The method is a template on the same `M,V,E,F,P` attribute types as `Hexmesh`.

### Valid geometry scope

The input is assumed to be a finite, conforming, non degenerate hexahedral volume mesh whose per-cell trilinear maps have a positive Jacobian throughout the parameter cube (inverted cells are neither validated nor repaired). Under that assumption every output cell remains a valid positively oriented hexahedron: adjacency, surface flags, face/vertex normals, bounding box and per-cell quality are all rebuilt, so `poly_sheet`, `poly_volume` and the existing mesh queries work directly on the result. Refinement can be applied repeatedly; each call takes the mesh as it exists at that moment and returns mappings relative to that numbering.

Selecting every edge is equivalent to the uniform midpoint subdivision: `Hexmesh::poly_subdivide(hex_to_grid_2x2x2)` and both `subdivision_midpoint` entry points (with and without the three `std::map<uint,uint>` node maps `edge_verts`, `face_verts`, `poly_verts`) use the same orientation and attribute rules and yield the same child geometry and connectivity. All midpoint entry points support passing the same object as input and output.

## Contributors
Marco Livesu is the creator and lead developer of the library. CinoLib has also received contributions from: Daniela Cabiddu and Tommaso Sorgente (CNR IMATI), Claudio Mancinelli and Enrico Puppo (University of Genoa), Chrystiano Araújo (UBC), Thomas Alderighi (CNR ISTI), Fabrizio Corda, Gianmarco Cherchi and Federico Meloni (University of Cagliari).

## Citing us
If you use CinoLib in your academic projects, please consider citing the library using the following 
BibTeX entry:

```bibtex
@article{cinolib,
  title   = {cinolib: a generic programming header only C++ library for processing polygonal and polyhedral meshes},
  author  = {Livesu, Marco},
  journal = {Transactions on Computational Science XXXIV},
  series  = {Lecture Notes in Computer Science},
  editor  = {Springer},
  note    = {https://github.com/mlivesu/cinolib/},
  year    = {2019},
  doi     = {10.1007/978-3-662-59958-7_4}}
```

## Acknowledgment
The software collected in CinoLib spans across a large period of time, starting from the beginning of my PhD to today. Since 2015, this work has been partly supported by various research projects, such as
* [ProMED](http://arm.mi.imati.cnr.it/imati/detail_pages.php?language=ENG&view=GEN&voice_code=PRG&fcode=WHA&ref_idk=PJ-176)
* [CHANGE](https://cordis.europa.eu/project/rcn/204834/en)
* [CAxMan](https://cordis.europa.eu/project/id/680448)
