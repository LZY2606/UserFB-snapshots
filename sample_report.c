/* sample_report.c -- Serialize libbacktrace source evidence.

   This driver only turns the structured evidence produced by the
   production library (backtrace_dwarf_evidence) into the tab separated
   protocol described in support/FORMAT.md.  It does not parse DWARF
   itself: every identity, range and byte span comes from the library's
   parsed state.  */

#include "config.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backtrace.h"
#include "internal.h"
#include "sample_io.h"
#include "report_api.h"
#include "evidence_const.h"

/* Names for enum dwarf_evidence_key, indexed by the enum.  */

static const char *const key_names[DW_EVIDENCE_KEY_COUNT] =
{
  "name",
  "decl_file",
  "decl_line",
  "decl_column",
  "call_file",
  "call_line",
  "call_column",
  "low_pc",
  "high_pc"
};

struct report_ctx
{
  FILE *out;
  int failed;
};

static void
report_error (void *data, const char *msg, int errnum)
{
  struct report_ctx *ctx = (struct report_ctx *) data;

  ctx->failed = 1;
  fprintf (stderr, "sample_report: %s (%d)\n", msg, errnum);
}

/* Append STR to OUT with the FORMAT.md escaping: whitespace, '%', tab
   and newline are written as %HH.  */

static void
emit_escaped (FILE *out, const char *str)
{
  const unsigned char *p = (const unsigned char *) str;

  while (*p != 0)
    {
      unsigned char c = *p;

      if (c == '%' || c == ' ' || c == '\t' || c == '\n' || c == '\r'
	  || c < 0x20 || c == 0x7f)
	fprintf (out, "%%%02X", c);
      else
	fputc (c, out);
      ++p;
    }
}

static void
emit_string_or_unknown (FILE *out, const char *str)
{
  if (str == NULL)
    fputs ("-", out);
  else
    emit_escaped (out, str);
}

/* Emit a section offset, length or address as 0x-prefixed hex so the
   value is unambiguous to the FORMAT.md parser.  */

static void
emit_x (FILE *out, uint64_t value)
{
  fprintf (out, "0x%" PRIx64, value);
}

/* Decode a name attribute into a NUL-terminated borrowed string.  */

static const char *
evidence_string (const struct dwarf_evidence_attr *attr)
{
  if (attr->encoding == DW_EVIDENCE_ENCODING_INLINE_STRING
      || attr->encoding == DW_EVIDENCE_ENCODING_STRP)
    return (const char *) attr->string;
  return NULL;
}

/* Write the A record for one resolved attribute.  */

static void
emit_attr (struct report_ctx *ctx, unsigned int qi, unsigned int depth,
	   unsigned int key, const struct dwarf_evidence_source *src)
{
  const struct dwarf_evidence_die *provider;
  const struct dwarf_evidence_attr *attr;
  FILE *out = ctx->out;

  if (!src->present)
    return;

  provider = &src->dies[src->provider_index];
  attr = &src->provider_attr;

  fprintf (out, "A\t%u\t%u\t%s\t", qi, depth, key_names[key]);
  emit_x (out, provider->cu_offset);
  fputc ('\t', out);
  emit_x (out, provider->die_offset);
  fputc ('\t', out);
  emit_x (out, attr->offset);
  fputc ('\t', out);
  emit_x (out, attr->length);
  fputc ('\t', out);
  emit_x (out, attr->form);
  fputc ('\t', out);

  if (attr->encoding == DW_EVIDENCE_ENCODING_INLINE_STRING)
    emit_escaped (out, (const char *) attr->string);
  else
    emit_x (out, attr->raw_value);
  fputc ('\n', out);
}

/* Write the R records for the reference path of one attribute.  */

static void
emit_refs (struct report_ctx *ctx, unsigned int qi, unsigned int depth,
	   unsigned int key, const struct dwarf_evidence_source *src)
{
  size_t i;
  FILE *out = ctx->out;

  for (i = 0; i < src->refs_count; ++i)
    {
      const struct dwarf_evidence_ref *r = &src->refs[i];

      fprintf (out,
	       "R\t%u\t%u\t%s\t",
	       qi, depth, key_names[key]);
      emit_x (out, i);
      fprintf (out, "\t%s\t",
	       r->attr == EVIDENCE_AT_ABSTRACT_ORIGIN
	       ? "abstract_origin" : "specification");
      emit_x (out, r->from_cu);
      fputc ('\t', out);
      emit_x (out, r->from_die);
      fputc ('\t', out);
      emit_x (out, r->attr_offset);
      fputc ('\t', out);
      emit_x (out, r->attr_length);
      fputc ('\t', out);
      emit_x (out, r->form);
      fputc ('\t', out);
      emit_x (out, r->to_cu);
      fputc ('\t', out);
      emit_x (out, r->to_die);
      fputc ('\n', out);
    }
}

