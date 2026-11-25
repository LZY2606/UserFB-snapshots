# Segment Scale Compensation

`com.jme3.anim.Joint` supports a per-joint **segment scale compensation** flag.
It lets a child joint ignore the local scale of its immediate parent when the
armature accumulates model and skinning transforms, while the child's
translation is still stretched by the parent (so bone length follows the
parent's scale). This is useful when a parent bone is stretched and the child
bone should keep its own thickness.

## API

| Method | Description |
| --- | --- |
| `void setSegmentScaleCompensate(boolean)` | Enable/disable compensation for this joint. |
| `boolean isSegmentScaleCompensate()` | Read the current setting. Defaults to `false`. |

The flag belongs to the **child** joint. It may be set independently on every
joint of an armature (mixed on/off is supported), and it is just a stored flag:
changes take effect at the next `Armature.update()` (or the next
`applyBindPose()`). Setting it on a root joint does not change any result
because a root has no parent.

## Model transform

For a compensating child joint, with column-vector convention:

```
model = P * T(t) * inverse(S_parent_local) * R(q) * S(s)
```

* `P` — parent's current complete model matrix.
* `t`, `q`, `s` — the child's local translation, rotation and scale.
* `S_parent_local` — the parent's **local** scale only (not the parent's model
  scale, and not its translation).

Translation is still affected by the parent's scale, so the bone segment length
changes; only the parent's local scale is canceled from the child's
orientation/scale. Shear produced by combining the child's rotation with
non-uniform scale is retained in the matrix path.

`MatrixJointModelTransform` implements exactly this formula.
`SeparateJointModelTransform` keeps its own TRS accumulation: position and
rotation are combined as before, while the model scale becomes
`parentModelScale * localScale / parentLocalScale` component-wise.

With compensation off, both implementations keep their original results.

## Bind pose

`saveBindPose()` / `applyBindPose()` follow the same per-joint rule. After
saving a pose, changing parent and child local transforms, and applying the
bind pose again, the saved local poses are restored and
`computeSkinningMatrices()` returns identity matrices. Repeated applies do not
accumulate error. Imported armatures that only provide bind matrices via
`setInverseModelBindMatrix` (without `saveBindPose()`) also restore a
representable local TRS. When a decomposition is ambiguous (e.g. negative
scale), compare recomposed matrices rather than individual scale signs.

Finite, non-zero parent local scales are supported, including negative
components.

## Persistence

The flag is serialized with the joint. `Cloner.deepClone` and
`BinaryExporter`/`BinaryImporter` round-trips preserve the flag, bind data and
post-load update capability; changing the flag on a clone does not affect the
original. Older J3O files that lack the field load with compensation off.

## Windowless example

See `task_tests/SegmentScaleExample.java`. It builds an armature and computes
skinning matrices without creating an `Application` or touching a graphics
device:

```
sh task_support/run.sh target   # compile core + tests, run the three test groups
```

or compile just the example against the built classes:

```
javac -cp .task-build/classes -d .task-build/classes task_tests/SegmentScaleExample.java
java -Djava.awt.headless=true -cp .task-build/classes SegmentScaleExample
```
