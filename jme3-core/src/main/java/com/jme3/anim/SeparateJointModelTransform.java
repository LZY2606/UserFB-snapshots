package com.jme3.anim;

import com.jme3.anim.util.JointModelTransform;
import com.jme3.math.Matrix4f;
import com.jme3.math.Quaternion;
import com.jme3.math.Transform;
import com.jme3.math.Vector3f;

/**
 * This JointModelTransform implementation accumulates model transform in a Transform class
 * This does NOT support proper nonuniform scale in the armature hierarchy.
 * But the effect might be useful in some circumstances.
 * Note that this is how the old animation system was working, so you might want to use this
 * if your model has nonuniform scale and was migrated from an old j3o model.
 */
public class SeparateJointModelTransform implements JointModelTransform {

    final private Transform modelTransform = new Transform();

    @Override
    public void updateModelTransform(Transform localTransform, Joint parent) {
        updateModelTransform(localTransform, parent, false);
    }

    @Override
    public void updateModelTransform(Transform localTransform, Joint parent,
            boolean segmentScaleCompensate) {
        modelTransform.set(localTransform);
        if (parent != null) {
            if (segmentScaleCompensate) {
                /*
                 * Cancel the parent's local scale (not its model scale) from
                 * the model scale. combineWithParent() then multiplies by the
                 * parent's model scale and applies the parent's scale to the
                 * translation as usual, so the bone-segment length still
                 * follows the parent's scaling. Position and rotation are
                 * unaffected.
                 */
                modelTransform.getScale().divideLocal(parent.getLocalScale());
            }
            modelTransform.combineWithParent(parent.getModelTransform());
        }
    }

    @Override
    public void getOffsetTransform(Matrix4f outTransform, Matrix4f inverseModelBindMatrix) {
        modelTransform.toTransformMatrix(outTransform).mult(inverseModelBindMatrix, outTransform);
    }

    @Override
    public void applyBindPose(Transform localTransform, Matrix4f inverseModelBindMatrix, Joint parent) {
        applyBindPose(localTransform, inverseModelBindMatrix, parent, false);
    }

    @Override
    public void applyBindPose(Transform localTransform, Matrix4f inverseModelBindMatrix,
            Joint parent, boolean segmentScaleCompensate) {
        Matrix4f inverse = inverseModelBindMatrix.invert();
        if (segmentScaleCompensate) {
            // Compensation must survive negative parent scales and reflections.
            SegmentScaleUtil.decompose(localTransform, inverse);
        } else {
            localTransform.fromTransformMatrix(inverse);
        }
        if (parent != null) {
            // Undo the parent transform without decomposing its potentially sheared inverse.
            Transform parentTransform = parent.getModelTransform();
            parentTransform.transformInverseVector(
                    localTransform.getTranslation(), localTransform.getTranslation());
            Quaternion inverseRotation = parentTransform.getRotation().inverse();
            inverseRotation.mult(localTransform.getRotation(), localTransform.getRotation());
            Vector3f parentModelScale = parentTransform.getScale();
            localTransform.getScale().divideLocal(parentModelScale);
            if (segmentScaleCompensate) {
                // Re-apply the parent's local scale, which was canceled during
                // accumulation, so the result is the original local TRS.
                localTransform.getScale().multLocal(parent.getLocalScale());
            }
        }
    }

    @Override
    public Transform getModelTransform() {
        return modelTransform;
    }

}