/* Write U records for a string attribute and, for file attributes,
   the comp_dir string of the supplying CU.  */

static void
emit_string_evidence (struct report_ctx *ctx, unsigned int qi,
		      unsigned int depth, unsigned int key,
		      const struct dwarf_evidence_source *src)
{
  const struct dwarf_evidence_attr *attr;
  FILE *out = ctx->out;

  if (!src->present)
    return;

  attr = &src->provider_attr;

  if (attr->encoding == DW_EVIDENCE_ENCODING_INLINE_STRING)
    {
      fprintf (out, "U\t%u\t%u\t%s\t.debug_info\t",
	       qi, depth, key_names[key]);
      emit_x (out, attr->offset);
      fputc ('\t', out);
      emit_x (out, attr->string_length);
      fputc ('\t', out);
      emit_escaped (out, (const char *) attr->string);
      fputc ('\n', out);
    }
  else if (attr->encoding == DW_EVIDENCE_ENCODING_STRP)
    {
      fprintf (out, "U\t%u\t%u\t%s\t.debug_str\t",
	       qi, depth, key_names[key]);
      emit_x (out, attr->raw_value);
      fputc ('\t', out);
      emit_x (out, attr->string_length);
      fputc ('\t', out);
      emit_escaped (out, (const char *) attr->string);
      fputc ('\n', out);
    }
}

/* Write a T record plus the comp_dir U record for a file attribute.  */

static void
emit_file_evidence (struct report_ctx *ctx, unsigned int qi,
		    unsigned int depth, unsigned int key,
		    const struct dwarf_evidence_source *src,
		    const struct dwarf_evidence_file *file)
{
  FILE *out = ctx->out;
  const char *compdir_key;

  if (file == NULL)
    return;

  compdir_key = (key == DW_EVIDENCE_KEY_DECL_FILE)
    ? "decl_comp_dir" : "call_comp_dir";

  fprintf (out, "T\t%u\t%u\t%s\t", qi, depth, key_names[key]);
  emit_x (out, file->cu_offset);
  fputc ('\t', out);
  emit_x (out, file->stmt_list);
  fputc ('\t', out);
  emit_x (out, file->file_index);
  fputc ('\t', out);
  emit_x (out, file->file_entry_offset);
  fputc ('\t', out);
  emit_x (out, file->file_entry_length);
  fputc ('\t', out);
  if (file->directory_index == 0)
    fputs ("-\t-", out);
  else
    {
      emit_x (out, file->directory_entry_offset);
      fputc ('\t', out);
      emit_x (out, file->directory_entry_length);
    }
  fputc ('\t', out);
  emit_x (out, file->compdir_attr_offset);
  fputc ('\t', out);
  emit_x (out, file->compdir_attr_length);
  fputc ('\t', out);
  emit_escaped (out, file->resolved_path);
  fputc ('\n', out);

  /* The comp_dir U record, keyed by the file attribute.  */
  if (file->compdir_form == EVIDENCE_FORM_STRING)
    {
      fprintf (out, "U\t%u\t%u\t%s\t.debug_info\t",
	       qi, depth, compdir_key);
      emit_x (out, file->compdir_attr_offset);
      fputc ('\t', out);
      emit_x (out, file->compdir_string_length);
      fputc ('\t', out);
      emit_escaped (out, (const char *) file->compdir_string);
      fputc ('\n', out);
    }
  else if (file->compdir_form == EVIDENCE_FORM_STRP)
    {
      fprintf (out, "U\t%u\t%u\t%s\t.debug_str\t",
	       qi, depth, compdir_key);
      emit_x (out, file->compdir_raw_value);
      fputc ('\t', out);
      emit_x (out, file->compdir_string_length);
      fputc ('\t', out);
      emit_escaped (out, (const char *) file->compdir_string);
      fputc ('\n', out);
    }

  (void) src;
}

/* Emit every record for one frame.  */

