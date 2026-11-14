// Copyright 2010 Google Inc. All Rights Reserved.
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
// Implementation of the script-output replay contract declared in
// script_replay.h.
//
// Strategy: the parse runs on a single contiguous, growable buffer that at
// all times holds the logical input consumed so far followed by the
// not-yet-consumed input.  When a script closes and the replay callback
// returns bytes, those bytes are spliced into the buffer at the tokenizer's
// current read position, so the existing tokenizer and tree-construction
// state simply continue over the expanded stream.  Because insertions only
// ever happen at the read position, every byte before it - and therefore
// every source slice already handed out - keeps its logical offset.
//
// If the buffer has to grow, it is reallocated and the tokenizer's internal
// input pointers are rebased.  Retired buffer generations are kept alive
// until the parse finishes, so slices recorded in the partially-built tree
// stay valid (and readable by the callback) throughout.  Once the parse
// completes, one pass over the finished tree and error list rewrites every
// input slice to point into the final generation, which is exactly the
// expanded source owned by the result; the retired generations are then
// freed.

#include "script_replay.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "error.h"
#include "parser.h"
#include "token_type.h"
#include "tokenizer.h"
#include "utf8.h"
#include "util.h"

// One generation of the input buffer.  Generations are linked in allocation
// order; all but the last are retired (no longer read by the tokenizer) but
// kept alive so that source slices already handed out remain valid.
typedef struct GumboReplayBuffer {
  char* base;
  size_t capacity;
  struct GumboReplayBuffer* next;
} GumboReplayBuffer;

typedef struct GumboInternalReplayState {
  // The client callback and its userdata, or NULL when replay is disabled.
  GumboScriptReplay callback;
  void* userdata;

  // All buffer generations, oldest first.
  GumboReplayBuffer* buffers;

  // The generation the tokenizer currently reads from.
  GumboReplayBuffer* active;

  // The number of valid bytes in the active buffer.
  size_t filled;
} GumboReplayState;

static void* replay_allocate(const GumboOptions* options, size_t size) {
  return options->allocator(options->userdata, size);
}

static void replay_deallocate(const GumboOptions* options, void* pointer) {
  options->deallocator(options->userdata, pointer);
}

// Splices 'length' bytes into the logical input at the tokenizer's current
// read position, growing the buffer into a new generation if necessary.
static void replay_insert(GumboParser* parser, GumboReplayState* replay,
    const char* data, size_t length) {
  Utf8Iterator* input = gumbo_tokenizer_input_iterator(parser);
  GumboReplayBuffer* active = replay->active;
  // Undo the tokenizer's read-ahead of the next unconsumed character (which
  // may span a CR/LF pair); it is re-decoded from the modified buffer below.
  gumbo_tokenizer_unread_current(parser, active->base);
  size_t offset = input->_start - active->base;
  assert(offset <= replay->filled);

  if (replay->filled + length + 1 > active->capacity) {
    // Grow, keeping one spare byte so that a pointer is always strictly
    // inside its generation's range (the final NUL terminator also lives
    // there).  The old generation is retired but not freed.
    size_t capacity = active->capacity * 2;
    size_t needed = replay->filled + length + 1;
    if (capacity < needed) {
      capacity = needed;
    }
    char* grown = gumbo_parser_allocate(parser, capacity);
    memcpy(grown, active->base, replay->filled);
    GumboReplayBuffer* generation =
        gumbo_parser_allocate(parser, sizeof(GumboReplayBuffer));
    generation->base = grown;
    generation->capacity = capacity;
    generation->next = NULL;
    active->next = generation;
    replay->active = generation;
    gumbo_tokenizer_shift_input_pointers(parser, grown - active->base);
    active = generation;
  }

  char* insertion_point = active->base + offset;
  memmove(insertion_point + length, insertion_point, replay->filled - offset);
  memcpy(insertion_point, data, length);
  replay->filled += length;
  input->_end += length;
  if (input->_mark >= insertion_point) {
    input->_mark += length;
  }
  // The character decoded before the insertion is stale; decode the first
  // inserted byte instead.  Entities, CR/LF pairs, and multi-byte characters
  // spanning the junction are handled by the normal decoding from here on.
  utf8iterator_reread_current(input);
  // The next token starts at the insertion point, exactly as if the bytes
  // had always been there (a folded CR/LF pair belongs to the preceding
  // token's original text, matching a plain parse of the expanded source).
  gumbo_tokenizer_sync_token_start(parser);
}

void gumbo_script_replay_closed(
    GumboParser* parser, GumboNode* script, GumboToken* token) {
  GumboReplayState* replay = parser->_replay;
  if (!replay) {
    return;
  }
  // Only a real HTML </script> end tag triggers replay; closes forced by EOF
  // arrive here with an EOF token instead.
  if (token->type != GUMBO_TOKEN_END_TAG ||
      token->v.end_tag != GUMBO_TAG_SCRIPT) {
    return;
  }
  if (!script || script->type != GUMBO_NODE_ELEMENT ||
      script->v.element.tag != GUMBO_TAG_SCRIPT ||
      script->v.element.tag_namespace != GUMBO_NAMESPACE_HTML) {
    return;
  }
  // Scripts inside template contents stay inert.
  for (GumboNode* ancestor = script->parent; ancestor;
       ancestor = ancestor->parent) {
    if (ancestor->type == GUMBO_NODE_TEMPLATE) {
      return;
    }
  }
  GumboStringPiece output =
      replay->callback(parser->_output, script, replay->userdata);
  if (!output.data || !output.length) {
    return;
  }
  // The callback's bytes are copied into the owned buffer before parsing
  // resumes, so the caller may reuse or free its own storage afterwards.
  replay_insert(parser, replay, output.data, output.length);
}

