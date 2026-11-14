package task_support;
import java.io.StringReader;
import org.jsoup.nodes.*;
import org.jsoup.parser.Parser;

public final class Baseline {
    public static void main(String[] args) throws Exception {
        String html = "<p>A &amp; B<b> C</b><br>D &NotEqualTilde; &#x1F642;</p>";
        Element p = Parser.htmlParser().setTrackPosition(true).parseInput(new StringReader(html), "").selectFirst("p");
        if (p == null || !p.text().equals("A & B C D \u2242\u0338 \uD83D\uDE42")) throw new AssertionError("production text");
        TextNode node = p.textNodes().get(0);
        Range range = node.sourceRange();
        if (!range.isTracked() || !html.substring(range.startPos(),range.endPos()).equals("A &amp; B")) throw new AssertionError("source range");
        System.out.println("TEXT="+p.text());
        System.out.println("NODE_SOURCE="+html.substring(range.startPos(),range.endPos()));
        System.out.println("DECODED_NODE="+node.getWholeText());
        boolean exists;
        try { Element.class.getMethod("textWithSource"); exists=true; }
        catch (NoSuchMethodException expected) { exists=false; }
        System.out.println("TEXT_WITH_SOURCE_API="+(exists ? "present" : "missing"));
    }
}
