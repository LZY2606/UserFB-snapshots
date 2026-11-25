/* evidence_test.c -- Production-path assertions for source evidence.

   These tests open the supplied ELF objects through the same
   sample_open/backtrace_dwarf_add path used by the report driver, run
   backtrace_dwarf_evidence, and compare concrete DIE identities,
   reference operands, file-table ownership and the raw bytes behind
   every span.  They do not merely check that columns exist.  */

#include "config.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backtrace.h"
#include "internal.h"
#include "sample_io.h"
#include "check_api.h"
#include "evidence_const.h"

static const char *const sample_dir
  = "support/samples";

static char path_buf[512];

static const char *
sample_path (const char *name)
{
  snprintf (path_buf, sizeof path_buf, "%s/%s", sample_dir, name);
  return path_buf;
}

/* Read a little-endian scalar of LEN bytes from the ELF file at the
   section-relative offset OFF in section SEC.  */

static uint64_t
read_le (const struct sample_object *obj, enum dwarf_section sec,
	 uint64_t off, size_t len)
{
  const unsigned char *p;
  uint64_t v = 0;
  size_t i;

  evidence_check (off + len <= obj->sections.size[sec],
		  "section offset in range");
  p = obj->sections.data[sec] + off;
  for (i = 0; i < len; ++i)
    v |= (uint64_t) p[i] << (8 * i);
  return v;
}

static const struct dwarf_evidence_frame *
frame_at_die (const struct dwarf_evidence *ev, size_t die_off)
{
  size_t i;

  for (i = 0; i < ev->frames_count; ++i)
    {
      if (ev->frames[i].concrete.die_offset == die_off)
	return &ev->frames[i];
    }
  return NULL;
}

static const struct dwarf_evidence_source *
source_of (const struct dwarf_evidence_frame *f, unsigned int key)
{
  return &f->sources[key];
}

