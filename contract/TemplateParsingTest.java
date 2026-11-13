import org.jsoup.Jsoup;
import org.jsoup.nodes.Document;
import org.jsoup.nodes.Element;
import org.jsoup.nodes.Node;
import org.jsoup.parser.ParseError;
import org.jsoup.parser.ParseErrorList;
import org.jsoup.parser.Parser;
import org.jsoup.nodes.FormElement;

import java.util.Arrays;
import java.util.List;

/**
 * Contract tests for HTML template tree construction.
 *
 * <p>Each method is a standalone, named scenario registered in one of the three required groups. Assertions state
 * the expected parent/child relationships and control ids explicitly; they never compare two parser runs against
 * each other and never treat a non-null node as success. Failure messages carry the input and the first structural
 * difference, so a broken combination can be read straight from the harness output.
 */
public final class TemplateParsingTest {

    private TemplateParsingTest() {
    }

    // ------------------------------------------------------------------
    // group "modes": per-template content insertion modes and restoration
    // ------------------------------------------------------------------

    @TemplateHarness.Case(group = "modes")
    public static void headTemplateKeepsTableRowContext() {
        String html = "<head><template id=t><tr id=r><td id=c>draft</td></tr></template></head><body><p id=live>live</p>";
        Document doc = Jsoup.parse(html);

        Element template = doc.head().getElementById("t");
        TemplateHarness.expect(template != null,
            html + " :: template missing from head");
        TemplateHarness.equal("head", describe(template.parentNode()),
            html + " :: template must stay in head, first difference at template parent");

        Element row = template.child(0);
        TemplateHarness.equal("tr", row.tagName(),
            html + " :: tr must be the direct child of the template, first difference at template child 0");
        TemplateHarness.equal("r", row.id(),
            html + " :: direct template child must carry the original row id");
        TemplateHarness.equal("template", row.parentNode().nodeName(),
            html + " :: row parent must be the template, not a synthesised table/tbody");

        Element cell = row.child(0);
        TemplateHarness.equal("td", cell.tagName(),
            html + " :: td must be the direct child of the tr, first difference at row child 0");
        TemplateHarness.equal("c", cell.id(),
            html + " :: cell must keep its id");
        TemplateHarness.equal("draft", cell.text(),
            html + " :: cell text must be preserved verbatim");
        TemplateHarness.expect(doc.body().getElementById("live") != null,
            html + " :: body content after the head template must still parse");
    }

