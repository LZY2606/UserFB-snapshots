import com.jme3.anim.Armature;
import com.jme3.anim.Joint;
import com.jme3.anim.MatrixJointModelTransform;
import com.jme3.anim.SeparateJointModelTransform;
import com.jme3.export.binary.BinaryExporter;
import com.jme3.export.binary.BinaryImporter;
import com.jme3.math.Matrix4f;
import com.jme3.math.Quaternion;
import com.jme3.math.Transform;
import com.jme3.math.Vector3f;
import com.jme3.util.clone.Cloner;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * Test harness for per-joint segment scale compensation.
 */
public final class SegmentScaleTests {

    private static final float EPS = 1e-4f;

    private SegmentScaleTests() {
    }

    // ------------------------------------------------------------------
    // Entry points
    // ------------------------------------------------------------------

    public static void matrix(TaskChecks c) {
        matrixModelValues(c, MatrixJointModelTransform.class);
        matrixModelValues(c, SeparateJointModelTransform.class);
        matrixInterleaved(c, MatrixJointModelTransform.class);
        matrixInterleaved(c, SeparateJointModelTransform.class);
        matrixNonCommutative(c);
        matrixNegativeScale(c, MatrixJointModelTransform.class);
        matrixNegativeScale(c, SeparateJointModelTransform.class);
        matrixRootToggle(c, MatrixJointModelTransform.class);
        matrixRootToggle(c, SeparateJointModelTransform.class);
        matrixDefaultOff(c, MatrixJointModelTransform.class);
        matrixDefaultOff(c, SeparateJointModelTransform.class);
        matrixSkinning(c, MatrixJointModelTransform.class);
        matrixSkinning(c, SeparateJointModelTransform.class);
        matrixToggleNextUpdate(c, MatrixJointModelTransform.class);
        matrixToggleNextUpdate(c, SeparateJointModelTransform.class);
    }

    public static void legacy(TaskChecks c) {
        legacyUnchanged(c, MatrixJointModelTransform.class);
        legacyUnchanged(c, SeparateJointModelTransform.class);
        legacyLocalAnimationCalls(c, MatrixJointModelTransform.class);
        legacyLocalAnimationCalls(c, SeparateJointModelTransform.class);
        legacyNegativeParentDefault(c, MatrixJointModelTransform.class);
        legacyNegativeParentDefault(c, SeparateJointModelTransform.class);
    }

    public static void lifecycle(TaskChecks c) {
        bindPoseRoundTrip(c, MatrixJointModelTransform.class);
        bindPoseRoundTrip(c, SeparateJointModelTransform.class);
        bindPoseNoAccumulation(c, MatrixJointModelTransform.class);
        bindPoseNoAccumulation(c, SeparateJointModelTransform.class);
        bindPoseFromInverseMatrices(c, MatrixJointModelTransform.class);
        bindPoseFromInverseMatrices(c, SeparateJointModelTransform.class);
        bindResetsAfterMatrixReset(c, MatrixJointModelTransform.class);
        bindResetsAfterMatrixReset(c, SeparateJointModelTransform.class);
        bindNegativeScaleRoundTrip(c, MatrixJointModelTransform.class);
        bindNegativeScaleRoundTrip(c, SeparateJointModelTransform.class);
        cloneRoundTrip(c);
        j3oRoundTrip(c);
        legacyJ3oDefaultsOff(c);
    }

    // ------------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------------

    private static Armature buildChain(Class<? extends com.jme3.anim.util.JointModelTransform> mode,
                                       Transform[] locals) {
        Joint[] joints = new Joint[locals.length];
        for (int i = 0; i < locals.length; i++) {
            Joint j = new Joint("j" + i);
            j.setLocalTransform(locals[i]);
            if (i > 0) {
                joints[i - 1].addChild(j);
            }
            joints[i] = j;
        }
        Armature arm = new Armature(joints);
        arm.setModelTransformClass(mode);
        arm.update();
        return arm;
    }

    private static void checkMatrix(TaskChecks c, Matrix4f actual, Matrix4f expected, String what) {
        float max = 0f;
        for (int i = 0; i < 16; i++) {
            max = Math.max(max, Math.abs(el(actual, i) - el(expected, i)));
        }
        c.check(max < EPS, what + " matrix differs by " + max);
    }

    private static float el(Matrix4f m, int i) {
        return m.get(i / 4, i % 4);
    }

    private static Matrix4f modelMatrix(Armature arm, int index) {
        // There is no public accessor for the accumulated model matrix, so
        // recover it through skinning with a temporary identity bind matrix:
        // skinning = model * inverseBind.
        Joint j = arm.getJoint(index);
        j.updateModelTransforms();
        Matrix4f saved = j.getInverseModelBindMatrix();
        j.setInverseModelBindMatrix(new Matrix4f(Matrix4f.IDENTITY));
        Matrix4f result = new Matrix4f(arm.computeSkinningMatrices()[index]);
        j.setInverseModelBindMatrix(saved);
        return result;
    }

    private static Transform[] chainLocals() {
        Transform[] locals = new Transform[3];
        locals[0] = new Transform(new Vector3f(0.5f, -0.25f, 0.75f),
                new Quaternion().fromAngles(0.3f, -0.2f, 0.1f),
                new Vector3f(1.5f, 2f, 0.5f));
        locals[1] = new Transform(new Vector3f(1.25f, 0.5f, -0.75f),
                new Quaternion().fromAngles(-0.4f, 0.7f, 0.2f),
                new Vector3f(0.75f, 1.25f, 1.5f));
        locals[2] = new Transform(new Vector3f(-0.5f, 1f, 0.25f),
                new Quaternion().fromAngles(0.55f, 0.15f, -0.65f),
                new Vector3f(1.1f, 0.9f, 1.3f));
        return locals;
    }

