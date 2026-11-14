// Streaming regression contract for ParserSax::beginStream/feed/finishStream
// and ParserDom::takeCompleted. Registered via task_support/check.h.

#include "html/ParserDom.h"
#include "task_support/check.h"

#include <stdexcept>
#include <string>
#include <vector>

using htmlcxx::HTML::Node;
using htmlcxx::HTML::ParserDom;
using htmlcxx::HTML::ParserSax;

namespace
{
	struct Event
	{
		int kind; // 0=open tag, 1=close tag, 2=text, 3=comment
		std::string text;
		std::string tag;
		unsigned offset;
		unsigned length;
	};

	class Recorder : public ParserSax
	{
		public:
			std::vector<Event> events;
			unsigned begins = 0;
			unsigned ends = 0;
		protected:
			void beginParsing() override { ++begins; }
			void endParsing() override { ++ends; }
			void foundTag(Node node, bool isEnd) override
			{
				Event e = { isEnd ? 1 : 0, node.text(), node.tagName(), node.offset(), node.length() };
				events.push_back(e);
			}
			void foundText(Node node) override
			{
				Event e = { 2, node.text(), node.tagName(), node.offset(), node.length() };
				events.push_back(e);
			}
			void foundComment(Node node) override
			{
				Event e = { 3, node.text(), node.tagName(), node.offset(), node.length() };
				events.push_back(e);
			}
	};

	int checkEventsEqual(const std::vector<Event> &got, const std::vector<Event> &want)
	{
		TASK_CHECK(got.size() == want.size());
		size_t n = got.size() < want.size() ? got.size() : want.size();
		for (size_t i = 0; i < n; ++i)
		{
			TASK_CHECK(got[i].kind == want[i].kind);
			TASK_CHECK(got[i].text == want[i].text);
			TASK_CHECK(got[i].tag == want[i].tag);
			TASK_CHECK(got[i].offset == want[i].offset);
			TASK_CHECK(got[i].length == want[i].length);
		}
		return 0;
	}

	int checkSameTree(const tree<Node> &got, const tree<Node> &want)
	{
		tree<Node>::pre_order_iterator ig = got.begin();
		tree<Node>::pre_order_iterator iw = want.begin();
		while (ig != got.end() && iw != want.end())
		{
			TASK_CHECK(got.depth(ig) == want.depth(iw));
			TASK_CHECK(ig->isTag() == iw->isTag());
			TASK_CHECK(ig->isComment() == iw->isComment());
			TASK_CHECK(ig->tagName() == iw->tagName());
			TASK_CHECK(ig->text() == iw->text());
			TASK_CHECK(ig->closingText() == iw->closingText());
			TASK_CHECK(ig->offset() == iw->offset());
			TASK_CHECK(ig->length() == iw->length());
			++ig;
			++iw;
		}
		TASK_CHECK(ig == got.end());
		TASK_CHECK(iw == want.end());
		return 0;
	}

	// Parse html in one shot and in a stream cut into chunk sizes, and
	// require identical SAX events and identical DOM trees.
	int checkStreamEqualsOneShot(const std::string &html, const std::vector<size_t> &chunks)
	{
		Recorder oneShot;
		oneShot.parse(html);

		Recorder streamed;
		streamed.beginStream();
		size_t pos = 0;
		for (size_t i = 0; i < chunks.size(); ++i)
		{
			streamed.feed(html.substr(pos, chunks[i]));
			pos += chunks[i];
			if (pos > html.size()) pos = html.size();
		}
		streamed.feed(html.substr(pos));
		streamed.finishStream();

		TASK_CHECK(streamed.begins == 1);
		TASK_CHECK(streamed.ends == 1);
		if (checkEventsEqual(streamed.events, oneShot.events)) return 1;

		ParserDom domOne;
		const tree<Node> &ref = domOne.parseTree(html);

		ParserDom domStream;
		domStream.beginStream();
		pos = 0;
		for (size_t i = 0; i < chunks.size(); ++i)
		{
			domStream.feed(html.substr(pos, chunks[i]));
			pos += chunks[i];
			if (pos > html.size()) pos = html.size();
		}
		domStream.feed(html.substr(pos));
		domStream.finishStream();

		TASK_CHECK(domStream.getTree().begin()->length() == html.size());
		if (checkSameTree(domStream.getTree(), ref)) return 1;
		return 0;
	}

