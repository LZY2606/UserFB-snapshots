/* Tests for the offline module state.  */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "backtrace.h"
#include "check.h"
#include "basic_fixture.h"

/* The offline tests never unwind the tool process, so these tests do
   not need the unwind support or the internal headers.  Every check is
   an explicit TASK_CHECK against an independent expectation.  */

/* A captured callback result.  Frame results are recorded in order,
   because an inlined PC produces more than one frame.  */

#define MAX_FRAMES 8

struct frame_result
{
  uintptr_t pc;
  const char *file;
  int line;
  const char *function;
};

struct capture
{
  /* Number of frames recorded.  */
  int frames;
  /* Each recorded frame.  The string pointers are copied into the
     buffers below so that they remain valid after the callback.  */
  struct frame_result frame[MAX_FRAMES];
  char file_buf[MAX_FRAMES][256];
  char function_buf[MAX_FRAMES][256];

  /* The last syminfo result.  */
  int have_symbol;
  uintptr_t sym_pc;
  char sym_name[256];
  uintptr_t sym_value;
  uintptr_t sym_size;
  int sym_name_null;

  /* Number of times a callback was invoked with a NULL file and
     function, which is how an address outside every module is
     reported.  */
  int null_frames;

  /* Number of calls to the error callback.  */
  int errors;

  /* MOREDATA results.  The callbacks store the version, the wrapped
     data pointer, the discriminator, and the declaration line.  */
  int moredata_calls;
  int moredata_version;
  void *moredata_data;
  int moredata_frames;
  int moredata_symbols;
  int moredata_bad_version;
};

static void
capture_error (void *vdata, const char *msg, int errnum)
{
  struct capture *capture = (struct capture *) vdata;

  (void) msg;
  (void) errnum;
  ++capture->errors;
}

static int
capture_frame (void *vdata, uintptr_t pc, const char *filename, int lineno,
	       const char *function)
{
  struct capture *capture = (struct capture *) vdata;
  struct frame_result *frame;

  if (capture->frames >= MAX_FRAMES)
    return 1;

  frame = &capture->frame[capture->frames];
  frame->pc = pc;
  frame->line = lineno;
  if (filename == NULL)
    frame->file = NULL;
  else
    {
      snprintf (capture->file_buf[capture->frames],
		sizeof capture->file_buf[capture->frames], "%s", filename);
      frame->file = capture->file_buf[capture->frames];
    }
  if (function == NULL)
    frame->function = NULL;
  else
    {
      snprintf (capture->function_buf[capture->frames],
		sizeof capture->function_buf[capture->frames], "%s", function);
      frame->function = capture->function_buf[capture->frames];
    }
  if (filename == NULL && function == NULL && lineno == 0)
    ++capture->null_frames;

  ++capture->frames;
  return 0;
}

static void
capture_symbol (void *vdata, uintptr_t pc, const char *symname,
		uintptr_t symval, uintptr_t symsize)
{
  struct capture *capture = (struct capture *) vdata;

  capture->have_symbol = 1;
  capture->sym_pc = pc;
  capture->sym_value = symval;
  capture->sym_size = symsize;
  capture->sym_name_null = symname == NULL;
  if (symname != NULL)
    snprintf (capture->sym_name, sizeof capture->sym_name, "%s", symname);
}

static void
capture_init (struct capture *capture)
{
  memset (capture, 0, sizeof *capture);
}

/* MOREDATA callbacks.  They verify the documented layout: the callback
   DATA is a struct backtrace_moredata whose backtrace_data field holds
   the value originally passed to backtrace_pcinfo or
   backtrace_syminfo.  */

static int
capture_frame_moredata (void *vdata, uintptr_t pc, const char *filename,
			int lineno, const char *function)
{
  struct backtrace_moredata *md = (struct backtrace_moredata *) vdata;
  struct capture *capture;

  if (md == NULL || md->backtrace_version != 4)
    {
      if (md != NULL)
	{
	  capture = (struct capture *) md->backtrace_data;
	  ++capture->moredata_bad_version;
	}
      return 1;
    }

  capture = (struct capture *) md->backtrace_data;
  capture->moredata_version = md->backtrace_version;
  capture->moredata_data = md->backtrace_data;
  ++capture->moredata_calls;
  ++capture->moredata_frames;
  (void) filename;
  (void) lineno;
  (void) function;
  (void) pc;
  return 0;
}

static void
capture_symbol_moredata (void *vdata, uintptr_t pc, const char *symname,
			 uintptr_t symval, uintptr_t symsize)
{
  struct backtrace_moredata *md = (struct backtrace_moredata *) vdata;
  struct capture *capture;

  if (md == NULL || md->backtrace_version != 4)
    {
      if (md != NULL)
	{
	  capture = (struct capture *) md->backtrace_data;
	  ++capture->moredata_bad_version;
	}
      return;
    }

  capture = (struct capture *) md->backtrace_data;
  capture->moredata_version = md->backtrace_version;
  capture->moredata_data = md->backtrace_data;
  ++capture->moredata_calls;
  ++capture->moredata_symbols;
  (void) symname;
  (void) symval;
  (void) symsize;
  (void) pc;
}

