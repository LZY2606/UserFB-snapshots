import com.sun.net.httpserver.HttpServer;
import org.jsoup.Connection;
import org.jsoup.Jsoup;
import org.jsoup.nodes.Document;
import org.jsoup.nodes.Element;
import org.jsoup.nodes.FormElement;
import org.jsoup.select.Elements;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 Contract tests for the association between form-listed elements and their owning form, after parse-time links and
 subsequent DOM edits. Registered with {@link FormHarness} in the ownership, edits, and copies groups.
 */
public final class FormOwnerTest {
    // ---------------------------------------------------------------- helpers

    private static FormElement form(Element el) {
        FormHarness.expect(el != null, "expected a FormElement");
        return (FormElement) el;
    }

    /** The id of the supplied element's current form owner, or "null". */
    private static String ownerId(Element el) {
        FormElement owner = el.formOwner();
        return owner == null ? "null" : owner.id();
    }

    private static String ids(FormElement form) {
        StringBuilder sb = new StringBuilder();
        for (Element el : form.elements()) {
            if (sb.length() > 0) sb.append(',');
            sb.append(el.id());
        }
        return sb.toString();
    }

    private static int originalControls(FormElement form, Document original) {
        int count = 0;
        for (Element el : form.elements()) {
            if (el.ownerDocument() == original) count++;
        }
        return count;
    }

    private static String join(Iterable<Connection.KeyVal> data) {
        StringBuilder sb = new StringBuilder();
        for (Connection.KeyVal kv : data) {
            if (sb.length() > 0) sb.append('&');
            sb.append(kv.key()).append('=').append(kv.value());
        }
        return sb.toString();
    }

    private static String dataString(FormElement form) {
        return join(form.formData());
    }

    private static String queryString(FormElement form) {
        return join(form.submit().request().data());
    }

    // The parser associates the hidden input `a` and the foster-parented `b` with the form `f`, even though the form
    // is not their ancestor (and doesn't contain `b` at all in the tree).
    private static final String TableFormHtml =
        "<table><tr><form id=f><input type=hidden id=a name=a value=1><td><input id=b name=b value=2></td></tr></table>";

    // ---------------------------------------------------------------- ownership

    @FormHarness.Case(group = "ownership")
    public static void parserAssociatesFosterParentedControls() {
        Document doc = Jsoup.parse(TableFormHtml, "http://localhost/query");
        FormElement f = form(doc.getElementById("f"));
        Element a = doc.getElementById("a");
        Element b = doc.getElementById("b");

        FormHarness.equal(f, a.formOwner(), "hidden input is associated with the form");
        FormHarness.equal(f, b.formOwner(), "foster-parented input is associated with the form");
        FormHarness.expect(!f.getAllElements().contains(b), "the foster-parented input is not a descendant of the form");
        FormHarness.equal("a,b", ids(f), "both controls are listed, in tree order");
        FormHarness.equal("a=1&b=2", dataString(f), "both controls are submitted");
        FormHarness.equal("a=1&b=2", queryString(f), "submit uses the same association");
    }

    @FormHarness.Case(group = "ownership")
    public static void nonFormListedElementsHaveNoForm() {
        Document doc = Jsoup.parse("<form id=f><div id=d><span id=s>x</span></div><input id=i><textarea id=t></textarea></form>");
        FormHarness.equal(null, doc.getElementById("d").formOwner(), "a div is not form-listed");
        FormHarness.equal(null, doc.getElementById("s").formOwner(), "a span is not form-listed");
        FormHarness.equal(null, doc.getElementById("f").formOwner(), "a form is not itself a form control");
        FormHarness.equal("f", ownerId(doc.getElementById("i")), "an input is form-listed");
        FormHarness.equal("f", ownerId(doc.getElementById("t")), "a textarea is form-listed");
    }

    @FormHarness.Case(group = "ownership")
    public static void nearestAncestorFormIsUsedWithoutFormAttribute() {
        Document doc = Jsoup.parse("<form id=outer><div id=wrap><input id=k name=k value=1></div></form>");
        FormHarness.equal("outer", ownerId(doc.getElementById("k")), "nearest ancestor form");
        doc.getElementById("wrap").append("<input id=m name=m value=2>");
        FormHarness.equal("outer", ownerId(doc.getElementById("m")), "newly appended control finds ancestor form");
        FormHarness.equal("k=1&m=2", dataString(form(doc.getElementById("outer"))), "both submitted");
    }

