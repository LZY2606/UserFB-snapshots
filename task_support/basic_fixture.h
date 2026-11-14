#ifndef TASK_BASIC_FIXTURE_H
#define TASK_BASIC_FIXTURE_H

#include <stdint.h>

/* Creates one inert little-endian ELF64 with ordinary DWARF4 and one symbol.
   No host assembler, linker or target program is used. */
int task_write_elf(const char *path, const char *function_name,
                   const char *symbol_name, const char *source_name, int dynamic);

/* A description of one synthetic ELF module.  This is an extension of
   task_write_elf used by the offline tests.  All link addresses are
   link time addresses; the bytes in the file are never loaded or
   relocated.  */
struct task_elf_module
{
  const char *path;

  /* Non-zero to write ET_DYN instead of ET_EXEC.  */
  int dynamic;
  /* If non-zero, overwrite e_type with this value (for example 1 for
     ET_REL) without changing the rest of the file.  */
  int force_type;
  /* If non-zero, overwrite EI_CLASS with this value (for example 1 for
     ELFCLASS32) without changing the rest of the file.  */
  int force_class;

  /* The link time address and size of the module's text and of its
     single compilation unit.  */
  uint64_t link_low;
  uint64_t size;

  /* The function and its symbol.  */
  const char *function_name;
  const char *symbol_name;
  const char *source_name;
  int line;
  int decl_line;

  /* If inline_name is not NULL, add one inlined subroutine covering
     [link_low + inline_low, link_low + inline_high) inside the
     function.  call_file is a line table file index (1 is the source
     file) and call_line is the line of the inlined call site.  */
  const char *inline_name;
  uint64_t inline_low;
  uint64_t inline_high;
  int call_file;
  int call_line;

  /* If extra_symbol_name is not NULL, add a second symbol with the
     given absolute link address and size.  */
  const char *extra_symbol_name;
  uint64_t extra_symbol_value;
  uint64_t extra_symbol_size;

  /* If non-zero, omit the function symbol, leaving only any extra
     symbol.  */
  int no_function_symbol;
  /* If non-zero, include a symbol table.  */
  int with_symtab;
  /* If non-zero, include DWARF .debug_info/.debug_abbrev/.debug_line.  */
  int with_debug;
};

/* Write the module described by M.  Returns 1 on success, 0 on
   error.  */
int task_write_elf_module(const struct task_elf_module *m);

#endif