    private static Transform copy(Transform t) {
        return new Transform(t.getTranslation().clone(), t.getRotation().clone(), t.getScale().clone());
    }

    private static Matrix4f t(Transform tr) {
        return tr.toTransformMatrix();
    }

    private static Matrix4f s(Vector3f scale) {
        Matrix4f m = new Matrix4f();
        m.setScale(scale);
        return m;
    }

    private static Matrix4f p(Transform tr) {
        Matrix4f m = new Matrix4f();
        m.setTranslation(tr.getTranslation());
        return m;
    }

    private static Matrix4f rs(Transform tr) {
        Matrix4f m = new Matrix4f();
        tr.getRotation().toTransformMatrix(m);
        m.setScale(tr.getScale());
        return m;
    }

    // ------------------------------------------------------------------
    // matrix: model values against independent composition
    // ------------------------------------------------------------------

    private static void matrixModelValues(TaskChecks c,
                                          Class<? extends com.jme3.anim.util.JointModelTransform> mode) {
        Transform[] locals = chainLocals();
        Armature arm = buildChain(mode, locals);
        boolean matrixMode = mode == MatrixJointModelTransform.class;

        for (int i = 0; i < 3; i++) {
            arm.getJoint(i).setSegmentScaleCompensate(true);
        }
        arm.getJoint(0).setSegmentScaleCompensate(false); // root flag must not matter
        arm.update();

        if (matrixMode) {
            // Compute expected model matrices independently:
            // root: T0 * R0 * S0
            // child: P * T(t) * inv(S_parent_local) * R(q) * S(s)
            Matrix4f m0 = p(locals[0]).mult(rs(locals[0]));
            Matrix4f m1 = m0.mult(p(locals[1])).mult(s(locals[0].getScale()).invertLocal()).mult(rs(locals[1]));
            Matrix4f m2 = m1.mult(p(locals[2])).mult(s(locals[1].getScale()).invertLocal()).mult(rs(locals[2]));
            checkMatrix(c, modelMatrix(arm, 0), m0, mode.getSimpleName() + " model0");
            checkMatrix(c, modelMatrix(arm, 1), m1, mode.getSimpleName() + " model1");
            checkMatrix(c, modelMatrix(arm, 2), m2, mode.getSimpleName() + " model2");

            // Translation of child must still be scaled by parent's scale.
            Vector3f origin = new Vector3f();
            modelMatrix(arm, 2).mult(new Vector3f(0, 0, 0), origin);
            Vector3f expectedOrigin = m2.mult(new Vector3f(0, 0, 0));
            c.check(origin.distance(expectedOrigin) < EPS, "model2 origin");
        } else {
            // SeparateJointModelTransform: position/rotation by usual rules;
            // model scale is parentModelScale * localScale / parentLocalScale.
            Transform m0 = copy(locals[0]);
            Transform m1 = copy(locals[1]);
            m1.combineWithParent(m0);
            m1.getScale().divideLocal(locals[0].getScale());
            Transform m2 = copy(locals[2]);
            m2.combineWithParent(m1);
            m2.getScale().divideLocal(locals[1].getScale());

            checkMatrix(c, modelMatrix(arm, 0), t(m0), mode.getSimpleName() + " sep model0");
            checkMatrix(c, modelMatrix(arm, 1), t(m1), mode.getSimpleName() + " sep model1");
            checkMatrix(c, modelMatrix(arm, 2), t(m2), mode.getSimpleName() + " sep model2");

            // Skinning result should match an independent composition of the
            // model transform with the saved inverse bind (identity bind here).
            Matrix4f skin2 = arm.computeSkinningMatrices()[2];
            checkMatrix(c, skin2, t(m2), mode.getSimpleName() + " sep skin2");
        }

        // Skinning matrices with bind saved at this pose must be identity.
        arm.saveBindPose();
        Matrix4f[] skin = arm.computeSkinningMatrices();
        for (int i = 0; i < skin.length; i++) {
            c.check(isIdentity(skin[i]), mode.getSimpleName() + " bind skin identity " + i);
        }
    }

    private static boolean isIdentity(Matrix4f m) {
        for (int r = 0; r < 4; r++) {
            for (int col = 0; col < 4; col++) {
                float expected = r == col ? 1f : 0f;
                if (Math.abs(m.get(r, col) - expected) > EPS) {
                    return false;
                }
            }
        }
        return true;
    }

