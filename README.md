libanim
=======

About
-----
Libanim is a C animation library, which can be used as a generic framework to
add keyframe interpolation tracks for any single-valued or 3-vector parameters,
or at a slightly higher level as a hierarchical PRS (position/rotation/scaling)
animation node framework for 3D graphics programs.

Version 2 of libanim dropped the dependency to libvmath, and instead carries a
copy of gph-cmath (https://github.com/jtsiomb/gph-cmath) internally. The API
has been reworked to avoid forcing a dependency to any math library to the user
program, relying on floats and float pointers instead, which can be aliased to
any kind contiguous `x,y,z` vector and `x,y,z,w` quaternion, or simple arrays
of floats. Matrix arguments are expected to be arrays of 16 contiguous floats,
in OpenGL-compatible order.

Programs written for earlier versions of libanim, and using the high-level PRS
interface in `anim.h` are not source-compatible, nor binary-compatible with
libanim 2. Though in practice the API changes are minor, and porting should be
straightforward. Programs using only the low-level keyframe tracks in `track.h`
are unaffected by these changes.

License
-------
Copyright (C) 2012-2024 John Tsiombikas <nuclear@member.fsf.org>

This program is free software. You may use, modify, and redistribute it under
the terms of the GNU Lesser General Public License v3 or (at your option), any
later version published by the Free Software Foundation. See COPYING and
COPYING.LESSER for details.

Usage
-----
libanim has two levels you can use: the low level keyframe tracks API in
`track.h`, and the high level animation node system in `anim.h`.

### Track interface
`track.h` is all about the basic "set keyframe", "evaluate at time t",
"interpolation/extrapolation" operations, and it works on single valued
keyframe tracks, just time/value pairs. The idea is that to
animate a position, you'd have a higher level abstraction which sets keyframes
to three tracks (xyz), and evaluate those three tracks to figure out the
position at time t. 

#### Quaternions
The above works great for position and scale, but rotation quaternions need
special interpolation (slerp). This used to be handled by the higher level
interface initially, but since the node animation interface is not always
appropriate for all projects, and it's useful to be able to use the low level
interface only, version 2.0 adds `anm_get_quat` which takes 4 tracks, treats
them as representing a rotation quaternion over time, and interpolates
accordingly using slerp.

### Animation node interface
Initially this was supposed to be "the" API for this library, but over time it
became apparent that it contains abstractions which the application *might*
want to handle by itself, and have them integrated with the rest of its scene
management. So it doesn't necessarilly fit into every application.

This interface is centered around the `anm_node` structure. `anm_node`
represents a hierarchical transformation node which can be part of a 3D object.
Calling `anm_set_position`, `anm_set_rotation` and so on, automatically sets
keyframes to the "default animation" used by that node (you can have multiple
animations and switch/blend between them). And then you can just call
`anm_get_matrix` for a specific time, which will evaluate all 10 tracks for
that time, combine position/rotation/scaling, and give you back a matrix. The
matrix will be cached, so if you call multiple times for the same time, it
won't have to be re-evaluated. If it's a child node, it will recurse up,
evaluate its parent, and combine that matrix too. There is also a
`anm_get_node_matrix` function, if you don't want to take hierarchy into
account.

The animation node interface is pretty useful for a wide range of applications,
but if it doesn't fit your design, just ignore it altogether and use the low
level track API directly (`track.h`).

### Pose graph interface
`pose_graph.h` adds a reusable soft relative-pose constraint layer,
`anm_pose_graph`, on top of the node forest. It is meant for rig assembly
problems where several mount points play separate animations and must be
joined with tolerance: two hands and a prop forming a closed loop, contacts
across branches, duplicate or mutually inconsistent endpoints. The existing
parent/child edges stay a forest and are themselves treated as weighted
observations; extra observations may connect any two nodes, so cycles are
solved instead of rejected or overwritten in connection order.

This is a soft pose constraint solver for animation assembly, **not** rigid
body dynamics: there is no velocity, inertia or momentum, and nothing
preserves bone segment lengths (a solution is allowed to stretch a chain).

The minimized objective over the world rigid poses `W_i` is, for every
observation between nodes i,j with target rigid transform `D_ij` and positive
weights `wt, wr`:

    wt * |translation(inv(W_i) W_j) - translation(D_ij)|^2
  + wr * |Log(inv(rotation(D_ij)) rotation(inv(W_i) W_j))|^2

`Log` is the radians axis-angle vector (a quaternion component difference is
not used). Anchors are nodes whose world pose is pinned exactly to the
independently sampled animation pose, and every undirected connected
component must contain at least one anchor. The solver is Gauss-Newton with
Levenberg damping and a backtracking line search over the joint 6D local
rotation/translation perturbation, starting every solve at the sampled
animation pose. It is only required to reach a local stationary point.

The API is:

- `anm_pg_create(nodes, num_nodes, anchors, num_anchors, parent_wt, parent_wr)`
  builds a graph. The node array carries full ancestor chains (every parent of
  a member is a member), the original forest is untouched, each node may belong
  to at most one live graph, and nodes must outlive the graph. Parent edges are
  generated at solve time with the uniform weights given here.
- `anm_pg_replace_observations(pg, obs, num_obs)` atomically swaps the extra
  observation table. The whole new table (node references, finiteness, rigid
  targets, positive weights, anchored components together with the parent
  edges) is validated before anything takes effect; on failure the previous
  table and any published result are untouched.
- `anm_pg_solve(pg, tm, opts)` independently re-samples the original animation
  at `tm` through the track APIs only (no matrix caches, no previous graph
  result), solves, and on success publishes the result for exactly that time.
  Failure, including non-convergence, never modifies the last published pose,
  the tracks or the caches. `opts` may be null for sensible defaults.
- `anm_pg_clear_overrides(pg)` drops the published pose so original animation
  queries resume at every time; the observation table is kept.
- `anm_pg_destroy(pg)` detaches all overrides and releases the graph; member
  nodes then behave exactly as before and may join a new graph.
- `anm_pg_get_report(pg, report)` returns the status, final cost, inf norm of
  the 6D local-perturbation gradient and the iteration count of the last solve.

A published pose is consumed only at its exact time by `anm_get_matrix`,
`anm_get_inv_matrix`, `anm_eval` (the `node.matrix` it writes) and
`anm_get_node_matrix` (which returns the local pose relative to the final
parent world pose). At every other time these functions evaluate the original
animation. Corrections are never written back into the keyframe tracks, results
are not accumulated between solves, and reverse-order queries, re-solving the
same time, or re-solving after a table swap all re-sample from scratch.
Clearing or destroying the graph invalidates the affected caches so the
forward and inverse matrices are recomputed; nodes outside the graph and other
graphs are unaffected.

Current scope: single threaded; finite rigid body poses; unit animated
scaling (non-unit scaling returns `ANM_PG_ESCALE`); existing node pivots, fixed
dual-animation mixing (`anm_use_node_animations`) and animation time offsets
are supported. An active high-level automatic transition
(`anm_transition`/`anm_node_transition`) is explicitly rejected with
`ANM_PG_ETRANSITION`; the rest of the animation API is unchanged. Distinct
error codes (`ANM_PG_EINVAL`, `ANM_PG_ENODE`, `ANM_PG_EWEIGHT`,
`ANM_PG_ERIGID`, `ANM_PG_EANCHOR`, `ANM_PG_ETRANSITION`, `ANM_PG_ESCALE`,
`ANM_PG_ECONVERGE`, `ANM_PG_ENOMEM`) identify each failure class. A headless
CPU example lives in `example/pose-graph-cpu.c`.

Build
-----
To build and install libanim on UNIX, run the usual:

    ./configure
    make
    make install

See `./configure --help` for a complete list of build-time options.

To cross-compile for windows with mingw-w64, try the following incantation:

    ./configure --prefix=/usr/i686-w64-mingw32
    make CC=i686-w64-mingw32-gcc AR=i686-w64-mingw32-ar sys=mingw
    make install sys=mingw
