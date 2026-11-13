/* Target tests for OSC 8 hyperlink support in libvterm.
 *
 * Cases are registered through task_support/cases.h and executed by
 * task_support/driver.c in the three groups normal/boundary/integration.
 * Everything is driven through in-memory byte streams; no real terminal and
 * no URL is ever opened.
 */

#include "include/vterm.h"
#include "cases.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static void feed(VTerm *vt, const char *s)
{
  vterm_input_write(vt, s, strlen(s));
}

static void feedn(VTerm *vt, const char *s, size_t n)
{
  vterm_input_write(vt, s, n);
}

static VTerm *term_new(int rows, int cols, VTermScreen **out)
{
  VTerm *vt = vterm_new(rows, cols);
  vterm_set_utf8(vt, 1);
  VTermScreen *screen = vterm_obtain_screen(vt);
  vterm_screen_reset(screen, 1);
  if(out)
    *out = screen;
  return vt;
}

static uint64_t link_at(VTermScreen *screen, int row, int col)
{
  VTermScreenCell cell;
  if(!vterm_screen_get_cell(screen, (VTermPos){ row, col }, &cell))
    return 0;
  return cell.hyperlink_id;
}

static uint32_t char_at(VTermScreen *screen, int row, int col)
{
  VTermScreenCell cell;
  if(!vterm_screen_get_cell(screen, (VTermPos){ row, col }, &cell))
    return 0;
  return cell.chars[0];
}

static void check_link(VTerm *vt, VTermScreen *screen, int row, int col,
                       const char *want_uri, const char *want_ext)
{
  uint64_t id = link_at(screen, row, col);
  TARGET_CHECK(id != 0);
  const char *uri = NULL, *ext = NULL;
  TARGET_CHECK(vterm_hyperlink_get(vt, id, &uri, &ext) == 1);
  TARGET_CHECK(uri && strcmp(uri, want_uri) == 0);
  TARGET_CHECK(ext && strcmp(ext, want_ext) == 0);
}

/* ------------------------------------------------------------------ */
/* normal group                                                       */
/* ------------------------------------------------------------------ */

static void normal_open_close_cells(void)
{
  VTermScreen *s;
  VTerm *vt = term_new(3, 20, &s);

  feed(vt, "\x1b]8;id=rec-1;https://example.invalid/rec/1\x1b\\");
  feed(vt, "AB");
  feed(vt, "\x1b]8;;\x1b\\"); /* close */
  feed(vt, "C");

  uint64_t idA = link_at(s, 0, 0);
  TARGET_CHECK(idA != 0);
  TARGET_CHECK(link_at(s, 0, 1) == idA);  /* same run shares the identity */
  TARGET_CHECK(link_at(s, 0, 2) == 0);    /* written after the close */

  /* closing the control string must not erase links already in cells */
  {
    const char *uri = NULL, *ext = NULL;
    TARGET_CHECK(vterm_hyperlink_get(vt, idA, &uri, &ext) == 1);
    TARGET_CHECK(uri && strcmp(uri, "https://example.invalid/rec/1") == 0);
    TARGET_CHECK(ext && strcmp(ext, "rec-1") == 0);
  }

  /* unknown or zero identities fail without touching the outputs */
  {
    const char *uri = "sentinel", *ext = "sentinel2";
    TARGET_CHECK(vterm_hyperlink_get(vt, 0, &uri, &ext) == 0);
    TARGET_CHECK(vterm_hyperlink_get(vt, idA + 5000, &uri, &ext) == 0);
    TARGET_CHECK(strcmp(uri, "sentinel") == 0 && strcmp(ext, "sentinel2") == 0);
  }

  /* get_cell hands out a borrowed identity: once the cells are erased and
   * no other reference exists, the target is gone */
  feed(vt, "\x1b[2J");
  TARGET_CHECK(link_at(s, 0, 0) == 0);
  {
    const char *uri = NULL;
    TARGET_CHECK(vterm_hyperlink_get(vt, idA, &uri, NULL) == 0);
  }

  vterm_free(vt);
}