    private static void matrixInterleaved(TaskChecks c,
                                          Class<? extends com.jme3.anim.util.JointModelTransform> mode) {
        // Toggle off/on/off along the chain; verify each joint independently.
        Transform[] locals = chainLocals();
        Armature arm = buildChain(mode, locals);
        arm.getJoint(0).setSegmentScaleCompensate(false);
        arm.getJoint(1).setSegmentScaleCompensate(true);
        arm.getJoint(2).setSegmentScaleCompensate(false);
        arm.update();

        boolean matrixMode = mode == MatrixJointModelTransform.class;
        if (matrixMode) {
            Matrix4f m0 = p(locals[0]).mult(rs(locals[0]));
            Matrix4f m1 = m0.mult(p(locals[1])).mult(s(locals[0].getScale()).invertLocal()).mult(rs(locals[1]));
            Matrix4f m2 = m1.mult(p(locals[2])).mult(rs(locals[2])); // off
            checkMatrix(c, modelMatrix(arm, 0), m0, "interleaved model0");
            checkMatrix(c, modelMatrix(arm, 1), m1, "interleaved model1");
            checkMatrix(c, modelMatrix(arm, 2), m2, "interleaved model2");
        } else {
            Transform m0 = copy(locals[0]);
            Transform m1 = copy(locals[1]);
            m1.combineWithParent(m0);
            m1.getScale().divideLocal(locals[0].getScale());
            Transform m2 = copy(locals[2]);
            m2.combineWithParent(m1);
            checkMatrix(c, modelMatrix(arm, 1), t(m1), "interleaved sep model1");
            checkMatrix(c, modelMatrix(arm, 2), t(m2), "interleaved sep model2");
        }

        // Mixed toggling must yield distinct results per joint, proving the flag
        // is not global.
        Vector3f on = new Vector3f();
        Vector3f off = new Vector3f();
        arm.getJoint(1).setSegmentScaleCompensate(true);
        arm.getJoint(2).setSegmentScaleCompensate(true);
        arm.update();
        modelMatrix(arm, 2).mult(new Vector3f(1, 0.5f, -0.25f), on);
        arm.getJoint(2).setSegmentScaleCompensate(false);
        arm.update();
        modelMatrix(arm, 2).mult(new Vector3f(1, 0.5f, -0.25f), off);
        c.check(on.distance(off) > 1e-3f, "interleaved toggling has effect");
    }

    private static void matrixNonCommutative(TaskChecks c) {
        // Parent non-uniform scale + child rotation do not commute: verify that
        // the compensated matrix keeps the child's rotation applied after the
        // scale removal, and preserves the resulting shear.
        Transform parentLocal = new Transform(new Vector3f(0.2f, -0.1f, 0.3f),
                new Quaternion().fromAngles(0.1f, 0.4f, -0.2f),
                new Vector3f(1.5f, 3f, 0.5f));
        Transform childLocal = new Transform(new Vector3f(0.7f, -0.4f, 1.1f),
                new Quaternion().fromAngles(0.9f, -0.3f, 0.6f),
                new Vector3f(1f, 1f, 1f));

        Armature arm = buildChain(MatrixJointModelTransform.class,
                new Transform[]{parentLocal, childLocal});
        arm.getJoint(1).setSegmentScaleCompensate(true);
        arm.update();

        Matrix4f p0 = p(parentLocal).mult(rs(parentLocal));
        Matrix4f expected = p0.mult(p(childLocal))
                .mult(s(parentLocal.getScale()).invertLocal())
                .mult(rs(childLocal));
        checkMatrix(c, modelMatrix(arm, 1), expected, "noncommutative model1");

        // The linear part of the rotated child under nonuniform parent scale
        // must contain shear: verify by comparing to a naive rotation-only
        // composition which would differ.
        Matrix4f naive = p0.mult(p(childLocal)).mult(rs(childLocal));
        Matrix4f actual = modelMatrix(arm, 1);
        // Pick a basis vector that is not aligned with parent axes.
        Vector3f v = new Vector3f(0.3f, 0.8f, -0.5f);
        Vector3f a = actual.mult(v);
        Vector3f n = naive.mult(v);
        c.check(a.distance(n) > 1e-3f, "shear/compensation changes child transform");

        // Rotation must not be lost: compare rotation of actual vs expected.
        Quaternion qa = actual.toRotationQuat();
        Quaternion qe = expected.toRotationQuat();
        c.check(Math.abs(qa.dot(qe)) > 0.999f, "child rotation preserved");
    }

    private static void matrixNegativeScale(TaskChecks c,
                                            Class<? extends com.jme3.anim.util.JointModelTransform> mode) {
        Transform parentLocal = new Transform(new Vector3f(0.1f, 0.2f, -0.3f),
                new Quaternion().fromAngles(0.2f, -0.5f, 0.3f),
                new Vector3f(-2f, 1.5f, 0.75f));
        Transform childLocal = new Transform(new Vector3f(-0.4f, 0.6f, 0.2f),
                new Quaternion().fromAngles(0.4f, 0.25f, -0.35f),
                new Vector3f(1f, 1f, 1f));
        Armature arm = buildChain(mode, new Transform[]{parentLocal, childLocal});
        arm.getJoint(1).setSegmentScaleCompensate(true);
        arm.update();

        boolean matrixMode = mode == MatrixJointModelTransform.class;
        Matrix4f expected;
        if (matrixMode) {
            Matrix4f p0 = p(parentLocal).mult(rs(parentLocal));
            expected = p0.mult(p(childLocal))
                    .mult(s(parentLocal.getScale()).invertLocal())
                    .mult(rs(childLocal));
            checkMatrix(c, modelMatrix(arm, 1), expected, "negative scale model1");
        } else {
            Transform m1 = copy(childLocal);
            Transform m0 = copy(parentLocal);
            m1.combineWithParent(m0);
            m1.getScale().divideLocal(parentLocal.getScale());
            expected = t(m1);
            checkMatrix(c, modelMatrix(arm, 1), expected, "negative scale sep model1");
        }

        // The result must be finite.
        for (int i = 0; i < 16; i++) {
            c.check(Float.isFinite(actualElement(modelMatrix(arm, 1), i)), "negative scale finite");
        }
    }

    private static float actualElement(Matrix4f m, int i) {
        return m.get(i / 4, i % 4);
    }

    private static void matrixRootToggle(TaskChecks c,
                                         Class<? extends com.jme3.anim.util.JointModelTransform> mode) {
        Transform[] locals = chainLocals();
        Armature arm = buildChain(mode, locals);
        arm.getJoint(1).setSegmentScaleCompensate(true);
        arm.update();
        Matrix4f before = modelMatrix(arm, 0);
        arm.getJoint(0).setSegmentScaleCompensate(true);
        arm.update();
        c.check(modelMatrix(arm, 0).equals(before), "root toggle does not change root");
        Joint root = arm.getJoint(0);
        c.check(root.isSegmentScaleCompensate(), "root flag readable");
    }

