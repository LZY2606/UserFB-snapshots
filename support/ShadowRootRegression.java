import org.jsoup.Jsoup;
import org.jsoup.nodes.*;
import org.jsoup.parser.*;
import java.io.*;
import java.util.*;

public class ShadowRootRegression {
    static Parser shadowParser() {
        return Parser.htmlParser().setDeclarativeShadowRoots(true);
    }

    /** A Reader that drips input a few chars at a time, to stress incremental tokenization. */
    static final class ChunkedReader extends StringReader {
        ChunkedReader(String s) { super(s); }
        @Override public int read(char[] cbuf, int off, int len) throws IOException {
            return super.read(cbuf, off, Math.min(len, 5));
        }
    }

    public static void testShadowAttachAndIsolation() throws Exception {
        Parser parser = shadowParser();
        Document doc = parser.parseInput(
            "<div id=host><template shadowrootmode=open><b id=inside>hidden</b></template><i id=light>light</i></div>",
            "http://ex.com/dir/");
        Element host = doc.selectFirst("#host");

        // root attached, host readable, light DOM untouched
        CaseHarness.check(host.shadowRoot() != null, "shadow root attached");
        ShadowRoot root = host.shadowRoot();
        CaseHarness.equal(host, root.host());
        CaseHarness.equal(1, host.childNodeSize());
        CaseHarness.equal(1, host.children().size());
        CaseHarness.equal("i", host.firstElementChild().normalName());
        CaseHarness.equal(0, host.firstElementChild().elementSiblingIndex());
        CaseHarness.equal("light", host.text());

        // host-side queries and traversal do not cross into the root
        CaseHarness.equal(0, doc.select("#inside").size());
        CaseHarness.equal(0, doc.select("template").size());
        CaseHarness.equal(0, doc.select("#host b").size());

        // root-side reads work and do not borrow outer ancestors
        CaseHarness.equal("hidden", root.selectFirst("#inside").text());
        CaseHarness.equal("hidden", root.text());
        CaseHarness.equal(1, root.childNodeSize());
        CaseHarness.equal(0, root.select("#host b").size());
        CaseHarness.check(root.parent() == null, "root has no parent link");

        // baseUri and ownerDocument follow the host
        CaseHarness.equal(doc, root.ownerDocument());
        CaseHarness.equal(doc, root.selectFirst("#inside").ownerDocument());
        CaseHarness.equal("http://ex.com/dir/", root.baseUri());

        // relative links inside the root resolve against the host's base
        Document linked = parser.parseInput(
            "<div id=h><template shadowrootmode=open><a href=rel/p.html>l</a></template></div>", "http://ex.com/dir/");
        CaseHarness.equal("http://ex.com/dir/rel/p.html",
            linked.selectFirst("#h").shadowRoot().selectFirst("a").absUrl("href"));

        // default is off: declaration parses as an ordinary template
        Document plain = Jsoup.parse("<div><template shadowrootmode=open><b id=x>x</b></template></div>");
        CaseHarness.check(plain.selectFirst("div").shadowRoot() == null, "default off: no root");
        CaseHarness.equal(1, plain.select("template > b").size());

        // XML parsing is unchanged, even with the option set
        Parser xmlParser = Parser.xmlParser().setDeclarativeShadowRoots(true);
        Document xml = xmlParser.parseInput("<div><template shadowrootmode=\"open\"><b>x</b></template></div>", "");
        CaseHarness.check(xml.selectFirst("div").shadowRoot() == null, "xml: no root");
        CaseHarness.equal(1, xml.selectFirst("template").childNodeSize());

        // invalid hosts, closed mode, and repeat declarations stay ordinary templates
        Document mixed = parser.parseInput(
            "<p><template shadowrootmode=open><b>p1</b></template></p>"
                + "<div><template shadowrootmode=closed><b>c</b></template></div>"
                + "<span><template shadowrootmode=open><b>s</b></template><template shadowrootmode=open><b>s2</b></template></span>",
            "");
        CaseHarness.check(mixed.selectFirst("p").shadowRoot() == null, "p is not a valid host");
        CaseHarness.equal(1, mixed.select("p > template > b").size());
        CaseHarness.check(mixed.selectFirst("div").shadowRoot() == null, "closed mode stays ordinary");
        CaseHarness.equal(1, mixed.select("div > template > b").size());
        Element span = mixed.selectFirst("span");
        CaseHarness.check(span.shadowRoot() != null, "first declaration attaches");
        CaseHarness.equal("s", span.shadowRoot().text());
        CaseHarness.equal(1, span.children().size());
        CaseHarness.equal("template", span.children().get(0).normalName());
        CaseHarness.equal("s2", span.children().get(0).text());

        // custom (hyphenated) elements may host
        Document custom = parser.parseInput("<my-el><template shadowrootmode=open><b>ce</b></template></my-el>", "");
        CaseHarness.equal("ce", custom.selectFirst("my-el").shadowRoot().text());

        // declarations inside ordinary template content stay ordinary templates
        Document nested = parser.parseInput(
            "<div id=o><template><div id=inner><template shadowrootmode=open><b>x</b></template></div></template></div>", "");
        Element inner = nested.selectFirst("#inner");
        CaseHarness.check(inner.shadowRoot() == null, "declaration in ordinary template stays ordinary");
        CaseHarness.equal(1, inner.select("template > b").size());

        // mismatched end tags inside the root cannot pop outside elements
        Document mismatched = parser.parseInput(
            "<div id=h><template shadowrootmode=open><b>bold</div></template><i id=after>ok</i></div>", "");
        CaseHarness.equal("bold", mismatched.selectFirst("#h").shadowRoot().text());
        CaseHarness.equal("h", mismatched.selectFirst("#after").parent().id());

        // EOF closes out an unclosed root, template-style
        Document eof = parser.parseInput("<div id=h><template shadowrootmode=open><b>eof", "");
        CaseHarness.equal("eof", eof.selectFirst("#h").shadowRoot().text());
    }

