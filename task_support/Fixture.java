import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.io.IOException;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.CyclicBarrier;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Small independent HTTP process used by the regression tests. It deliberately
 * serves ordinary sitemaps only; all traversal logic lives in the parser.
 */
public final class Fixture {

    private static final String SITEMAP_NS = "http://www.sitemaps.org/schemas/sitemap/0.9";

    private static final Map<String, AtomicInteger> COUNTERS = new ConcurrentHashMap<>();
    private static final AtomicInteger LEAVES = new AtomicInteger();

    private static volatile String base;

    /** Barriers used to prove that two downloads are issued concurrently. */
    private static volatile CyclicBarrier p2Barrier = new CyclicBarrier(2);
    private static volatile CyclicBarrier p1Barrier = new CyclicBarrier(2);
    private static final AtomicBoolean P2_TRIPPED = new AtomicBoolean();
    private static final AtomicBoolean P1_TRIPPED = new AtomicBoolean();

    /** Latches for the "late left branch" scenario. */
    private static volatile CountDownLatch lateGate = new CountDownLatch(1);
    private static volatile boolean lateRightRequested;
    private static volatile boolean lateLeftArrived;
    private static volatile boolean lateLeftReleased;
    private static volatile boolean lateSharedBeforeRelease;

    public static void main(String[] args) throws Exception {
        HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
        var workers = Executors.newCachedThreadPool();
        server.setExecutor(workers);
        base = "http://127.0.0.1:" + server.getAddress().getPort();
        server.createContext("/", Fixture::handle);
        server.start();
        System.out.println(base);
        System.out.flush();
        try {
            while (System.in.read() != -1) {
                // stay alive until the parent closes stdin
            }
        } finally {
            server.stop(0);
            workers.shutdownNow();
        }
    }

