#include "html.hpp"
#include "task_support/check.h"

#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Reference aliases: safe across translation units, unlike string copies.
const std::string& HTML_NS = html::ns_uri::html;
const std::string& SVG_NS = html::ns_uri::svg;
const std::string& MATHML_NS = html::ns_uri::mathml;
const std::string& XLINK_NS = html::ns_uri::xlink;
const std::string& XML_NS = html::ns_uri::xml;
const std::string& XMLNS_NS = html::ns_uri::xmlns;

std::map<std::string, std::string> base_bindings() {
	return {{"svg", SVG_NS}, {"m", MATHML_NS}, {"x", XLINK_NS}};
}

// ---------------------------------------------------------------- TREE

int tree_integration_points() {
	html::parser p;
	auto doc = p.parse("<svg><title><b>caption</b></title><g><![CDATA[price<net>]]></g></svg>");
	auto svg = doc->select("svg");
	TASK_CHECK(svg.size() == 1);
	TASK_CHECK(svg[0]->namespace_uri() == SVG_NS);
	// <b> inside SVG <title> is an HTML field, not rawtext
	auto b = doc->select("b");
	TASK_CHECK(b.size() == 1);
	TASK_CHECK(b[0]->namespace_uri() == HTML_NS);
	TASK_CHECK(b[0]->to_text() == "caption");
	auto title = doc->select("title");
	TASK_CHECK(title.size() == 1);
	TASK_CHECK(title[0]->namespace_uri() == SVG_NS);
	TASK_CHECK(title[0]->at(0)->tag_name == "b");
	auto g = doc->select("g");
	TASK_CHECK(g.size() == 1);
	TASK_CHECK(g[0]->namespace_uri() == SVG_NS);
	TASK_CHECK(g[0]->at(0)->type_node == html::node_t::text);
	TASK_CHECK(g[0]->at(0)->content == "price<net>");

	// foreignObject keeps HTML text element rules for textarea
	auto d2 = p.parse("<svg><foreignObject><textarea>a<b</textarea></foreignObject></svg>");
	auto fo = d2->select("foreignObject");
	TASK_CHECK(fo.size() == 1);
	TASK_CHECK(fo[0]->namespace_uri() == SVG_NS);
	auto ta = d2->select("textarea");
	TASK_CHECK(ta.size() == 1);
	TASK_CHECK(ta[0]->namespace_uri() == HTML_NS);
	TASK_CHECK(ta[0]->to_text() == "a<b");

	// MathML text integration points
	auto d3 = p.parse("<math><mi><b>v</b></mi><mtext><span>s</span></mtext></math>");
	auto mi = d3->select("mi");
	TASK_CHECK(mi.size() == 1 && mi[0]->namespace_uri() == MATHML_NS);
	auto b3 = d3->select("b");
	TASK_CHECK(b3.size() == 1 && b3[0]->namespace_uri() == HTML_NS);
	TASK_CHECK(b3[0]->get_parent() == mi[0]);
	auto span = d3->select("span");
	TASK_CHECK(span.size() == 1 && span[0]->namespace_uri() == HTML_NS);

	// annotation-xml encoding switch and the annotation-xml/svg exception
	auto d4 = p.parse("<math><annotation-xml encoding='text/html'><p>h</p></annotation-xml></math>");
	auto ax = d4->select("annotation-xml");
	TASK_CHECK(ax.size() == 1 && ax[0]->namespace_uri() == MATHML_NS);
	auto p4 = d4->select("p");
	TASK_CHECK(p4.size() == 1 && p4[0]->namespace_uri() == HTML_NS);
	TASK_CHECK(p4[0]->get_parent() == ax[0]);
	auto d5 = p.parse("<math><annotation-xml><svg><circle/></svg></annotation-xml></math>");
	auto svg5 = d5->select("svg");
	TASK_CHECK(svg5.size() == 1 && svg5[0]->namespace_uri() == SVG_NS);
	auto circle = d5->select("circle");
	TASK_CHECK(circle.size() == 1 && circle[0]->namespace_uri() == SVG_NS);
	TASK_CHECK(circle[0]->get_parent() == svg5[0]);

	// mglyph/malignmark stay MathML even inside a text integration point
	auto d6 = p.parse("<math><mi><mglyph/></mi></math>");
	auto mglyph = d6->select("mglyph");
	TASK_CHECK(mglyph.size() == 1 && mglyph[0]->namespace_uri() == MATHML_NS);
	return 0;
}