/* Build a simple module with debug info, a function, and a symbol.  */

static void
simple_module (struct task_elf_module *m, const char *path,
	       const char *function, const char *symbol,
	       const char *source, uint64_t link_low, uint64_t size,
	       int line)
{
  memset (m, 0, sizeof *m);
  m->path = path;
  m->link_low = link_low;
  m->size = size;
  m->function_name = function;
  m->symbol_name = symbol;
  m->source_name = source;
  m->line = line;
  m->decl_line = 1;
  m->with_symtab = 1;
  m->with_debug = 1;
}

/* The temporary directory holding the test ELF files.  */

static const char *
test_dir (void)
{
  const char *tmp;

  tmp = getenv ("TMPDIR");
  if (tmp == NULL || tmp[0] == '\0')
    tmp = ".";
  return tmp;
}

static void
make_path (char *buf, size_t size, const char *name)
{
  snprintf (buf, size, "%s/offline_task_%s", test_dir (), name);
}

/* Run pcinfo and capture up to MAX_FRAMES frames.  */

static int
run_pcinfo (struct backtrace_state *state, uintptr_t pc,
	    struct capture *capture)
{
  capture_init (capture);
  return backtrace_pcinfo (state, pc, capture_frame, capture_error, capture);
}

/* Run syminfo and capture the result.  */

static int
run_syminfo (struct backtrace_state *state, uintptr_t pc,
	     struct capture *capture)
{
  capture_init (capture);
  return backtrace_syminfo (state, pc, capture_symbol, capture_error, capture);
}

/* Group modules: registration behavior.  */

static int
test_add_and_query (void)
{
  struct task_elf_module m;
  struct backtrace_state *state;
  struct capture capture;
  char path[512];
  uintptr_t base = 0x400000;
  uintptr_t runtime = base + 0x2;

  make_path (path, sizeof path, "basic.elf");
  simple_module (&m, path, "offline_func", "offline_sym", "offline.c",
		 0x1000, 16, 42);
  TASK_CHECK (task_write_elf_module (&m));

  state = backtrace_create_offline_state (0, capture_error, &capture);
  TASK_CHECK (state != NULL);

  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, path, base, base + 0x1000,
					    0x1000, capture_error,
					    &capture) == 1);
  TASK_CHECK (capture.errors == 0);

  TASK_CHECK (run_pcinfo (state, runtime, &capture) == 0);
  TASK_CHECK (capture.frames == 1);
  TASK_CHECK (capture.frame[0].pc == runtime);
  TASK_CHECK (capture.frame[0].file != NULL);
  TASK_CHECK (strcmp (capture.frame[0].file, "/fixture/offline.c") == 0);
  TASK_CHECK (capture.frame[0].line == 42);
  TASK_CHECK (capture.frame[0].function != NULL);
  TASK_CHECK (strcmp (capture.frame[0].function, "offline_func") == 0);

  TASK_CHECK (run_syminfo (state, runtime, &capture) == 1);
  TASK_CHECK (capture.have_symbol);
  TASK_CHECK (!capture.sym_name_null);
  TASK_CHECK (strcmp (capture.sym_name, "offline_sym") == 0);
  TASK_CHECK (capture.sym_pc == runtime);
  TASK_CHECK (capture.sym_value == base);
  TASK_CHECK (capture.sym_size == 16);
  TASK_CHECK (capture.errors == 0);

  unlink (path);
  return 0;
}

static int
test_dynamic_module (void)
{
  struct task_elf_module m;
  struct backtrace_state *state;
  struct capture capture;
  char path[512];
  uintptr_t base = 0xb00000;

  /* An ET_DYN module is read from its own file, with the runtime base
     supplied explicitly rather than taken from the process loader.  */
  make_path (path, sizeof path, "pie.elf");
  simple_module (&m, path, "pie_func", "pie_sym", "pie.c", 0x0, 16, 64);
  m.dynamic = 1;
  TASK_CHECK (task_write_elf_module (&m));

  state = backtrace_create_offline_state (0, capture_error, &capture);
  TASK_CHECK (state != NULL);

  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, path, base, base + 0x1000,
					    0x0, capture_error,
					    &capture) == 1);
  TASK_CHECK (capture.errors == 0);

  TASK_CHECK (run_pcinfo (state, base + 0x2, &capture) == 0);
  TASK_CHECK (capture.frames == 1);
  TASK_CHECK (strcmp (capture.frame[0].function, "pie_func") == 0);
  TASK_CHECK (strcmp (capture.frame[0].file, "/fixture/pie.c") == 0);
  TASK_CHECK (capture.frame[0].line == 64);
  TASK_CHECK (capture.frame[0].pc == base + 0x2);

  TASK_CHECK (run_syminfo (state, base + 0x2, &capture) == 1);
  TASK_CHECK (strcmp (capture.sym_name, "pie_sym") == 0);
  TASK_CHECK (capture.sym_value == base);
  TASK_CHECK (capture.sym_size == 16);

  unlink (path);
  return 0;
}

