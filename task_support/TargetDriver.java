import task_support.Checks;

public final class TargetDriver {
    public static void main(String[] args) throws Exception {
        Checks checks = new Checks();
        Class.forName("crawlercommons.sitemaps.WalkGraphRegression").getMethod("run", Checks.class).invoke(null, checks);
        checks.finish();
    }
}
