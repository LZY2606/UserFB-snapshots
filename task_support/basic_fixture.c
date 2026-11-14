#include "basic_fixture.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* This file writes small synthetic little-endian ELF64 files by hand.
   It does not use the host assembler, linker, or any target program.
   The files are inert: nothing here loads, executes, or relocates
   them.  */

struct buf
{
  unsigned char b[65536];
  size_t n;
};

static void
u8 (struct buf *b, unsigned x)
{
  assert (b->n < sizeof b->b);
  b->b[b->n++] = (unsigned char) x;
}

static void
num (struct buf *b, uint64_t x, unsigned n)
{
  unsigned i;

  for (i = 0; i < n; i++)
    {
      u8 (b, (unsigned) x);
      x >>= 8;
    }
}

static void
str (struct buf *b, const char *s)
{
  do
    u8 (b, (unsigned char) *s);
  while (*s++);
}

static void
patch (unsigned char *b, size_t o, uint64_t x, unsigned n)
{
  unsigned i;

  for (i = 0; i < n; i++)
    {
      b[o + i] = (unsigned char) x;
      x >>= 8;
    }
}

static void
sleb (struct buf *b, int64_t x)
{
  int more;

  do
    {
      unsigned byte = (unsigned) (x & 0x7f);
      int sign;

      x >>= 7;
      sign = (byte & 0x40) != 0;
      more = !((x == 0 && !sign) || (x == -1 && sign));
      if (more)
	byte |= 0x80;
      u8 (b, byte);
    }
  while (more);
}

struct sec
{
  const char *name;
  uint32_t type;
  uint64_t flags, addr;
  struct buf data;
  uint32_t link, info;
  uint64_t entsize;
};

#define NSEC 7

/* Write the module described by M.  */

