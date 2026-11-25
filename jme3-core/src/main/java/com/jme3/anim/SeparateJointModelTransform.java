/*
 * Copyright (c) 2009-2025 jMonkeyEngine
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *
 * * Neither the name of 'jMonkeyEngine' nor the names of its contributors
 *   may be used to endorse or promote products derived from this software
 *   without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
package com.jme3.anim;

import com.jme3.anim.util.JointModelTransform;
import com.jme3.math.Matrix4f;
import com.jme3.math.Quaternion;
import com.jme3.math.Transform;

/**
 * This JointModelTransform implementation accumulates model transform in a Transform class
 * This does NOT support proper nonuniform scale in the armature hierarchy.
 * But the effect might be useful in some circumstances.
 * Note that this is how the old animation system was working, so you might want to use this
 * if your model has nonuniform scale and was migrated from an old j3o model.
 * <p>
 * When segment scale compensation is enabled on the joint, translation and
 * rotation are still combined by the usual rules, but the accumulated model
 * scale becomes {@code parentModelScale * localScale / parentLocalScale}
 * component-wise. The parent's local scale is thus canceled from this joint's
 * own scale while the parent model scale is retained.
 */
public class SeparateJointModelTransform implements JointModelTransform {

    final private Transform modelTransform = new Transform();

    private boolean segmentScaleCompensate = false;

    @Override
    public void setSegmentScaleCompensate(boolean segmentScaleCompensate) {
        this.segmentScaleCompensate = segmentScaleCompensate;
    }

    @Override
    public void updateModelTransform(Transform localTransform, Joint parent) {
        modelTransform.set(localTransform);
        if (parent != null) {
            Transform parentTransform = parent.getModelTransform();
            modelTransform.combineWithParent(parentTransform);
            if (segmentScaleCompensate) {
                // combineWithParent() produced localScale * parentModelScale;
                // replace it with parentModelScale * localScale / parentLocalScale.
                modelTransform.getScale().divideLocal(parent.getLocalScale());
            }
        }
    }

    @Override
    public void getOffsetTransform(Matrix4f outTransform, Matrix4f inverseModelBindMatrix) {
        modelTransform.toTransformMatrix(outTransform).mult(inverseModelBindMatrix, outTransform);
    }

    @Override
    public void applyBindPose(Transform localTransform, Matrix4f inverseModelBindMatrix, Joint parent) {
        Matrix4f modelBind = inverseModelBindMatrix.invert();
        if (parent != null && segmentScaleCompensate) {
            // Signed-aware decomposition so negative bind scale survives.
            JointTransformMath.fromTransformMatrix(modelBind, localTransform);
            Transform parentTransform = parent.getModelTransform();
            parentTransform.transformInverseVector(
                    localTransform.getTranslation(), localTransform.getTranslation());
            Quaternion inverseRotation = parentTransform.getRotation().inverse();
            inverseRotation.mult(localTransform.getRotation(), localTransform.getRotation());
            // Invert parentModelScale * localScale / parentLocalScale.
            localTransform.getScale()
                    .multLocal(parent.getLocalScale())
                    .divideLocal(parentTransform.getScale());
            return;
        }
        localTransform.fromTransformMatrix(modelBind);
        if (parent != null) {
            // Undo the parent transform without decomposing its potentially sheared inverse.
            Transform parentTransform = parent.getModelTransform();
            parentTransform.transformInverseVector(
                    localTransform.getTranslation(), localTransform.getTranslation());
            Quaternion inverseRotation = parentTransform.getRotation().inverse();
            inverseRotation.mult(localTransform.getRotation(), localTransform.getRotation());
            localTransform.getScale().divideLocal(parentTransform.getScale());
        }
    }

    @Override
    public Transform getModelTransform() {
        return modelTransform;
    }

}