    @TemplateHarness.Case(group = "modes")
    public static void leadingTableElementsNeedNoSyntheticTable() {
        String rowHtml = "<template id=tt><tr id=r><td id=c>x</td></tr></template>";
        Document rowDoc = Jsoup.parse(rowHtml);
        Element rowTemplate = rowDoc.getElementById("tt");
        TemplateHarness.equal("tr", rowTemplate.child(0).tagName(),
            rowHtml + " :: a leading tr must sit directly under the template, first difference at template child 0");
        TemplateHarness.expect(rowDoc.select("table").isEmpty(),
            rowHtml + " :: no table element may be invented to host the row");

        String cellHtml = "<template id=tc><td id=lead>y</td></template>";
        Document cellDoc = Jsoup.parse(cellHtml);
        Element cellTemplate = cellDoc.getElementById("tc");
        Element impliedRow = cellTemplate.child(0);
        TemplateHarness.equal("tr", impliedRow.tagName(),
            cellHtml + " :: a leading td implies a tr, first difference at template child 0");
        TemplateHarness.equal("template", impliedRow.parentNode().nodeName(),
            cellHtml + " :: implied tr must hang directly off the template");
        TemplateHarness.equal("lead", impliedRow.child(0).id(),
            cellHtml + " :: leading cell must be kept under the implied tr");
        TemplateHarness.expect(cellDoc.select("table").isEmpty(),
            cellHtml + " :: the implied row context must not invent a table element");

        String colHtml = "<template id=tcol><col id=col1></template>";
        Document colDoc = Jsoup.parse(colHtml);
        Element col = colDoc.getElementById("col1");
        TemplateHarness.equal("tcol", ((Element) col.parentNode()).id(),
            colHtml + " :: a leading col must be a direct template child, first difference at col parent");

        String captionHtml = "<template id=tcap><caption id=cap>cap</caption></template>";
        Document captionDoc = Jsoup.parse(captionHtml);
        TemplateHarness.equal("tcap", ((Element) captionDoc.getElementById("cap").parentNode()).id(),
            captionHtml + " :: a leading caption must be a direct template child, first difference at caption parent");

        String groupHtml = "<template id=tcg><colgroup id=cg><col id=col2></colgroup></template>";
        Document groupDoc = Jsoup.parse(groupHtml);
        TemplateHarness.equal("tcg", ((Element) groupDoc.getElementById("cg").parentNode()).id(),
            groupHtml + " :: a leading colgroup must be a direct template child, first difference at colgroup parent");
        TemplateHarness.equal("cg", ((Element) groupDoc.getElementById("col2").parentNode()).id(),
            groupHtml + " :: a real colgroup must still hold its col children");

        String bodyHtml = "<template id=ttb><tbody id=tb><tr id=r2><th id=h>H</th></tr></tbody></template>";
        Document bodyDoc = Jsoup.parse(bodyHtml);
        TemplateHarness.equal("ttb", ((Element) bodyDoc.getElementById("tb").parentNode()).id(),
            bodyHtml + " :: a leading tbody must be a direct template child, first difference at tbody parent");
        TemplateHarness.equal("r2", ((Element) bodyDoc.getElementById("h").parentNode()).id(),
            bodyHtml + " :: the th must keep its row parent inside the template table context");
    }

    @TemplateHarness.Case(group = "modes")
    public static void plainContentUsesBodyMode() {
        String html = "<template id=t><div id=d><p id=p>text</p></div></template>";
        Document doc = Jsoup.parse(html);
        Element template = doc.getElementById("t");

        TemplateHarness.equal("div", template.child(0).tagName(),
            html + " :: ordinary content must follow in-body rules, first difference at template child 0");
        TemplateHarness.equal("d", template.child(0).id(),
            html + " :: the div must keep its id as a direct template child");
        TemplateHarness.equal("p", template.getElementById("p").tagName(),
            html + " :: nested paragraph must be retained");
        TemplateHarness.equal("text", template.getElementById("p").text(),
            html + " :: paragraph text must be retained inside the template");
    }

    @TemplateHarness.Case(group = "modes")
    public static void lexicalElementsKeepTheirTokeniserRules() {
        String html = "<template id=t>"
            + "<style id=s>.a > .b { content: \"</style>\" }</style>"
            + "<title id=ti>head <b> text</title>"
            + "<script id=sc>var v = 1 < 2;</script>"
            + "<p id=p>after</p></template>";
        Document doc = Jsoup.parse(html);
        Element template = doc.getElementById("t");

        TemplateHarness.expect(template.getElementById("s").data().contains(".a > .b"),
            html + " :: style content must be read as raw text, first difference at style data node");
        TemplateHarness.expect(template.getElementById("ti").text().contains("head <b> text"),
            html + " :: title content must be read as RCDATA instead of opening a b element");
        TemplateHarness.expect(template.getElementById("sc").data().contains("var v = 1 < 2;"),
            html + " :: script content must be read as script data, first difference at script data node");
        TemplateHarness.equal("p", template.child(template.children().size() - 1).tagName(),
            html + " :: parsing must return to template content after the lexical elements");
    }

