#include "../src/pugixml.hpp"
#include "cases.hpp"

#include <string>
#include <vector>

namespace
{
	typedef pugi::char_t char_t;

	std::basic_string<char_t> T(const char* text)
	{
	#ifdef PUGIXML_WCHAR_MODE
		return pugi::as_wide(text);
	#else
		return std::basic_string<char_t>(text);
	#endif
	}

	std::string N(const char_t* text)
	{
	#ifdef PUGIXML_WCHAR_MODE
		return pugi::as_utf8(text);
	#else
		return std::string(text);
	#endif
	}

	bool same(const char_t* lhs, const char* rhs)
	{
		return N(lhs) == rhs;
	}

	struct selection
	{
		std::vector<std::string> names;

		void add_node(const pugi::xpath_node& n)
		{
			if (n.is_namespace())
				names.push_back("ns:" + N(n.namespace_prefix()) + "=" + N(n.namespace_value()));
			else if (n.attribute())
				names.push_back("attr:" + N(n.attribute().name()) + "=" + N(n.attribute().value()));
			else
			{
				// include ancestors to distinguish equally named elements in different scopes
				std::string path = N(n.node().name());
				for (pugi::xml_node parent = n.node().parent(); parent && parent.type() == pugi::node_element; parent = parent.parent())
					path = N(parent.name()) + "/" + path;

				names.push_back("node:" + path);
			}
		}
	};

	selection collect(pugi::xpath_node_set set, bool sorted = true)
	{
		if (sorted) set.sort();

		selection result;

		for (size_t i = 0; i < set.size(); ++i)
			result.add_node(set[i]);

		return result;
	}

	// assert two selections are identical
	void equal_selection(const selection& actual, const char* expect, int line)
	{
		std::vector<std::string> wanted;
		const char* p = expect;

		while (*p)
		{
			const char* end = p;

			while (*end && *end != '|') ++end;

			wanted.push_back(std::string(p, end));

			p = *end ? end + 1 : end;
		}

		tcase::check(actual.names == wanted, "selection mismatch", line);
	}
}

// Prefix aliases: same URI under different prefixes matches, same prefix with a different URI does not
CASE(alias_names)
{
	pugi::xml_document doc;
	CHECK(doc.load_string(T("<r xmlns:a='urn:one' xmlns:b='urn:one' xmlns:c='urn:two'>"
		"<a:item a:k='7'/>"
		"<b:item b:k='8'/>"
		"<c:item c:k='9'/>"
		"<plain pl='1'/>"
		"</r>").c_str()));

	pugi::xpath_namespace_set ns;
	CHECK(ns.set(T("u").c_str(), T("urn:one").c_str()));

	pugi::xpath_query q(T("//u:item").c_str(), 0, &ns);
	selection got = collect(q.evaluate_node_set(doc));

	equal_selection(got, "node:r/a:item|node:r/b:item", __LINE__);

	// same prefix, different URI binding: u rebinding to urn:two matches only c:item
	pugi::xpath_namespace_set ns2;
	CHECK(ns2.set(T("u").c_str(), T("urn:two").c_str()));
	selection got2 = collect(pugi::xpath_query(T("//u:item").c_str(), 0, &ns2).evaluate_node_set(doc));
	equal_selection(got2, "node:r/c:item", __LINE__);

	// expanded URI matching must be independent of the document prefix
	equal_selection(collect(pugi::xpath_query(T("//u:item[@u:k='7']").c_str(), 0, &ns).evaluate_node_set(doc)),
		"node:r/a:item", __LINE__);

	// xml prefix keeps its standard binding
	pugi::xml_document lang;
	CHECK(lang.load_string(T("<r><n xml:lang='en'/><m/></r>").c_str()));
	CHECK(!got.names.empty());
	equal_selection(collect(pugi::xpath_query(T("//n[@xml:lang='en']").c_str(), 0, &ns).evaluate_node_set(lang)),
		"node:r/n", __LINE__);

	// invalid bindings are rejected and leave prior value intact
	pugi::xpath_namespace_set rejected;
	CHECK(rejected.set(T("p").c_str(), T("urn:keep").c_str()));
	CHECK(!rejected.set(T("").c_str(), T("urn:x").c_str()));
	CHECK(!rejected.set(T("p").c_str(), T("").c_str()));
	CHECK(!rejected.set(T("xmlns").c_str(), T("urn:x").c_str()));
	CHECK(!rejected.set(T("xml").c_str(), T("urn:wrong").c_str()));
	CHECK(rejected.set(T("xml").c_str(), T("http://www.w3.org/XML/1998/namespace").c_str()));
	CHECK(!rejected.set(T("q").c_str(), T("http://www.w3.org/2000/xmlns/").c_str()));
	CHECK(!rejected.set(T("q").c_str(), T("http://www.w3.org/XML/1998/namespace").c_str()));
	CHECK(rejected._find(T("p").c_str()) != 0 && same(rejected._find(T("p").c_str()), "urn:keep"));
}