static int
test_link_offset (void)
{
  struct task_elf_module m;
  struct backtrace_state *state;
  struct capture capture;
  char path[512];
  /* Link and runtime addresses differ by a non-zero offset.  */
  uintptr_t base = 0x900000;
  uintptr_t link_low = 0x200000;
  uintptr_t runtime = base + 0x10;

  make_path (path, sizeof path, "offset.elf");
  simple_module (&m, path, "offset_func", "offset_sym", "offset.c",
		 link_low, 32, 11);
  TASK_CHECK (task_write_elf_module (&m));

  state = backtrace_create_offline_state (0, capture_error, &capture);
  TASK_CHECK (state != NULL);

  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, path, base, base + 0x200,
					    link_low, capture_error,
					    &capture) == 1);
  TASK_CHECK (capture.errors == 0);

  TASK_CHECK (run_pcinfo (state, runtime, &capture) == 0);
  TASK_CHECK (capture.frames == 1);
  TASK_CHECK (capture.frame[0].pc == runtime);
  TASK_CHECK (capture.frame[0].line == 11);
  TASK_CHECK (strcmp (capture.frame[0].function, "offset_func") == 0);

  /* The symbol's link time value is link_low, which is runtime base.  */
  TASK_CHECK (run_syminfo (state, runtime, &capture) == 1);
  TASK_CHECK (capture.sym_value == base);
  TASK_CHECK (capture.sym_size == 32);

  unlink (path);
  return 0;
}

static int
test_empty_range (void)
{
  struct task_elf_module m;
  struct backtrace_state *state;
  struct capture capture;
  char path[512];

  make_path (path, sizeof path, "empty.elf");
  simple_module (&m, path, "empty_func", "empty_sym", "empty.c",
		 0x1000, 16, 1);
  TASK_CHECK (task_write_elf_module (&m));

  state = backtrace_create_offline_state (0, capture_error, &capture);
  TASK_CHECK (state != NULL);

  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, path, 0x1000, 0x1000,
					    0x1000, capture_error,
					    &capture) == 0);
  TASK_CHECK (capture.errors == 1);

  /* A reversed range is also empty.  */
  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, path, 0x2000, 0x1000,
					    0x1000, capture_error,
					    &capture) == 0);
  TASK_CHECK (capture.errors == 1);

  /* Nothing was registered: a query reports no information.  */
  TASK_CHECK (run_pcinfo (state, 0x1000, &capture) == 0);
  TASK_CHECK (capture.frames == 1);
  TASK_CHECK (capture.frame[0].pc == 0x1000);
  TASK_CHECK (capture.frame[0].file == NULL);
  TASK_CHECK (capture.frame[0].line == 0);
  TASK_CHECK (capture.frame[0].function == NULL);
  TASK_CHECK (capture.errors == 0);

  unlink (path);
  return 0;
}

static int
test_overlap_rejected (void)
{
  struct task_elf_module m;
  struct backtrace_state *state;
  struct capture capture;
  char path1[512];
  char path2[512];

  make_path (path1, sizeof path1, "overlap1.elf");
  make_path (path2, sizeof path2, "overlap2.elf");
  simple_module (&m, path1, "func1", "sym1", "one.c", 0x1000, 16, 1);
  TASK_CHECK (task_write_elf_module (&m));
  simple_module (&m, path2, "func2", "sym2", "two.c", 0x1000, 16, 2);
  TASK_CHECK (task_write_elf_module (&m));

  state = backtrace_create_offline_state (0, capture_error, &capture);
  TASK_CHECK (state != NULL);

  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, path1, 0x1000, 0x2000,
					    0x1000, capture_error,
					    &capture) == 1);
  TASK_CHECK (capture.errors == 0);

  /* A range that shares a single byte is rejected, and the failure
     does not disturb the module already registered.  */
  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, path2, 0x1fff, 0x3000,
					    0x1000, capture_error,
					    &capture) == 0);
  TASK_CHECK (capture.errors == 1);

  TASK_CHECK (run_pcinfo (state, 0x1002, &capture) == 0);
  TASK_CHECK (capture.frames == 1);
  TASK_CHECK (strcmp (capture.frame[0].function, "func1") == 0);

  /* Adjacent ranges are accepted.  */
  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, path2, 0x2000, 0x3000,
					    0x1000, capture_error,
					    &capture) == 1);
  TASK_CHECK (capture.errors == 0);

  TASK_CHECK (run_pcinfo (state, 0x2002, &capture) == 0);
  TASK_CHECK (capture.frames == 1);
  TASK_CHECK (strcmp (capture.frame[0].function, "func2") == 0);

  unlink (path1);
  unlink (path2);
  return 0;
}

