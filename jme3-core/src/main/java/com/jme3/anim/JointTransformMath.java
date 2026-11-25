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

import com.jme3.math.Matrix4f;
import com.jme3.math.Quaternion;
import com.jme3.math.Transform;
import com.jme3.math.Vector3f;

/**
 * Internal helpers shared by the joint model transform implementations.
 */
final class JointTransformMath {

    private JointTransformMath() {
    }

    /**
     * Decompose an affine matrix into a {@link Transform}, allowing a negative
     * scale component when the matrix has a negative determinant.
     *
     * <p>{@link Transform#fromTransformMatrix(Matrix4f)} always returns
     * non-negative scale factors, so a matrix containing a reflection cannot
     * be recomposed from the result. Here, when the linear part has a negative
     * determinant, one scale component (X) is made negative and the matching
     * normalized column is negated so the resulting rotation is proper. The
     * decomposition of a reflection is not unique; this method only
     * guarantees that the recomposed matrix matches the input (for matrices
     * that can be represented by translation, rotation and scale).
     *
     * @param mat the matrix to decompose (not null, unaffected)
     * @param store storage for the result (not null, modified)
     */
    static void fromTransformMatrix(Matrix4f mat, Transform store) {
        // Standard decomposition loses the sign of a reflection. Detect that
        // case first; for ordinary matrices use the standard implementation so
        // existing results are reproduced bit-for-bit.
        float determinant = mat.m00 * (mat.m11 * mat.m22 - mat.m12 * mat.m21)
                - mat.m01 * (mat.m10 * mat.m22 - mat.m12 * mat.m20)
                + mat.m02 * (mat.m10 * mat.m21 - mat.m11 * mat.m20);
        if (determinant >= 0f) {
            store.fromTransformMatrix(mat);
            return;
        }

        store.getTranslation().set(mat.m03, mat.m13, mat.m23);

        Vector3f c0 = new Vector3f(mat.m00, mat.m10, mat.m20);
        Vector3f c1 = new Vector3f(mat.m01, mat.m11, mat.m21);
        Vector3f c2 = new Vector3f(mat.m02, mat.m12, mat.m22);

        float sx = c0.length();
        float sy = c1.length();
        float sz = c2.length();

        Vector3f r0 = new Vector3f(c0);
        Vector3f r1 = new Vector3f(c1);
        Vector3f r2 = new Vector3f(c2);
        if (sx != 0f) {
            r0.divideLocal(sx);
        } else {
            r0.set(1, 0, 0);
            sx = 1f;
        }
        if (sy != 0f) {
            r1.divideLocal(sy);
        } else {
            r1.set(0, 1, 0);
            sy = 1f;
        }
        if (sz != 0f) {
            r2.divideLocal(sz);
        } else {
            r2.set(0, 0, 1);
            sz = 1f;
        }

        float det = r0.cross(r1).dot(r2);
        if (det < 0f) {
            // Move the reflection into the X scale so the rotation stays proper.
            sx = -sx;
            r0.negateLocal();
        }

        Quaternion rotation = new Quaternion();
        rotation.fromRotationMatrix(
                r0.x, r1.x, r2.x,
                r0.y, r1.y, r2.y,
                r0.z, r1.z, r2.z);
        rotation.normalizeLocal();

        store.setRotation(rotation);
        store.setScale(sx, sy, sz);
    }
}