	int checkStreamByteByByte(const std::string &html)
	{
		Recorder oneShot;
		oneShot.parse(html);

		Recorder rec;
		ParserDom dom;
		rec.beginStream();
		dom.beginStream();
		unsigned expected = 0;
		for (size_t i = 0; i < html.size(); ++i)
		{
			rec.feed(html.substr(i, 1));
			dom.feed(html.substr(i, 1));
			if (i % 3 == 0)
			{
				rec.feed("");  // empty chunks have no effect
				dom.feed("");
			}
			if (!rec.events.empty())
				expected = rec.events.back().offset + rec.events.back().length;
			// The live virtual root spans exactly the emitted tokens
			TASK_CHECK(dom.getTree().begin()->length() == expected);
		}
		rec.finishStream();
		dom.finishStream();

		TASK_CHECK(rec.begins == 1);
		TASK_CHECK(rec.ends == 1);
		if (checkEventsEqual(rec.events, oneShot.events)) return 1;

		ParserDom domOne;
		const tree<Node> &ref = domOne.parseTree(html);
		TASK_CHECK(dom.getTree().begin()->length() == html.size());
		if (checkSameTree(dom.getTree(), ref)) return 1;
		return 0;
	}

	unsigned rootChildren(const tree<Node> &t)
	{
		return t.number_of_children(t.begin());
	}

	const std::string kDoc1 =
		"<!DOCTYPE html>"
		"<div class='a>b' id=\"x\">"
		"<script>if(a<b){s=\"</scr\";}</script>"
		"<style>a>b{}</style>"
		"<!-- c1 --><!-- a -- b -->"
		"<textarea><b></textarea>"
		"<p>one<br>two</p>"
		"</div>"
		"<ul><li>x<li>y</ul>"
		"<?pi data?>"
		"</ >"
		"a<b"
		"tail";

	const std::string kDoc2 =
		"<SCRIPT>a<!--x-->b</SCRIPT>"
		"<XMP><p></XMP>"
		"<xmp>1</xmp >"
		"<textarea>2</TEXTAREA >"
		"<plaintext>rest <b> not closed";

	const std::string kDocUnclosed = "<div><span>abc";
	const std::string kDocQuote = "<a href=\"unterminated";
	const std::string kDocComment = "text<!-- comment";
	const std::string kDocScript = "<script>var x = \"";
	const std::string kDocLt = "hello<";
	const std::string kDocClose = "a</";
	const std::string kDocBang = "<p>x</p><!";
	const std::string kDocDash = "<p>x</p><!-";

	// ---------------------------------------------------------------- CHUNKS

	int chunks_two_way_splits()
	{
		const std::string docs[] = {
			kDoc1, kDoc2, kDocUnclosed, kDocQuote, kDocComment,
			kDocScript, kDocLt, kDocClose, kDocBang, kDocDash,
		};
		for (size_t d = 0; d < sizeof(docs) / sizeof(docs[0]); ++d)
		{
			for (size_t cut = 0; cut <= docs[d].size(); ++cut)
			{
				std::vector<size_t> chunks(1, cut);
				if (checkStreamEqualsOneShot(docs[d], chunks)) return 1;
			}
		}
		return 0;
	}

	int chunks_byte_by_byte()
	{
		const std::string docs[] = {
			kDoc1, kDoc2, kDocQuote, kDocComment, kDocScript,
		};
		for (size_t d = 0; d < sizeof(docs) / sizeof(docs[0]); ++d)
			if (checkStreamByteByByte(docs[d])) return 1;
		return 0;
	}

	int chunks_uneven_chunkings()
	{
		// fibonacci-like growth, single big chunk, empty chunks mixed in
		const size_t patterns[][8] = {
			{ 1, 2, 3, 5, 8, 13, 21, 34 },
			{ 4096, 0, 0, 0, 0, 0, 0, 0 },
			{ 0, 7, 0, 11, 0, 1, 0, 64 },
			{ 10, 10, 10, 10, 10, 10, 10, 10 },
		};
		const std::string docs[] = { kDoc1, kDoc2, kDocComment };
		for (size_t d = 0; d < sizeof(docs) / sizeof(docs[0]); ++d)
		{
			for (size_t p = 0; p < sizeof(patterns) / sizeof(patterns[0]); ++p)
			{
				std::vector<size_t> chunks(patterns[p], patterns[p] + 8);
				if (checkStreamEqualsOneShot(docs[d], chunks)) return 1;
			}
		}
		return 0;
	}

	// ----------------------------------------------------------------- DRAIN