static void normal_identity_reuse(void)
{
  VTermScreen *s;
  VTerm *vt = term_new(3, 20, &s);

  /* same nonempty id= and same URI reuse the identity while it is alive */
  feed(vt, "\x1b]8;id=K;https://e.invalid/u\x1b\\A");
  feed(vt, "\x1b]8;;\x1b\\");
  feed(vt, "\x1b]8;id=K;https://e.invalid/u\x1b\\B");
  uint64_t idK = link_at(s, 0, 0);
  TARGET_CHECK(idK != 0);
  TARGET_CHECK(link_at(s, 0, 1) == idK);

  /* same URI but a different explicit id is a different identity */
  feed(vt, "\x1b]8;;\x1b\\");
  feed(vt, "\x1b]8;id=J;https://e.invalid/u\x1b\\C");
  uint64_t idJ = link_at(s, 0, 2);
  TARGET_CHECK(idJ != 0 && idJ != idK);

  /* same id but a different URI is a different identity */
  feed(vt, "\x1b]8;;\x1b\\");
  feed(vt, "\x1b]8;id=K;https://e.invalid/other\x1b\\D");
  TARGET_CHECK(link_at(s, 0, 3) != idK);

  /* anonymous links never reuse: each nonempty open is a fresh identity */
  feed(vt, "\x1b]8;;\x1b\\");
  feed(vt, "\x1b]8;;https://e.invalid/u\x1b\\E");
  feed(vt, "\x1b]8;;\x1b\\");
  feed(vt, "\x1b]8;;https://e.invalid/u\x1b\\F");
  uint64_t idE = link_at(s, 0, 4);
  uint64_t idF = link_at(s, 0, 5);
  TARGET_CHECK(idE != 0 && idF != 0 && idE != idF);

  /* anonymous links report an empty external id */
  {
    const char *uri = NULL, *ext = NULL;
    TARGET_CHECK(vterm_hyperlink_get(vt, idE, &uri, &ext) == 1);
    TARGET_CHECK(uri && strcmp(uri, "https://e.invalid/u") == 0);
    TARGET_CHECK(ext && strcmp(ext, "") == 0);
  }

  /* unrelated params are ignored; the first id= wins; ':' splits params */
  feed(vt, "\x1b]8;;\x1b\\");
  feed(vt, "\x1b]8;foo=bar:id=win:baz=1:id=lose;https://e.invalid/p\x1b\\G");
  check_link(vt, s, 0, 6, "https://e.invalid/p", "win");

  /* an empty id= value counts as no explicit id */
  feed(vt, "\x1b]8;;\x1b\\");
  feed(vt, "\x1b]8;id=;https://e.invalid/emptyid\x1b\\H");
  {
    const char *uri = NULL, *ext = NULL;
    TARGET_CHECK(vterm_hyperlink_get(vt, link_at(s, 0, 7), &uri, &ext) == 1);
    TARGET_CHECK(ext && strcmp(ext, "") == 0);
  }

  /* semicolons inside the URI are literal, not new parameter separators */
  feed(vt, "\x1b]8;;\x1b\\");
  feed(vt, "\x1b]8;id=semi;https://e.invalid/a;b;c\x1b\\I");
  check_link(vt, s, 0, 8, "https://e.invalid/a;b;c", "semi");

  /* once every reference is gone the identity value itself is retired:
   * reopening the same id=/URI pair mints a fresh identity */
  feed(vt, "\x1b[2J");        /* drop all cell references */
  feed(vt, "\x1b]8;;\x1b\\"); /* make sure the pen holds none either */
  {
    const char *uri = NULL;
    TARGET_CHECK(vterm_hyperlink_get(vt, idK, &uri, NULL) == 0);
  }
  feed(vt, "\x1b[H");
  feed(vt, "\x1b]8;id=K;https://e.invalid/u\x1b\\Z");
  uint64_t idK2 = link_at(s, 0, 0);
  TARGET_CHECK(idK2 != 0 && idK2 != idK);
  check_link(vt, s, 0, 0, "https://e.invalid/u", "K");

  vterm_free(vt);
}

static void normal_reset_modes(void)
{
  VTermScreen *s;
  VTerm *vt = term_new(2, 10, &s);

  feed(vt, "\x1b]8;id=s1;https://e.invalid/s1\x1b\\");
  feed(vt, "\x1b" "7"); /* DECSC: save cursor */
  feed(vt, "\x1b]8;id=s2;https://e.invalid/s2\x1b\\");
  feed(vt, "\x1b" "8"); /* DECRC: restore must not bring the old link back */
  feed(vt, "Q");
  check_link(vt, s, 0, 0, "https://e.invalid/s2", "s2");

  /* SGR reset does not close the active link */
  feed(vt, "\x1b[1;31m\x1b[0m");
  feed(vt, "R");
  check_link(vt, s, 0, 1, "https://e.invalid/s2", "s2");

  /* DECSTR soft reset closes it */
  feed(vt, "\x1b[!p");
  feed(vt, "S");
  TARGET_CHECK(link_at(s, 0, 2) == 0);

  /* RIS hard reset closes it (and wipes the screen) */
  feed(vt, "\x1b]8;id=s3;https://e.invalid/s3\x1b\\");
  feed(vt, "\x1b" "c");
  feed(vt, "T");
  TARGET_CHECK(link_at(s, 0, 0) == 0);
  TARGET_CHECK(char_at(s, 0, 1) == 0);

  vterm_free(vt);
}

