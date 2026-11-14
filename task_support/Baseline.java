import com.jme3.anim.*;
import com.jme3.math.*;
import com.jme3.util.clone.Cloner;
public final class Baseline {
    static void need(boolean v) { if (!v) throw new AssertionError("baseline"); }
    public static void main(String[] args) throws Exception {
        for (Class<? extends com.jme3.anim.util.JointModelTransform> mode : java.util.List.of(MatrixJointModelTransform.class, SeparateJointModelTransform.class)) {
            Joint parent = new Joint("upper"), child = new Joint("lower");
            parent.setLocalScale(new Vector3f(2,3,1));
            child.setLocalTranslation(new Vector3f(1,0,0)); parent.addChild(child);
            Armature arm = new Armature(new Joint[]{parent,child});
            arm.setModelTransformClass(mode); arm.update(); arm.saveBindPose();
            for (Matrix4f m:arm.computeSkinningMatrices()) need(m.isIdentity());
            parent.setLocalScale(new Vector3f(4,3,1)); arm.update();
            Matrix4f m=arm.computeSkinningMatrices()[1];
            Vector3f p=m.mult(new Vector3f(2,1,0));
            need(p.distance(new Vector3f(4,1,0))<1e-5f);
            arm.applyBindPose(); arm.update(); need(child.getLocalTranslation().distance(new Vector3f(1,0,0))<1e-5f);
            Armature clone=Cloner.deepClone(arm); clone.update();
            need(clone.getJoint(1)!=child);
            System.out.println("BASELINE_OK mode="+mode.getSimpleName()+" deformed="+p);
        }
        boolean api=true;
        try { Joint.class.getMethod("setSegmentScaleCompensate",boolean.class); }
        catch(NoSuchMethodException e) {api=false;}
        need(!api);
        System.out.println("SEGMENT_SCALE_API_PRESENT="+api);
    }
}
