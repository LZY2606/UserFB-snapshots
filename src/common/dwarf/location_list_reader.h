// Copyright 2026 Google LLC
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google LLC nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// Reading of DWARF location lists (.debug_loc and .debug_loclists).
//
// The reader consumes a section map and an immutable snapshot of the
// compilation unit state that location lists depend on. It never mutates
// the snapshot and never assumes that a section contains only one
// contribution: every call is interpreted using the address size, offset
// size and base addresses that belong to the snapshot it was handed.

#ifndef COMMON_DWARF_LOCATION_LIST_READER_H__
#define COMMON_DWARF_LOCATION_LIST_READER_H__

#include <stdint.h>

#include <string>
#include <vector>

#include "common/dwarf/bytereader.h"
#include "common/dwarf/dwarf2enums.h"
#include "common/dwarf/dwarf2reader.h"

namespace google_breakpad {

// The compilation unit state needed to interpret a location list.  This is a
// value snapshot: copying it does not retain any ELF or section memory, and
// reusing a ByteReader to read another compilation unit cannot change it.
struct LocationListContext {
  LocationListContext()
      : version(0),
        endianness(ENDIANNESS_LITTLE),
        address_size(0),
        offset_size(0),
        low_pc(0),
        loclists_base(0),
        addr_base(0) {}

  // DWARF version of the owning compilation unit.
  uint16_t version;
  // Byte order of the owning ELF file.
  Endianness endianness;
  // Size of an address in the owning compilation unit.
  uint8_t address_size;
  // Size of a section offset (4 or 8) in the owning compilation unit.
  uint8_t offset_size;
  // DW_AT_low_pc of the root compilation unit DIE. This is the default base
  // address of DWARF 4 .debug_loc and of DWARF 5 DW_LLE_offset_pair entries.
  uint64_t low_pc;
  // DW_AT_loclists_base of the root compilation unit DIE. Offset tables for
  // DW_FORM_loclistx start here; entries are offsets relative to this value.
  uint64_t loclists_base;
  // DW_AT_addr_base of the root compilation unit DIE. Address indexes in
  // .debug_loclists are resolved through this base in .debug_addr.
  uint64_t addr_base;
};

// One decoded location list entry.  The expression bytes are owned by the
// entry. Section-relative offsets identify the entry and the first byte of
// its expression in the section named by |section|.
struct LocationEntry {
  LocationEntry()
      : begin(0),
        end(0),
        is_default(false),
        entry_offset(0),
        expression_offset(0) {}

  // Half-open address range [begin, end). Default entries have an empty
  // range and are only consulted when no explicit entry matches.
  uint64_t begin;
  uint64_t end;
  // Raw DWARF expression bytes, in encoding order.
  std::vector<uint8_t> expression;
  bool is_default;
  // Section-relative offset of this entry (the LLE or old address pair).
  uint64_t entry_offset;
  // Section-relative offset of the first expression byte.
  uint64_t expression_offset;
  // ".debug_loc" or ".debug_loclists".
  std::string section;
};

// A diagnostic describing why location list data could not be read.  Both
// fields are filled so callers can report where the problem was found.
struct LocationListDiagnostic {
  LocationListDiagnostic() : offset(0) {}

  bool empty() const { return message.empty(); }

  std::string section;
  uint64_t offset;
  std::string message;
};

class LocationListReader {
 public:
  LocationListReader(const SectionMap& sections,
                     const LocationListContext& context);
  ~LocationListReader();

  LocationListReader(const LocationListReader&) = delete;
  LocationListReader& operator=(const LocationListReader&) = delete;

  // Reads the location list named by FORM/VALUE into OUT, preserving the
  // encoded order and the raw expression bytes. FORM must be
  // DW_FORM_sec_offset or DW_FORM_loclistx. Returns false and leaves a
  // diagnostic on failure; never returns an empty success for malformed or
  // unsupported data.
  bool ReadLocations(enum DwarfForm form, uint64_t value,
                     std::vector<LocationEntry>* out);

  // Selects the first explicit entry whose half-open range contains PC. If
  // no explicit entry matches, the first default entry is used. A matching
  // explicit entry with an empty expression returns true and sets an empty
  // EXPRESSION; it does not fall back to a default entry. Zero-length ranges
  // never match and entries are not merged.
  static bool SelectLocation(const std::vector<LocationEntry>& entries,
                             uint64_t pc,
                             std::vector<uint8_t>* expression);

  // Same as above, additionally reporting which entry was selected.
  static bool SelectLocation(const std::vector<LocationEntry>& entries,
                             uint64_t pc,
                             std::vector<uint8_t>* expression,
                             const LocationEntry** selected_entry);

  const LocationListDiagnostic& diagnostic() const { return diagnostic_; }

 private:
  bool Fail(const std::string& section, uint64_t offset,
            const std::string& message);
  bool CheckContext();
  bool ReadDebugLoc(uint64_t offset, std::vector<LocationEntry>* out);
  bool ReadDebugLocLists(uint64_t offset, std::vector<LocationEntry>* out);

  // Resolves an index into .debug_addr using the snapshot's address base and
  // address size.
  bool ReadAddressIndex(uint64_t index, uint64_t* address);
  // Reads an address of the snapshot's address size at a section offset.
  uint64_t ReadAddressAt(const uint8_t* buffer, uint64_t size, uint64_t offset,
                         bool* ok);

  const SectionMap& sections_;
  const LocationListContext context_;
  ByteReader reader_;
  LocationListDiagnostic diagnostic_;
};

}  // namespace google_breakpad

#endif  // COMMON_DWARF_LOCATION_LIST_READER_H__
