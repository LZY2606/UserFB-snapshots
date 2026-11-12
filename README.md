[![Discord Chat](https://img.shields.io/discord/480318777943392266.svg)](https://discord.gg/RgFybs6) 
[![License](http://img.shields.io/:license-mit-blue.svg)](http://doge.mit-license.org)
![build status](https://github.com/nem0/openfbx/actions/workflows/build.yml/badge.svg)

# OpenFBX

Lightweight open source FBX importer. Used in [Lumix Engine](https://github.com/nem0/lumixengine) and [Flax Engine](https://flaxengine.com/). It's an *almost* full-featured importer. It can load geometry (with uvs, normals, tangents, colors), skeletons, animations, blend shapes, materials, textures, cameras and lights.

[UFBX](https://github.com/bqqbarbhg/ufbx) is similar project in C.
Using [libdeflate](https://github.com/ebiggers/libdeflate) for decompression.

## Use the library in your own project

Note: It's recommended to be familiar with fbx format to use this library, you can read about it more [here](http://help.autodesk.com/view/FBX/2017/ENU/?guid=__files_GUID_F194000D_5AD4_49C1_86CC_5DAC2CE64E97_htm).

1. add files from [src](https://github.com/nem0/OpenFBX/tree/master/src) to your project
2. use

See [demo](https://github.com/nem0/OpenFBX/blob/master/demo/main.cpp#L203) as an example how to use the library.
See [Lumix Engine](https://github.com/nem0/LumixEngine/blob/master/src/renderer/editor/fbx_importer.cpp) as more advanced use case.

Alternatively, CMake support is provided by community but it's not supported by me - @nem0.

## Transform inheritance and poses

`Object::getGlobalTransform()` honors the FBX `InheritType` property
(`Properties70` and legacy `Properties60`):

* `0` RrSs (default when the property is absent): the child rotation is applied
  *before* the inherited scale,
* `1` RSrs: the child rotation is applied *after* the inherited scale (plain
  parent-world × local matrix multiplication),
* `2` Rrs: only the direct parent's own local scale is compensated; scale
  inherited from earlier ancestors is preserved.

All three modes use the Autodesk "Transformations" residual formulas. The local
transform definition (Euler order, Pre/PostRotation, rotation/scaling pivots and
offsets) is unchanged. Translation is always transformed by the parent's complete
world matrix — including its scale — for every mode, and matrices carrying
anisotropic scale are kept opaque (no column-length TRS reassembly, so shear is
preserved).

`src/ofbx_pose.h` adds a read-only pose/skinning API built on the same
inheritance engine, so static queries, animated poses and skinning can never
drift apart:

```cpp
#include "ofbx.h"
#include "ofbx_pose.h"

// layer == nullptr evaluates the static pose; otherwise the Lcl
// Translation/Rotation/Scaling curve nodes in the layer are sampled at `seconds`,
// with static values and curve-node defaults filling the gaps.
std::vector<ofbx::NodePose> pose;
if (!ofbx::evaluatePose(*scene, layer, seconds, pose)) { /* unsupported input */ }

// One matrix per cluster, in the Skin's cluster order. Each matrix accepts the
// raw Geometry control points and already contains the Mesh geometric transform
// once (do not apply getGeometricMatrix() again).
std::vector<ofbx::SkinPose> skins;
if (!ofbx::buildSkinMatrices(*mesh, pose, skins)) { /* singular bind matrix, ... */ }
```

The supported domain is finite TRS with strictly positive local scaling; zero
scale, reflections and hand authored shear are not required. On unsupported
input, unknown inheritance values, non-finite time, a layer from another scene,
a missing cluster link or a non-invertible `TransformLink`, the new entry points
return `false` and leave their output untouched. The legacy static interface
keeps its old plain-matrix-multiplication behavior on those same inputs, so
files that previously loaded continue to work.

## Compile demo project

1. download source code
2. execute [projects/genie_vs19.bat](https://github.com/nem0/OpenFBX/blob/master/projects/genie_vs19.bat)
3. open projects/tmp/vs2019/OpenFBX.sln in Visual Studio 2019
4. compile and run

Demo is windows only. Library is multiplatform.

![ofbx](https://user-images.githubusercontent.com/153526/27876079-eea3c872-61b5-11e7-9fce-3a7c558fb0d2.png)