static int fb_seen_other, fb_seen_osc8;
static int fb_osc(int command, VTermStringFragment frag, void *user)
{
  (void)frag; (void)user;
  if(command == 8)
    fb_seen_osc8++;
  else
    fb_seen_other++;
  return 1;
}

static void normal_unknown_osc_fallback(void)
{
  VTermScreen *s;
  VTerm *vt = term_new(2, 10, &s);

  VTermStateFallbacks fb = { .osc = fb_osc };
  vterm_state_set_unrecognised_fallbacks(vterm_obtain_state(vt), &fb, NULL);

  fb_seen_other = fb_seen_osc8 = 0;

  /* unknown control strings still reach the fallback exactly as before */
  feed(vt, "\x1b]99;something\x1b\\");
  TARGET_CHECK(fb_seen_other == 1);

  /* OSC 8 is now recognised and no longer falls through */
  feed(vt, "\x1b]8;id=q;https://e.invalid/q\x1b\\");
  TARGET_CHECK(fb_seen_osc8 == 0);
  TARGET_CHECK(fb_seen_other == 1);

  feed(vt, "Z");
  check_link(vt, s, 0, 0, "https://e.invalid/q", "q");

  vterm_free(vt);
}

/* ------------------------------------------------------------------ */
/* boundary group                                                     */
/* ------------------------------------------------------------------ */

static void boundary_split_points(void)
{
  /* ST-terminated, with an explicit id */
  static const char seq_st[] = "\x1b]8;id=sp;https://e.invalid/split\x1b\\Z";
  const size_t len_st = sizeof(seq_st) - 1;

  for(size_t cut = 0; cut <= len_st; cut++) {
    VTermScreen *s;
    VTerm *vt = term_new(2, 10, &s);
    feedn(vt, seq_st, cut);
    feedn(vt, seq_st + cut, len_st - cut);
    check_link(vt, s, 0, 0, "https://e.invalid/split", "sp");
    TARGET_CHECK(char_at(s, 0, 0) == 'Z');
    vterm_free(vt);
  }

  /* BEL-terminated, anonymous */
  static const char seq_bel[] = "\x1b]8;;https://e.invalid/bel\x07Q";
  const size_t len_bel = sizeof(seq_bel) - 1;

  for(size_t cut = 0; cut <= len_bel; cut++) {
    VTermScreen *s;
    VTerm *vt = term_new(2, 10, &s);
    feedn(vt, seq_bel, cut);
    feedn(vt, seq_bel + cut, len_bel - cut);
    check_link(vt, s, 0, 0, "https://e.invalid/bel", "");
    TARGET_CHECK(char_at(s, 0, 0) == 'Q');
    vterm_free(vt);
  }

  /* one byte at a time, mixing both terminators */
  {
    VTermScreen *s;
    VTerm *vt = term_new(2, 10, &s);
    static const char seq[] = "\x1b]8;id=one;https://e.invalid/1\x07X"
                              "\x1b]8;;\x1b\\"
                              "\x1b]8;id=two;https://e.invalid/2\x1b\\Y";
    for(size_t i = 0; i < sizeof(seq) - 1; i++)
      feedn(vt, seq + i, 1);
    check_link(vt, s, 0, 0, "https://e.invalid/1", "one");
    check_link(vt, s, 0, 1, "https://e.invalid/2", "two");
    vterm_free(vt);
  }
}