    public static void testShadowNestedRootsAndBoundaries() throws Exception {
        Parser parser = shadowParser();

        // shadow content may host its own open roots
        Document doc = parser.parseInput(
            "<div id=o><template shadowrootmode=open><span id=i><template shadowrootmode=open><b id=deep>deep</b></template></span></template></div>",
            "");
        ShadowRoot outer = doc.selectFirst("#o").shadowRoot();
        CaseHarness.check(outer != null, "outer root attached");
        Element innerHost = outer.selectFirst("#i");
        CaseHarness.check(innerHost.shadowRoot() != null, "nested root attached");
        CaseHarness.equal("deep", innerHost.shadowRoot().selectFirst("#deep").text());
        CaseHarness.equal(innerHost, innerHost.shadowRoot().host());
        CaseHarness.equal(0, doc.select("#deep").size());

        // table content modes work inside the root
        Document table = parser.parseInput(
            "<div id=h><template shadowrootmode=open><table><tr><td id=c>cell</td></tr></table></template></div>", "");
        Element td = table.selectFirst("#h").shadowRoot().selectFirst("#c");
        CaseHarness.equal("cell", td.text());
        CaseHarness.equal("tr", td.parent().normalName());
        CaseHarness.equal("tbody", td.parent().parent().normalName());
        CaseHarness.equal("table", td.parent().parent().parent().normalName());

        // a table straddling light and shadow keeps its own contents on each side
        Document split = parser.parseInput(
            "<div id=h><template shadowrootmode=open><table><tr><td id=shc>shadow</td></tr></table></template>"
                + "<table><tr><td id=lic>light</td></tr></table></div>", "");
        CaseHarness.equal("shadow", split.selectFirst("#h").shadowRoot().selectFirst("#shc").text());
        CaseHarness.equal("light", split.selectFirst("#lic").text());

        // forms: inner form does not take the form pointer; inner controls do not join the outer form
        Document forms = parser.parseInput(
            "<form id=outer><div id=h><template shadowrootmode=open><form id=innerf><input id=in2></form></template>"
                + "<input id=in1></div></form>", "");
        FormElement outerForm = (FormElement) forms.selectFirst("#outer");
        CaseHarness.check(outerForm.elements().contains(forms.selectFirst("#in1")), "light control associated");
        CaseHarness.check(!outerForm.elements().contains(forms.selectFirst("#in2")), "shadow control not associated");
        CaseHarness.check(forms.selectFirst("#h").shadowRoot().selectFirst("#innerf") != null, "inner form kept in root");
        CaseHarness.equal(1, outerForm.elements().size());

        // formatting elements outside do not reconstruct inside the root (template marker boundary)
        Document fmt = parser.parseInput(
            "<b><div id=h><template shadowrootmode=open><p id=p>para</template></div></b>", "");
        ShadowRoot fmtRoot = fmt.selectFirst("#h").shadowRoot();
        CaseHarness.equal("p", fmtRoot.firstElementChild().normalName());
        CaseHarness.equal(0, fmtRoot.select("b").size());

        // light DOM after ordinary/declarative template nesting lands back on the host
        Document sequence = parser.parseInput(
            "<div id=h><template><b id=o1>ordinary</b></template>"
                + "<template shadowrootmode=open><template><b id=n1>nested</b></template></template>"
                + "<i id=tail>tail</i></div>", "");
        Element host = sequence.selectFirst("#h");
        CaseHarness.equal(2, host.children().size());
        CaseHarness.equal("template", host.children().get(0).normalName());
        CaseHarness.equal("i", host.children().get(1).normalName());
        CaseHarness.equal("ordinary tail", host.text()); // light DOM only; shadow content stays out
        CaseHarness.equal("nested", host.shadowRoot().selectFirst("#n1").text());
        CaseHarness.equal(0, host.shadowRoot().select("#tail").size());
    }

