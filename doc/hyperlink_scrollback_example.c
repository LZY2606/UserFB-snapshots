/* Example: saving and restoring scrollback lines that carry OSC 8
 * hyperlinks, showing exactly who owns which reference at each step.
 *
 * The ownership rules demonstrated here:
 *
 *  - VTermScreenCell.hyperlink_id values handed to sb_pushline /
 *    sb_pushline4 are BORROWED. The screen drops its own references when
 *    the line scrolls away, so a store that wants the targets to stay
 *    resolvable must call vterm_hyperlink_retain() for each identity it
 *    keeps. That creates an EXTERNAL reference owned by the store.
 *
 *  - When sb_popline hands a line back, the screen takes its own INTERNAL
 *    references for the cells it accepts. The store must not release its
 *    external references inside the callback; it may release them once
 *    vterm_set_size() (the call that triggered sb_popline) has returned.
 *    After that, the restored cells still resolve through
 *    vterm_hyperlink_get().
 *
 *  - Saved lines that are never restored only need their external
 *    references released; once the last internal/external reference is
 *    gone the library reclaims the URI and id storage.
 *
 *  - All external references must be released before vterm_free().
 *
 * Build (from the repository root):
 *   make
 *   cc -std=c99 -Iinclude doc/hyperlink_scrollback_example.c .libs/libvterm.a -lm -o sb-example
 */

#include "vterm.h"

#include <stdio.h>
#include <string.h>

#define SB_MAX_LINES 16
#define SB_MAX_COLS  80

typedef struct {
  VTerm *vt;
  int nlines;
  struct {
    int ncells;
    VTermScreenCell cells[SB_MAX_COLS];
  } lines[SB_MAX_LINES];
} ScrollbackStore;

/* sb_pushline: the terminal is about to destroy this row. The cells (and
 * their hyperlink identities) are borrowed, so retain every identity we
 * want to keep. */
static int sb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
  ScrollbackStore *sb = user;
  if(sb->nlines >= SB_MAX_LINES)
    return 1; /* store full: the targets simply die with the row */

  sb->lines[sb->nlines].ncells = cols;
  for(int i = 0; i < cols; i++) {
    sb->lines[sb->nlines].cells[i] = cells[i];
    if(cells[i].hyperlink_id)
      vterm_hyperlink_retain(sb->vt, cells[i].hyperlink_id);
  }
  sb->nlines++;
  return 1;
}

/* sb_popline: the terminal wants a line back (here: because the terminal
 * grew). Copy the saved cells out; the screen takes internal references
 * for the cells it accepts. Our external references stay with the store
 * until vterm_set_size() has returned - see main(). */
static int sb_popline(int cols, VTermScreenCell *cells, void *user)
{
  ScrollbackStore *sb = user;
  if(!sb->nlines)
    return 0;

  sb->nlines--;
  int n = sb->lines[sb->nlines].ncells < cols ? sb->lines[sb->nlines].ncells : cols;
  memcpy(cells, sb->lines[sb->nlines].cells, n * sizeof(VTermScreenCell));
  return 1;
}

/* Release the external references held for one stored line. Used both for
 * lines that were restored (after vterm_set_size() returned) and for lines
 * that will never be restored. */
static void sb_release_line(ScrollbackStore *sb, int line)
{
  for(int i = 0; i < sb->lines[line].ncells; i++)
    if(sb->lines[line].cells[i].hyperlink_id)
      vterm_hyperlink_release(sb->vt, sb->lines[line].cells[i].hyperlink_id);
}

int main(void)
{
  VTerm *vt = vterm_new(3, 10);
  vterm_set_utf8(vt, 1);
  VTermScreen *screen = vterm_obtain_screen(vt);
  vterm_screen_reset(screen, 1);

  ScrollbackStore sb = { .vt = vt, .nlines = 0 };
  VTermScreenCallbacks callbacks = {
    .sb_pushline = sb_pushline,
    .sb_popline  = sb_popline,
  };
  vterm_screen_set_callbacks(screen, &callbacks, &sb);

  /* Five linked lines on a three-row screen: the first two scroll out and
   * are captured by sb_pushline. */
  static const char *lines[] = {
    "\x1b]8;id=r1;https://example.invalid/1\x1b\\A\r\n",
    "\x1b]8;id=r2;https://example.invalid/2\x1b\\B\r\n",
    "\x1b]8;id=r3;https://example.invalid/3\x1b\\C\r\n",
    "\x1b]8;id=r4;https://example.invalid/4\x1b\\D\r\n",
    "\x1b]8;id=r5;https://example.invalid/5\x1b\\E",
  };
  for(size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
    vterm_input_write(vt, lines[i], strlen(lines[i]));

  printf("scrollback holds %d lines\n", sb.nlines); /* 2 */

  /* Remember the identities of the stored lines so we can release our
   * external references after the restore. */
  uint64_t stored[SB_MAX_LINES];
  for(int l = 0; l < sb.nlines; l++)
    stored[l] = sb.lines[l].cells[0].hyperlink_id;
  int stored_count = sb.nlines;

  /* Grow the terminal: sb_popline runs inside this call and the screen
   * takes internal references for the restored cells. */
  vterm_set_size(vt, 5, 10);

  /* Now that vterm_set_size() has returned, the store's external
   * references for the restored lines can be released. The cells on
   * screen keep the targets alive. */
  for(int l = 0; l < stored_count; l++)
    vterm_hyperlink_release(vt, stored[l]);

  /* The restored rows still resolve to their URIs. */
  for(int row = 0; row < 2; row++) {
    VTermScreenCell cell;
    vterm_screen_get_cell(screen, (VTermPos){ row, 0 }, &cell);
    const char *uri, *ext_id;
    if(vterm_hyperlink_get(vt, cell.hyperlink_id, &uri, &ext_id))
      printf("row %d: '%c' -> %s (id=%s)\n", row, cell.chars[0], uri, ext_id);
  }

  /* Any stored lines that were never restored would only need
   * sb_release_line() on them; none remain here. */
  for(int l = 0; l < sb.nlines; l++)
    sb_release_line(&sb, l);

  vterm_free(vt);
  return 0;
}