    @FormHarness.Case(group = "ownership")
    public static void explicitFormAttributeResolvesByIdInDocument() {
        Document doc = Jsoup.parse("<form id=one><input id=a name=a value=1></form>" +
            "<form id=two><input id=b name=b value=2 form=one></form>");
        Element b = doc.getElementById("b");
        FormHarness.equal("one", ownerId(b), "form attribute overrides the ancestor form");
        FormHarness.equal("a=1&b=2", dataString(form(doc.getElementById("one"))),
            "explicitly associated control submits with the target");
        FormHarness.equal("", dataString(form(doc.getElementById("two"))), "ancestor form no longer submits the control");
    }

    @FormHarness.Case(group = "ownership")
    public static void explicitIdUsesFirstMatchingElementInTreeOrder() {
        Document doc = Jsoup.parse("<div id=target></div><form id=target><input id=q name=q value=v></form>" +
            "<input id=w name=w value=2 form=target>");
        Element w = doc.getElementById("w");
        FormHarness.equal(null, w.formOwner(), "first match is not a form, so no owner (no ancestor fallback)");

        doc.selectFirst("div#target").remove();
        FormHarness.equal("target", ownerId(w), "after removing the non-form, the form is the first match");
    }

    @FormHarness.Case(group = "ownership")
    public static void missingOrEmptyFormAttributeDoesNotFallBack() {
        Document doc = Jsoup.parse("<form id=f><input id=a name=a value=1 form=missing>" +
            "<input id=b name=b value=2 form=''></form>");
        FormHarness.equal(null, doc.getElementById("a").formOwner(), "unresolvable form attribute has no fallback");
        FormHarness.equal(null, doc.getElementById("b").formOwner(), "empty form attribute has no fallback");

        doc.getElementById("a").attr("form", "f");
        FormHarness.equal("f", ownerId(doc.getElementById("a")), "changing form attribute re-resolves");
    }

    @FormHarness.Case(group = "ownership")
    public static void explicitIdIsNotResolvedInDetachedTree() {
        Document doc = Jsoup.parse("<form id=f><div id=box><input id=k name=k value=k form=f></div></form>");
        Element box = doc.getElementById("box");
        box.remove();
        FormHarness.equal(null, box.getElementById("k").formOwner(), "detached tree ignores the explicit id");

        // but an ancestor form in the detached tree is still used
        Document doc2 = Jsoup.parse("<body><form id=g><input id=m name=m value=m></form></body>");
        Element g = doc2.getElementById("g");
        g.remove();
        FormHarness.equal("g", ownerId(g.getElementById("m")), "detached tree still finds an ancestor form");
    }

    // ---------------------------------------------------------------- edits

    @FormHarness.Case(group = "edits")
    public static void parseLinkSurvivesUnrelatedEdits() {
        Document doc = Jsoup.parse(TableFormHtml, "http://localhost/query");
        FormElement f = form(doc.getElementById("f"));
        Element a = doc.getElementById("a");
        Element b = doc.getElementById("b");

        // add and remove unrelated siblings, and change name/value of the controls
        Element cell = doc.selectFirst("td");
        cell.appendElement("span").attr("id", "sibling1");
        cell.appendElement("span").attr("id", "sibling2").remove();
        a.attr("name", "a2").attr("value", "10");
        b.attr("value", "20");

        FormHarness.equal(f, a.formOwner(), "unrelated sibling edits do not lose the parse association");
        FormHarness.equal(f, b.formOwner(), "foster-parented control keeps its association");
        FormHarness.equal("a2=10&b=20", dataString(f), "values reflect the edits");
        FormHarness.equal("a,b", ids(f), "both are still listed");
    }

    @FormHarness.Case(group = "edits")
    public static void movingControlContainerInvalidatesParseLink() {
        Document doc = Jsoup.parse("<form id=x><div id=box><input id=q name=q value=v></div></form><form id=y></form>");
        FormElement x = form(doc.getElementById("x"));
        FormElement y = form(doc.getElementById("y"));
        Element q = doc.getElementById("q");

        y.appendChild(doc.getElementById("box")); // move the container (with the input) into the other form
        FormHarness.equal("y", ownerId(q), "moved control resolves to its new ancestor form");
        FormHarness.equal("", dataString(x), "old form no longer sends the control");
        FormHarness.equal("q=v", dataString(y), "new form sends it");
        FormHarness.equal("", ids(x), "old form's element list no longer contains it");
        FormHarness.equal("q", ids(y), "new form's element list contains it");
    }

