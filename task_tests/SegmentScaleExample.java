import com.jme3.anim.Armature;
import com.jme3.anim.Joint;
import com.jme3.anim.MatrixJointModelTransform;
import com.jme3.math.Matrix4f;
import com.jme3.math.Quaternion;
import com.jme3.math.Transform;
import com.jme3.math.Vector3f;

/**
 * A windowless example of per-joint segment scale compensation.
 *
 * <p>It builds a small three-joint armature, enables compensation on one child
 * joint, updates the armature and prints the resulting skinning matrices. No
 * {@code Application}, {@code Camera} or graphics device is needed, so it can
 * run with {@code -Djava.awt.headless=true}.
 *
 * <pre>
 *   javac -cp jme3-core.jar SegmentScaleExample.java
 *   java -Djava.awt.headless=true -cp .:jme3-core.jar SegmentScaleExample
 * </pre>
 *
 * <p>The compensation flag belongs to the <em>child</em> joint. With it on,
 * the child's model matrix is
 * {@code P * T(t) * inverse(S_parent_local) * R(q) * S(s)} where {@code P} is
 * the parent's full model matrix. The child's translation is still stretched
 * by the parent, but the parent's local scale no longer stretches the child's
 * own orientation and scale. Without compensation the model matrix is
 * {@code P * T(t) * R(q) * S(s)}.
 */
public final class SegmentScaleExample {

    public static void main(String[] args) {
        // 1. Build joints with local transforms (scale, rotate, translate).
        Joint root = new Joint("root");
        root.setLocalTransform(new Transform(
                new Vector3f(0f, 0f, 0f),
                new Quaternion(),
                new Vector3f(1f, 1f, 1f)));

        Joint upper = new Joint("upper");
        upper.setLocalTransform(new Transform(
                new Vector3f(0f, 1f, 0f),
                new Quaternion(),
                new Vector3f(1.5f, 2f, 1f)));

        Joint lower = new Joint("lower");
        lower.setLocalTransform(new Transform(
                new Vector3f(0f, 1f, 0f),
                new Quaternion().fromAngles(0.3f, 0.2f, 0f),
                new Vector3f(1f, 1f, 1f)));

        root.addChild(upper);
        upper.addChild(lower);

        // 2. Create the armature. It updates model transforms on construction.
        Armature armature = new Armature(new Joint[]{root, upper, lower});
        // Optional: pick the matrix-based accumulation (default is separate).
        armature.setModelTransformClass(MatrixJointModelTransform.class);

        // 3. Enable compensation on the child whose parent stretches it.
        lower.setSegmentScaleCompensate(true);

        // 4. Apply any local animation, then update once. The flag is read
        //    during update(), so a change takes effect on the next update.
        upper.setLocalScale(new Vector3f(2f, 1f, 3f));
        armature.update();

        // 5. Bind the mesh's pose, then read skinning matrices.
        armature.saveBindPose();
        Matrix4f[] skinning = armature.computeSkinningMatrices();
        for (int i = 0; i < skinning.length; i++) {
            System.out.println("joint " + i + " = " + skinning[i]);
        }

        // 6. Toggle compensation at runtime; mix freely within an armature.
        lower.setSegmentScaleCompensate(false);
        armature.update();
        System.out.println("compensation now " + lower.isSegmentScaleCompensate());
        System.out.println("done");
    }
}
