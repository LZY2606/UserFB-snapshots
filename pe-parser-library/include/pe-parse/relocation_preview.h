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

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace peparse {

// Kind of a relocation entry, mirroring the on-disk 4-bit type field.
enum relocation_preview_kind {
  RPK_ABSOLUTE = 0, // padding entry; its target is never read or written
  RPK_HIGH = 1,     // add the high 16 bits of the low 32 bits of delta
  RPK_LOW = 2,      // add the low 16 bits of the low 32 bits of delta
  RPK_HIGHLOW = 3,  // add delta modulo 2^32 to a 32-bit field
  RPK_HIGHADJ = 4,  // paired HIGH/LOW adjustment, consumes one following word
  RPK_DIR64 = 10    // add delta modulo 2^64 to a 64-bit field
};

// Width, in bytes, of the field a relocation reads and rewrites.
enum relocation_preview_width {
  RPW_NONE = 0, // ABSOLUTE: no target field
  RPW_16 = 2,
  RPW_32 = 4,
  RPW_64 = 8
};

// Structural category of a rejected input. Every error also reports the
// actual file offset or RVA where the problem was found.
enum relocation_preview_error_category {
  RPE_NONE = 0,
  RPE_FORMAT,        // malformed DOS/NT headers, section table or directory
  RPE_UNSUPPORTED,   // machine, layout or relocation flavour not supported
  RPE_MACHINE_MISMATCH, // relocation type not valid for the target machine
  RPE_UNKNOWN_TYPE,  // relocation type outside the supported set
  RPE_ADDRESS,       // RVA/offset does not map to a unique raw byte
  RPE_TRUNCATION,    // a structure or field runs past the end of the file
  RPE_OVERFLOW,      // base move does not fit in int64_t or address wraps
  RPE_LAYOUT_CONFLICT // target writes alias each other or the reloc directory
};

struct relocation_preview_error {
  relocation_preview_error_category category;
  bool location_is_rva; // true: rva is set; false: file_offset is set
  std::uint32_t file_offset;
  std::uint32_t rva;
  const char *what; // static, NUL-terminated diagnostic text
};

// One contiguous run of raw bytes used while reading a field. A single field
// may produce several spans when a continuous RVA range crosses a section
// boundary.
struct relocation_preview_span {
  std::size_t section_index; // index of the section in the section table
  std::uint32_t file_offset;
  std::uint32_t size;
};

// A value read from the target field, kept at its natural width. The bytes
// are little-endian copies of the (possibly previewed) file contents.
struct relocation_preview_value {
  relocation_preview_width width;
  std::uint8_t bytes[8];
};

// One raw 16-bit relocation entry, in directory order.
struct relocation_preview_op {
  std::uint32_t entry_file_offset; // file offset of the raw 16-bit entry
  std::uint16_t raw_entry;         // original 16-bit type/offset word
  std::uint8_t raw_type;           // high nibble of raw_entry
  std::uint16_t page_offset;       // low 12 bits of raw_entry
  relocation_preview_kind kind;
  relocation_preview_width field_width;

  std::uint32_t target_rva; // PageRVA + page_offset; 0 for ABSOLUTE padding

  // Spans covering every byte of the target field, located byte by byte
  // through the section table. Empty for ABSOLUTE entries.
  std::vector<relocation_preview_span> target_spans;

  // Field value immediately before and after this entry is applied.
  // ABSOLUTE entries report RPW_NONE. Later entries see the bytes written by
  // earlier entries, never a fresh copy of the original file.
  relocation_preview_value before;
  relocation_preview_value after;

  // HIGHADJ only: the following 16-bit word inside the same block that the
  // adjustment consumes as its signed low half-word.
  bool has_pair;
  std::uint32_t pair_file_offset;
  std::uint16_t pair_raw;
  std::int16_t pair_signed;
};

// One IMAGE_BASE_RELOCATION block, in directory order.
struct relocation_preview_block {
  std::uint32_t header_file_offset; // file offset of PageRVA/BlockSize
  std::uint32_t page_rva;
  std::uint32_t block_size;
  std::vector<relocation_preview_op> ops;
};

// One merged run of target bytes, sorted by RVA, with both the original
// bytes and the bytes produced by replaying every relocation in order.
struct relocation_preview_interval {
  std::uint32_t rva;
  std::vector<std::uint8_t> original_bytes;
  std::vector<std::uint8_t> preview_bytes;
  // Indices, into the report's flattened operation list, of every operation
  // touching this interval, in application order.
  std::vector<std::size_t> op_indices;
};

// Self-contained interpretation report. A report owns all of its storage and
// remains valid after the input buffer passed to BuildBaseRelocationPreview
// has been freed.
struct base_relocation_preview {
  bool is_pe32_plus; // false: PE32 (I386), true: PE32+ (AMD64)
  std::uint16_t machine;
  std::uint64_t old_image_base;
  std::uint64_t new_image_base;
  std::int64_t delta; // new_image_base - old_image_base, mathematical

  bool directory_present;
  std::uint32_t directory_rva;
  std::uint32_t directory_size;
  std::uint32_t directory_file_offset;

  std::vector<relocation_preview_block> blocks;
  std::vector<relocation_preview_interval> intervals;

  // Number of operations across every block (ABSOLUTE entries included).
  std::size_t total_op_count() const;
};

// Interpret the base relocations of a read-only little-endian PE image and
// preview every pointer field after moving ImageBase to new_image_base.
//
// The input bytes are never modified and are not retained: on success the
// returned report owns all of its data. On failure no report is published and
// the error detail is returned; later calls are unaffected by earlier
// failures.
//
// Only standard little-endian I386 PE32 and AMD64 PE32+ images are accepted.
// This is a purely static byte-level interpretation: nothing is loaded or
// executed and no file is written. A missing relocation directory together
// with a zero move yields an empty report; a missing directory together with
// a non-zero move is an error rather than a guess.
bool BuildBaseRelocationPreview(const std::uint8_t *data,
                                std::size_t size,
                                std::uint64_t new_image_base,
                                base_relocation_preview &report_out,
                                relocation_preview_error &error_out);

} // namespace peparse