    @FormHarness.Case(group = "edits")
    public static void removingControlInvalidatesParseLinkAndReattachDoesNotReviveIt() {
        Document doc = Jsoup.parse(TableFormHtml, "http://localhost/query");
        FormElement f = form(doc.getElementById("f"));
        Element a = doc.getElementById("a");
        Element b = doc.getElementById("b");

        a.remove();
        FormHarness.equal(null, a.formOwner(), "a removed control has no owner");
        FormHarness.equal("b=2", dataString(f), "the removed control is no longer submitted");

        doc.selectFirst("td").appendChild(a);
        FormHarness.equal(null, a.formOwner(), "reattaching elsewhere does not revive the old parse link");
        FormHarness.expect(!f.elements().contains(a), "the old form does not list it again");

        b.remove();
        FormHarness.equal(null, b.formOwner(), "foster-parented control loses association when detached");
        FormHarness.equal("", ids(f), "form has no controls after removal");
    }

    @FormHarness.Case(group = "edits")
    public static void removingFormInvalidatesItsParseLinks() {
        Document doc = Jsoup.parse(TableFormHtml, "http://localhost/query");
        FormElement f = form(doc.getElementById("f"));
        Element a = doc.getElementById("a");

        f.remove();
        FormHarness.equal(null, a.formOwner(), "the control loses its owner when the form is detached");

        // And it can be re-adopted elsewhere without reviving the association
        doc.selectFirst("body").appendChild(f);
        FormHarness.expect(!f.elements().contains(a), "the re-adopted form does not regain the control");
    }

    @FormHarness.Case(group = "edits")
    public static void movingAncestorIntoAnotherFormChangesOwner() {
        Document doc = Jsoup.parse("<form id=x><div id=box><input id=q name=q value=v></div></form><form id=y></form>");
        FormElement x = form(doc.getElementById("x"));
        FormElement y = form(doc.getElementById("y"));
        Element q = doc.getElementById("q");

        y.appendChild(doc.getElementById("box"));
        FormHarness.equal("y", ownerId(q), "control follows its container to the new form");
        FormHarness.equal("", dataString(x), "old form no longer sends it");
        FormHarness.equal("q=v", dataString(y), "new form sends it");
    }

    @FormHarness.Case(group = "edits")
    public static void emptyFormDropsDescendantControls() {
        Document doc = Jsoup.parse("<form id=f><input id=a name=a value=1><div id=box><input id=b name=b value=2></div></form>");
        FormElement f = form(doc.getElementById("f"));

        f.empty();
        FormHarness.equal("", dataString(f), "emptying the form drops all descendant controls");
        FormHarness.equal("", ids(f), "no controls remain listed");

        Element a = new Element("input").attr("id", "a").attr("name", "a").attr("value", "1");
        doc.body().appendChild(a);
        FormHarness.equal(null, a.formOwner(), "a control removed by empty() has no owner afterwards");
    }

    @FormHarness.Case(group = "edits")
    public static void insertingDuplicateIdTargetAffectsResolution() {
        Document doc = Jsoup.parse("<form id=f><input id=a name=a value=1></form><input id=q name=q value=v form=f>");
        Element q = doc.getElementById("q");
        FormHarness.equal("f", ownerId(q), "form attribute resolves to the form");

        Element nonForm = new Element("div").attr("id", "f");
        doc.body().insertChildren(0, nonForm);
        FormHarness.expect(doc.getElementById("f") == nonForm, "the inserted div is now the first element with the id");
        FormHarness.equal(null, q.formOwner(), "a non-form first with the same id wins; no fallback");

        nonForm.remove();
        FormHarness.equal("f", ownerId(q), "removing the shadowing element restores the form association");
    }

    @FormHarness.Case(group = "edits")
    public static void movingAFormAffectsIdResolution() {
        Document doc = Jsoup.parse("<div id=host></div><form id=f><input id=q name=q value=v form=f></form>");
        Element formEl = doc.getElementById("f");
        FormHarness.equal("f", ownerId(doc.getElementById("q")), "initially resolves to the form");

        doc.getElementById("host").appendChild(formEl);
        FormHarness.equal("f", ownerId(doc.getElementById("q")), "moving the form keeps the id resolution (same document)");
    }