    private static void matrixDefaultOff(TaskChecks c,
                                         Class<? extends com.jme3.anim.util.JointModelTransform> mode) {
        Transform[] locals = chainLocals();
        Armature arm = buildChain(mode, locals);
        for (int i = 0; i < 3; i++) {
            c.check(!arm.getJoint(i).isSegmentScaleCompensate(), "default off " + i);
        }
        arm.update();
        // Compare to the legacy composition.
        Matrix4f legacyRoot = p(locals[0]).mult(rs(locals[0]));
        Matrix4f legacy1 = legacyRoot.mult(p(locals[1])).mult(rs(locals[1]));
        Matrix4f legacy2 = legacy1.mult(p(locals[2])).mult(rs(locals[2]));
        if (mode == MatrixJointModelTransform.class) {
            checkMatrix(c, modelMatrix(arm, 1), legacy1, "default off model1");
            checkMatrix(c, modelMatrix(arm, 2), legacy2, "default off model2");
        }
    }

    private static void matrixSkinning(TaskChecks c,
                                       Class<? extends com.jme3.anim.util.JointModelTransform> mode) {
        // Verify skinning matrices (not just model transforms) against an
        // independent composition, with interleaved flags on a 3-level chain.
        Transform[] locals = chainLocals();
        Armature arm = buildChain(mode, locals);
        boolean[] flags = {false, true, false};
        for (int i = 0; i < 3; i++) {
            arm.getJoint(i).setSegmentScaleCompensate(flags[i]);
        }
        arm.update();

        // Bind at a deliberately different pose, then animate.
        arm.saveBindPose();
        Transform[] bindLocals = new Transform[3];
        for (int i = 0; i < 3; i++) {
            bindLocals[i] = copy(arm.getJoint(i).getLocalTransform());
        }
        // Animate: new local TRS.
        Transform[] anim = new Transform[3];
        for (int i = 0; i < 3; i++) {
            anim[i] = copy(locals[i]);
            anim[i].getTranslation().addLocal(0.4f * (i + 1), -0.2f, 0.3f);
            anim[i].getRotation().fromAngles(0.3f + 0.1f * i, -0.4f, 0.2f);
            anim[i].getScale().multLocal(1.2f, 0.9f, 1.3f);
            arm.getJoint(i).setLocalTransform(anim[i]);
        }
        arm.update();

        boolean matrixMode = mode == MatrixJointModelTransform.class;
        Matrix4f[] bindModels = new Matrix4f[3];
        Matrix4f[] animModels = new Matrix4f[3];
        if (matrixMode) {
            bindModels[0] = p(bindLocals[0]).mult(rs(bindLocals[0]));
            animModels[0] = p(anim[0]).mult(rs(anim[0]));
            for (int i = 1; i < 3; i++) {
                if (flags[i]) {
                    bindModels[i] = bindModels[i - 1].mult(p(bindLocals[i]))
                            .mult(s(bindLocals[i - 1].getScale()).invertLocal()).mult(rs(bindLocals[i]));
                    animModels[i] = animModels[i - 1].mult(p(anim[i]))
                            .mult(s(anim[i - 1].getScale()).invertLocal()).mult(rs(anim[i]));
                } else {
                    bindModels[i] = bindModels[i - 1].mult(p(bindLocals[i])).mult(rs(bindLocals[i]));
                    animModels[i] = animModels[i - 1].mult(p(anim[i])).mult(rs(anim[i]));
                }
            }
        } else {
            bindModels[0] = p(bindLocals[0]).mult(rs(bindLocals[0]));
            animModels[0] = p(anim[0]).mult(rs(anim[0]));
            Transform[] bindT = new Transform[3];
            Transform[] animT = new Transform[3];
            bindT[0] = copy(bindLocals[0]);
            animT[0] = copy(anim[0]);
            for (int i = 1; i < 3; i++) {
                bindT[i] = copy(bindLocals[i]);
                bindT[i].combineWithParent(bindT[i - 1]);
                animT[i] = copy(anim[i]);
                animT[i].combineWithParent(animT[i - 1]);
                if (flags[i]) {
                    bindT[i].getScale().divideLocal(bindLocals[i - 1].getScale());
                    animT[i].getScale().divideLocal(anim[i - 1].getScale());
                }
            }
            for (int i = 0; i < 3; i++) {
                bindModels[i] = bindT[i].toTransformMatrix();
                animModels[i] = animT[i].toTransformMatrix();
            }
        }

        Matrix4f[] skin = arm.computeSkinningMatrices();
        for (int i = 0; i < 3; i++) {
            Matrix4f expected = animModels[i].mult(bindModels[i].invert());
            checkMatrix(c, skin[i], expected, mode.getSimpleName() + " skinning composition " + i);
        }
    }

    private static void matrixToggleNextUpdate(TaskChecks c,
                                               Class<? extends com.jme3.anim.util.JointModelTransform> mode) {
        Transform[] locals = chainLocals();
        Armature arm = buildChain(mode, locals);
        arm.saveBindPose();
        arm.update();
        Matrix4f offSkin = new Matrix4f(arm.computeSkinningMatrices()[1]);

        arm.getJoint(1).setSegmentScaleCompensate(true);
        // No update yet: skinning matrices must not have changed.
        Matrix4f stale = new Matrix4f(arm.computeSkinningMatrices()[1]);
        checkMatrix(c, stale, offSkin, "flag change not applied before update");
        arm.update();
        Matrix4f onSkin = new Matrix4f(arm.computeSkinningMatrices()[1]);
        c.check(!onSkin.equals(offSkin), "flag change applied on next update");
    }

