import org.jsoup.Jsoup;
import org.jsoup.nodes.Document;
import org.jsoup.nodes.Element;
import org.jsoup.nodes.ShadowRoot;
import org.jsoup.parser.Parser;

/**
 Example: reading declarative shadow roots (Web Components that emit
 {@code <template shadowrootmode="open">}) with jsoup, keeping the host's light DOM and the
 component's shadow fields apart.

 Run with: javac -cp jsoup.jar ShadowRootExample.java && java -cp jsoup.jar:. ShadowRootExample
 */
public class ShadowRootExample {
    public static void main(String[] args) {
        String html =
            "<div id=profile>" +
                "<template shadowrootmode=open>" +
                    "<span class=name>Ada Lovelace</span>" +
                    "<a href=/u/ada>profile</a>" +
                "</template>" +
                "<p class=bio>First programmer.</p>" + // light DOM, stays on the host
            "</div>";

        // opt in; default parsers keep treating these as ordinary <template> elements
        Parser parser = Parser.htmlParser().setDeclarativeShadowRoots(true);
        Document doc = Jsoup.parse(html, "https://example.com/", parser);

        Element host = doc.selectFirst("#profile");

        // 1. light DOM: host queries and text never cross into the shadow tree
        System.out.println("light text : " + host.text());          // First programmer.
        System.out.println("light hits : " + doc.select(".name"));  // (empty)

        // 2. shadow tree: read the component's fields from the root
        ShadowRoot root = host.shadowRoot();
        System.out.println("root host  : " + root.host().tagName() + "#" + root.host().id());
        System.out.println("name       : " + root.selectFirst(".name").text());
        System.out.println("link       : " + root.selectFirst("a").absUrl("href")); // inherits the host's base URI

        // 3. serialization: outerHtml() stays light-only; outerHtmlWithShadowRoots() round-trips
        System.out.println("light only : " + host.outerHtml());
        String withShadow = host.outerHtmlWithShadowRoots();
        System.out.println("with shadow: " + withShadow);

        Document reparsed = Jsoup.parse(withShadow, "https://example.com/", parser);
        System.out.println("round trip : " + reparsed.selectFirst("#profile").shadowRoot().selectFirst(".name").text());
    }
}
