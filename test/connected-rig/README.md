# Connected-rig regression (headless)

A window-free regression suite for Fullik's connected-chain solver paths:
`Structure3D.update` driving chains connected to a host bone, plus direct
`Chain3D.solveForTarget` solves for contrast. It uses the real
`src/core/Structure3D.js`, `src/core/Chain3D.js`, `src/core/Joint3D.js` and
`src/math/V3.js`/`M3.js` — no Three.js, no browser, no second FABRIK
implementation.

## Usage

```sh
sh test/connected-rig/check.sh
```

The script

1. bundles `test/connected-rig/run.js` with the esbuild locked in
   `baseline/package-lock.json` (installed with `npm ci` into `baseline/` on
   first use, same toolchain as `baseline/run-node.sh`);
2. runs the suite against the pristine `src/`;
3. copies `src/` into a temporary directory three times and applies one
   single-behaviour mutation to each copy (`mutations/*.patch`), then runs the
   same suite, with the same assertions, against each mutant;
4. prints a per-case diff summary (which fixtures failed, with bone index,
   expected and actual values) and finishes with `CONNECTED_RIG_QA_OK`.

Everything happens in a temporary directory; repeated runs do not modify
`src/`, `build/` or `demos/` (the script fingerprints them before and after).
A mutant only counts as "detected" when the suite runs cleanly and fails
geometric assertions — a build failure, import failure, missing fixture or
crash is reported as an infrastructure error instead.

## What is checked

All assertions are derived from the solved bones' actual start/end points
(never from `solveForTarget`'s returned distance or the solver's cached
relative-constraint vectors):

- **Bone lengths** preserved exactly, and **joint connectivity**
  (`bones[i].end == bones[i+1].start`).
- **Fixed bases** stay put; a connected child's base equals the **real
  connection point** (`start` or `end`) of the host bone, while the host
  rotates (`connection-start-end`) and while a non-fixed host translates
  (`connection-translated-host`).
- **Rotor cones**: basebone direction stays within the cone about the
  constraint axis; for connected local rotors the axis is the one carried
  into world space by the host frame (recomputed in the test with `M3`, not
  read back from the solver).
- **Hinge plane and directed limits**: global hinges keep their world
  rotation axis (`|dir . axis| ~ 0`, signed angle within `[-cw, +acw]`) even
  after the host rotates — the world hinge axis must not be dragged along.
- **Analytic poses**: reachable targets with closed-form answers assert the
  exact effector position (straight chains, single-bone rotor clamped at
  exactly 40 deg, single-bone hinge clamped at exactly +55 / -15 deg, the
  local-hinge right-angle elbow).
- **Asymmetric limits** (15 deg clockwise / 55 deg anticlockwise) make
  sign/direction mix-ups unable to pass by accident.
- **Sequential retargeting**: targets move +X -> +Z -> +X; both passes at the
  original target must reproduce the same effector position.
- **Unreachable targets**: only finite coordinates, lengths, connectivity,
  the true connection point and constraint legality are required — no
  global-optimum demand.
- **Inputs stay inputs**: template chains handed to `connectChain`, caller
  target vectors, and sibling chains/children are verified unchanged after
  solving.

Tolerances are documented inline in `run.js` (1e-9 for copied/constructed
coordinates, 5e-3 for reachable effectors on unit bones, 0.5 deg for directed
angles, 1e-6 for hinge-plane membership); each is far tighter than the
>=0.3-position / >=10-degree shifts the guarded mutations produce.

## Mutation coverage

| mutant | behaviour broken | fixtures expected to fail |
| --- | --- | --- |
| `mut-local-frame` | connected child's local constraint axis is no longer transformed by the host bone direction | `baseline-host-rotation`, `local-rotor-single`, `local-hinge-connected`, `two-children-shared-host`, `sequential-retarget`, `unreachable-target` |
| `mut-connection-point` | `start`/`end` connection-point selection swapped | `connection-start-end`, `connection-translated-host` (plus every connected base check) |
| `mut-hinge-limits` | `Joint3D` clockwise/anticlockwise limits swapped | `direct-global-hinge-limits`, `global-hinge-connected-control` |

## Behaviour this suite pins down

The expectations describe the solver as shipped, not an idealised one. Two
properties of the current sources are load-bearing here and are asserted
deliberately:

- `M3.createRotationMatrix` builds its X/Y basis from `V3.cross`, which is a
  pure function — the cross products are discarded, so the local frame's X/Y
  axes equal the reference direction. A connected local hinge therefore
  enforces a directed-angle bound about the host direction rather than a
  textbook hinge plane.
- `V3.projectOnPlane` returns a closure without applying it, so the plane
  projection step in the hinge paths is inert; only the directed-angle clamp
  (`constrainedUV`) has an effect.

If either is fixed in `src/`, the connected local-hinge expectations in
`local-hinge-connected` / `two-children-shared-host` should be revisited —
that is the suite doing its job.

## Not covered (yet)

- `Chain2D` / `Structure2D` and the 2D joint types.
- Mid-chain `J_LOCAL` hinges/rotors between non-base bones, freely rotating
  hinges, and rotor angles above 90 deg.
- Embedded targets (`useEmbeddedTarget`), non-fixed connected children, and
  structures with more than two levels of nesting.
- Mesh/THREE integration (`addChainMeshs`, `isWithMesh`).
- Solve-quality metrics for unreachable targets (distance optimality,
  iteration counts) and any performance bounds.