int tree_cdata_state() {
	html::parser p;
	// CDATA is text in foreign content, a bogus comment back in HTML
	auto doc = p.parse("<svg><g><![CDATA[a<b&c]]></g></svg><div><![CDATA[x]]></div>");
	auto g = doc->select("g");
	TASK_CHECK(g.size() == 1);
	TASK_CHECK(g[0]->at(0)->type_node == html::node_t::text);
	TASK_CHECK(g[0]->at(0)->content == "a<b&c");
	auto div = doc->select("div");
	TASK_CHECK(div.size() == 1);
	TASK_CHECK(div[0]->at(0)->type_node == html::node_t::comment);
	TASK_CHECK(div[0]->at(0)->content == "[CDATA[x]]");

	// reusing the same parser must not leak namespace state
	auto d2 = p.parse("<div><![CDATA[y]]></div>");
	auto div2 = d2->select("div");
	TASK_CHECK(div2.size() == 1);
	TASK_CHECK(div2[0]->at(0)->type_node == html::node_t::comment);
	auto d3 = p.parse("<title>plain</title>");
	auto t3 = d3->select("title");
	TASK_CHECK(t3.size() == 1 && t3[0]->namespace_uri() == HTML_NS);
	TASK_CHECK(t3[0]->to_text() == "plain");

	// string and istream entries agree
	const std::string src = "<svg><g><![CDATA[a<b]]></g></svg>";
	std::istringstream in(src);
	auto from_string = p.parse(src);
	auto from_stream = p.parse(in);
	TASK_CHECK(from_stream->to_raw_html() == from_string->to_raw_html());
	auto gs = from_stream->select("g");
	TASK_CHECK(gs.size() == 1 && gs[0]->at(0)->content == "a<b");
	return 0;
}

int tree_foreign_attributes() {
	html::parser p;
	auto doc = p.parse("<svg viewBox='0 0 10 10' xlink:href='#i' xml:space='preserve' "
		"xmlns='http://www.w3.org/2000/svg'><rect/><foreignobject></foreignobject></svg>");
	auto svg = doc->select("svg");
	TASK_CHECK(svg.size() == 1);
	TASK_CHECK(svg[0]->namespace_uri() == SVG_NS);
	// SVG attribute case adjustment, qualified name interface preserved
	TASK_CHECK(svg[0]->has_attr("viewBox"));
	TASK_CHECK(svg[0]->get_attr("viewBox") == "0 0 10 10");
	TASK_CHECK(!svg[0]->has_attr("viewbox"));
	// foreign attribute namespace adjustment
	TASK_CHECK(svg[0]->has_attr("xlink:href"));
	TASK_CHECK(svg[0]->has_attr_ns(XLINK_NS, "href"));
	TASK_CHECK(svg[0]->get_attr_ns(XLINK_NS, "href") == "#i");
	TASK_CHECK(svg[0]->has_attr_ns(XML_NS, "space"));
	TASK_CHECK(svg[0]->get_attr_ns(XML_NS, "space") == "preserve");
	TASK_CHECK(svg[0]->has_attr_ns(XMLNS_NS, "xmlns"));
	// plain attributes keep the empty namespace
	TASK_CHECK(svg[0]->has_attr_ns("", "viewBox"));
	TASK_CHECK(!svg[0]->has_attr_ns("", "href"));
	TASK_CHECK(!svg[0]->has_attr_ns(XLINK_NS, "missing"));
	TASK_CHECK(svg[0]->get_attr_ns(XLINK_NS, "missing").empty());
	auto rect = doc->select("rect");
	TASK_CHECK(rect.size() == 1 && rect[0]->namespace_uri() == SVG_NS);
	// SVG tag name case adjustment
	auto fo = doc->select("foreignObject");
	TASK_CHECK(fo.size() == 1 && fo[0]->namespace_uri() == SVG_NS);

	// MathML definitionURL adjustment
	auto d2 = p.parse("<math definitionurl='top'><ms definitionurl='fn'>x</ms></math>");
	auto math = d2->select("math");
	TASK_CHECK(math.size() == 1 && math[0]->namespace_uri() == MATHML_NS);
	TASK_CHECK(math[0]->has_attr("definitionURL"));
	auto ms = d2->select("ms");
	TASK_CHECK(ms.size() == 1);
	TASK_CHECK(ms[0]->has_attr("definitionURL"));
	TASK_CHECK(ms[0]->get_attr("definitionURL") == "fn");
	TASK_CHECK(!ms[0]->has_attr("definitionurl"));
	return 0;
}

