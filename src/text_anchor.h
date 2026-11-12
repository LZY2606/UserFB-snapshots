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
// A text index over a parsed Gumbo document.  The index owns a normalized
// UTF-8 string (visible text in final DOM order, with structural and
// whitespace folding applied) together with a per-scalar map back to the
// half-open byte spans of the original response that produced each scalar.
// It is built from the source data recorded when
// GumboOptions.track_text_sources was enabled; parsing itself is unchanged
// when that option is off, and no browser-style layout is performed.

#ifndef GUMBO_TEXT_ANCHOR_H_
#define GUMBO_TEXT_ANCHOR_H_

#include <stdbool.h>
#include <stddef.h>

#include "gumbo.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque index constructed by gumbo_text_index_build and released with
// gumbo_text_index_destroy.  Node pointers handed out by the index are
// borrowed from the GumboOutput and are valid only while that output (and its
// input buffer) remains alive.
typedef struct GumboTextIndex GumboTextIndex;

// One fragment of original source text: a half-open byte range relative to
// the parse input, together with the text node that owns it.
typedef struct {
  size_t source_start;
  size_t source_end;
  const GumboNode* node;
} GumboTextSourceSpan;

// One successful exact-match hit.  `match_start`/`match_end` are a half-open
// byte range within the index's normalized text; `spans` are the original
// source fragments that produced the matched text, in the order the matched
// text appears (which may differ from source byte order).  Fragments are only
// merged when they belong to the same node and are adjacent or overlapping;
// intervening tags and hidden content never fall inside a fragment.
typedef struct {
  size_t match_start;
  size_t match_end;
  GumboTextSourceSpan* spans;
  size_t span_count;
} GumboTextQuote;

// The complete set of hits for one gumbo_text_quote_find call.  Individually
// releasable via gumbo_text_quote_results_destroy.
typedef struct {
  GumboTextQuote* quotes;
  size_t count;

  // Internal: allocator pair used for every allocation in this result set, so
  // it can be released independently of the index.  Do not set directly.
  GumboDeallocatorFunction _deallocator;
  void* _userdata;
} GumboTextQuoteResults;

// Outcome of a gumbo_text_quote_find call.
typedef enum {
  GUMBO_TEXT_QUOTE_OK,
  // `exact` was empty.
  GUMBO_TEXT_QUOTE_EMPTY_EXACT,
  // One of the query strings was not well-formed UTF-8.
  GUMBO_TEXT_QUOTE_INVALID_UTF8,
  // Memory allocation failed; the index is left usable.
  GUMBO_TEXT_QUOTE_OUT_OF_MEMORY
} GumboTextQuoteStatus;

// Builds a text index from a live GumboOutput whose parse had
// options.track_text_sources enabled.  `input`/`input_length` must be the same
// buffer passed to gumbo_parse_with_options.  Returns NULL (and allocates
// nothing) when tracking was not enabled for the output.  The index owns its
// normalized string and maps; node references are borrowed from the output.
GumboTextIndex* gumbo_text_index_build(const GumboOptions* options,
    GumboOutput* output, const char* input, size_t input_length);

// Releases an index and everything it owns.
void gumbo_text_index_destroy(GumboTextIndex* index);

// The normalized UTF-8 string and its byte length.  The pointer is owned by
// the index and valid until gumbo_text_index_destroy.
const char* gumbo_text_index_normalized_text(const GumboTextIndex* index);
size_t gumbo_text_index_normalized_length(const GumboTextIndex* index);

// Number of Unicode scalars in the normalized string.
size_t gumbo_text_index_scalar_count(const GumboTextIndex* index);

// The source spans contributing to the scalar at `scalar_index`, returned via
// *out_count.  Ordinary scalars have one span; a folded whitespace scalar may
// have several, and a purely structural separator space has zero.  The spans
// are owned by the index.
const GumboTextSourceSpan* gumbo_text_index_scalar_spans(
    const GumboTextIndex* index, size_t scalar_index, size_t* out_count);

// Finds every scalar-aligned occurrence of `exact` in the normalized text.
// When non-NULL, `prefix` and `suffix` must sit immediately against the two
// ends of a hit for it to be returned; pass NULL (length 0) to omit either
// constraint.  Overlapping hits are all returned.  On GUMBO_TEXT_QUOTE_OK,
// *out_results is a newly allocated result set (possibly with count 0) that
// the caller must release; on any error status *out_results is set to NULL
// and the index is not modified, so repeated queries stay consistent.
GumboTextQuoteStatus gumbo_text_quote_find(const GumboTextIndex* index,
    const char* exact, size_t exact_length, const char* prefix,
    size_t prefix_length, const char* suffix, size_t suffix_length,
    GumboTextQuoteResults** out_results);

// Releases a result set and its fragments.  Safe on NULL.
void gumbo_text_quote_results_destroy(GumboTextQuoteResults* results);

#ifdef __cplusplus
}
#endif

#endif  // GUMBO_TEXT_ANCHOR_H_
