// Copyright 2016 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Author: jdtang@google.com (Jonathan Tang)
//
// Contract tests for the optional text-index / quote-location API.  This file
// has no main(): harness/main.c drives text_anchor_contract() and checks that
// each of the four scenario groups records at least one case.

#include "text_anchor.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "case.h"
#include "gumbo.h"

// UTF-8 encodings used by the two-codepoint reference &acE; (U+223E U+0333).
static const char kSquiggle[] = "\xe2\x88\xbe";  // U+223E
static const char kCombining[] = "\xcc\xb3";     // U+0333
static const char kNbsp[] = "\xc2\xa0";          // U+00A0
static const char kReplacement[] = "\xef\xbf\xbd";  // U+FFFD

typedef struct {
  GumboOptions options;
  GumboOutput* output;
  GumboTextIndex* index;
  const char* html;
} Context;

static size_t byte_offset(const char* haystack, const char* needle) {
  const char* found = strstr(haystack, needle);
  assert(found != NULL);
  return (size_t)(found - haystack);
}

static Context make_context(const char* html) {
  Context ctx;
  ctx.options = kGumboDefaultOptions;
  ctx.options.track_text_sources = true;
  ctx.html = html;
  ctx.output =
      gumbo_parse_with_options(&ctx.options, html, strlen(html));
  assert(ctx.output != NULL);
  ctx.index =
      gumbo_text_index_build(&ctx.options, ctx.output, html, strlen(html));
  assert(ctx.index != NULL);
  return ctx;
}

static void destroy_context(Context* ctx) {
  gumbo_text_index_destroy(ctx->index);
  gumbo_destroy_output(&ctx->options, ctx->output);
}

static GumboTextQuoteResults* must_find(const GumboTextIndex* index,
    const char* exact, const char* prefix, const char* suffix) {
  GumboTextQuoteResults* results = NULL;
  GumboTextQuoteStatus status = gumbo_text_quote_find(index, exact,
      strlen(exact), prefix, prefix ? strlen(prefix) : 0, suffix,
      suffix ? strlen(suffix) : 0, &results);
  assert(status == GUMBO_TEXT_QUOTE_OK);
  assert(results != NULL);
  return results;
}