static void
test_capture_a (void)
{
  struct sample_object a;
  const struct dwarf_evidence *ev = NULL;
  const struct dwarf_evidence_frame *fold;
  const struct dwarf_evidence_frame *mix1;
  const struct dwarf_evidence_frame *mix2;
  const struct dwarf_evidence_frame *worker;
  const struct dwarf_evidence_source *name_src;
  const struct dwarf_evidence_source *decl_file_src;
  const struct dwarf_evidence_ref *r;
  const struct dwarf_evidence_file *tf;

  evidence_check (sample_open (sample_path ("capture-A.elf"), &a),
		  "open capture-A");

  evidence_check (backtrace_dwarf_evidence (a.state, a.entry, 0x400111,
					    &ev, NULL, NULL),
		  "evidence for 0x400111");
  evidence_check (ev != NULL && ev->frames_count == 3,
		  "A 0x400111 has three frames");

  /* Innermost first, physical last, depths 2,1,0.  */
  evidence_check (ev->frames[0].depth == 2
		  && ev->frames[0].concrete.die_offset == 0x64,
		  "innermost frame is fold concrete DIE 0x64");
  evidence_check (ev->frames[2].depth == 0
		  && ev->frames[2].concrete.die_offset == 0x38,
		  "physical frame is worker DIE 0x38");

  fold = frame_at_die (ev, 0x64);
  evidence_check (fold != NULL, "find fold frame");

  /* The fold name travels abstract_origin and then two specification
     hops (the library DIE 0xc5 forwards via ref4 to 0xcf, which
     forwards via ref_addr across CUs).  */
  name_src = source_of (fold, DW_EVIDENCE_KEY_NAME);
  evidence_check (name_src->present && name_src->refs_count == 3,
		  "fold name has three reference hops");
  r = &name_src->refs[0];
  evidence_check (r->attr == EVIDENCE_AT_ABSTRACT_ORIGIN
		  && r->form == EVIDENCE_FORM_REF_ADDR
		  && r->from_die == 0x64 && r->to_die == 0xc5
		  && r->from_cu == 0 && r->to_cu == 0x90
		  && r->attr_offset == 0x65 && r->attr_length == 4,
		  "first fold hop is ref_addr 0x64 -> 0xc5");
  r = &name_src->refs[1];
  evidence_check (r->attr == EVIDENCE_AT_SPECIFICATION
		  && r->form == EVIDENCE_FORM_REF4
		  && r->from_die == 0xc5 && r->to_die == 0xcf
		  && r->from_cu == 0x90 && r->to_cu == 0x90
		  && r->attr_offset == 0xcb && r->attr_length == 4,
		  "second fold hop is local ref4 0xc5 -> 0xcf");
  r = &name_src->refs[2];
  evidence_check (r->attr == EVIDENCE_AT_SPECIFICATION
		  && r->form == EVIDENCE_FORM_REF_ADDR
		  && r->from_die == 0xcf && r->to_die == 0x100
		  && r->from_cu == 0x90 && r->to_cu == 0xd5
		  && r->attr_offset == 0xd0 && r->attr_length == 4,
		  "third fold hop is cross-CU ref_addr 0xcf -> 0x100");
  evidence_check (name_src->dies[name_src->provider_index].die_offset
		  == 0x100,
		  "fold name provider is declaration DIE 0x100");
  evidence_check (strcmp ((const char *)
			  name_src->provider_attr.string, "fold") == 0,
		  "fold provider name string");

  /* Reference operand byte readback: .debug_info[0x65..0x69) == 0xc5
     little endian.  */
  evidence_check (read_le (&a, DEBUG_INFO, 0x65, 4) == 0xc5,
		  "ref_addr operand bytes encode 0xc5");
  evidence_check (read_le (&a, DEBUG_INFO, 0x69, 1) == 2,
		  "fold concrete decl_file operand is 2");

  /* decl_file is the concrete override on DIE 0x64, value 2, and its
     file table entry belongs to the client CU line table.  */
  decl_file_src = source_of (fold, DW_EVIDENCE_KEY_DECL_FILE);
  evidence_check (decl_file_src->present
		  && decl_file_src->provider_index == 0
		  && decl_file_src->provider_attr.raw_value == 2,
		  "fold decl_file supplied by concrete DIE");
  tf = fold->decl_file_evidence;
  evidence_check (tf != NULL && tf->file_index == 2
		  && tf->file_entry_offset == 0x27
		  && tf->file_entry_length == 12
		  && tf->cu_offset == 0,
		  "fold decl_file table entry span");
  evidence_check (memcmp (a.sections.data[DEBUG_LINE]
			  + tf->file_entry_offset, "shared.h", 8) == 0,
		  "file entry bytes read back as shared.h");
  evidence_check (strcmp (tf->resolved_path,
			  "/build/a/client/shared.h") == 0,
		  "fold decl_file resolved against comp_dir");

  /* A zero decl_column is present, not missing.  */
  mix1 = frame_at_die (ev, 0x4f);
  evidence_check (mix1 != NULL, "find first mix instance");
  evidence_check (mix1->sources[DW_EVIDENCE_KEY_DECL_COLUMN].present
		  && mix1->sources[DW_EVIDENCE_KEY_DECL_COLUMN]
		     .provider_attr.raw_value == 0,
		  "mix decl_column 0 is present");

  /* Two inlined instances reuse one abstract function but remain
     distinct concrete DIEs with distinct call sites.  */
  evidence_check (backtrace_dwarf_evidence (a.state, a.entry, 0x400131,
					    &ev, NULL, NULL),
		  "evidence for 0x400131");
  mix2 = frame_at_die (ev, 0x7a);
  evidence_check (mix2 != NULL, "find second mix instance");
  evidence_check (mix1->concrete.die_offset != mix2->concrete.die_offset,
		  "same-name mix instances are different DIEs");
  evidence_check (mix1->sources[DW_EVIDENCE_KEY_CALL_LINE]
		  .provider_attr.raw_value == 19
		  && mix2->sources[DW_EVIDENCE_KEY_CALL_LINE]
		  .provider_attr.raw_value == 29,
		  "mix instances have distinct call lines");
  evidence_check (mix2->sources[DW_EVIDENCE_KEY_LOW_PC].present
		  && mix2->low_pc == 0x400130,
		  "second mix hit range low pc");

  /* The physical worker has no call coordinates.  */
  worker = frame_at_die (ev, 0x38);
  evidence_check (worker != NULL
		  && !worker->sources[DW_EVIDENCE_KEY_CALL_FILE].present
		  && !worker->sources[DW_EVIDENCE_KEY_CALL_LINE].present,
		  "physical function call site unknown");

  /* Boundary PC just past the range yields an empty result, not a
     fabricated frame.  */
  evidence_check (backtrace_dwarf_evidence (a.state, a.entry, 0x400140,
					    &ev, NULL, NULL)
		  && ev->frames_count == 0,
		  "out-of-range PC gives no frames");
}