static int
test_address_overflow (void)
{
  struct task_elf_module m;
  struct backtrace_state *state;
  struct capture capture;
  char path[512];

  make_path (path, sizeof path, "overflow.elf");
  simple_module (&m, path, "of_func", "of_sym", "of.c", 0x1000, 16, 1);
  TASK_CHECK (task_write_elf_module (&m));

  state = backtrace_create_offline_state (0, capture_error, &capture);
  TASK_CHECK (state != NULL);

  /* link_low near the top of the address space cannot be translated
     for a range of 0x2000 bytes.  */
  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, path, 0x1000, 0x3000,
					    (uintptr_t) -1 - 0x100,
					    capture_error, &capture) == 0);
  TASK_CHECK (capture.errors == 1);

  /* A runtime range that wraps around the address space is empty when
     compared with unsigned arithmetic, and is rejected.  */
  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, path, 0xfffffffffffff000ULL,
					    0x1000, 0x1000, capture_error,
					    &capture) == 0);
  TASK_CHECK (capture.errors == 1);

  unlink (path);
  return 0;
}

/* Group callbacks: module ownership and callback PC values.  */

static int
test_same_link_address (void)
{
  struct task_elf_module m;
  struct backtrace_state *state;
  struct capture capture;
  char path1[512];
  char path2[512];
  uintptr_t base1 = 0x100000;
  uintptr_t base2 = 0x200000;

  /* Two modules with exactly the same link time addresses.  Only the
     module that contains the runtime address may answer.  */
  make_path (path1, sizeof path1, "same1.elf");
  make_path (path2, sizeof path2, "same2.elf");
  simple_module (&m, path1, "first_func", "first_sym", "first.c",
		 0x1000, 16, 10);
  TASK_CHECK (task_write_elf_module (&m));
  simple_module (&m, path2, "second_func", "second_sym", "second.c",
		 0x1000, 16, 20);
  TASK_CHECK (task_write_elf_module (&m));

  state = backtrace_create_offline_state (0, capture_error, &capture);
  TASK_CHECK (state != NULL);

  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, path1, base1, base1 + 0x1000,
					    0x1000, capture_error,
					    &capture) == 1);
  TASK_CHECK (capture.errors == 0);
  TASK_CHECK (backtrace_offline_add_module (state, path2, base2, base2 + 0x1000,
					    0x1000, capture_error,
					    &capture) == 1);
  TASK_CHECK (capture.errors == 0);

  TASK_CHECK (run_pcinfo (state, base1 + 0x2, &capture) == 0);
  TASK_CHECK (capture.frames == 1);
  TASK_CHECK (strcmp (capture.frame[0].function, "first_func") == 0);
  TASK_CHECK (strcmp (capture.frame[0].file, "/fixture/first.c") == 0);
  TASK_CHECK (capture.frame[0].line == 10);
  TASK_CHECK (capture.frame[0].pc == base1 + 0x2);

  TASK_CHECK (run_pcinfo (state, base2 + 0x2, &capture) == 0);
  TASK_CHECK (capture.frames == 1);
  TASK_CHECK (strcmp (capture.frame[0].function, "second_func") == 0);
  TASK_CHECK (strcmp (capture.frame[0].file, "/fixture/second.c") == 0);
  TASK_CHECK (capture.frame[0].line == 20);
  TASK_CHECK (capture.frame[0].pc == base2 + 0x2);

  /* The same link address maps to different runtime symbol values.  */
  TASK_CHECK (run_syminfo (state, base1 + 0x2, &capture) == 1);
  TASK_CHECK (strcmp (capture.sym_name, "first_sym") == 0);
  TASK_CHECK (capture.sym_value == base1);
  TASK_CHECK (capture.sym_size == 16);

  TASK_CHECK (run_syminfo (state, base2 + 0x2, &capture) == 1);
  TASK_CHECK (strcmp (capture.sym_name, "second_sym") == 0);
  TASK_CHECK (capture.sym_value == base2);
  TASK_CHECK (capture.sym_size == 16);

  unlink (path1);
  unlink (path2);
  return 0;
}

