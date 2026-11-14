import com.jme3.anim.Armature;
import com.jme3.anim.Joint;
import com.jme3.anim.MatrixJointModelTransform;
import com.jme3.anim.SeparateJointModelTransform;
import com.jme3.anim.util.JointModelTransform;
import com.jme3.export.binary.BinaryExporter;
import com.jme3.export.binary.BinaryImporter;
import com.jme3.math.Matrix4f;
import com.jme3.math.Quaternion;
import com.jme3.math.Transform;
import com.jme3.math.Vector3f;
import com.jme3.util.clone.Cloner;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * Tests for per-joint segment-scale compensation in the jme3-core armature.
 *
 * <p>Three entry points are invoked by TargetMain through TaskChecks, which
 * counts every successful assertion and fails a group that runs none. All
 * results are cross-checked against independently composed matrices rather
 * than against the flag itself.
 */
public final class SegmentScaleTests {

    private static final float EPS = 1e-4f;

    private SegmentScaleTests() {
        // test class: not instantiable
    }

    // ------------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------------

    /** Builds T(t) * R(q) * S(s) (column vectors) as a 4x4 matrix. */
    private static Matrix4f trs(Vector3f t, Quaternion q, Vector3f s) {
        Matrix4f m = new Matrix4f();
        m.setTranslation(t);
        q.toTransformMatrix(m);
        m.setScale(s);
        return m;
    }

    private static Matrix4f trs(Transform transform) {
        return transform.toTransformMatrix();
    }

    /** Builds diag(sx, sy, sz) with a zero translation column. */
    private static Matrix4f diagonal(Vector3f s) {
        Matrix4f m = new Matrix4f();
        m.m00 = s.x;
        m.m11 = s.y;
        m.m22 = s.z;
        return m;
    }

    private static Vector3f inv(Vector3f s) {
        return new Vector3f(1f / s.x, 1f / s.y, 1f / s.z);
    }

    private static Transform xform(float tx, float ty, float tz,
            float ax, float ay, float az, float sx, float sy, float sz) {
        return new Transform(new Vector3f(tx, ty, tz),
                new Quaternion().fromAngles(ax, ay, az), new Vector3f(sx, sy, sz));
    }