int tree_breakout_and_endtags() {
	html::parser p;
	// an HTML start tag in foreign content pops out and is reprocessed
	auto d1 = p.parse("<svg><p>out</p></svg>");
	auto svg1 = d1->select("svg");
	TASK_CHECK(svg1.size() == 1 && svg1[0]->size() == 0);
	auto p1 = d1->select("p");
	TASK_CHECK(p1.size() == 1 && p1[0]->namespace_uri() == HTML_NS);
	TASK_CHECK(p1[0]->get_parent()->type_node == html::node_t::none);
	TASK_CHECK(p1[0]->to_text() == "out");

	auto d2 = p.parse("<svg><g><div>x</div></g></svg>");
	auto g2 = d2->select("g");
	TASK_CHECK(g2.size() == 1 && g2[0]->size() == 0);
	auto div2 = d2->select("div");
	TASK_CHECK(div2.size() == 1 && div2[0]->namespace_uri() == HTML_NS);
	TASK_CHECK(div2[0]->get_parent()->type_node == html::node_t::none);

	// foreign end tags fall back through the open element stack
	auto d3 = p.parse("<svg><g><circle/></g></svg><p>after</p>");
	auto circle = d3->select("circle");
	TASK_CHECK(circle.size() == 1 && circle[0]->namespace_uri() == SVG_NS);
	TASK_CHECK(circle[0]->get_parent()->tag_name == "g");
	TASK_CHECK(circle[0]->get_parent()->get_parent()->tag_name == "svg");
	auto p3 = d3->select("p");
	TASK_CHECK(p3.size() == 1 && p3[0]->namespace_uri() == HTML_NS);

	// case-adjusted foreign names still match their end tags
	auto d4 = p.parse("<svg><foreignObject></foreignObject><circle/></svg>");
	auto fo = d4->select("foreignObject");
	TASK_CHECK(fo.size() == 1 && fo[0]->size() == 0);
	auto c4 = d4->select("circle");
	TASK_CHECK(c4.size() == 1);
	TASK_CHECK(c4[0]->get_parent()->tag_name == "svg");
	return 0;
}

int tree_copy_append_attr() {
	html::parser p;
	auto doc = p.parse("<svg viewBox='0 0 1 1' xlink:href='#l'><g><![CDATA[a<b]]></g></svg>");
	auto svg = doc->select("svg");
	TASK_CHECK(svg.size() == 1);

	// copy construction keeps namespace identity
	html::node copied(*svg[0]);
	TASK_CHECK(copied.namespace_uri() == SVG_NS);
	TASK_CHECK(copied.get_attr("viewBox") == "0 0 1 1");
	TASK_CHECK(copied.has_attr_ns(XLINK_NS, "href"));
	auto cg = copied.select("g");
	TASK_CHECK(cg.size() == 1 && cg[0]->namespace_uri() == SVG_NS);
	TASK_CHECK(cg[0]->at(0)->content == "a<b");
	TASK_CHECK(cg[0]->get_parent() == &copied);

	// move construction keeps it too
	html::node moved(std::move(copied));
	TASK_CHECK(moved.namespace_uri() == SVG_NS);
	TASK_CHECK(moved.select("g").size() == 1);
	TASK_CHECK(moved.has_attr_ns(XLINK_NS, "href"));

	// append re-parents but preserves subtree identity
	html::node host = html::utils::make_node(html::node_t::tag, "div");
	TASK_CHECK(host.namespace_uri() == HTML_NS);
	host.append(*moved.select("g")[0]);
	auto hg = host.select("g");
	TASK_CHECK(hg.size() == 1);
	TASK_CHECK(hg[0]->get_parent() == &host);
	TASK_CHECK(hg[0]->namespace_uri() == SVG_NS);
	TASK_CHECK(hg[0]->at(0)->content == "a<b");

	// set_attr/del_attr on foreign nodes stay in sync with ns queries
	html::node* g = hg[0];
	g->set_attr("xlink:href", "#x");
	TASK_CHECK(g->has_attr("xlink:href"));
	TASK_CHECK(g->has_attr_ns(XLINK_NS, "href"));
	TASK_CHECK(g->get_attr_ns(XLINK_NS, "href") == "#x");
	g->set_attr("viewbox", "1 2 3");
	TASK_CHECK(g->has_attr("viewBox") && g->get_attr("viewBox") == "1 2 3");
	TASK_CHECK(!g->has_attr("viewbox"));
	g->del_attr("xlink:href");
	TASK_CHECK(!g->has_attr("xlink:href"));
	TASK_CHECK(!g->has_attr_ns(XLINK_NS, "href"));
	g->del_attr("viewBox");
	TASK_CHECK(!g->has_attr("viewBox"));
	g->set_attr({{"xml:space", "preserve"}, {"id", "g1"}});
	TASK_CHECK(g->has_attr_ns(XML_NS, "space"));
	TASK_CHECK(g->get_attr("id") == "g1");
	TASK_CHECK(!g->has_attr("viewBox"));
	return 0;
}

