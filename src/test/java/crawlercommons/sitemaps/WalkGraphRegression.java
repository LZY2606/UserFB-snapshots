package crawlercommons.sitemaps;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.net.URI;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

import task_support.Checks;

/**
 * Regression tests for the graph-aware, optionally parallel sitemap walk.
 * <p>
 * The test starts an independent HTTP fixture process and drives the parser
 * against it. Downloads are observed through server-side counters and explicit
 * latches, so no assumptions are made about timing beyond generous timeouts.
 */
public final class WalkGraphRegression {

    private static String base;
    private static Process fixture;

    public static void run(Checks c) throws Exception {
        startFixture();
        try {
            c.group("graph");
            graphSharedAndCycle(c);
            graphAliasAndIdentity(c);
            graphRedirectBase(c);
            graphParsedRoot(c);

            c.group("parallel");
            parallelConcurrentDispatch(c);
            parallelLateLeftBranch(c);
            parallelSingleThread(c);

            c.group("cleanup");
            cleanupActionException(c);
            cleanupRequestFailure(c);
            cleanupParseFailure(c);
            cleanupAfterFailure(c);
        } finally {
            stopFixture();
        }
    }

    /* ------------------------------------------------------------------ */
    /* Fixture lifecycle                                                   */
    /* ------------------------------------------------------------------ */

    private static void startFixture() throws Exception {
        String java = Path.of(System.getProperty("java.home"), "bin", "java").toString();
        fixture = new ProcessBuilder(java, "--add-modules", "jdk.httpserver", "-cp", System.getProperty("java.class.path"), "Fixture").redirectError(ProcessBuilder.Redirect.INHERIT).start();
        BufferedReader reader = new BufferedReader(new InputStreamReader(fixture.getInputStream(), StandardCharsets.UTF_8));
        base = reader.readLine();
        if (base == null || !base.startsWith("http://127.0.0.1:")) {
            throw new AssertionError("fixture startup failed");
        }
    }

    private static void stopFixture() throws Exception {
        if (fixture == null) {
            return;
        }
        fixture.getOutputStream().close();
        if (!fixture.waitFor(5, TimeUnit.SECONDS)) {
            fixture.destroyForcibly();
            fixture.waitFor();
        }
    }

    /* ------------------------------------------------------------------ */
    /* Graph: shared children, cycle, aliases, document identity           */
    /* ------------------------------------------------------------------ */

    private static void graphSharedAndCycle(Checks c) throws Exception {
        resetCounters();
        SiteMapParser parser = new SiteMapParser();
        List<String> delivered = new ArrayList<>();
        parser.walkSiteMap(new URL(base + "/g/index.xml"), url -> delivered.add(url.getUrl().toString()));

        // depth-first order: A's shared leaf, its cycle back-edge (stopped),
        // then B's shared leaf (already expanded) and B's own leaf
        List<String> expected = List.of(base + "/g/shared-page", base + "/g/shared-page", base + "/g/b-page");
        c.check(delivered.equals(expected), "shared/cycle delivery order: " + delivered);
        c.check(count("/count/g_shared") == 1, "shared leaf fetched exactly once: " + count("/count/g_shared"));
        c.check(count("/count/g_cycle") == 1, "back-edge index fetched once: " + count("/count/g_cycle"));
        c.check(count("/count/g_a_index") == 1 && count("/count/g_b_index") == 1, "each branch index fetched once");
        c.check(occurrences(delivered, base + "/g/shared-page") == 2, "repeated page delivered once per listing: " + delivered);
    }

    private static void graphAliasAndIdentity(Checks c) throws Exception {
        resetCounters();
        SiteMapParser parser = new SiteMapParser();
        List<String> delivered = new ArrayList<>();
        parser.walkSiteMap(new URL(base + "/g/alias-index.xml"), url -> delivered.add(url.getUrl().toString()));

        // alias.xml redirects to real.xml; only the final document is expanded,
        // at the position of the first (alias) reference
        c.check(delivered.equals(List.of(base + "/g/real-page")), "alias expanded once at first reference: " + delivered);
        c.check(count("/count/g_real") == 1, "alias target body parsed once: " + count("/count/g_real"));

        resetCounters();
        delivered.clear();
        parser.walkSiteMap(new URL(base + "/g/query-index.xml"), url -> delivered.add(url.getUrl().toString()));
        // same path with different queries are distinct documents, a fragment
        // does not distinguish documents
        c.check(count("/count/g_q_x=1") == 1 && count("/count/g_q_x=2") == 1, "distinct queries downloaded separately");
        c.check(count("/count/g_frag") == 1, "fragment did not create a second document");
        c.check(delivered.size() == 3, "two query leaves plus one fragment leaf delivered: " + delivered);
        Set<String> pages = new LinkedHashSet<>(delivered);
        c.check(pages.contains(base + "/g/q-page?x=1") && pages.contains(base + "/g/q-page?x=2"), "query preserved in listed pages: " + pages);
    }

