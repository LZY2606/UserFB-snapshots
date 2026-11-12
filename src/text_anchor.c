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
// Builds the normalized visible-text index from the per-node source logs and
// resolves exact-text quotes back to original source fragments.

#include "text_anchor.h"

#include <stdlib.h>
#include <string.h>

#include "text_source.h"

// One normalized output scalar and the source fragments that produced it.
typedef struct {
  // Byte range of the scalar inside the normalized string.
  size_t norm_start;
  size_t norm_length;

  // Contributing source fragments; owned by this record.  A folded
  // whitespace scalar may hold several, and a purely structural separator
  // holds none.
  GumboTextSourceSpan* spans;
  size_t span_count;
} ScalarRecord;

struct GumboTextIndex {
  const GumboOptions* options;

  // Owned, NUL-terminated normalized text.
  char* normalized;
  size_t normalized_length;
  size_t normalized_capacity;

  ScalarRecord* scalars;
  size_t scalar_count;
  size_t scalar_capacity;

  const char* input;
  size_t input_length;
};

// Mutable construction state.
typedef struct {
  GumboTextIndex* index;
  const GumboTextSourceRegistry* registry;

  // A structural break (block boundary) that has been requested but not yet
  // materialized into a separator space.
  bool pending_break;

  // Accumulated spans of an unflushed run of ASCII whitespace, and whether
  // such a run exists.
  bool whitespace_pending;
  GumboTextSourceSpan* whitespace_spans;
  size_t whitespace_count;
  size_t whitespace_capacity;
} BuildCtx;

static void* index_allocate(const GumboTextIndex* index, size_t size) {
  return index->options->allocator(index->options->userdata, size);
}

static void index_deallocate(const GumboTextIndex* index, void* ptr) {
  index->options->deallocator(index->options->userdata, ptr);
}

static void normalized_reserve(GumboTextIndex* index, size_t extra) {
  size_t needed = index->normalized_length + extra + 1;
  if (needed <= index->normalized_capacity) {
    return;
  }
  size_t new_capacity = index->normalized_capacity
                            ? index->normalized_capacity * 2
                            : 32;
  while (new_capacity < needed) {
    new_capacity *= 2;
  }
  char* new_data = index_allocate(index, new_capacity);
  if (index->normalized_length) {
    memcpy(new_data, index->normalized, index->normalized_length);
  }
  if (index->normalized) {
    index_deallocate(index, index->normalized);
  }
  index->normalized = new_data;
  index->normalized_capacity = new_capacity;
}

static void scalar_capacity_reserve(BuildCtx* ctx, size_t extra) {
  GumboTextIndex* index = ctx->index;
  if (index->scalar_count + extra <= index->scalar_capacity) {
    return;
  }
  size_t new_capacity = index->scalar_capacity ? index->scalar_capacity * 2
                                               : 16;
  while (new_capacity < index->scalar_count + extra) {
    new_capacity *= 2;
  }
  ScalarRecord* new_records =
      index_allocate(index, new_capacity * sizeof(*new_records));
  if (index->scalar_count) {
    memcpy(new_records, index->scalars,
        index->scalar_count * sizeof(*new_records));
  }
  if (index->scalars) {
    index_deallocate(index, index->scalars);
  }
  index->scalars = new_records;
  index->scalar_capacity = new_capacity;
}

static void whitespace_capacity_reserve(BuildCtx* ctx) {
  if (ctx->whitespace_count < ctx->whitespace_capacity) {
    return;
  }
  GumboTextIndex* index = ctx->index;
  size_t new_capacity = ctx->whitespace_capacity
                            ? ctx->whitespace_capacity * 2
                            : 8;
  GumboTextSourceSpan* new_spans =
      index_allocate(index, new_capacity * sizeof(*new_spans));
  if (ctx->whitespace_count) {
    memcpy(new_spans, ctx->whitespace_spans,
        ctx->whitespace_count * sizeof(*new_spans));
  }
  if (ctx->whitespace_spans) {
    index_deallocate(index, ctx->whitespace_spans);
  }
  ctx->whitespace_spans = new_spans;
  ctx->whitespace_capacity = new_capacity;
}