static int
test_no_borrowing (void)
{
  struct task_elf_module m;
  struct backtrace_state *state;
  struct capture capture;
  char path1[512];
  char path2[512];
  uintptr_t base1 = 0x100000;
  uintptr_t base2 = 0x200000;

  /* The first module has symbols and no debug info.  The second has
     debug info.  Querying the first must not use the second's DWARF
     entry, even though both use the same link address.  */
  make_path (path1, sizeof path1, "nodwarf.elf");
  make_path (path2, sizeof path2, "dwarf.elf");
  simple_module (&m, path1, "nodwarf_func", "nodwarf_sym", "nodwarf.c",
		 0x1000, 16, 5);
  m.with_debug = 0;
  TASK_CHECK (task_write_elf_module (&m));
  simple_module (&m, path2, "dwarf_func", "dwarf_sym", "dwarf.c",
		 0x1000, 16, 6);
  TASK_CHECK (task_write_elf_module (&m));

  state = backtrace_create_offline_state (0, capture_error, &capture);
  TASK_CHECK (state != NULL);

  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, path1, base1, base1 + 0x1000,
					    0x1000, capture_error,
					    &capture) == 1);
  TASK_CHECK (backtrace_offline_add_module (state, path2, base2, base2 + 0x1000,
					    0x1000, capture_error,
					    &capture) == 1);
  TASK_CHECK (capture.errors == 0);

  /* The first module has no DWARF, so no file or line is reported.
     It must not report the second module's file, line, or function
     name.  The function name may be the symbol name from the first
     module.  */
  TASK_CHECK (run_pcinfo (state, base1 + 0x2, &capture) == 0);
  TASK_CHECK (capture.frames == 1);
  TASK_CHECK (capture.frame[0].pc == base1 + 0x2);
  TASK_CHECK (capture.frame[0].file == NULL);
  TASK_CHECK (capture.frame[0].line == 0);
  if (capture.frame[0].function != NULL)
    TASK_CHECK (strcmp (capture.frame[0].function, "nodwarf_sym") == 0);
  TASK_CHECK (capture.frame[0].function == NULL
	      || strcmp (capture.frame[0].function, "dwarf_func") != 0);

  /* The second module still answers from its own DWARF.  */
  TASK_CHECK (run_pcinfo (state, base2 + 0x2, &capture) == 0);
  TASK_CHECK (capture.frames == 1);
  TASK_CHECK (strcmp (capture.frame[0].function, "dwarf_func") == 0);
  TASK_CHECK (strcmp (capture.frame[0].file, "/fixture/dwarf.c") == 0);
  TASK_CHECK (capture.frame[0].line == 6);

  unlink (path1);
  unlink (path2);
  return 0;
}

static int
test_inline_frames (void)
{
  struct task_elf_module m;
  struct backtrace_state *state;
  struct capture capture;
  char path[512];
  uintptr_t base = 0x300000;

  make_path (path, sizeof path, "inline.elf");
  simple_module (&m, path, "outer_func", "outer_sym", "inline.c",
		 0x1000, 64, 100);
  m.inline_name = "inner_func";
  m.inline_low = 0x10;
  m.inline_high = 0x20;
  m.call_file = 1;
  m.call_line = 55;
  TASK_CHECK (task_write_elf_module (&m));

  state = backtrace_create_offline_state (0, capture_error, &capture);
  TASK_CHECK (state != NULL);

  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, path, base, base + 0x1000,
					    0x1000, capture_error,
					    &capture) == 1);
  TASK_CHECK (capture.errors == 0);

  /* A PC inside the inline range reports the inlined function and then
     the containing function.  Both callbacks see the same runtime
     address.  */
  TASK_CHECK (run_pcinfo (state, base + 0x12, &capture) == 0);
  TASK_CHECK (capture.frames == 2);
  TASK_CHECK (capture.frame[0].pc == base + 0x12);
  TASK_CHECK (capture.frame[0].function != NULL);
  TASK_CHECK (strcmp (capture.frame[0].function, "inner_func") == 0);
  TASK_CHECK (capture.frame[0].line == 100);
  TASK_CHECK (capture.frame[1].pc == base + 0x12);
  TASK_CHECK (capture.frame[1].function != NULL);
  TASK_CHECK (strcmp (capture.frame[1].function, "outer_func") == 0);
  TASK_CHECK (capture.frame[1].line == 55);

  /* A PC outside the inline range reports only the outer function.  */
  TASK_CHECK (run_pcinfo (state, base + 0x2, &capture) == 0);
  TASK_CHECK (capture.frames == 1);
  TASK_CHECK (strcmp (capture.frame[0].function, "outer_func") == 0);

  unlink (path);
  return 0;
}