    @FormHarness.Case(group = "edits")
    public static void addElementLinksAndDeduplicates() {
        Document doc = Jsoup.parse("<form id=f></form><input id=q name=q value=v>");
        FormElement f = form(doc.getElementById("f"));
        Element q = doc.getElementById("q");

        f.addElement(q);
        f.addElement(q);
        FormHarness.equal("f", ownerId(q), "addElement associates the control");
        FormHarness.equal("q=v", dataString(f), "the added control is submitted, once");
        FormHarness.equal(1, f.elements().size(), "duplicate addElement does not duplicate the control");

        q.remove();
        FormHarness.equal("", dataString(f), "removing the added control drops it");
    }

    @FormHarness.Case(group = "edits")
    public static void changingFormAttributeUpdatesOwner() {
        Document doc = Jsoup.parse("<form id=one></form><form id=two></form><input id=q name=q value=v>");
        Element q = doc.getElementById("q");
        FormElement one = form(doc.getElementById("one"));
        FormElement two = form(doc.getElementById("two"));

        FormHarness.equal(null, q.formOwner(), "no owner initially");
        q.attr("form", "one");
        FormHarness.equal("one", ownerId(q), "form attribute adds the association");
        FormHarness.equal("q=v", dataString(one), "target form now submits it");
        q.attr("form", "two");
        FormHarness.equal("two", ownerId(q), "changing the attribute moves the association");
        FormHarness.equal("", dataString(one), "old target drops it");
        FormHarness.equal("q=v", dataString(two), "new target gains it");
        q.removeAttr("form");
        FormHarness.equal(null, q.formOwner(), "removing the attribute removes the explicit association");
    }

    @FormHarness.Case(group = "ownership")
    public static void elementsListIsIndependentOfOwnership() {
        Document doc = Jsoup.parse(TableFormHtml, "http://localhost/query");
        FormElement f = form(doc.getElementById("f"));
        Element a = doc.getElementById("a");
        Element b = doc.getElementById("b");

        Elements list = f.elements();
        list.remove(0); // list mutators must not detach controls or change ownership
        list.clear();
        FormHarness.equal(f, a.formOwner(), "clearing the returned list does not change control a's owner");
        FormHarness.equal(f, b.formOwner(), "clearing the returned list does not change control b's owner");
        FormHarness.equal("a,b", ids(f), "the form still reports both controls");
        FormHarness.equal("a=1&b=2", dataString(f), "form data is still complete");

        Elements second = f.elements();
        FormHarness.expect(second != list, "each call returns a new list");
        FormHarness.expect(second.get(0) == a && second.get(1) == b, "the list contains the same control nodes");
    }

    @FormHarness.Case(group = "edits")
    public static void insertingSiblingsAroundLinkedControlsKeepsTheLink() {
        Document doc = Jsoup.parse(TableFormHtml, "http://localhost/query");
        FormElement f = form(doc.getElementById("f"));
        Element a = doc.getElementById("a");
        Element b = doc.getElementById("b");

        a.before("<span id=before>before</span>");
        a.after("<span id=after>after</span>");
        b.before(new Element("em").attr("id", "beforeB"));
        FormHarness.equal(f, a.formOwner(), "inserting a sibling before/after does not break the parse link");
        FormHarness.equal(f, b.formOwner(), "inserting a sibling before a foster-parented control keeps its link");
        FormHarness.equal("a,b", ids(f), "unrelated inserted elements are not form controls");

        Element replacement = new Element("input").attr("id", "r").attr("name", "r").attr("value", "9");
        a.replaceWith(replacement);
        FormHarness.equal(null, a.formOwner(), "a replaced control loses its parse link");
        FormHarness.equal("b=2", dataString(f), "the replacement is not associated with the old form");
        FormHarness.equal("b", ids(f), "the replaced control is dropped from the list");
    }

