//
// Regression tests for value-semantics Cue ownership, isolated preview
// trees, and visitor edits that stay on their own cue.
//
// Registered through task_support/check.h in three groups:
//   ownership, preview, visitor
//

#include <TimedText/Cue.h>
#include <TimedText/WebVTTParser.h>
#include <TimedText/StringBuilder.h>
#include "task_support/check.h"
#include <cstring>

using namespace TimedText;

// Nested subtitle exercising classes, voice annotation, language, ruby
// and an inline timestamp node.
static const char richCueText[] =
  "<c.hot><v Jane><lang en-US>Ruby <ruby>base<rt>b</rt></ruby>"
  "<00:01.500>done</lang></v></c>";

// Collect every TextNode's text, in traversal order.
struct TextCollector : NodeVisitor
{
  StringBuilder out;
  void visit(Node &node)
  {
    if(node.element() == TextNode)
      out.append(node.text());
  }
};

// Replace the text of every TextNode encountered.
struct TextRewriter : NodeVisitor
{
  explicit TextRewriter(const char *replacement) : word(replacement) {}
  void visit(Node &node)
  {
    if(node.element() == TextNode)
      node.setText(String(word));
  }
  const char *word;
};

static String collectedText(const Cue &cue)
{
  // TASK_CHECK can only be used in int-returning cases, so this helper
  // relies on its callers to assert on the produced string.
  Node tree;
  cue.nodes(tree);
  TextCollector collector;
  tree.visit(collector);
  String result;
  collector.out.toString(result);
  return result;
}

// Locate the first node of the given element type in a depth-first walk.
static bool findNode(Node &root, NodeElementType element, Node &result)
{
  if(root.element() == element) {
    result = root;
    return true;
  }
  for(int i = 0; i < root.childCount(); ++i) {
    Node child;
    if(root.itemAt(i, child) && findNode(child, element, result))
      return true;
  }
  return false;
}

static int timestampCount(const Node &root)
{
  int count = root.element() == TimestampNode ? 1 : 0;
  const List<Node> &children = root.children();
  for(int i = 0; i < children.count(); ++i) {
    Node child;
    if(children.itemAt(i, child))
      count += timestampCount(child);
  }
  return count;
}

// ---------------------------------------------------------------------------
// ownership: copies (ctor, assignment, List round-trip) keep a full snapshot
// and are independently editable.
// ---------------------------------------------------------------------------

static int ownership_nested_snapshot(void)
{
  Cue original(WebVTTCue, Timestamp(1.0), Timestamp(3.0),
               String("cue-1"), String(richCueText));
  TASK_CHECK(WebVTTParser::parseCuetext(original));
  TASK_CHECK(original.setAlign(Cue::Start));
  TASK_CHECK(original.setSize("42%"));
  TASK_CHECK(original.setVertical("rl"));

  Cue copy(original);

  // The copy preserves text, timings, id, layout and the parsed tree.
  TASK_CHECK(std::strcmp(copy.text().text(), richCueText) == 0);
  TASK_CHECK((Milliseconds)copy.startTime() == 1000);
  TASK_CHECK((Milliseconds)copy.endTime() == 3000);
  TASK_CHECK(std::strcmp(copy.id().text(), "cue-1") == 0);
  TASK_CHECK(copy.align() == Cue::Start);
  TASK_CHECK(copy.size() == 42);
  TASK_CHECK(copy.vertical() == Cue::VerticalRightToLeft);
  TASK_CHECK(std::strcmp(collectedText(copy).text(), "Ruby basebdone") == 0);
  Node copyTree;
  TASK_CHECK(copy.nodes(copyTree));
  TASK_CHECK(timestampCount(copyTree) == 1);
  Node voice;
  TASK_CHECK(findNode(copyTree, VoiceNode, voice));
  TASK_CHECK(std::strcmp(voice.voice().text(), "Jane") == 0);
  Node lang;
  TASK_CHECK(findNode(copyTree, LangNode, lang));
  TASK_CHECK(std::strcmp(lang.lang().text(), "en-US") == 0);
  Node classNode;
  TASK_CHECK(findNode(copyTree, ClassNode, classNode));
  List<String> classes = classNode.applicableClasses();
  TASK_CHECK(classes.count() == 1);
  String className;
  TASK_CHECK(classes.itemAt(0, className));
  TASK_CHECK(std::strcmp(className.text(), "hot") == 0);
  Node ruby;
  TASK_CHECK(findNode(copyTree, RubyNode, ruby));
  TASK_CHECK(ruby.childCount() == 2);
  return 0;
}

