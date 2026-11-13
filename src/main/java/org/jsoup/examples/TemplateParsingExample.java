package org.jsoup.examples;

import org.jsoup.Jsoup;
import org.jsoup.nodes.Document;
import org.jsoup.nodes.Element;
import org.jsoup.nodes.FormElement;
import org.jsoup.nodes.Node;
import org.jsoup.parser.Parser;

import java.util.List;

/**
 * Example: parsing pages that carry unexpanded HTML {@code <template>} elements.
 * <p>
 * jsoup builds template contents as ordinary child {@link Element}s of the {@code template}, using the HTML
 * Standard's "in template" tree-construction rules. There is no separate {@code content} or DocumentFragment
 * object: the normal select, clone, and serialise APIs work on the template subtree directly.
 * </p>
 * <p>
 * To invoke from the command line, assuming the jsoup classes are on the class path:</p>
 * <p><code>java -cp jsoup.jar org.jsoup.examples.TemplateParsingExample</code></p>
 */
public class TemplateParsingExample {

    public static void main(String[] args) {
        TemplateParsingExample example = new TemplateParsingExample();
        example.tableRowsInsideHeadTemplate();
        example.nestedTemplatesKeepSeparateModes();
        example.templateFormsAreIsolated();
        example.fragmentParsingAgainstTemplateContext();
        example.reusingOneParserForSeveralInputs();
    }

    /** A template in head that opens with table rows keeps the rows, without inventing a surrounding table. */
    void tableRowsInsideHeadTemplate() {
        String html = "<head><template id=\"row-template\">"
            + "<tr id=\"draft-row\"><td id=\"draft-cell\">draft</td></tr>"
            + "</template></head><body><p>published content</p></body>";

        Document doc = Jsoup.parse(html);
        Element template = doc.head().getElementById("row-template");

        Element row = template.child(0);                 // tr is a direct child of the template
        Element cell = row.child(0);                    // td is a direct child of the tr
        System.out.println("row parent: " + row.parentNode().nodeName());   // template
        System.out.println("cell text: " + cell.text());                    // draft
        System.out.println("serialised: " + template.outerHtml());
    }

    /** Each template layer keeps its own content mode; closing a nested template restores the outer one. */
    void nestedTemplatesKeepSeparateModes() {
        String html = "<template id=\"outer\"><div id=\"wrapper\">"
            + "<template id=\"inner\"><tr><td>nested row</td></tr></template>"
            + "<p id=\"after\">back to body mode</p></div></template>";

        Document doc = Jsoup.parse(html);
        Element inner = doc.getElementById("inner");

        System.out.println("inner first child: " + inner.child(0).tagName());       // tr
        System.out.println("paragraph parent: " + ((Element) doc.getElementById("after")
            .parentNode()).id());                                                   // wrapper
    }

    /** Forms and controls inside a template never register on an outer form's elements() list. */
    void templateFormsAreIsolated() {
        String html = "<body><form id=\"outer\"><input id=\"a\">"
            + "<template id=\"t\"><form id=\"inner\"><input id=\"b\"></form></template>"
            + "<input id=\"c\"></form>";

        Document doc = Jsoup.parse(html);
        FormElement outer = (FormElement) doc.getElementById("outer");
        Element inner = doc.getElementById("inner");

        System.out.println("outer controls: " + outer.elements().eachAttr("id"));   // [a, c]
        System.out.println("inner controls: " + ((FormElement) inner).elements()
            .eachAttr("id"));                                                         // []
        System.out.println("input b still in DOM: " + (doc.getElementById("b") != null));
    }

    /** A template element can be the fragment parse context, so bare rows/cells/cols parse directly. */
    void fragmentParsingAgainstTemplateContext() {
        Element context = Jsoup.parse("<template id=\"context\"></template>")
            .getElementById("context");

        List<Node> fragment = Parser.parseFragment(
            "<tr id=\"fragment-row\"><td>fragment cell</td></tr>", context, "");

        Element row = (Element) fragment.get(0);
        System.out.println("fragment root: " + row.tagName());                       // tr
        System.out.println("fragment cell: " + row.child(0).text());                 // fragment cell
        System.out.println("context untouched: " + context.childNodes().isEmpty());  // true
    }

    /** Template mode, formatting markers, and form state never leak across parses on one Parser. */
    void reusingOneParserForSeveralInputs() {
        Parser parser = Parser.htmlParser();

        Document first = parser.parseInput("<template id=\"t\"><tr><td>unexpanded</td></tr>", "");
        Document second = parser.parseInput("<p id=\"plain\">ordinary page</p>", "");

        System.out.println("first has template: " + (first.getElementById("t") != null));   // true
        System.out.println("second template count: " + second.select("template").size());   // 0
        System.out.println("second paragraph: " + second.getElementById("plain").text());   // ordinary page
    }
}
