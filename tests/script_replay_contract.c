// Contract tests for the script-output replay interface (src/script_replay.h).
// Registered with task_support/check.h; no main() here - the driver in
// task_support/driver.c runs the cases.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gumbo.h"
#include "error.h"
#include "script_replay.h"
#include "task_support/check.h"

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

// Counting allocator, to prove that every allocation (tree, source, wrapper)
// goes through the caller's options and is released exactly once.
static int live_allocs;

static void* test_alloc(void* userdata, size_t size) {
  (void) userdata;
  void* p = malloc(size);
  if (p) ++live_allocs;
  return p;
}

static void test_dealloc(void* userdata, void* p) {
  (void) userdata;
  if (p) {
    --live_allocs;
    free(p);
  }
}

static GumboOptions test_options(void) {
  GumboOptions options = kGumboDefaultOptions;
  options.allocator = test_alloc;
  options.deallocator = test_dealloc;
  return options;
}

static int is_elementish(const GumboNode* node) {
  return node->type == GUMBO_NODE_ELEMENT || node->type == GUMBO_NODE_TEMPLATE;
}

static GumboNode* find_first(GumboNode* node, GumboTag tag) {
  GumboVector* children = NULL;
  if (node->type == GUMBO_NODE_DOCUMENT) {
    children = &node->v.document.children;
  } else if (is_elementish(node)) {
    if (node->v.element.tag == tag) return node;
    children = &node->v.element.children;
  }
  if (children) {
    for (unsigned int i = 0; i < children->length; ++i) {
      GumboNode* found = find_first(children->data[i], tag);
      if (found) return found;
    }
  }
  return NULL;
}

static int count_tag(GumboNode* node, GumboTag tag) {
  int count = 0;
  GumboVector* children = NULL;
  if (node->type == GUMBO_NODE_DOCUMENT) {
    children = &node->v.document.children;
  } else if (is_elementish(node)) {
    count = node->v.element.tag == tag;
    children = &node->v.element.children;
  }
  if (children) {
    for (unsigned int i = 0; i < children->length; ++i) {
      count += count_tag(children->data[i], tag);
    }
  }
  return count;
}

static GumboNode* find_template(GumboNode* node) {
  GumboVector* children = NULL;
  if (node->type == GUMBO_NODE_DOCUMENT) {
    children = &node->v.document.children;
  } else if (is_elementish(node)) {
    if (node->type == GUMBO_NODE_TEMPLATE) return node;
    children = &node->v.element.children;
  }
  if (children) {
    for (unsigned int i = 0; i < children->length; ++i) {
      GumboNode* found = find_template(children->data[i]);
      if (found) return found;
    }
  }
  return NULL;
}

static const char* script_text(const GumboNode* script) {
  if (!is_elementish(script)) return "";
  const GumboVector* children = &script->v.element.children;
  if (!children->length) return "";
  const GumboNode* first = children->data[0];
  if (first->type != GUMBO_NODE_TEXT) return "";
  return first->v.text.text;
}

// ---------------------------------------------------------------------------
// Tree equality: structure, tags, namespaces, text, attributes, positions,
// parse flags, and the *contents* of original-text slices (the two parses own
// different buffers, so slices are compared by value, not by pointer).
// ---------------------------------------------------------------------------

static int positions_equal(GumboSourcePosition a, GumboSourcePosition b) {
  return a.offset == b.offset && a.line == b.line && a.column == b.column;
}

static int pieces_equal(GumboStringPiece a, GumboStringPiece b) {
  return a.length == b.length &&
         (!a.length || !memcmp(a.data, b.data, a.length));
}

static int nodes_equal(GumboNode* a, GumboNode* b) {
  if (a->type != b->type || a->parse_flags != b->parse_flags) return 0;
  if (a->type == GUMBO_NODE_DOCUMENT) {
    GumboVector* ca = &a->v.document.children;
    GumboVector* cb = &b->v.document.children;
    if (ca->length != cb->length) return 0;
    for (unsigned int i = 0; i < ca->length; ++i) {
      if (!nodes_equal(ca->data[i], cb->data[i])) return 0;
    }
    return 1;
  }
  if (is_elementish(a)) {
    GumboElement* ea = &a->v.element;
    GumboElement* eb = &b->v.element;
    if (ea->tag != eb->tag || ea->tag_namespace != eb->tag_namespace) return 0;
    if (!positions_equal(ea->start_pos, eb->start_pos) ||
        !positions_equal(ea->end_pos, eb->end_pos)) return 0;
    if (!pieces_equal(ea->original_tag, eb->original_tag) ||
        !pieces_equal(ea->original_end_tag, eb->original_end_tag)) return 0;
    if (ea->attributes.length != eb->attributes.length) return 0;
    for (unsigned int i = 0; i < ea->attributes.length; ++i) {
      GumboAttribute* aa = ea->attributes.data[i];
      GumboAttribute* ab = eb->attributes.data[i];
      if (strcmp(aa->name, ab->name) || strcmp(aa->value, ab->value)) return 0;
      if (aa->attr_namespace != ab->attr_namespace) return 0;
      if (!pieces_equal(aa->original_name, ab->original_name) ||
          !pieces_equal(aa->original_value, ab->original_value)) return 0;
      if (!positions_equal(aa->name_start, ab->name_start) ||
          !positions_equal(aa->value_start, ab->value_start)) return 0;
    }
    if (ea->children.length != eb->children.length) return 0;
    for (unsigned int i = 0; i < ea->children.length; ++i) {
      if (!nodes_equal(ea->children.data[i], eb->children.data[i])) return 0;
    }
    return 1;
  }
  return !strcmp(a->v.text.text, b->v.text.text) &&
         pieces_equal(a->v.text.original_text, b->v.text.original_text) &&
         positions_equal(a->v.text.start_pos, b->v.text.start_pos);
}

