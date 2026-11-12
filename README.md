# tinyply 3.0

[![Release is 3.0](http://img.shields.io/badge/release-3.0-blue.svg?style=flat)](https://raw.githubusercontent.com/ddiakopoulos/tinyply/master/source/tinyply.h)
[![License is Unlicense](http://img.shields.io/badge/license-Unlicense-blue.svg?style=flat)](http://unlicense.org/)
[![CMake on multiple platforms](https://github.com/ddiakopoulos/tinyply/actions/workflows/cmake-multi-platform.yml/badge.svg)](https://github.com/ddiakopoulos/tinyply/actions/workflows/cmake-multi-platform.yml)

A single-header, zero-dependency (except the C++ STL) __public domain__ implementation of the PLY mesh file format. An overview and definition of the file format is available [here](http://paulbourke.net/dataformats/ply/). This format is often used in the computer vision and graphics communities for its relative simplicity, ability to support arbitrary mesh attributes, and binary modes. Since ~2023 it has been used one of the primary interchange formats between applications & pipelines for gaussian splatting. Most famously, PLY is used to distribute models in the [Stanford 3D Scanning Repository](http://graphics.stanford.edu/data/3Dscanrep/), including the original bunny. 

The library is written in C++17 and requires a recent compiler. Tinyply supports importing and exporting PLY files in both binary and ascii formats. Tinyply supports filesizes >= 4gb and can read big-endian binary files (but not write them). Recently in the 3.0 release, tinyply adds a fast-path parser for the most common import scheme (binary little-endian) when specific conditions are met. 

## Getting Started

The project comes with a simple example program demonstrating a circular write / read and all of the major API functionality.

## Writing variable-length (ragged) lists

Many polygon meshes mix triangles and quads in a single `face` element, where `vertex_indices`
and the per-corner `texcoord` lists have a different length on every row. The fixed-count
`add_properties_to_element` cannot express such data, because it assigns one `listCount` to
every row. For these meshes tinyply provides an offset-based constructor:

```cpp
void PlyFile::add_list_property_to_element(
    const std::string & elementKey,
    const std::string & propertyKey,
    Type valueType,
    size_t count,
    const size_t * offsets,     // count + 1 entries, in scalar elements (not bytes)
    const uint8_t * data,
    size_t dataBytes,
    Type listType);
```

Row *i* spans `data[offsets[i] .. offsets[i+1])`. Each list property carries its own offsets,
so two faces can have index offsets `[0, 3, 7]` and texcoord offsets `[0, 6, 14]`. The
properties coexist with fixed per-row data (such as a per-face `material_index` or per-vertex
coordinates added through the legacy interface) and are written in registration order. Empty
rows are legal, including an empty list sandwiched between non-empty rows; `count == 0` still
takes a single zero offset. `offsets[0]` must be zero, the offsets must be non-decreasing,
`offsets[count] * sizeof(valueType)` must equal `dataBytes`, and every row length must fit the
positive range of `listType` (one of the six signed/unsigned integer types). Invalid arguments
throw `std::invalid_argument` and leave the `PlyFile` unchanged.

### Ownership: the two writing interfaces differ

* `add_properties_to_element` **borrows** the caller's buffer. The `PlyFile` keeps the raw
  pointer and does not copy or take ownership, so the caller must keep the backing memory alive
  until `write` returns. This behavior is unchanged.
* `add_list_property_to_element` **takes ownership of copies**. On success the library copies
  both the payload and the offsets; the caller may modify or free the source arrays immediately
  after the call without affecting what gets written.

The same `PlyFile` can be written repeatedly with `write(os, false)` (ascii) and
`write(os, true)` (little-endian binary); internal cursors are reset on every call, so writing
ascii, then binary, then ascii again produces identical list segmentation each time. Big-endian
writing is not supported.

### Editing a mesh and re-exporting it

`source/polygon_subset.h` provides `tinyply_extras::extract_polygon_subset`, a reusable helper
that takes decoded vertex channels, ragged face indices and per-corner texcoords (each with its
own offsets), per-face materials, and an ordered list of face ids to keep. It renumbers the
referenced vertices by first appearance, remaps every numeric vertex channel through that
mapping, preserves the original corner order and UV correspondence, and returns the new-to-old
vertex mapping (unreferenced vertices are dropped). The companion
`add_polygon_subset_to_ply` writes the result through `add_list_property_to_element`, so the
export does not depend on the original file object. Invalid selections (duplicate or
out-of-range face ids, out-of-range vertex references, texcoord counts that are not twice the
corner counts) throw `std::invalid_argument` without producing a partial result. The example
program wires this into a full edit-and-export pass over a generated five-vertex, triangle-plus-
quad mesh.

The regression suite in `source/ragged_export_tests.cpp` covers the ragged construction path,
including empty lists, zero rows, the int8 127/128 length boundary, bad offsets and byte
counts, duplicate properties, recovery after a rejected call, source-array mutation after a
successful call, and the legacy fixed-triangle interface. It verifies the output with both a
tinyply round-trip and an independent little-endian byte / ascii token decoder. Build and run it
from the repository root:

```
bash source/check_ragged_export.sh
```

## In The Wild

Since 2015, tinyply has been used in thousands of projects including pointcloud tools, raytracers, synthetic data renderers, computational geometry libraries, and more. It's even listed by the [US Library of Congress](https://www.loc.gov/preservation/digital/formats/fdd/fdd000501.shtml) as a utility for archival preservation of digital media. A few other notable projects are highlighted below: 

* [libigl](https://libigl.github.io/), a robust computational geometry library from UoT professors Alec Jacobson and Daniele Panozzo.
* [Maplab](https://github.com/ethz-asl/maplab) from ETH Zürich, a research-oriented visual-inertial mapping framework. 
* [glChAoS.P](https://github.com/BrutPitt/glChAoS.P) from Michele Morrone, a rendering sandbox for 3D strange attractors.
* [Cilantro](https://github.com/kzampog/cilantro), a robust and featureful C++ library for working with pointcloud data. 
* [ScanNet](http://www.scan-net.org/), an RGB+D dataset of 2.5 million views across 1500 scans.
* [PlaneRCNN](https://github.com/NVlabs/planercnn), 3D plane detection via single-shot images from NVIDIA Research
* [KNOSSOS](https://knossos.app/), a framework to visualize and annotate 3D image data (neural morphology and connectivity). 
* [fVDB](https://github.com/openvdb/fvdb-core), a Python library for building applications using NanoVDB on the GPU in PyTorch.

tinyply not what you're looking for? tinyply trades some performance for simplicity and flexibility. For domain-specific uses (e.g. where your application does not need to handle arbitrary user-fed PLY files), there may be other alternatives. For more, please check out the following benchmarks:

* Vilya Harvey's [ply-parsing-perf](https://github.com/vilya/ply-parsing-perf)
* Maciej Halber's [ply_io_benchmark](https://github.com/mhalber/ply_io_benchmark)

## Past Versions

* `version 2.0` is an API re-write to support later improvements towards variable length lists. One notable change is that tinyply now produces and consumes structured byte arrays, with type information held as metadata.
* `version 2.1` contained minor bugfixes and speed improvements.
* `version 2.2` is a rewrite of the inner read/write loop. Compared to `version 2.0`, this version reads and writes binary about five times faster. When a list size hint is given for reading, the performance is approximately comparable to rply. 
* `version 2.3` contains minor bugfixes and performance improvements. 
* `version 3.0` is a little less tiny, but it's a major performance upgrade and adds support for variable length lists alongside better error handling, 

## License

This software is in the public domain. Where that dedication is not recognized, you are granted a perpetual, irrevocable license to copy, distribute, and modify this file as you see fit. If these terms are not suitable to your organization, you may choose to license it under the terms of the 2-clause simplified BSD. 