    @FormHarness.Case(group = "edits")
    public static void duplicateIdReorderChangesSubmittedFields() throws IOException {
        // Two elements share the id "target": the form is first, then a div. A third input's owner depends on tree
        // order, so reordering them must change the fields the receiving end gets.
        String html = "<form id=target><input id=first name=first value=one></form>" +
            "<div id=target></div>" +
            "<input id=sub name=sub value=payload form=target>";
        Document doc = Jsoup.parse(html, "http://localhost/query");
        FormElement target = form(doc.getElementById("first").formOwner());
        Element sub = doc.getElementById("sub");

        FormHarness.equal("target", ownerId(sub), "form is the first matching id, so sub is associated");
        FormHarness.equal("first=one&sub=payload", dataString(target), "formData has both fields");
        FormHarness.equal("first=one&sub=payload", queryString(target), "submit payload has both fields");

        try (TestHttp server = TestHttp.start()) {
            if (server != null) {
                String base = server.url();
                target.attr("action", base + "/submit");
            }
            String received = receive(target, server);
            FormHarness.equal("first=one&sub=payload", received, "receiving end got both fields");

            // Reorder: move the div before the form. Now the div is the first element with the id, so sub has no owner.
            Element div = doc.selectFirst("div#target");
            doc.body().insertChildren(0, div);
            FormHarness.equal(null, sub.formOwner(), "after reorder, a non-form is the first match so sub loses its owner");
            FormHarness.equal("first=one", dataString(target), "formData now has only the form's own control");
            String receivedAfter = receive(target, server);
            FormHarness.equal("first=one", receivedAfter, "receiving end gets only the remaining field");

            // Reorder back: the form becomes the first match again, and sub is included again (recomputed, not revived)
            doc.body().insertChildren(0, target);
            FormHarness.equal("target", ownerId(sub), "moving the form back re-resolves the association");
            String receivedAgain = receive(target, server);
            FormHarness.equal("first=one&sub=payload", receivedAgain, "receiving end gets both fields again");
        }
    }

    /**
     Captures the fields the receiving end gets for a submission. When a local HTTP server can be bound, this executes
     the request for real and returns what the server received. If the environment blocks local port binding, it uses
     the connection's submitted payload (the same data that would be sent), so the field assertions still run.
     */
    private static String receive(FormElement form, TestHttp server) throws IOException {
        if (server != null) return server.receive(form);
        return join(form.submit().request().data());
    }

    // ---------------------------------------------------------------- copies

    @FormHarness.Case(group = "copies")
    public static void documentCloneMapsParseLinksToCopies() {
        Document original = Jsoup.parse(TableFormHtml, "http://localhost/query");
        Document copy = original.clone();

        FormElement copyForm = form(copy.getElementById("f"));
        Element copyA = copy.getElementById("a");
        Element copyB = copy.getElementById("b");

        FormHarness.expect(copyForm != original.getElementById("f"), "the copy has its own form element");
        FormHarness.equal(copyForm, copyA.formOwner(), "copied hidden input is owned by the copied form");
        FormHarness.equal(copyForm, copyB.formOwner(), "copied foster-parented input is owned by the copied form");
        FormHarness.equal("a,b", ids(copyForm), "copied form lists both copied controls");
        FormHarness.equal(0, originalControls(copyForm, original), "the copied form has no original-document controls");
        FormHarness.equal("a=1&b=2", dataString(copyForm), "copied form data is usable");

        // Mutating the copy must not affect the original
        copyA.attr("value", "100");
        copyB.remove();
        FormHarness.equal("1", original.getElementById("a").attr("value"), "original value is unchanged");
        FormHarness.equal(2, form(original.getElementById("f")).elements().size(), "original still has both controls");
        FormHarness.equal("a=1&b=2", dataString(form(original.getElementById("f"))), "original data unchanged");

        // And the copy reflects its own edits
        FormHarness.equal("a=100", dataString(copyForm), "copy reflects its own value change and removal");
    }

    @FormHarness.Case(group = "copies")
    public static void cloneWithoutCounterpartDropsTheLink() {
        Document original = Jsoup.parse(
            "<form id=f><input id=a name=a value=1></form><input id=b name=b value=2 form=f>");
        Element a = original.getElementById("a");
        Element b = original.getElementById("b");

        // Clone just the form element: the explicit-id control b is not part of that subtree
        FormElement formClone = form(original.getElementById("f").clone());
        FormHarness.equal(1, formClone.elements().size(), "only the cloned descendant control remains");
        FormHarness.equal("a", ids(formClone), "the original b is not pulled into the clone");
        FormHarness.equal(0, originalControls(formClone, original), "no original-document controls leak into the clone");

        // The originals still reference each other correctly
        FormHarness.equal("f", ownerId(a), "original control a keeps its owner");
        FormHarness.equal("f", ownerId(b), "original control b keeps its owner");
        FormHarness.equal(2, form(original.getElementById("f")).elements().size(), "original form unchanged");
    }