static void
test_cross_object_isolation (void)
{
  struct sample_object a;
  struct sample_object b;
  const struct dwarf_evidence *eva = NULL;
  const struct dwarf_evidence *evb = NULL;
  const struct dwarf_evidence_frame *fa;
  const struct dwarf_evidence_frame *fb;

  evidence_check (sample_open (sample_path ("capture-A.elf"), &a),
		  "reopen capture-A");
  evidence_check (sample_open (sample_path ("capture-B.elf"), &b),
		  "open capture-B");

  evidence_check (backtrace_dwarf_evidence (a.state, a.entry, 0x400111,
					    &eva, NULL, NULL),
		  "A evidence");
  evidence_check (backtrace_dwarf_evidence (b.state, b.entry, 0x500311,
					    &evb, NULL, NULL),
		  "B evidence");

  evidence_check (eva != evb, "objects keep separate evidence records");
  evidence_check (evb->frames_count == 3, "B chain depth");

  fa = frame_at_die (eva, 0x64);
  fb = frame_at_die (evb, 0x69);
  evidence_check (fa != NULL && fb != NULL
		  && fa->concrete.die_offset != fb->concrete.die_offset,
		  "same logical fold maps to different concrete DIEs");
  evidence_check (strcmp (fa->decl_file_evidence->resolved_path,
			  "/build/a/client/shared.h") == 0
		  && strcmp (fb->decl_file_evidence->resolved_path,
			     "/build/b/client/shared.h") == 0,
		  "file table ownership follows each object CU");

  /* Repeated query returns the cached object-local record.  */
  {
    const struct dwarf_evidence *again = NULL;

    evidence_check (backtrace_dwarf_evidence (a.state, a.entry, 0x400111,
					      &again, NULL, NULL),
		    "repeated A query");
    evidence_check (again == eva, "per-object evidence cache reused");
  }

  /* Interleaving B again cannot overwrite A's view.  */
  evidence_check (backtrace_dwarf_evidence (b.state, b.entry, 0x500309,
					    &evb, NULL, NULL),
		  "interleaved B query");
  evidence_check (eva->frames[0].concrete.die_offset == 0x64,
		  "interleaving objects does not mix evidence");
}