    private static void graphRedirectBase(Checks c) throws Exception {
        resetCounters();
        // online parse: /rd/old/start.xml redirects to /rd/new/map.xml, the
        // listed /rd/new/page must be accepted in strict mode
        SiteMapParser parser = new SiteMapParser();
        AbstractSiteMap map = parser.parseSiteMap(new URL(base + "/rd/old/start.xml"));
        c.check(map instanceof SiteMap, "redirect target parsed as leaf");
        c.check(map.getUrl().getPath().equals("/rd/new/map.xml"), "final response URL used as identity: " + map.getUrl());
        c.check(((SiteMap) map).getSiteMapUrls().size() == 1, "strict mode kept the page under the final base: " + ((SiteMap) map).getSiteMapUrls());

        // walked download must use the final URL for parsing as well
        resetCounters();
        List<String> delivered = new ArrayList<>();
        parser.walkSiteMap(new URL(base + "/rd/old/start.xml"), url -> delivered.add(url.getUrl().getPath()));
        c.check(delivered.equals(List.of("/rd/new/page")), "walked redirect leaf delivered under final base: " + delivered);
    }

    private static void graphParsedRoot(Checks c) throws Exception {
        resetCounters();
        SiteMapParser parser = new SiteMapParser();
        String xml = "<urlset xmlns='http://www.sitemaps.org/schemas/sitemap/0.9'><url><loc>" + base + "/parsed-page</loc></url>"
                        + "<url><loc>" + base + "/parsed-page</loc></url></urlset>";
        AbstractSiteMap root = parser.parseSiteMap("application/xml", xml.getBytes(StandardCharsets.UTF_8), new URL(base + "/parsed-root.xml"));
        List<String> delivered = new ArrayList<>();
        parser.walkSiteMap(root, url -> delivered.add(url.getUrl().toString()));
        // repeated pages are still delivered as listed
        c.check(delivered.equals(List.of(base + "/parsed-page", base + "/parsed-page")), "parsed root used directly: " + delivered);
        c.check(count("/count/parsed-root.xml") == 0, "parsed root object was not downloaded again");
    }

    /* ------------------------------------------------------------------ */
    /* Parallel downloads                                                  */
    /* ------------------------------------------------------------------ */

    private static void parallelConcurrentDispatch(Checks c) throws Exception {
        resetCounters();
        get("/p2/reset");
        SiteMapParser parser = new SiteMapParser();
        parser.setWalkParallelism(2);
        List<String> delivered = new ArrayList<>();
        parser.walkSiteMap(new URL(base + "/p2/index.xml"), url -> delivered.add(url.getUrl().getPath()));

        c.check(get("/p2/state").equals("tripped=1"), "two sibling maps requested concurrently");
        c.check(delivered.equals(List.of("/p2/a-page", "/p2/b-page")), "parallel delivery still depth-first: " + delivered);

        // the two branch leaves share the same parent; the delivery order must
        // not depend on which download finishes first
        resetCounters();
        parser.setWalkParallelism(4);
        delivered.clear();
        parser.walkSiteMap(new URL(base + "/p2/index.xml"), url -> delivered.add(url.getUrl().getPath()));
        c.check(delivered.equals(List.of("/p2/a-page", "/p2/b-page")), "stable index order with parallelism 4: " + delivered);
    }

