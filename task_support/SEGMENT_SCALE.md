# Segment-Scale Compensation

`com.jme3.anim.Joint` supports per-joint segment-scale compensation. It is
intended for rigs whose bone segments get stretched by a parent's scale, while
the child bones' own geometry should keep its thickness.

## API

```java
joint.setSegmentScaleCompensate(boolean enabled); // default: false
joint.isSegmentScaleCompensate();                // read the current value
```

The flag belongs to the **child** joint:

* `true` — this joint cancels the local scale of its immediate parent, so the
  parent's scaling does not scale this joint's own geometry.
* `false` — the legacy behavior is unchanged.

Bone-segment lengths still change, because the joint's translation keeps going
through the parent's full model transform. Joints in one armature may use
different values. A root joint has no parent, so its flag does not change the
result. Changes take effect on the next `Armature.update()`; no other call
sequence has to change.

## Matrix ordering

With column vectors, a compensated child's model matrix in
`MatrixJointModelTransform` is

```
P * T(t) * inverse(S_parent_local) * R(q) * S(s)
```

where `P` is the parent's complete model matrix and `t`, `q`, `s` are the
child's local TRS. The inverse is applied between the translation and the
rotation/scale block — it cancels the parent's *local* scale only. It never
divides by the parent's model scale and never touches the translation. Shear
that results from a child's rotation combined with the parent's non-uniform
scale stays in the matrix representation.

`SeparateJointModelTransform` keeps its Transform-based accumulation: position
and rotation combine as before, and the model scale becomes

```
parent model scale * local scale / parent local scale
```

when compensation is on. With compensation off, both implementations produce
exactly the results they produced before.

## Bind pose and persistence

`saveBindPose()`, `applyBindPose()` and `computeSkinningMatrices()` follow the
same rule. Saving the current pose, changing the local transforms, then
applying the bind pose restores the saved local pose and returns the skinning
matrices to identity, repeatedly and without accumulating error.

An imported armature that only provides bind matrices through
`setInverseModelBindMatrix()` (without `saveBindPose()`) is supported too:
`applyBindPose()` reconstructs a representable local TRS that reproduces the
bind model matrices. Replacing a bind matrix invalidates any pose snapshot
taken earlier, so the new matrices are used on the next `applyBindPose()`.
Reflections (negative scales) have no unique decomposition; compare recomposed
matrices rather than individual scale signs.

The flag survives `Cloner.deepClone` and BinaryExporter/BinaryImporter round
trips, and a cloned armature can change its flags without affecting the
original. J3O files written before the field existed read back with
compensation disabled.

## Example

`SegmentScaleExample.java` (in this directory) builds a three-bone arm and
prints skinning matrices before and after stretching the shoulder. It needs no
window, graphics device, or `AssetManager`:

```
javac --release 17 -d /tmp/demo $(find jme3-core/src/main/java \
    jme3-core/src/plugins/java jme3-core/src/tools/java -name '*.java')
javac --release 17 -cp /tmp/demo -d /tmp/demo task_support/SegmentScaleExample.java
java -cp /tmp/demo SegmentScaleExample
```

Running the task harness (`sh task_support/run.sh target`) compiles the example
together with the test suite.