static int ownership_copy_is_independent(void)
{
  Cue original(WebVTTCue, Timestamp(1.0), Timestamp(3.0),
               String("cue-1"), String("<b>old</b>"));
  TASK_CHECK(WebVTTParser::parseCuetext(original));

  Cue copy(original);
  copy.setId("draft");
  copy.setText("<i>new</i>");
  TASK_CHECK(copy.setAlign(Cue::End));
  copy.setStartTime(Timestamp(5.0));
  copy.setEndTime(Timestamp(7.0));

  // Every edit lands on the copy only; the original stays a snapshot.
  TASK_CHECK(std::strcmp(original.id().text(), "cue-1") == 0);
  TASK_CHECK(std::strcmp(original.text().text(), "<b>old</b>") == 0);
  TASK_CHECK(original.align() == Cue::Middle);
  TASK_CHECK((Milliseconds)original.startTime() == 1000);
  TASK_CHECK((Milliseconds)original.endTime() == 3000);
  TASK_CHECK(std::strcmp(collectedText(original).text(), "old") == 0);

  TASK_CHECK(std::strcmp(copy.id().text(), "draft") == 0);
  TASK_CHECK(copy.align() == Cue::End);
  TASK_CHECK((Milliseconds)copy.startTime() == 5000);
  TASK_CHECK((Milliseconds)copy.endTime() == 7000);
  TASK_CHECK(std::strcmp(collectedText(copy).text(), "new") == 0);

  // Assignment has the same value semantics.
  Cue assigned;
  assigned = original;
  assigned.setId("second-draft");
  TASK_CHECK(std::strcmp(original.id().text(), "cue-1") == 0);
  TASK_CHECK(std::strcmp(assigned.id().text(), "second-draft") == 0);
  TASK_CHECK(std::strcmp(collectedText(assigned).text(), "old") == 0);
  return 0;
}

static int ownership_list_roundtrip(void)
{
  Cue original(WebVTTCue, Timestamp(1.0), Timestamp(3.0),
               String("cue-1"), String(richCueText));
  TASK_CHECK(WebVTTParser::parseCuetext(original));

  List<Cue> store;
  TASK_CHECK(store.push(original));

  Cue fetched;
  TASK_CHECK(store.itemAt(0, fetched));
  fetched.setId("from-list");
  fetched.setText("<u>rewritten</u>");

  // The list's own copy, and the source cue, are both untouched.
  Cue stillStored;
  TASK_CHECK(store.itemAt(0, stillStored));
  TASK_CHECK(std::strcmp(stillStored.id().text(), "cue-1") == 0);
  TASK_CHECK(std::strcmp(stillStored.text().text(), richCueText) == 0);
  TASK_CHECK(std::strcmp(original.id().text(), "cue-1") == 0);
  TASK_CHECK(std::strcmp(collectedText(fetched).text(), "rewritten") == 0);
  TASK_CHECK(std::strcmp(collectedText(stillStored).text(),
                         "Ruby basebdone") == 0);

  // A self-assignment must remain valid.
  fetched = fetched;
  TASK_CHECK(std::strcmp(fetched.id().text(), "from-list") == 0);
  return 0;
}