// ---------------------------------------------------------------- SELECT

int select_qualified_types() {
	html::parser p;
	auto doc = p.parse("<div><a class='hot' href='h'>html</a><svg>"
		"<a class='hot' xlink:href='urn-x'>svg</a><circle/><foreignObject/></svg></div>");
	auto bindings = base_bindings();

	html::selector svg_a("svg|a", bindings);
	TASK_CHECK(static_cast<bool>(svg_a));
	auto r = doc->select(svg_a);
	TASK_CHECK(r.size() == 1 && r[0]->namespace_uri() == SVG_NS);

	TASK_CHECK(doc->select(html::selector("*|a", bindings)).size() == 2);

	// compiled before the default binding exists: stays unconstrained
	html::selector any_a("a", bindings);
	TASK_CHECK(doc->select(any_a).size() == 2);

	// mutating the caller's table must not recompile existing selectors
	bindings[""] = HTML_NS;
	TASK_CHECK(doc->select(any_a).size() == 2);
	html::selector html_a("a", bindings);
	TASK_CHECK(doc->select(html_a).size() == 1);
	TASK_CHECK(doc->select(html_a)[0]->namespace_uri() == HTML_NS);

	// explicit no-namespace matches nothing here (all elements have one)
	TASK_CHECK(doc->select(html::selector("|a", bindings)).size() == 0);

	// prefixed universal selector
	TASK_CHECK(doc->select(html::selector("svg|*", bindings)).size() == 4);

	// default namespace also constrains compounds without a type selector
	TASK_CHECK(doc->select(html::selector(".hot", bindings)).size() == 1);
	TASK_CHECK(doc->select(html::selector(".hot", base_bindings())).size() == 2);

	// HTML names stay case-insensitive, foreign names compare exactly
	TASK_CHECK(doc->select(html::selector("A", bindings)).size() == 1);
	TASK_CHECK(doc->select(html::selector("svg|foreignObject", bindings)).size() == 1);
	TASK_CHECK(doc->select(html::selector("svg|FOREIGNOBJECT", bindings)).size() == 0);
	return 0;
}

int select_qualified_attrs() {
	html::parser p;
	auto doc = p.parse("<svg><a xlink:href='urn-x' href='plain'>t</a></svg><a href='x'>h</a>");
	auto bindings = base_bindings();

	TASK_CHECK(doc->select(html::selector("[x|href]", bindings)).size() == 1);
	TASK_CHECK(doc->select(html::selector("[*|href]", bindings)).size() == 2);
	// unprefixed attribute names only see no-namespace attributes
	TASK_CHECK(doc->select(html::selector("[href]", bindings)).size() == 2);
	TASK_CHECK(doc->select(html::selector("[|href]", bindings)).size() == 2);

	// the '|' of the |= operator is not a namespace separator
	TASK_CHECK(doc->select(html::selector("[x|href|='urn']", bindings)).size() == 1);
	TASK_CHECK(doc->select(html::selector("[x|href='urn-x']", bindings)).size() == 1);
	TASK_CHECK(doc->select(html::selector("[x|href='urn']", bindings)).size() == 0);

	// namespace conditions compose with ordinary ones
	TASK_CHECK(doc->select(html::selector("svg|a[x|href='urn-x']", bindings)).size() == 1);
	TASK_CHECK(doc->select(html::selector("a[href='plain']", bindings)).size() == 1);
	return 0;
}