static int outputs_equal(GumboOutput* a, GumboOutput* b) {
  return nodes_equal(a->document, b->document) &&
         a->errors.length == b->errors.length;
}

// Computes the line/column that gumbo's UTF-8 iterator would report for the
// given byte offset: CRLF folds to one line break, tabs round up to the next
// multiple of 8, and columns count code points rather than bytes.
static void expected_position(const char* source, size_t target,
    unsigned int* line, unsigned int* column) {
  unsigned int l = 1, c = 1;
  size_t i = 0;
  while (i < target) {
    unsigned char ch = (unsigned char) source[i];
    if (ch == '\r') {
      ++i;
      if (i < target && source[i] == '\n') ++i;
      ++l;
      c = 1;
    } else if (ch == '\n') {
      ++i;
      ++l;
      c = 1;
    } else if (ch == '\t') {
      ++i;
      c = ((c / 8) + 1) * 8;
    } else {
      size_t width = 1;
      if (ch >= 0xF0) {
        width = 4;
      } else if (ch >= 0xE0) {
        width = 3;
      } else if (ch >= 0xC0) {
        width = 2;
      }
      i += width;
      ++c;
    }
  }
  *line = l;
  *column = c;
}

static int piece_in_source(const GumboReplayOutput* result, GumboStringPiece p) {
  return p.data >= result->source.data &&
         p.data + p.length <= result->source.data + result->source.length;
}

// ---------------------------------------------------------------------------
// Recording replay callback.  All outputs are served through one reused
// scratch buffer: if the library did not copy the returned bytes before
// resuming the parse, later callbacks would corrupt earlier insertions.
// ---------------------------------------------------------------------------

#define MAX_REPLAY_CALLS 8

typedef struct {
  const char* outputs[MAX_REPLAY_CALLS];
  int count;
  int use_scratch;
  char scratch[8192];
  // Observations recorded per call.
  char texts[MAX_REPLAY_CALLS][32];
  const GumboNode* nodes[MAX_REPLAY_CALLS];
  const GumboNode* parents[MAX_REPLAY_CALLS];
  int end_tag_ok[MAX_REPLAY_CALLS];
  int tr_count[MAX_REPLAY_CALLS];
  int td_count[MAX_REPLAY_CALLS];
  int p_count[MAX_REPLAY_CALLS];
  int i_count[MAX_REPLAY_CALLS];
  const GumboNode* body_node;
} ReplayLog;

static GumboStringPiece replay_callback(const GumboOutput* partial,
    const GumboNode* script, void* userdata) {
  ReplayLog* log = (ReplayLog*) userdata;
  int i = log->count++;
  log->nodes[i] = script;
  log->parents[i] = script->parent;
  snprintf(log->texts[i], sizeof log->texts[i], "%s", script_text(script));
  GumboStringPiece end = script->v.element.original_end_tag;
  log->end_tag_ok[i] = end.length == 9 && !memcmp(end.data, "</script>", 9);
  GumboNode* doc = (GumboNode*) partial->document;
  log->tr_count[i] = count_tag(doc, GUMBO_TAG_TR);
  log->td_count[i] = count_tag(doc, GUMBO_TAG_TD);
  log->p_count[i] = count_tag(doc, GUMBO_TAG_P);
  log->i_count[i] = count_tag(doc, GUMBO_TAG_I);
  if (!log->body_node) {
    log->body_node = find_first(doc, GUMBO_TAG_BODY);
  }
  const char* out = log->outputs[i];
  GumboStringPiece piece = {NULL, 0};
  if (!out) return piece;
  if (log->use_scratch) {
    size_t n = strlen(out);
    memcpy(log->scratch, out, n);
    piece.data = log->scratch;
    piece.length = n;
    return piece;
  }
  piece.data = out;
  piece.length = strlen(out);
  return piece;
}

static void log_init(ReplayLog* log, const char* out1, const char* out2) {
  memset(log, 0, sizeof *log);
  log->outputs[0] = out1;
  log->outputs[1] = out2;
  log->use_scratch = 1;
}

// Parses with replay and returns the result; also verifies the expanded
// source and (when reference_source is non-NULL) that the tree is identical
// to a plain one-shot parse of the expanded source.  The reference output is
// destroyed before returning.
static int check_against_reference(const GumboOptions* options,
    const GumboReplayOutput* result, const char* reference_source) {
  TASK_CHECK(result->source.length == strlen(reference_source));
  TASK_CHECK(!memcmp(result->source.data, reference_source, result->source.length));
  GumboOutput* reference = gumbo_parse_with_options(
      options, reference_source, strlen(reference_source));
  TASK_CHECK(outputs_equal(result->tree, reference));
  gumbo_destroy_output(options, reference);
  return 0;
}

// ---------------------------------------------------------------------------
// INSERTION group
// ---------------------------------------------------------------------------