    public static void testShadowStreamingIsolation() throws Exception {
        Parser parser = shadowParser();
        String html =
            "<div id=h1><template shadowrootmode=open><b id=s1>S1</b></template><i id=l1>L1</i></div>"
                + "<div id=h2><template shadowrootmode=open><b id=s2>S2</b></template></div>";

        // chunked reader: roots are built during the parse, and shadow content is never emitted
        try (StreamParser sp = new StreamParser(parser)) {
            sp.parse(new ChunkedReader(html), "");
            Set<String> ids = new HashSet<>();
            Element host1 = null, host2 = null;
            Iterator<Element> it = sp.iterator();
            while (it.hasNext()) {
                Element el = it.next();
                ids.add(el.id());
                if (el.id().equals("h1")) host1 = el;
                if (el.id().equals("h2")) host2 = el;
            }
            CaseHarness.check(!ids.contains("s1") && !ids.contains("s2"), "shadow content not emitted");
            CaseHarness.check(ids.contains("h1") && ids.contains("h2"), "hosts emitted");
            CaseHarness.check(host1.shadowRoot() != null, "emitted host has its root");
            CaseHarness.equal("S1", host1.shadowRoot().selectFirst("#s1").text());
            CaseHarness.equal("S2", host2.shadowRoot().selectFirst("#s2").text());
        }

        // selectNext / selectFirst never hand out shadow elements; hosts come back complete
        try (StreamParser sp = new StreamParser(parser)) {
            sp.parse(new ChunkedReader(html), "");
            Element first = sp.selectNext("div");
            CaseHarness.equal("h1", first.id());
            CaseHarness.check(first.shadowRoot() != null, "root complete when host returned");
            CaseHarness.equal("S1", first.shadowRoot().text());
            CaseHarness.equal(null, sp.selectFirst("#s1"));
        }

        // stream() likewise skips shadow content
        try (StreamParser sp = new StreamParser(parser)) {
            sp.parse(html, "");
            long leaked = sp.stream().filter(el -> el.id().equals("s1") || el.id().equals("s2")).count();
            CaseHarness.equal(0L, leaked);
        }

        // fragment parse with a context: context untouched, new hosts attach normally
        Document contextDoc = Jsoup.parse("<body></body>");
        Element context = contextDoc.body();
        List<Node> nodes = parser.parseFragmentInput(
            "<div id=f><template shadowrootmode=open><b>frag</b></template></div>", context, "");
        CaseHarness.equal(0, context.childNodeSize());
        CaseHarness.check(contextDoc.select("#f").isEmpty(), "context document unchanged");
        Element fragHost = (Element) nodes.get(0);
        CaseHarness.check(fragHost.shadowRoot() != null, "fragment host attached");
        CaseHarness.equal("frag", fragHost.shadowRoot().text());
    }