static void boundary_cancel_malformed(void)
{
  VTermScreen *s;
  VTerm *vt = term_new(2, 30, &s);

  feed(vt, "\x1b]8;id=keep;https://e.invalid/keep\x1b\\");
  feed(vt, "A");
  uint64_t keep = link_at(s, 0, 0);
  TARGET_CHECK(keep != 0);
  int col = 1;

  /* a half-written string aborted by CAN changes nothing */
  feed(vt, "\x1b]8;id=bad;https://bad.invalid/");
  feed(vt, "\x18");
  feed(vt, "B");
  TARGET_CHECK(link_at(s, 0, col++) == keep);

  /* same for SUB */
  feed(vt, "\x1b]8;id=bad;https://bad.invalid/");
  feed(vt, "\x1a");
  feed(vt, "C");
  TARGET_CHECK(link_at(s, 0, col++) == keep);

  /* same for ESC starting another sequence; the parser cancels the string
   * and (per its existing behaviour) prints the 'm' as text */
  feed(vt, "\x1b]8;id=bad;https://bad.invalid/");
  feed(vt, "\x1b[m");
  TARGET_CHECK(link_at(s, 0, col) == keep && char_at(s, 0, col) == 'm');
  col++;
  feed(vt, "D");
  TARGET_CHECK(link_at(s, 0, col++) == keep);

  /* missing the params/URI separator */
  feed(vt, "\x1b]8;idonly\x1b\\");
  feed(vt, "E");
  TARGET_CHECK(link_at(s, 0, col++) == keep);

  /* nothing at all after "8;" */
  feed(vt, "\x1b]8;\x1b\\");
  feed(vt, "F");
  TARGET_CHECK(link_at(s, 0, col++) == keep);

  /* NUL inside the URI rejects the whole string */
  {
    static const char nul_uri[] = "\x1b]8;id=nul;https://n\x00ul\x1b\\";
    feedn(vt, nul_uri, sizeof(nul_uri) - 1);
  }
  feed(vt, "G");
  TARGET_CHECK(link_at(s, 0, col++) == keep);

  /* NUL inside the params rejects it too */
  {
    static const char nul_par[] = "\x1b]8;id\x00=x;https://u\x1b\\";
    feedn(vt, nul_par, sizeof(nul_par) - 1);
  }
  feed(vt, "H");
  TARGET_CHECK(link_at(s, 0, col++) == keep);

  /* a well-formed string right after the rejected ones still works */
  feed(vt, "\x1b]8;id=ok;https://e.invalid/ok\x1b\\");
  feed(vt, "I");
  check_link(vt, s, 0, col++, "https://e.invalid/ok", "ok");

  /* empty URI closes the link; cells written earlier keep theirs */
  feed(vt, "\x1b]8;;\x1b\\");
  feed(vt, "J");
  TARGET_CHECK(link_at(s, 0, col++) == 0);
  TARGET_CHECK(link_at(s, 0, 0) == keep);

  vterm_free(vt);
}

static void boundary_overlong(void)
{
  VTermScreen *s;
  VTerm *vt = term_new(2, 10, &s);

  static char big[4300];

  /* params + URI of exactly 4096 bytes is accepted: "id=z" is 4 bytes of
   * params, so a 4092-byte URI reaches the limit exactly */
  {
    size_t n = 0;
    memcpy(big, "\x1b]8;id=z;", 9); n += 9;
    memset(big + n, 'u', 4092);   n += 4092;
    memcpy(big + n, "\x1b\\", 2); n += 2;
    feedn(vt, big, n);
  }
  feed(vt, "M");
  uint64_t idz = link_at(s, 0, 0);
  TARGET_CHECK(idz != 0);
  {
    const char *uri = NULL;
    TARGET_CHECK(vterm_hyperlink_get(vt, idz, &uri, NULL) == 1);
    TARGET_CHECK(uri && strlen(uri) == 4092);
  }

  /* one byte more discards the whole string; the active link is unchanged.
   * Feed it in odd chunks to prove the limit applies to the reassembled
   * stream, not to individual writes */
  {
    size_t n = 0;
    memcpy(big, "\x1b]8;id=y;", 9); n += 9;
    memset(big + n, 'v', 4093);   n += 4093;
    memcpy(big + n, "\x1b\\", 2); n += 2;
    for(size_t i = 0; i < n; i += 7)
      feedn(vt, big + i, (n - i < 7) ? n - i : 7);
  }
  feed(vt, "N");
  TARGET_CHECK(link_at(s, 0, 1) == idz);

  /* an overlong close is discarded too: the link stays open */
  {
    size_t n = 0;
    memcpy(big, "\x1b]8;", 4); n += 4;
    memset(big + n, 'p', 4100);   n += 4100;
    memcpy(big + n, ";\x1b\\", 3); n += 3;
    feedn(vt, big, n);
  }
  feed(vt, "O");
  TARGET_CHECK(link_at(s, 0, 2) == idz);

  /* a legal string right after the discarded ones still takes effect */
  feed(vt, "\x1b]8;;\x1b\\");
  feed(vt, "P");
  TARGET_CHECK(link_at(s, 0, 3) == 0);
  feed(vt, "\x1b]8;id=after;https://e.invalid/after\x1b\\");
  feed(vt, "Q");
  check_link(vt, s, 0, 4, "https://e.invalid/after", "after");

  vterm_free(vt);
}

/* counting allocator to prove hyperlink storage is reclaimed once the last
 * reference is gone */
typedef struct {
  unsigned long allocs;
  unsigned long frees;
} CountAlloc;