// Nested replay: the outer script's output contains another script, whose own
// output must be spliced in and tree-built in the current table context.
static int insertion_nested_table_replay(void) {
  static const char input[] =
      "<!doctype html><table><script>outer()</script>"
      "<tr><td>orig</td></tr></table>";
  static const char expanded[] =
      "<!doctype html><table><script>outer()</script>"
      "<tr><td><script>inner()</script><span>cell</span></td></tr>"
      "<tr><td>orig</td></tr></table>";
  GumboOptions options = test_options();
  ReplayLog log;
  log_init(&log, "<tr><td><script>inner()</script></td></tr>",
      "<span>cell</span>");
  GumboReplayOutput* result = gumbo_parse_with_script_replay(
      &options, input, sizeof(input) - 1, replay_callback, &log);
  TASK_CHECK(result != NULL);

  // Callback order and partial-tree observations at each replay point.
  TASK_CHECK(log.count == 2);
  TASK_CHECK(!strcmp(log.texts[0], "outer()"));
  TASK_CHECK(!strcmp(log.texts[1], "inner()"));
  TASK_CHECK(log.tr_count[0] == 0 && log.td_count[0] == 0);
  TASK_CHECK(log.tr_count[1] == 1 && log.td_count[1] == 1);
  TASK_CHECK(log.end_tag_ok[0] && log.end_tag_ok[1]);

  // Final tree shape: both rows exist, the inner span landed inside the
  // first row's cell (current table context preserved, not foster-parented).
  GumboNode* doc = result->tree->document;
  TASK_CHECK(count_tag(doc, GUMBO_TAG_TR) == 2);
  TASK_CHECK(count_tag(doc, GUMBO_TAG_TD) == 2);
  TASK_CHECK(count_tag(doc, GUMBO_TAG_SCRIPT) == 2);
  GumboNode* span = find_first(doc, GUMBO_TAG_SPAN);
  TASK_CHECK(span != NULL);
  TASK_CHECK(span->parent != NULL && is_elementish(span->parent) &&
             span->parent->v.element.tag == GUMBO_TAG_TD);
  GumboNode* td = span->parent;
  TASK_CHECK(td->v.element.children.length == 2);
  GumboNode* first = td->v.element.children.data[0];
  TASK_CHECK(is_elementish(first) && first->v.element.tag == GUMBO_TAG_SCRIPT);
  TASK_CHECK(td->v.element.children.data[1] == span);
  TASK_CHECK(span->v.element.children.length == 1);
  GumboNode* span_text = span->v.element.children.data[0];
  TASK_CHECK(span_text->type == GUMBO_NODE_TEXT &&
             !strcmp(span_text->v.text.text, "cell"));

  TASK_CHECK(check_against_reference(&options, result, expanded) == 0);
  gumbo_destroy_replay_output(&options, result);
  TASK_CHECK(live_allocs == 0);
  return 0;
}

// The inserted output ends mid-tag and mid-entity; the junction must be
// tokenized as continuous text, not as separate fragments.
static int insertion_partial_tag_entity_junction(void) {
  static const char input[] =
      "<!doctype html><body><script>a()</script>card><script>b()</script>"
      "p;X</div>";
  static const char expanded[] =
      "<!doctype html><body><script>a()</script><div class=card>"
      "<script>b()</script>&amp;X</div>";
  GumboOptions options = test_options();
  ReplayLog log;
  log_init(&log, "<div class=", "&am");
  GumboReplayOutput* result = gumbo_parse_with_script_replay(
      &options, input, sizeof(input) - 1, replay_callback, &log);
  TASK_CHECK(result != NULL);
  TASK_CHECK(log.count == 2);
  TASK_CHECK(!strcmp(log.texts[0], "a()"));
  TASK_CHECK(!strcmp(log.texts[1], "b()"));

  GumboNode* doc = result->tree->document;
  GumboNode* div = find_first(doc, GUMBO_TAG_DIV);
  TASK_CHECK(div != NULL);
  // The tag was lexed as one token spanning the junction.
  TASK_CHECK(div->v.element.original_tag.length == 16 &&
             !memcmp(div->v.element.original_tag.data, "<div class=card>", 16));
  TASK_CHECK(piece_in_source(result, div->v.element.original_tag));
  GumboAttribute* attr = gumbo_get_attribute(&div->v.element.attributes, "class");
  TASK_CHECK(attr != NULL);
  TASK_CHECK(!strcmp(attr->value, "card"));
  // The <div> start tag begins right after the first script's end tag.
  const char* div_at = strstr(expanded, "<div class=");
  TASK_CHECK(div->v.element.start_pos.offset ==
             (unsigned int) (div_at - expanded));
  // The entity crossed the junction and decoded to '&'.
  TASK_CHECK(div->v.element.children.length == 2);
  GumboNode* text = div->v.element.children.data[1];
  TASK_CHECK(text->type == GUMBO_NODE_TEXT && !strcmp(text->v.text.text, "&X"));

  TASK_CHECK(check_against_reference(&options, result, expanded) == 0);
  gumbo_destroy_replay_output(&options, result);
  TASK_CHECK(live_allocs == 0);
  return 0;
}

// A multi-byte UTF-8 sequence and a CRLF pair each split across the junction
// must decode as if the bytes had always been adjacent.
static int insertion_crlf_utf8_junction(void) {
  static const char input[] =
      "<!doctype html><body><script>c()</script>\x86\x85<script>d()</script>"
      "\n</em>";
  static const char expanded[] =
      "<!doctype html><body><script>c()</script><em>\xE5\x86\x85"
      "<script>d()</script>\r\n</em>";
  GumboOptions options = test_options();
  ReplayLog log;
  log_init(&log, "<em>\xE5", "\r");
  GumboReplayOutput* result = gumbo_parse_with_script_replay(
      &options, input, sizeof(input) - 1, replay_callback, &log);
  TASK_CHECK(result != NULL);
  TASK_CHECK(log.count == 2);

  GumboNode* em = find_first(result->tree->document, GUMBO_TAG_EM);
  TASK_CHECK(em != NULL);
  TASK_CHECK(em->v.element.children.length == 3);
  // U+5185 ('内') reassembled from bytes split across the junction.
  GumboNode* hanzi = em->v.element.children.data[0];
  TASK_CHECK(hanzi->type == GUMBO_NODE_TEXT &&
             !strcmp(hanzi->v.text.text, "\xE5\x86\x85"));
  GumboNode* inner_script = em->v.element.children.data[1];
  TASK_CHECK(is_elementish(inner_script) &&
             inner_script->v.element.tag == GUMBO_TAG_SCRIPT);
  // The CRLF pair spanning the junction collapsed to a single LF in the text.
  // Per the library's CR/LF rules (verified against a plain parse of the
  // expanded source), the text node's original text covers just the \n byte,
  // with the \r belonging to the preceding token gap.
  GumboNode* newline = em->v.element.children.data[2];
  TASK_CHECK(newline->type == GUMBO_NODE_WHITESPACE &&
             !strcmp(newline->v.text.text, "\n"));
  TASK_CHECK(newline->v.text.original_text.length == 1);
  TASK_CHECK(piece_in_source(result, newline->v.text.original_text));
  TASK_CHECK(newline->v.text.original_text.data[0] == '\n');
  TASK_CHECK(newline->v.text.original_text.data[-1] == '\r');
  TASK_CHECK(newline->v.text.start_pos.offset ==
             (unsigned int) (newline->v.text.original_text.data -
                             result->source.data));
  TASK_CHECK(em->v.element.original_end_tag.length == 5 &&
             !memcmp(em->v.element.original_end_tag.data, "</em>", 5));

  TASK_CHECK(check_against_reference(&options, result, expanded) == 0);
  gumbo_destroy_replay_output(&options, result);
  TASK_CHECK(live_allocs == 0);
  return 0;
}