    @TemplateHarness.Case(group = "modes")
    public static void nestedTemplateRestoresOuterMode() {
        String html = "<template id=outer><div id=d>"
            + "<template id=inner><tr id=ir><td id=ic>nested</td></tr></template>"
            + "<p id=after>ok</p></div></template>";
        Document doc = Jsoup.parse(html);
        Element outer = doc.getElementById("outer");
        Element inner = doc.getElementById("inner");

        TemplateHarness.equal("d", outer.child(0).id(),
            html + " :: outer template must start in body mode, first difference at outer child 0");
        TemplateHarness.equal("d", inner.parentNode().attr("id"),
            html + " :: nested template must stay inside the outer div, first difference at inner template parent");
        TemplateHarness.equal("tr", inner.child(0).tagName(),
            html + " :: nested template must independently open with row context, first difference at inner child 0");
        TemplateHarness.equal("ic", inner.child(0).child(0).id(),
            html + " :: nested cell must be kept under the nested row");
        Element after = doc.getElementById("after");
        TemplateHarness.equal("d", after.parentNode().attr("id"),
            html + " :: after the nested template closes, body mode must be restored, first difference at p parent");
    }

    @TemplateHarness.Case(group = "modes")
    public static void siblingTemplatesKeepIndependentModes() {
        String html = "<template id=h1><tr id=r1><td>a</td></tr></template>"
            + "<template id=h2><div id=d2>b</div></template>";
        Document doc = Jsoup.parse(html);

        TemplateHarness.equal("h1", ((Element) doc.getElementById("r1").parentNode()).id(),
            html + " :: first template row must belong to the first template, first difference at r1 parent");
        TemplateHarness.equal("h2", ((Element) doc.getElementById("d2").parentNode()).id(),
            html + " :: the second template must reset to its own body mode, first difference at d2 parent");
        TemplateHarness.expect(doc.getElementById("h1").getElementById("d2") == null,
            html + " :: template content must not leak between sibling templates");
    }

    // ----------------------------------------------------------------------
    // group "boundaries": tables, active formatting elements, and form state
    // ----------------------------------------------------------------------

    @TemplateHarness.Case(group = "boundaries")
    public static void mismatchedEndTagsCannotPopOuterContext() {
        String html = "<table id=tab><tr id=row><td id=cell><b id=bold>hi"
            + "<template id=t><td id=bad></td></tr></table></template>tail"
            + "</b></td></tr></table>";
        Document doc = Jsoup.parse(html);

        TemplateHarness.expect(doc.getElementById("tab") != null,
            html + " :: the outer table must not be closed by template end tags");
        TemplateHarness.expect(doc.getElementById("row") != null,
            html + " :: the outer row must survive template end tags, first difference at outer row");
        TemplateHarness.expect(doc.getElementById("cell") != null,
            html + " :: the outer cell must survive template end tags, first difference at outer cell");
        TemplateHarness.equal("cell", ((Element) doc.getElementById("bold").parentNode()).id(),
            html + " :: the outer formatting element must stay in the cell, first difference at b parent");
        Element bad = doc.getElementById("bad");
        Node boundary = bad;
        while (boundary != null && !boundary.nodeName().equals("template")) {
            boundary = boundary.parentNode();
        }
        TemplateHarness.equal("t", boundary == null ? "<none>" : boundary.attr("id"),
            html + " :: the stray cell must be contained by the template, first difference walking up from td#bad");
    }

    @TemplateHarness.Case(group = "boundaries")
    public static void fosterParentingRespectsTemplateBoundary() {
        String inside = "<template id=tpl><table id=inner><tr><td>cell</td></tr><b id=fostered>x</b></table>"
            + "<p id=after>y</p></template>";
        Document insideDoc = Jsoup.parse(inside);
        Element template = insideDoc.getElementById("tpl");
        Element fostered = insideDoc.getElementById("fostered");
        TemplateHarness.equal("tpl", ((Element) fostered.parentNode()).id(),
            inside + " :: a node fostered inside a template table must land in the template, first difference at b parent");
        TemplateHarness.expect(insideDoc.body().children().stream().noneMatch(e -> e.id().equals("fostered")),
            inside + " :: fostered content must never escape to the document body");
        TemplateHarness.equal("tpl", ((Element) insideDoc.getElementById("after").parentNode()).id(),
            inside + " :: content following the template table must stay in the template");

        String outside = "<table id=outer><tr><td id=cell><template id=inner>"
            + "<tr id=ir><td>ic</td></tr><div id=dv>d</div></template></td></tr></table>";
        Document outsideDoc = Jsoup.parse(outside);
        Element outer = outsideDoc.getElementById("outer");
        TemplateHarness.expect(outsideDoc.body().children().size() == 1 && outsideDoc.body().child(0) == outer,
            outside + " :: template-internal fostering must not insert before the nearest outer table, "
                + "first difference at body children");
        TemplateHarness.equal("inner", ((Element) outsideDoc.getElementById("dv").parentNode()).id(),
            outside + " :: the div must remain inside the template rather than foster out of the cell");
    }