static void *count_malloc(size_t size, void *data)
{
  CountAlloc *ca = data;
  ca->allocs++;
  void *p = malloc(size);
  if(p)
    memset(p, 0, size); /* libvterm relies on zeroed allocations */
  return p;
}

static void count_free(void *ptr, void *data)
{
  CountAlloc *ca = data;
  if(ptr)
    ca->frees++;
  free(ptr);
}

static void boundary_refcount_reclaim(void)
{
  CountAlloc ca = { 0, 0 };
  VTermAllocatorFunctions funcs = { count_malloc, count_free };

  VTerm *vt = vterm_new_with_allocator(3, 10, &funcs, &ca);
  vterm_set_utf8(vt, 1);
  VTermScreen *s = vterm_obtain_screen(vt);
  vterm_screen_reset(s, 1);

  unsigned long live_at_start = ca.allocs - ca.frees;

  feed(vt, "\x1b]8;id=rc;https://rc.invalid/x\x1b\\");
  feed(vt, "AB");
  feed(vt, "\x1b]8;;\x1b\\"); /* close: drops the pen's reference */

  /* entry + uri + external id strings are the only new live allocations */
  TARGET_CHECK(ca.allocs - ca.frees == live_at_start + 3);

  uint64_t id = link_at(s, 0, 0);
  TARGET_CHECK(id != 0);
  TARGET_CHECK(vterm_hyperlink_retain(vt, id) == 1);

  /* erasing the cells drops their internal references; the external
   * reference keeps the target alive */
  feed(vt, "\x1b[2J");
  TARGET_CHECK(link_at(s, 0, 0) == 0);
  TARGET_CHECK(ca.allocs - ca.frees == live_at_start + 3);
  {
    const char *uri = NULL, *ext = NULL;
    TARGET_CHECK(vterm_hyperlink_get(vt, id, &uri, &ext) == 1);
    TARGET_CHECK(uri && strcmp(uri, "https://rc.invalid/x") == 0);
    TARGET_CHECK(ext && strcmp(ext, "rc") == 0);
  }

  /* releasing the last reference reclaims the URI and id storage */
  TARGET_CHECK(vterm_hyperlink_release(vt, id) == 1);
  TARGET_CHECK(ca.allocs - ca.frees == live_at_start);
  TARGET_CHECK(vterm_hyperlink_get(vt, id, NULL, NULL) == 0);

  /* nothing left to release: a second release fails instead of
   * underflowing into somebody else's references */
  TARGET_CHECK(vterm_hyperlink_release(vt, id) == 0);
  TARGET_CHECK(vterm_hyperlink_release(vt, 0) == 0);

  /* release must not touch internal references held by cells or the pen */
  feed(vt, "\x1b]8;id=in;https://rc.invalid/internal\x1b\\");
  feed(vt, "Z"); /* pen still open: one pen ref + one cell ref */
  uint64_t id2 = link_at(s, 0, 2);
  TARGET_CHECK(id2 != 0);
  TARGET_CHECK(vterm_hyperlink_release(vt, id2) == 0);
  TARGET_CHECK(vterm_hyperlink_get(vt, id2, NULL, NULL) == 1);

  /* unknown identities cannot be retained or released */
  TARGET_CHECK(vterm_hyperlink_retain(vt, id2 + 777) == 0);
  TARGET_CHECK(vterm_hyperlink_release(vt, id2 + 777) == 0);

  vterm_free(vt);

  /* vterm_free tears down every remaining internal reference */
  TARGET_CHECK(ca.allocs == ca.frees);
}

/* ------------------------------------------------------------------ */
/* integration group                                                  */
/* ------------------------------------------------------------------ */

