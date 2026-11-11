# Debug
We have forked the debug/ folder from the standard library, to take direct control of the debug/elf, debug/macho, and debug/pe binary format parsers. To these parsers, we have added the ability to also generate executable files from the parsed intermediate data structures. This lets us load a file with debug parsers, make changes by interacting with the parser structures, and then write those changes back out to a new file.

## Relocation editing
- `debug/elf`: `AddRelocation`, `AddRelocations`, `ReplaceRelocations`, `RemoveRelocations`, `AddRelocationForSymbol`, `AddRelocationForAddr`, `AddRelocationsToRelocSection`, `RemoveRelocationsFromRelocSection` (alloc relocs can now grow via a new PT_LOAD when needed)
- `debug/macho`: `AddRelocation`, `AddRelocations`, `ReplaceRelocations`, `RemoveRelocations`, `AddScatteredRelocation`, `AddRelocationForSymbol`, `AddRelocationForSymbolWithDylibOrdinal`, `SetDylibOrdinalForSymbol`, `SetBindKindForSymbol`
- `debug/pe`: `AddBaseRelocation`, `ReplaceBaseRelocations`, `RemoveBaseRelocations`, `AddBaseReloc`, `AddSectionRelocation`, `AddSectionRelocationForSymbol`, `ReplaceSectionRelocations`, `RemoveSectionRelocations`

## PE import parsing
Normal and delay import parsing resolve every reference through its own RVA against the section map, so descriptors, DLL names, thunk arrays and hint/name records can be split across sections; a descriptor or thunk may straddle two adjacent sections, and contiguous RVAs are allowed to map to discontiguous file offsets. Reads into unmapped gaps, ambiguously overlapping sections or VirtualSize-only regions without raw bytes fail with an error carrying the directory category and RVA instead of borrowing neighboring bytes or padding with zeros. The same image yields identical dependency lists through `NewFile` (disk layout) and `NewFileFromMemory` (mapped layout), since RVAs are never treated as shared file offsets. Public signatures are unchanged: the `name:dll` output, descriptor and symbol ordering, ordinal handling (consumed, omitted from the named list) and empty results when no import directory exists are preserved. When `OriginalFirstThunk` is zero the parser falls back to `FirstThunk`, but a bound IAT (nonzero `TimeDateStamp`) is reported as an error rather than guessed at. Delay descriptors honor the `GrAttrs` RVA/VA bit, validate VAs against the optional header image base without truncation, and retain the raw descriptor fields with only `DllName` resolved.


## Read more about the project here:
https://www.symbolcrash.com/2019/02/23/introducing-symbol-crash/