    private static void handle(HttpExchange exchange) throws IOException {
        String path = exchange.getRequestURI().getPath();
        String query = exchange.getRequestURI().getRawQuery();
        try {
            if (path.equals("/alias.xml") || path.equals("/old/start.xml")) {
                redirect(exchange, path.equals("/alias.xml") ? "/leaf.xml" : "/new/leaf.xml");
                return;
            }
            switch (path) {
                case "/index.xml": {
                    text(exchange, index(sitemapEntry(base + "/leaf.xml") + sitemapEntry(base + "/leaf.xml") + sitemapEntry(base + "/alias.xml")));
                    return;
                }
                case "/leaf.xml":
                case "/new/leaf.xml": {
                    LEAVES.incrementAndGet();
                    String page = base + (path.startsWith("/new/") ? "/new/page" : "/page");
                    text(exchange, leaf(url(page) + url(page)));
                    return;
                }
                case "/stats":
                    text(exchange, Integer.toString(LEAVES.get()));
                    return;

                /* ----- graph: shared leaves, cycle, aliases, identities ----- */
                case "/g/index.xml":
                    text(exchange, index(sitemapEntry(base + "/g/a/index.xml") + sitemapEntry(base + "/g/b/index.xml")));
                    return;
                case "/g/a/index.xml":
                    count("g_a_index");
                    text(exchange, index(sitemapEntry(base + "/g/shared.xml") + sitemapEntry(base + "/g/cycle.xml")));
                    return;
                case "/g/b/index.xml":
                    count("g_b_index");
                    text(exchange, index(sitemapEntry(base + "/g/shared.xml") + sitemapEntry(base + "/g/b-leaf.xml")));
                    return;
                case "/g/shared.xml":
                    count("g_shared");
                    // the same page is listed twice in one leaf and must be
                    // delivered twice
                    text(exchange, leaf(url(base + "/g/shared-page") + url(base + "/g/shared-page")));
                    return;
                case "/g/cycle.xml":
                    count("g_cycle");
                    text(exchange, index(sitemapEntry(base + "/g/index.xml")));
                    return;
                case "/g/b-leaf.xml":
                    count("g_b_leaf");
                    text(exchange, leaf(url(base + "/g/b-page")));
                    return;
                case "/g/alias-index.xml":
                    text(exchange, index(sitemapEntry(base + "/g/alias.xml") + sitemapEntry(base + "/g/real.xml")));
                    return;
                case "/g/alias.xml":
                    count("g_alias");
                    redirect(exchange, "/g/real.xml");
                    return;
                case "/g/real.xml":
                    count("g_real");
                    text(exchange, leaf(url(base + "/g/real-page")));
                    return;
                case "/g/query-index.xml":
                    text(exchange, index(sitemapEntry(base + "/g/q.xml?x=1") + sitemapEntry(base + "/g/q.xml?x=2") + sitemapEntry(base + "/g/frag.xml#one")
                                    + sitemapEntry(base + "/g/frag.xml#two")));
                    return;
                case "/g/q.xml": {
                    count("g_q_" + query);
                    String pageQuery = base + "/g/q-page?" + query;
                    text(exchange, leaf(url(pageQuery)));
                    return;
                }
                case "/g/frag.xml":
                    count("g_frag");
                    text(exchange, leaf(url(base + "/g/frag-page")));
                    return;

                /* ----- redirect bases and strict checks ----- */
                case "/rd/old/start.xml":
                    redirect(exchange, "/rd/new/map.xml");
                    return;
                case "/rd/new/map.xml":
                    count("rd_map");
                    text(exchange, leaf(url(base + "/rd/new/page")));
                    return;

                /* ----- parallel dispatch ----- */
                case "/p2/index.xml":
                    text(exchange, index(sitemapEntry(base + "/p2/a.xml") + sitemapEntry(base + "/p2/b.xml")));
                    return;
                case "/p2/a.xml":
                    count("p2_a");
                    triangulate(p2Barrier, P2_TRIPPED);
                    text(exchange, leaf(url(base + "/p2/a-page")));
                    return;
                case "/p2/b.xml":
                    count("p2_b");
                    triangulate(p2Barrier, P2_TRIPPED);
                    text(exchange, leaf(url(base + "/p2/b-page")));
                    return;
                case "/p2/reset":
                    p2Barrier = new CyclicBarrier(2);
                    P2_TRIPPED.set(false);
                    text(exchange, "ok");
                    return;
                case "/p2/state":
                    text(exchange, "tripped=" + (P2_TRIPPED.get() ? 1 : 0));
                    return;
                case "/p1/index.xml":
                    text(exchange, index(sitemapEntry(base + "/p1/a.xml") + sitemapEntry(base + "/p1/b.xml")));
                    return;
                case "/p1/a.xml":
                    count("p1_a");
                    triangulate(p1Barrier, P1_TRIPPED);
                    text(exchange, leaf(url(base + "/p1/a-page")));
                    return;
                case "/p1/b.xml":
                    count("p1_b");
                    triangulate(p1Barrier, P1_TRIPPED);
                    text(exchange, leaf(url(base + "/p1/b-page")));
                    return;
                case "/p1/reset":
                    p1Barrier = new CyclicBarrier(2);
                    P1_TRIPPED.set(false);
                    text(exchange, "ok");
                    return;
                case "/p1/state":
                    text(exchange, "tripped=" + (P1_TRIPPED.get() ? 1 : 0));
                    return;

                /* ----- late left branch sharing one leaf ----- */
                case "/late/index.xml":
                    text(exchange, index(sitemapEntry(base + "/late/left.xml") + sitemapEntry(base + "/late/right.xml")));
                    return;
                case "/late/left.xml":
                    count("late_left");
                    lateLeftArrived = true;
                    awaitLateGate();
                    lateLeftReleased = true;
                    text(exchange, index(sitemapEntry(base + "/late/shared.xml") + sitemapEntry(base + "/late/left-only.xml")));
                    return;
                case "/late/right.xml":
                    count("late_right");
                    lateRightRequested = true;
                    text(exchange, index(sitemapEntry(base + "/late/right-only.xml") + sitemapEntry(base + "/late/shared.xml")));
                    return;
                case "/late/shared.xml":
                    if (!lateLeftReleased) {
                        // the right branch fetched the shared leaf while the left
                        // branch was still blocked: proves downloads prefetch
                        lateSharedBeforeRelease = true;
                    }
                    count("late_shared");
                    text(exchange, leaf(url(base + "/late/shared-page")));
                    return;
                case "/late/left-only.xml":
                    count("late_left_leaf");
                    text(exchange, leaf(url(base + "/late/left-page")));
                    return;
                case "/late/right-only.xml":
                    count("late_right_leaf");
                    text(exchange, leaf(url(base + "/late/right-page")));
                    return;
                case "/late/reset":
                    lateGate = new CountDownLatch(1);
                    lateRightRequested = false;
                    lateLeftArrived = false;
                    lateLeftReleased = false;
                    lateSharedBeforeRelease = false;
                    text(exchange, "ok");
                    return;
                case "/late/state":
                    text(exchange, "left=" + (lateLeftArrived ? 1 : 0) + ";right=" + (lateRightRequested ? 1 : 0) + ";released=" + (lateLeftReleased ? 1 : 0) + ";shared-before="
                                    + (lateSharedBeforeRelease ? 1 : 0));
                    return;
                case "/late/shared-fetched-wait": {
                    // block until the shared leaf has been fetched (or timeout)
                    long deadline = System.currentTimeMillis() + 5000;
                    while (!lateSharedBeforeRelease && System.currentTimeMillis() < deadline) {
                        try {
                            Thread.sleep(10);
                        } catch (InterruptedException e) {
                            Thread.currentThread().interrupt();
                            break;
                        }
                    }
                    text(exchange, "shared-before=" + (lateSharedBeforeRelease ? 1 : 0));
                    return;
                }
                case "/late/release":
                    lateGate.countDown();
                    text(exchange, "ok");
                    return;

                /* ----- nested indexes at parallelism 1 ----- */
                case "/n/index.xml":
                    count("n_index");
                    text(exchange, index(sitemapEntry(base + "/n/inner.xml")));
                    return;
                case "/n/inner.xml":
                    count("n_inner");
                    text(exchange, index(sitemapEntry(base + "/n/leaf.xml")));
                    return;
                case "/n/leaf.xml":
                    count("n_leaf");
                    text(exchange, leaf(url(base + "/n/page")));
                    return;

                /* ----- failures ----- */
                case "/f/index.xml":
                    text(exchange, index(sitemapEntry(base + "/f/ok.xml") + sitemapEntry(base + "/f/missing.xml") + sitemapEntry(base + "/f/after.xml")));
                    return;
                case "/f/ok.xml":
                    count("f_ok");
                    text(exchange, leaf(url(base + "/f/ok-page")));
                    return;
                case "/f/missing.xml":
                    count("f_missing");
                    notFound(exchange);
                    return;
                case "/f/after.xml":
                    count("f_after");
                    text(exchange, leaf(url(base + "/f/after-page")));
                    return;
                case "/f/action.xml":
                    count("f_action");
                    text(exchange, leaf(url(base + "/f/p1") + url(base + "/f/p2") + url(base + "/f/p3")));
                    return;
                case "/f/parse-index.xml":
                    text(exchange, index(sitemapEntry(base + "/f/parse-ok.xml") + sitemapEntry(base + "/f/broken.xml") + sitemapEntry(base + "/f/parse-after.xml")));
                    return;
                case "/f/parse-ok.xml":
                    count("f_parse_ok");
                    text(exchange, leaf(url(base + "/f/parse-ok-page")));
                    return;
                case "/f/broken.xml":
                    count("f_broken");
                    xml(exchange, "<urlset xmlns='" + SITEMAP_NS + "'><url><loc>" + base + "/f/truncated");
                    return;
                case "/f/parse-after.xml":
                    count("f_parse_after");
                    text(exchange, leaf(url(base + "/f/parse-after-page")));
                    return;

                default:
                    if (path.startsWith("/count/")) {
                        text(exchange, Integer.toString(counters(path.substring("/count/".length())).get()));
                        return;
                    }
                    if (path.equals("/reset-counters")) {
                        COUNTERS.clear();
                        LEAVES.set(0);
                        text(exchange, "ok");
                        return;
                    }
                    notFound(exchange);
            }
        } catch (RuntimeException e) {
            notFound(exchange);
        }
    }