// Scenario group "mapping": ordinary CJK text, a two-codepoint character
// reference and a CRLF line ending must each map partial hits back to the
// exact original bytes that produced them.
static void test_mapping(void) {
  // --- Ordinary multi-byte UTF-8 (Chinese) with partial hits. ---
  {
    const char* html = "<p>中文</p>";
    Context ctx = make_context(html);
    assert(gumbo_text_index_normalized_length(ctx.index) == 6);
    assert(memcmp(gumbo_text_index_normalized_text(ctx.index), "中文", 6) ==
           0);
    assert(gumbo_text_index_scalar_count(ctx.index) == 2);

    // Partial hit on the second scalar only.
    GumboTextQuoteResults* r = must_find(ctx.index, "文", NULL, NULL);
    assert(r->count == 1);
    GumboTextQuote* q = &r->quotes[0];
    assert(q->match_start == 3 && q->match_end == 6);
    assert(q->span_count == 1);
    size_t wen = byte_offset(html, "文");
    assert(q->spans[0].source_start == wen);
    assert(q->spans[0].source_end == wen + 3);
    assert(q->spans[0].node != NULL);
    gumbo_text_quote_results_destroy(r);

    // Whole word: the two adjacent scalars live in one node and touch, so the
    // fragments merge into a single span covering both characters.
    r = must_find(ctx.index, "中文", NULL, NULL);
    assert(r->count == 1);
    assert(r->quotes[0].span_count == 1);
    size_t zhong = byte_offset(html, "中");
    assert(r->quotes[0].spans[0].source_start == zhong);
    assert(r->quotes[0].spans[0].source_end == wen + 3);
    gumbo_text_quote_results_destroy(r);
    destroy_context(&ctx);
  }

  // --- Two-codepoint named reference: both scalars cite the whole ref. ---
  {
    const char* html = "<p>a&acE;b</p>";
    Context ctx = make_context(html);
    assert(gumbo_text_index_scalar_count(ctx.index) == 4);
    size_t ref = byte_offset(html, "&acE;");

    size_t n0 = 0, n1 = 0;
    const GumboTextSourceSpan* s1 =
        gumbo_text_index_scalar_spans(ctx.index, 1, &n1);
    const GumboTextSourceSpan* s2 =
        gumbo_text_index_scalar_spans(ctx.index, 2, &n0);
    assert(n1 == 1 && n0 == 1);
    assert(s1[0].source_start == ref && s1[0].source_end == ref + 5);
    assert(s2[0].source_start == ref && s2[0].source_end == ref + 5);

    // A partial hit on either decoded scalar still quotes the entire entity.
    GumboTextQuoteResults* r = must_find(ctx.index, kSquiggle, NULL, NULL);
    assert(r->count == 1 && r->quotes[0].span_count == 1);
    assert(r->quotes[0].spans[0].source_start == ref);
    assert(r->quotes[0].spans[0].source_end == ref + 5);
    gumbo_text_quote_results_destroy(r);

    r = must_find(ctx.index, kCombining, NULL, NULL);
    assert(r->count == 1 && r->quotes[0].span_count == 1);
    assert(r->quotes[0].spans[0].source_end == ref + 5);
    gumbo_text_quote_results_destroy(r);

    // The literal 'b' after the entity maps to the byte right after it.
    r = must_find(ctx.index, "b", NULL, NULL);
    assert(r->count == 1);
    assert(r->quotes[0].spans[0].source_start == ref + 5);
    gumbo_text_quote_results_destroy(r);
    destroy_context(&ctx);
  }

  // --- CRLF normalized to one line feed that covers both input bytes. ---
  {
    const char* html = "<p>a\r\nb</p>";
    Context ctx = make_context(html);
    assert(memcmp(gumbo_text_index_normalized_text(ctx.index), "a b", 3) ==
           0);
    size_t space_spans = 99;
    const GumboTextSourceSpan* sp =
        gumbo_text_index_scalar_spans(ctx.index, 1, &space_spans);
    assert(space_spans == 1);
    size_t cr = byte_offset(html, "\r\n");
    assert(sp[0].source_start == cr);
    assert(sp[0].source_end == cr + 2);  // Both CR and LF are covered.

    GumboTextQuoteResults* r = must_find(ctx.index, "a b", NULL, NULL);
    assert(r->count == 1);
    // Same node, contiguous source bytes: everything merges to one span.
    assert(r->quotes[0].span_count == 1);
    size_t a = byte_offset(html, "a");
    assert(r->quotes[0].spans[0].source_start == a);
    assert(r->quotes[0].spans[0].source_end == a + 4);  // "a\r\nb"
    gumbo_text_quote_results_destroy(r);
    destroy_context(&ctx);
  }

  // --- Invalid UTF-8 follows the parser's replacement behavior. ---
  {
    const char* html = "<p>\xc3\x28z</p>";
    Context ctx = make_context(html);
    const char* text = gumbo_text_index_normalized_text(ctx.index);
    // One replacement char, then the valid '(' and 'z'.
    assert(gumbo_text_index_scalar_count(ctx.index) == 3);
    assert(memcmp(text, kReplacement, 3) == 0);
    assert(text[3] == '(' && text[4] == 'z');

    // The replacement scalar points only at the offending lead byte, and the
    // following valid scalars are not mis-mapped onto it.
    size_t count = 0;
    const GumboTextSourceSpan* sp =
        gumbo_text_index_scalar_spans(ctx.index, 0, &count);
    assert(count == 1);
    size_t bad = byte_offset(html, "\xc3");
    assert(sp[0].source_start == bad);
    assert(sp[0].source_end == bad + 1);
    const GumboTextSourceSpan* paren =
        gumbo_text_index_scalar_spans(ctx.index, 1, &count);
    assert(count == 1);
    assert(paren[0].source_start == bad + 1);
    assert(paren[0].source_end == bad + 2);
    destroy_context(&ctx);
  }

  contract_case("mapping");
}

