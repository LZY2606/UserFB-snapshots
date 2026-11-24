These are CC0-1.0, locally authored, freestanding x86-64 ELF samples for read-only analysis. No target program needs to run. The C sources were compiled with Clang 21, linked with LLD 20, packed with LLVM dwp 20.1.8 and stripped with LLVM objcopy 20.1.8. The library and runner need only Go; they do not need these authoring tools.

Each directory contains device.elf (no .symtab or .dynsym), device.elf.dwp (DWARF5 CU package), and queries.json (a JSON array of unsigned, link-time PCs). Both images use the same compilation inputs, but the link base, link order and package order differ. Treat the files as inputs. There is no expected-result table here.

The retained image sections are .debug_info, .debug_abbrev, .debug_str, .debug_str_offsets, .debug_addr, .debug_line and .debug_line_str. The supported production path does not need to interpret line programs, type layout, locations or machine instructions. The split files contain strx1, addrx, rnglistx, unit-relative DIE references, and offset_pair ranges with end_of_list. Symbol tables and individual .dwo files are not supplied.

Read all inputs as bytes. Do not execute the ELF, dynamically load it, open the embedded source paths or fetch external .dwo files.
