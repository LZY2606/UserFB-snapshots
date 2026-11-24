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

#include "common/dwarf/location_list_reader.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "common/dwarf/bytereader-inl.h"

namespace google_breakpad {

namespace {

const char kDebugLoc[] = ".debug_loc";
const char kDebugLocLists[] = ".debug_loclists";
const char kDebugAddr[] = ".debug_addr";

uint64_t MaxAddressForSize(uint8_t address_size) {
  if (address_size == 4)
    return 0xffffffffULL;
  return std::numeric_limits<uint64_t>::max();
}

// Reads an unsigned LEB128 value from BUFFER[OFFSET..SIZE) without reading
// past the end of the section. Returns false on truncation.
bool ReadULeb128(const uint8_t* buffer, uint64_t size, uint64_t offset,
                 ByteReader* reader, uint64_t* value, uint64_t* length) {
  if (offset >= size)
    return false;
  size_t len = 0;
  *value = reader->ReadUnsignedLEB128(buffer + offset, &len);
  if (len > size - offset)
    return false;
  *length = len;
  return true;
}

}  // namespace

LocationListReader::LocationListReader(const SectionMap& sections,
                                       const LocationListContext& context)
    : sections_(sections),
      context_(context),
      reader_(context.endianness) {
  if (context_.address_size == 4 || context_.address_size == 8)
    reader_.SetAddressSize(context_.address_size);
  if (context_.offset_size == 4 || context_.offset_size == 8)
    reader_.SetOffsetSize(context_.offset_size);
}

LocationListReader::~LocationListReader() = default;

bool LocationListReader::Fail(const std::string& section, uint64_t offset,
                              const std::string& message) {
  diagnostic_.section = section;
  diagnostic_.offset = offset;
  diagnostic_.message = message;
  return false;
}

bool LocationListReader::CheckContext() {
  if (context_.address_size != 4 && context_.address_size != 8) {
    return Fail(kDebugLoc, 0,
                "unsupported compilation unit address size " +
                    std::to_string(context_.address_size));
  }
  if (context_.offset_size != 4 && context_.offset_size != 8) {
    return Fail(kDebugLoc, 0,
                "unsupported compilation unit offset size " +
                    std::to_string(context_.offset_size));
  }
  return true;
}

uint64_t LocationListReader::ReadAddressAt(const uint8_t* buffer,
                                           uint64_t size, uint64_t offset,
                                           bool* ok) {
  if (offset > size || context_.address_size > size - offset) {
    *ok = false;
    return 0;
  }
  *ok = true;
  return reader_.ReadAddress(buffer + offset);
}

bool LocationListReader::ReadAddressIndex(uint64_t index, uint64_t* address) {
  SectionMap::const_iterator addr = GetSectionByName(sections_, kDebugAddr);
  if (addr == sections_.end()) {
    return Fail(kDebugLocLists, context_.addr_base,
                "DW_FORM_loclistx requires a .debug_addr section");
  }
  const uint8_t* buffer = addr->second.first;
  const uint64_t size = addr->second.second;
  const uint64_t offset = context_.addr_base + index * context_.address_size;
  bool ok = false;
  const uint64_t value = ReadAddressAt(buffer, size, offset, &ok);
  if (!ok) {
    return Fail(kDebugAddr, offset,
                "address index " + std::to_string(index) +
                    " points outside .debug_addr");
  }
  *address = value;
  return true;
}

bool LocationListReader::ReadLocations(enum DwarfForm form, uint64_t value,
                                       std::vector<LocationEntry>* out) {
  diagnostic_ = LocationListDiagnostic();
  if (out == nullptr)
    return Fail(kDebugLoc, 0, "no output vector supplied");
  out->clear();
  if (!CheckContext())
    return false;

  if (context_.version >= 5) {
    if (form != DW_FORM_sec_offset && form != DW_FORM_loclistx) {
      return Fail(kDebugLocLists, value,
                  "unsupported location attribute form " +
                      std::to_string(static_cast<unsigned>(form)));
    }
    uint64_t offset = value;
    if (form == DW_FORM_loclistx) {
      SectionMap::const_iterator lists =
          GetSectionByName(sections_, kDebugLocLists);
      if (lists == sections_.end()) {
        return Fail(kDebugLocLists, 0,
                    "DW_FORM_loclistx requires a .debug_loclists section");
      }
      const uint64_t index_offset =
          context_.loclists_base + value * context_.offset_size;
      if (index_offset > lists->second.second ||
          context_.offset_size > lists->second.second - index_offset) {
        return Fail(kDebugLocLists, index_offset,
                    "loclistx index " + std::to_string(value) +
                        " points outside .debug_loclists");
      }
      offset = context_.loclists_base +
               reader_.ReadOffset(lists->second.first + index_offset);
      if (offset >= lists->second.second) {
        return Fail(kDebugLocLists, offset,
                    "loclistx offset points outside .debug_loclists");
      }
    }
    return ReadDebugLocLists(offset, out);
  }

  if (form != DW_FORM_sec_offset) {
    return Fail(kDebugLoc, value,
                "unsupported location attribute form " +
                    std::to_string(static_cast<unsigned>(form)));
  }
  return ReadDebugLoc(value, out);
}

bool LocationListReader::ReadDebugLoc(uint64_t offset,
                                      std::vector<LocationEntry>* out) {
  SectionMap::const_iterator loc = GetSectionByName(sections_, kDebugLoc);
  if (loc == sections_.end()) {
    return Fail(kDebugLoc, offset,
                "location list requires a .debug_loc section");
  }
  const uint8_t* buffer = loc->second.first;
  const uint64_t size = loc->second.second;
  const uint64_t max_address = MaxAddressForSize(context_.address_size);
  const uint64_t pair_size = context_.address_size * 2;
  uint64_t base = context_.low_pc;

  while (true) {
    if (offset > size || pair_size > size - offset) {
      return Fail(kDebugLoc, offset, "location list runs past .debug_loc");
    }
    bool ok = false;
    const uint64_t begin_raw = ReadAddressAt(buffer, size, offset, &ok);
    if (!ok)
      return Fail(kDebugLoc, offset, "truncated .debug_loc begin address");
    const uint64_t end_raw =
        ReadAddressAt(buffer, size, offset + context_.address_size, &ok);
    if (!ok)
      return Fail(kDebugLoc, offset, "truncated .debug_loc end address");
    const uint64_t entry_offset = offset;
    offset += pair_size;

    if (begin_raw == max_address) {
      // Base address selection entry. It changes the base for all following
      // entries and is not itself a location entry.
      base = end_raw;
      continue;
    }
    if (begin_raw == 0 && end_raw == 0) {
      // End of list.
      return true;
    }

    if (offset > size || 2 > size - offset) {
      return Fail(kDebugLoc, entry_offset,
                  "truncated .debug_loc expression length");
    }
    const uint64_t length = reader_.ReadTwoBytes(buffer + offset);
    offset += 2;
    if (offset > size || length > size - offset) {
      return Fail(kDebugLoc, offset, "location expression runs past .debug_loc");
    }

    LocationEntry entry;
    entry.begin = begin_raw + base;
    entry.end = end_raw + base;
    entry.is_default = false;
    entry.entry_offset = entry_offset;
    entry.expression_offset = offset;
    entry.section = kDebugLoc;
    entry.expression.assign(buffer + offset, buffer + offset + length);
    offset += length;
    out->push_back(std::move(entry));
  }
}

bool LocationListReader::ReadDebugLocLists(uint64_t offset,
                                           std::vector<LocationEntry>* out) {
  SectionMap::const_iterator lists =
      GetSectionByName(sections_, kDebugLocLists);
  if (lists == sections_.end()) {
    return Fail(kDebugLocLists, offset,
                "location list requires a .debug_loclists section");
  }
  const uint8_t* buffer = lists->second.first;
  const uint64_t size = lists->second.second;
  uint64_t base = context_.low_pc;

  while (true) {
    if (offset >= size) {
      return Fail(kDebugLocLists, offset,
                  "location list runs past .debug_loclists");
    }
    const uint64_t entry_offset = offset;
    const uint8_t kind = reader_.ReadOneByte(buffer + offset);
    ++offset;

    LocationEntry entry;
    entry.entry_offset = entry_offset;
    entry.is_default = false;
    entry.section = kDebugLocLists;
    uint64_t operand = 0;
    uint64_t operand2 = 0;
    uint64_t leb_len = 0;

    switch (kind) {
      case DW_LLE_end_of_list:
        return true;
      case DW_LLE_base_addressx:
        if (!ReadULeb128(buffer, size, offset, &reader_, &operand, &leb_len)) {
          return Fail(kDebugLocLists, offset,
                      "truncated DW_LLE_base_addressx index");
        }
        offset += leb_len;
        if (!ReadAddressIndex(operand, &base))
          return false;
        continue;
      case DW_LLE_base_address: {
        bool ok = false;
        const uint64_t address = ReadAddressAt(buffer, size, offset, &ok);
        if (!ok) {
          return Fail(kDebugLocLists, offset,
                      "truncated DW_LLE_base_address");
        }
        offset += context_.address_size;
        base = address;
        continue;
      }
      case DW_LLE_startx_endx:
      case DW_LLE_startx_length:
        if (!ReadULeb128(buffer, size, offset, &reader_, &operand, &leb_len)) {
          return Fail(kDebugLocLists, offset,
                      "truncated DW_LLE_startx index");
        }
        offset += leb_len;
        if (!ReadULeb128(buffer, size, offset, &reader_, &operand2, &leb_len)) {
          return Fail(kDebugLocLists, offset,
                      "truncated DW_LLE_startx second operand");
        }
        offset += leb_len;
        if (kind == DW_LLE_startx_endx) {
          uint64_t start = 0, end = 0;
          if (!ReadAddressIndex(operand, &start) ||
              !ReadAddressIndex(operand2, &end)) {
            return false;
          }
          entry.begin = start;
          entry.end = end;
        } else {
          uint64_t start = 0;
          if (!ReadAddressIndex(operand, &start))
            return false;
          entry.begin = start;
          entry.end = start + operand2;
          if (entry.end < entry.begin)
            entry.end = std::numeric_limits<uint64_t>::max();
        }
        break;
      case DW_LLE_offset_pair:
        if (!ReadULeb128(buffer, size, offset, &reader_, &operand, &leb_len)) {
          return Fail(kDebugLocLists, offset,
                      "truncated DW_LLE_offset_pair begin");
        }
        offset += leb_len;
        if (!ReadULeb128(buffer, size, offset, &reader_, &operand2, &leb_len)) {
          return Fail(kDebugLocLists, offset,
                      "truncated DW_LLE_offset_pair end");
        }
        offset += leb_len;
        entry.begin = base + operand;
        entry.end = base + operand2;
        break;
      case DW_LLE_default_location:
        entry.is_default = true;
        break;
      case DW_LLE_start_end: {
        bool ok = false;
        entry.begin = ReadAddressAt(buffer, size, offset, &ok);
        if (!ok) {
          return Fail(kDebugLocLists, offset,
                      "truncated DW_LLE_start_end begin");
        }
        offset += context_.address_size;
        entry.end = ReadAddressAt(buffer, size, offset, &ok);
        if (!ok) {
          return Fail(kDebugLocLists, offset,
                      "truncated DW_LLE_start_end end");
        }
        offset += context_.address_size;
        break;
      }
      case DW_LLE_start_length: {
        bool ok = false;
        entry.begin = ReadAddressAt(buffer, size, offset, &ok);
        if (!ok) {
          return Fail(kDebugLocLists, offset,
                      "truncated DW_LLE_start_length address");
        }
        offset += context_.address_size;
        if (!ReadULeb128(buffer, size, offset, &reader_, &operand, &leb_len)) {
          return Fail(kDebugLocLists, offset,
                      "truncated DW_LLE_start_length length");
        }
        offset += leb_len;
        entry.end = entry.begin + operand;
        if (entry.end < entry.begin)
          entry.end = std::numeric_limits<uint64_t>::max();
        break;
      }
      default:
        return Fail(kDebugLocLists, entry_offset,
                    "unsupported .debug_loclists entry type " +
                        std::to_string(static_cast<unsigned>(kind)));
    }

    if (!ReadULeb128(buffer, size, offset, &reader_, &operand, &leb_len)) {
      return Fail(kDebugLocLists, offset,
                  "truncated .debug_loclists expression length");
    }
    offset += leb_len;
    if (operand > size - offset) {
      return Fail(kDebugLocLists, offset,
                  "location expression runs past .debug_loclists");
    }
    entry.expression_offset = offset;
    entry.expression.assign(buffer + offset, buffer + offset + operand);
    offset += operand;
    out->push_back(std::move(entry));
  }
}

bool LocationListReader::SelectLocation(
    const std::vector<LocationEntry>& entries, uint64_t pc,
    std::vector<uint8_t>* expression) {
  return SelectLocation(entries, pc, expression, nullptr);
}

bool LocationListReader::SelectLocation(
    const std::vector<LocationEntry>& entries, uint64_t pc,
    std::vector<uint8_t>* expression, const LocationEntry** selected_entry) {
  if (expression == nullptr)
    return false;
  for (const LocationEntry& entry : entries) {
    if (!entry.is_default && entry.begin != entry.end && entry.begin <= pc &&
        pc < entry.end) {
      *expression = entry.expression;
      if (selected_entry)
        *selected_entry = &entry;
      return true;
    }
  }
  for (const LocationEntry& entry : entries) {
    if (entry.is_default) {
      *expression = entry.expression;
      if (selected_entry)
        *selected_entry = &entry;
      return true;
    }
  }
  return false;
}

}  // namespace google_breakpad