// The replay point sits right before a CR/LF pair: the output must be
// spliced before the \r (the pair is one logical character), and positions
// after it must match a plain parse of the expanded source.
static int insertion_before_crlf_pair(void) {
  static const char input[] =
      "<!doctype html><body><script>s()</script>\r\n<p id=z>x</p>";
  static const char expanded[] =
      "<!doctype html><body><script>s()</script><em>e</em>\r\n<p id=z>x</p>";
  GumboOptions options = test_options();
  ReplayLog log;
  log_init(&log, "<em>e</em>", NULL);
  GumboReplayOutput* result = gumbo_parse_with_script_replay(
      &options, input, sizeof(input) - 1, replay_callback, &log);
  TASK_CHECK(result != NULL);
  TASK_CHECK(log.count == 1);
  TASK_CHECK(result->source.length == sizeof(expanded) - 1);
  TASK_CHECK(!memcmp(result->source.data, expanded, sizeof(expanded) - 1));

  GumboNode* doc = result->tree->document;
  GumboNode* em = find_first(doc, GUMBO_TAG_EM);
  TASK_CHECK(em != NULL);
  // The folded newline survived the splice as a single LF text node.
  GumboNode* body = find_first(doc, GUMBO_TAG_BODY);
  TASK_CHECK(body != NULL && body->v.element.children.length == 4);
  GumboNode* newline = body->v.element.children.data[2];
  TASK_CHECK(newline->type == GUMBO_NODE_WHITESPACE &&
             !strcmp(newline->v.text.text, "\n"));
  // The <p> starts on line 2, exactly where the expanded source puts it.
  GumboNode* p = find_first(doc, GUMBO_TAG_P);
  TASK_CHECK(p != NULL);
  const char* p_at = strstr(expanded, "<p id=z>");
  unsigned int line, column;
  expected_position(expanded, p_at - expanded, &line, &column);
  TASK_CHECK(p->v.element.start_pos.offset ==
             (unsigned int) (p_at - expanded));
  TASK_CHECK(p->v.element.start_pos.line == line && line == 2);
  TASK_CHECK(p->v.element.start_pos.column == column && column == 1);

  TASK_CHECK(check_against_reference(&options, result, expanded) == 0);
  gumbo_destroy_replay_output(&options, result);
  TASK_CHECK(live_allocs == 0);
  return 0;
}

// ---------------------------------------------------------------------------
// CONTEXT group
// ---------------------------------------------------------------------------

// A script closed by a real end tag replays; the callback sees the popped
// script with complete text and end tag before the following markup is
// consumed.  A script only closed by EOF stays inert.
static int context_real_close_timing_and_eof(void) {
  static const char input[] =
      "<!doctype html><body><script>one()</script><p>after</p>";
  static const char expanded[] =
      "<!doctype html><body><script>one()</script><i>x</i><p>after</p>";
  GumboOptions options = test_options();
  ReplayLog log;
  log_init(&log, "<i>x</i>", NULL);
  GumboReplayOutput* result = gumbo_parse_with_script_replay(
      &options, input, sizeof(input) - 1, replay_callback, &log);
  TASK_CHECK(result != NULL);
  TASK_CHECK(log.count == 1);
  TASK_CHECK(!strcmp(log.texts[0], "one()"));
  // The script was already popped (its parent is body), its end tag was
  // complete, and neither the following <p> nor the replayed <i> had been
  // consumed yet.
  TASK_CHECK(log.parents[0] != NULL && is_elementish(log.parents[0]) &&
             log.parents[0]->v.element.tag == GUMBO_TAG_BODY);
  TASK_CHECK(log.end_tag_ok[0]);
  TASK_CHECK(log.p_count[0] == 0);
  TASK_CHECK(log.i_count[0] == 0);

  // Body children end up in source order: script, replayed <i>, then <p>.
  GumboNode* body = find_first(result->tree->document, GUMBO_TAG_BODY);
  TASK_CHECK(body != NULL && body->v.element.children.length == 3);
  GumboNode* c0 = body->v.element.children.data[0];
  GumboNode* c1 = body->v.element.children.data[1];
  GumboNode* c2 = body->v.element.children.data[2];
  TASK_CHECK(c0->v.element.tag == GUMBO_TAG_SCRIPT);
  TASK_CHECK(c1->v.element.tag == GUMBO_TAG_I);
  TASK_CHECK(c2->v.element.tag == GUMBO_TAG_P);
  TASK_CHECK(c0->v.element.original_end_tag.length == 9 &&
             !memcmp(c0->v.element.original_end_tag.data, "</script>", 9));
  TASK_CHECK(piece_in_source(result, c0->v.element.original_end_tag));

  TASK_CHECK(check_against_reference(&options, result, expanded) == 0);
  gumbo_destroy_replay_output(&options, result);
  TASK_CHECK(live_allocs == 0);

  // EOF-forced close: no end tag, no replay, script still in the tree.
  static const char eof_input[] = "<!doctype html><body><script>two()";
  ReplayLog eof_log;
  log_init(&eof_log, "<i>y</i>", NULL);
  result = gumbo_parse_with_script_replay(
      &options, eof_input, sizeof(eof_input) - 1, replay_callback, &eof_log);
  TASK_CHECK(result != NULL);
  TASK_CHECK(eof_log.count == 0);
  GumboNode* script = find_first(result->tree->document, GUMBO_TAG_SCRIPT);
  TASK_CHECK(script != NULL);
  TASK_CHECK(!strcmp(script_text(script), "two()"));
  TASK_CHECK(script->parse_flags & GUMBO_INSERTION_IMPLICIT_END_TAG);
  TASK_CHECK(count_tag(result->tree->document, GUMBO_TAG_I) == 0);
  TASK_CHECK(result->source.length == sizeof(eof_input) - 1 &&
             !memcmp(result->source.data, eof_input, result->source.length));
  gumbo_destroy_replay_output(&options, result);
  TASK_CHECK(live_allocs == 0);
  return 0;
}