    private static void parallelLateLeftBranch(Checks c) throws Exception {
        resetCounters();
        get("/late/reset");
        SiteMapParser parser = new SiteMapParser();
        parser.setWalkParallelism(2);

        List<String> delivered = Collections.synchronizedList(new ArrayList<>());
        Throwable[] thrown = new Throwable[1];
        CountDownLatch done = new CountDownLatch(1);
        Thread walkThread = new Thread(() -> {
            try {
                parser.walkSiteMap(new URL(base + "/late/index.xml"), url -> {
                    delivered.add(url.getUrl().getPath());
                    if (delivered.size() == 1) {
                        // the fast right branch has already fetched the shared
                        // leaf, but delivery is at the slow left branch now
                        try {
                            get("/late/release");
                        } catch (Exception e) {
                            throw new RuntimeException(e);
                        }
                    }
                });
            } catch (Throwable t) {
                thrown[0] = t;
            } finally {
                done.countDown();
            }
        }, "walk-caller");
        walkThread.start();

        // wait until the shared leaf was fetched but the left branch is still
        // blocked in the parse phase; this is a download, not a delivery
        String sharedBefore = get("/late/shared-fetched-wait");
        c.check(sharedBefore.equals("shared-before=1"), "shared leaf downloaded before the slow left branch was released: " + sharedBefore);
        synchronized (delivered) {
            c.check(delivered.isEmpty(), "nothing delivered while the left branch is late: " + delivered);
        }

        // release the gate and let the walk finish
        get("/late/release");
        c.check(done.await(10, TimeUnit.SECONDS), "walk finished after release");
        if (thrown[0] != null) {
            throw new AssertionError("walk failed", thrown[0]);
        }

        List<String> expected = List.of("/late/shared-page", "/late/left-page", "/late/right-page");
        c.check(delivered.equals(expected), "shared leaf delivered at first depth-first reference: " + delivered);
        c.check(count("/count/late_shared") == 1, "shared leaf expanded once: " + count("/count/late_shared"));
        c.check(get("/late/state").endsWith("shared-before=1"), "fixture confirms early download: " + get("/late/state"));
    }

    private static void parallelSingleThread(Checks c) throws Exception {
        // parallelism 1: nested indexes terminate and requests are sequential,
        // proven by a barrier which only trips if two downloads overlap
        resetCounters();
        get("/p1/reset");
        SiteMapParser parser = new SiteMapParser();
        parser.setWalkParallelism(1);
        List<String> delivered = new ArrayList<>();
        parser.walkSiteMap(new URL(base + "/p1/index.xml"), url -> delivered.add(url.getUrl().getPath()));
        c.check(delivered.equals(List.of("/p1/a-page", "/p1/b-page")), "serial delivery: " + delivered);
        c.check(get("/p1/state").equals("tripped=0"), "parallelism 1 does not overlap requests");

        resetCounters();
        delivered.clear();
        parser.walkSiteMap(new URL(base + "/n/index.xml"), url -> delivered.add(url.getUrl().toString()));
        c.check(delivered.equals(List.of(base + "/n/page")), "nested index at parallelism 1 terminates: " + delivered);
        c.check(count("/count/n_index") == 1 && count("/count/n_inner") == 1 && count("/count/n_leaf") == 1, "each nested index fetched once");

        // both walks reused the same parser instance successfully
        c.check(parser.getWalkParallelism() == 1, "parallelism accessor reflects the value");
    }

    /* ------------------------------------------------------------------ */
    /* Cleanup: action exceptions, request/parse failures, next call       */
    /* ------------------------------------------------------------------ */

    private static void cleanupActionException(Checks c) throws Exception {
        resetCounters();
        SiteMapParser parser = new SiteMapParser();
        IllegalArgumentException thrown = new IllegalArgumentException("action failed");
        AtomicInteger delivered = new AtomicInteger();
        Throwable caught = null;
        try {
            parser.walkSiteMap(new URL(base + "/f/action.xml"), url -> {
                if (delivered.incrementAndGet() == 2) {
                    throw thrown;
                }
            });
        } catch (Throwable t) {
            caught = t;
        }
        c.check(caught == thrown, "action exception relayed unchanged: " + caught);
        c.check(delivered.get() == 2, "pages before the failing action were delivered: " + delivered.get());
    }

