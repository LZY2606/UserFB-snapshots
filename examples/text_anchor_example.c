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
// Demonstrates locating a field whose text runs across an inline tag and
// recovering the several original source fragments that produced it.  The
// normalized field is found as one string, but the quote results keep each
// source span separate instead of swallowing the intervening tag.

#include <stdio.h>
#include <string.h>

#include "gumbo.h"
#include "text_anchor.h"

int main(void) {
  // The visible price is split across a <b> tag, with an entity inside.
  const char* html =
      "<div>Price: <span>USD&nbsp;<b>19</b>.99</span></div>"
      "<div>Price: <span>EUR&nbsp;<b>9</b>.50</span></div>";
  size_t html_length = strlen(html);

  GumboOptions options = kGumboDefaultOptions;
  options.track_text_sources = true;  // Opt in before parsing.

  GumboOutput* output =
      gumbo_parse_with_options(&options, html, html_length);

  // The output must still be alive and must have been parsed with tracking.
  GumboTextIndex* index =
      gumbo_text_index_build(&options, output, html, html_length);
  if (!index) {
    fprintf(stderr, "source tracking was not enabled\n");
    gumbo_destroy_output(&options, output);
    return 1;
  }

  printf("normalized text: [%s]\n\n",
      gumbo_text_index_normalized_text(index));

  // Two prices share the "Price:" prefix style; disambiguate with a suffix so
  // only the dollar amount's numeric tail is selected.
  const char* exact = "19.99";
  const char* prefix = "USD\xc2\xa0";  // "USD" + NBSP, tightly bound.
  GumboTextQuoteResults* results = NULL;
  GumboTextQuoteStatus status = gumbo_text_quote_find(index, exact,
      strlen(exact), prefix, strlen(prefix), NULL, 0, &results);
  if (status != GUMBO_TEXT_QUOTE_OK) {
    fprintf(stderr, "quote_find failed: %d\n", (int) status);
    gumbo_text_index_destroy(index);
    gumbo_destroy_output(&options, output);
    return 1;
  }

  for (size_t i = 0; i < results->count; ++i) {
    const GumboTextQuote* quote = &results->quotes[i];
    printf("hit %zu: normalized bytes [%zu, %zu)\n", i, quote->match_start,
        quote->match_end);
    printf("  %zu original source fragment(s):\n", quote->span_count);
    for (size_t j = 0; j < quote->span_count; ++j) {
      const GumboTextSourceSpan* span = &quote->spans[j];
      printf("    input[%zu, %zu) = \"%.*s\"\n", span->source_start,
          span->source_end, (int)(span->source_end - span->source_start),
          html + span->source_start);
    }
  }

  gumbo_text_quote_results_destroy(results);
  gumbo_text_index_destroy(index);
  gumbo_destroy_output(&options, output);
  return 0;
}