// Maps a pointer into any buffer generation to the corresponding pointer into
// the final generation.  Pointers that don't belong to any generation (NULL,
// string literals) are returned unchanged.
static const char* remap_pointer(
    const GumboReplayState* replay, const char* pointer) {
  if (!pointer) {
    return NULL;
  }
  uintptr_t address = (uintptr_t) pointer;
  for (GumboReplayBuffer* buffer = replay->buffers; buffer;
       buffer = buffer->next) {
    uintptr_t base = (uintptr_t) buffer->base;
    if (address - base < buffer->capacity) {
      return replay->active->base + (address - base);
    }
  }
  return pointer;
}

static void remap_node(const GumboReplayState* replay, GumboNode* node) {
  switch (node->type) {
    case GUMBO_NODE_ELEMENT:
    case GUMBO_NODE_TEMPLATE: {
      GumboElement* element = &node->v.element;
      element->original_tag.data =
          remap_pointer(replay, element->original_tag.data);
      element->original_end_tag.data =
          remap_pointer(replay, element->original_end_tag.data);
      for (unsigned int i = 0; i < element->attributes.length; ++i) {
        GumboAttribute* attribute = element->attributes.data[i];
        attribute->original_name.data =
            remap_pointer(replay, attribute->original_name.data);
        attribute->original_value.data =
            remap_pointer(replay, attribute->original_value.data);
      }
      for (unsigned int i = 0; i < element->children.length; ++i) {
        remap_node(replay, element->children.data[i]);
      }
      break;
    }
    case GUMBO_NODE_DOCUMENT: {
      GumboVector* children = &node->v.document.children;
      for (unsigned int i = 0; i < children->length; ++i) {
        remap_node(replay, children->data[i]);
      }
      break;
    }
    default:
      node->v.text.original_text.data =
          remap_pointer(replay, node->v.text.original_text.data);
      break;
  }
}

static void remap_errors(const GumboReplayState* replay, GumboOutput* output) {
  for (unsigned int i = 0; i < output->errors.length; ++i) {
    GumboError* error = output->errors.data[i];
    error->original_text = remap_pointer(replay, error->original_text);
    if (error->type == GUMBO_ERR_NAMED_CHAR_REF_WITHOUT_SEMICOLON ||
        error->type == GUMBO_ERR_NAMED_CHAR_REF_INVALID) {
      error->v.text.data = remap_pointer(replay, error->v.text.data);
    }
  }
}

GumboReplayOutput* gumbo_parse_with_script_replay(const GumboOptions* options,
    const char* input, size_t length, GumboScriptReplay callback,
    void* userdata) {
  GumboReplayState replay;
  replay.callback = callback;
  replay.userdata = userdata;
  replay.buffers = NULL;
  replay.active = NULL;
  replay.filled = length;

  // The result always owns its source, so the input is copied up front into
  // the first (and usually only) buffer generation.
  char* initial = replay_allocate(options, length + 1);
  if (!initial) {
    return NULL;
  }
  memcpy(initial, input, length);
  GumboReplayBuffer* first = replay_allocate(options, sizeof(GumboReplayBuffer));
  if (!first) {
    replay_deallocate(options, initial);
    return NULL;
  }
  first->base = initial;
  first->capacity = length + 1;
  first->next = NULL;
  replay.buffers = first;
  replay.active = first;

  // Fragment parses never replay; run them through the plain driver so the
  // result is just an owning copy of the input plus a normal fragment tree.
  GumboReplayState* driver_state =
      (callback && options->fragment_context == GUMBO_TAG_LAST) ? &replay
                                                                : NULL;
  GumboOutput* tree =
      gumbo_parse_internal(options, initial, length, driver_state);

  // Repoint every input slice recorded during the parse at the final buffer
  // generation, then release the retired generations.
  remap_node(&replay, tree->document);
  remap_errors(&replay, tree);

  GumboReplayOutput* output = replay_allocate(options, sizeof(GumboReplayOutput));
  if (!output) {
    gumbo_destroy_output(options, tree);
    for (GumboReplayBuffer* buffer = replay.buffers, *next; buffer;
         buffer = next) {
      next = buffer->next;
      replay_deallocate(options, buffer->base);
      replay_deallocate(options, buffer);
    }
    return NULL;
  }

  char* source = replay.active->base;
  source[replay.filled] = '\0';  // Capacity always exceeds filled by >= 1.
  output->tree = tree;
  output->source.data = source;
  output->source.length = replay.filled;

  for (GumboReplayBuffer* buffer = replay.buffers, *next; buffer;
       buffer = next) {
    next = buffer->next;
    if (buffer != replay.active) {
      replay_deallocate(options, buffer->base);
    }
    replay_deallocate(options, buffer);
  }
  return output;
}

void gumbo_destroy_replay_output(
    const GumboOptions* options, GumboReplayOutput* output) {
  if (!output) {
    return;
  }
  if (output->tree) {
    gumbo_destroy_output(options, output->tree);
  }
  if (output->source.data) {
    replay_deallocate(options, (void*) output->source.data);
  }
  replay_deallocate(options, output);
}