int select_invalid_callbacks() {
	auto bindings = base_bindings();
	html::selector undeclared("zzz|a", bindings);
	TASK_CHECK(!undeclared);
	html::selector incomplete("svg|", bindings);
	TASK_CHECK(!incomplete);
	html::selector bad_attr("[x|]", bindings);
	TASK_CHECK(!bad_attr);
	html::selector double_sep("svg|a|b", bindings);
	TASK_CHECK(!double_sep);

	html::parser p;
	auto doc = p.parse("<div><a>x</a></div>");
	TASK_CHECK(doc->select(undeclared).empty());
	TASK_CHECK(doc->select(incomplete).empty());

	// an invalid selector must not degrade into an all-node callback
	html::parser p2;
	unsigned calls = 0;
	p2.set_callback(undeclared, [&](html::node&) { calls++; });
	p2.set_callback(bad_attr, [&](html::node&) { calls++; });
	p2.parse("<div><a>x</a><p>y</p></div>");
	TASK_CHECK(calls == 0);

	// the unfiltered callback keeps its old behavior
	html::parser p3;
	unsigned all = 0;
	p3.set_callback([&](html::node&) { all++; });
	p3.parse("<div><a>x</a></div>");
	TASK_CHECK(all > 0);
	return 0;
}

int select_callback_consistency() {
	auto bindings = base_bindings();
	html::parser p;
	std::vector<html::node*> from_callback;
	bool ns_ready = true;
	p.set_callback(html::selector("svg|a", bindings), [&](html::node& n) {
		if(n.type_node == html::node_t::tag && n.type_tag == html::tag_t::open) {
			// identity must be resolved before the callback runs
			ns_ready = ns_ready && n.namespace_uri() == SVG_NS;
			from_callback.push_back(&n);
		}
	});
	auto doc = p.parse("<svg><a href='1'>x</a><g><a href='2'>y</a></g></svg><a href='3'>h</a>");
	TASK_CHECK(ns_ready);
	TASK_CHECK(from_callback.size() == 2);
	auto from_select = doc->select(html::selector("svg|a", bindings));
	TASK_CHECK(from_select.size() == 2);
	TASK_CHECK(from_callback == from_select);

	// namespace conditions combine with descendant/child relations
	TASK_CHECK(doc->select(html::selector("svg|g svg|a", bindings)).size() == 1);
	TASK_CHECK(doc->select(html::selector("svg|g>svg|a", bindings)).size() == 1);
	TASK_CHECK(doc->select(html::selector("svg|a[href='2']", bindings)).size() == 1);
	return 0;
}

// ---------------------------------------------------------------- ROUNDTRIP

int roundtrip_foreign_subtree() {
	html::parser p;
	auto doc = p.parse("<svg viewBox='0 0 10 10'><title><b>caption</b></title>"
		"<g><![CDATA[price<net> & more]]></g><circle/></svg>");
	auto svg1 = doc->select("svg");
	TASK_CHECK(svg1.size() == 1);
	std::string out = svg1[0]->to_raw_html();

	auto doc2 = p.parse(out);
	auto svg2 = doc2->select("svg");
	TASK_CHECK(svg2.size() == 1);
	TASK_CHECK(svg2[0]->namespace_uri() == SVG_NS);
	TASK_CHECK(svg2[0]->get_attr("viewBox") == "0 0 10 10");
	auto b2 = doc2->select("b");
	TASK_CHECK(b2.size() == 1 && b2[0]->namespace_uri() == HTML_NS);
	TASK_CHECK(b2[0]->to_text() == "caption");
	TASK_CHECK(b2[0]->get_parent()->tag_name == "title");
	TASK_CHECK(b2[0]->get_parent()->namespace_uri() == SVG_NS);
	auto g2 = doc2->select("g");
	TASK_CHECK(g2.size() == 1 && g2[0]->get_parent() == svg2[0]);
	TASK_CHECK(g2[0]->at(0)->type_node == html::node_t::text);
	TASK_CHECK(g2[0]->at(0)->content == "price<net> & more");
	auto c2 = doc2->select("circle");
	TASK_CHECK(c2.size() == 1 && c2[0]->namespace_uri() == SVG_NS);
	return 0;
}

