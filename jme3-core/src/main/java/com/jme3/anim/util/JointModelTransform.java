/*
 * Copyright (c) 2009-2024 jMonkeyEngine
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
package com.jme3.anim.util;

import com.jme3.anim.Joint;
import com.jme3.math.Matrix4f;
import com.jme3.math.Transform;

/**
 * Implementations of this interface holds accumulated model transform of a Joint.
 * Implementation might choose different accumulation strategy.
 */
public interface JointModelTransform {

    /**
     * Update the joint's transform in model space.
     *
     * @param localTransform the joint's local transform (not null, unaffected)
     * @param parent the joint's parent, or null for a root joint
     */
    void updateModelTransform(Transform localTransform, Joint parent);

    /**
     * Update the joint's transform in model space, optionally compensating for
     * the local scale of the joint's immediate parent.
     * <p>
     * The default implementation ignores the compensation flag and delegates
     * to {@link #updateModelTransform(Transform, Joint)}, preserving the
     * behavior of existing implementations.
     *
     * @param localTransform the joint's local transform (not null, unaffected)
     * @param parent the joint's parent, or null for a root joint
     * @param segmentScaleCompensate true to cancel the parent's local scale
     */
    default void updateModelTransform(Transform localTransform, Joint parent,
            boolean segmentScaleCompensate) {
        updateModelTransform(localTransform, parent);
    }

    /**
     * Determine the joint's skinning transform.
     *
     * @param outTransform storage for the result (modified if not null)
     * @param inverseModelBindMatrix the joint's inverse model bind matrix (not
     * null, unaffected)
     */
    void getOffsetTransform(Matrix4f outTransform, Matrix4f inverseModelBindMatrix);

    /**
     * Configure joint's local transform for bind pose.
     *
     * @param localTransform the joint's local transform (not null, unaffected)
     * @param inverseModelBindMatrix the joint's inverse model bind matrix (not
     * null, unaffected)
     * @param parent the joint's parent, or null for a root joint
     */
    void applyBindPose(Transform localTransform, Matrix4f inverseModelBindMatrix, Joint parent);

    /**
     * Configure the joint's local transform for the bind pose, optionally
     * compensating for the local scale of the joint's immediate parent.
     * <p>
     * The default implementation ignores the compensation flag and delegates
     * to {@link #applyBindPose(Transform, Matrix4f, Joint)}, preserving the
     * behavior of existing implementations.
     *
     * @param localTransform the joint's local transform (not null, modified)
     * @param inverseModelBindMatrix the joint's inverse model bind matrix (not
     * null, unaffected)
     * @param parent the joint's parent, or null for a root joint
     * @param segmentScaleCompensate true to cancel the parent's local scale
     */
    default void applyBindPose(Transform localTransform, Matrix4f inverseModelBindMatrix,
            Joint parent, boolean segmentScaleCompensate) {
        applyBindPose(localTransform, inverseModelBindMatrix, parent);
    }

    /**
     * Determine the joint's transform in model space.
     *
     * @return a new instance or a pre-existing one
     */
    Transform getModelTransform();
}
