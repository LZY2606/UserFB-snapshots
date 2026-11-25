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
import com.jme3.math.Transform;
import com.jme3.math.Vector3f;

/**
 * An implementation of {@link JointModelTransform} that accumulates joint transformations
 * into a {@link Matrix4f}. This approach is particularly useful for correctly handling
 * non-uniform scaling within an armature hierarchy, as {@code Matrix4f} can represent
 * non-uniform scaling directly, unlike {@link Transform}, which typically handles
 * uniform scaling.
 * <p>
 * This class maintains a single {@link Matrix4f} to represent the accumulated
 * model-space transform of the joint it's associated with.
 * <p>
 * When segment scale compensation is enabled on the joint, the model matrix of
 * this joint is
 * {@code P * T(t) * inverse(S_parent_local) * R(q) * S(s)}, where {@code P} is
 * the parent's current model matrix and {@code t}, {@code q} and {@code s} are
 * this joint's local translation, rotation and scale. The translation is still
 * affected by the parent's scale, so bone lengths change, while only the
 * parent's local scale is canceled from the orientation/scale accumulation.
 * Shear caused by combining the rotation with non-uniform scale is preserved.
 */
public class MatrixJointModelTransform implements JointModelTransform {

    /**
     * The model-space transform of the joint represented as a Matrix4f.
     * This matrix accumulates the local transform of the joint and the model transform
     * of its parent.
     */
    private final Matrix4f modelTransformMatrix = new Matrix4f();
    /**
     * A temporary Transform instance used for converting the modelTransformMatrix
     * to a Transform object when {@link #getModelTransform()} is called.
     */
    private final Transform modelTransform = new Transform();
    /**
     * Scratch matrices reused while accumulating the transform.
     */
    private final Matrix4f scratchA = new Matrix4f();
    private final Matrix4f scratchB = new Matrix4f();
    private final Matrix4f scratchC = new Matrix4f();
    private final Matrix4f scratchD = new Matrix4f();

    private boolean segmentScaleCompensate = false;

    @Override
    public void setSegmentScaleCompensate(boolean segmentScaleCompensate) {
        this.segmentScaleCompensate = segmentScaleCompensate;
    }

    @Override
    public void updateModelTransform(Transform localTransform, Joint parent) {
        // Reset the affine row after matrix inversions performed by applyBindPose().
        modelTransformMatrix.loadIdentity();
        localTransform.toTransformMatrix(modelTransformMatrix);
        if (parent != null) {
            MatrixJointModelTransform transform = (MatrixJointModelTransform) parent.getJointModelTransform();
            if (segmentScaleCompensate) {
                // P * T(t) * inverse(S_parent_local) * R(q) * S(s)
                Matrix4f parentMatrix = transform.getModelTransformMatrix();
                Matrix4f translationPart = scratchA;
                translationPart.loadIdentity();
                translationPart.setTranslation(localTransform.getTranslation());
                Matrix4f parentLocalScaleInverse = scratchB;
                parentLocalScaleInverse.loadIdentity();
                parentLocalScaleInverse.setScale(1f / parent.getLocalScale().x,
                        1f / parent.getLocalScale().y,
                        1f / parent.getLocalScale().z);
                Matrix4f rotationScale = scratchC;
                rotationScale.loadIdentity();
                localTransform.getRotation().toTransformMatrix(rotationScale);
                rotationScale.setScale(localTransform.getScale());

                Matrix4f tmp = scratchD;
                parentMatrix.mult(translationPart, tmp);
                tmp.mult(parentLocalScaleInverse, tmp);
                tmp.mult(rotationScale, modelTransformMatrix);
            } else {
                transform.getModelTransformMatrix().mult(modelTransformMatrix, modelTransformMatrix);
            }
        }
    }

    @Override
    public void getOffsetTransform(Matrix4f outTransform, Matrix4f inverseModelBindMatrix) {
        modelTransformMatrix.mult(inverseModelBindMatrix, outTransform);
    }

    @Override
    public void applyBindPose(Transform localTransform, Matrix4f inverseModelBindMatrix, Joint parent) {
        modelTransformMatrix.set(inverseModelBindMatrix).invertLocal(); // model transform = model bind
        if (parent != null) {
            MatrixJointModelTransform transform = (MatrixJointModelTransform) parent.getJointModelTransform();
            Matrix4f parentModelInverse = transform.getModelTransformMatrix().invert(); // does not modify the parent
            if (segmentScaleCompensate) {
                /*
                 * The update computed
                 *   modelBind = P * T(t) * inv(S_parent_local) * R(q) * S(s),
                 * so P^-1 * modelBind = T(t) * inv(S_parent_local) * R(q) * S(s).
                 * The translation is recovered directly; the linear part is
                 * scaled by the parent's local scale to get R(q) * S(s).
                 */
                Matrix4f localBind = scratchA;
                parentModelInverse.mult(modelTransformMatrix, localBind);
                Vector3f translation = new Vector3f(localBind.m03, localBind.m13, localBind.m23);
                Matrix4f parentLocalScale = scratchB;
                parentLocalScale.loadIdentity();
                parentLocalScale.setScale(parent.getLocalScale());
                Matrix4f rotationScale = scratchC;
                parentLocalScale.mult(localBind, rotationScale);
                rotationScale.setTranslation(translation);
                JointTransformMath.fromTransformMatrix(rotationScale, localTransform);
            } else {
                parentModelInverse.mult(modelTransformMatrix, modelTransformMatrix);
                localTransform.fromTransformMatrix(modelTransformMatrix);
            }
        } else {
            localTransform.fromTransformMatrix(modelTransformMatrix);
        }
    }

    /**
     * Access the model transform.
     *
     * @return the pre-existing instance
     */
    public Matrix4f getModelTransformMatrix() {
        return modelTransformMatrix;
    }

    @Override
    public Transform getModelTransform() {
        modelTransform.fromTransformMatrix(modelTransformMatrix);
        return modelTransform;
    }
}
