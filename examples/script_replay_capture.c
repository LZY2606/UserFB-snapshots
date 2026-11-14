// Example: replaying archived script output into a saved directory page.
//
// An archive stores, alongside each saved page, the bytes every inline script
// was observed to write into the document (e.g. via document.write).  This
// program feeds those recorded bytes back into Gumbo's normal parse so the
// final tree matches what a browser would have built, without executing any
// JavaScript.
//
// Build from the repository root:
//   cc -std=c99 -I. -Isrc examples/script_replay_capture.c .task-build/libgumbo.a \
//     -o script_replay_capture

#include <stdio.h>
#include <string.h>

#include "gumbo.h"
#include "script_replay.h"

// One archive record: the script's source text and the bytes it was recorded
// to have written into the document at that point.
typedef struct {
  const char* script_source;
  const char* recorded_output;
} ArchiveEntry;

// The archived page: a saved directory listing whose rows were emitted by two
// inline scripts.
static const char kArchivedPage[] =
    "<!doctype html><title>dir</title><table>"
    "<script>emit_row(1)</script>"
    "<script>emit_row(2)</script>"
    "</table>";

// The archive's recorded output for each script, in the order the scripts
// close in the page.
static const ArchiveEntry kArchive[] = {
    {"emit_row(1)", "<tr><td>alpha</td></tr>"},
    {"emit_row(2)", "<tr><td>beta</td></tr>"},
};
static const size_t kArchiveCount = sizeof(kArchive) / sizeof(kArchive[0]);

static const char* script_text(const GumboNode* script) {
  const GumboVector* children = &script->v.element.children;
  if (!children->length) return "";
  const GumboNode* first = children->data[0];
  return first->type == GUMBO_NODE_TEXT ? first->v.text.text : "";
}

// GumboScriptReplay callback: look up the just-closed script in the archive
// and hand its recorded bytes back to the parser.
static GumboStringPiece replay_from_archive(const GumboOutput* partial,
    const GumboNode* script, void* userdata) {
  (void) partial;
  (void) userdata;
  GumboStringPiece piece = {NULL, 0};
  for (size_t i = 0; i < kArchiveCount; ++i) {
    if (!strcmp(script_text(script), kArchive[i].script_source)) {
      piece.data = kArchive[i].recorded_output;
      piece.length = strlen(kArchive[i].recorded_output);
      return piece;
    }
  }
  return piece;  // Unknown script: nothing recorded, insert nothing.
}

// Collects the text of every table cell, demonstrating that the replayed rows
// became ordinary nodes in the final tree.
static void print_cells(GumboNode* node) {
  if (node->type == GUMBO_NODE_ELEMENT || node->type == GUMBO_NODE_TEMPLATE) {
    if (node->v.element.tag == GUMBO_TAG_TD &&
        node->v.element.children.length) {
      GumboNode* text = node->v.element.children.data[0];
      if (text->type == GUMBO_NODE_TEXT) {
        printf("cell: %s\n", text->v.text.text);
      }
    }
    GumboVector* children = &node->v.element.children;
    for (unsigned int i = 0; i < children->length; ++i) {
      print_cells(children->data[i]);
    }
  } else if (node->type == GUMBO_NODE_DOCUMENT) {
    GumboVector* children = &node->v.document.children;
    for (unsigned int i = 0; i < children->length; ++i) {
      print_cells(children->data[i]);
    }
  }
}

int main(void) {
  GumboReplayOutput* result = gumbo_parse_with_script_replay(
      &kGumboDefaultOptions, kArchivedPage, sizeof(kArchivedPage) - 1,
      replay_from_archive, NULL);
  if (!result) {
    fprintf(stderr, "parse failed\n");
    return 1;
  }

  // The result owns the fully expanded source: the archived page with each
  // script's recorded output spliced in after its end tag.
  printf("expanded source: %.*s\n", (int) result->source.length,
      result->source.data);
  print_cells(result->tree->document);

  gumbo_destroy_replay_output(&kGumboDefaultOptions, result);
  return 0;
}
