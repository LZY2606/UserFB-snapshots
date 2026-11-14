/*
The MIT License (MIT)

Copyright (c) 2013 Andrew Ruef

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

// This header is deliberately limited to freestanding-safe C/C++ headers.
// It must stay includable with an arbitrary first include directory (for
// example a project root containing a file named "version"), so no C++
// container or string header is pulled in. All variable-length data is
// exposed as owning arrays paired with their element counts.

#pragma once

#include <cstdint>
#include <stddef.h>

namespace peparse {

// Structural category of a rejected preview. Every error carries the
// category together with the actual file offset and/or RVA at which the
// structure could not be interpreted.
enum RelocPreviewErrorCategory {
  RelocPreviewError_None = 0,
  RelocPreviewError_Header,         // DOS/NT headers, magic, byte reversal
  RelocPreviewError_Machine,        // unsupported machine / type mismatch
  RelocPreviewError_Base,           // base width or base delta range
  RelocPreviewError_RelocsStripped, // RELOCS_STRIPPED with a non-zero move
  RelocPreviewError_Directory,      // absent or malformed data directory
  RelocPreviewError_Block,          // block size, alignment, truncation
  RelocPreviewError_Entry,          // unknown entry / missing HIGHADJ pair
  RelocPreviewError_Mapping,        // no unique raw-backed byte for an RVA
  RelocPreviewError_Overlap,        // target writes intersect the directory
  RelocPreviewError_Memory          // report allocation failure
};

enum RelocPreviewKind {
  RelocPreviewKind_Absolute = 0,
  RelocPreviewKind_High,
  RelocPreviewKind_Low,
  RelocPreviewKind_HighLow,
  RelocPreviewKind_HighAdj,
  RelocPreviewKind_Dir64
};

// Field width that an operation previews at its target.
enum RelocPreviewWidth {
  RelocPreviewWidth_None = 0,
  RelocPreviewWidth_Word16 = 2,
  RelocPreviewWidth_Dword32 = 4,
  RelocPreviewWidth_Qword64 = 8
};

struct RelocPreviewError {
  RelocPreviewErrorCategory category;
  char message[256];

  bool hasFileOffset;
  std::uint64_t fileOffset;

  bool hasRva;
  std::uint32_t rva;
};

// One contiguous run of original file bytes used while reading a field.
// A field whose RVA range crosses a section boundary is represented by
// several fragments, one per section, in RVA order.
struct RelocPreviewFragment {
  std::uint16_t sectionIndex;
  std::uint32_t fileOffset;
  std::uint32_t byteCount;
  std::uint8_t *bytes;

  RelocPreviewFragment()
      : sectionIndex(0), fileOffset(0), byteCount(0), bytes(nullptr) {
  }

  ~RelocPreviewFragment() {
    delete[] bytes;
  }

private:
  RelocPreviewFragment(const RelocPreviewFragment &);
  RelocPreviewFragment &operator=(const RelocPreviewFragment &);
};

// A single interpreted 16-bit base relocation table entry (including the
// word consumed by a HIGHADJ pair). All arrays are owned by the report.
struct RelocPreviewOperation {
  // Position among all operations across all blocks; ABSOLUTE included.
  size_t ordinal;

  // Block-relative index of the raw entry that produced this operation.
  size_t entryIndex;

  RelocPreviewKind kind;
  RelocPreviewWidth width;

  // File location and frozen value of the raw 16-bit entry word.
  std::uint16_t sectionIndex;
  std::uint32_t entryFileOffset;
  std::uint16_t rawEntry;
  std::uint32_t entryFragmentCount;
  RelocPreviewFragment *entryFragments;

  // Target RVA. Meaningful for every kind except Absolute, which is
  // sourced padding and never reads a target.
  std::uint32_t targetRva;

  // Field bytes immediately before and after this operation. Later
  // operations observe the bytes previewed by earlier ones.
  std::uint32_t beforeByteCount;
  std::uint8_t *beforeBytes;
  std::uint32_t afterByteCount;
  std::uint8_t *afterBytes;

  // Section index and original file fragment(s) of the target field,
  // located byte by byte through the section table.
  std::uint32_t targetFragmentCount;
  RelocPreviewFragment *targetFragments;

  // HIGHADJ only: location, value and source fragment of the paired
  // 16-bit word consumed from the same block.
  bool hasPair;
  std::uint16_t pairSectionIndex;
  std::uint32_t pairFileOffset;
  std::uint16_t pairRawValue;
  std::uint32_t pairFragmentCount;
  RelocPreviewFragment *pairFragments;

  RelocPreviewOperation()
      : ordinal(0),
        entryIndex(0),
        kind(RelocPreviewKind_Absolute),
        width(RelocPreviewWidth_None),
        sectionIndex(0),
        entryFileOffset(0),
        rawEntry(0),
        entryFragmentCount(0),
        entryFragments(nullptr),
        targetRva(0),
        beforeByteCount(0),
        beforeBytes(nullptr),
        afterByteCount(0),
        afterBytes(nullptr),
        targetFragmentCount(0),
        targetFragments(nullptr),
        hasPair(false),
        pairSectionIndex(0),
        pairFileOffset(0),
        pairRawValue(0),
        pairFragmentCount(0),
        pairFragments(nullptr) {
  }

  ~RelocPreviewOperation() {
    delete[] entryFragments;
    delete[] beforeBytes;
    delete[] afterBytes;
    delete[] targetFragments;
    delete[] pairFragments;
  }

private:
  RelocPreviewOperation(const RelocPreviewOperation &);
  RelocPreviewOperation &operator=(const RelocPreviewOperation &);
};

struct RelocPreviewBlock {
  // PageRVA and BlockSize as stored in the block header.
  std::uint32_t pageRva;
  std::uint32_t blockSize;

  // Offset of the block header relative to the directory's first RVA,
  // and its absolute file offset.
  std::uint32_t directoryOffset;
  std::uint32_t headerFileOffset;

  // Frozen source bytes of the 8-byte block header, located through the
  // section table like every other field read.
  std::uint32_t headerFragmentCount;
  RelocPreviewFragment *headerFragments;

  // Operations in raw table order; ABSOLUTE padding is included.
  std::uint32_t operationCount;
  RelocPreviewOperation *operations;

  RelocPreviewBlock()
      : pageRva(0),
        blockSize(0),
        directoryOffset(0),
        headerFileOffset(0),
        headerFragmentCount(0),
        headerFragments(nullptr),
        operationCount(0),
        operations(nullptr) {
  }

  ~RelocPreviewBlock() {
    delete[] headerFragments;
    delete[] operations;
  }

private:
  RelocPreviewBlock(const RelocPreviewBlock &);
  RelocPreviewBlock &operator=(const RelocPreviewBlock &);
};

// A merged run of previewed bytes, identified by RVA. Different RVAs
// aliasing the same physical file byte produce separate intervals and
// never cross-write each other.
struct RelocPreviewInterval {
  std::uint32_t firstRva;
  std::uint32_t lastRva; // inclusive

  std::uint32_t originalByteCount;
  std::uint8_t *originalBytes;
  std::uint32_t finalByteCount;
  std::uint8_t *finalBytes;

  // Ordinals of the operations that touched this interval, in execution
  // order, corresponding to the per-operation intermediate records.
  std::uint32_t operationOrdinalCount;
  size_t *operationOrdinals;

  RelocPreviewInterval()
      : firstRva(0),
        lastRva(0),
        originalByteCount(0),
        originalBytes(nullptr),
        finalByteCount(0),
        finalBytes(nullptr),
        operationOrdinalCount(0),
        operationOrdinals(nullptr) {
  }

  ~RelocPreviewInterval() {
    delete[] originalBytes;
    delete[] finalBytes;
    delete[] operationOrdinals;
  }

private:
  RelocPreviewInterval(const RelocPreviewInterval &);
  RelocPreviewInterval &operator=(const RelocPreviewInterval &);
};

struct RelocPreviewReport {
  bool pe32Plus;

  std::uint64_t oldImageBase;
  std::uint64_t newImageBase;

  // Mathematical difference newImageBase - oldImageBase, guaranteed to be
  // representable by int64_t.
  std::int64_t baseDelta;

  // Frozen copy of the relocation directory location.
  bool directoryPresent;
  std::uint32_t directoryRva;
  std::uint32_t directorySize;

  // Blocks and operations in directory order.
  std::uint32_t blockCount;
  RelocPreviewBlock *blocks;

  // All previewed bytes merged into maximal contiguous RVA runs, sorted
  // by RVA.
  std::uint32_t intervalCount;
  RelocPreviewInterval *intervals;

  RelocPreviewReport()
      : pe32Plus(false),
        oldImageBase(0),
        newImageBase(0),
        baseDelta(0),
        directoryPresent(false),
        directoryRva(0),
        directorySize(0),
        blockCount(0),
        blocks(nullptr),
        intervalCount(0),
        intervals(nullptr) {
  }

  ~RelocPreviewReport() {
    delete[] blocks;
    delete[] intervals;
  }

private:
  RelocPreviewReport(const RelocPreviewReport &);
  RelocPreviewReport &operator=(const RelocPreviewReport &);
};

struct RelocPreviewResult {
  bool ok;
  RelocPreviewError error;

  // Owned by the caller when ok is true; null on failure. Release it with
  // DestroyBaseRelocationPreview. The report owns all of its byte arrays
  // and stays valid after the input buffer is released.
  RelocPreviewReport *report;
};

// Free a report returned by BuildBaseRelocationPreview. A null pointer is
// accepted.
void DestroyBaseRelocationPreview(RelocPreviewReport *report);

// Build a self-contained, static explanation of how every pointer field
// covered by the base relocation directory would change if the image
// base moved from the file's ImageBase to newImageBase.
//
// The input is the complete, read-only PE image file bytes; it is never
// modified, no image is loaded and no code is executed. The returned
// report owns all of its data and remains valid after the input buffer
// is released.
//
// Supported inputs:
//   * little-endian I386 PE32 images (HIGH, LOW, HIGHLOW, HIGHADJ and
//     ABSOLUTE entries)
//   * little-endian AMD64 PE32+ images (DIR64 and ABSOLUTE entries)
// Files marked with either byte-reversed characteristic are rejected.
//
// On failure no partial report is published: ok is false, report is null
// and error describes the structural category and the offending file
// offset or RVA. A missing directory yields a successful, empty report.
// The function is stateless, so a failed call never affects later calls.
RelocPreviewResult BuildBaseRelocationPreview(const std::uint8_t *data,
                                              size_t size,
                                              std::uint64_t newImageBase);

} // namespace peparse