	int drain_records_prefix()
	{
		const std::string r1 = "<div class='rec'>first</div>";
		const std::string r2 = "<div class='rec'>second</div>";
		const std::string r3 = "<div class='rec'>third</div>";
		const std::string html = r1 + r2 + r3;

		ParserDom oneShot;
		const tree<Node> &ref = oneShot.parseTree(html);
		TASK_CHECK(rootChildren(oneShot.getTree()) == 3);

		ParserDom p;
		p.beginStream();
		p.feed(r1);
		TASK_CHECK(p.getTree().begin()->length() == r1.size());

		tree<Node> t1 = p.takeCompleted();
		TASK_CHECK(t1.begin()->isTag()); // same virtual root
		TASK_CHECK(t1.begin()->length() == r1.size());
		TASK_CHECK(rootChildren(t1) == 1);
		// taking must not reset the live root length
		TASK_CHECK(p.getTree().begin()->length() == r1.size());
		TASK_CHECK(rootChildren(p.getTree()) == 0);

		// a repeated take does not hand out old nodes again
		tree<Node> t1again = p.takeCompleted();
		TASK_CHECK(rootChildren(t1again) == 0);
		TASK_CHECK(t1again.begin()->isTag());

		tree<Node>::sibling_iterator rec1 = t1.begin(t1.begin());
		TASK_CHECK(rec1->isTag() && rec1->tagName() == "div");
		TASK_CHECK(rec1->offset() == 0);
		TASK_CHECK(rec1->length() == r1.size());
		TASK_CHECK(rec1->closingText() == "</div>");

		// feed the second record in awkward pieces, then drain again
		p.feed(r2.substr(0, 5));
		p.feed("");
		p.feed(r2.substr(5));
		TASK_CHECK(p.getTree().begin()->length() == r1.size() + r2.size());
		tree<Node> t2 = p.takeCompleted();
		TASK_CHECK(rootChildren(t2) == 1);
		tree<Node>::sibling_iterator rec2 = t2.begin(t2.begin());
		TASK_CHECK(rec2->offset() == r1.size());
		TASK_CHECK(rec2->length() == r2.size());
		TASK_CHECK(rec2->closingText() == "</div>");

		p.feed(r3.substr(0, 10));
		p.feed(r3.substr(10));
		p.finishStream();
		TASK_CHECK(p.getTree().begin()->length() == html.size());
		tree<Node> t3 = p.takeCompleted();
		TASK_CHECK(rootChildren(t3) == 1);
		tree<Node>::sibling_iterator rec3 = t3.begin(t3.begin());
		TASK_CHECK(rec3->offset() == r1.size() + r2.size());
		TASK_CHECK(rec3->length() == r3.size());

		// everything drained now; further takes stay empty
		TASK_CHECK(rootChildren(p.getTree()) == 0);
		tree<Node> tEnd = p.takeCompleted();
		TASK_CHECK(rootChildren(tEnd) == 0);

		// the taken records match the one-shot root children one by one
		const tree<Node>::sibling_iterator taken[] = { rec1, rec2, rec3 };
		tree<Node>::sibling_iterator want = ref.begin(ref.begin());
		for (int i = 0; i < 3; ++i)
		{
			TASK_CHECK(taken[i]->tagName() == want->tagName());
			TASK_CHECK(taken[i]->text() == want->text());
			TASK_CHECK(taken[i]->closingText() == want->closingText());
			TASK_CHECK(taken[i]->offset() == want->offset());
			TASK_CHECK(taken[i]->length() == want->length());
			++want;
		}
		return 0;
	}