    @TemplateHarness.Case(group = "boundaries")
    public static void templateFormsAreIsolatedFromOuterForm() {
        String html = "<body><form id=outer><input id=a>"
            + "<template id=t><form id=inner><input id=b></form></template>"
            + "<input id=c></form>";
        Document doc = Jsoup.parse(html);
        FormElement outer = (FormElement) doc.getElementById("outer");
        Element template = doc.getElementById("t");
        Element inner = doc.getElementById("inner");

        List<String> outerIds = outer.elements().eachAttr("id");
        TemplateHarness.equal(Arrays.asList("a", "c"), outerIds,
            html + " :: outer form must list exactly the controls parsed outside the template, "
                + "first difference in FormElement.elements()");
        TemplateHarness.equal("t", inner.parentNode().attr("id"),
            html + " :: the inner form must remain inside the template, first difference at inner form parent");
        TemplateHarness.equal("inner", ((Element) doc.getElementById("b").parentNode()).id(),
            html + " :: the template control must keep its template form parent in the DOM");
        TemplateHarness.equal("outer", ((Element) doc.getElementById("c").parentNode()).id(),
            html + " :: after leaving the template, controls must re-associate with the outer form");
    }

    @TemplateHarness.Case(group = "boundaries")
    public static void internalTemplateFormRegistersNoControls() {
        String html = "<template id=t><form id=inner><input id=b><select id=s><option id=o>x</option></select></form></template>";
        Document doc = Jsoup.parse(html);
        FormElement inner = (FormElement) doc.getElementById("inner");

        TemplateHarness.expect(inner != null,
            html + " :: a form element inside a template must be preserved in the DOM");
        TemplateHarness.equal(0, inner.elements().size(),
            html + " :: template-internal forms must not register any controls in elements(), "
                + "first difference at inner form control count");
        TemplateHarness.expect(doc.getElementById("b") != null && doc.getElementById("s") != null,
            html + " :: the unregistered controls must still exist in the template tree");
    }

    @TemplateHarness.Case(group = "boundaries")
    public static void activeFormattingMarkersStopAtTemplate() {
        String html = "<b id=outerb>before<template id=t><b>in</i></b><i id=leak>x</i></template>after</b>";
        Document doc = Jsoup.parse(html);
        Element outerB = doc.getElementById("outerb");

        TemplateHarness.expect(outerB != null,
            html + " :: the outer formatting element must survive malformed template formatting");
        TemplateHarness.expect(outerB.childNode(0).outerHtml().contains("before"),
            html + " :: text before the template must stay in the outer b, first difference at b first child");
        TemplateHarness.expect(outerB.childNode(outerB.childNodeSize() - 1).outerHtml().contains("after"),
            html + " :: text after the template must stay in the outer b, first difference at b last child");
        TemplateHarness.expect(doc.getElementById("leak").parentNode().nodeName().equals("template"),
            html + " :: formatting opened in the template must not reconstruct across the template boundary, "
                + "first difference at i#leak parent");
    }