static int
test_holes (void)
{
  struct task_elf_module m;
  struct backtrace_state *state;
  struct capture capture;
  char path[512];
  uintptr_t base = 0x500000;

  make_path (path, sizeof path, "hole.elf");
  simple_module (&m, path, "hole_func", "hole_sym", "hole.c",
		 0x1000, 16, 3);
  TASK_CHECK (task_write_elf_module (&m));

  state = backtrace_create_offline_state (0, capture_error, &capture);
  TASK_CHECK (state != NULL);

  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, path, base, base + 0x100,
					    0x1000, capture_error,
					    &capture) == 1);
  TASK_CHECK (capture.errors == 0);

  /* Addresses in the module range but outside the module's text get
     the standard "no information" callback.  */
  TASK_CHECK (run_pcinfo (state, base + 0x180, &capture) == 0);
  TASK_CHECK (capture.frames == 1);
  TASK_CHECK (capture.frame[0].pc == base + 0x180);
  TASK_CHECK (capture.frame[0].file == NULL);
  TASK_CHECK (capture.frame[0].line == 0);
  TASK_CHECK (capture.frame[0].function == NULL);

  TASK_CHECK (run_syminfo (state, base + 0x180, &capture) == 1);
  TASK_CHECK (capture.sym_name_null);
  TASK_CHECK (capture.sym_pc == base + 0x180);
  TASK_CHECK (capture.sym_value == 0);
  TASK_CHECK (capture.sym_size == 0);

  /* Addresses below and above the module range do the same.  */
  TASK_CHECK (run_pcinfo (state, base - 1, &capture) == 0);
  TASK_CHECK (capture.frames == 1);
  TASK_CHECK (capture.null_frames == 1);

  TASK_CHECK (run_pcinfo (state, base + 0x100, &capture) == 0);
  TASK_CHECK (capture.frames == 1);
  TASK_CHECK (capture.null_frames == 1);

  TASK_CHECK (run_syminfo (state, base - 1, &capture) == 1);
  TASK_CHECK (capture.sym_name_null);
  TASK_CHECK (capture.sym_value == 0);
  TASK_CHECK (capture.sym_size == 0);

  unlink (path);
  return 0;
}

static int
test_boundaries (void)
{
  struct task_elf_module m;
  struct backtrace_state *state;
  struct capture capture;
  char path[512];
  uintptr_t base = 0x600000;

  make_path (path, sizeof path, "boundary.elf");
  simple_module (&m, path, "boundary_func", "boundary_sym", "boundary.c",
		 0x1000, 16, 9);
  TASK_CHECK (task_write_elf_module (&m));

  state = backtrace_create_offline_state (0, capture_error, &capture);
  TASK_CHECK (state != NULL);

  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, path, base, base + 0x100,
					    0x1000, capture_error,
					    &capture) == 1);

  /* The low end of the half-open range is included.  */
  TASK_CHECK (run_syminfo (state, base, &capture) == 1);
  TASK_CHECK (!capture.sym_name_null);
  TASK_CHECK (strcmp (capture.sym_name, "boundary_sym") == 0);
  TASK_CHECK (capture.sym_value == base);

  /* The high end is excluded.  */
  TASK_CHECK (run_syminfo (state, base + 0x100, &capture) == 1);
  TASK_CHECK (capture.sym_name_null);

  unlink (path);
  return 0;
}

static int
test_reject_bad_file (void)
{
  struct task_elf_module m;
  struct backtrace_state *state;
  struct capture capture;
  char path1[512];
  char path2[512];
  char path3[512];
  uintptr_t base = 0x700000;

  /* An ET_REL file and a 32-bit file are both rejected, and a missing
     file is rejected.  A good module registered first keeps working.  */
  make_path (path1, sizeof path1, "good.elf");
  make_path (path2, sizeof path2, "rel.elf");
  make_path (path3, sizeof path3, "elf32.elf");
  simple_module (&m, path1, "good_func", "good_sym", "good.c",
		 0x1000, 16, 1);
  TASK_CHECK (task_write_elf_module (&m));
  simple_module (&m, path2, "rel_func", "rel_sym", "rel.c", 0x1000, 16, 1);
  m.force_type = 1; /* ET_REL */
  m.with_debug = 0;
  TASK_CHECK (task_write_elf_module (&m));
  simple_module (&m, path3, "elf32_func", "elf32_sym", "e32.c",
		 0x1000, 16, 1);
  m.force_type = 0;
  m.force_class = 1; /* ELFCLASS32 */
  TASK_CHECK (task_write_elf_module (&m));

  state = backtrace_create_offline_state (0, capture_error, &capture);
  TASK_CHECK (state != NULL);

  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, path1, base, base + 0x1000,
					    0x1000, capture_error,
					    &capture) == 1);
  TASK_CHECK (capture.errors == 0);

  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, path2, base + 0x1000,
					    base + 0x2000, 0x1000,
					    capture_error, &capture) == 0);
  TASK_CHECK (capture.errors == 1);

  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, path3, base + 0x1000,
					    base + 0x2000, 0x1000,
					    capture_error, &capture) == 0);
  TASK_CHECK (capture.errors == 1);

  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, "/nonexistent/offline.elf",
					    base + 0x1000, base + 0x2000,
					    0x1000, capture_error,
					    &capture) == 0);
  TASK_CHECK (capture.errors == 1);

  TASK_CHECK (run_pcinfo (state, base + 0x2, &capture) == 0);
  TASK_CHECK (capture.frames == 1);
  TASK_CHECK (strcmp (capture.frame[0].function, "good_func") == 0);

  unlink (path1);
  unlink (path2);
  unlink (path3);
  return 0;
}

/* Group lifetime: deferred source lookups after the file changes.  */