// Ancestor declarations, same-element declarations, local shadowing and xmlns='' clearing
CASE(scope_shadow)
{
	pugi::xml_document doc;
	CHECK(doc.load_string(T("<root xmlns='urn:def' xmlns:s='urn:one'>"
		"<a><s:xa/></a>"
		"<b xmlns:s='urn:two'><s:xb/></b>"
		"<c xmlns=''><s:xc/></c>"
		"<d xmlns='urn:deep'><plain/></d>"
		"<e><s:xe/></e>"
		"<f xmlns=''><plain/></f>"
		"</root>").c_str()));

	pugi::xpath_namespace_set ns;
	CHECK(ns.set(T("u").c_str(), T("urn:one").c_str()));

	// prefix inherited from an ancestor declaration
	selection inherited = collect(pugi::xpath_query(T("//u:xa").c_str(), 0, &ns).evaluate_node_set(doc));
	equal_selection(inherited, "node:root/a/s:xa", __LINE__);

	// the declaration on c's own element is inherited from root, not shadowed by xmlns=''
	equal_selection(collect(pugi::xpath_query(T("//u:xc").c_str(), 0, &ns).evaluate_node_set(doc)),
		"node:root/c/s:xc", __LINE__);

	// local shadowing: b's declaration rebinds s to urn:two
	pugi::xpath_namespace_set two;
	CHECK(two.set(T("u").c_str(), T("urn:two").c_str()));
	equal_selection(collect(pugi::xpath_query(T("//u:xb").c_str(), 0, &two).evaluate_node_set(doc)),
		"node:root/b/s:xb", __LINE__);
	CHECK(pugi::xpath_query(T("//u:xb").c_str(), 0, &ns).evaluate_node_set(doc).empty());

	// unprefixed element tests only match elements with no namespace; the inherited default does not apply
	equal_selection(collect(pugi::xpath_query(T("//plain").c_str(), 0, &ns).evaluate_node_set(doc)),
		"node:root/f/plain", __LINE__);

	// d has a non-empty default namespace, so its plain child is namespaced; e has no default
	pugi::xml_document cleared;
	CHECK(cleared.load_string(T("<root xmlns='urn:def'><c xmlns=''><plain1/></c><d><plain2/></d></root>").c_str()));
	equal_selection(collect(pugi::xpath_query(T("//plain1").c_str(), 0, &ns).evaluate_node_set(cleared)),
		"node:root/c/plain1", __LINE__);
	CHECK(pugi::xpath_query(T("//plain2").c_str(), 0, &ns).evaluate_node_set(cleared).empty());

	// unprefixed attribute names never inherit the default namespace, while prefixed attributes resolve
	pugi::xml_document attrs;
	CHECK(attrs.load_string(T("<root xmlns='urn:def' xmlns:s='urn:one'><c xmlns='' plain='1' s:prefixed='2'/></root>").c_str()));
	equal_selection(collect(pugi::xpath_query(T("//c[@plain='1']").c_str(), 0, &ns).evaluate_node_set(attrs)),
		"node:root/c", __LINE__);
	equal_selection(collect(pugi::xpath_query(T("//c[@u:prefixed='2']").c_str(), 0, &ns).evaluate_node_set(attrs)),
		"node:root/c", __LINE__);
	CHECK(pugi::xpath_query(T("//c[@plain]").c_str(), 0, &ns).evaluate_node_set(attrs).size() == 1);

	// declaration on the same element as the test target
	pugi::xml_document same_element;
	CHECK(same_element.load_string(T("<r><n xmlns:p='urn:one'><p:item p:k='1'/></n></r>").c_str()));
	equal_selection(collect(pugi::xpath_query(T("//*[local-name()='item'][@u:k='1']").c_str(), 0, &ns).evaluate_node_set(same_element)),
		"node:r/n/p:item", __LINE__);

	// a prefixed name whose prefix has no declaration is not treated as no-namespace
	pugi::xml_document unresolved;
	CHECK(unresolved.load_string(T("<r><a:x/><x/><n a:k='v' k='w'/></r>").c_str()));
	CHECK(pugi::xpath_query(T("//x").c_str(), 0, &ns).evaluate_node_set(unresolved).size() == 1);
	CHECK(pugi::xpath_query(T("//n/@k").c_str(), 0, &ns).evaluate_node_set(unresolved).size() == 1);
}

