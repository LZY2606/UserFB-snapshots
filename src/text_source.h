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
// Internal-only machinery for recording, while the tree is being built, the
// original source span of every decoded character that ends up in a text node.
// It is activated only when GumboOptions.track_text_sources is true, so that a
// default parse performs no extra work and allocates no per-character state.
// The public text-anchor index (text_anchor.h) is later constructed from this
// log; nothing here re-scans the input or re-decodes entities.

#ifndef GUMBO_TEXT_SOURCE_H_
#define GUMBO_TEXT_SOURCE_H_

#include <stdbool.h>
#include <stddef.h>

#include "gumbo.h"

#ifdef __cplusplus
extern "C" {
#endif

struct GumboInternalParser;

// One entry per *decoded scalar value* that is appended to the pending text
// node buffer.  Each entry records the half-open byte span of the original
// input that produced the scalar, along with the number of UTF-8 bytes the
// scalar occupies once encoded into the (decoded) text node.  A character
// reference that decodes to two scalars emits two entries whose spans both
// cover the whole reference; a CRLF that becomes a single '\n' produces one
// entry covering both input bytes.
typedef struct {
  // Offset of the first source byte, relative to the parse input.
  size_t source_start;

  // One past the last source byte, relative to the parse input.  Entries with
  // source_end == source_start carry no source bytes (the second scalar of a
  // two-codepoint reference has a zero-length token, handled at record time).
  size_t source_end;

  // Byte offset of the encoded scalar within the owning text node's decoded
  // text, i.e. GumboText.text.
  size_t text_offset;

  // Length in bytes of the encoded scalar in the decoded text.
  size_t text_length;
} GumboTextSourceEntry;

// A growable log of source entries.  All storage uses the parser's allocator.
typedef struct {
  GumboTextSourceEntry* data;
  size_t length;
  size_t capacity;
} GumboTextSourceLog;

// Opaque registry mapping each flushed text node to its source log.  The
// registry is owned by GumboOutput and outlives parser-state teardown; the
// node pointers are borrowed from the live tree.
typedef struct GumboTextSourceRegistry GumboTextSourceRegistry;

// Initializes an empty log.  Safe to call regardless of the tracking option.
void gumbo_text_source_log_init(
    struct GumboInternalParser* parser, GumboTextSourceLog* log);

// Releases the log's storage.
void gumbo_text_source_log_destroy(
    struct GumboInternalParser* parser, GumboTextSourceLog* log);

// Appends an entry, growing the log as needed.
void gumbo_text_source_log_add(struct GumboInternalParser* parser,
    GumboTextSourceLog* log, const GumboTextSourceEntry* entry);

// Returns true when source tracking is enabled for this parse.
bool gumbo_text_source_tracking_enabled(
    const struct GumboInternalParser* parser);

// Allocates an empty registry using the supplied options' allocator.
GumboTextSourceRegistry* gumbo_text_source_registry_create(
    const GumboOptions* options);

// Transfers ownership of `log`'s storage to the registry under the given text
// node, and resets `log` to empty.
void gumbo_text_source_registry_store(GumboTextSourceRegistry* registry,
    GumboNode* node, GumboTextSourceLog* log, const GumboOptions* options);

// Looks up the source log for a text node, or NULL if it has none.
const GumboTextSourceLog* gumbo_text_source_registry_find(
    const GumboTextSourceRegistry* registry, const GumboNode* node);

// Frees the registry and every log it owns.  Safe on NULL.
void gumbo_text_source_registry_destroy(
    const GumboOptions* options, GumboTextSourceRegistry* registry);

#ifdef __cplusplus
}
#endif

#endif  // GUMBO_TEXT_SOURCE_H_