int
task_write_elf_module (const struct task_elf_module *m)
{
  struct sec s[NSEC + 1];
  struct buf file;
  uint32_t names[NSEC + 1];
  size_t offsets[NSEC + 1];
  size_t sho, i, o, header_end;
  size_t function_name_offset;
  size_t extra_name_offset;
  FILE *fp;
  uint64_t line = (uint64_t) m->line;

  memset (s, 0, sizeof s);
  memset (names, 0, sizeof names);
  memset (offsets, 0, sizeof offsets);
  memset (&file, 0, sizeof file);

  /* .text at the module's link address.  */
  s[1].name = ".text";
  s[1].type = 1; /* SHT_PROGBITS */
  s[1].flags = 6; /* SHF_ALLOC | SHF_EXECINSTR */
  s[1].addr = m->link_low;
  for (i = 0; i < (size_t) m->size; i++)
    u8 (&s[1].data, 0x90);

  /* .strtab starts with the empty string.  */
  s[2].name = ".strtab";
  s[2].type = 3; /* SHT_STRTAB */
  u8 (&s[2].data, 0);
  function_name_offset = 1;
  extra_name_offset = 0;
  if (m->with_symtab && !m->no_function_symbol && m->symbol_name != NULL)
    str (&s[2].data, m->symbol_name);
  if (m->with_symtab && m->extra_symbol_name != NULL)
    {
      extra_name_offset = s[2].data.n;
      str (&s[2].data, m->extra_symbol_name);
    }

  /* .symtab with a null entry and one or two symbols.  */
  s[3].name = ".symtab";
  s[3].type = 2; /* SHT_SYMTAB */
  s[3].link = 2;
  s[3].info = 1;
  s[3].entsize = 24;
  if (m->with_symtab)
    {
      for (i = 0; i < 24; i++)
	u8 (&s[3].data, 0);
      if (!m->no_function_symbol && m->symbol_name != NULL)
	{
	  num (&s[3].data, function_name_offset, 4);
	  u8 (&s[3].data, 0x12); /* GLOBAL | STT_FUNC */
	  u8 (&s[3].data, 0);
	  num (&s[3].data, 1, 2); /* st_shndx = .text */
	  num (&s[3].data, m->link_low, 8);
	  num (&s[3].data, m->size, 8);
	}
      if (m->extra_symbol_name != NULL)
	{
	  num (&s[3].data, extra_name_offset, 4);
	  u8 (&s[3].data, 0x12);
	  u8 (&s[3].data, 0);
	  num (&s[3].data, 1, 2);
	  num (&s[3].data, m->extra_symbol_value, 8);
	  num (&s[3].data, m->extra_symbol_size, 8);
	}
    }

  s[4].name = ".debug_abbrev";
  s[4].type = 1; /* SHT_PROGBITS */

  s[5].name = ".debug_info";
  s[5].type = 1;

  s[6].name = ".debug_line";
  s[6].type = 1;

  if (m->with_debug)
    {
      /* Abbreviation 1: compile unit with name, comp_dir, stmt_list,
	 low_pc, and high_pc.  Abbreviation 2: subprogram with name,
	 low_pc, high_pc, decl_line, and decl_file.  Abbreviation 3:
	 inlined subroutine with name, low_pc, high_pc, call_file, and
	 call_line.  */
      u8 (&s[4].data, 1);
      u8 (&s[4].data, 0x11); /* DW_TAG_compile_unit */
      u8 (&s[4].data, 1);    /* DW_CHILDREN_yes */
      u8 (&s[4].data, 3);  u8 (&s[4].data, 8);  /* DW_AT_name, string */
      u8 (&s[4].data, 0x1b); u8 (&s[4].data, 8); /* DW_AT_comp_dir */
      u8 (&s[4].data, 0x10); u8 (&s[4].data, 0x17); /* DW_AT_stmt_list */
      u8 (&s[4].data, 0x11); u8 (&s[4].data, 1);  /* DW_AT_low_pc, addr */
      u8 (&s[4].data, 0x12); u8 (&s[4].data, 6);  /* DW_AT_high_pc, data4 */
      u8 (&s[4].data, 0); u8 (&s[4].data, 0);
      u8 (&s[4].data, 2);
      u8 (&s[4].data, 0x2e); /* DW_TAG_subprogram */
      u8 (&s[4].data, m->inline_name != NULL ? 1 : 0);
      u8 (&s[4].data, 3);  u8 (&s[4].data, 8);
      u8 (&s[4].data, 0x11); u8 (&s[4].data, 1);
      u8 (&s[4].data, 0x12); u8 (&s[4].data, 6);
      u8 (&s[4].data, 0x3a); u8 (&s[4].data, 0xb); /* DW_AT_decl_line */
      u8 (&s[4].data, 0x3b); u8 (&s[4].data, 0xb); /* DW_AT_decl_file */
      u8 (&s[4].data, 0); u8 (&s[4].data, 0);
      if (m->inline_name != NULL)
	{
	  u8 (&s[4].data, 3);
	  u8 (&s[4].data, 0x1d); /* DW_TAG_inlined_subroutine */
	  u8 (&s[4].data, 0);
	  u8 (&s[4].data, 3);  u8 (&s[4].data, 8);
	  u8 (&s[4].data, 0x11); u8 (&s[4].data, 1);
	  u8 (&s[4].data, 0x12); u8 (&s[4].data, 6);
	  u8 (&s[4].data, 0x58); u8 (&s[4].data, 0xb); /* DW_AT_call_file */
	  u8 (&s[4].data, 0x59); u8 (&s[4].data, 0xb); /* DW_AT_call_line */
	  u8 (&s[4].data, 0); u8 (&s[4].data, 0);
	}
      u8 (&s[4].data, 0); /* End of abbreviations.  */

      /* .debug_info: one compilation unit.  Length is patched below.  */
      num (&s[5].data, 0, 4);
      num (&s[5].data, 4, 2); /* version */
      num (&s[5].data, 0, 4); /* abbrev offset */
      u8 (&s[5].data, 8);     /* address size */
      u8 (&s[5].data, 1);     /* compile unit */
      str (&s[5].data, m->function_name);
      str (&s[5].data, "/fixture");
      num (&s[5].data, 0, 4); /* stmt_list */
      num (&s[5].data, m->link_low, 8);
      num (&s[5].data, m->size, 4); /* high_pc relative to low_pc */
      u8 (&s[5].data, 2);     /* subprogram */
      str (&s[5].data, m->function_name);
      num (&s[5].data, m->link_low, 8);
      num (&s[5].data, m->size, 4);
      u8 (&s[5].data, (unsigned) m->decl_line);
      u8 (&s[5].data, 1);     /* decl_file */
      if (m->inline_name != NULL)
	{
	  u8 (&s[5].data, 3);   /* inlined subroutine */
	  str (&s[5].data, m->inline_name);
	  num (&s[5].data, m->link_low + m->inline_low, 8);
	  num (&s[5].data, m->inline_high - m->inline_low, 4);
	  u8 (&s[5].data, (unsigned) m->call_file);
	  u8 (&s[5].data, (unsigned) m->call_line);
	  u8 (&s[5].data, 0);   /* End of subprogram children.  */
	}
      u8 (&s[5].data, 0);       /* End of compile unit children.  */
      patch (s[5].data.b, 0, s[5].data.n - 4, 4);

      /* .debug_line.  */
      num (&s[6].data, 0, 4);
      num (&s[6].data, 4, 2); /* version */
      {
	unsigned char lengths[] = {0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1};

	num (&s[6].data, 0, 4); /* header_length, patched below */
	u8 (&s[6].data, 1);     /* minimum_instruction_length */
	u8 (&s[6].data, 1);     /* maximum_operations_per_instruction */
	u8 (&s[6].data, 1);     /* default_is_stmt */
	u8 (&s[6].data, 251);   /* line_base = -5 */
	u8 (&s[6].data, 14);    /* line_range */
	u8 (&s[6].data, 13);    /* opcode_base */
	for (i = 0; i < sizeof lengths; i++)
	  u8 (&s[6].data, lengths[i]);
	u8 (&s[6].data, 0);     /* End of include directories.  */
	str (&s[6].data, m->source_name);
	u8 (&s[6].data, 0);     /* directory index */
	u8 (&s[6].data, 0);     /* mtime */
	u8 (&s[6].data, 0);     /* length */
	u8 (&s[6].data, 0);     /* End of file names.  */
      }
      header_end = s[6].data.n;
      patch (s[6].data.b, 6, header_end - 10, 4);

      /* Line program: a mapping at the module start, optionally one at
	 the inline range start, and a final mapping at the module end.
	 Addresses use DW_LNE_set_address so nothing depends on the
	 previous address.  */
      {
	uint64_t addrs[3];
	int naddrs;
	int a;
	int64_t current_line;

	addrs[0] = m->link_low;
	naddrs = 1;
	if (m->inline_name != NULL)
	  addrs[naddrs++] = m->link_low + m->inline_low;
	addrs[naddrs++] = m->link_low + m->size;

	current_line = 1;
	for (a = 0; a < naddrs; a++)
	  {
	    u8 (&s[6].data, 0);   /* Extended opcode.  */
	    u8 (&s[6].data, 9);   /* Length: opcode byte plus address.  */
	    u8 (&s[6].data, 2);   /* DW_LNE_set_address.  */
	    num (&s[6].data, addrs[a], 8);
	    u8 (&s[6].data, 3);   /* DW_LNS_advance_line.  */
	    sleb (&s[6].data, (int64_t) line - current_line);
	    current_line = (int64_t) line;
	    u8 (&s[6].data, 1);   /* DW_LNS_copy.  */
	  }
	u8 (&s[6].data, 0);       /* Extended opcode.  */
	u8 (&s[6].data, 1);       /* Length.  */
	u8 (&s[6].data, 1);       /* DW_LNE_end_sequence.  */
      }
      patch (s[6].data.b, 0, s[6].data.n - 4, 4);
    }

  s[7].name = ".shstrtab";
  s[7].type = 3;
  u8 (&s[7].data, 0);
  for (i = 1; i <= NSEC; i++)
    {
      names[i] = (uint32_t) s[7].data.n;
      str (&s[7].data, s[i].name);
    }

  /* Lay out the file: ELF header, section contents, then the section
     header table.  */
  for (i = 0; i < 64; i++)
    u8 (&file, 0);
  for (i = 1; i <= NSEC; i++)
    {
      while (file.n % 8 != 0)
	u8 (&file, 0);
      offsets[i] = file.n;
      for (o = 0; o < s[i].data.n; o++)
	u8 (&file, s[i].data.b[o]);
    }
  while (file.n % 8 != 0)
    u8 (&file, 0);
  sho = file.n;
  for (i = 0; i < (NSEC + 1) * 64; i++)
    u8 (&file, 0);

  memcpy (file.b, "\177ELF\2\1\1", 7);
  patch (file.b, 16, m->dynamic ? 3 : 2, 2);
  if (m->force_type != 0)
    patch (file.b, 16, (uint64_t) m->force_type, 2);
  if (m->force_class != 0)
    patch (file.b, 4, (uint64_t) m->force_class, 1);
  patch (file.b, 18, 62, 2);       /* EM_X86_64 */
  patch (file.b, 20, 1, 4);
  patch (file.b, 40, sho, 8);
  patch (file.b, 52, 64, 2);
  patch (file.b, 58, 64, 2);
  patch (file.b, 60, NSEC + 1, 2);
  patch (file.b, 62, NSEC, 2);
  for (i = 1; i <= NSEC; i++)
    {
      o = sho + i * 64;
      patch (file.b, o, names[i], 4);
      patch (file.b, o + 4, s[i].type, 4);
      patch (file.b, o + 8, s[i].flags, 8);
      patch (file.b, o + 16, s[i].addr, 8);
      patch (file.b, o + 24, offsets[i], 8);
      patch (file.b, o + 32, s[i].data.n, 8);
      patch (file.b, o + 40, s[i].link, 4);
      patch (file.b, o + 44, s[i].info, 4);
      patch (file.b, o + 48, 1, 8);
      patch (file.b, o + 56, s[i].entsize, 8);
    }

  fp = fopen (m->path, "wb");
  if (fp == NULL)
    return 0;
  o = fwrite (file.b, 1, file.n, fp);
  return fclose (fp) == 0 && o == file.n;
}

/* The original single-module fixture used by the baseline harness.  */

int
task_write_elf (const char *path, const char *function_name,
		const char *symbol_name, const char *source_name, int dynamic)
{
  struct task_elf_module m;

  memset (&m, 0, sizeof m);
  m.path = path;
  m.dynamic = dynamic;
  m.link_low = 0x1000;
  m.size = 16;
  m.function_name = function_name;
  m.symbol_name = symbol_name;
  m.source_name = source_name;
  m.line = 7;
  m.decl_line = 1;
  m.with_symtab = 1;
  m.with_debug = 1;

  return task_write_elf_module (&m);
}
