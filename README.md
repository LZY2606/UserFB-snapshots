[![Build Status](https://travis-ci.com/jdupuy/HalfedgeCatmullClark.svg?branch=master)](https://travis-ci.com/jdupuy/HalfedgeCatmullClark)
[![Build status](https://ci.appveyor.com/api/projects/status/sqogwscp9vscyo7s?svg=true)](https://ci.appveyor.com/project/jdupuy/halfedgecatmullclark)

This repository provides source code to reproduce some of the results of my paper ["A Halfedge Refinement Rule for Parallel Catmull-Clark Subdivision"](https://onrendering.com/).
The key contribution of this paper is to provide super simple algorithms to compute 
Catmull-Clark subdivision in parallel with support for semi-sharp creases. The algorithms are compiled in the C header-only library `CatmullClark.h`. In addition you will find a direct GLSL port of these algorithms in the 
`glsl/` folder. For various usage examples, see the `examples/` folder.

### Sparse edit plans (`cc_EditPlan.h`)

For interactive control-cage editing, the companion single-header library
`cc_EditPlan.h` (implementation macro: `CC_EDIT_IMPLEMENTATION`) provides a
reusable sparse edit plan. `ccs_EditPlanCreate(cage, depth)` (depth 1--5)
deep-copies the cage (positions, UVs, halfedges, mappings and creases),
performs the full initial subdivision, and precomputes per-row position
dependencies from the frozen topology and crease rules. After creation the
source cage may be edited or released; the plan is unaffected.
`ccs_EditPlanUpdate` then applies a batch of absolute control-point
positions and only re-evaluates the position rows that actually depend on
the edited vertices, level by level; the result matches a full
`ccs_Refine_Gather` of the latest cage. `ccs_EditPlanSubd` returns the
plan-owned, read-only `cc_Subd`. See `examples/edit_plan_preview.c` for a
complete program.

Memory and invalidation boundaries:

- A plan owns its private cage copy, the full `cc_Subd` (all levels), and
  the dependency stencils plus reverse indices: memory is linear in the
  cumulative number of position rows over all depths, with a constant
  factor of a few dozen bytes per row. It is allocated once at creation;
  updates perform no allocation.
- Topology, UVs, crease sharpness and the subdivision depth are frozen at
  creation. Changing any of them invalidates the plan: release it and
  create a new one. Only control-point *positions* may be edited through
  `ccs_EditPlanUpdate`.
- The `cc_Subd` returned by `ccs_EditPlanSubd` is read-only and owned by
  the plan; it is invalidated by `ccs_EditPlanRelease` (and must not be
  released or refined by the caller).
- A rejected update (invalid ids, non-finite positions, duplicates)
  returns non-zero and leaves the cage copy and every subdivision level
  untouched; subsequent updates resume from the last successful state.
- Updates to the same plan are not thread-safe, but independent plans
  share no state and can be interleaved freely.

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