static bool is_ascii_whitespace(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

static bool is_block_separator_tag(GumboTag tag) {
  switch (tag) {
    case GUMBO_TAG_P:
    case GUMBO_TAG_DIV:
    case GUMBO_TAG_SECTION:
    case GUMBO_TAG_ARTICLE:
    case GUMBO_TAG_LI:
    case GUMBO_TAG_TR:
    case GUMBO_TAG_BR:
      return true;
    default:
      return false;
  }
}

static bool is_excluded_tag(GumboTag tag) {
  switch (tag) {
    case GUMBO_TAG_HEAD:
    case GUMBO_TAG_SCRIPT:
    case GUMBO_TAG_STYLE:
    case GUMBO_TAG_TEMPLATE:
      return true;
    default:
      return false;
  }
}

// Appends one scalar with an explicit set of source spans.
static void append_scalar(BuildCtx* ctx, const char* bytes, size_t length,
    const GumboTextSourceSpan* spans, size_t span_count) {
  GumboTextIndex* index = ctx->index;
  scalar_capacity_reserve(ctx, 1);
  normalized_reserve(index, length);

  ScalarRecord* record = &index->scalars[index->scalar_count];
  record->norm_start = index->normalized_length;
  record->norm_length = length;
  record->span_count = span_count;
  record->spans = NULL;
  if (span_count) {
    record->spans =
        index_allocate(index, span_count * sizeof(*record->spans));
    memcpy(record->spans, spans, span_count * sizeof(*record->spans));
  }
  memcpy(index->normalized + index->normalized_length, bytes, length);
  index->normalized_length += length;
  index->normalized[index->normalized_length] = '\0';
  ++index->scalar_count;
}

// Materializes any pending structural break / whitespace run into a single
// separator space.  Leading separators are dropped, matching the trim rule.
static void flush_pending_separator(BuildCtx* ctx) {
  if (!ctx->pending_break && !ctx->whitespace_pending) {
    return;
  }
  GumboTextIndex* index = ctx->index;
  if (index->normalized_length > 0) {
    static const char kSpace = ' ';
    append_scalar(ctx, &kSpace, 1, ctx->whitespace_spans,
        ctx->whitespace_count);
  }
  ctx->pending_break = false;
  ctx->whitespace_pending = false;
  ctx->whitespace_count = 0;
}

static void request_break(BuildCtx* ctx) { ctx->pending_break = true; }

static void add_whitespace(BuildCtx* ctx, const GumboTextSourceSpan* span) {
  whitespace_capacity_reserve(ctx);
  ctx->whitespace_spans[ctx->whitespace_count++] = *span;
  ctx->whitespace_pending = true;
}

static void emit_text_node(BuildCtx* ctx, const GumboNode* node) {
  const GumboTextSourceLog* log =
      gumbo_text_source_registry_find(ctx->registry, node);
  if (!log || log->length == 0) {
    // No tracked characters (e.g. parser-synthesized text): nothing to map.
    return;
  }
  const char* text = node->v.text.text;
  for (size_t i = 0; i < log->length; ++i) {
    const GumboTextSourceEntry* entry = &log->data[i];
    const char* bytes = text + entry->text_offset;
    GumboTextSourceSpan span;
    span.source_start = entry->source_start;
    span.source_end = entry->source_end;
    span.node = node;

    if (entry->text_length == 1 && is_ascii_whitespace(
                                       (unsigned char) bytes[0])) {
      add_whitespace(ctx, &span);
    } else {
      // A non-whitespace scalar closes any pending separator or whitespace
      // run, and NBSP / other Unicode scalars pass through untouched.
      flush_pending_separator(ctx);
      append_scalar(ctx, bytes, entry->text_length, &span, 1);
    }
  }
}

static void walk(BuildCtx* ctx, GumboNode* node) {
  switch (node->type) {
    case GUMBO_NODE_DOCUMENT: {
      GumboVector* children = &node->v.document.children;
      for (size_t i = 0; i < children->length; ++i) {
        walk(ctx, (GumboNode*) children->data[i]);
      }
      break;
    }
    case GUMBO_NODE_TEMPLATE:
      // Template contents are never visible text.
      return;
    case GUMBO_NODE_ELEMENT: {
      GumboElement* element = &node->v.element;
      if (is_excluded_tag(element->tag) ||
          gumbo_get_attribute(&element->attributes, "hidden") != NULL) {
        return;
      }
      bool is_block = is_block_separator_tag(element->tag);
      if (is_block) {
        request_break(ctx);
      }
      for (size_t i = 0; i < element->children.length; ++i) {
        walk(ctx, (GumboNode*) element->children.data[i]);
      }
      if (is_block) {
        request_break(ctx);
      }
      break;
    }
    case GUMBO_NODE_TEXT:
    case GUMBO_NODE_WHITESPACE:
    case GUMBO_NODE_CDATA:
      emit_text_node(ctx, node);
      break;
    default:
      // Comments and any other node kinds carry no visible text.
      break;
  }
}

GumboTextIndex* gumbo_text_index_build(const GumboOptions* options,
    GumboOutput* output, const char* input, size_t input_length) {
  if (!output || !output->_text_sources) {
    // Tracking was not enabled for this parse: fail explicitly rather than
    // guessing at mappings.
    return NULL;
  }

  GumboTextIndex* index = options->allocator(
      options->userdata, sizeof(GumboTextIndex));
  index->options = options;
  index->normalized = NULL;
  index->normalized_length = 0;
  index->normalized_capacity = 0;
  index->scalars = NULL;
  index->scalar_count = 0;
  index->scalar_capacity = 0;
  index->input = input;
  index->input_length = input_length;

  BuildCtx ctx;
  ctx.index = index;
  ctx.registry = (const GumboTextSourceRegistry*) output->_text_sources;
  ctx.pending_break = false;
  ctx.whitespace_pending = false;
  ctx.whitespace_spans = NULL;
  ctx.whitespace_count = 0;
  ctx.whitespace_capacity = 0;

  walk(&ctx, output->document);

  // Trim a trailing separator/whitespace run; leading ones were never
  // materialized.
  while (index->scalar_count > 0) {
    ScalarRecord* last = &index->scalars[index->scalar_count - 1];
    if (last->norm_length != 1 ||
        index->normalized[last->norm_start] != ' ') {
      break;
    }
    index_deallocate(index, last->spans);
    index->normalized_length = last->norm_start;
    index->normalized[index->normalized_length] = '\0';
    --index->scalar_count;
  }

  if (ctx.whitespace_spans) {
    index_deallocate(index, ctx.whitespace_spans);
  }

  // Guarantee a NUL-terminated string even when the document has no visible
  // text, so normalized_text() never returns NULL for a successfully built
  // index.
  if (index->normalized == NULL) {
    index->normalized = index_allocate(index, 1);
    index->normalized[0] = '\0';
    index->normalized_capacity = 1;
  }
  return index;
}

void gumbo_text_index_destroy(GumboTextIndex* index) {
  if (!index) {
    return;
  }
  for (size_t i = 0; i < index->scalar_count; ++i) {
    if (index->scalars[i].spans) {
      index_deallocate(index, index->scalars[i].spans);
    }
  }
  if (index->scalars) {
    index_deallocate(index, index->scalars);
  }
  if (index->normalized) {
    index_deallocate(index, index->normalized);
  }
  index->options->deallocator(index->options->userdata, index);
}

const char* gumbo_text_index_normalized_text(const GumboTextIndex* index) {
  return index->normalized;
}

size_t gumbo_text_index_normalized_length(const GumboTextIndex* index) {
  return index->normalized_length;
}

size_t gumbo_text_index_scalar_count(const GumboTextIndex* index) {
  return index->scalar_count;
}

const GumboTextSourceSpan* gumbo_text_index_scalar_spans(
    const GumboTextIndex* index, size_t scalar_index, size_t* out_count) {
  if (scalar_index >= index->scalar_count) {
    *out_count = 0;
    return NULL;
  }
  const ScalarRecord* record = &index->scalars[scalar_index];
  *out_count = record->span_count;
  return record->spans;
}

// Strict UTF-8 validation: rejects overlong encodings, surrogates and code
// points above U+10FFFF, as well as truncated or malformed continuation
// sequences.
static bool is_valid_utf8(const char* data, size_t length) {
  const unsigned char* s = (const unsigned char*) data;
  size_t i = 0;
  while (i < length) {
    unsigned char lead = s[i];
    size_t extra;
    unsigned int codepoint;
    unsigned int minimum;
    if (lead < 0x80) {
      ++i;
      continue;
    } else if ((lead & 0xE0) == 0xC0) {
      extra = 1;
      codepoint = lead & 0x1F;
      minimum = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
      extra = 2;
      codepoint = lead & 0x0F;
      minimum = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
      extra = 3;
      codepoint = lead & 0x07;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (i + extra >= length) {
      return false;
    }
    for (size_t k = 1; k <= extra; ++k) {
      if ((s[i + k] & 0xC0) != 0x80) {
        return false;
      }
      codepoint = (codepoint << 6) | (s[i + k] & 0x3F);
    }
    if (codepoint < minimum || codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
      return false;
    }
    i += extra + 1;
  }
  return true;
}

// Returns the index of the scalar whose normalized range starts exactly at
// `offset`, which must be a scalar boundary.
static size_t scalar_index_at(const GumboTextIndex* index, size_t offset) {
  size_t lo = 0;
  size_t hi = index->scalar_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    size_t start = index->scalars[mid].norm_start;
    if (start == offset) {
      return mid;
    }
    if (start < offset) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

static bool is_scalar_boundary(const GumboTextIndex* index, size_t offset) {
  if (offset == index->normalized_length) {
    return true;
  }
  size_t i = scalar_index_at(index, offset);
  return i < index->scalar_count &&
         index->scalars[i].norm_start == offset;
}

// Appends a source fragment, merging with the previous one only when both
// belong to the same node and touch or overlap in the original input.
static bool append_quote_span(const GumboTextIndex* index,
    GumboTextQuote* quote, const GumboTextSourceSpan* span) {
  if (quote->span_count > 0) {
    GumboTextSourceSpan* last = &quote->spans[quote->span_count - 1];
    if (last->node == span->node &&
        span->source_start <= last->source_end) {
      if (span->source_end > last->source_end) {
        last->source_end = span->source_end;
      }
      return true;
    }
  }
  GumboTextSourceSpan* grown = index_allocate(index,
      (quote->span_count + 1) * sizeof(*grown));
  if (!grown) {
    return false;
  }
  if (quote->span_count) {
    memcpy(grown, quote->spans,
        quote->span_count * sizeof(*grown));
    index->options->deallocator(
        index->options->userdata, quote->spans);
  }
  grown[quote->span_count++] = *span;
  quote->spans = grown;
  return true;
}

static void free_quote(const GumboTextIndex* index, GumboTextQuote* quote) {
  if (quote->spans) {
    index->options->deallocator(
        index->options->userdata, quote->spans);
    quote->spans = NULL;
  }
  quote->span_count = 0;
}

GumboTextQuoteStatus gumbo_text_quote_find(const GumboTextIndex* index,
    const char* exact, size_t exact_length, const char* prefix,
    size_t prefix_length, const char* suffix, size_t suffix_length,
    GumboTextQuoteResults** out_results) {
  *out_results = NULL;
  if (exact_length == 0) {
    return GUMBO_TEXT_QUOTE_EMPTY_EXACT;
  }
  if (!is_valid_utf8(exact, exact_length) ||
      (prefix_length && !is_valid_utf8(prefix, prefix_length)) ||
      (suffix_length && !is_valid_utf8(suffix, suffix_length))) {
    return GUMBO_TEXT_QUOTE_INVALID_UTF8;
  }

  GumboTextQuoteResults* results =
      index_allocate(index, sizeof(GumboTextQuoteResults));
  results->quotes = NULL;
  results->count = 0;
  results->_deallocator = index->options->deallocator;
  results->_userdata = index->options->userdata;
  size_t capacity = 0;

  const char* haystack = index->normalized;
  size_t haystack_length = index->normalized_length;

  for (size_t boundary = 0; boundary + exact_length <= haystack_length;
       ++boundary) {
    if (!is_scalar_boundary(index, boundary)) {
      continue;
    }
    if (memcmp(haystack + boundary, exact, exact_length) != 0) {
      continue;
    }
    size_t match_end = boundary + exact_length;
    if (!is_scalar_boundary(index, match_end)) {
      continue;
    }
    if (prefix_length) {
      if (boundary < prefix_length ||
          memcmp(haystack + boundary - prefix_length, prefix,
              prefix_length) != 0 ||
          !is_scalar_boundary(index, boundary - prefix_length)) {
        continue;
      }
    }
    if (suffix_length) {
      if (match_end + suffix_length > haystack_length ||
          memcmp(haystack + match_end, suffix, suffix_length) != 0 ||
          !is_scalar_boundary(index, match_end + suffix_length)) {
        continue;
      }
    }

    if (results->count == capacity) {
      size_t new_capacity = capacity ? capacity * 2 : 4;
      GumboTextQuote* grown = index_allocate(index,
          new_capacity * sizeof(*grown));
      if (!grown) {
        goto oom;
      }
      if (results->count) {
        memcpy(grown, results->quotes,
            results->count * sizeof(*grown));
        index->options->deallocator(
            index->options->userdata, results->quotes);
      }
      results->quotes = grown;
      capacity = new_capacity;
    }

    GumboTextQuote* quote = &results->quotes[results->count];
    quote->match_start = boundary;
    quote->match_end = match_end;
    quote->spans = NULL;
    quote->span_count = 0;

    bool ok = true;
    size_t i = scalar_index_at(index, boundary);
    for (; i < index->scalar_count &&
           index->scalars[i].norm_start < match_end;
         ++i) {
      const ScalarRecord* record = &index->scalars[i];
      for (size_t j = 0; j < record->span_count; ++j) {
        if (!append_quote_span(index, quote, &record->spans[j])) {
          ok = false;
          break;
        }
      }
      if (!ok) {
        break;
      }
    }
    if (!ok) {
      free_quote(index, quote);
      goto oom;
    }
    ++results->count;
  }

  *out_results = results;
  return GUMBO_TEXT_QUOTE_OK;

oom:
  for (size_t i = 0; i < results->count; ++i) {
    free_quote(index, &results->quotes[i]);
  }
  if (results->quotes) {
    index->options->deallocator(
        index->options->userdata, results->quotes);
  }
  index->options->deallocator(index->options->userdata, results);
  return GUMBO_TEXT_QUOTE_OUT_OF_MEMORY;
}

void gumbo_text_quote_results_destroy(GumboTextQuoteResults* results) {
  if (!results) {
    return;
  }
  GumboDeallocatorFunction deallocate = results->_deallocator;
  void* userdata = results->_userdata;
  for (size_t i = 0; i < results->count; ++i) {
    deallocate(userdata, results->quotes[i].spans);
  }
  deallocate(userdata, results->quotes);
  deallocate(userdata, results);
}
