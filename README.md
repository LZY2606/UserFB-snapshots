# Debug
We have forked the debug/ folder from the standard library, to take direct control of the debug/elf, debug/macho, and debug/pe binary format parsers. To these parsers, we have added the ability to also generate executable files from the parsed intermediate data structures. This lets us load a file with debug parsers, make changes by interacting with the parser structures, and then write those changes back out to a new file.

## Relocation editing
- `debug/elf`: `AddRelocation`, `AddRelocations`, `ReplaceRelocations`, `RemoveRelocations`, `AddRelocationForSymbol`, `AddRelocationForAddr`, `AddRelocationsToRelocSection`, `RemoveRelocationsFromRelocSection` (alloc relocs can now grow via a new PT_LOAD when needed)
- `debug/macho`: `AddRelocation`, `AddRelocations`, `ReplaceRelocations`, `RemoveRelocations`, `AddScatteredRelocation`, `AddRelocationForSymbol`, `AddRelocationForSymbolWithDylibOrdinal`, `SetDylibOrdinalForSymbol`, `SetBindKindForSymbol`
- `debug/pe`: `AddBaseRelocation`, `ReplaceBaseRelocations`, `RemoveBaseRelocations`, `AddBaseReloc`, `AddSectionRelocation`, `AddSectionRelocationForSymbol`, `ReplaceSectionRelocations`, `RemoveSectionRelocations`


## Read more about the project here:
https://www.symbolcrash.com/2019/02/23/introducing-symbol-crash/

## AMD64 offline unwind
- `debug/pe`: `File.UnwindAMD64` reconstructs one previous stack frame of an AMD64 PE32+ image from a crash snapshot, using the exception directory's `RUNTIME_FUNCTION`/`UNWIND_INFO` metadata. It is read-only, never executes machine code or language handlers, and documents the supported opcodes and error cases in [`docs/amd64-unwind.md`](docs/amd64-unwind.md).
