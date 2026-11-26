[![Build Status](https://travis-ci.com/jdupuy/HalfedgeCatmullClark.svg?branch=master)](https://travis-ci.com/jdupuy/HalfedgeCatmullClark)
[![Build status](https://ci.appveyor.com/api/projects/status/sqogwscp9vscyo7s?svg=true)](https://ci.appveyor.com/project/jdupuy/halfedgecatmullclark)

This repository provides source code to reproduce some of the results of my paper ["A Halfedge Refinement Rule for Parallel Catmull-Clark Subdivision"](https://onrendering.com/).
The key contribution of this paper is to provide super simple algorithms to compute 
Catmull-Clark subdivision in parallel with support for semi-sharp creases. The algorithms are compiled in the C header-only library `CatmullClark.h`. In addition you will find a direct GLSL port of these algorithms in the 
`glsl/` folder. For various usage examples, see the `examples/` folder.

### License

Apart from the submodules folder, the code from this repository is released in public domain. You can do anything you want with them. You have no legal obligation to do anything else, although I appreciate attribution.

It is also licensed under the MIT open source license, if you have lawyers who are unhappy with public domain.

### Cloning

Clone the repository and all its submodules using the following command:
```sh
git clone --recursive git@github.com:jdupuy/HalfedgeCatmullClark.git
```

If you accidentally omitted the `--recursive` flag when cloning the repository you can retrieve the submodules like so:
```sh
git submodule update --init --recursive
```

### Sparse editing with `cc_EditPlan`

The single-header `cc_EditPlan.h` (`CC_EDIT_IMPLEMENTATION`, in the same style
as `CatmullClark.h`) supports interactive control-cage dragging without
re-subdividing the whole surface on every move. Given a fixed topology,
subdivision depth (1–5) and crease configuration, `ccs_EditPlanCreate` deep
copies the cage (points, UVs, halfedges, vertex/edge/face mappings and creases),
runs the full initial `ccs_Refine_Gather`, and precomputes the per-level face /
edge / vertex position dependencies together with a reverse index. Each
`ccs_EditPlanUpdate` then propagates a batch of absolute control-point moves
through exactly the rows they can affect, in the same face-to-edge-to-vertex
order as the Gather path; the result is numerically equivalent to re-running
`ccs_Refine_Gather` on the edited cage (within a relative tolerance of `2e-5`).

Memory and ownership boundary:

- A plan owns a private `cc_Mesh` copy and a private `cc_Subd`
  (`ccs_EditPlanSubd` returns a read-only pointer). The source cage may be
  mutated or released immediately after creation; plans never share state, so
  two plans over the same cage, or interleaved drags, stay independent.
- Memory scales with the fully subdivided surface (the stored positions,
  halfedges, creases and per-level reverse index). Forward dependency scratch is
  released once the reverse index is built. Nothing is rebuilt per update, and
  per-update work and temporary allocation grow only with the edit footprint;
  an untouched connected component is never visited or rewritten.
- The plan is valid only while topology, UVs and creases stay fixed. Changing
  the number or connectivity of vertices, faces or edges, the UVs, or any crease
  is an invalidation event: release and recreate the plan. Only control-point
  positions are editable through `ccs_EditPlanUpdate`.
- A failed (non-zero) update — out-of-range or duplicated ids, non-finite
  positions, or a non-empty batch with NULL arrays — changes no stored position
  and leaves the plan on its last successful state, so the next valid batch
  accumulates correctly. Writing an unchanged position performs no geometry
  work and reports zero rows in `cc_EditStats`.

A dependency-free C demo lives in `examples/edit_plan_drag.c`; the regression
suite is in `checks/test_edit_plan.c` and runs through
`bash task_support/run.sh target`.