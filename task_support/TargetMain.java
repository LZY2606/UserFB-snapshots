public final class TargetMain {
    public static void main(String[] args) throws Exception {
        TaskChecks.group("matrix", SegmentScaleTests::matrix);
        TaskChecks.group("legacy", SegmentScaleTests::legacy);
        TaskChecks.group("lifecycle", SegmentScaleTests::lifecycle);
        System.out.println("TARGET_COMPLETE groups=3");
    }
}