    public static void testShadowCloneAndSerialize() throws Exception {
        Parser parser = shadowParser();
        Document doc = parser.parseInput(
            "<div id=h><template shadowrootmode=open><span id=s>sh<template><b id=nt>nested</b></template></span></template>"
                + "<i id=l>light</i></div>",
            "http://ex.com/base/");
        Element host = doc.selectFirst("#h");

        // plain outerHtml stays light-DOM only
        CaseHarness.check(!host.outerHtml().contains("shadowrootmode"), "outerHtml has no declarative template");
        CaseHarness.check(!host.outerHtml().contains("nested"), "outerHtml has no shadow content");

        // shadow-aware serialization: one declarative wrapper, nested ordinary template emitted once
        doc.outputSettings().prettyPrint(false);
        String withShadow = host.outerHtmlWithShadowRoots();
        CaseHarness.equal(
            "<div id=\"h\"><template shadowrootmode=\"open\"><span id=\"s\">sh<template><b id=\"nt\">nested</b></template></span></template><i id=\"l\">light</i></div>",
            withShadow);

        // round trip restores the same root hierarchy
        Document reparsed = parser.parseInput(withShadow, "http://ex.com/base/");
        Element rehost = reparsed.selectFirst("#h");
        CaseHarness.check(rehost.shadowRoot() != null, "root restored on reparse");
        CaseHarness.equal("sh", rehost.shadowRoot().selectFirst("#s").ownText());
        CaseHarness.equal("nested", rehost.shadowRoot().selectFirst("#nt").text());
        CaseHarness.equal("light", rehost.selectFirst("#l").text());
        CaseHarness.equal(1, rehost.children().size());

        // Element.clone deep-copies the root and re-points the host
        Element hostClone = host.clone();
        CaseHarness.check(hostClone.shadowRoot() != null, "clone has a root");
        CaseHarness.check(hostClone.shadowRoot() != host.shadowRoot(), "root is a copy");
        CaseHarness.equal(hostClone, hostClone.shadowRoot().host());
        hostClone.shadowRoot().selectFirst("#s").text("changed");
        CaseHarness.equal("sh", host.shadowRoot().selectFirst("#s").ownText());

        // Document.clone keeps the hierarchy, attached to the cloned hosts
        Document docClone = doc.clone();
        Element clonedHost = docClone.selectFirst("#h");
        CaseHarness.check(clonedHost.shadowRoot() != null, "document clone keeps root");
        CaseHarness.equal(clonedHost, clonedHost.shadowRoot().host());
        CaseHarness.equal(docClone, clonedHost.shadowRoot().ownerDocument());

        // moving a host moves the root's document and base URI with it
        Document other = Jsoup.parse("<div id=newhome></div>", "http://other.com/");
        ShadowRoot root = host.shadowRoot();
        other.selectFirst("#newhome").appendChild(host);
        CaseHarness.equal(other, root.ownerDocument());
        CaseHarness.equal("http://other.com/", root.baseUri());

        // nested roots serialize and restore recursively
        Document nestedDoc = parser.parseInput(
            "<div id=o><template shadowrootmode=open><span id=i><template shadowrootmode=open><b>deep</b></template></span></template></div>",
            "");
        nestedDoc.outputSettings().prettyPrint(false);
        String nestedOut = nestedDoc.selectFirst("#o").outerHtmlWithShadowRoots();
        CaseHarness.equal(2, countOccurrences(nestedOut, "shadowrootmode=\"open\""));
        Document nestedRt = parser.parseInput(nestedOut, "");
        Element innerRt = nestedRt.selectFirst("#o").shadowRoot().selectFirst("#i");
        CaseHarness.check(innerRt.shadowRoot() != null, "nested root restored");
        CaseHarness.equal("deep", innerRt.shadowRoot().text());
    }

    public static void testShadowParserConfigAndReuse() throws Exception {
        Parser parser = shadowParser();
        CaseHarness.check(parser.isDeclarativeShadowRoots(), "option readable");
        CaseHarness.check(!Parser.htmlParser().isDeclarativeShadowRoots(), "default off");

        // newInstance and clone carry the configuration
        Parser instanced = parser.newInstance();
        Parser cloned = parser.clone();
        CaseHarness.check(instanced.isDeclarativeShadowRoots(), "newInstance copies option");
        CaseHarness.check(cloned.isDeclarativeShadowRoots(), "clone copies option");
        Document fromInstance = instanced.parseInput("<div><template shadowrootmode=open><b>y</b></template></div>", "");
        CaseHarness.check(fromInstance.selectFirst("div").shadowRoot() != null, "copied parser attaches");

        // copies are independent: toggling one does not affect the other
        cloned.setDeclarativeShadowRoots(false);
        CaseHarness.check(parser.isDeclarativeShadowRoots(), "original unaffected by copy change");

        // the same parser, reused across documents, keeps roots per-document
        Document first = parser.parseInput("<div id=a><template shadowrootmode=open><b>one</b></template></div>", "");
        Document second = parser.parseInput("<div id=b></div>", "");
        CaseHarness.check(first.selectFirst("#a").shadowRoot() != null, "first doc has root");
        CaseHarness.check(second.selectFirst("#b").shadowRoot() == null, "second doc clean");
        CaseHarness.equal(0, second.select("template").size());
        CaseHarness.equal("one", first.selectFirst("#a").shadowRoot().text());
    }

    static int countOccurrences(String haystack, String needle) {
        int count = 0, from = 0;
        while ((from = haystack.indexOf(needle, from)) != -1) {
            count++;
            from += needle.length();
        }
        return count;
    }
}