    private static void awaitLateGate() {
        try {
            lateGate.await(5, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private static void triangulate(CyclicBarrier barrier, AtomicBoolean tripped) {
        try {
            barrier.await(800, TimeUnit.MILLISECONDS);
            tripped.set(true);
        } catch (Exception e) {
            // barrier timed out or was broken: the peer request did not overlap
        }
    }

    private static AtomicInteger counters(String name) {
        return COUNTERS.computeIfAbsent(name, key -> new AtomicInteger());
    }

    private static void count(String name) {
        counters(name).incrementAndGet();
    }

    private static String url(String loc) {
        return "<url><loc>" + escape(loc) + "</loc></url>";
    }

    private static String sitemapEntry(String loc) {
        return "<sitemap><loc>" + escape(loc) + "</loc></sitemap>";
    }

    private static String index(String entries) {
        return "<sitemapindex xmlns='" + SITEMAP_NS + "'>" + entries + "</sitemapindex>";
    }

    private static String leaf(String entries) {
        return "<urlset xmlns='" + SITEMAP_NS + "'>" + entries + "</urlset>";
    }

    private static String escape(String value) {
        return value.replace("&", "&amp;");
    }

    private static void redirect(HttpExchange exchange, String location) throws IOException {
        exchange.getResponseHeaders().set("Location", location);
        exchange.sendResponseHeaders(302, -1);
        exchange.close();
    }

    private static void notFound(HttpExchange exchange) throws IOException {
        exchange.sendResponseHeaders(404, -1);
        exchange.close();
    }

    private static void text(HttpExchange exchange, String body) throws IOException {
        xml(exchange, body);
    }

    private static void xml(HttpExchange exchange, String body) throws IOException {
        byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
        exchange.getResponseHeaders().set("Content-Type", "application/xml");
        exchange.sendResponseHeaders(200, bytes.length);
        try (OutputStream out = exchange.getResponseBody()) {
            out.write(bytes);
        } finally {
            exchange.close();
        }
    }
}