// Attribute predicates: optimized and ordinary attribute lookup agree, including variables
CASE(attribute_predicate)
{
	pugi::xml_document doc;
	CHECK(doc.load_string(T("<r xmlns:a='urn:one' xmlns:b='urn:one'>"
		"<a:item a:code='x' id='first'/>"
		"<b:item b:code='y' id='second'/>"
		"<c:item xmlns:c='urn:one' c:code='x' id='third'/>"
		"<plain id='plain'/>"
		"</r>").c_str()));

	pugi::xpath_namespace_set ns;
	CHECK(ns.set(T("u").c_str(), T("urn:one").c_str()));

	// optimized @u:code = 'x' (literal)
	equal_selection(collect(pugi::xpath_query(T("//u:item[@u:code='x']").c_str(), 0, &ns).evaluate_node_set(doc)),
		"node:r/a:item|node:r/c:item", __LINE__);

	// variable-form attribute condition follows the same rule
	pugi::xpath_variable_set vars;
	CHECK(vars.set(T("value").c_str(), T("y").c_str()));
	equal_selection(collect(pugi::xpath_query(T("//u:item[@u:code=$value]").c_str(), &vars, &ns).evaluate_node_set(doc)),
		"node:r/b:item", __LINE__);

	// attribute selection alone and through a union gives the same expanded-name matches
	equal_selection(collect(pugi::xpath_query(T("//u:item/@u:code").c_str(), 0, &ns).evaluate_node_set(doc)),
		"attr:a:code=x|attr:b:code=y|attr:c:code=x", __LINE__);

	// expanded-name lookup is by URI/local name regardless of the document prefix
	equal_selection(collect(pugi::xpath_query(T("//u:item/@*[local-name()='code'][namespace-uri()='urn:one']").c_str(), 0, &ns).evaluate_node_set(doc)),
		"attr:a:code=x|attr:b:code=y|attr:c:code=x", __LINE__);

	// xmlns declarations must not appear as attributes nor match wildcard
	equal_selection(collect(pugi::xpath_query(T("//u:item/@*").c_str(), 0, &ns).evaluate_node_set(doc)),
		"attr:a:code=x|attr:id=first|attr:b:code=y|attr:id=second|attr:c:code=x|attr:id=third", __LINE__);

	// predicate with variable + position/union keeps document order
	equal_selection(collect(pugi::xpath_query(T("(//u:item[@u:code='x'] | //u:item[@u:code=$value])[1]").c_str(), &vars, &ns).evaluate_node_set(doc)),
		"node:r/a:item", __LINE__);
}