static void integration_wide_combine_protect(void)
{
  VTermScreen *s;
  VTerm *vt = term_new(4, 10, &s);

  /* a wide glyph: both the head cell and the trailing cell carry the link */
  feed(vt, "\x1b]8;id=w;https://e.invalid/wide\x1b\\");
  feed(vt, "\xe4\xb8\xad"); /* U+4E2D, width 2 */
  feed(vt, "\x1b]8;;\x1b\\");
  uint64_t idw = link_at(s, 0, 0);
  TARGET_CHECK(idw != 0);
  TARGET_CHECK(link_at(s, 0, 1) == idw);
  TARGET_CHECK(char_at(s, 0, 0) == 0x4E2D);

  /* a combining mark joins the glyph's original link; it must not rewrite
   * the cell to the pen's new active link */
  feed(vt, "\x1b]8;id=c1;https://e.invalid/first\x1b\\");
  feed(vt, "e");
  feed(vt, "\x1b]8;;\x1b\\");
  feed(vt, "\x1b]8;id=c2;https://e.invalid/second\x1b\\");
  feed(vt, "\xcc\x81"); /* U+0301 combining acute */
  check_link(vt, s, 0, 2, "https://e.invalid/first", "c1");
  {
    VTermScreenCell cell;
    TARGET_CHECK(vterm_screen_get_cell(s, (VTermPos){ 0, 2 }, &cell));
    TARGET_CHECK(cell.chars[0] == 'e' && cell.chars[1] == 0x0301);
  }
  feed(vt, "\x1b]8;;\x1b\\");

  /* a DECSCA-protected cell survives selective erase with its link */
  feed(vt, "\x1b[1\"q"); /* DECSCA: protect on */
  feed(vt, "\x1b]8;id=p;https://e.invalid/prot\x1b\\");
  feed(vt, "P");
  feed(vt, "\x1b]8;;\x1b\\");
  feed(vt, "\x1b[0\"q"); /* DECSCA: protect off */
  feed(vt, "\x1b[?2J");  /* DECSED: selective erase in display */
  TARGET_CHECK(char_at(s, 0, 0) == 0);
  TARGET_CHECK(link_at(s, 0, 0) == 0);
  TARGET_CHECK(char_at(s, 0, 3) == 'P');
  check_link(vt, s, 0, 3, "https://e.invalid/prot", "p");

  /* a plain erase takes the protected cell (and its link) out too */
  feed(vt, "\x1b[2J");
  TARGET_CHECK(char_at(s, 0, 3) == 0);
  TARGET_CHECK(link_at(s, 0, 3) == 0);

  /* blanks produced by erasure carry no link even while the pen has one */
  feed(vt, "\x1b]8;id=b;https://e.invalid/blank\x1b\\");
  feed(vt, "XYZ"); /* cursor was at column 4 */
  TARGET_CHECK(link_at(s, 0, 5) != 0);
  feed(vt, "\x1b[1K"); /* erase from line start to cursor */
  TARGET_CHECK(char_at(s, 0, 5) == 0);
  TARGET_CHECK(link_at(s, 0, 5) == 0);
  feed(vt, "\x1b]8;;\x1b\\");

  /* deleting/inserting characters moves existing cells with their links;
   * the blanks born from the insertion have none */
  feed(vt, "\x1b[H");
  feed(vt, "\x1b]8;id=m;https://e.invalid/move\x1b\\");
  feed(vt, "ABC");
  feed(vt, "\x1b]8;;\x1b\\");
  uint64_t idm = link_at(s, 0, 1);
  TARGET_CHECK(idm != 0);
  feed(vt, "\x1b[H");
  feed(vt, "\x1b[P"); /* DCH: delete the 'A', the rest shifts left */
  TARGET_CHECK(char_at(s, 0, 0) == 'B');
  TARGET_CHECK(link_at(s, 0, 0) == idm);
  TARGET_CHECK(link_at(s, 0, 1) == idm);
  feed(vt, "\x1b[@"); /* ICH: insert a blank at the cursor */
  TARGET_CHECK(char_at(s, 0, 1) == 'B');
  TARGET_CHECK(link_at(s, 0, 1) == idm);
  TARGET_CHECK(link_at(s, 0, 0) == 0);

  vterm_free(vt);
}

/* scrollback store used by the next two cases: the pushline callbacks take
 * an external reference for every identity they keep */
#define SB_MAX 8
#define SB_COLS 32
typedef struct {
  VTerm *vt;
  int nlines;
  struct {
    int ncells;
    VTermScreenCell cells[SB_COLS];
  } lines[SB_MAX];
} SbStore;

static void sb_save(SbStore *sb, int cols, const VTermScreenCell *cells)
{
  if(sb->nlines >= SB_MAX)
    return;
  sb->lines[sb->nlines].ncells = cols;
  for(int i = 0; i < cols; i++) {
    sb->lines[sb->nlines].cells[i] = cells[i];
    if(cells[i].hyperlink_id)
      TARGET_CHECK(vterm_hyperlink_retain(sb->vt, cells[i].hyperlink_id) == 1);
  }
  sb->nlines++;
}

static int sb_pushline_cb(int cols, const VTermScreenCell *cells, void *user)
{
  sb_save(user, cols, cells);
  return 1;
}

static int sb_pushline4_cb(int cols, const VTermScreenCell *cells, bool continuation, void *user)
{
  (void)continuation;
  sb_save(user, cols, cells);
  return 1;
}