// Scripts in template contents, in foreign namespaces, and in fragment parses
// stay inert; a script regenerated inside an HTML integration point does
// replay (no template ancestor).
static int context_lazy_contexts(void) {
  GumboOptions options = test_options();
  ReplayLog log;

  // Template contents.
  static const char tpl[] =
      "<!doctype html><body><template><script>t()</script></template></body>";
  log_init(&log, "<span>tpl</span>", NULL);
  GumboReplayOutput* result = gumbo_parse_with_script_replay(
      &options, tpl, sizeof(tpl) - 1, replay_callback, &log);
  TASK_CHECK(result != NULL);
  TASK_CHECK(log.count == 0);
  GumboNode* template_node = find_template(result->tree->document);
  TASK_CHECK(template_node != NULL);
  TASK_CHECK(find_first(template_node, GUMBO_TAG_SCRIPT) != NULL);
  TASK_CHECK(count_tag(result->tree->document, GUMBO_TAG_SPAN) == 0);
  gumbo_destroy_replay_output(&options, result);

  // Foreign (SVG) namespace script.
  static const char svg[] =
      "<!doctype html><body><svg><script>s()</script></svg></body>";
  log_init(&log, "<span>svg</span>", NULL);
  result = gumbo_parse_with_script_replay(
      &options, svg, sizeof(svg) - 1, replay_callback, &log);
  TASK_CHECK(result != NULL);
  TASK_CHECK(log.count == 0);
  GumboNode* script = find_first(result->tree->document, GUMBO_TAG_SCRIPT);
  TASK_CHECK(script != NULL);
  TASK_CHECK(script->v.element.tag_namespace == GUMBO_NAMESPACE_SVG);
  TASK_CHECK(count_tag(result->tree->document, GUMBO_TAG_SPAN) == 0);
  gumbo_destroy_replay_output(&options, result);

  // Fragment parse: no replay at all, but the source is still owned.
  static const char frag[] = "<script>f()</script><p>x</p>";
  GumboOptions frag_options = test_options();
  frag_options.fragment_context = GUMBO_TAG_DIV;
  log_init(&log, "<span>frag</span>", NULL);
  result = gumbo_parse_with_script_replay(
      &frag_options, frag, sizeof(frag) - 1, replay_callback, &log);
  TASK_CHECK(result != NULL);
  TASK_CHECK(log.count == 0);
  TASK_CHECK(count_tag(result->tree->document, GUMBO_TAG_SCRIPT) == 1);
  TASK_CHECK(count_tag(result->tree->document, GUMBO_TAG_SPAN) == 0);
  TASK_CHECK(result->source.length == sizeof(frag) - 1 &&
             !memcmp(result->source.data, frag, result->source.length));
  gumbo_destroy_replay_output(&frag_options, result);

  // HTML integration point inside SVG: the script is HTML again and replays.
  static const char integration[] =
      "<!doctype html><body><svg><foreignObject><script>ok()</script>"
      "</foreignObject></svg></body>";
  log_init(&log, "<b>w</b>", NULL);
  result = gumbo_parse_with_script_replay(
      &options, integration, sizeof(integration) - 1, replay_callback, &log);
  TASK_CHECK(result != NULL);
  TASK_CHECK(log.count == 1);
  TASK_CHECK(log.nodes[0]->v.element.tag_namespace == GUMBO_NAMESPACE_HTML);
  GumboNode* bold = find_first(result->tree->document, GUMBO_TAG_B);
  TASK_CHECK(bold != NULL);
  TASK_CHECK(bold->parent != NULL && is_elementish(bold->parent) &&
             bold->parent->v.element.tag == GUMBO_TAG_FOREIGNOBJECT);
  gumbo_destroy_replay_output(&options, result);

  // Attributes (src/type/async/defer) are not inspected: every HTML script
  // closed by a real end tag replays.
  static const char attrs[] =
      "<!doctype html><body><script src=x.js type=text/javascript async>"
      "a()</script>";
  log_init(&log, "<b>a</b>", NULL);
  result = gumbo_parse_with_script_replay(
      &options, attrs, sizeof(attrs) - 1, replay_callback, &log);
  TASK_CHECK(result != NULL);
  TASK_CHECK(log.count == 1);
  TASK_CHECK(count_tag(result->tree->document, GUMBO_TAG_B) == 1);
  gumbo_destroy_replay_output(&options, result);
  TASK_CHECK(live_allocs == 0);
  return 0;
}

