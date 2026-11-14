#ifndef TASK_BASIC_FIXTURE_H
#define TASK_BASIC_FIXTURE_H
/* Creates one inert little-endian ELF64 with ordinary DWARF4 and one symbol.
   No host assembler, linker or target program is used. */
int task_write_elf(const char *path, const char *function_name,
                   const char *symbol_name, const char *source_name, int dynamic);
#endif
