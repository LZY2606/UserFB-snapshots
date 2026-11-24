Small MSF container support feeds module-symbol analysis. Legacy CodeView record conversion is outside scope.

Small MSF 2.00 (all integers little-endian):

* 0: 44-byte magic, `Microsoft C/C++ program database 2.00\r\n\x1aJG` plus two zero bytes.
* 44: u32 page size; 48: u16 FPM page; 50: u16 page count.
* 52: u32 stream-directory byte length; 56: obsolete u32 memory-pointer slot (opaque).
* 60: ceil(directory_size/page_size) u16 **directory data page numbers**, directly. There is no MSF7 block-map level here.
* The logical directory starts with u16 stream count and u16 reserved zero. It has one `(u32 byte_size, u32 obsolete_pointer)` descriptor per stream, then all stream page-number lists, in stream order, with u16 page numbers. Obsolete pointers do not identify file offsets.
* byte_size 0xffffffff means nil; nil and empty streams each consume zero page numbers. Other streams consume ceil(byte_size/page_size) page numbers. Nonempty final pages can be partial.
* Small page sizes 1024, 2048 and 4096 have FPM choices (1,9), (1,5) and (1,2), respectively. Do not apply the MSF7 fixed FPM rule to every Small page size.

MSF7 retains the existing 32-bit fields and two-level directory block map.

Primary format reference: Microsoft PDB implementation, `PDB/msf/msf.cpp`, `MSF_HDR`, `SI_PERSIST`, `internalSerializeSmallMsf` and `rgmsfparms`:
https://github.com/microsoft/microsoft-pdb/blob/master/PDB/msf/msf.cpp
Modern container background: https://llvm.org/docs/PDB/MsfFile.html

`small_fixture.h` is only a serializer for legal synthetic bytes. It does not
implement a reader or the requested production state. `msf7_fixture.h` retains
the original normal container builder. Neither builder executes an image.