// Insertion must not disturb the surrounding tree-construction state: foster
// parenting of table-stray text, the open-form pointer, and active formatting
// elements all survive the splice.
static int context_table_form_formatting_state(void) {
  GumboOptions options = test_options();

  // Foster parenting + form pointer.
  static const char table_input[] =
      "<!doctype html><body><form id=outer><table><script>s()</script>"
      "<tr><td>a</td></tr></table><form id=inner></form></form>";
  static const char table_expanded[] =
      "<!doctype html><body><form id=outer><table><script>s()</script>loose"
      "<tr><td>a</td></tr></table><form id=inner></form></form>";
  ReplayLog log;
  log_init(&log, "loose", NULL);
  GumboReplayOutput* result = gumbo_parse_with_script_replay(
      &options, table_input, sizeof(table_input) - 1, replay_callback, &log);
  TASK_CHECK(result != NULL);
  TASK_CHECK(log.count == 1);
  GumboNode* doc = result->tree->document;
  // The second <form> start tag is ignored because the form element pointer
  // was still set across the splice.
  TASK_CHECK(count_tag(doc, GUMBO_TAG_FORM) == 1);
  GumboNode* form = find_first(doc, GUMBO_TAG_FORM);
  TASK_CHECK(form != NULL);
  // "loose" was foster-parented out of the table to just before it.
  TASK_CHECK(form->v.element.children.length == 2);
  GumboNode* loose = form->v.element.children.data[0];
  TASK_CHECK(loose->type == GUMBO_NODE_TEXT &&
             !strcmp(loose->v.text.text, "loose"));
  GumboNode* table = form->v.element.children.data[1];
  TASK_CHECK(is_elementish(table) && table->v.element.tag == GUMBO_TAG_TABLE);
  GumboNode* td = find_first(doc, GUMBO_TAG_TD);
  TASK_CHECK(td != NULL && td->v.element.children.length == 1);
  TASK_CHECK(check_against_reference(&options, result, table_expanded) == 0);
  gumbo_destroy_replay_output(&options, result);

  // Active formatting elements: <b> stays open across the splice and the
  // replayed <u> plus the trailing text nest inside it.
  static const char fmt_input[] =
      "<!doctype html><body><b><script>s()</script>tail</b>";
  static const char fmt_expanded[] =
      "<!doctype html><body><b><script>s()</script><u>u</u>tail</b>";
  log_init(&log, "<u>u</u>", NULL);
  result = gumbo_parse_with_script_replay(
      &options, fmt_input, sizeof(fmt_input) - 1, replay_callback, &log);
  TASK_CHECK(result != NULL);
  TASK_CHECK(log.count == 1);
  GumboNode* bold = find_first(result->tree->document, GUMBO_TAG_B);
  TASK_CHECK(bold != NULL && bold->v.element.children.length == 3);
  GumboNode* u = bold->v.element.children.data[1];
  TASK_CHECK(is_elementish(u) && u->v.element.tag == GUMBO_TAG_U);
  GumboNode* tail = bold->v.element.children.data[2];
  TASK_CHECK(tail->type == GUMBO_NODE_TEXT &&
             !strcmp(tail->v.text.text, "tail"));
  TASK_CHECK(check_against_reference(&options, result, fmt_expanded) == 0);
  gumbo_destroy_replay_output(&options, result);
  TASK_CHECK(live_allocs == 0);
  return 0;
}

// ---------------------------------------------------------------------------
// SOURCE group
// ---------------------------------------------------------------------------

// Positions, slices, and error records are all expressed against the expanded
// source, including across newlines and an invalid byte inside the replay
// output.
static int source_expanded_positions_and_errors(void) {
  static const char input[] =
      "<!doctype html>\n<body>\n<script>s()</script>\r\n<p id=tail>x</p>";
  static const char expanded[] =
      "<!doctype html>\n<body>\n<script>s()</script><q>\xFF</q>\n\r\n"
      "<p id=tail>x</p>";
  GumboOptions options = test_options();
  ReplayLog log;
  log_init(&log, "<q>\xFF</q>\n", NULL);
  GumboReplayOutput* result = gumbo_parse_with_script_replay(
      &options, input, sizeof(input) - 1, replay_callback, &log);
  TASK_CHECK(result != NULL);
  TASK_CHECK(log.count == 1);
  TASK_CHECK(result->source.length == sizeof(expanded) - 1);
  TASK_CHECK(!memcmp(result->source.data, expanded, sizeof(expanded) - 1));

  // The <p> element's position is counted in the expanded source.
  GumboNode* p = find_first(result->tree->document, GUMBO_TAG_P);
  TASK_CHECK(p != NULL);
  const char* p_at = strstr(expanded, "<p id=tail>");
  size_t p_offset = p_at - expanded;
  unsigned int line, column;
  expected_position(expanded, p_offset, &line, &column);
  TASK_CHECK(p->v.element.start_pos.offset == (unsigned int) p_offset);
  TASK_CHECK(p->v.element.start_pos.line == line);
  TASK_CHECK(p->v.element.start_pos.column == column);
  TASK_CHECK(line == 5 && column == 1);
  TASK_CHECK(p->v.element.original_tag.length == 11 &&
             !memcmp(p->v.element.original_tag.data, "<p id=tail>", 11));
  TASK_CHECK(piece_in_source(result, p->v.element.original_tag));

  // The invalid byte inside the replayed output was replaced and reported
  // against the expanded source.
  GumboNode* q = find_first(result->tree->document, GUMBO_TAG_Q);
  TASK_CHECK(q != NULL && q->v.element.children.length == 1);
  GumboNode* bad = q->v.element.children.data[0];
  TASK_CHECK(bad->type == GUMBO_NODE_TEXT &&
             !strcmp(bad->v.text.text, "\xEF\xBF\xBD"));
  TASK_CHECK(result->tree->errors.length >= 1);
  GumboError* utf8_error = NULL;
  for (unsigned int i = 0; i < result->tree->errors.length; ++i) {
    GumboError* error = result->tree->errors.data[i];
    TASK_CHECK(error->original_text >= result->source.data &&
               error->original_text <
                   result->source.data + result->source.length);
    if (error->type == GUMBO_ERR_UTF8_INVALID) utf8_error = error;
  }
  TASK_CHECK(utf8_error != NULL);
  TASK_CHECK(*utf8_error->original_text == (char) 0xFF);
  const char* bad_at = (const char*) memchr(expanded, 0xFF, sizeof(expanded) - 1);
  TASK_CHECK(bad_at != NULL);
  TASK_CHECK(utf8_error->original_text == result->source.data + (bad_at - expanded));
  TASK_CHECK(utf8_error->position.offset == (unsigned int) (bad_at - expanded));

  TASK_CHECK(check_against_reference(&options, result, expanded) == 0);
  gumbo_destroy_replay_output(&options, result);
  TASK_CHECK(live_allocs == 0);
  return 0;
}