    @TemplateHarness.Case(group = "boundaries")
    public static void htmlAndBodyTagsInTemplateDoNotMergeAttributes() {
        String html = "<html lang=en><body class=outer>"
            + "<template id=t><html id=fake lang=xx><body id=fakebody class=inner><p id=p>x</p></template>";
        Document doc = Jsoup.parse(html);

        TemplateHarness.equal("en", doc.child(0).attr("lang"),
            html + " :: a html start tag in template content must not merge onto the document root, "
                + "first difference at root lang");
        TemplateHarness.expect(!doc.body().hasClass("inner"),
            html + " :: a body start tag in template content must not merge onto the active body, "
                + "first difference at body class");
        TemplateHarness.expect(doc.getElementById("p") != null,
            html + " :: template content after the stray html/body tags must still be retained");
    }

    // ---------------------------------------------------------------
    // group "fragments": context fragments, EOF handling, parser reuse
    // ---------------------------------------------------------------

    @TemplateHarness.Case(group = "fragments")
    public static void templateContextFragmentParsesTableContents() {
        Element context = Jsoup.parse("<template id=ctx></template>").getElementById("ctx");

        String rowFragment = "<tr id=fr><td id=fc>z</td></tr>";
        List<Node> rowNodes = Parser.parseFragment(rowFragment, context, "");
        TemplateHarness.equal(1, rowNodes.size(),
            rowFragment + " :: the row must be returned as a top-level fragment node, first difference at fragment size");
        Element fragmentRow = (Element) rowNodes.get(0);
        TemplateHarness.equal("tr", fragmentRow.tagName(),
            rowFragment + " :: no table may be synthesised, first difference at fragment node 0 tag");
        TemplateHarness.equal("fc", fragmentRow.child(0).id(),
            rowFragment + " :: the cell must hang directly under the fragment row");

        String colFragment = "<col id=fcol><caption id=fcap>c</caption>";
        List<Node> colNodes = Parser.parseFragment(colFragment, context, "");
        TemplateHarness.equal(2, colNodes.size(),
            colFragment + " :: col and caption must both be returned directly, first difference at fragment size");
        TemplateHarness.equal("fcol", colNodes.get(0).attr("id"),
            colFragment + " :: first fragment node must be the col");
        TemplateHarness.equal("fcap", colNodes.get(1).attr("id"),
            colFragment + " :: second fragment node must be the caption, not a table child");
    }

    @TemplateHarness.Case(group = "fragments")
    public static void fragmentParseLeavesContextAndAncestorsUntouched() {
        Document contextDoc = Jsoup.parse("<div id=wrap><template id=ctx></template></div>");
        Element context = contextDoc.getElementById("ctx");
        String before = contextDoc.outerHtml();
        String fragment = "<tr><td>z</td></tr>";

        List<Node> nodes = Parser.parseFragment(fragment, context, "");
        TemplateHarness.expect(nodes.size() == 1,
            fragment + " :: the fragment must be returned rather than attached to the context");
        TemplateHarness.expect(context.childNodes().isEmpty(),
            fragment + " :: the template context must not gain children, first difference at context children");
        TemplateHarness.equal(before, contextDoc.outerHtml(),
            fragment + " :: the context document and its ancestors must be unchanged, first difference in serialised doc");
    }

    @TemplateHarness.Case(group = "fragments")
    public static void contextlessFragmentKeepsTemplateContents() {
        String fragment = "<template id=ft><tr id=ftr><td id=fc>q</td></tr></template><p id=fp>p</p>";
        List<Node> nodes = Parser.parseFragment(fragment, null, "");

        Element template = null;
        Element paragraph = null;
        for (Node node : nodes) {
            if (node instanceof Element) {
                if (((Element) node).getElementById("ft") != null) {
                    template = ((Element) node).getElementById("ft");
                }
                if (((Element) node).getElementById("fp") != null) {
                    paragraph = ((Element) node).getElementById("fp");
                }
            }
        }
        TemplateHarness.expect(template != null,
            fragment + " :: the no-context fragment must retain the template element, first difference at template lookup");
        TemplateHarness.equal("ftr", template.child(0).id(),
            fragment + " :: the template row must survive fragment parsing, first difference at template child 0");
        TemplateHarness.equal("fc", template.child(0).child(0).id(),
            fragment + " :: the template cell must keep its row parent in the fragment");
        TemplateHarness.expect(paragraph != null && paragraph.parent() != template,
            fragment + " :: the sibling paragraph must remain outside the template, first difference at p parent");
    }

