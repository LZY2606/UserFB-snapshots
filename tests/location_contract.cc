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

// Location contract tests: legacy (.debug_loc), indexed (.debug_loclists),
// default entries, compilation unit context snapshots, and the real samples.

#include <stdint.h>

#include <cstring>
#include <string>
#include <vector>

#include "common/dwarf/bytereader.h"
#include "common/dwarf/dwarf2reader.h"
#include "common/dwarf/elf_reader.h"
#include "common/dwarf/location_list_reader.h"
#include "common/dwarf/variable_location.h"
#include "contract_api.h"

using google_breakpad::ByteReader;
using google_breakpad::CompilationUnit;
using google_breakpad::Dwarf2Handler;
using google_breakpad::DwarfForm;
using google_breakpad::ElfReader;
using google_breakpad::ENDIANNESS_LITTLE;
using google_breakpad::LocationEntry;
using google_breakpad::LocationListContext;
using google_breakpad::LocationListReader;
using google_breakpad::SectionMap;
using google_breakpad::VariableDiagnostic;
using google_breakpad::VariableLocationAnalyzer;
using google_breakpad::VariableLocationResult;
using google_breakpad::VariableLocationStatus;

namespace {

void Put8(std::vector<uint8_t>* bytes, uint64_t value) {
  for (int i = 0; i < 8; ++i)
    bytes->push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void Put2(std::vector<uint8_t>* bytes, uint16_t value) {
  bytes->push_back(static_cast<uint8_t>(value));
  bytes->push_back(static_cast<uint8_t>(value >> 8));
}

void Put4Little(std::vector<uint8_t>* bytes, uint32_t value) {
  for (int i = 0; i < 4; ++i)
    bytes->push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void PutULEB(std::vector<uint8_t>* bytes, uint64_t value) {
  do {
    uint8_t byte = value & 0x7f;
    value >>= 7;
    if (value)
      byte |= 0x80;
    bytes->push_back(byte);
  } while (value);
}

void PutSLEB(std::vector<uint8_t>* bytes, int64_t value) {
  bool more = true;
  while (more) {
    uint8_t byte = value & 0x7f;
    value >>= 7;
    if ((value == 0 && (byte & 0x40) == 0) ||
        (value == -1 && (byte & 0x40) != 0)) {
      more = false;
    } else {
      byte |= 0x80;
    }
    bytes->push_back(byte);
  }
}

bool SameBytes(const std::vector<uint8_t>& got,
               const std::vector<uint8_t>& expected) {
  return got == expected;
}

void AppendBytes(std::vector<uint8_t>* out, const char* hex) {
  for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
    auto nibble = [](char c) -> unsigned {
      if (c >= '0' && c <= '9')
        return c - '0';
      return c - 'a' + 10;
    };
    out->push_back(static_cast<uint8_t>((nibble(hex[i]) << 4) |
                                        nibble(hex[i + 1])));
  }
}

// Reads a whole ELF into a SectionMap of .debug_* sections. The returned
// pointers live as long as ELF.
SectionMap MakeSectionMap(const ElfReader& elf) {
  SectionMap sections;
  for (uint64_t i = 1; i < const_cast<ElfReader&>(elf).GetNumSections(); ++i) {
    const char* name =
        const_cast<ElfReader&>(elf).GetSectionName(static_cast<int>(i));
    if (name == nullptr || std::strncmp(name, ".debug_", 7) != 0)
      continue;
    size_t size = 0;
    const char* data =
        const_cast<ElfReader&>(elf).GetSectionByIndex(static_cast<int>(i),
                                                      &size);
    if (data != nullptr)
      sections[name] = std::make_pair(
          reinterpret_cast<const uint8_t*>(data), size);
  }
  return sections;
}

// Queries the analyzer and returns the candidate with the given variable DIE.
const VariableLocationResult& ResultForDie(
    VariableLocationAnalyzer* analyzer, const std::string& function,
    const std::string& variable, uint64_t pc, uint64_t variable_die,
    std::vector<VariableLocationResult>* storage) {
  VariableDiagnostic diagnostic;
  bool ok = analyzer->Query(function, variable, pc, storage, &diagnostic);
  location_expect(ok, "sample query unexpectedly failed");
  for (const auto& result : *storage) {
    if (result.variable_die == variable_die)
      return result;
  }
  location_expect(false, "expected candidate DIE was not returned");
  return storage->front();
}

// Queries the analyzer and returns the single candidate.
const VariableLocationResult& SingleResult(
    VariableLocationAnalyzer* analyzer, const std::string& function,
    const std::string& variable, uint64_t pc,
    std::vector<VariableLocationResult>* storage) {
  VariableDiagnostic diagnostic;
  bool ok = analyzer->Query(function, variable, pc, storage, &diagnostic);
  location_expect(ok, "sample query unexpectedly failed");
  location_expect(storage->size() == 1, "expected one variable candidate");
  return storage->front();
}

bool HasScope(const VariableLocationResult& result, uint64_t die,
              uint64_t begin, uint64_t end) {
  for (const auto& scope : result.scopes) {
    if (scope.die_offset == die && scope.begin == begin && scope.end == end)
      return true;
  }
  return false;
}

// -----------------------------------------------------------------------
// legacy: DWARF 4 .debug_loc with base selection.
// -----------------------------------------------------------------------
void LegacyCase() {
  std::vector<uint8_t> loc;
  // Entries are emitted first, then offsets recorded.
  // Pair 1: 0x10..0x20 under the base address 0x1000 -> 0x1010..0x1020.
  const uint64_t entry1 = loc.size();
  Put8(&loc, 0x10);
  Put8(&loc, 0x20);
  Put2(&loc, 1);
  loc.push_back(0x55);
  const uint64_t expr1 = entry1 + 18;
  // Base selection: set base to 0x2000.
  const uint64_t base_entry = loc.size();
  Put8(&loc, ~static_cast<uint64_t>(0));
  Put8(&loc, 0x2000);
  // Pair 2: 0x5..0x7 -> 0x2005..0x2007 with two bytes.
  const uint64_t entry2 = loc.size();
  Put8(&loc, 0x5);
  Put8(&loc, 0x7);
  Put2(&loc, 4);
  std::vector<uint8_t> expr2;
  AppendBytes(&expr2, "f301559f");
  loc.insert(loc.end(), expr2.begin(), expr2.end());
  const uint64_t expr2_offset = entry2 + 18;
  // Terminator.
  Put8(&loc, 0);
  Put8(&loc, 0);

  SectionMap sections;
  sections[".debug_loc"] = std::make_pair(loc.data(), loc.size());
  LocationListContext context;
  context.version = 4;
  context.address_size = 8;
  context.offset_size = 4;
  context.low_pc = 0x1000;

  LocationListReader reader(sections, context);
  std::vector<LocationEntry> entries;
  location_expect(reader.ReadLocations(google_breakpad::DW_FORM_sec_offset, 0,
                                       &entries),
                  "legacy list should parse");
  location_expect(entries.size() == 2, "legacy list should have two entries");
  location_expect(entries[0].begin == 0x1010 && entries[0].end == 0x1020,
                  "legacy base address must apply to the first pair");
  location_expect(entries[0].entry_offset == entry1,
                  "legacy entry offset must point at the address pair");
  location_expect(entries[0].expression_offset == expr1,
                  "legacy expression offset must point at the opcode");
  location_expect(entries[0].section == ".debug_loc",
                  "legacy section provenance");
  location_expect(entries[0].expression.size() == 1 &&
                      entries[0].expression[0] == 0x55,
                  "legacy first expression bytes");
  location_expect(entries[1].begin == 0x2005 && entries[1].end == 0x2007,
                  "base selection must change later pairs");
  location_expect(entries[1].entry_offset == entry2,
                  "second legacy entry offset");
  location_expect(entries[1].expression_offset == expr2_offset,
                  "second legacy expression offset");
  location_expect(SameBytes(entries[1].expression, expr2),
                  "second legacy expression bytes");
  location_expect(base_entry < entry2, "base selection precedes pair two");

  std::vector<uint8_t> selected;
  location_expect(LocationListReader::SelectLocation(entries, 0x1010,
                                                     &selected),
                  "legacy selection at range start");
  location_expect(selected.size() == 1 && selected[0] == 0x55,
                  "legacy selection bytes");
  location_expect(!LocationListReader::SelectLocation(entries, 0x1020,
                                                      &selected),
                  "legacy half-open end must not match");
  location_expect(LocationListReader::SelectLocation(entries, 0x2006,
                                                     &selected) &&
                      SameBytes(selected, expr2),
                  "legacy selection after base change");
  location_expect(!LocationListReader::SelectLocation(entries, 0x1000,
                                                      &selected),
                  "legacy hole must not match");

  // A missing section is an error with a diagnostic, not an empty success.
  SectionMap empty;
  LocationListReader missing(empty, context);
  std::vector<LocationEntry> none;
  location_expect(!missing.ReadLocations(google_breakpad::DW_FORM_sec_offset,
                                         0, &none),
                  "missing .debug_loc must fail");
  location_expect(missing.diagnostic().section == ".debug_loc" &&
                      !missing.diagnostic().message.empty(),
                  "missing section diagnostic");
}

// -----------------------------------------------------------------------
// indexed: DWARF 5 .debug_loclists with an offset table and .debug_addr.
// -----------------------------------------------------------------------
void IndexedCase() {
  std::vector<uint8_t> loclists;
  // Unit header: length, version, address size, segment selector,
  // offset-entry count. The offset table starts at loclists_base.
  std::vector<uint8_t> body;
  // Two addresses in .debug_addr at index 0 and 1.
  std::vector<uint8_t> addr;
  Put8(&addr, 0x3000);
  Put8(&addr, 0x3010);

  // List 0: base_addressx index 0, offset_pair 0..0x10, then default.
  std::vector<uint8_t> list0;
  list0.push_back(google_breakpad::DW_LLE_base_addressx);
  PutULEB(&list0, 0);
  list0.push_back(google_breakpad::DW_LLE_offset_pair);
  PutULEB(&list0, 0x0);
  PutULEB(&list0, 0x10);
  PutULEB(&list0, 1);
  list0.push_back(0x56);
  list0.push_back(google_breakpad::DW_LLE_default_location);
  PutULEB(&list0, 4);
  AppendBytes(&list0, "f301559f");
  list0.push_back(google_breakpad::DW_LLE_end_of_list);

  // List 1: startx_endx indices 0 and 1, then startx_length, then a hole.
  std::vector<uint8_t> list1;
  list1.push_back(google_breakpad::DW_LLE_startx_endx);
  PutULEB(&list1, 0);
  PutULEB(&list1, 1);
  PutULEB(&list1, 1);
  list1.push_back(0x57);
  list1.push_back(google_breakpad::DW_LLE_startx_length);
  PutULEB(&list1, 1);
  PutULEB(&list1, 0x8);
  PutULEB(&list1, 1);
  list1.push_back(0x50);
  list1.push_back(google_breakpad::DW_LLE_end_of_list);

  // The unit header is 12 bytes; the offset table follows and
  // loclists_base points at the start of that table. Entry offsets are
  // relative to loclists_base.
  const uint64_t header_size = 12;
  const uint64_t table_size = 8;
  const uint64_t list0_abs = header_size + table_size;
  const uint64_t list1_abs = list0_abs + list0.size();
  const uint64_t unit_length = 8 + table_size + list0.size() +
                               list1.size();
  std::vector<uint8_t> header;
  // unit_length: bytes after the length field.
  for (int i = 0; i < 4; ++i)
    header.push_back(static_cast<uint8_t>(unit_length >> (8 * i)));
  Put2(&header, 5);
  header.push_back(8);  // address size
  header.push_back(0);  // segment selector
  for (int i = 0; i < 4; ++i)
    header.push_back(static_cast<uint8_t>(table_size >> (8 * i)));
  for (int i = 0; i < 4; ++i)
    header.push_back(static_cast<uint8_t>((list0_abs - header_size) >>
                                          (8 * i)));
  for (int i = 0; i < 4; ++i)
    header.push_back(static_cast<uint8_t>((list1_abs - header_size) >>
                                          (8 * i)));
  loclists = header;
  loclists.insert(loclists.end(), list0.begin(), list0.end());
  loclists.insert(loclists.end(), list1.begin(), list1.end());

  SectionMap sections;
  sections[".debug_loclists"] = std::make_pair(loclists.data(),
                                               loclists.size());
  sections[".debug_addr"] = std::make_pair(addr.data(), addr.size());

  LocationListContext context;
  context.version = 5;
  context.address_size = 8;
  context.offset_size = 4;
  context.low_pc = 0x3000;
  // loclists_base points at the offset table; entries are addressed
  // relative to it.
  context.loclists_base = header_size;
  context.addr_base = 0;

  LocationListReader reader(sections, context);
  std::vector<LocationEntry> entries;
  location_expect(reader.ReadLocations(google_breakpad::DW_FORM_loclistx, 0,
                                       &entries),
                  "indexed list 0 should parse");
  location_expect(entries.size() == 2, "indexed list 0 entry count");
  location_expect(entries[0].begin == 0x3000 && entries[0].end == 0x3010,
                  "offset_pair uses base_addressx address");
  location_expect(entries[0].entry_offset == list0_abs + 2,
                  "indexed entry offset points at the LLE");
  location_expect(entries[0].section == ".debug_loclists",
                  "indexed section provenance");
  location_expect(entries[1].is_default, "default entry flag");

  std::vector<LocationEntry> entries1;
  location_expect(reader.ReadLocations(google_breakpad::DW_FORM_loclistx, 1,
                                       &entries1),
                  "indexed list 1 should parse");
  location_expect(entries1.size() == 2, "indexed list 1 entry count");
  location_expect(entries1[0].begin == 0x3000 && entries1[0].end == 0x3010,
                  "startx_endx resolves .debug_addr indexes");
  location_expect(entries1[1].begin == 0x3010 && entries1[1].end == 0x3018,
                  "startx_length computes the end from the length");

  // Selection: explicit first, and the default is used only outside ranges.
  std::vector<uint8_t> selected;
  location_expect(LocationListReader::SelectLocation(entries, 0x3008,
                                                     &selected) &&
                      selected.size() == 1 && selected[0] == 0x56,
                  "indexed explicit selection");
  location_expect(LocationListReader::SelectLocation(entries, 0x9999,
                                                     &selected) &&
                      selected.size() == 4,
                  "indexed default fallback");

  // Index outside the offset table must diagnose.
  std::vector<LocationEntry> none;
  location_expect(!reader.ReadLocations(google_breakpad::DW_FORM_loclistx, 7,
                                        &none),
                  "loclistx out of range must fail");
  location_expect(reader.diagnostic().section == ".debug_loclists",
                  "loclistx diagnostic section");

  // A form the analyzer cannot interpret is rejected for each version.
  LocationListReader wrong_form(sections, context);
  location_expect(!wrong_form.ReadLocations(google_breakpad::DW_FORM_data4, 0,
                                            &none),
                  "unsupported DWARF5 location form must fail");
}

// -----------------------------------------------------------------------
// defaults: selection rules for explicit, default, empty and zero-length
// entries. The lists are built directly, so no sections are needed.
// -----------------------------------------------------------------------
void DefaultsCase() {
  auto make = [](uint64_t begin, uint64_t end, bool is_default,
                 const char* hex) {
    LocationEntry entry;
    entry.begin = begin;
    entry.end = end;
    entry.is_default = is_default;
    AppendBytes(&entry.expression, hex);
    return entry;
  };

  std::vector<LocationEntry> entries;
  entries.push_back(make(0x100, 0x110, false, "50"));
  entries.push_back(make(0x110, 0x120, false, "51"));
  entries.push_back(make(0, 0, true, "52"));

  std::vector<uint8_t> selected;
  location_expect(LocationListReader::SelectLocation(entries, 0x100,
                                                     &selected) &&
                      selected.size() == 1 && selected[0] == 0x50,
                  "first explicit entry selected");
  location_expect(LocationListReader::SelectLocation(entries, 0x110,
                                                     &selected) &&
                      selected[0] == 0x51,
                  "second explicit entry selected at its start");
  location_expect(LocationListReader::SelectLocation(entries, 0x120,
                                                     &selected) &&
                      selected[0] == 0x52,
                  "default used when no explicit entry matches");
  location_expect(!LocationListReader::SelectLocation(entries, 0x120,
                                                      nullptr),
                  "null expression output is rejected");

  // Zero-length explicit entries never match, and entries are not merged
  // across a hole.
  std::vector<LocationEntry> zero;
  zero.push_back(make(0x200, 0x200, false, "53"));
  zero.push_back(make(0x201, 0x210, false, "54"));
  location_expect(!LocationListReader::SelectLocation(zero, 0x200, &selected),
                  "zero-length range must not match");

  // An explicit empty expression is a match that means unavailable; it must
  // not fall back to the default entry.
  std::vector<LocationEntry> empty_hit;
  empty_hit.push_back(make(0x300, 0x310, false, ""));
  empty_hit.push_back(make(0, 0, true, "55"));
  const LocationEntry* chosen = nullptr;
  location_expect(LocationListReader::SelectLocation(empty_hit, 0x305,
                                                     &selected, &chosen),
                  "explicit empty entry is still a match");
  location_expect(selected.empty(), "explicit empty expression stays empty");
  location_expect(chosen != nullptr && !chosen->is_default,
                  "explicit empty entry is not replaced by the default");

  // Without a matching explicit entry the default is selected even if the
  // default expression is empty.
  std::vector<LocationEntry> default_only;
  default_only.push_back(make(0, 0, true, ""));
  location_expect(LocationListReader::SelectLocation(default_only, 0x400,
                                                     &selected),
                  "default-only selection");
  location_expect(selected.empty(), "default-only empty expression");

  // Expression interpretation: only the documented single-opcode shapes are
  // given a structured description; everything else is opaque bytes.
  std::string description;
  VariableDiagnostic expression_diagnostic;
  auto describe = [&](const char* hex, bool has_frame,
                      const char* frame_hex) {
    std::vector<uint8_t> expression;
    AppendBytes(&expression, hex);
    std::vector<uint8_t> frame;
    if (frame_hex != nullptr)
      AppendBytes(&frame, frame_hex);
    description.clear();
    expression_diagnostic = VariableDiagnostic();
    return google_breakpad::DescribeLocationExpression(
        expression, has_frame, frame, &description, &expression_diagnostic);
  };

  location_expect(describe("5f", false, nullptr) && description == "reg:15",
                  "single reg opcode");
  location_expect(describe("7670", false, nullptr) &&
                      description == "breg:6:-16",
                  "breg with a complete negative SLEB128");
  location_expect(describe("7600", false, nullptr) && description == "breg:6:0",
                  "breg with a positive zero offset");
  location_expect(describe("7608", false, nullptr) && description == "breg:6:8",
                  "breg with a positive offset");
  location_expect(describe("76307f", false, nullptr) &&
                      description == "opaque",
                  "reg opcode followed by more bytes is opaque");
  location_expect(describe("76", false, nullptr) && description == "opaque",
                  "truncated breg operand is opaque, not misattributed");
  location_expect(describe("9174", true, "56") &&
                      description == "fbreg:-12:frame=56",
                  "fbreg with the owning frame base");
  location_expect(!describe("9174", false, nullptr) &&
                      !expression_diagnostic.empty(),
                  "fbreg without a frame base must diagnose");
  location_expect(describe("f301559f", false, nullptr) &&
                      description == "opaque",
                  "entry value expression is opaque");
  location_expect(describe("9603", false, nullptr) &&
                      description == "opaque",
                  "arithmetic expression is opaque");
}

// -----------------------------------------------------------------------
// cu_context: read the two real compilation units and verify the value
// snapshot, root-only bases, and ByteReader reuse.
// -----------------------------------------------------------------------
class LowPcProbe : public Dwarf2Handler {
 public:
  bool StartCompilationUnit(uint64_t offset, uint8_t, uint8_t, uint64_t,
                            uint8_t) override {
    offsets.push_back(offset);
    return true;
  }
  bool NeedSplitDebugInfo() override { return false; }
  bool StartDIE(uint64_t, google_breakpad::DwarfTag) override { return true; }
  void ProcessAttributeUnsigned(uint64_t offset,
                                google_breakpad::DwarfAttribute attr,
                                DwarfForm, uint64_t data) override {
    if (attr == google_breakpad::DW_AT_low_pc)
      lows.push_back(std::make_pair(offset, data));
  }

  std::vector<uint64_t> offsets;
  std::vector<std::pair<uint64_t, uint64_t>> lows;
};

void CuContextCase() {
  ElfReader elf("samples/optimized4.elf");
  location_expect(elf.IsElf64File(), "sample 4 must be ELF64");
  SectionMap sections = MakeSectionMap(elf);
  ByteReader reader(ENDIANNESS_LITTLE);

  // First compilation unit.
  LowPcProbe first_probe;
  CompilationUnit first("samples/optimized4.elf", sections, 0, &reader,
                        &first_probe);
  const uint64_t first_length = first.Start();
  location_expect(first_length != 0, "first CU must parse");
  LocationListContext first_context = first.GetLocationListContext();
  location_expect(first_context.version == 4, "first CU version snapshot");
  location_expect(first_context.address_size == 8,
                  "first CU address size snapshot");
  location_expect(first_context.offset_size == 4,
                  "first CU offset size snapshot");
  location_expect(first_context.low_pc == 0x401000,
                  "first CU root low_pc snapshot");
  location_expect(first_context.loclists_base == 0,
                  "DWARF4 has no loclists base");
  location_expect(first_context.addr_base == 0, "DWARF4 has no addr base");

  // The ByteReader is reused for the second compilation unit. This must not
  // change the snapshot we already captured.
  LowPcProbe second_probe;
  CompilationUnit second_unit("samples/optimized4.elf", sections, first_length,
                              &reader, &second_probe);
  location_expect(second_unit.Start() != 0, "second CU must parse");
  LocationListContext second_context = second_unit.GetLocationListContext();
  location_expect(second_context.low_pc == 0x401050,
                  "second CU root low_pc snapshot");
  location_expect(second_probe.offsets.size() == 1 &&
                      second_probe.offsets[0] == first_length,
                  "second CU offset");
  location_expect(first_context.low_pc == 0x401000,
                  "reusing the ByteReader must not rewrite the old snapshot");
  location_expect(first_context.version == 4 && first_context.offset_size == 4,
                  "old snapshot header values stay unchanged");

  // The DWARF5 sample exposes root addr_base and loclists_base values, and
  // they differ per CU.
  ElfReader elf5("samples/optimized5.elf");
  SectionMap sections5 = MakeSectionMap(elf5);
  ByteReader reader5(ENDIANNESS_LITTLE);
  LowPcProbe probe5a;
  CompilationUnit unit5a("samples/optimized5.elf", sections5, 0, &reader5,
                         &probe5a);
  const uint64_t length5a = unit5a.Start();
  location_expect(length5a > 0, "first DWARF5 CU must parse");
  LocationListContext context5a = unit5a.GetLocationListContext();
  location_expect(context5a.version == 5, "DWARF5 version snapshot");
  location_expect(context5a.addr_base == 8, "first DWARF5 addr base");
  location_expect(context5a.loclists_base == 12,
                  "first DWARF5 loclists base");
  LowPcProbe probe5b;
  CompilationUnit unit5b("samples/optimized5.elf", sections5, length5a,
                          &reader5, &probe5b);
  location_expect(unit5b.Start() != 0, "second DWARF5 CU must parse");
  LocationListContext context5b = unit5b.GetLocationListContext();
  location_expect(context5b.addr_base == 48, "second DWARF5 addr base");
  location_expect(context5b.loclists_base == 74,
                  "second DWARF5 loclists base");
  location_expect(context5a.addr_base == 8 &&
                      context5a.loclists_base == 12,
                  "reused ByteReader leaves the first DWARF5 snapshot intact");

  // Values captured from the root DIE must not be overwritten by a child
  // DIE. Build one synthetic CU whose subprogram carries a different low_pc.
  std::vector<uint8_t> abbrev;
  // Abbrev 1: DW_TAG_compile_unit, children yes, DW_AT_low_pc/addr,
  // DW_AT_high_pc/data4.
  PutULEB(&abbrev, 1);
  PutULEB(&abbrev, google_breakpad::DW_TAG_compile_unit);
  abbrev.push_back(static_cast<uint8_t>(google_breakpad::DW_children_yes));
  PutULEB(&abbrev, google_breakpad::DW_AT_low_pc);
  PutULEB(&abbrev, google_breakpad::DW_FORM_addr);
  PutULEB(&abbrev, google_breakpad::DW_AT_high_pc);
  PutULEB(&abbrev, google_breakpad::DW_FORM_data4);
  PutULEB(&abbrev, 0);
  PutULEB(&abbrev, 0);
  // Abbrev 2: DW_TAG_subprogram, no children, same low_pc/high_pc pair.
  PutULEB(&abbrev, 2);
  PutULEB(&abbrev, google_breakpad::DW_TAG_subprogram);
  abbrev.push_back(static_cast<uint8_t>(google_breakpad::DW_children_no));
  PutULEB(&abbrev, google_breakpad::DW_AT_low_pc);
  PutULEB(&abbrev, google_breakpad::DW_FORM_addr);
  PutULEB(&abbrev, google_breakpad::DW_AT_high_pc);
  PutULEB(&abbrev, google_breakpad::DW_FORM_data4);
  PutULEB(&abbrev, 0);
  PutULEB(&abbrev, 0);
  abbrev.push_back(0);

  std::vector<uint8_t> info;
  std::vector<uint8_t> body;
  Put2(&body, 4);  // version
  Put4Little(&body, 0);  // abbrev offset
  body.push_back(8);  // address size
  PutULEB(&body, 1);  // root abbrev
  Put8(&body, 0x1000);  // root low_pc
  Put4Little(&body, 0x100);  // root high_pc length
  PutULEB(&body, 2);  // child abbrev
  Put8(&body, 0x2000);  // child low_pc, different from the root
  Put4Little(&body, 0x10);
  PutULEB(&body, 0);  // end of root children
  Put4Little(&info, static_cast<uint32_t>(body.size()));
  info.insert(info.end(), body.begin(), body.end());
  body.clear();

  SectionMap synthetic;
  synthetic[".debug_abbrev"] = std::make_pair(abbrev.data(), abbrev.size());
  synthetic[".debug_info"] = std::make_pair(info.data(), info.size());
  ByteReader synthetic_reader(ENDIANNESS_LITTLE);
  LowPcProbe synthetic_probe;
  CompilationUnit synthetic_unit("", synthetic, 0, &synthetic_reader,
                                  &synthetic_probe);
  location_expect(synthetic_unit.Start() != 0, "synthetic CU must parse");
  LocationListContext synthetic_context =
      synthetic_unit.GetLocationListContext();
  location_expect(synthetic_context.low_pc == 0x1000,
                  "a child low_pc must not overwrite the root snapshot");
  location_expect(synthetic_probe.lows.size() == 2,
                  "synthetic CU must report both low_pc attributes");
}

// -----------------------------------------------------------------------
// samples: read both real samples through the analysis interface and check
// real ranges, availability, bytes and provenance.
// -----------------------------------------------------------------------
void SamplesCase() {
  VariableLocationAnalyzer analyzer;
  VariableDiagnostic diagnostic;
  location_expect(analyzer.Open("samples/optimized4.elf", &diagnostic),
                  "open optimized4.elf");

  std::vector<VariableLocationResult> storage;

  // alpha: moving is in rbp-16 early, a hole, then rbp-16 again.
  const VariableLocationResult& early = ResultForDie(
      &analyzer, "recover_alpha", "moving", 0x401006, 0x52, &storage);
  location_expect(early.status == VariableLocationStatus::kDescribed,
                  "alpha moving 0x401006 described");
  location_expect(early.description == "breg:6:-16",
                  "alpha moving breg description");
  location_expect(early.cu_offset == 0, "alpha CU offset");
  location_expect(early.function_die == 0x2a, "alpha function DIE");
  location_expect(early.variable_die == 0x52, "alpha moving DIE");
  location_expect(early.has_entry && early.entry_section == ".debug_loc" &&
                      early.entry_offset == 0x39,
                  "alpha moving entry provenance");
  location_expect(early.has_expression &&
                      early.expression_section == ".debug_loc" &&
                      early.expression_offset == 0x4b,
                  "alpha moving expression provenance");
  std::vector<uint8_t> expected;
  AppendBytes(&expected, "7670");
  location_expect(SameBytes(early.expression_bytes, expected),
                  "alpha moving expression bytes");
  location_expect(HasScope(early, 0x2a, 0x401000, 0x401042),
                  "alpha moving function scope");
  // STORAGE is reused by later queries, so remember the identity now.
  const uint64_t early_die = early.variable_die;
  const uint64_t early_function_die = early.function_die;

  const VariableLocationResult& hole = ResultForDie(
      &analyzer, "recover_alpha", "moving", 0x401012, 0x52, &storage);
  location_expect(hole.status == VariableLocationStatus::kUnavailable,
                  "alpha moving hole is unavailable");
  location_expect(!hole.has_entry && !hole.has_expression,
                  "a hole must not select a list entry");
  location_expect(hole.has_location_attribute,
                  "a hole still keeps the attribute");

  const VariableLocationResult& after = ResultForDie(
      &analyzer, "recover_alpha", "moving", 0x401042, 0x52, &storage);
  location_expect(after.status == VariableLocationStatus::kOutOfScope,
                  "pc past the function is out of scope");
  (void)early_function_die;

  // The lexical block has a second, distinct moving variable at die 0x7d.
  const VariableLocationResult& block = ResultForDie(
      &analyzer, "recover_alpha", "moving", 0x40101b, 0x7d, &storage);
  location_expect(block.variable_die == 0x7d, "block moving DIE identity");
  location_expect(block.scopes.size() == 2 &&
                      HasScope(block, 0x70, 0x40101b, 0x40102d),
                  "block scope range");
  location_expect(block.status == VariableLocationStatus::kDescribed,
                  "block moving described");
  location_expect(block.description == "fbreg:-12:frame=56",
                  "block moving fbreg description with frame base");
  location_expect(!block.has_entry, "inline location has no list entry");
  location_expect(block.has_expression &&
                      block.expression_section == ".debug_info" &&
                      block.expression_offset == 0x7f,
                  "inline expression provenance is .debug_info");
  expected.clear();
  AppendBytes(&expected, "9174");
  location_expect(SameBytes(block.expression_bytes, expected),
                  "inline expression bytes");

  const VariableLocationResult& outside_block = ResultForDie(
      &analyzer, "recover_alpha", "moving", 0x40102d, 0x7d, &storage);
  location_expect(outside_block.variable_die == 0x7d &&
                      outside_block.status ==
                          VariableLocationStatus::kOutOfScope,
                  "block variable is out of scope past the block end");

  // stable is a register at 0x401012.
  const VariableLocationResult& stable = SingleResult(
      &analyzer, "recover_alpha", "stable", 0x401012, &storage);
  location_expect(stable.status == VariableLocationStatus::kDescribed &&
                      stable.description == "reg:3",
                  "stable is described as reg:3");
  location_expect(stable.expression_offset == 0x96 &&
                      stable.entry_offset == 0x84,
                  "stable provenance");
  expected.clear();
  AppendBytes(&expected, "53");
  location_expect(SameBytes(stable.expression_bytes, expected),
                  "stable expression bytes");

  // opaqueness: input at 0x40101e uses an entry-value expression.
  const VariableLocationResult& opaque = SingleResult(
      &analyzer, "recover_alpha", "input", 0x40101e, &storage);
  location_expect(opaque.status == VariableLocationStatus::kDescribed &&
                      opaque.description == "opaque",
                  "entry value expression is opaque but described");
  expected.clear();
  AppendBytes(&expected, "f301559f");
  location_expect(SameBytes(opaque.expression_bytes, expected),
                  "entry value bytes");

  // Same variable name in two functions: distinct identities and values.
  const VariableLocationResult& beta = SingleResult(
      &analyzer, "recover_beta", "moving", 0x401058, &storage);
  location_expect(beta.variable_die != early_die,
                  "same name in another function keeps its own DIE");
  location_expect(beta.function_die == 0xf9 && beta.cu_offset == 0xcf,
                  "beta function identity");
  location_expect(beta.description == "breg:6:-4",
                  "beta moving breg description");
  location_expect(beta.entry_offset == 0xe0 && beta.expression_offset == 0xf2,
                  "beta moving provenance");
  location_expect(beta.status == VariableLocationStatus::kDescribed,
                  "beta moving described");

  // A missing variable yields exactly one not_found result with no identity.
  std::vector<VariableLocationResult> missing;
  location_expect(analyzer.Query("recover_alpha", "missing", 0x401009,
                                 &missing, &diagnostic),
                  "missing variable query succeeds");
  location_expect(missing.size() == 1 &&
                      missing[0].status == VariableLocationStatus::kNotFound,
                  "missing variable is not_found");
  location_expect(missing[0].variable_die == 0 && missing[0].cu_offset == 0,
                  "not_found must not invent identity");

  // A missing function is also not_found.
  location_expect(analyzer.Query("missing", "moving", 0x401009, &missing,
                                 &diagnostic) &&
                      missing.size() == 1 &&
                      missing[0].status == VariableLocationStatus::kNotFound,
                  "missing function is not_found");

  // The DWARF5 sample gives the same variable facts through loclists.
  VariableLocationAnalyzer analyzer5;
  location_expect(analyzer5.Open("samples/optimized5.elf", &diagnostic),
                  "open optimized5.elf");
  std::vector<VariableLocationResult> storage5;
  const VariableLocationResult& early5 = ResultForDie(
      &analyzer5, "recover_alpha", "moving", 0x501006, 0x3f, &storage5);
  location_expect(early5.status == VariableLocationStatus::kDescribed &&
                      early5.description == "breg:6:-16",
                  "DWARF5 moving description");
  location_expect(early5.variable_die == 0x3f, "DWARF5 moving DIE");
  location_expect(early5.has_entry &&
                      early5.entry_section == ".debug_loclists" &&
                      early5.entry_offset == 0x26,
                  "DWARF5 entry provenance");
  location_expect(early5.expression_offset == 0x2a,
                  "DWARF5 expression provenance");
  location_expect(early5.location_form == google_breakpad::DW_FORM_loclistx &&
                      early5.location_value == 1,
                  "DWARF5 attribute uses loclistx");
  expected.clear();
  AppendBytes(&expected, "7670");
  location_expect(SameBytes(early5.expression_bytes, expected),
                  "DWARF5 expression bytes");

  const VariableLocationResult& block5 = ResultForDie(
      &analyzer5, "recover_alpha", "moving", 0x50101b, 0x57, &storage5);
  location_expect(block5.variable_die == 0x57 &&
                      block5.description == "fbreg:-12:frame=56",
                  "DWARF5 block fbreg description");
  location_expect(block5.expression_offset == 0x59,
                  "DWARF5 inline expression provenance");

  // Results own their bytes: reopening the analyzer with the other sample
  // must not invalidate a previously returned result.
  std::vector<uint8_t> saved = early5.expression_bytes;
  location_expect(analyzer5.Open("samples/optimized4.elf", &diagnostic),
                  "reopen analyzer with the other sample");
  location_expect(SameBytes(early5.expression_bytes, saved),
                  "stored expression bytes survive reopening the analyzer");
  location_expect(early5.description == "breg:6:-16",
                  "stored description survives reopening the analyzer");
}

}  // namespace

void register_location_cases() {
  location_add_case("legacy", LegacyCase);
  location_add_case("indexed", IndexedCase);
  location_add_case("defaults", DefaultsCase);
  location_add_case("cu_context", CuContextCase);
  location_add_case("samples", SamplesCase);
}