    private static void cleanupRequestFailure(Checks c) throws Exception {
        resetCounters();
        // parallelism 1 keeps the failure deterministic: no speculative
        // download of a later sibling may be started before the failure is seen
        SiteMapParser parser = new SiteMapParser();
        parser.setWalkParallelism(1);
        List<String> delivered = new ArrayList<>();
        Throwable caught = null;
        try {
            parser.walkSiteMap(new URL(base + "/f/index.xml"), url -> delivered.add(url.getUrl().getPath()));
        } catch (Throwable t) {
            caught = t;
        }
        c.check(caught instanceof IOException, "request failure thrown at its depth-first position: " + caught);
        c.check(delivered.equals(List.of("/f/ok-page")), "pages before the failing map kept: " + delivered);

        c.check(count("/count/f_after") == 0, "no request issued after the failure");
        // the failure happened before the third child: the walk stopped,
        // neither the failing nor the following map raised further requests

        // with parallel downloads the failure is still reported at the same
        // depth-first position and no further page is delivered
        resetCounters();
        parser.setWalkParallelism(2);
        delivered.clear();
        caught = null;
        try {
            parser.walkSiteMap(new URL(base + "/f/index.xml"), url -> delivered.add(url.getUrl().getPath()));
        } catch (Throwable t) {
            caught = t;
        }
        c.check(caught instanceof IOException, "parallel request failure thrown: " + caught);
        c.check(delivered.equals(List.of("/f/ok-page")), "parallel failure keeps earlier pages: " + delivered);
    }

    private static void cleanupParseFailure(Checks c) throws Exception {
        // parallelism 1: the broken map is downloaded when its depth-first
        // position is reached, so nothing after it is requested
        resetCounters();
        SiteMapParser parser = new SiteMapParser();
        parser.setWalkParallelism(1);
        List<String> delivered = new ArrayList<>();
        Throwable caught = null;
        try {
            parser.walkSiteMap(new URL(base + "/f/parse-index.xml"), url -> delivered.add(url.getUrl().getPath()));
        } catch (Throwable t) {
            caught = t;
        }
        c.check(caught instanceof UnknownFormatException, "parse failure thrown at its depth-first position: " + caught);
        c.check(delivered.equals(List.of("/f/parse-ok-page")), "pages before the broken map kept: " + delivered);
        c.check(count("/count/f_parse_after") == 0, "no page delivered or requested after the parse failure");

        // parallel downloads: the throw position and kept pages are unchanged
        resetCounters();
        parser.setWalkParallelism(2);
        delivered.clear();
        caught = null;
        try {
            parser.walkSiteMap(new URL(base + "/f/parse-index.xml"), url -> delivered.add(url.getUrl().getPath()));
        } catch (Throwable t) {
            caught = t;
        }
        c.check(caught instanceof UnknownFormatException, "parallel parse failure thrown: " + caught);
        c.check(delivered.equals(List.of("/f/parse-ok-page")), "parallel parse failure keeps earlier pages: " + delivered);
    }

    private static void cleanupAfterFailure(Checks c) throws Exception {
        // the same parser must still work after failed walks: the download
        // workers were released and no state carried over
        SiteMapParser parser = new SiteMapParser();
        parser.setWalkParallelism(2);
        Throwable failedWalk = null;
        try {
            parser.walkSiteMap(new URL(base + "/f/index.xml"), url -> {
            });
        } catch (Throwable t) {
            failedWalk = t;
        }
        c.check(failedWalk instanceof IOException, "first walk failed as expected: " + failedWalk);

        List<String> delivered = new ArrayList<>();
        parser.walkSiteMap(new URL(base + "/p2/index.xml"), url -> delivered.add(url.getUrl().getPath()));
        c.check(delivered.equals(List.of("/p2/a-page", "/p2/b-page")), "same parser works after a failed walk: " + delivered);

        // and a fresh walk does not reuse the previous walk's downloads
        resetCounters();
        delivered.clear();
        parser.walkSiteMap(new URL(base + "/leaf.xml"), url -> delivered.add(url.getUrl().getPath()));
        c.check(count("/stats") == 1 && delivered.size() == 2, "no cross-walk cache, leaf fetched once: " + delivered + ", stats=" + count("/stats"));
        c.check(delivered.get(0).equals("/page"), "second walk delivered its own pages: " + delivered);
    }

    /* ------------------------------------------------------------------ */
    /* HTTP helpers                                                        */
    /* ------------------------------------------------------------------ */

    private static void resetCounters() throws IOException {
        get("/reset-counters");
    }

    private static int count(String name) throws IOException {
        return Integer.parseInt(get(name));
    }

    private static String get(String path) throws IOException {
        try (var in = new URL(base + path).openStream()) {
            return new String(in.readAllBytes(), StandardCharsets.UTF_8);
        }
    }

    private static int occurrences(List<String> values, String value) {
        int count = 0;
        for (String candidate : values) {
            if (candidate.equals(value)) {
                count++;
            }
        }
        return count;
    }
}