// Scenario group "collapse": whitespace folding across nodes, structural
// separators, inline adjacency without invented spaces, and NBSP retention.
static void test_collapse(void) {
  // Whitespace on both sides of an inline tag folds to one space that keeps
  // every contributing source.
  {
    const char* html = "<p>foo <b> bar</b></p>";
    Context ctx = make_context(html);
    assert(memcmp(gumbo_text_index_normalized_text(ctx.index), "foo bar", 7) ==
           0);
    size_t count = 0;
    const GumboTextSourceSpan* sp =
        gumbo_text_index_scalar_spans(ctx.index, 3, &count);
    assert(count == 2);  // One source from each side of <b>.
    size_t left = byte_offset(html, " <b>");
    size_t right = byte_offset(html, "> ") + 1;
    // The two contributions come from different text nodes (the one inside
    // <p> and the one inside <b>), yet the folded space retains both.
    assert(sp[0].node != sp[1].node);
    assert((sp[0].source_start == left && sp[1].source_start == right) ||
           (sp[1].source_start == left && sp[0].source_start == right));
    destroy_context(&ctx);
  }

  // A block boundary creates a separator space with no source bytes.
  {
    const char* html = "<p>a</p><p>b</p>";
    Context ctx = make_context(html);
    assert(memcmp(gumbo_text_index_normalized_text(ctx.index), "a b", 3) ==
           0);
    size_t count = 1;
    gumbo_text_index_scalar_spans(ctx.index, 1, &count);
    assert(count == 0);  // Purely structural: no contributing source bytes.
    destroy_context(&ctx);
  }

  // Adjacent inline elements get no separator of their own.
  {
    const char* html = "<span>x</span><i>y</i>";
    Context ctx = make_context(html);
    assert(memcmp(gumbo_text_index_normalized_text(ctx.index), "xy", 2) ==
           0);
    destroy_context(&ctx);
  }

  // NBSP is preserved verbatim rather than treated as foldable whitespace.
  {
    const char* html = "<p>a&nbsp;b</p>";
    Context ctx = make_context(html);
    const char* text = gumbo_text_index_normalized_text(ctx.index);
    assert(gumbo_text_index_normalized_length(ctx.index) == 4);
    assert(memcmp(text, "a", 1) == 0);
    assert(memcmp(text + 1, kNbsp, 2) == 0);
    assert(memcmp(text + 3, "b", 1) == 0);
    size_t count = 0;
    const GumboTextSourceSpan* sp =
        gumbo_text_index_scalar_spans(ctx.index, 1, &count);
    assert(count == 1);  // The NBSP still points at its entity reference.
    size_t ref = byte_offset(html, "&nbsp;");
    assert(sp[0].source_start == ref && sp[0].source_end == ref + 6);
    destroy_context(&ctx);
  }

  // Leading and trailing whitespace is trimmed away entirely.
  {
    const char* html = "<div>  c  </div>";
    Context ctx = make_context(html);
    assert(memcmp(gumbo_text_index_normalized_text(ctx.index), "c", 1) == 0);
    destroy_context(&ctx);
  }

  // head/script/style/template and any hidden subtree contribute nothing,
  // including text nested several levels deep.
  {
    const char* html =
        "<head><title>secret title</title></head>"
        "<body><script>var s='secret script';</script>"
        "<style>.secret{}</style>"
        "<div hidden><span><b>secret hidden</b></span></div>"
        "<template><p>secret template</p></template>"
        "<p>visible</p></body>";
    Context ctx = make_context(html);
    assert(memcmp(gumbo_text_index_normalized_text(ctx.index), "visible", 7) ==
           0);
    assert(gumbo_text_index_scalar_count(ctx.index) == 7);
    GumboTextQuoteResults* r =
        must_find(ctx.index, "secret", NULL, NULL);
    assert(r->count == 0);
    gumbo_text_quote_results_destroy(r);
    destroy_context(&ctx);
  }

  contract_case("collapse");
}

// Scenario group "reparent": table error recovery foster-parents text so that
// final DOM order differs from source byte order; the index follows the DOM
// but preserves the true (here, reversed) source positions.
static void test_reparent(void) {
  const char* html = "<table><tr><td>cell</td></tr>shifted</table>";
  Context ctx = make_context(html);

  assert(memcmp(gumbo_text_index_normalized_text(ctx.index), "shifted cell",
             12) == 0);

  size_t cell_src = byte_offset(html, "cell");
  size_t shifted_src = byte_offset(html, "shifted");
  assert(shifted_src > cell_src);  // In the raw response, "cell" comes first.

  // In DOM order "shifted" is emitted before "cell".
  GumboTextQuoteResults* r = must_find(ctx.index, "shifted", NULL, NULL);
  assert(r->count == 1);
  assert(r->quotes[0].spans[0].source_start == shifted_src);
  gumbo_text_quote_results_destroy(r);

  r = must_find(ctx.index, "cell", NULL, NULL);
  assert(r->count == 1);
  assert(r->quotes[0].spans[0].source_start == cell_src);
  gumbo_text_quote_results_destroy(r);

  // A quote spanning both keeps fragments in match (DOM) order even though
  // their source offsets go backwards, and never merges across the table.
  r = must_find(ctx.index, "shifted cell", NULL, NULL);
  assert(r->count == 1);
  GumboTextQuote* q = &r->quotes[0];
  assert(q->span_count == 2);
  assert(q->spans[0].source_start == shifted_src);
  assert(q->spans[1].source_start == cell_src);
  assert(q->spans[0].source_start > q->spans[1].source_start);
  assert(q->spans[0].node != q->spans[1].node);
  gumbo_text_quote_results_destroy(r);

  destroy_context(&ctx);
  contract_case("reparent");
}