static int sb_popline_cb(int cols, VTermScreenCell *cells, void *user)
{
  SbStore *sb = user;
  if(!sb->nlines)
    return 0;
  sb->nlines--;
  int n = sb->lines[sb->nlines].ncells < cols ? sb->lines[sb->nlines].ncells : cols;
  memcpy(cells, sb->lines[sb->nlines].cells, n * sizeof(VTermScreenCell));
  /* the screen takes its own internal references for the accepted cells;
   * our external references are released by the test afterwards */
  return 1;
}

static void sb_release_all(SbStore *sb)
{
  for(int l = 0; l < sb->nlines; l++)
    for(int c = 0; c < sb->lines[l].ncells; c++)
      if(sb->lines[l].cells[c].hyperlink_id)
        TARGET_CHECK(vterm_hyperlink_release(sb->vt, sb->lines[l].cells[c].hyperlink_id) == 1);
  sb->nlines = 0;
}

static void integration_scroll_altscreen(void)
{
  VTermScreen *s;
  VTerm *vt = term_new(3, 10, &s);

  SbStore sb = { .vt = vt, .nlines = 0 };
  VTermScreenCallbacks cbs = {
    .sb_pushline = sb_pushline_cb,
    .sb_popline  = sb_popline_cb,
  };
  vterm_screen_set_callbacks(s, &cbs, &sb);
  vterm_screen_enable_altscreen(s, 1);

  /* three lines under three links; the third linefeed scrolls the first
   * line into the scrollback store */
  feed(vt, "\x1b]8;id=L1;https://e.invalid/1\x1b\\A\r\n");
  feed(vt, "\x1b]8;id=L2;https://e.invalid/2\x1b\\B\r\n");
  feed(vt, "\x1b]8;id=L3;https://e.invalid/3\x1b\\C\r\n");

  TARGET_CHECK(sb.nlines == 1);
  uint64_t idL1 = sb.lines[0].cells[0].hyperlink_id;
  TARGET_CHECK(idL1 != 0);
  {
    const char *uri = NULL, *ext = NULL;
    TARGET_CHECK(vterm_hyperlink_get(vt, idL1, &uri, &ext) == 1);
    TARGET_CHECK(uri && strcmp(uri, "https://e.invalid/1") == 0);
    TARGET_CHECK(ext && strcmp(ext, "L1") == 0);
  }

  /* scrolling moved the surviving cells together with their links */
  uint64_t idL2 = link_at(s, 0, 0);
  uint64_t idL3 = link_at(s, 1, 0);
  TARGET_CHECK(idL2 != 0 && idL3 != 0 && idL2 != idL1 && idL3 != idL1);
  TARGET_CHECK(char_at(s, 0, 0) == 'B');
  TARGET_CHECK(char_at(s, 1, 0) == 'C');

  /* switch to the altscreen: it starts blank, the active link (L3) is not
   * implicitly changed by the switch */
  feed(vt, "\x1b[?1049h");
  TARGET_CHECK(link_at(s, 0, 0) == 0);
  feed(vt, "D"); /* cursor was saved/restored at row 2 */
  TARGET_CHECK(link_at(s, 2, 0) == idL3);
  feed(vt, "\x1b]8;id=L4;https://e.invalid/4\x1b\\");
  feed(vt, "E");
  uint64_t idL4 = link_at(s, 2, 1);
  TARGET_CHECK(idL4 != 0 && idL4 != idL3 && idL4 != idL2);

  /* back on the primary screen the old content kept its own identities */
  feed(vt, "\x1b[?1049l");
  TARGET_CHECK(link_at(s, 0, 0) == idL2);
  TARGET_CHECK(link_at(s, 1, 0) == idL3);
  TARGET_CHECK(char_at(s, 0, 0) == 'B');

  /* re-entering the altscreen erases it (upstream behaviour); the erased
   * cells' references are dropped, and the primary screen is still intact
   * and unmixed when we come back */
  feed(vt, "\x1b[?1049h");
  TARGET_CHECK(link_at(s, 2, 0) == 0);
  TARGET_CHECK(char_at(s, 2, 0) == 0);
  feed(vt, "\x1b[?1049l");
  TARGET_CHECK(link_at(s, 0, 0) == idL2);
  TARGET_CHECK(link_at(s, 1, 0) == idL3);
  TARGET_CHECK(char_at(s, 0, 0) == 'B');
  TARGET_CHECK(char_at(s, 1, 0) == 'C');

  sb_release_all(&sb);
  vterm_free(vt);
}