// Large outputs force the input buffer through several generations; slices
// recorded in earlier generations must be remapped to the final source.
static int source_growth_remapped_slices(void) {
  static const char part1[] =
      "<!doctype html><body class=top><script>a()</script>";
  static const char part2[] = "<script>b()</script>";
  static const char part3[] = "<p id=z>end</p>";
  static char big1[2048];
  static char big2[2048];
  big1[0] = big2[0] = '\0';
  for (int i = 0; i < 200; ++i) strcat(big1, "<i>x</i>");
  for (int i = 0; i < 150; ++i) strcat(big2, "<u>y</u>");

  size_t input_length =
      strlen(part1) + strlen(part2) + strlen(part3);
  char* input = malloc(input_length + 1);
  char* expected = malloc(input_length + strlen(big1) + strlen(big2) + 1);
  TASK_CHECK(input != NULL && expected != NULL);
  snprintf(input, input_length + 1, "%s%s%s", part1, part2, part3);
  snprintf(expected, input_length + strlen(big1) + strlen(big2) + 1,
      "%s%s%s%s%s", part1, big1, part2, big2, part3);

  GumboOptions options = test_options();
  ReplayLog log;
  log_init(&log, big1, big2);
  GumboReplayOutput* result =
      gumbo_parse_with_script_replay(&options, input, input_length,
          replay_callback, &log);
  TASK_CHECK(result != NULL);
  TASK_CHECK(log.count == 2);
  TASK_CHECK(result->source.length == strlen(expected));
  TASK_CHECK(!memcmp(result->source.data, expected, result->source.length));

  GumboNode* doc = result->tree->document;
  TASK_CHECK(count_tag(doc, GUMBO_TAG_I) == 200);
  TASK_CHECK(count_tag(doc, GUMBO_TAG_U) == 150);

  // The first script's slices were recorded in the original (now retired)
  // buffer generation; they must point into the final source.
  GumboNode* script_a = find_first(doc, GUMBO_TAG_SCRIPT);
  TASK_CHECK(script_a != NULL);
  TASK_CHECK(!strcmp(script_text(script_a), "a()"));
  TASK_CHECK(script_a->v.element.original_tag.length == 8 &&
             !memcmp(script_a->v.element.original_tag.data, "<script>", 8));
  TASK_CHECK(piece_in_source(result, script_a->v.element.original_tag));
  TASK_CHECK(piece_in_source(result, script_a->v.element.original_end_tag));

  // Attribute slices recorded before any growth are remapped too.
  GumboNode* body = find_first(doc, GUMBO_TAG_BODY);
  TASK_CHECK(body != NULL);
  GumboAttribute* attr = gumbo_get_attribute(&body->v.element.attributes, "class");
  TASK_CHECK(attr != NULL && !strcmp(attr->value, "top"));
  TASK_CHECK(attr->original_name.length == 5 &&
             !memcmp(attr->original_name.data, "class", 5));
  TASK_CHECK(attr->original_value.length == 3 &&
             !memcmp(attr->original_value.data, "top", 3));
  TASK_CHECK(piece_in_source(result, attr->original_name));
  TASK_CHECK(piece_in_source(result, attr->original_value));

  // Positions after all insertions count the fully expanded source.
  GumboNode* p = find_first(doc, GUMBO_TAG_P);
  TASK_CHECK(p != NULL);
  const char* p_at = strstr(expected, "<p id=z>");
  TASK_CHECK(p->v.element.start_pos.offset ==
             (unsigned int) (p_at - expected));

  TASK_CHECK(check_against_reference(&options, result, expected) == 0);
  gumbo_destroy_replay_output(&options, result);
  free(input);
  free(expected);
  TASK_CHECK(live_allocs == 0);
  return 0;
}

