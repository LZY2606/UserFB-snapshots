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

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "parse.h"

namespace peparse {

// Which header field of a debug directory entry located a payload
// candidate.
enum class debug_candidate_source {
  file_pointer, // PointerToRawData, a direct file offset
  rva           // AddressOfRawData, mapped through the section table
};

// Which file region fully owns a candidate byte range.
enum class debug_owner_kind {
  headers, // before the end of SizeOfHeaders
  section, // inside one section's raw data
  overlay, // at or past the end of headers and all non-empty sections
  gap      // between headers/sections, before the overlay
};

// How the file-pointer and RVA candidates of one record relate.
enum class debug_relation {
  none,           // zero-length record, no coordinate is dereferenced
  file_only,      // only PointerToRawData is non-zero
  rva_only,       // only AddressOfRawData is non-zero
  same,           // both coordinates resolve to the same file offset
  equal_distinct, // different file offsets, identical bytes
  conflict        // different file offsets, different bytes
};

enum class codeview_format {
  rsds,
  nb10,
  opaque // unknown CodeView signature or a non-CodeView debug type
};

// Interpretation of one candidate's payload bytes. Only the fields
// that belong to format are meaningful; the path is never part of an
// identity.
struct codeview_record {
  codeview_format format = codeview_format::opaque;

  // RSDS identity fields
  std::array<std::uint8_t, 16> guid{}; // raw 16 bytes as stored
  std::string guid_text;               // 8-4-4-4-12 field-endian text

  // shared RSDS/NB10 identity field
  std::uint32_t age = 0;

  // NB10 identity fields (signature is not a GUID)
  std::uint32_t offset = 0;
  std::uint32_t signature = 0;

  // payload bytes after the fixed header, up to the NUL terminator
  std::string path;
};

// One located payload candidate. The byte range always belongs to a
// single owner; ranges that would span owners are reported as errors
// instead of being spliced together.
struct debug_payload_candidate {
  debug_candidate_source source = debug_candidate_source::file_pointer;
  std::uint32_t file_offset = 0; // absolute offset in the original file
  debug_owner_kind owner = debug_owner_kind::gap;
  std::uint32_t owner_section = 0; // section table index, owner == section
  std::vector<std::uint8_t> bytes;
  codeview_record codeview;
};

// One 28-byte debug directory entry, kept even when it is a
// zero-length marker or duplicates another entry.
struct debug_record {
  std::uint32_t ordinal = 0;           // index within the directory
  std::uint32_t entry_file_offset = 0; // file offset of this entry
  std::uint32_t directory_section_index = 0; // section holding the directory
  std::array<std::uint8_t, 28> raw_entry{};  // the entry as stored

  std::uint32_t characteristics = 0;
  std::uint32_t timestamp = 0;
  std::uint16_t major_version = 0;
  std::uint16_t minor_version = 0;
  std::uint32_t type = 0;
  std::uint32_t size_of_data = 0;
  std::uint32_t address_of_raw_data = 0;
  std::uint32_t pointer_to_raw_data = 0;

  debug_relation relation = debug_relation::none;
  std::vector<debug_payload_candidate> candidates; // file candidate first
};

struct debug_records {
  std::vector<debug_record> records;

  // Set when the directory could not be read completely. The records
  // vector then holds only what was decoded before the problem and
  // must not be treated as a complete result.
  bool has_error = false;
  std::uint32_t error_entry_file_offset = 0; // 0xffffffff if not located
  std::string error_message;
};

// Copy the debug directory records of pe into out. Every record of the
// Size-delimited directory is kept, including zero-length markers and
// duplicates. Returns false when the directory is truncated, unmapped,
// ambiguous, overflowing or otherwise unreadable, or when a recognized
// CodeView payload is truncated or misses its NUL terminator; in that
// case out.error_message and out.error_entry_file_offset describe the
// failing entry. The result owns all of its data and stays valid after
// DestructParsedPE.
bool ReadDebugRecords(parsed_pe *pe, debug_records &out);

} // namespace peparse