static int
test_delete_after_add (void)
{
  struct task_elf_module m;
  struct backtrace_state *state;
  struct capture capture;
  char path[512];
  uintptr_t base = 0x800000;

  make_path (path, sizeof path, "delete.elf");
  simple_module (&m, path, "delete_func", "delete_sym", "delete.c",
		 0x1000, 32, 77);
  TASK_CHECK (task_write_elf_module (&m));

  state = backtrace_create_offline_state (0, capture_error, &capture);
  TASK_CHECK (state != NULL);

  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, path, base, base + 0x1000,
					    0x1000, capture_error,
					    &capture) == 1);
  TASK_CHECK (capture.errors == 0);

  /* Remove the file before the first source lookup.  The line table is
     read lazily, so the lookup must still succeed.  */
  TASK_CHECK (unlink (path) == 0);

  TASK_CHECK (run_pcinfo (state, base + 0x2, &capture) == 0);
  TASK_CHECK (capture.errors == 0);
  TASK_CHECK (capture.frames == 1);
  TASK_CHECK (capture.frame[0].pc == base + 0x2);
  TASK_CHECK (capture.frame[0].file != NULL);
  TASK_CHECK (strcmp (capture.frame[0].file, "/fixture/delete.c") == 0);
  TASK_CHECK (capture.frame[0].line == 77);
  TASK_CHECK (strcmp (capture.frame[0].function, "delete_func") == 0);

  /* A second lookup also works, and the symbols were read eagerly.  */
  TASK_CHECK (run_syminfo (state, base + 0x2, &capture) == 1);
  TASK_CHECK (capture.errors == 0);
  TASK_CHECK (strcmp (capture.sym_name, "delete_sym") == 0);
  TASK_CHECK (capture.sym_value == base);
  TASK_CHECK (capture.sym_size == 32);

  return 0;
}

static int
test_rename_after_add (void)
{
  struct task_elf_module m;
  struct backtrace_state *state;
  struct capture capture;
  char path[512];
  char other[512];
  uintptr_t base = 0x880000;

  make_path (path, sizeof path, "rename.elf");
  make_path (other, sizeof other, "rename_moved.elf");
  simple_module (&m, path, "rename_func", "rename_sym", "rename.c",
		 0x1000, 32, 88);
  TASK_CHECK (task_write_elf_module (&m));

  state = backtrace_create_offline_state (0, capture_error, &capture);
  TASK_CHECK (state != NULL);

  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, path, base, base + 0x1000,
					    0x1000, capture_error,
					    &capture) == 1);

  /* Move the file out of the way, then remove it.  The module still
     works.  */
  TASK_CHECK (rename (path, other) == 0);
  TASK_CHECK (unlink (other) == 0);

  TASK_CHECK (run_pcinfo (state, base + 0x2, &capture) == 0);
  TASK_CHECK (capture.errors == 0);
  TASK_CHECK (capture.frames == 1);
  TASK_CHECK (strcmp (capture.frame[0].file, "/fixture/rename.c") == 0);
  TASK_CHECK (capture.frame[0].line == 88);

  TASK_CHECK (run_syminfo (state, base + 0x2, &capture) == 1);
  TASK_CHECK (strcmp (capture.sym_name, "rename_sym") == 0);

  return 0;
}

static int
test_many_modules (void)
{
  struct task_elf_module m;
  struct backtrace_state *state;
  struct capture capture;
  char paths[4][512];
  uintptr_t bases[4];
  static const char *names[4] = {"mod_a", "mod_b", "mod_c", "mod_d"};
  int i;

  /* Register modules out of order to exercise the sorted lookup, then
     query each one.  */
  for (i = 0; i < 4; i++)
    {
      char name[32];

      snprintf (name, sizeof name, "many%d.elf", i);
      make_path (paths[i], sizeof paths[i], name);
      bases[i] = 0x900000 + (uintptr_t) i * 0x10000;
      simple_module (&m, paths[i], names[i], names[i], "many.c", 0x1000,
		     16, 30 + i);
      TASK_CHECK (task_write_elf_module (&m));
    }

  state = backtrace_create_offline_state (0, capture_error, &capture);
  TASK_CHECK (state != NULL);

  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, paths[2], bases[2],
					    bases[2] + 0x1000, 0x1000,
					    capture_error, &capture) == 1);
  TASK_CHECK (backtrace_offline_add_module (state, paths[0], bases[0],
					    bases[0] + 0x1000, 0x1000,
					    capture_error, &capture) == 1);
  TASK_CHECK (backtrace_offline_add_module (state, paths[3], bases[3],
					    bases[3] + 0x1000, 0x1000,
					    capture_error, &capture) == 1);
  TASK_CHECK (backtrace_offline_add_module (state, paths[1], bases[1],
					    bases[1] + 0x1000, 0x1000,
					    capture_error, &capture) == 1);
  TASK_CHECK (capture.errors == 0);

  for (i = 0; i < 4; i++)
    {
      TASK_CHECK (run_pcinfo (state, bases[i] + 0x2, &capture) == 0);
      TASK_CHECK (capture.frames == 1);
      TASK_CHECK (strcmp (capture.frame[0].function, names[i]) == 0);
      TASK_CHECK (capture.frame[0].line == 30 + i);
    }

  /* The gaps between the modules are holes.  */
  TASK_CHECK (run_pcinfo (state, 0x980000, &capture) == 0);
  TASK_CHECK (capture.frames == 1);
  TASK_CHECK (capture.null_frames == 1);

  for (i = 0; i < 4; i++)
    unlink (paths[i]);
  return 0;
}

