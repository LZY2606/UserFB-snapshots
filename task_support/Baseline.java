import crawlercommons.sitemaps.*;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.concurrent.TimeUnit;

public final class Baseline {
    public static void main(String[] args) throws Exception {
        Compatibility.main(args);
        String java = Path.of(System.getProperty("java.home"), "bin", "java").toString();
        Process child = new ProcessBuilder(java, "--add-modules", "jdk.httpserver", "-cp", System.getProperty("java.class.path"), "Fixture").redirectError(ProcessBuilder.Redirect.INHERIT).start();
        try {
            String base = new BufferedReader(new InputStreamReader(child.getInputStream(), StandardCharsets.UTF_8)).readLine();
            if (base == null || !base.startsWith("http://127.0.0.1:")) throw new AssertionError("fixture startup");
            SiteMapParser parser = new SiteMapParser();
            var pages = new ArrayList<SiteMapURL>();
            parser.walkSiteMap(URI.create(base + "/index.xml").toURL(), pages::add);
            String count;
            try (var in = URI.create(base + "/stats").toURL().openStream()) { count = new String(in.readAllBytes(), StandardCharsets.UTF_8); }
            if (pages.size() != 6 || !count.equals("3")) throw new AssertionError("unexpected baseline graph state: " + pages.size() + "," + count);
            System.out.println("CURRENT_WALK leaf_requests=" + count + " callbacks=" + pages.size());
            SiteMap moved = (SiteMap) parser.parseSiteMap(URI.create(base + "/old/start.xml").toURL());
            if (moved.getSiteMapUrls().size() != 0 || !moved.getUrl().getPath().equals("/old/start.xml")) throw new AssertionError("unexpected redirect baseline");
            System.out.println("CURRENT_REDIRECT parsed_base=" + moved.getUrl().getPath() + " accepted_pages=" + moved.getSiteMapUrls().size());
            try { SiteMapParser.class.getMethod("setWalkParallelism", int.class); throw new AssertionError("feature already exists"); }
            catch (NoSuchMethodException expected) { System.out.println("MISSING_WALK_PARALLELISM"); }
            System.out.println("BASELINE_OK");
        } finally {
            child.getOutputStream().close();
            if (!child.waitFor(5, TimeUnit.SECONDS)) { child.destroyForcibly(); child.waitFor(); }
        }
    }
}