int roundtrip_pretty_and_compat() {
	html::parser p;
	// pretty output of a foreign subtree reparses to the same fields
	auto doc = p.parse("<svg viewBox='0 0 10 10'><title><b>caption</b></title>"
		"<g><![CDATA[price<net>]]></g></svg>");
	std::string pretty = doc->select("svg")[0]->to_html();
	auto doc2 = p.parse(pretty);
	auto svg2 = doc2->select("svg");
	TASK_CHECK(svg2.size() == 1 && svg2[0]->namespace_uri() == SVG_NS);
	TASK_CHECK(svg2[0]->get_attr("viewBox") == "0 0 10 10");
	auto g2 = doc2->select("g");
	TASK_CHECK(g2.size() == 1 && g2[0]->at(0)->content == "price<net>");
	auto b2 = doc2->select("b");
	TASK_CHECK(b2.size() == 1 && b2[0]->to_text() == "caption");

	// plain HTML output is unchanged: no CDATA, same raw serialization
	auto h = p.parse("<div><p class='x'>hello</p><br/><textarea>a<b</textarea></div>");
	std::string raw = h->to_raw_html();
	TASK_CHECK(raw == "<div><p class=\"x\">hello</p><br /><textarea>a<b</textarea></div>");
	TASK_CHECK(raw.find("<![CDATA[") == std::string::npos);
	TASK_CHECK(h->to_html().find("<![CDATA[") == std::string::npos);
	TASK_CHECK(h->select("p[class='x']").size() == 1);
	TASK_CHECK(h->select("P").size() == 1);
	TASK_CHECK(h->select("p[CLASS='x']").size() == 1);
	return 0;
}

int roundtrip_mixed_mathml() {
	html::parser p;
	auto doc = p.parse("<math><mi><b>v</b></mi>"
		"<annotation-xml encoding='text/html'><p>h</p></annotation-xml></math>");
	std::string out = doc->to_raw_html();
	auto doc2 = p.parse(out);
	auto mi2 = doc2->select("mi");
	TASK_CHECK(mi2.size() == 1 && mi2[0]->namespace_uri() == MATHML_NS);
	auto b2 = doc2->select("b");
	TASK_CHECK(b2.size() == 1 && b2[0]->namespace_uri() == HTML_NS);
	TASK_CHECK(b2[0]->get_parent() == mi2[0]);
	auto ax2 = doc2->select("annotation-xml");
	TASK_CHECK(ax2.size() == 1 && ax2[0]->namespace_uri() == MATHML_NS);
	TASK_CHECK(ax2[0]->get_attr("encoding") == "text/html");
	auto p2 = doc2->select("p");
	TASK_CHECK(p2.size() == 1 && p2[0]->namespace_uri() == HTML_NS);
	TASK_CHECK(p2[0]->get_parent() == ax2[0]);
	return 0;
}

} // namespace

const task_case task_cases[] = {
	{"TREE", "integration_points", tree_integration_points},
	{"TREE", "cdata_state", tree_cdata_state},
	{"TREE", "foreign_attributes", tree_foreign_attributes},
	{"TREE", "breakout_and_endtags", tree_breakout_and_endtags},
	{"TREE", "copy_append_attr", tree_copy_append_attr},
	{"SELECT", "qualified_types", select_qualified_types},
	{"SELECT", "qualified_attrs", select_qualified_attrs},
	{"SELECT", "invalid_callbacks", select_invalid_callbacks},
	{"SELECT", "callback_consistency", select_callback_consistency},
	{"ROUNDTRIP", "foreign_subtree", roundtrip_foreign_subtree},
	{"ROUNDTRIP", "pretty_and_compat", roundtrip_pretty_and_compat},
	{"ROUNDTRIP", "mixed_mathml", roundtrip_mixed_mathml},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