static int
test_symbol_out_of_range (void)
{
  struct task_elf_module m;
  struct backtrace_state *state;
  struct capture capture;
  char path[512];
  uintptr_t base = 0xc00000;

  /* The only symbol starts below link_low, so its link time start
     cannot be represented as a runtime address.  A symbol query that
     finds it must report an error rather than a bogus value.  */
  make_path (path, sizeof path, "symrange.elf");
  simple_module (&m, path, "range_func", "range_sym", "range.c",
		 0x1000, 16, 1);
  m.no_function_symbol = 1;
  m.extra_symbol_name = "below_sym";
  m.extra_symbol_value = 0x800;
  m.extra_symbol_size = 0x1000;
  TASK_CHECK (task_write_elf_module (&m));

  state = backtrace_create_offline_state (0, capture_error, &capture);
  TASK_CHECK (state != NULL);

  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, path, base, base + 0x1000,
					    0x1000, capture_error,
					    &capture) == 1);
  TASK_CHECK (capture.errors == 0);

  TASK_CHECK (run_syminfo (state, base + 0x2, &capture) == 1);
  TASK_CHECK (capture.errors == 1);
  TASK_CHECK (!capture.have_symbol);

  unlink (path);
  return 0;
}

static int
test_moredata_layout (void)
{
  struct task_elf_module m;
  struct backtrace_state *state;
  struct capture capture;
  char path[512];
  uintptr_t base = 0xa00000;

  make_path (path, sizeof path, "moredata.elf");
  simple_module (&m, path, "more_func", "more_sym", "more.c",
		 0x1000, 16, 123);
  TASK_CHECK (task_write_elf_module (&m));

  /* Flags bit 2 selects MOREDATA.  */
  state = backtrace_create_offline_state (2, capture_error, &capture);
  TASK_CHECK (state != NULL);

  capture_init (&capture);
  TASK_CHECK (backtrace_offline_add_module (state, path, base, base + 0x1000,
					    0x1000, capture_error,
					    &capture) == 1);
  TASK_CHECK (capture.errors == 0);

  TASK_CHECK (backtrace_pcinfo (state, base + 0x2, capture_frame_moredata,
				capture_error, &capture) == 0);
  TASK_CHECK (capture.moredata_calls == 1);
  TASK_CHECK (capture.moredata_frames == 1);
  TASK_CHECK (capture.moredata_version == 4);
  TASK_CHECK (capture.moredata_data == (void *) &capture);
  TASK_CHECK (capture.moredata_bad_version == 0);

  TASK_CHECK (backtrace_syminfo (state, base + 0x2, capture_symbol_moredata,
				 capture_error, &capture) == 1);
  TASK_CHECK (capture.moredata_symbols == 1);
  TASK_CHECK (capture.moredata_version == 4);
  TASK_CHECK (capture.moredata_data == (void *) &capture);
  TASK_CHECK (capture.moredata_bad_version == 0);

  /* A MOREDATA query for an address in no module also uses the layout.  */
  TASK_CHECK (backtrace_pcinfo (state, base - 1, capture_frame_moredata,
				capture_error, &capture) == 0);
  TASK_CHECK (capture.moredata_frames == 2);
  TASK_CHECK (capture.moredata_bad_version == 0);

  unlink (path);
  return 0;
}

const struct task_case task_cases[] =
{
  { "modules", "add_and_query", test_add_and_query },
  { "modules", "dynamic_module", test_dynamic_module },
  { "modules", "link_offset", test_link_offset },
  { "modules", "empty_range", test_empty_range },
  { "modules", "overlap_rejected", test_overlap_rejected },
  { "modules", "address_overflow", test_address_overflow },
  { "callbacks", "same_link_address", test_same_link_address },
  { "callbacks", "no_borrowing", test_no_borrowing },
  { "callbacks", "inline_frames", test_inline_frames },
  { "callbacks", "holes", test_holes },
  { "callbacks", "boundaries", test_boundaries },
  { "callbacks", "reject_bad_file", test_reject_bad_file },
  { "callbacks", "symbol_out_of_range", test_symbol_out_of_range },
  { "callbacks", "moredata_layout", test_moredata_layout },
  { "lifetime", "delete_after_add", test_delete_after_add },
  { "lifetime", "rename_after_add", test_rename_after_add },
  { "lifetime", "many_modules", test_many_modules },
};

const size_t task_case_count = sizeof task_cases / sizeof task_cases[0];