    // ------------------------------------------------------------------
    // legacy: default behaviour unchanged
    // ------------------------------------------------------------------

    private static void legacyUnchanged(TaskChecks c,
                                        Class<? extends com.jme3.anim.util.JointModelTransform> mode) {
        // Mirror the original baseline scenario with compensation off.
        Joint parent = new Joint("upper");
        Joint child = new Joint("lower");
        parent.setLocalScale(new Vector3f(2, 3, 1));
        child.setLocalTranslation(new Vector3f(1, 0, 0));
        parent.addChild(child);
        Armature arm = new Armature(new Joint[]{parent, child});
        arm.setModelTransformClass(mode);
        arm.update();
        arm.saveBindPose();
        for (Matrix4f m : arm.computeSkinningMatrices()) {
            c.check(m.isIdentity(), mode.getSimpleName() + " initial skin identity");
        }
        parent.setLocalScale(new Vector3f(4, 3, 1));
        arm.update();
        Matrix4f m = arm.computeSkinningMatrices()[1];
        Vector3f p = m.mult(new Vector3f(2, 1, 0));
        c.check(p.distance(new Vector3f(4, 1, 0)) < 1e-5f, mode.getSimpleName() + " legacy deform " + p);
        arm.applyBindPose();
        arm.update();
        c.check(child.getLocalTranslation().distance(new Vector3f(1, 0, 0)) < 1e-5f,
                mode.getSimpleName() + " legacy bind translation");
    }

    private static void legacyLocalAnimationCalls(TaskChecks c,
                                                  Class<? extends com.jme3.anim.util.JointModelTransform> mode) {
        // Existing animation code mutates local TRS then calls update().
        Transform[] locals = chainLocals();
        Armature arm = buildChain(mode, locals);
        Joint child = arm.getJoint(1);
        child.setSegmentScaleCompensate(true);
        arm.saveInitialPose();
        child.setLocalTranslation(new Vector3f(2, 0, 0));
        child.getLocalRotation().fromAngles(0.5f, 0.1f, -0.2f);
        child.setLocalScale(new Vector3f(2f, 2f, 2f));
        arm.update();

        c.check(child.getLocalTranslation().distance(new Vector3f(2, 0, 0)) < EPS, "animation local trans");
        Vector3f expected = new Vector3f();
        modelMatrix(arm, 1).mult(new Vector3f(0, 0, 0), expected);
        // Translation is still stretched by the parent's model scale along each axis.
        Vector3f scaleOnly = new Vector3f(2, 0, 0);
        scaleOnly.multLocal(locals[0].getScale());
        Vector3f parentRotated = locals[0].getRotation().mult(scaleOnly);
        parentRotated.addLocal(locals[0].getTranslation());
        modelMatrix(arm, 0).mult(new Vector3f(0, 0, 0), new Vector3f());
        c.check(expected.distance(parentRotated) < 1e-3f,
                "compensated joint origin still translated by parent scale");
        arm.applyInitialPose();
        arm.update();
        c.check(child.getLocalTranslation().distance(locals[1].getTranslation()) < 1e-3f,
                "applyInitialPose restores animated local");
    }

    private static void legacyNegativeParentDefault(TaskChecks c,
                                                    Class<? extends com.jme3.anim.util.JointModelTransform> mode) {
        // With compensation off, a negative parent scale must still follow the
        // original accumulation rules (no compensation is applied).
        Transform parentLocal = new Transform(new Vector3f(0.4f, 0.1f, -0.2f),
                new Quaternion().fromAngles(0.2f, 0.3f, -0.4f),
                new Vector3f(-1.5f, 2f, 0.5f));
        Transform childLocal = new Transform(new Vector3f(0.6f, -0.8f, 0.2f),
                new Quaternion().fromAngles(-0.2f, 0.5f, 0.1f),
                new Vector3f(0.5f, 1.5f, 2f));
        Armature arm = buildChain(mode, new Transform[]{parentLocal, childLocal});
        arm.update();
        Matrix4f actual = modelMatrix(arm, 1);

        if (mode == MatrixJointModelTransform.class) {
            Matrix4f expected = p(parentLocal).mult(rs(parentLocal))
                    .mult(p(childLocal)).mult(rs(childLocal));
            checkMatrix(c, actual, expected, "negative legacy model1");
        } else {
            Transform m1 = copy(childLocal);
            m1.combineWithParent(copy(parentLocal));
            checkMatrix(c, actual, t(m1), "negative legacy sep model1");
        }

        // Skinning at the bind pose is still identity when compensation is off.
        arm.saveBindPose();
        for (Matrix4f m : arm.computeSkinningMatrices()) {
            c.check(isIdentity(m), mode.getSimpleName() + " negative legacy bind identity");
        }
    }

    private static float[] flatten(Matrix4f m) {
        float[] f = new float[16];
        for (int i = 0; i < 16; i++) {
            f[i] = m.get(i / 4, i % 4);
        }
        return f;
    }

    // ------------------------------------------------------------------
    // lifecycle: bind pose, clone, J3O
    // ------------------------------------------------------------------