    @TemplateHarness.Case(group = "fragments")
    public static void unclosedTemplatesAreFinalisedAtEof() {
        String nested = "<template id=outer><p id=op>keep<template id=inner><tr id=ir><td>open";
        Document nestedDoc = Jsoup.parse(nested);
        Element outer = nestedDoc.getElementById("outer");
        Element inner = nestedDoc.getElementById("inner");

        TemplateHarness.expect(outer != null,
            nested + " :: the outer unclosed template must be finalised at EOF instead of aborting");
        TemplateHarness.equal("op", outer.child(0).id(),
            nested + " :: content before the nested template must survive EOF teardown, first difference at outer child 0");
        TemplateHarness.expect(inner != null && "tr".equals(inner.child(0).tagName()),
            nested + " :: the nested unclosed template must also be finalised, first difference at inner child 0");
        TemplateHarness.expect(inner.getElementById("ir") != null,
            nested + " :: table markers left inside the template must be removed together with its boundary");

        Parser tracking = Parser.htmlParser().setTrackErrors(5);
        tracking.parseInput(nested, "");
        boolean sawEofError = false;
        for (ParseError error : tracking.getErrors()) {
            sawEofError |= error.getErrorMessage().contains("EOF");
        }
        TemplateHarness.expect(sawEofError,
            nested + " :: an unclosed template at EOF must be reported through ParseErrorList");
    }

    @TemplateHarness.Case(group = "fragments")
    public static void reusedParserCarriesNoTemplateState() {
        Parser parser = Parser.htmlParser();
        String first = "<template id=t1><tr><td>a</td></tr>";
        String second = "<p id=p2>b</p>";

        Document firstDoc = parser.parseInput(first, "");
        Document secondDoc = parser.parseInput(second, "");

        TemplateHarness.expect(firstDoc.getElementById("t1") != null,
            first + " :: the first parse must keep its template content");
        TemplateHarness.expect(secondDoc.select("template").isEmpty(),
            second + " :: template state must not leak into the next parse, first difference at template count");
        TemplateHarness.equal("p2", secondDoc.body().child(0).id(),
            second + " :: the second document must parse as a plain body document");
        Document thirdDoc = parser.parseInput("<template id=t3><div id=d3>c</div></template>", "");
        TemplateHarness.expect(thirdDoc.getElementById("d3") != null,
            "third parse on the same parser must independently build template content again");
    }

    @TemplateHarness.Case(group = "fragments")
    public static void parseErrorsCoverStrayEndTagAndEofWithCap() {
        String html = "</template><template id=t><tr><td>a";

        Parser capped = Parser.htmlParser().setTrackErrors(1);
        capped.parseInput(html, "");
        ParseErrorList cappedErrors = capped.getErrors();
        TemplateHarness.equal(1, cappedErrors.size(),
            html + " :: setTrackErrors must cap the collected errors, first difference at error count");
        TemplateHarness.equal(11, cappedErrors.get(0).getPosition(),
            html + " :: the stray template end tag must keep its reader position, first difference at error position");

        Parser tracking = Parser.htmlParser().setTrackErrors(10);
        tracking.parseInput(html, "");
        ParseErrorList errors = tracking.getErrors();
        TemplateHarness.expect(errors.size() >= 2,
            html + " :: both the stray end tag and the unclosed-template EOF should be reported, got " + errors.size());
        TemplateHarness.expect(errors.get(0).getPosition() >= 0 && errors.get(0).getPosition() <= html.length(),
            html + " :: every recorded error must carry a position inside the input");
    }

    private static String describe(Node node) {
        if (node == null) {
            return "<null>";
        }
        if (node instanceof Element) {
            Element element = (Element) node;
            return element.id().isEmpty() ? element.tagName() : element.tagName() + "#" + element.id();
        }
        return node.nodeName();
    }
}
