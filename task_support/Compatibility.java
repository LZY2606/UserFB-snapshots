import crawlercommons.sitemaps.*;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;

public final class Compatibility {
    public static void main(String[] args) throws Exception {
        SiteMapParser parser = new SiteMapParser();
        String xml = "<urlset xmlns='http://www.sitemaps.org/schemas/sitemap/0.9'><url><loc>http://example.org/page</loc></url></urlset>";
        AbstractSiteMap parsed = parser.parseSiteMap("application/xml", xml.getBytes(StandardCharsets.UTF_8), URI.create("http://example.org/map.xml").toURL());
        var urls = new ArrayList<SiteMapURL>();
        parser.walkSiteMap(parsed, urls::add);
        if (urls.size() != 1 || !urls.get(0).getUrl().toString().equals("http://example.org/page")) throw new AssertionError("parsed leaf walk");
        if (!parsed.isProcessed()) throw new AssertionError("processed flag");
        System.out.println("COMPATIBILITY_OK parsed_leaf=1");
    }
}