    private static void bindPoseRoundTrip(TaskChecks c,
                                          Class<? extends com.jme3.anim.util.JointModelTransform> mode) {
        Transform[] locals = chainLocals();
        Armature arm = buildChain(mode, locals);
        arm.getJoint(1).setSegmentScaleCompensate(true);
        arm.getJoint(2).setSegmentScaleCompensate(true);
        arm.update();
        arm.saveBindPose();
        Matrix4f[] bindSkin = arm.computeSkinningMatrices();
        for (Matrix4f m : bindSkin) {
            c.check(isIdentity(m), mode.getSimpleName() + " bind identity");
        }

        // Store the saved local pose, then alter the hierarchy.
        Transform[] saved = new Transform[3];
        for (int i = 0; i < 3; i++) {
            saved[i] = copy(arm.getJoint(i).getLocalTransform());
        }
        Transform[] altered = chainLocals();
        for (int i = 0; i < 3; i++) {
            altered[i].getTranslation().addLocal(1.3f, -0.7f, 0.9f);
            altered[i].getRotation().fromAngles(0.7f + i, -0.3f, 0.4f);
            altered[i].getScale().multLocal(1.7f, 0.6f, 2.1f);
            arm.getJoint(i).setLocalTransform(altered[i]);
        }
        arm.update();

        arm.applyBindPose();
        arm.update();
        for (int i = 0; i < 3; i++) {
            checkTransformEquivalent(c, arm.getJoint(i).getLocalTransform(), saved[i],
                    mode.getSimpleName() + " restore joint " + i);
        }
        Matrix4f[] restoredSkin = arm.computeSkinningMatrices();
        for (Matrix4f m : restoredSkin) {
            c.check(isIdentity(m), mode.getSimpleName() + " restored skin identity");
        }
    }

    private static void checkTransformEquivalent(TaskChecks c, Transform actual, Transform expected, String what) {
        float trans = actual.getTranslation().distance(expected.getTranslation());
        c.check(trans < 1e-3f, what + " translation " + trans);
        // Compare by recomposing, sign of scale components may differ.
        Matrix4f a = t(actual);
        Matrix4f e = t(expected);
        float max = 0f;
        for (int i = 0; i < 16; i++) {
            max = Math.max(max, Math.abs(a.get(i / 4, i % 4) - e.get(i / 4, i % 4)));
        }
        c.check(max < 2e-3f, what + " recomposed matrix " + max);
        Quaternion qa = actual.getRotation().clone().normalizeLocal();
        Quaternion qe = expected.getRotation().clone().normalizeLocal();
        c.check(Math.abs(qa.dot(qe)) > 0.999f, what + " rotation");
    }

    private static void bindPoseNoAccumulation(TaskChecks c,
                                               Class<? extends com.jme3.anim.util.JointModelTransform> mode) {
        Transform[] locals = chainLocals();
        Armature arm = buildChain(mode, locals);
        arm.getJoint(1).setSegmentScaleCompensate(true);
        arm.getJoint(2).setSegmentScaleCompensate(true);
        arm.saveBindPose();
        Transform[] saved = new Transform[3];
        for (int i = 0; i < 3; i++) {
            saved[i] = copy(arm.getJoint(i).getLocalTransform());
        }
        for (int round = 0; round < 5; round++) {
            arm.applyBindPose();
            arm.update();
        }
        for (int i = 0; i < 3; i++) {
            checkTransformEquivalent(c, arm.getJoint(i).getLocalTransform(), saved[i],
                    mode.getSimpleName() + " no accumulation " + i);
        }
        for (Matrix4f m : arm.computeSkinningMatrices()) {
            c.check(isIdentity(m), mode.getSimpleName() + " no accumulation skin");
        }
    }

    private static void bindPoseFromInverseMatrices(TaskChecks c,
                                                    Class<? extends com.jme3.anim.util.JointModelTransform> mode) {
        // Simulates an imported armature: bind matrices supplied directly via
        // setInverseModelBindMatrix, without calling saveBindPose().
        Transform[] locals = chainLocals();
        Joint[] joints = new Joint[locals.length];
        for (int i = 0; i < locals.length; i++) {
            Joint j = new Joint("j" + i);
            j.setLocalTransform(locals[i]);
            if (i > 0) {
                joints[i - 1].addChild(j);
            }
            joints[i] = j;
        }
        Armature arm = new Armature(joints);
        arm.setModelTransformClass(mode);
        arm.getJoint(1).setSegmentScaleCompensate(true);
        arm.getJoint(2).setSegmentScaleCompensate(true);
        arm.update();

        // Build inverse bind matrices matching the current model transforms.
        Matrix4f[] bind = new Matrix4f[joints.length];
        for (int i = 0; i < joints.length; i++) {
            bind[i] = new Matrix4f(modelMatrix(arm, i)).invertLocal();
        }
        for (int i = 0; i < joints.length; i++) {
            joints[i].setInverseModelBindMatrix(bind[i]);
        }

        // Alter local transforms and then apply the bind pose.
        for (int i = 0; i < joints.length; i++) {
            Transform changed = copy(locals[i]);
            changed.getTranslation().addLocal(-0.3f, 0.8f, 0.4f);
            changed.getRotation().fromAngles(0.2f + i, 0.6f, -0.1f);
            changed.getScale().multLocal(0.7f, 1.4f, 1.1f);
            joints[i].setLocalTransform(changed);
        }
        arm.update();
        arm.applyBindPose();
        arm.update();
        for (int i = 0; i < joints.length; i++) {
            checkTransformEquivalent(c, joints[i].getLocalTransform(), locals[i],
                    mode.getSimpleName() + " imported bind joint " + i);
        }
        for (Matrix4f m : arm.computeSkinningMatrices()) {
            c.check(isIdentity(m), mode.getSimpleName() + " imported bind skin");
        }

        // Re-set the bind matrices to a different pose; a later apply must use
        // the new matrices, not a stale snapshot.
        Transform[] newPose = new Transform[joints.length];
        for (int i = 0; i < joints.length; i++) {
            newPose[i] = copy(locals[i]);
            newPose[i].getTranslation().addLocal(0.2f, -0.1f, 0.3f);
            newPose[i].getRotation().fromAngles(0.1f * i, 0.4f, -0.2f);
            newPose[i].getScale().multLocal(1.2f, 0.8f, 1.1f);
            joints[i].setLocalTransform(newPose[i]);
        }
        arm.update();
        for (int i = 0; i < joints.length; i++) {
            joints[i].setInverseModelBindMatrix(new Matrix4f(modelMatrix(arm, i)).invertLocal());
        }
        // Move away, then restore: should land on newPose.
        for (int i = 0; i < joints.length; i++) {
            Transform moved = copy(newPose[i]);
            moved.getTranslation().addLocal(-0.6f, 0.5f, 0.2f);
            joints[i].setLocalTransform(moved);
        }
        arm.update();
        arm.applyBindPose();
        arm.update();
        for (int i = 0; i < joints.length; i++) {
            checkTransformEquivalent(c, joints[i].getLocalTransform(), newPose[i],
                    mode.getSimpleName() + " rebind joint " + i);
        }
        for (Matrix4f m : arm.computeSkinningMatrices()) {
            c.check(isIdentity(m), mode.getSimpleName() + " rebind skin identity");
        }
    }