static void
test_optimized_c (void)
{
  struct sample_object c;
  const struct dwarf_evidence *ev = NULL;
  const struct dwarf_evidence_frame *inner_mix;
  const struct dwarf_evidence_frame *fold;
  const struct dwarf_evidence_frame *worker;
  const struct dwarf_evidence_source *name_src;
  const struct dwarf_evidence_ref *r;
  const struct dwarf_evidence_file *tf;

  evidence_check (sample_open (sample_path ("optimized-C.elf"), &c),
		  "open optimized-C");

  evidence_check (backtrace_dwarf_evidence (c.state, c.entry, 0x600100,
					    &ev, NULL, NULL),
		  "C evidence for 0x600100");
  evidence_check (ev != NULL && ev->frames_count == 3,
		  "C 0x600100 has three frames");
  evidence_check (ev->frames[0].concrete.die_offset == 0xd9
		  && ev->frames[2].concrete.die_offset == 0x7c,
		  "C innermost fold and physical worker identities");

  fold = frame_at_die (ev, 0xd9);
  evidence_check (fold != NULL, "find C fold instance");

  /* CU-internal ref4 references.  */
  name_src = source_of (fold, DW_EVIDENCE_KEY_NAME);
  evidence_check (name_src->present && name_src->refs_count == 1,
		  "C fold name has one abstract_origin hop");
  r = &name_src->refs[0];
  evidence_check (r->attr == EVIDENCE_AT_ABSTRACT_ORIGIN
		  && r->form == EVIDENCE_FORM_REF4
		  && r->from_die == 0xd9 && r->to_die == 0x2a
		  && r->from_cu == 0 && r->to_cu == 0
		  && r->attr_offset == 0xda && r->attr_length == 4,
		  "C fold hop is ref4 0xd9 -> 0x2a with operand span");
  evidence_check (read_le (&c, DEBUG_INFO, 0xda, 4) == 0x2a,
		  "ref4 operand bytes encode 0x2a");

  /* strp name: raw .debug_str offset and readback bytes.  */
  evidence_check (name_src->provider_attr.form == EVIDENCE_FORM_STRP,
		  "C fold name uses strp");
  evidence_check (read_le (&c, DEBUG_STR,
			   name_src->provider_attr.raw_value, 5)
		  == 0x00646c6f66,
		  "strp offset bytes read back as fold NUL");

  /* File table with a non-zero directory index.  */
  tf = fold->decl_file_evidence;
  evidence_check (tf != NULL && tf->directory_index == 1
		  && tf->file_index == 1
		  && tf->directory_entry_offset == 0x1c
		  && tf->directory_entry_length == 8
		  && tf->file_entry_offset == 0x25
		  && tf->file_entry_length == 15,
		  "C file and directory entry spans");
  evidence_check (strcmp (tf->resolved_path,
			  "/sample/build/private/optimized.c") == 0,
		  "C relative directory joined with comp_dir");

  /* Call coordinates come from the concrete inline DIE only.  */
  inner_mix = frame_at_die (ev, 0xb3);
  evidence_check (inner_mix != NULL
		  && inner_mix->sources[DW_EVIDENCE_KEY_CALL_LINE]
		     .provider_attr.raw_value == 12
		  && inner_mix->sources[DW_EVIDENCE_KEY_CALL_COLUMN]
		     .provider_attr.raw_value == 16,
		  "C mix call site read from concrete DIE");

  worker = frame_at_die (ev, 0x7c);
  evidence_check (worker != NULL
		  && !worker->sources[DW_EVIDENCE_KEY_CALL_FILE].present,
		  "C physical worker has no call file");

  /* Second reuse of the same abstract mix, distinct concrete DIE.  */
  evidence_check (backtrace_dwarf_evidence (c.state, c.entry, 0x600117,
					    &ev, NULL, NULL),
		  "C evidence for 0x600117");
  evidence_check (ev->frames_count == 3
		  && frame_at_die (ev, 0x100) != NULL
		  && frame_at_die (ev, 0x126) != NULL,
		  "second call site reuses abstract mix and fold");
}

static void
test_reverse_order (void)
{
  struct sample_object a;
  const struct dwarf_evidence *fwd = NULL;
  const struct dwarf_evidence *rev = NULL;
  static const uintptr_t order[] =
    { 0x400100, 0x400109, 0x400111, 0x400131, 0x40013f };
  static const uintptr_t reverse[] =
    { 0x40013f, 0x400131, 0x400111, 0x400109, 0x400100 };
  size_t i;

  evidence_check (sample_open (sample_path ("capture-A.elf"), &a),
		  "open A for ordering test");

  for (i = 0; i < sizeof order / sizeof order[0]; ++i)
    evidence_check (backtrace_dwarf_evidence (a.state, a.entry, order[i],
					      &fwd, NULL, NULL),
		    "forward query");
  for (i = 0; i < sizeof reverse / sizeof reverse[0]; ++i)
    evidence_check (backtrace_dwarf_evidence (a.state, a.entry, reverse[i],
					      &rev, NULL, NULL),
		    "reverse query");
  evidence_check (fwd != NULL && rev != NULL
		  && fwd->frames_count == rev->frames_count,
		  "reverse order yields same frame count");
  evidence_check (rev->frames[0].concrete.die_offset
		  == fwd->frames[0].concrete.die_offset,
		  "reverse order selects the same innermost DIE");
}

void
evidence_tests (void)
{
  test_capture_a ();
  test_cross_object_isolation ();
  test_optimized_c ();
  test_reverse_order ();
}