static int ownership_unparsed_and_empty_cues(void)
{
  // A cue whose text was never parsed still copies its raw text safely.
  Cue unparsed(WebVTTCue, Timestamp(0.250), Timestamp(0.750),
               String("raw"), String("<b>not parsed yet"));
  Cue unparsedCopy(unparsed);
  unparsedCopy.setText("plain replacement");
  TASK_CHECK(std::strcmp(unparsed.text().text(), "<b>not parsed yet") == 0);
  TASK_CHECK(std::strcmp(unparsedCopy.text().text(),
                         "plain replacement") == 0);

  // Default-constructed empty cues must copy and mutate without trouble.
  Cue empty;
  Cue emptyCopy(empty);
  emptyCopy.setId("ignored-on-empty");
  TASK_CHECK(empty.type() == EmptyCue);
  TASK_CHECK(emptyCopy.type() == EmptyCue);
  TASK_CHECK(std::strcmp(empty.id().text(), "") == 0);
  TASK_CHECK(std::strcmp(emptyCopy.id().text(), "") == 0);
  Cue emptyAssigned;
  emptyAssigned = empty;
  TASK_CHECK(emptyAssigned.type() == EmptyCue);
  return 0;
}

// ---------------------------------------------------------------------------
// preview: setText rebuilds the tree from current text (empty included),
// setNodes owns an isolated tree until the next setText, and parseCuetext
// regenerates the preview without dropping layout settings.
// ---------------------------------------------------------------------------

static int preview_settext_rebuilds_tree(void)
{
  Cue cue(WebVTTCue, Timestamp(0.0), Timestamp(1.0), String(),
          String("<b>old</b>"));
  TASK_CHECK(WebVTTParser::parseCuetext(cue));
  TASK_CHECK(std::strcmp(collectedText(cue).text(), "old") == 0);

  cue.setText("<i>new</i>");
  // No stale tree from the previous draft may survive.
  Node tree;
  TASK_CHECK(cue.nodes(tree));
  TASK_CHECK(tree.element() == InternalTextNode);
  TASK_CHECK(tree.childCount() == 1);
  Node wrapper;
  TASK_CHECK(tree.itemAt(0, wrapper));
  TASK_CHECK(wrapper.element() == ItalicNode);
  TASK_CHECK(std::strcmp(collectedText(cue).text(), "new") == 0);

  // Empty text still produces a readable, empty InternalTextNode root.
  cue.setText(String());
  TASK_CHECK(std::strcmp(cue.text().text(), "") == 0);
  Node emptyTree;
  TASK_CHECK(cue.nodes(emptyTree));
  TASK_CHECK(emptyTree.element() == InternalTextNode);
  TASK_CHECK(emptyTree.childCount() == 0);
  TASK_CHECK(std::strcmp(collectedText(cue).text(), "") == 0);

  // A freshly constructed cue is likewise readable with an empty root.
  Cue fresh(WebVTTCue);
  Node freshTree;
  TASK_CHECK(fresh.nodes(freshTree));
  TASK_CHECK(freshTree.element() == InternalTextNode);
  TASK_CHECK(freshTree.childCount() == 0);
  return 0;
}

static int preview_setnodes_isolated_and_stable(void)
{
  Cue cue(WebVTTCue, Timestamp(0.0), Timestamp(1.0), String(),
          String("<b>text</b>"));
  TASK_CHECK(WebVTTParser::parseCuetext(cue));

  // Build an explicit preview tree with nested children.
  Node root(InternalTextNode);
  Node bold(BoldNode);
  Node leaf(TextNode);
  TASK_CHECK(leaf.setText(String("handmade")));
  TASK_CHECK(bold.push(leaf));
  TASK_CHECK(root.push(bold));

  TASK_CHECK(cue.setNodes(root));

  // Mutating the caller's tree after setNodes must not reach the cue.
  TASK_CHECK(leaf.setText(String("mutated outside")));
  TASK_CHECK(bold.pop(leaf));
  TASK_CHECK(std::strcmp(collectedText(cue).text(), "handmade") == 0);

  // The explicit tree stays current across reads ...
  Node firstRead;
  TASK_CHECK(cue.nodes(firstRead));
  TASK_CHECK(std::strcmp(collectedText(cue).text(), "handmade") == 0);

  // ... and a tree handed out by nodes() cannot write back into the cue.
  Node editable;
  TASK_CHECK(cue.nodes(editable));
  Node editableLeaf;
  TASK_CHECK(findNode(editable, TextNode, editableLeaf));
  TASK_CHECK(editableLeaf.setText(String("mutated copy")));
  TASK_CHECK(std::strcmp(collectedText(cue).text(), "handmade") == 0);
  (void)firstRead;

  // The explicit preview survives until the next setText, which replaces it
  // with a tree parsed from the new text.
  cue.setText("<u>from text</u>");
  Node afterText;
  TASK_CHECK(cue.nodes(afterText));
  Node wrapper;
  TASK_CHECK(afterText.itemAt(0, wrapper));
  TASK_CHECK(wrapper.element() == UnderlineNode);
  TASK_CHECK(std::strcmp(collectedText(cue).text(), "from text") == 0);

  // A non-root node passed to setNodes is wrapped in an InternalTextNode.
  Cue wrapped(WebVTTCue);
  Node lone(ItalicNode);
  TASK_CHECK(wrapped.setNodes(lone));
  Node wrappedTree;
  TASK_CHECK(wrapped.nodes(wrappedTree));
  TASK_CHECK(wrappedTree.element() == InternalTextNode);
  TASK_CHECK(wrappedTree.childCount() == 1);
  return 0;
}