// Compiled binding lifetime: namespace set mutation/destruction must not affect compiled queries;
// one query is reusable across documents that use different prefixes
CASE(plan_lifetime)
{
	pugi::xpath_namespace_set ns;
	CHECK(ns.set(T("u").c_str(), T("urn:one").c_str()));

	pugi::xpath_query q(T("//u:item").c_str(), 0, &ns);

	// mutate and destroy the source set; the query must keep working
	CHECK(ns.set(T("u").c_str(), T("urn:other").c_str()));

	{
		pugi::xpath_namespace_set* temporary = new pugi::xpath_namespace_set();
		temporary->set(T("v").c_str(), T("urn:one").c_str());

		pugi::xpath_query retained(T("//v:item").c_str(), 0, temporary);
		delete temporary;

		pugi::xml_document dummy;
		CHECK(dummy.load_string(T("<r xmlns:w='urn:one'><w:item/></r>").c_str()));
		CHECK(retained.evaluate_node_set(dummy).size() == 1);
	}

	pugi::xml_document first;
	pugi::xml_document second;
	CHECK(first.load_string(T("<r xmlns:a='urn:one'><a:item/></r>").c_str()));
	CHECK(second.load_string(T("<r xmlns:b='urn:one'><b:item/></r>").c_str()));

	// the same compiled query against two trees with different prefixes
	CHECK(q.evaluate_node_set(first).size() == 1);
	CHECK(q.evaluate_node_set(second).size() == 1);
	CHECK(q.evaluate_node_set(first).size() == 1);

	// an unbound prefix is a compile-time error with a position
	bool failed = false;
	ptrdiff_t offset = 0;

	try
	{
		pugi::xpath_query bad(T("//missing:item").c_str(), 0, &ns);
	}
	catch (const pugi::xpath_exception& e)
	{
		failed = true;
		offset = e.result().offset;
	}

	CHECK(failed);
	CHECK(offset > 0);

	// no compile-time error for an expression that never traverses

	// changing the compiled set after query compilation changed nothing
	pugi::xml_document check;
	CHECK(check.load_string(T("<r xmlns:a='urn:one'><a:item/></r>").c_str()));
	CHECK(q.evaluate_node_set(check).size() == 1);
}

// Legacy mode retains literal QName semantics and the empty namespace axis
CASE(legacy_mode)
{
	pugi::xml_document doc;
	CHECK(doc.load_string(T("<r xmlns:a='urn:one' xmlns:b='urn:one'><a:item/><b:item/></r>").c_str()));

	// literal names: only the exact document prefix matches
	CHECK(doc.select_nodes(T("/r/a:item").c_str()).size() == 1);
	CHECK(doc.select_nodes(T("/r/b:item").c_str()).size() == 1);
	CHECK(doc.select_nodes(T("/r/c:item").c_str()).size() == 0);

	// legacy namespace axis is still empty
	CHECK(doc.select_nodes(T("/r/*/namespace::*").c_str()).size() == 0);
	CHECK(doc.select_nodes(T("/r/namespace::node()").c_str()).size() == 0);

	// namespace::p is also empty in legacy mode
	CHECK(doc.select_nodes(T("/r/*/namespace::a").c_str()).size() == 0);

	// literal attribute selection is unchanged
	CHECK(doc.select_nodes(T("/r/*[@a:item]").c_str()).size() == 0);
	CHECK(doc.select_nodes(T("/r/*[local-name()='item']").c_str()).size() == 2);

	// a default-namespace document still matches literal unprefixed names in legacy mode
	pugi::xml_document def;
	CHECK(def.load_string(T("<r xmlns='urn:def'><item/></r>").c_str()));
	CHECK(def.select_nodes(T("/r/item").c_str()).size() == 1);

	// namespace-aware mode must reject a qualified node test on the namespace axis
	pugi::xpath_namespace_set ns;
	CHECK(ns.set(T("u").c_str(), T("urn:one").c_str()));

	bool failed = false;

	try
	{
		pugi::xpath_query bad(T("namespace::u:item").c_str(), 0, &ns);
	}
	catch (const pugi::xpath_exception&)
	{
		failed = true;
	}

	CHECK(failed);

	// legacy mode still allows the same expression syntax (it just yields an empty set)
	CHECK(doc.select_nodes(T("/r/*/namespace::a:b").c_str()).size() == 0);
}