    private static void bindNegativeScaleRoundTrip(TaskChecks c,
                                                   Class<? extends com.jme3.anim.util.JointModelTransform> mode) {
        // Root stays positive (a root has no parent, so compensation cannot
        // apply). Its middle child carries a negative local scale, and the
        // leaf compensates for that negative parent scale.
        Transform rootLocal = new Transform(new Vector3f(0.3f, -0.2f, 0.1f),
                new Quaternion().fromAngles(0.25f, -0.35f, 0.45f),
                new Vector3f(1.2f, 1.5f, 0.75f));
        Transform middleLocal = new Transform(new Vector3f(0.4f, -0.6f, 0.8f),
                new Quaternion().fromAngles(-0.5f, 0.3f, 0.2f),
                new Vector3f(-2f, 1.5f, 0.75f));
        Transform leafLocal = new Transform(new Vector3f(0.2f, 0.9f, -0.3f),
                new Quaternion().fromAngles(0.35f, -0.15f, 0.5f),
                new Vector3f(0.8f, 1.2f, 1.6f));

        Armature arm = buildChain(mode, new Transform[]{rootLocal, middleLocal, leafLocal});
        arm.getJoint(1).setSegmentScaleCompensate(true);
        arm.getJoint(2).setSegmentScaleCompensate(true);
        arm.update();
        arm.saveBindPose();

        Transform savedRoot = copy(rootLocal);
        Transform savedMiddle = copy(middleLocal);
        Transform savedLeaf = copy(leafLocal);
        Matrix4f savedRootModel = modelMatrix(arm, 0);
        Matrix4f savedMiddleModel = modelMatrix(arm, 1);
        Matrix4f savedLeafModel = modelMatrix(arm, 2);

        arm.getJoint(0).setLocalScale(new Vector3f(1.3f, 0.4f, 2.2f));
        arm.getJoint(0).setLocalTranslation(new Vector3f(-0.5f, 0.7f, -0.1f));
        arm.getJoint(1).setLocalScale(new Vector3f(0.3f, -1.9f, 0.6f));
        arm.getJoint(1).setLocalTranslation(new Vector3f(1.1f, -0.9f, 0.2f));
        arm.getJoint(2).setLocalTranslation(new Vector3f(-0.7f, 0.5f, 1.4f));
        arm.update();

        arm.applyBindPose();
        arm.update();
        checkTransformEquivalent(c, arm.getJoint(0).getLocalTransform(), savedRoot, "neg bind root");
        checkTransformEquivalent(c, arm.getJoint(1).getLocalTransform(), savedMiddle, "neg bind middle");
        checkTransformEquivalent(c, arm.getJoint(2).getLocalTransform(), savedLeaf, "neg bind leaf");
        checkMatrix(c, modelMatrix(arm, 0), savedRootModel, "neg bind root model");
        checkMatrix(c, modelMatrix(arm, 1), savedMiddleModel, "neg bind middle model");
        checkMatrix(c, modelMatrix(arm, 2), savedLeafModel, "neg bind leaf model");
        for (Matrix4f m : arm.computeSkinningMatrices()) {
            c.check(isIdentity(m), mode.getSimpleName() + " neg bind skin identity");
        }
    }

    private static void bindResetsAfterMatrixReset(TaskChecks c,
                                                   Class<? extends com.jme3.anim.util.JointModelTransform> mode) {
        // Repeatedly resetting the inverse bind matrix must clear any old
        // snapshot rather than continuing to blend it in.
        Transform[] locals = chainLocals();
        Armature arm = buildChain(mode, locals);
        arm.getJoint(1).setSegmentScaleCompensate(true);
        arm.getJoint(2).setSegmentScaleCompensate(true);
        arm.update();
        arm.saveBindPose();

        for (int pass = 0; pass < 3; pass++) {
            // Move everything.
            for (int i = 0; i < 3; i++) {
                Transform moved = copy(arm.getJoint(i).getLocalTransform());
                moved.getTranslation().addLocal(0.05f * (pass + 1), -0.02f * pass, 0.03f);
                arm.getJoint(i).setLocalTransform(moved);
            }
            arm.update();
            // Explicitly reset bind to the current pose using fresh matrices.
            for (int i = 0; i < 3; i++) {
                Joint j = arm.getJoint(i);
                j.setInverseModelBindMatrix(new Matrix4f(modelMatrix(arm, i)).invertLocal());
            }
            Matrix4f[] skin = arm.computeSkinningMatrices();
            for (Matrix4f m : skin) {
                c.check(isIdentity(m), mode.getSimpleName() + " fresh bind matrix identity pass " + pass);
            }
        }
    }

