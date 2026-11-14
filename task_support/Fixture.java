import com.sun.net.httpserver.HttpServer;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicInteger;

/** Small independent HTTP process, deliberately contains only ordinary maps. */
public final class Fixture {
    public static void main(String[] args) throws Exception {
        HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
        var workers = Executors.newCachedThreadPool();
        server.setExecutor(workers);
        String base = "http://127.0.0.1:" + server.getAddress().getPort();
        AtomicInteger leaves = new AtomicInteger();
        server.createContext("/", exchange -> {
            String path = exchange.getRequestURI().getPath();
            String body;
            if (path.equals("/alias.xml") || path.equals("/old/start.xml")) {
                exchange.getResponseHeaders().set("Location", path.equals("/alias.xml") ? "/leaf.xml" : "/new/leaf.xml");
                exchange.sendResponseHeaders(302, -1);
                exchange.close();
                return;
            } else if (path.equals("/index.xml")) {
                String entry = "<sitemap><loc>" + base + "/leaf.xml</loc></sitemap>";
                body = "<sitemapindex xmlns='http://www.sitemaps.org/schemas/sitemap/0.9'>" + entry + entry
                    + "<sitemap><loc>" + base + "/alias.xml</loc></sitemap></sitemapindex>";
            } else if (path.equals("/leaf.xml") || path.equals("/new/leaf.xml")) {
                leaves.incrementAndGet();
                String page = base + (path.startsWith("/new/") ? "/new/page" : "/page");
                String entry = "<url><loc>" + page + "</loc></url>";
                body = "<urlset xmlns='http://www.sitemaps.org/schemas/sitemap/0.9'>" + entry + entry + "</urlset>";
            } else if (path.equals("/stats")) {
                body = Integer.toString(leaves.get());
            } else {
                exchange.sendResponseHeaders(404, -1);
                exchange.close();
                return;
            }
            byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
            exchange.getResponseHeaders().set("Content-Type", "application/xml");
            exchange.sendResponseHeaders(200, bytes.length);
            try (var out = exchange.getResponseBody()) { out.write(bytes); }
            exchange.close();
        });
        server.start();
        System.out.println(base);
        System.out.flush();
        try { while (System.in.read() != -1) { } }
        finally { server.stop(0); workers.shutdownNow(); }
    }
}