    @FormHarness.Case(group = "copies")
    public static void partialCloneKeepsLinksWithinTheCopiedSubtree() {
        Document original = Jsoup.parse("<div id=host><form id=f><input id=a name=a value=1></form></div>" +
            "<input id=b name=b value=2 form=f>");
        Element host = original.getElementById("host");
        Element hostClone = host.clone();
        FormElement formClone = form(hostClone.getElementById("f"));

        FormHarness.equal(formClone, hostClone.getElementById("a").formOwner(), "descendant link is mapped to the clone");
        FormHarness.equal("a=1", dataString(formClone), "partial clone form data comes from the copy");
        FormHarness.equal(0, originalControls(formClone, original), "the original b is not pulled into the partial clone");

        // The original tree is unaffected
        FormHarness.equal(2, form(original.getElementById("f")).elements().size(), "original form still has both controls");
    }

    @FormHarness.Case(group = "copies")
    public static void shallowCloneDoesNotCarryControls() {
        Document original = Jsoup.parse("<form id=f><input id=a name=a value=1></form><input id=b name=b value=2 form=f>");
        FormElement formClone = form(original.getElementById("f").shallowClone());
        FormHarness.equal(0, formClone.elements().size(), "shallow clone has no controls");
        FormHarness.equal("", dataString(formClone), "shallow clone submits nothing");
        FormHarness.equal(2, form(original.getElementById("f")).elements().size(), "original is unchanged");
    }

    @FormHarness.Case(group = "copies")
    public static void copyingDetachedControlsDoesNotResurrectTheLink() {
        Document original = Jsoup.parse(TableFormHtml, "http://localhost/query");
        FormElement f = form(original.getElementById("f"));
        Element b = original.getElementById("b");

        b.remove(); // invalidates b's parse link
        FormHarness.expect(!f.elements().contains(b), "a detached control is no longer in the original form");
        FormHarness.equal(null, b.formOwner(), "the detached control has no owner");
    }

    // ---------------------------------------------------------------- local HTTP server for the reordering test

    /** Minimal local HTTP server; captures the query string or POST body of the next request. */
    private static final class TestHttp implements AutoCloseable {
        private final HttpServer server;
        private final List<String> received = new ArrayList<>();

        private TestHttp(HttpServer server) {
            this.server = server;
        }

        /** Starts a local server, or returns null if this environment does not permit binding a local port. */
        static TestHttp start() throws IOException {
            HttpServer server;
            try {
                server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
            } catch (IOException | SecurityException e) {
                return null; // local binding not available here; the caller falls back to the request payload
            }
            TestHttp testHttp = new TestHttp(server);
            server.createContext("/", exchange -> {
                String body;
                try (InputStream in = exchange.getRequestBody()) {
                    body = new String(readAll(in), StandardCharsets.UTF_8);
                }
                String query = exchange.getRequestURI().getRawQuery();
                String data = query != null && !query.isEmpty() ? query : body;
                testHttp.received.add(data);
                byte[] response = "ok".getBytes(StandardCharsets.UTF_8);
                exchange.getResponseHeaders().add("Content-Type", "text/plain");
                exchange.sendResponseHeaders(200, response.length);
                try (OutputStream out = exchange.getResponseBody()) {
                    out.write(response);
                }
            });
            server.start();
            return testHttp;
        }

        String url() {
            return "http://127.0.0.1:" + server.getAddress().getPort();
        }

        String receive(FormElement form) throws IOException {
            int before = received.size();
            form.submit().ignoreHttpErrors(true).execute();
            FormHarness.expect(received.size() > before, "the local server received the request");
            return received.get(received.size() - 1);
        }

        @Override
        public void close() {
            server.stop(0);
        }

        private static byte[] readAll(InputStream in) throws IOException {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            byte[] buffer = new byte[1024];
            int read;
            while ((read = in.read(buffer)) != -1) {
                out.write(buffer, 0, read);
            }
            return out.toByteArray();
        }
    }
}