// Node addresses captured during callbacks stay valid in the final tree; the
// result outlives the original input and the callback's output buffers; a
// NULL replay and an empty-output replay both match a plain parse.
static int source_identity_lifetime_and_control(void) {
  static const char input_text[] =
      "<!doctype html><body><script>id()</script><p>after</p>";
  static const char expanded[] =
      "<!doctype html><body><script>id()</script><div>gen</div><p>after</p>";
  GumboOptions options = test_options();

  // The input and the callback output both live in heap buffers that are
  // released right after the parse returns.
  char* input = malloc(sizeof(input_text));
  memcpy(input, input_text, sizeof(input_text));
  char* output = malloc(32);
  strcpy(output, "<div>gen</div>");
  ReplayLog log;
  log_init(&log, output, NULL);
  log.use_scratch = 0;
  GumboReplayOutput* result = gumbo_parse_with_script_replay(
      &options, input, sizeof(input_text) - 1, replay_callback, &log);
  TASK_CHECK(result != NULL);
  TASK_CHECK(log.count == 1);
  const GumboNode* script_during = log.nodes[0];
  const GumboNode* body_during = log.body_node;
  TASK_CHECK(script_during != NULL && body_during != NULL);
  free(input);
  free(output);

  // The result is self-contained after the source buffers are gone.
  TASK_CHECK(result->source.length == sizeof(expanded) - 1);
  TASK_CHECK(!memcmp(result->source.data, expanded, sizeof(expanded) - 1));
  GumboNode* doc = result->tree->document;
  TASK_CHECK(find_first(doc, GUMBO_TAG_SCRIPT) == script_during);
  TASK_CHECK(find_first(doc, GUMBO_TAG_BODY) == body_during);
  TASK_CHECK(!strcmp(script_text(find_first(doc, GUMBO_TAG_SCRIPT)), "id()"));
  GumboNode* div = find_first(doc, GUMBO_TAG_DIV);
  TASK_CHECK(div != NULL && div->v.element.children.length == 1);
  TASK_CHECK(!strcmp(
      ((GumboNode*) div->v.element.children.data[0])->v.text.text, "gen"));
  TASK_CHECK(check_against_reference(&options, result, expanded) == 0);
  gumbo_destroy_replay_output(&options, result);
  TASK_CHECK(live_allocs == 0);

  // NULL replay: plain parse, but the result still owns a copy of the input.
  GumboReplayOutput* null_result = gumbo_parse_with_script_replay(
      &options, input_text, sizeof(input_text) - 1, NULL, NULL);
  TASK_CHECK(null_result != NULL);
  TASK_CHECK(null_result->source.length == sizeof(input_text) - 1);
  TASK_CHECK(!memcmp(null_result->source.data, input_text,
             null_result->source.length));
  TASK_CHECK(null_result->source.data != input_text);
  GumboOutput* plain = gumbo_parse_with_options(
      &options, input_text, sizeof(input_text) - 1);
  TASK_CHECK(outputs_equal(null_result->tree, plain));

  // An empty-output callback produces the same tree as no replay at all.
  ReplayLog empty_log;
  log_init(&empty_log, NULL, NULL);
  GumboReplayOutput* empty_result = gumbo_parse_with_script_replay(
      &options, input_text, sizeof(input_text) - 1, replay_callback,
      &empty_log);
  TASK_CHECK(empty_result != NULL);
  TASK_CHECK(empty_log.count == 1);
  TASK_CHECK(outputs_equal(empty_result->tree, plain));
  TASK_CHECK(outputs_equal(empty_result->tree, null_result->tree));

  gumbo_destroy_output(&options, plain);
  gumbo_destroy_replay_output(&options, null_result);
  gumbo_destroy_replay_output(&options, empty_result);
  TASK_CHECK(live_allocs == 0);
  return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

// A callback that runs an independent plain parse of its own inside the
// replay callback, to prove parse sessions stay isolated.
static int reentrant_parse_ok;

static GumboStringPiece reentrant_replay(const GumboOutput* partial,
    const GumboNode* script, void* userdata) {
  (void) partial;
  (void) script;
  GumboOptions* options = (GumboOptions*) userdata;
  static const char inner[] = "<!doctype html><p>inner</p>";
  GumboOutput* inner_tree =
      gumbo_parse_with_options(options, inner, sizeof(inner) - 1);
  reentrant_parse_ok =
      inner_tree && count_tag(inner_tree->document, GUMBO_TAG_P) == 1;
  if (inner_tree) gumbo_destroy_output(options, inner_tree);
  GumboStringPiece out = {"<em>ok</em>", 8};
  return out;
}

static int source_reentrant_plain_parse(void) {
  static const char input[] =
      "<!doctype html><body><script>r()</script><p>tail</p>";
  GumboOptions options = test_options();
  reentrant_parse_ok = 0;
  GumboReplayOutput* result = gumbo_parse_with_script_replay(
      &options, input, sizeof(input) - 1, reentrant_replay, &options);
  TASK_CHECK(result != NULL);
  TASK_CHECK(reentrant_parse_ok);
  GumboNode* doc = result->tree->document;
  TASK_CHECK(count_tag(doc, GUMBO_TAG_EM) == 1);
  TASK_CHECK(count_tag(doc, GUMBO_TAG_P) == 1);
  gumbo_destroy_replay_output(&options, result);
  TASK_CHECK(live_allocs == 0);
  return 0;
}

const struct task_case task_cases[] = {
    {"INSERTION", "nested_table_replay", insertion_nested_table_replay},
    {"INSERTION", "partial_tag_entity_junction",
        insertion_partial_tag_entity_junction},
    {"INSERTION", "crlf_utf8_junction", insertion_crlf_utf8_junction},
    {"INSERTION", "before_crlf_pair", insertion_before_crlf_pair},
    {"CONTEXT", "real_close_timing_and_eof", context_real_close_timing_and_eof},
    {"CONTEXT", "lazy_contexts", context_lazy_contexts},
    {"CONTEXT", "table_form_formatting_state",
        context_table_form_formatting_state},
    {"SOURCE", "expanded_positions_and_errors",
        source_expanded_positions_and_errors},
    {"SOURCE", "growth_remapped_slices", source_growth_remapped_slices},
    {"SOURCE", "identity_lifetime_and_control",
        source_identity_lifetime_and_control},
    {"SOURCE", "reentrant_plain_parse", source_reentrant_plain_parse},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