    private static void cloneRoundTrip(TaskChecks c) {
        Transform[] locals = chainLocals();
        Armature arm = buildChain(MatrixJointModelTransform.class, locals);
        arm.getJoint(1).setSegmentScaleCompensate(true);
        arm.getJoint(2).setSegmentScaleCompensate(false);
        arm.update();
        arm.saveBindPose();
        Armature clone = Cloner.deepClone(arm);
        clone.update();

        c.check(clone.getJoint(1) != arm.getJoint(1), "clone is separate instance");
        c.check(clone.getJoint(1).isSegmentScaleCompensate(), "clone keeps on flag");
        c.check(!clone.getJoint(2).isSegmentScaleCompensate(), "clone keeps off flag");

        // Clone skinning matches original at the same pose.
        Matrix4f[] a = arm.computeSkinningMatrices();
        Matrix4f[] b = clone.computeSkinningMatrices();
        for (int i = 0; i < a.length; i++) {
            checkMatrix(c, b[i], a[i], "clone skin " + i);
        }

        // Changing the clone's flag does not affect the original.
        clone.getJoint(1).setSegmentScaleCompensate(false);
        clone.update();
        c.check(arm.getJoint(1).isSegmentScaleCompensate(), "original flag untouched");
        arm.update();
        Matrix4f originalAfter = modelMatrix(arm, 1);
        clone.update();
        Matrix4f cloneAfter = modelMatrix(clone, 1);
        c.check(!sameMatrix(originalAfter, cloneAfter), "clone flag change takes effect on clone");
    }

    private static boolean sameMatrix(Matrix4f a, Matrix4f b) {
        for (int i = 0; i < 16; i++) {
            if (Math.abs(a.get(i / 4, i % 4) - b.get(i / 4, i % 4)) > EPS) {
                return false;
            }
        }
        return true;
    }

    private static void j3oRoundTrip(TaskChecks c) {
        Transform[] locals = chainLocals();
        Armature arm = buildChain(MatrixJointModelTransform.class, locals);
        arm.getJoint(1).setSegmentScaleCompensate(true);
        arm.getJoint(2).setSegmentScaleCompensate(false);
        arm.getJoint(0).setSegmentScaleCompensate(false);
        arm.update();
        arm.saveInitialPose();
        arm.saveBindPose();

        try {
            Armature loaded = exportImport(arm);
            c.check(loaded.getJointCount() == 3, "j3o joint count");
            c.check(loaded.getJoint(1).isSegmentScaleCompensate(), "j3o keeps on flag");
            c.check(!loaded.getJoint(2).isSegmentScaleCompensate(), "j3o keeps off flag");
            loaded.update();
            Matrix4f[] originalSkin = arm.computeSkinningMatrices();
            Matrix4f[] loadedSkin = loaded.computeSkinningMatrices();
            for (int i = 0; i < originalSkin.length; i++) {
                checkMatrix(c, loadedSkin[i], originalSkin[i], "j3o skin " + i);
            }

            // Bind data preserved: skinning at bind pose is identity.
            for (Matrix4f m : loadedSkin) {
                c.check(isIdentity(m), "j3o bind identity");
            }

            // Post-load update capability: alter transform and check it changes.
            loaded.getJoint(1).setLocalTranslation(new Vector3f(0.3f, -0.6f, 1.2f));
            loaded.update();
            Matrix4f before = new Matrix4f(modelMatrix(loaded, 1));
            loaded.getJoint(1).setLocalTranslation(new Vector3f(-0.8f, 0.4f, 0.1f));
            loaded.update();
            c.check(!before.equals(modelMatrix(loaded, 1)), "j3o post load update works");

            // Toggling the flag on the loaded copy behaves like the original.
            // Put both armatures back at the same local pose first.
            loaded.getJoint(1).setLocalTransform(locals[1]);
            arm.getJoint(1).setLocalTransform(locals[1]);
            loaded.getJoint(2).setSegmentScaleCompensate(true);
            loaded.update();
            arm.getJoint(2).setSegmentScaleCompensate(true);
            arm.update();
            checkMatrix(c, modelMatrix(loaded, 2), modelMatrix(arm, 2), "j3o toggle matches original");
        } catch (Exception e) {
            throw new RuntimeException(e);
        }
    }

    private static Armature exportImport(Armature arm) throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        BinaryExporter exporter = BinaryExporter.getInstance();
        exporter.save(arm, out);
        byte[] bytes = out.toByteArray();
        BinaryImporter importer = BinaryImporter.getInstance();
        importer.setAssetManager(null);
        Object loaded = importer.load(new ByteArrayInputStream(bytes));
        return (Armature) loaded;
    }

    private static void legacyJ3oDefaultsOff(TaskChecks c) {
        // A J3O that predates the field is simulated by writing a Joint whose
        // boolean has the default value, since the default is applied when the
        // field is absent from the capsule.
        Transform[] locals = chainLocals();
        Armature arm = buildChain(MatrixJointModelTransform.class, locals);
        arm.update();
        arm.saveBindPose();
        try {
            Armature loaded = exportImport(arm);
            for (int i = 0; i < loaded.getJointCount(); i++) {
                c.check(!loaded.getJoint(i).isSegmentScaleCompensate(),
                        "old j3o defaults off " + i);
            }
        } catch (Exception e) {
            throw new RuntimeException(e);
        }
    }
}
