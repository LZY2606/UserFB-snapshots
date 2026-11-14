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
// Replays archived script output (the recorded bytes a script was known to
// write into the document) during a normal tree-construction parse, without
// executing any JavaScript.  This header declares the public replay contract;
// the implementation lives in script_replay.c and hooks into the regular
// parser/tokenizer chain.

#ifndef GUMBO_SCRIPT_REPLAY_H_
#define GUMBO_SCRIPT_REPLAY_H_

#include <stddef.h>

#include "gumbo.h"

#ifdef __cplusplus
extern "C" {
#endif

// Callback that supplies the known output bytes for one script element.
//
// Invoked once per HTML script element that was closed by a real HTML
// </script> end tag in a full-page parse, after the script text and its
// original_end_tag are complete, the script has been popped from the stack of
// open elements, and the original insertion mode has been restored, but before
// the next token is consumed.  Scripts only closed by EOF, scripts inside
// template contents, scripts in foreign namespaces, and all scripts in
// fragment parses do not trigger this callback.
//
// 'partial' is the partially-built output and is read-only; 'script' is the
// just-closed script node inside that live tree.  The callback must not
// modify or destroy the tree, nor re-enter this parse session; it may run
// independent parses of its own.  The returned piece holds the bytes to be
// written into the document at this point; an empty piece (NULL data or zero
// length) writes nothing.  The returned bytes only need to stay valid until
// the next callback invocation or until the parse call returns - the library
// copies them before parsing resumes.
typedef GumboStringPiece (*GumboScriptReplay)(const GumboOutput *partial,
                                             const GumboNode *script,
                                             void *userdata);

// The result of a replay parse: the parse tree, plus the fully expanded
// logical source (the original input with every triggered script's output
// spliced in after that script's end tag, recursively).  All original-text
// slices, source positions, and error records in the tree are expressed
// against 'source', which this result owns; both remain valid until
// gumbo_destroy_replay_output.
typedef struct {
  GumboOutput *tree;
  GumboStringPiece source;
} GumboReplayOutput;

// Parses a full document like gumbo_parse_with_options, additionally feeding
// the bytes returned by 'replay' back into the live input stream whenever an
// HTML script element is closed by a real end tag.  Inserted bytes are
// tokenized and tree-built in the current parser state (they are not parsed
// as separate fragments), and scripts closed inside inserted output trigger
// 'replay' again before the bytes after them are consumed.  'replay' may be
// NULL, in which case this behaves like a plain parse whose result still owns
// a copy of the input.  All allocations use options->allocator.
GumboReplayOutput *gumbo_parse_with_script_replay(const GumboOptions *options,
    const char *input, size_t length, GumboScriptReplay replay, void *userdata);

// Releases the replay wrapper, the owned source, and the parse tree.  None of
// the pieces may be used afterwards.
void gumbo_destroy_replay_output(const GumboOptions *options,
                                GumboReplayOutput *output);

// ---------------------------------------------------------------------------
// Internal plumbing, used by the parser/tokenizer chain.  Not public API.
// ---------------------------------------------------------------------------

struct GumboInternalParser;
struct GumboInternalToken;

// Tree-builder hook invoked from the TEXT insertion mode after an element has
// been popped and the original insertion mode restored.  When the parser was
// started by gumbo_parse_with_script_replay and the element is an HTML script
// closed by a real </script> end tag (not EOF, not in template contents, not
// foreign, not a fragment parse), this invokes the replay callback and
// splices the returned bytes into the input stream ahead of the unconsumed
// input.  No-op for ordinary parses.
void gumbo_script_replay_closed(struct GumboInternalParser* parser,
    GumboNode* script, struct GumboInternalToken* token);

#ifdef __cplusplus
}
#endif
#endif  // GUMBO_SCRIPT_REPLAY_H_