static void
emit_frame (struct report_ctx *ctx, unsigned int qi,
	    const struct dwarf_evidence_frame *frame)
{
  FILE *out = ctx->out;
  unsigned int depth = frame->depth;
  const struct dwarf_evidence_source *name_src
    = &frame->sources[DW_EVIDENCE_KEY_NAME];
  const struct dwarf_evidence_source *decl_file_src
    = &frame->sources[DW_EVIDENCE_KEY_DECL_FILE];
  const struct dwarf_evidence_source *decl_line_src
    = &frame->sources[DW_EVIDENCE_KEY_DECL_LINE];
  const struct dwarf_evidence_source *decl_col_src
    = &frame->sources[DW_EVIDENCE_KEY_DECL_COLUMN];
  const struct dwarf_evidence_source *call_file_src
    = &frame->sources[DW_EVIDENCE_KEY_CALL_FILE];
  const struct dwarf_evidence_source *call_line_src
    = &frame->sources[DW_EVIDENCE_KEY_CALL_LINE];
  const struct dwarf_evidence_source *call_col_src
    = &frame->sources[DW_EVIDENCE_KEY_CALL_COLUMN];
  unsigned int k;

  /* F record.  */
  fprintf (out, "F\t%u\t%u\t", qi, depth);
  emit_x (out, frame->concrete.cu_offset);
  fputc ('\t', out);
  emit_x (out, frame->concrete.die_offset);
  fputc ('\t', out);
  emit_x (out, (uint64_t) frame->low_pc);
  fputc ('\t', out);
  emit_x (out, (uint64_t) frame->high_pc);
  fputc ('\t', out);

  emit_string_or_unknown (out,
			  name_src->present
			  ? evidence_string (&name_src->provider_attr)
			  : NULL);
  fputc ('\t', out);
  emit_string_or_unknown (out,
			  frame->decl_file_evidence != NULL
			  ? frame->decl_file_evidence->resolved_path
			  : NULL);
  fputc ('\t', out);
  if (decl_line_src->present)
    fprintf (out, "%" PRIu64, decl_line_src->provider_attr.raw_value);
  else
    fputc ('-', out);
  fputc ('\t', out);
  if (decl_col_src->present)
    fprintf (out, "%" PRIu64, decl_col_src->provider_attr.raw_value);
  else
    fputc ('-', out);
  fputc ('\t', out);
  emit_string_or_unknown (out,
			  frame->call_file_evidence != NULL
			  ? frame->call_file_evidence->resolved_path
			  : NULL);
  fputc ('\t', out);
  if (call_line_src->present)
    fprintf (out, "%" PRIu64, call_line_src->provider_attr.raw_value);
  else
    fputc ('-', out);
  fputc ('\t', out);
  if (call_col_src->present)
    fprintf (out, "%" PRIu64, call_col_src->provider_attr.raw_value);
  else
    fputc ('-', out);
  fputc ('\n', out);

  /* A and R records for every tracked attribute.  */
  for (k = 0; k < DW_EVIDENCE_KEY_COUNT; ++k)
    {
      const struct dwarf_evidence_source *src = &frame->sources[k];

      emit_attr (ctx, qi, depth, k, src);
      emit_refs (ctx, qi, depth, k, src);
    }

  /* Name string evidence.  */
  emit_string_evidence (ctx, qi, depth, DW_EVIDENCE_KEY_NAME, name_src);

  /* File table evidence and comp_dir strings.  */
  emit_file_evidence (ctx, qi, depth, DW_EVIDENCE_KEY_DECL_FILE,
		      decl_file_src, frame->decl_file_evidence);
  emit_file_evidence (ctx, qi, depth, DW_EVIDENCE_KEY_CALL_FILE,
		      call_file_src, frame->call_file_evidence);
}

static void
emit_sections (FILE *out, const struct sample_object *obj)
{
  static const char *const names[DEBUG_MAX] =
    {
      ".debug_info", ".debug_line", ".debug_abbrev", ".debug_ranges",
      ".debug_str", ".debug_addr", ".debug_str_offsets",
      ".debug_line_str", ".debug_rnglists"
    };
  int i;

  for (i = 0; i < DEBUG_MAX; ++i)
    {
      if (obj->sections.size[i] != 0)
	{
	  fprintf (out, "S\t%s\t", names[i]);
	  emit_x (out, (uint64_t) obj->offsets[i]);
	  fputc ('\t', out);
	  emit_x (out, (uint64_t) obj->sections.size[i]);
	  fputc ('\n', out);
	}
    }
}

int
sample_report (const char *elf_path, const char *pc_path, FILE *output)
{
  struct sample_object obj;
  uintptr_t pcs[256];
  size_t npc = 0;
  size_t i;
  struct report_ctx ctx;

  ctx.out = output;
  ctx.failed = 0;

  if (!sample_open (elf_path, &obj))
    {
      fprintf (stderr, "sample_report: cannot open %s\n", elf_path);
      return 1;
    }
  if (!sample_read_pcs (pc_path, pcs, 256, &npc))
    {
      fprintf (stderr, "sample_report: cannot read %s\n", pc_path);
      return 1;
    }

  emit_sections (output, &obj);

  for (i = 0; i < npc; ++i)
    {
      const struct dwarf_evidence *evidence = NULL;

      if (!backtrace_dwarf_evidence (obj.state, obj.entry, pcs[i],
				     &evidence, report_error, &ctx))
	return 1;

      fprintf (output, "Q\t%zx\t", i);
      emit_x (output, (uint64_t) pcs[i]);
      fprintf (output, "\t%zx\n",
	       evidence == NULL ? 0 : evidence->frames_count);

      if (evidence != NULL)
	{
	  size_t f;

	  for (f = 0; f < evidence->frames_count; ++f)
	    emit_frame (&ctx, i, &evidence->frames[f]);
	}
    }

  fflush (output);
  return ctx.failed ? 1 : 0;
}