static int preview_reparse_keeps_layout(void)
{
  Cue cue(WebVTTCue, Timestamp(0.0), Timestamp(1.0), String("id"),
          String("<b>first</b>"));
  TASK_CHECK(cue.setAlign(Cue::End));
  TASK_CHECK(cue.setLine("10%"));
  TASK_CHECK(cue.setPosition("30%"));
  TASK_CHECK(cue.setSize("70%"));
  TASK_CHECK(cue.setVertical("lr"));

  cue.setText("<i>second</i>");
  TASK_CHECK(WebVTTParser::parseCuetext(cue));

  // Re-parsing regenerates the preview from the current text ...
  TASK_CHECK(std::strcmp(collectedText(cue).text(), "second") == 0);
  Node tree;
  TASK_CHECK(cue.nodes(tree));
  Node wrapper;
  TASK_CHECK(tree.itemAt(0, wrapper));
  TASK_CHECK(wrapper.element() == ItalicNode);

  // ... while every layout setting is retained.
  TASK_CHECK(cue.align() == Cue::End);
  TASK_CHECK(cue.line() == 10);
  TASK_CHECK(cue.snapToLines() == false);
  TASK_CHECK(cue.position() == 30);
  TASK_CHECK(cue.size() == 70);
  TASK_CHECK(cue.vertical() == Cue::VerticalLeftToRight);
  TASK_CHECK(std::strcmp(cue.id().text(), "id") == 0);

  // Invalid layout values are still rejected, and leave the value intact.
  TASK_CHECK(!cue.setSize("nope"));
  TASK_CHECK(cue.size() == 70);
  TASK_CHECK(!cue.setVertical("sideways"));
  TASK_CHECK(cue.vertical() == Cue::VerticalLeftToRight);
  return 0;
}

// ---------------------------------------------------------------------------
// visitor: edits through Cue::visit persist on that cue only.
// ---------------------------------------------------------------------------

static int visitor_edits_persist_on_cue(void)
{
  Cue cue(WebVTTCue, Timestamp(0.0), Timestamp(1.0), String(),
          String(richCueText));
  TASK_CHECK(WebVTTParser::parseCuetext(cue));

  TextRewriter rewriter("X");
  cue.visit(rewriter);

  // The rewritten leaves are now part of the cue's current preview tree.
  TASK_CHECK(std::strcmp(collectedText(cue).text(), "XXXX") == 0);

  // Re-reading yields the stored edit, not the original parse.
  Node tree;
  TASK_CHECK(cue.nodes(tree));
  Node textNode;
  TASK_CHECK(findNode(tree, TextNode, textNode));
  TASK_CHECK(std::strcmp(textNode.text().text(), "X") == 0);

  // Structural payload of the tree is preserved by the round trip.
  TASK_CHECK(timestampCount(tree) == 1);
  Node voice;
  TASK_CHECK(findNode(tree, VoiceNode, voice));
  TASK_CHECK(std::strcmp(voice.voice().text(), "Jane") == 0);
  return 0;
}