// Namespace axis: effective bindings, shadowing, default clearing, identity and ordering
CASE(namespace_nodes)
{
	pugi::xml_document doc;
	CHECK(doc.load_string(T("<root xmlns='urn:def' xmlns:p='urn:one' xmlns:q='urn:one'>"
		"<a attr='a'><p:x/></a>"
		"<b xmlns:p='urn:two'><p:y/></b>"
		"<c xmlns=''><plain/></c>"
		"</root>").c_str()));

	pugi::xpath_namespace_set ns;
	CHECK(ns.set(T("u").c_str(), T("urn:one").c_str()));

	pugi::xml_node a = doc.child(T("root")).child(T("a"));
	pugi::xml_node b = doc.child(T("root")).child(T("b"));
	pugi::xml_node c = doc.child(T("root")).child(T("c"));

	// effective bindings of a: default + p + q + xml, in prefix order (empty prefix first)
	{
		pugi::xpath_node_set s = pugi::xpath_query(T("namespace::*").c_str(), 0, &ns).evaluate_node_set(a);
		CHECK(s.size() == 4);
		CHECK(s[0].is_namespace());
		CHECK(same(s[0].namespace_prefix(), ""));
		CHECK(same(s[0].namespace_value(), "urn:def"));
		CHECK(same(s[1].namespace_prefix(), "p"));
		CHECK(same(s[1].namespace_value(), "urn:one"));
		CHECK(same(s[2].namespace_prefix(), "q"));
		CHECK(same(s[2].namespace_value(), "urn:one"));
		CHECK(same(s[3].namespace_prefix(), "xml"));
		CHECK(same(s[3].namespace_value(), "http://www.w3.org/XML/1998/namespace"));
	}

	// namespace::node() equals namespace::*
	{
		pugi::xpath_node_set all = pugi::xpath_query(T("namespace::*").c_str(), 0, &ns).evaluate_node_set(a);
		pugi::xpath_node_set node = pugi::xpath_query(T("namespace::node()").c_str(), 0, &ns).evaluate_node_set(a);
		CHECK(all.size() == node.size() && all.size() == 4);
	}

	// a single-prefix test selects the in-scope binding only
	{
		pugi::xpath_node_set s = pugi::xpath_query(T("namespace::p").c_str(), 0, &ns).evaluate_node_set(b);
		CHECK(s.size() == 1 && same(s[0].namespace_value(), "urn:two"));

		// b inherits q from root but has its own p declaration
		pugi::xpath_node_set qs = pugi::xpath_query(T("namespace::q").c_str(), 0, &ns).evaluate_node_set(b);
		CHECK(qs.size() == 1 && same(qs[0].namespace_value(), "urn:one"));
	}

	// xmlns='' clears the default binding: c has no default namespace node
	{
		pugi::xpath_node_set s = pugi::xpath_query(T("namespace::*").c_str(), 0, &ns).evaluate_node_set(c);
		CHECK(s.size() == 3); // p, q and xml remain; the empty default declaration produces no node
		CHECK(!same(s[0].namespace_prefix(), ""));
		CHECK(same(s[0].namespace_prefix(), "p") && same(s[0].namespace_value(), "urn:one"));
		CHECK(same(s[1].namespace_prefix(), "q") && same(s[1].namespace_value(), "urn:one"));
		CHECK(same(s[2].namespace_prefix(), "xml"));
		for (size_t i = 0; i < s.size(); ++i)
			CHECK(!same(s[i].namespace_prefix(), ""));
	}

	// names are prefixes; namespace-uri is empty; string is the value
	{
		pugi::xpath_node n = pugi::xpath_query(T("namespace::p").c_str(), 0, &ns).evaluate_node_set(a).first();
		CHECK(n.is_namespace());
		CHECK(n.node().empty());
		CHECK(n.attribute().empty());
		CHECK(n.parent() == a);
		CHECK(N(pugi::xpath_query(T("name(.)").c_str()).evaluate_string(n).c_str()) == "p");
		CHECK(N(pugi::xpath_query(T("local-name(.)").c_str()).evaluate_string(n).c_str()) == "p");
		CHECK(N(pugi::xpath_query(T("namespace-uri(.)").c_str()).evaluate_string(n).c_str()) == "");
		CHECK(N(pugi::xpath_query(T("string(.)").c_str()).evaluate_string(n).c_str()) == "urn:one");

		// parent axis and self::node()
		CHECK(pugi::xpath_query(T("parent::node()").c_str(), 0, &ns).evaluate_node(n).node() == a);
		pugi::xpath_node self = pugi::xpath_query(T("self::node()").c_str(), 0, &ns).evaluate_node(n);
		CHECK(self.is_namespace() && same(self.namespace_prefix(), "p"));
	}

	// inherited same declaration on two different owners => distinct identities
	{
		pugi::xpath_node_set s = pugi::xpath_query(T("//*[local-name()='a']/namespace::p | //*[local-name()='c']/namespace::p").c_str(), 0, &ns).evaluate_node_set(doc);
		CHECK(s.size() == 2);

		// union with itself on one element deduplicates
		pugi::xpath_node_set dup = pugi::xpath_query(T("//*[local-name()='a']/namespace::p | //*[local-name()='a']/namespace::p").c_str(), 0, &ns).evaluate_node_set(doc);
		CHECK(dup.size() == 1);
	}

	// combined element + namespace nodes + attributes + descendants order
	{
		pugi::xpath_node_set s = pugi::xpath_query(
			T("//*[local-name()='a'] | //*[local-name()='a']/namespace::* | //*[local-name()='a']/@* | //*[local-name()='a']/descendant::*").c_str(), 0, &ns)
			.evaluate_node_set(doc);

		CHECK(s.size() == 7);
		CHECK(!s[0].is_namespace() && s[0].node() == a);

		for (size_t i = 1; i <= 4; ++i)
		{
			CHECK(s[i].is_namespace());
			CHECK(s[i].parent() == a);
		}

		// prefixes are ordered within the element's namespace nodes
		CHECK(same(s[1].namespace_prefix(), ""));
		CHECK(same(s[2].namespace_prefix(), "p"));
		CHECK(same(s[3].namespace_prefix(), "q"));
		CHECK(same(s[4].namespace_prefix(), "xml"));

		CHECK(!s[5].is_namespace() && s[5].attribute().name() == pugi::string_t(T("attr")));
		CHECK(!s[6].is_namespace() && s[6].node().name() == pugi::string_t(T("p:x")));
	}

	// namespace nodes survive in a node-set variable and outlive the query
	{
		pugi::xpath_node_set saved;

		{
			// the query is destroyed before the set is used
			pugi::xpath_query q(T("//*[local-name()='a']/namespace::*").c_str(), 0, &ns);
			saved = q.evaluate_node_set(doc);
		}

		CHECK(saved.size() == 4);

		// putting the result into a node-set variable keeps namespace identities intact
		pugi::xpath_variable_set vars;
		CHECK(vars.set(T("v").c_str(), saved));

		pugi::xpath_query use(T("$v[1]").c_str(), &vars, &ns);
		pugi::xpath_node via_var = use.evaluate_node_set(doc).first();
		CHECK(via_var.is_namespace() && via_var.parent() == a && same(via_var.namespace_prefix(), ""));

		// stored handles remain usable after the query object is gone
		pugi::xpath_node n = saved[3];
		CHECK(n.is_namespace() && same(n.namespace_prefix(), "xml"));
		CHECK(same(n.namespace_value(), "http://www.w3.org/XML/1998/namespace"));
	}

	// fully qualified name test on the namespace axis is rejected at compile time
	bool failed = false;

	try
	{
		pugi::xpath_query bad(T("namespace::p:x").c_str(), 0, &ns);
	}
	catch (const pugi::xpath_exception&)
	{
		failed = true;
	}

	CHECK(failed);
}