// Scenario group "quotes": context-disambiguated repeats, overlapping hits,
// no-match results, explicit query errors, and repeatable queries.
static void test_quotes(void) {
  const char* html = "<p>alpha x beta</p><p>gamma x delta</p>";
  Context ctx = make_context(html);

  // The same exact text occurs twice; without context both are returned.
  GumboTextQuoteResults* all = must_find(ctx.index, "x", NULL, NULL);
  assert(all->count == 2);
  assert(all->quotes[0].match_start != all->quotes[1].match_start);

  // A prefix tightly bound to one side selects exactly one occurrence.
  GumboTextQuoteResults* first =
      must_find(ctx.index, "x", "alpha ", NULL);
  assert(first->count == 1);
  assert(first->quotes[0].match_start == all->quotes[0].match_start);
  gumbo_text_quote_results_destroy(first);

  GumboTextQuoteResults* second =
      must_find(ctx.index, "x", "gamma ", NULL);
  assert(second->count == 1);
  assert(second->quotes[0].match_start == all->quotes[1].match_start);
  gumbo_text_quote_results_destroy(second);

  // A suffix constraint works the same way at the other end.
  GumboTextQuoteResults* suffixed =
      must_find(ctx.index, "x", NULL, " delta");
  assert(suffixed->count == 1);
  assert(suffixed->quotes[0].match_start == all->quotes[1].match_start);
  gumbo_text_quote_results_destroy(suffixed);
  gumbo_text_quote_results_destroy(all);

  // Overlapping occurrences are all reported rather than just the first.
  {
    const char* overlap_html = "<p>ababa</p>";
    Context octx = make_context(overlap_html);
    GumboTextQuoteResults* r = must_find(octx.index, "aba", NULL, NULL);
    assert(r->count == 2);
    assert(r->quotes[0].match_start == 0);
    assert(r->quotes[1].match_start == 2);
    gumbo_text_quote_results_destroy(r);
    destroy_context(&octx);
  }

  // No match is a successful call with an empty result set.
  {
    GumboTextQuoteResults* r = must_find(ctx.index, "zzz", NULL, NULL);
    assert(r->count == 0);
    gumbo_text_quote_results_destroy(r);
  }

  // Explicit query errors leave the index untouched and usable.
  {
    GumboTextQuoteResults* r = (GumboTextQuoteResults*) 0x1;
    assert(gumbo_text_quote_find(ctx.index, "", 0, NULL, 0, NULL, 0, &r) ==
           GUMBO_TEXT_QUOTE_EMPTY_EXACT);
    assert(r == NULL);
    const char bad[] = "\xff";
    assert(gumbo_text_quote_find(ctx.index, bad, 1, NULL, 0, NULL, 0, &r) ==
           GUMBO_TEXT_QUOTE_INVALID_UTF8);
    assert(r == NULL);
    assert(gumbo_text_quote_find(ctx.index, "x", 1, bad, 1, NULL, 0, &r) ==
           GUMBO_TEXT_QUOTE_INVALID_UTF8);
    assert(r == NULL);

    // Repeated queries on the same index return identical results.
    GumboTextQuoteResults* run1 = must_find(ctx.index, "x", NULL, NULL);
    GumboTextQuoteResults* run2 = must_find(ctx.index, "x", NULL, NULL);
    assert(run1->count == run2->count);
    for (size_t i = 0; i < run1->count; ++i) {
      assert(run1->quotes[i].match_start == run2->quotes[i].match_start);
      assert(run1->quotes[i].match_end == run2->quotes[i].match_end);
    }
    gumbo_text_quote_results_destroy(run1);
    gumbo_text_quote_results_destroy(run2);
  }

  destroy_context(&ctx);
  contract_case("quotes");
}

// Building without the tracking option must fail explicitly rather than
// fabricate mappings.
static void test_tracking_required(void) {
  GumboOutput* output = gumbo_parse("<p>x</p>");
  GumboTextIndex* index = gumbo_text_index_build(
      &kGumboDefaultOptions, output, "<p>x</p>", 8);
  assert(index == NULL);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

void text_anchor_contract(void) {
  test_mapping();
  test_collapse();
  test_reparent();
  test_quotes();
  test_tracking_required();
  printf("TEXT_ANCHOR_CONTRACT_OK\n");
}