static int visitor_edits_do_not_escape(void)
{
  Cue original(WebVTTCue, Timestamp(0.0), Timestamp(1.0), String(),
               String(richCueText));
  TASK_CHECK(WebVTTParser::parseCuetext(original));

  Cue snapshot(original);
  Cue assigned;
  assigned = original;

  TextRewriter rewriter("X");
  original.visit(rewriter);

  // Neither stored copy sees the visitor's edits.
  TASK_CHECK(std::strcmp(collectedText(original).text(),
                         "XXXX") == 0);
  TASK_CHECK(std::strcmp(collectedText(snapshot).text(),
                         "Ruby basebdone") == 0);
  TASK_CHECK(std::strcmp(collectedText(assigned).text(),
                         "Ruby basebdone") == 0);

  // A tree obtained before the visit is unaffected by it.
  Cue other(WebVTTCue, Timestamp(0.0), Timestamp(1.0), String(),
            String(richCueText));
  TASK_CHECK(WebVTTParser::parseCuetext(other));
  Node handedOut;
  TASK_CHECK(other.nodes(handedOut));
  TextRewriter otherRewriter("Y");
  other.visit(otherRewriter);
  Node handedLeaf;
  TASK_CHECK(findNode(handedOut, TextNode, handedLeaf));
  TASK_CHECK(std::strcmp(handedLeaf.text().text(), "Ruby ") == 0);
  TASK_CHECK(std::strcmp(collectedText(other).text(), "YYYY") == 0);
  return 0;
}

static int visitor_branches_stay_independent(void)
{
  Cue first(WebVTTCue, Timestamp(0.0), Timestamp(1.0), String("a"),
            String("<b>alpha</b>"));
  Cue second(WebVTTCue, Timestamp(2.0), Timestamp(3.0), String("b"),
             String("<i>beta</i>"));
  TASK_CHECK(WebVTTParser::parseCuetext(first));
  TASK_CHECK(WebVTTParser::parseCuetext(second));

  TextRewriter firstRewriter("one");
  first.visit(firstRewriter);
  TextRewriter secondRewriter("two");
  second.visit(secondRewriter);

  TASK_CHECK(std::strcmp(collectedText(first).text(), "one") == 0);
  TASK_CHECK(std::strcmp(collectedText(second).text(), "two") == 0);
  TASK_CHECK(std::strcmp(first.text().text(), "<b>alpha</b>") == 0);
  TASK_CHECK(std::strcmp(second.text().text(), "<i>beta</i>") == 0);
  TASK_CHECK(std::strcmp(first.id().text(), "a") == 0);
  TASK_CHECK(std::strcmp(second.id().text(), "b") == 0);

  // Visiting again edits the current (already visited) tree, not a stale one.
  TextRewriter again("again");
  first.visit(again);
  TASK_CHECK(std::strcmp(collectedText(first).text(), "again") == 0);
  TASK_CHECK(std::strcmp(collectedText(second).text(), "two") == 0);
  return 0;
}

const struct task_case task_cases[] = {
  { "ownership", "nested_snapshot", ownership_nested_snapshot },
  { "ownership", "copy_is_independent", ownership_copy_is_independent },
  { "ownership", "list_roundtrip", ownership_list_roundtrip },
  { "ownership", "unparsed_and_empty_cues", ownership_unparsed_and_empty_cues },
  { "preview", "settext_rebuilds_tree", preview_settext_rebuilds_tree },
  { "preview", "setnodes_isolated_and_stable", preview_setnodes_isolated_and_stable },
  { "preview", "reparse_keeps_layout", preview_reparse_keeps_layout },
  { "visitor", "edits_persist_on_cue", visitor_edits_persist_on_cue },
  { "visitor", "edits_do_not_escape", visitor_edits_do_not_escape },
  { "visitor", "branches_stay_independent", visitor_branches_stay_independent },
  { 0, 0, 0 }
};

const size_t task_case_count =
  sizeof(task_cases) / sizeof(task_cases[0]) - 1;