    private static float maxDiff(Matrix4f a, Matrix4f b) {
        float d = 0f;
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                d = Math.max(d, Math.abs(a.get(row, col) - b.get(row, col)));
            }
        }
        return d;
    }

    private static boolean sameMatrix(Matrix4f a, Matrix4f b) {
        return maxDiff(a, b) <= EPS;
    }

    /** Compares TRS by recomposition; reflections have no unique decomposition. */
    private static boolean sameTransform(Transform a, Transform b) {
        return sameMatrix(a.toTransformMatrix(), b.toTransformMatrix());
    }

    private static List<Class<? extends JointModelTransform>> modes() {
        List<Class<? extends JointModelTransform>> list = new ArrayList<>();
        list.add(MatrixJointModelTransform.class);
        list.add(SeparateJointModelTransform.class);
        return list;
    }

    /**
     * A three-joint chain: grandparent -> parent -> child, created with the
     * specified model-transform implementation and updated once.
     */
    private static final class Chain {
        final Transform grandLocal;
        final Transform parentLocal;
        final Transform childLocal;
        final Joint grand;
        final Joint parent;
        final Joint child;
        final Armature armature;

        Chain(Transform grandLocal, Transform parentLocal, Transform childLocal,
                Class<? extends JointModelTransform> mode) {
            this.grandLocal = grandLocal.clone();
            this.parentLocal = parentLocal.clone();
            this.childLocal = childLocal.clone();
            grand = new Joint("grand");
            parent = new Joint("parent");
            child = new Joint("child");
            grand.setLocalTransform(grandLocal);
            parent.setLocalTransform(parentLocal);
            child.setLocalTransform(childLocal);
            grand.addChild(parent);
            parent.addChild(child);
            armature = new Armature(new Joint[]{grand, parent, child});
            armature.setModelTransformClass(mode);
            armature.update();
        }

        void flags(boolean grandFlag, boolean parentFlag, boolean childFlag) {
            grand.setSegmentScaleCompensate(grandFlag);
            parent.setSegmentScaleCompensate(parentFlag);
            child.setSegmentScaleCompensate(childFlag);
            armature.update();
        }

        /**
         * The model matrix as computed by the armature when the bind matrices
         * are still identity, which makes the skinning matrix equal to the
         * model matrix.
         */
        Matrix4f modelOf(int index) {
            return new Matrix4f(armature.computeSkinningMatrices()[index]);
        }

        /**
         * The model matrix regardless of the bind matrices: skinning matrix
         * times the bind model matrix.
         */
        Matrix4f worldModelOf(int index) {
            Joint joint = armature.getJoint(index);
            Matrix4f bindModel = new Matrix4f(joint.getInverseModelBindMatrix()).invertLocal();
            return armature.computeSkinningMatrices()[index].mult(bindModel);
        }
    }

    /**
     * Expected matrix-path model matrix for a child joint:
     * P * T(t) * inverse(S_parent_local) * R(q) * S(s), or
     * P * T(t) * R(q) * S(s) when compensation is disabled.
     * <p>
     * Note the ordering: T(t) is the left factor, so the inverse scale is
     * applied to the rotation/scale block only and the translation is
     * unaffected.
     */
    private static Matrix4f expectedModelMatrix(Matrix4f parentModel,
            Transform local, Vector3f parentLocalScale, boolean compensate) {
        Matrix4f rotationScale = trs(Vector3f.ZERO, local.getRotation(), local.getScale());
        if (compensate) {
            rotationScale = diagonal(inv(parentLocalScale)).mult(rotationScale);
        }
        Matrix4f translation = new Matrix4f();
        translation.setTranslation(local.getTranslation());
        return parentModel.mult(translation.mult(rotationScale));
    }

    /**
     * Expected separate-path model transform for a child joint: translation
     * and rotation combine as before, and model scale becomes parent model
     * scale * local scale / parent local scale when compensating.
     */
    private static Transform expectedSeparateModel(Transform parentModel,
            Transform local, Vector3f parentLocalScale, boolean compensate) {
        Transform result = local.clone();
        if (compensate) {
            result.getScale().divideLocal(parentLocalScale);
        }
        return result.combineWithParent(parentModel);
    }

    // ------------------------------------------------------------------
    // matrix: model and skinning values
    // ------------------------------------------------------------------

    public static void matrix(TaskChecks c) {
        Transform g = xform(1f, 2f, -3f, 0.2f, -0.4f, 0.1f, -2f, 3f, 1.5f);
        Transform p = xform(0.5f, -1f, 2f, -0.1f, 0.7f, 0.3f, 2f, 0.5f, -1.5f);
        Transform ch = xform(1.5f, 2f, 0.25f, 0.6f, 0.2f, -1.1f, 0.75f, 2.5f, 1.25f);
        Chain chain = new Chain(g, p, ch, MatrixJointModelTransform.class);

        boolean[] both = {false, true};
        for (boolean grandFlag : both) {
            for (boolean parentFlag : both) {
                for (boolean childFlag : both) {
                    String label = "[" + grandFlag + "," + parentFlag + "," + childFlag + "]";
                    chain.flags(grandFlag, parentFlag, childFlag);

                    Matrix4f grandModel = trs(g);
                    Matrix4f parentModel = expectedModelMatrix(grandModel, p,
                            g.getScale(), parentFlag);
                    Matrix4f childModel = expectedModelMatrix(parentModel, ch,
                            p.getScale(), childFlag);

                    Matrix4f actualGrand = chain.modelOf(0);
                    Matrix4f actualParent = chain.modelOf(1);
                    Matrix4f actualChild = chain.modelOf(2);

                    c.check(sameMatrix(actualGrand, grandModel),
                            "matrix grandparent model " + label);
                    c.check(sameMatrix(actualParent, parentModel),
                            "matrix parent model " + label);
                    c.check(sameMatrix(actualChild, childModel),
                            "matrix child model " + label);

                    // Skinning matrices equal model matrices here (identity bind).
                    Matrix4f[] skin = chain.armature.computeSkinningMatrices();
                    c.check(sameMatrix(skin[1], parentModel),
                            "matrix parent skin " + label);
                    c.check(sameMatrix(skin[2], childModel),
                            "matrix child skin " + label);
                }
            }
        }

        /*
         * The root flag does not change the result, because a root joint has no
         * parent scale to cancel.
         */
        chain.flags(true, false, false);
        Matrix4f rootOnParent = chain.modelOf(1);
        Matrix4f rootOnChild = chain.modelOf(2);
        chain.flags(false, false, false);
        Matrix4f rootOffParent = chain.modelOf(1);
        Matrix4f rootOffChild = chain.modelOf(2);
        c.check(sameMatrix(rootOnParent, rootOffParent),
                "matrix root flag does not change parent");
        c.check(sameMatrix(rootOnChild, rootOffChild),
                "matrix root flag does not change child");

        /*
         * With compensation on, the child's translation still goes through the
         * full parent model matrix, so stretching the upper arm still moves
         * the elbow: the child origin is P * t, unaffected by the cancellation
         * of the parent's local scale.
         */
        chain.flags(false, true, true);
        Matrix4f parentModelForTranslation = expectedModelMatrix(trs(g), p,
                g.getScale(), true);
        Vector3f expectedChildOrigin = parentModelForTranslation.mult(
                ch.getTranslation());
        Vector3f actualChildOrigin = chain.modelOf(2).mult(new Vector3f());
        c.check(actualChildOrigin.distance(expectedChildOrigin) <= EPS,
                "matrix child translation still follows parent scale");

        /*
         * Shear from rotation combined with non-uniform scale must survive in
         * the matrix path. Use an uncompensated parent so the parent's
         * non-uniform scale acts on the rotated child: the child model then
         * cannot be reproduced by a plain TRS decomposition. Decomposing and
         * recomposing it (what Transform-based accumulation does) must lose
         * information, proving the matrix path keeps the shear.
         */
        chain.flags(false, false, true);
        Matrix4f shearedChild = chain.modelOf(2);
        Transform decomposedChild = new Transform();
        decomposedChild.fromTransformMatrix(shearedChild);
        c.check(!sameMatrix(decomposedChild.toTransformMatrix(), shearedChild),
                "matrix path preserves shear from rotation and non-uniform scale");

        /*
         * Finite, non-zero parent local scale with negative components: the
         * inverse appears only in the 3x3 block, while the translation keeps
         * its direction.
         */
        Transform negGrand = xform(1f, 2f, 3f, 0.1f, -0.2f, 0.3f, -2f, 3f, 1.5f);
        Transform negParent = xform(0.5f, -1f, 2f, -0.1f, 0.7f, 0.3f, -1f, -2f, 2.5f);
        Chain negative = new Chain(negGrand, negParent,
                xform(1.5f, 2f, 0.25f, 0.6f, 0.2f, -1.1f, 1f, 1f, 1f),
                MatrixJointModelTransform.class);
        negative.flags(false, true, false);
        Matrix4f negGrandModel = trs(negGrand);
        Matrix4f negParentExpected = expectedModelMatrix(negGrandModel, negParent,
                negGrand.getScale(), true);
        c.check(sameMatrix(negative.modelOf(1), negParentExpected),
                "matrix negative parent local scale model");
        c.check(sameMatrix(negative.armature.computeSkinningMatrices()[1],
                negParentExpected), "matrix negative parent local scale skin");
        Vector3f expectedNegOrigin = negGrandModel.mult(negParent.getTranslation());
        Vector3f actualNegOrigin = negative.modelOf(1).mult(new Vector3f());
        c.check(actualNegOrigin.distance(expectedNegOrigin) <= EPS,
                "matrix negative scale keeps translation direction");
    }

    /**
     * Check that the flag takes effect on the next update: the model matrix
     * changes immediately after the flag is toggled and no additional call is
     * needed.
     */
    private static void checkNextUpdate(TaskChecks c, Class<? extends JointModelTransform> mode) {
        Transform g = xform(1f, 2f, 3f, 0.1f, -0.2f, 0.3f, 2f, 3f, 0.5f);
        Transform p = xform(0.5f, -1f, 2f, 0.2f, 0.1f, -0.4f, 1.5f, 0.75f, 2f);
        Transform ch = xform(1.5f, 2f, 0.25f, 0.6f, 0.2f, -1.1f, 1f, 1f, 1f);
        Chain chain = new Chain(g, p, ch, mode);

        chain.flags(false, false, false);
        Matrix4f uncompensated = chain.modelOf(2);
        // Toggle only; do not call update() here.
        chain.parent.setSegmentScaleCompensate(true);
        chain.armature.update();
        Matrix4f compensated = chain.modelOf(2);
        c.check(!sameMatrix(uncompensated, compensated),
                "flag change takes effect on next update " + mode);

        /*
         * A modified local TRS after the flag change is still honored, i.e. the
         * new value is used in this same update.
         */
        chain.parent.setLocalRotation(new Quaternion().fromAngles(-0.3f, 0.8f, 0.2f));
        chain.armature.update();
        Matrix4f afterRotation = chain.modelOf(2);
        c.check(!sameMatrix(compensated, afterRotation),
                "local TRS animation still works with compensation " + mode);
    }

    // ------------------------------------------------------------------
    // legacy: original behavior and the separate accumulation rule
    // ------------------------------------------------------------------

    public static void legacy(TaskChecks c) {
        /*
         * Defaults: the switch is off, and updating without it reproduces the
         * original matrix results for both implementations.
         */
        Joint parent = new Joint("upper");
        Joint child = new Joint("lower");
        parent.setLocalScale(new Vector3f(2f, 3f, 1f));
        child.setLocalTranslation(new Vector3f(1f, 0f, 0f));
        parent.addChild(child);
        c.check(!child.isSegmentScaleCompensate(), "legacy default flag is false");
        c.check(!parent.isSegmentScaleCompensate(), "legacy root default flag is false");

        for (Class<? extends JointModelTransform> mode : modes()) {
            Armature armature = new Armature(new Joint[]{parent, child});
            armature.setModelTransformClass(mode);
            armature.update();
            armature.saveBindPose();
            Matrix4f[] bind = armature.computeSkinningMatrices();
            c.check(sameMatrix(bind[0], Matrix4f.IDENTITY), "legacy bind skin root " + mode);
            c.check(sameMatrix(bind[1], Matrix4f.IDENTITY), "legacy bind skin child " + mode);

            parent.setLocalScale(new Vector3f(4f, 3f, 1f));
            armature.update();
            Matrix4f skin = new Matrix4f(armature.computeSkinningMatrices()[1]);
            // Legacy deformation: the elbow at x=1 moves to x=4 and is not
            // further scaled by the parent in the mapping of vertex (2, 1, 0).
            Vector3f mapped = skin.mult(new Vector3f(2f, 1f, 0f));
            c.check(mapped.distance(new Vector3f(4f, 1f, 0f)) <= EPS,
                    "legacy deformation " + mode);

            armature.applyBindPose();
            armature.update();
            c.check(child.getLocalTranslation().distance(new Vector3f(1f, 0f, 0f)) <= EPS,
                    "legacy bind restore translation " + mode);

            checkNextUpdate(c, mode);
        }

        /*
         * Separate path: verify the exact accumulation rule with a
         * Transform-only reference: model scale = parent model scale * local
         * scale / parent local scale, position and rotation unchanged.
         */
        Transform g = xform(1f, 2f, 3f, 0.2f, -0.4f, 0.1f, 1.5f, 2f, 0.8f);
        Transform p = xform(0.5f, -1f, 2f, -0.1f, 0.7f, 0.3f, 2f, 3f, 1.5f);
        Transform ch = xform(1.5f, 2f, 0.25f, 0.6f, 0.2f, -1.1f, 2f, 0.5f, 1.5f);
        Chain chain = new Chain(g, p, ch, SeparateJointModelTransform.class);
        chain.flags(false, true, true);

        Transform grandReference = g.clone();
        Transform parentReference = expectedSeparateModel(grandReference, p,
                g.getScale(), true);
        Transform childReference = expectedSeparateModel(parentReference, ch,
                p.getScale(), true);

        c.check(sameMatrix(chain.modelOf(0), trs(grandReference)),
                "legacy separate grandparent model");
        c.check(sameMatrix(chain.modelOf(1), trs(parentReference)),
                "legacy separate parent model scale rule");
        c.check(sameMatrix(chain.modelOf(2), trs(childReference)),
                "legacy separate child model scale rule");

        /*
         * Position and rotation of the separate path must not be affected by
         * the compensation (only the model scale changes). Compare the
         * compensated model transform against an uncompensated one built from
         * the same positions and rotations.
         */
        Transform parentUncompensated = p.clone().combineWithParent(grandReference);
        c.check(parentReference.getTranslation().distance(
                parentUncompensated.getTranslation()) <= EPS,
                "legacy separate compensation leaves translation");
        c.check(Math.abs(Math.abs(parentReference.getRotation().dot(
                parentUncompensated.getRotation())) - 1f) <= EPS,
                "legacy separate compensation leaves rotation");

        /*
         * Root flag is irrelevant for the separate implementation too.
         */
        chain.flags(true, false, false);
        Matrix4f[] rootOn = chain.armature.computeSkinningMatrices();
        Matrix4f rootOnParent = new Matrix4f(rootOn[1]);
        chain.flags(false, false, false);
        Matrix4f[] rootOff = chain.armature.computeSkinningMatrices();
        c.check(sameMatrix(rootOnParent, rootOff[1]),
                "legacy separate root flag does not change result");
    }

    // ------------------------------------------------------------------
    // lifecycle: bind pose, imported bind matrices, clone and J3O round trip
    // ------------------------------------------------------------------

    public static void lifecycle(TaskChecks c) {
        Transform g = xform(1f, 2f, 3f, 0.2f, -0.4f, 0.1f, 1.5f, 2f, 0.8f);
        Transform p = xform(0.5f, -1f, 2f, -0.1f, 0.7f, 0.3f, 2f, -3f, 1.5f);
        Transform ch = xform(1.5f, 2f, 0.25f, 0.6f, 0.2f, -1.1f, 2f, 0.5f, 1.5f);

        for (Class<? extends JointModelTransform> mode : modes()) {
            Chain chain = new Chain(g, p, ch, mode);
            chain.flags(false, true, true);

            chain.armature.saveBindPose();
            Matrix4f[] bindSkin = chain.armature.computeSkinningMatrices();
            for (int i = 0; i < 3; ++i) {
                c.check(sameMatrix(bindSkin[i], Matrix4f.IDENTITY),
                        "bind pose skinning identity " + mode + " #" + i);
            }

            /*
             * Perturb all local TRS, then restore. The restored local TRS must
             * recompose to the saved one and the skinning matrices must return
             * to identity, repeatedly, without accumulating error.
             */
            chain.grand.setLocalTranslation(new Vector3f(7f, -2f, 1f));
            chain.grand.setLocalRotation(new Quaternion().fromAngles(1f, 2f, 3f));
            chain.grand.setLocalScale(new Vector3f(0.3f, -2f, 4f));
            chain.parent.setLocalTranslation(new Vector3f(-3f, 4f, 0.5f));
            chain.parent.setLocalRotation(new Quaternion().fromAngles(-1f, 0.2f, 0.6f));
            chain.parent.setLocalScale(new Vector3f(-1.5f, 2.5f, 0.75f));
            chain.child.setLocalTranslation(new Vector3f(0.1f, -0.2f, 0.3f));
            chain.child.setLocalRotation(new Quaternion().fromAngles(0.4f, -0.9f, 1.3f));
            chain.child.setLocalScale(new Vector3f(3f, 0.4f, -2.2f));
            chain.armature.update();

            for (int iteration = 0; iteration < 3; ++iteration) {
                chain.armature.applyBindPose();
                c.check(sameTransform(chain.grand.getLocalTransform(), g),
                        "bind restore grandparent " + mode + " iter " + iteration);
                c.check(sameTransform(chain.parent.getLocalTransform(), p),
                        "bind restore parent " + mode + " iter " + iteration);
                c.check(sameTransform(chain.child.getLocalTransform(), ch),
                        "bind restore child " + mode + " iter " + iteration);
                Matrix4f[] restored = chain.armature.computeSkinningMatrices();
                for (int i = 0; i < 3; ++i) {
                    c.check(sameMatrix(restored[i], Matrix4f.IDENTITY),
                            "bind restore skinning identity " + mode + " iter "
                            + iteration + " #" + i);
                }
            }

            /*
             * Imported armature: only setInverseModelBindMatrix() is used and
             * saveBindPose() is never called. Capturing the current model
             * matrices as binds and then restoring must produce a representable
             * local TRS that reproduces those bind model matrices, even with
             * compensation. A reflection (negative scale) does not decompose
             * uniquely, so the pose is compared through the recomposed model
             * matrices rather than through individual scale signs.
             */
            Chain imported = new Chain(g, p, ch, mode);
            imported.flags(false, true, true);
            Matrix4f[] models = imported.armature.computeSkinningMatrices();
            Matrix4f bindModelGrand = new Matrix4f(models[0]);
            Matrix4f bindModelParent = new Matrix4f(models[1]);
            Matrix4f bindModelChild = new Matrix4f(models[2]);
            for (int i = 0; i < 3; ++i) {
                imported.armature.getJoint(i).setInverseModelBindMatrix(
                        new Matrix4f(models[i]).invertLocal());
            }
            Transform bindGrand = imported.grand.getLocalTransform().clone();

            imported.grand.setLocalTranslation(new Vector3f(9f, 9f, 9f));
            imported.parent.setLocalRotation(new Quaternion().fromAngles(1f, 1f, 1f));
            imported.parent.setLocalScale(new Vector3f(0.25f, -2f, 3f));
            imported.child.setLocalTranslation(new Vector3f(-4f, 5f, 6f));
            imported.armature.update();

            imported.armature.applyBindPose();
            c.check(sameTransform(imported.grand.getLocalTransform(), bindGrand),
                    "imported bind restore root " + mode);
            /*
             * With the bind matrices as the only input, a restored pose must
             * reproduce the bind model matrices. That is exactly what identity
             * skinning matrices mean, so check both views.
             */
            c.check(sameMatrix(imported.worldModelOf(0), bindModelGrand),
                    "imported bind model grandparent " + mode);
            c.check(sameMatrix(imported.worldModelOf(1), bindModelParent),
                    "imported bind model parent " + mode);
            c.check(sameMatrix(imported.worldModelOf(2), bindModelChild),
                    "imported bind model child " + mode);
            Matrix4f[] importedSkin = imported.armature.computeSkinningMatrices();
            for (int i = 0; i < 3; ++i) {
                c.check(sameMatrix(importedSkin[i], Matrix4f.IDENTITY),
                        "imported bind skinning identity " + mode + " #" + i);
            }

            /*
             * Re-setting the bind matrices must not keep using the old
             * snapshot: applyBindPose() afterwards has to follow the new
             * matrices.
             */
            imported.grand.setLocalScale(new Vector3f(3f, 1f, 1f));
            imported.armature.update();
            Matrix4f newModelGrand = imported.worldModelOf(0);
            Matrix4f newModelParent = imported.worldModelOf(1);
            Matrix4f newModelChild = imported.worldModelOf(2);
            Matrix4f[] newModels = {newModelGrand, newModelParent, newModelChild};
            for (int i = 0; i < 3; ++i) {
                imported.armature.getJoint(i).setInverseModelBindMatrix(
                        new Matrix4f(newModels[i]).invertLocal());
            }
            imported.armature.applyBindPose();
            c.check(sameMatrix(imported.worldModelOf(0), newModelGrand),
                    "re-set bind matrices follow new snapshot grandparent " + mode);
            c.check(sameMatrix(imported.worldModelOf(1), newModelParent),
                    "re-set bind matrices follow new snapshot parent " + mode);
            c.check(sameMatrix(imported.worldModelOf(2), newModelChild),
                    "re-set bind matrices follow new snapshot child " + mode);
        }

        /*
         * Bind restore must also work when local scales are negative, which is
         * the ambiguous decomposition case. The restored local TRS must
         * reproduce the bind model matrices exactly and the skinning matrices
         * must return to identity, repeatedly.
         */
        Transform negG = xform(-1f, 2f, 3f, 0.3f, -0.5f, 0.2f, -2f, 3f, 1.5f);
        Transform negP = xform(0.5f, -1f, 2f, -0.1f, 0.7f, 0.3f, -1f, -2f, 2.5f);
        Transform negC = xform(1.5f, 2f, 0.25f, 0.6f, 0.2f, -1.1f, 2f, -0.5f, 1.5f);
        for (Class<? extends JointModelTransform> mode : modes()) {
            Chain negative = new Chain(negG, negP, negC, mode);
            negative.flags(false, true, true);
            negative.armature.saveBindPose();
            Matrix4f bindGrand = negative.worldModelOf(0);
            Matrix4f bindParent = negative.worldModelOf(1);
            Matrix4f bindChild = negative.worldModelOf(2);

            negative.grand.setLocalScale(new Vector3f(0.3f, 2f, 4f));
            negative.parent.setLocalRotation(new Quaternion().fromAngles(1f, 1f, 1f));
            negative.parent.setLocalScale(new Vector3f(0.5f, 4f, 3f));
            negative.child.setLocalTranslation(new Vector3f(-4f, 5f, 6f));
            negative.child.setLocalScale(new Vector3f(2f, 2f, -3f));
            negative.armature.update();

            for (int iteration = 0; iteration < 2; ++iteration) {
                negative.armature.applyBindPose();
                c.check(sameMatrix(negative.worldModelOf(0), bindGrand),
                        "negative bind model grandparent " + mode + " iter " + iteration);
                c.check(sameMatrix(negative.worldModelOf(1), bindParent),
                        "negative bind model parent " + mode + " iter " + iteration);
                c.check(sameMatrix(negative.worldModelOf(2), bindChild),
                        "negative bind model child " + mode + " iter " + iteration);
                Matrix4f[] skin = negative.armature.computeSkinningMatrices();
                for (int i = 0; i < 3; ++i) {
                    c.check(sameMatrix(skin[i], Matrix4f.IDENTITY),
                            "negative bind skinning identity " + mode + " iter "
                            + iteration + " #" + i);
                }
            }
        }

        /*
         * Clone and in-memory J3O round trip: the flag, the bind data and the
         * ability to update are preserved. Changing the switch on the clone
         * must not affect the original.
         */
        Chain original = new Chain(g, p, ch, MatrixJointModelTransform.class);
        original.flags(false, true, true);
        original.armature.saveBindPose();
        Matrix4f[] originalSkin = original.armature.computeSkinningMatrices();
        Matrix4f originalParentSkin = new Matrix4f(originalSkin[1]);
        Matrix4f originalChildSkin = new Matrix4f(originalSkin[2]);

        Armature clone = Cloner.deepClone(original.armature);
        c.check(clone.getJoint(0) != original.grand, "clone holds distinct joints");
        c.check(clone.getJoint(1).isSegmentScaleCompensate(),
                "clone preserves parent flag");
        c.check(clone.getJoint(2).isSegmentScaleCompensate(),
                "clone preserves child flag");
        clone.update();
        Matrix4f[] cloneSkin = clone.computeSkinningMatrices();
        c.check(sameMatrix(cloneSkin[1], originalParentSkin),
                "clone reproduces parent skinning");
        c.check(sameMatrix(cloneSkin[2], originalChildSkin),
                "clone reproduces child skinning");

        clone.getJoint(1).setSegmentScaleCompensate(false);
        clone.getJoint(2).setSegmentScaleCompensate(false);
        c.check(!clone.getJoint(1).isSegmentScaleCompensate(),
                "clone flag can be changed");
        c.check(original.parent.isSegmentScaleCompensate(),
                "changing clone flag leaves original unchanged");
        clone.update();
        c.check(!sameMatrix(clone.computeSkinningMatrices()[1], originalParentSkin),
                "clone skinning responds to its own flag");
        original.armature.update();
        c.check(sameMatrix(original.armature.computeSkinningMatrices()[1],
                originalParentSkin), "original skinning still unchanged");

        /*
         * J3O round trip. Joints currently serialize their initial transform,
         * so record the initial pose first; then the round-trip file can
         * restore the same state.
         */
        original.armature.saveInitialPose();
        for (int i = 0; i < 3; ++i) {
            original.armature.getJoint(i).setInverseModelBindMatrix(
                    new Matrix4f(original.armature.computeSkinningMatrices()[i]));
        }
        original.armature.saveBindPose();
        Matrix4f[] beforeIo = original.armature.computeSkinningMatrices();
        Matrix4f beforeIoParent = new Matrix4f(beforeIo[1]);
        Matrix4f beforeIoChild = new Matrix4f(beforeIo[2]);

        Armature loaded = roundTrip(original.armature);
        c.check(loaded.getJointCount() == 3, "J3O keeps joint count");
        c.check(loaded.getJoint(1).isSegmentScaleCompensate(),
                "J3O preserves parent flag");
        c.check(loaded.getJoint(2).isSegmentScaleCompensate(),
                "J3O preserves child flag");
        c.check(!loaded.getJoint(0).isSegmentScaleCompensate(),
                "J3O preserves disabled grandparent flag");
        c.check(sameMatrix(loaded.computeSkinningMatrices()[1], beforeIoParent),
                "J3O preserves parent skinning");
        c.check(sameMatrix(loaded.computeSkinningMatrices()[2], beforeIoChild),
                "J3O preserves child skinning");

        // The loaded armature must still update after the flag changes.
        loaded.getJoint(0).setLocalScale(new Vector3f(4f, 1f, 1f));
        loaded.update();
        c.check(!sameMatrix(loaded.computeSkinningMatrices()[1], beforeIoParent),
                "J3O loaded armature updates");
        loaded.applyBindPose();
        c.check(sameMatrix(loaded.computeSkinningMatrices()[1], Matrix4f.IDENTITY),
                "J3O loaded armature can restore its bind pose");
    }

    private static Armature roundTrip(Armature armature) {
        try {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            new BinaryExporter().save(armature, out);
            return (Armature) new BinaryImporter().load(out.toByteArray());
        } catch (IOException ex) {
            throw new AssertionError(ex);
        }
    }
}
