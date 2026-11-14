# COFF layouts used by these synthetic objects

All integer fields are little endian. The objects are read, never loaded or executed.

The standard COFF header is 20 bytes. BigObj v2 uses a 56-byte header:

| Offset | Size | Meaning |
|---|---|---|
| 0 | 2 | Sig1 = 0 |
| 2 | 2 | Sig2 = 0xffff |
| 4 | 2 | Version = 2 |
| 6 | 2 | Machine = 0x8664 |
| 8 | 4 | TimeDateStamp |
| 12 | 16 | ClassID bytes c7 a1 ba d1 ee ba a9 4b af 20 fa f6 6a a4 dc b8 |
| 28 | 16 | SizeOfData, Flags, MetaDataSize, MetaDataOffset (four uint32 values) |
| 44 | 4 | NumberOfSections |
| 48 | 4 | PointerToSymbolTable |
| 52 | 4 | NumberOfSymbols, including auxiliary slots |

The section table starts immediately after this header and has ordinary 40-byte COFF section headers. There is no DOS/PE optional header in a BigObj. The four metadata fields above are zero in the supplied basic object; metadata interpretation is outside this task. A short import object shares the initial signature but has a different header and is not a BigObj.

A standard symbol slot is 18 bytes: Name[8], Value:u32, SectionNumber:i16, Type:u16, StorageClass:u8, NumberOfAuxSymbols:u8. A BigObj slot is 20 bytes and changes only SectionNumber to i32. Auxiliary slots have the same stride as primary slots and are opaque bytes unless their owner defines their format. The string table immediately follows all symbol slots. Its first uint32 is total length including that uint32. A long primary symbol name has four zero bytes followed by a string-table offset; a section can use slash plus decimal offset. SectionNumber is one based when positive; 0 is undefined (Value>0 represents common), -1 absolute and -2 debug.

For this task canonical weak externals have StorageClass=105, SectionNumber=0, Value=0, and one auxiliary slot. In that auxiliary slot the first uint32 is TagIndex, the next uint32 is Characteristics, and the remaining bytes are unused. Characteristics 1/2/3 are SEARCH_NOLIBRARY/SEARCH_LIBRARY/SEARCH_ALIAS. Return their declared conditional fallback path without library searches or provider selection. Each TagIndex and relocation SymbolTableIndex addresses the entire original slot table, including auxiliary records. A relocation is 10 bytes: VirtualAddress:u32, SymbolTableIndex:u32, Type:u16.

A positive terminal symbol identifies a section plus Value. Report a file location only when the value names an actual raw byte of that section; keep zero-fill/out-of-raw-range explicitly without inventing bytes. This task does not interpret instruction addends or relocation arithmetic. Unknown primary storage classes may terminate as ordinary symbols according to their section classification; an unknown weak search mode is an error.

The representation details above follow [LLVM's COFF object structures](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/Object/COFF.h) and [Microsoft's COFF symbol and weak-external specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format#auxiliary-format-3-weak-externals). These links document the format and are not runtime dependencies. `fixture.Basic` supplies ordinary and BigObj normal input only; weak-chain regression objects are to be written with the feature.