	int drain_active_branch_and_flatten()
	{
		const std::string part1 = "<div><span><b>bold";
		const std::string part2 = "</div>";
		const std::string part3 = "<p>next</p>";
		const std::string html = part1 + part2 + part3;

		ParserDom p;
		p.beginStream();
		p.feed(part1);

		// the div branch is still active: nothing may be taken
		tree<Node> t0 = p.takeCompleted();
		TASK_CHECK(rootChildren(t0) == 0);

		// the live tree matches a one-shot parse of the emitted prefix
		// (the trailing text "bold" is still pending inside the parser)
		ParserDom prefixOne;
		const tree<Node> &prefixRef = prefixOne.parseTree("<div><span><b>");
		if (checkSameTree(p.getTree(), prefixRef)) return 1;

		// remember the addresses of the nodes still being built
		std::vector<const Node *> before;
		for (tree<Node>::pre_order_iterator it = p.getTree().begin(); it != p.getTree().end(); ++it)
			before.push_back(&*it);
		TASK_CHECK(before.size() == 4); // root, div, span, b

		// closing the ancestor flattens the unclosed successors first
		p.feed(part2);

		std::vector<const Node *> after;
		for (tree<Node>::pre_order_iterator it = p.getTree().begin(); it != p.getTree().end(); ++it)
			after.push_back(&*it);
		TASK_CHECK(after.size() == before.size() + 1); // the "bold" text arrived
		for (size_t i = 0; i < before.size() && i < after.size(); ++i)
			TASK_CHECK(before[i] == after[i]); // node identity preserved

		ParserDom prefix2One;
		const tree<Node> &prefix2Ref = prefix2One.parseTree(part1 + part2);
		if (checkSameTree(p.getTree(), prefix2Ref)) return 1;

		// flatten rules: span and b lost their children to the div
		tree<Node>::iterator root = p.getTree().begin();
		tree<Node>::sibling_iterator div = p.getTree().begin(root);
		TASK_CHECK(div->tagName() == "div");
		TASK_CHECK(div->closingText() == part2);
		TASK_CHECK(p.getTree().number_of_children(div) == 3);
		tree<Node>::sibling_iterator span = p.getTree().begin(div);
		TASK_CHECK(span->tagName() == "span");
		TASK_CHECK(p.getTree().number_of_children(span) == 0);

		// once the ancestor is closed the branch can be taken, successors too
		p.feed(part3);
		p.finishStream();
		tree<Node> all = p.takeCompleted();
		TASK_CHECK(rootChildren(all) == 2);
		ParserDom fullOne;
		const tree<Node> &fullRef = fullOne.parseTree(html);
		if (checkSameTree(all, fullRef)) return 1;
		TASK_CHECK(rootChildren(p.getTree()) == 0);
		return 0;
	}

	int drain_then_continue()
	{
		const std::string r1 = "<div>one</div>";
		const std::string r2 = "<div>two</div>";

		ParserDom p;
		p.beginStream();
		p.feed(r1);
		tree<Node> t1 = p.takeCompleted();
		TASK_CHECK(rootChildren(t1) == 1);

		// keep feeding after the drain; offsets keep counting original bytes
		p.feed(r2.substr(0, 4));
		p.feed(r2.substr(4));
		p.finishStream();
		TASK_CHECK(p.getTree().begin()->length() == r1.size() + r2.size());

		tree<Node> t2 = p.takeCompleted();
		TASK_CHECK(rootChildren(t2) == 1);
		tree<Node>::sibling_iterator rec2 = t2.begin(t2.begin());
		TASK_CHECK(rec2->offset() == r1.size());
		TASK_CHECK(rec2->length() == r2.size());

		// the tree taken earlier is unaffected by the later parsing
		TASK_CHECK(rootChildren(t1) == 1);
		tree<Node>::sibling_iterator rec1 = t1.begin(t1.begin());
		TASK_CHECK(rec1->tagName() == "div");
		TASK_CHECK(rec1->offset() == 0);
		TASK_CHECK(rec1->length() == r1.size());
		TASK_CHECK(rec1->closingText() == "</div>");
		tree<Node>::sibling_iterator text1 = t1.begin(rec1);
		TASK_CHECK(text1->text() == "one");
		TASK_CHECK(text1->offset() == 5);
		return 0;
	}

	// ------------------------------------------------------------- LIFECYCLE

	int lifecycle_state_errors()
	{
		Recorder r;

		bool threw = false;
		try { r.feed("<p>x</p>"); }
		catch (const std::logic_error &) { threw = true; }
		TASK_CHECK(threw);
		TASK_CHECK(r.events.empty());

		r.finishStream(); // no session: no effect
		TASK_CHECK(r.ends == 0);

		r.beginStream();
		r.feed("<p>a</p>");
		r.finishStream();
		r.finishStream(); // repeated finish: no effect
		TASK_CHECK(r.begins == 1);
		TASK_CHECK(r.ends == 1);
		TASK_CHECK(r.events.size() == 3); // open tag, text, close tag

		threw = false;
		try { r.feed("<p>y</p>"); }
		catch (const std::logic_error &) { threw = true; }
		TASK_CHECK(threw);

		// a finished parser accepts a new session; offsets restart at zero
		r.beginStream();
		r.feed("<p>b</p>");
		r.finishStream();
		TASK_CHECK(r.begins == 2);
		TASK_CHECK(r.ends == 2);
		TASK_CHECK(r.events.size() == 6);
		TASK_CHECK(r.events[3].offset == 0);

		// one-shot parses stay independent one-shot sessions
		r.parse(std::string("<p>c</p>"));
		TASK_CHECK(r.begins == 3);
		TASK_CHECK(r.ends == 3);
		TASK_CHECK(r.events.size() == 9);
		TASK_CHECK(r.events[6].offset == 0);

		// and a stream session still works afterwards
		r.beginStream();
		r.feed("<p>d");
		r.feed("</p>");
		r.finishStream();
		TASK_CHECK(r.begins == 4);
		TASK_CHECK(r.ends == 4);
		TASK_CHECK(r.events.size() == 12);
		TASK_CHECK(r.events[9].offset == 0);
		return 0;
	}

