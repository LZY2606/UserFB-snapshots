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
 * Internal helpers shared by the {@code JointModelTransform} implementations
 * to support segment-scale compensation. Not part of the public API.
 */
final class SegmentScaleUtil {

    /**
     * Scaling tolerance used when checking a decomposed matrix.
     */
    private static final float DECOMPOSE_EPSILON = 1e-4f;

    private SegmentScaleUtil() {
        // utility class: not instantiable
    }

    /**
     * Left-multiply the rotation/scale part of an affine matrix by a diagonal
     * scaling matrix, leaving the translation column untouched.
     * <p>
     * In matrix terms this computes {@code diag(sx, sy, sz) * matrix}, ignoring
     * the translation of {@code matrix}.
     *
     * @param matrix the matrix to modify (not null)
     * @param sx factor for the first row
     * @param sy factor for the second row
     * @param sz factor for the third row
     */
    static void leftScaleRotationRows(Matrix4f matrix, float sx, float sy, float sz) {
        matrix.m00 *= sx;
        matrix.m01 *= sx;
        matrix.m02 *= sx;
        matrix.m10 *= sy;
        matrix.m11 *= sy;
        matrix.m12 *= sy;
        matrix.m20 *= sz;
        matrix.m21 *= sz;
        matrix.m22 *= sz;
    }

    /**
     * Left-multiply the rotation/scale part of the specified affine matrix by
     * a diagonal scaling matrix with the specified scale factors, leaving the
     * translation column untouched.
     *
     * @param matrix the matrix to modify (not null)
     * @param scale the scale factors (not null, unaffected)
     */
    static void leftScaleRotationRows(Matrix4f matrix, Vector3f scale) {
        leftScaleRotationRows(matrix, scale.x, scale.y, scale.z);
    }

    /**
     * Derive a local TRS from the specified affine matrix, preserving
     * reflections (negative scale) whenever the matrix can be represented as
     * translation + rotation + scale.
     * <p>
     * {@link Transform#fromTransformMatrix(Matrix4f)} always yields
     * non-negative scale factors and folds a reflection into the rotation,
     * which a quaternion cannot represent. When the plain decomposition would
     * not reproduce the input matrix, this method negates one scale component
     * and the matching rotation column so the recomposed matrix matches.
     *
     * @param localTransform storage for the result (modified)
     * @param matrix the local transform matrix (not null, unaffected)
     */
    static void decompose(Transform localTransform, Matrix4f matrix) {
        localTransform.fromTransformMatrix(matrix);
        if (localTransform.toTransformMatrix().isSimilar(matrix, DECOMPOSE_EPSILON)) {
            return;
        }

        float sx = (float) Math.sqrt(matrix.m00 * matrix.m00
                + matrix.m10 * matrix.m10 + matrix.m20 * matrix.m20);
        float sy = (float) Math.sqrt(matrix.m01 * matrix.m01
                + matrix.m11 * matrix.m11 + matrix.m21 * matrix.m21);
        float sz = (float) Math.sqrt(matrix.m02 * matrix.m02
                + matrix.m12 * matrix.m12 + matrix.m22 * matrix.m22);
        if (sx == 0f || sy == 0f || sz == 0f) {
            return; // singular: keep the plain decomposition
        }

        /*
         * Normalize each column by its length. The result is either a proper
         * rotation (det=+1) or a reflection (det=-1).
         */
        Matrix4f normalized = new Matrix4f();
        normalized.m00 = matrix.m00 / sx;
        normalized.m10 = matrix.m10 / sx;
        normalized.m20 = matrix.m20 / sx;
        normalized.m01 = matrix.m01 / sy;
        normalized.m11 = matrix.m11 / sy;
        normalized.m21 = matrix.m21 / sy;
        normalized.m02 = matrix.m02 / sz;
        normalized.m12 = matrix.m12 / sz;
        normalized.m22 = matrix.m22 / sz;

        Vector3f translation = matrix.toTranslationVector();
        if (determinant3x3(normalized) < 0f) {
            /*
             * Negate the X column and the X scale component. This turns the
             * reflection into a proper rotation while leaving the recomposed
             * matrix unchanged. Sign patterns are not unique for a
             * reflection, so any single axis suffices.
             */
            normalized.m00 = -normalized.m00;
            normalized.m10 = -normalized.m10;
            normalized.m20 = -normalized.m20;
            Transform candidate = new Transform(translation,
                    new Quaternion().fromRotationMatrix(normalized.toRotationMatrix()),
                    new Vector3f(-sx, sy, sz));
            if (candidate.toTransformMatrix().isSimilar(matrix, DECOMPOSE_EPSILON)) {
                localTransform.set(candidate);
                return;
            }
        } else {
            Transform candidate = new Transform(translation,
                    new Quaternion().fromRotationMatrix(normalized.toRotationMatrix()),
                    new Vector3f(sx, sy, sz));
            if (candidate.toTransformMatrix().isSimilar(matrix, DECOMPOSE_EPSILON)) {
                localTransform.set(candidate);
                return;
            }
        }

        /*
         * The matrix contains shear or is otherwise not a pure TRS: keep the
         * plain decomposition.
         */
    }

    /**
     * Determine the determinant of the upper-left 3x3 block.
     *
     * @param matrix the matrix to examine (not null, unaffected)
     * @return the determinant
     */
    static float determinant3x3(Matrix4f matrix) {
        return matrix.m00 * (matrix.m11 * matrix.m22 - matrix.m12 * matrix.m21)
                - matrix.m01 * (matrix.m10 * matrix.m22 - matrix.m12 * matrix.m20)
                + matrix.m02 * (matrix.m10 * matrix.m21 - matrix.m11 * matrix.m20);
    }
}
