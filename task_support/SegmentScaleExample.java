import com.jme3.anim.Armature;
import com.jme3.anim.Joint;
import com.jme3.anim.MatrixJointModelTransform;
import com.jme3.math.Quaternion;
import com.jme3.math.Transform;
import com.jme3.math.Vector3f;

/**
 * A windowless demonstration of per-joint segment-scale compensation.
 *
 * <p>It builds a three-bone arm, enables compensation on the elbow and wrist,
 * stretches the upper arm, and prints the resulting skinning matrices. It does
 * not need a graphics device, an AssetManager, or a running application.
 *
 * <p>Run it with:
 * <pre>
 *   javac --release 17 -d /tmp/demo $(find jme3-core/src/main/java \
 *       jme3-core/src/plugins/java jme3-core/src/tools/java \
 *       -name '*.java')
 *   javac --release 17 -cp /tmp/demo -d /tmp/demo task_support/SegmentScaleExample.java
 *   java -cp /tmp/demo SegmentScaleExample
 * </pre>
 * or simply through the task harness with {@code sh task_support/run.sh target},
 * which compiles this file as part of its input set.
 */
public final class SegmentScaleExample {

    private SegmentScaleExample() {
        // example class: not instantiable
    }

    public static void main(String[] args) {
        /*
         * Build a small skeleton: shoulder -> elbow -> wrist.
         * Local translations place each bone along its parent's X axis.
         */
        Joint shoulder = new Joint("shoulder");
        Joint elbow = new Joint("elbow");
        Joint wrist = new Joint("wrist");

        shoulder.setLocalTransform(new Transform(
                new Vector3f(0f, 2f, 0f),
                new Quaternion(),
                new Vector3f(1f, 1f, 1f)));
        elbow.setLocalTransform(new Transform(
                new Vector3f(1f, 0f, 0f),
                new Quaternion(),
                new Vector3f(1f, 1f, 1f)));
        wrist.setLocalTransform(new Transform(
                new Vector3f(1f, 0f, 0f),
                new Quaternion(),
                new Vector3f(1f, 1f, 1f)));

        shoulder.addChild(elbow);
        elbow.addChild(wrist);

        /*
         * Compensation belongs to the child joint. Enable it on the elbow and
         * the wrist so that stretching the shoulder moves them but does not
         * scale their own geometry. The shoulder (a root) has nothing to
         * compensate and its flag is ignored.
         */
        elbow.setSegmentScaleCompensate(true);
        wrist.setSegmentScaleCompensate(true);

        Armature armature = new Armature(new Joint[]{shoulder, elbow, wrist});
        armature.setModelTransformClass(MatrixJointModelTransform.class);
        armature.update();
        armature.saveBindPose();

        System.out.println("At bind pose:");
        printSkinning(armature);

        /*
         * Elongate the upper arm. Because compensation is enabled on the
         * children, the elbow and wrist keep their own scale: only the bone
         * segment lengths change.
         */
        shoulder.setLocalScale(new Vector3f(2.5f, 1f, 1f));
        armature.update();

        System.out.println("After stretching the shoulder:");
        printSkinning(armature);

        /*
         * Restore the bind pose. The skinning matrices return to identity.
         */
        armature.applyBindPose();
        System.out.println("After applyBindPose():");
        printSkinning(armature);

        /*
         * The flag is mutable at runtime; it takes effect on the next update.
         */
        elbow.setSegmentScaleCompensate(false);
        armature.update();
        System.out.println("elbow compensation now: " + elbow.isSegmentScaleCompensate());
    }

    private static void printSkinning(Armature armature) {
        for (Joint joint : armature.getJointList()) {
            System.out.println("  " + joint.getName() + " compensate="
                    + joint.isSegmentScaleCompensate());
        }
        for (int i = 0; i < armature.getJointCount(); ++i) {
            /*
             * computeSkinningMatrices() returns a shared array; copy the entry
             * if it needs to outlive the next call.
             */
            System.out.println("    skin[" + armature.getJoint(i).getName() + "]="
                    + armature.computeSkinningMatrices()[i]);
        }
    }
}