	int lifecycle_dom_reuse_and_lifetime()
	{
		ParserDom p;
		p.parseTree("<div>one</div>");
		TASK_CHECK(rootChildren(p.getTree()) == 1);

		// restarting as a stream clears the old tree
		p.beginStream();
		TASK_CHECK(rootChildren(p.getTree()) == 0);
		p.feed("<div>t");
		p.feed("wo</div>");
		p.finishStream();
		TASK_CHECK(rootChildren(p.getTree()) == 1);

		// a taken tree owns its nodes and survives parser reuse
		tree<Node> taken = p.takeCompleted();
		TASK_CHECK(rootChildren(taken) == 1);
		TASK_CHECK(rootChildren(p.getTree()) == 0);

		p.beginStream();
		p.feed("<span>three</span>");
		p.finishStream();
		TASK_CHECK(rootChildren(taken) == 1);
		tree<Node>::sibling_iterator kept = taken.begin(taken.begin());
		TASK_CHECK(kept->tagName() == "div");
		TASK_CHECK(kept->length() == std::string("<div>two</div>").size());
		TASK_CHECK(kept->closingText() == "</div>");

		// the new session only holds the new record
		TASK_CHECK(rootChildren(p.getTree()) == 1);
		tree<Node>::sibling_iterator span = p.getTree().begin(p.getTree().begin());
		TASK_CHECK(span->tagName() == "span");
		TASK_CHECK(span->offset() == 0);

		// one-shot parseTree still works after streaming
		const tree<Node> &again = p.parseTree("<i>four</i>");
		TASK_CHECK(rootChildren(again) == 1);
		tree<Node>::sibling_iterator i = again.begin(again.begin());
		TASK_CHECK(i->tagName() == "i");
		TASK_CHECK(again.begin()->length() == std::string("<i>four</i>").size());
		return 0;
	}

	void collectRecordTexts(const tree<Node> &t, std::vector<std::string> &out)
	{
		tree<Node>::sibling_iterator it = t.begin(t.begin());
		tree<Node>::sibling_iterator end = t.end(t.begin());
		for (; it != end; ++it)
		{
			std::string text;
			tree<Node>::pre_order_iterator d = t.begin(it);
			tree<Node>::pre_order_iterator dend = t.end(it);
			for (; d != dend; ++d)
				if (!d->isTag() && !d->isComment())
					text += d->text();
			out.push_back(text);
		}
	}

	// Small usage example: drain the records of a long list while the
	// response is still arriving, instead of parsing everything at the end.
	int lifecycle_example()
	{
		ParserDom parser;
		std::vector<std::string> records;
		const char *chunks[] = {
			"<li>first</li><li>sec",
			"ond</li><li>third</li",
			"><li>fourth</li>",
		};

		parser.beginStream();
		for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); ++i)
		{
			parser.feed(chunks[i]);
			tree<Node> done = parser.takeCompleted();
			collectRecordTexts(done, records);
		}
		parser.finishStream();
		tree<Node> done = parser.takeCompleted();
		collectRecordTexts(done, records);

		TASK_CHECK(records.size() == 4);
		if (records.size() == 4)
		{
			TASK_CHECK(records[0] == "first");
			TASK_CHECK(records[1] == "second");
			TASK_CHECK(records[2] == "third");
			TASK_CHECK(records[3] == "fourth");
		}
		TASK_CHECK(rootChildren(parser.getTree()) == 0);
		return 0;
	}
}

const struct task_case task_cases[] = {
	{ "CHUNKS", "two_way_splits", chunks_two_way_splits },
	{ "CHUNKS", "byte_by_byte", chunks_byte_by_byte },
	{ "CHUNKS", "uneven_chunkings", chunks_uneven_chunkings },
	{ "DRAIN", "records_prefix", drain_records_prefix },
	{ "DRAIN", "active_branch_and_flatten", drain_active_branch_and_flatten },
	{ "DRAIN", "then_continue", drain_then_continue },
	{ "LIFECYCLE", "state_errors", lifecycle_state_errors },
	{ "LIFECYCLE", "dom_reuse_and_lifetime", lifecycle_dom_reuse_and_lifetime },
	{ "LIFECYCLE", "example", lifecycle_example },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