static void integration_scrollback_resize(void)
{
  VTermScreen *s;
  VTerm *vt = term_new(3, 10, &s);

  SbStore sb = { .vt = vt, .nlines = 0 };
  VTermScreenCallbacks cbs = {
    .sb_pushline4 = sb_pushline4_cb,
    .sb_popline   = sb_popline_cb,
  };
  vterm_screen_callbacks_has_pushline4(s);
  vterm_screen_set_callbacks(s, &cbs, &sb);

  /* five one-glyph lines, each under its own link; the first two scroll
   * into the scrollback store */
  feed(vt, "\x1b]8;id=L1;https://e.invalid/1\x1b\\A\r\n");
  feed(vt, "\x1b]8;id=L2;https://e.invalid/2\x1b\\B\r\n");
  feed(vt, "\x1b]8;id=L3;https://e.invalid/3\x1b\\C\r\n");
  feed(vt, "\x1b]8;id=L4;https://e.invalid/4\x1b\\D\r\n");
  feed(vt, "\x1b]8;id=L5;https://e.invalid/5\x1b\\E");

  TARGET_CHECK(sb.nlines == 2);
  uint64_t id1 = sb.lines[0].cells[0].hyperlink_id;
  uint64_t id2 = sb.lines[1].cells[0].hyperlink_id;
  TARGET_CHECK(id1 != 0 && id2 != 0 && id1 != id2);
  uint64_t id5 = link_at(s, 2, 0);
  TARGET_CHECK(id5 != 0);

  /* growing the terminal pops the saved lines back; the screen takes its
   * own internal references for the accepted cells */
  vterm_set_size(vt, 5, 10);
  TARGET_CHECK(sb.nlines == 0);

  /* the callback's external references may be released once vterm_set_size
   * has returned; the restored cells must still resolve */
  TARGET_CHECK(vterm_hyperlink_release(vt, id1) == 1);
  TARGET_CHECK(vterm_hyperlink_release(vt, id2) == 1);
  check_link(vt, s, 0, 0, "https://e.invalid/1", "L1");
  check_link(vt, s, 1, 0, "https://e.invalid/2", "L2");
  check_link(vt, s, 4, 0, "https://e.invalid/5", "L5");

  /* shrinking again pushes the top lines back out with their identities */
  vterm_set_size(vt, 3, 10);
  TARGET_CHECK(sb.nlines == 2);
  TARGET_CHECK(sb.lines[0].cells[0].hyperlink_id == id1);
  TARGET_CHECK(sb.lines[1].cells[0].hyperlink_id == id2);
  TARGET_CHECK(link_at(s, 2, 0) == id5);
  sb_release_all(&sb);

  vterm_free(vt);

  /* reflow on resize keeps the links of surviving cells */
  vt = term_new(2, 10, &s);
  vterm_screen_enable_reflow(s, true);
  feed(vt, "\x1b]8;id=rf;https://e.invalid/reflow\x1b\\");
  feed(vt, "ABCDEFGHIJABCDEFGHIJ"); /* wraps onto a continuation row */
  uint64_t idrf = link_at(s, 0, 0);
  TARGET_CHECK(idrf != 0);
  TARGET_CHECK(link_at(s, 1, 9) == idrf);

  vterm_set_size(vt, 1, 20); /* unwraps into a single row */
  for(int col = 0; col < 20; col++)
    TARGET_CHECK(link_at(s, 0, col) == idrf);
  TARGET_CHECK(char_at(s, 0, 19) == 'J');

  vterm_set_size(vt, 4, 5); /* re-wraps into four rows */
  TARGET_CHECK(link_at(s, 0, 0) == idrf);
  TARGET_CHECK(link_at(s, 3, 4) == idrf);
  TARGET_CHECK(char_at(s, 3, 4) == 'J');

  vterm_free(vt);
}

/* ------------------------------------------------------------------ */
/* registration                                                       */
/* ------------------------------------------------------------------ */

const TargetCase target_cases[] = {
  { "normal",      "open_close_cells",        normal_open_close_cells        },
  { "normal",      "identity_reuse",          normal_identity_reuse          },
  { "normal",      "reset_modes",             normal_reset_modes             },
  { "normal",      "unknown_osc_fallback",    normal_unknown_osc_fallback    },
  { "boundary",    "split_points",            boundary_split_points          },
  { "boundary",    "cancel_malformed",        boundary_cancel_malformed      },
  { "boundary",    "overlong",                boundary_overlong              },
  { "boundary",    "refcount_reclaim",        boundary_refcount_reclaim      },
  { "integration", "wide_combine_protect",    integration_wide_combine_protect },
  { "integration", "scroll_altscreen",        integration_scroll_altscreen   },
  { "integration", "scrollback_resize",       integration_scrollback_resize  },
};

const size_t target_count = sizeof(target_cases) / sizeof(target_cases[0]);
