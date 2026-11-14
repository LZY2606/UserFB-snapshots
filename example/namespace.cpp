#include "html.hpp"
#include <iostream>
#include <map>
#include <cassert>

// Collecting fields from a mixed HTML/SVG/MathML product page using
// namespace-qualified selectors.
int main() {
	const char* page = R"(<main>
	<h1 class="name">Fancy Kite</h1>
	<a class="buy" href="/cart/add/42">Add to cart</a>
	<svg viewBox="0 0 24 24" width="24" height="24">
		<title><b>Kite icon</b></title>
		<a xlink:href="/help-icon"><circle cx="12" cy="12" r="10"/></a>
		<foreignObject><textarea>leave a note</textarea></foreignObject>
	</svg>
	<math>
		<mi><b>x</b></mi><mo>+</mo><mn>1</mn>
		<annotation-xml encoding="text/html"><p>LaTeX fallback</p></annotation-xml>
	</math>
</main>)";

	// prefix -> URI bindings are captured when the selector is compiled
	std::map<std::string, std::string> ns = {
		{"", html::ns_uri::html},        // default element namespace
		{"svg", html::ns_uri::svg},
		{"m", html::ns_uri::mathml},
		{"xlink", html::ns_uri::xlink}
	};

	html::parser parser;
	auto doc = parser.parse(page);

	// Unqualified type selectors are constrained by the default namespace,
	// so "a" only matches the HTML link, not the SVG one.
	auto html_links = doc->select(html::selector("a", ns));
	assert(html_links.size() == 1);
	std::cout << "HTML link: " << html_links[0]->get_attr("href") << std::endl;

	// Qualified type selector for the SVG link; its href lives in the
	// XLink namespace after the standard foreign attribute adjustment.
	auto svg_links = doc->select(html::selector("svg|a", ns));
	assert(svg_links.size() == 1);
	std::cout << "SVG link: " << svg_links[0]->get_attr_ns(html::ns_uri::xlink, "href") << std::endl;

	// *|a matches links in any namespace.
	assert(doc->select(html::selector("*|a", ns)).size() == 2);

	// <b> inside the SVG <title> is real HTML markup (HTML integration
	// point), not rawtext; the MathML <mi> text integration point works
	// the same way.
	auto bold = doc->select(html::selector("b", ns));
	assert(bold.size() == 2);
	std::cout << "Icon caption: " << bold[0]->to_text() << std::endl;
	std::cout << "Formula term: " << bold[1]->to_text() << std::endl;

	// Qualified attribute selector; the second '|' in |= is the
	// dash-match operator, not a namespace separator.
	auto icon = doc->select(html::selector("svg|a[xlink|href|='/help']", ns));
	assert(icon.size() == 1);

	// Adjusted SVG attribute names keep their canonical case.
	auto svg = doc->select(html::selector("svg|svg", ns));
	assert(svg.size() == 1 && svg[0]->get_attr("viewBox") == "0 0 24 24");

	// The annotation-xml encoding switch and the foreignObject integration
	// point both expose regular HTML fields.
	auto note = doc->select(html::selector("svg|foreignObject textarea", ns));
	assert(note.size() == 1 && note[0]->to_text() == "leave a note");
	auto fallback = doc->select(html::selector("m|annotation-xml[encoding='text/html'] p", ns));
	assert(fallback.size() == 1 && fallback[0]->to_text() == "LaTeX fallback");

	std::cout << "All namespace fields collected." << std::endl;
	return 0;
}
