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

// Static recovery of variable storage descriptions from optimized ELF files.
//
// The analyzer reads real .debug_info/.debug_abbrev (and location list
// sections) through the existing ElfReader/CompilationUnit/Dwarf2Handler
// chain. It answers queries of the form "at PC, where does variable V of
// function F live?" while keeping variable identity tied to DIE offsets, not
// names.
//
// All returned data own their expression bytes, so results stay usable after
// the analyzer, its ELF reader and its section buffers are gone.

#ifndef COMMON_DWARF_VARIABLE_LOCATION_H__
#define COMMON_DWARF_VARIABLE_LOCATION_H__

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "common/dwarf/dwarf2enums.h"
#include "common/dwarf/location_list_reader.h"

namespace google_breakpad {

enum class VariableLocationStatus {
  kDescribed,
  kUnavailable,
  kOutOfScope,
  kNotFound,
};

// A DIE that constrains where a variable is visible: the function itself and
// any lexical block ancestor that has its own range. The range is half-open.
struct VariableScopeRange {
  VariableScopeRange() : die_offset(0), begin(0), end(0) {}

  uint64_t die_offset;
  uint64_t begin;
  uint64_t end;
};

// A diagnostic for data that cannot be analysed. SECTION and OFFSET identify
// the bytes the analysis stopped at; consumers must report them rather than
// silently returning an empty result.
struct VariableDiagnostic {
  VariableDiagnostic() : offset(0) {}

  bool empty() const { return message.empty(); }

  std::string section;
  uint64_t offset;
  std::string message;
};

// One candidate variable for one query. Multiple same-named variables are
// returned independently; each carries its own DIE identity.
struct VariableLocationResult {
  VariableLocationResult()
      : cu_offset(0),
        function_die(0),
        variable_die(0),
        status(VariableLocationStatus::kNotFound),
        has_location_attribute(false),
        location_form(DW_FORM_sec_offset),
        location_value(0),
        location_is_buffer(false),
        has_entry(false),
        entry_offset(0),
        has_expression(false),
        expression_offset(0),
        frame_base_present(false),
        frame_base_offset(0) {}

  uint64_t cu_offset;
  uint64_t function_die;
  uint64_t variable_die;
  std::vector<VariableScopeRange> scopes;

  VariableLocationStatus status;
  // "reg:N", "breg:N:D", "fbreg:D:frame=HEX" or "opaque"; empty unless the
  // status is kDescribed.
  std::string description;

  // DW_AT_location. When the attribute is absent, has_location_attribute is
  // false and the other attribute fields are ignored.
  bool has_location_attribute;
  DwarfForm location_form;
  uint64_t location_value;
  bool location_is_buffer;

  // Selected location list entry, if any. entry_section is ".debug_loc" or
  // ".debug_loclists"; the offset is section-relative.
  bool has_entry;
  std::string entry_section;
  uint64_t entry_offset;

  // Expression provenance and bytes. expression_section is ".debug_info" for
  // direct exprloc/block attributes and the list section for list entries.
  bool has_expression;
  std::string expression_section;
  uint64_t expression_offset;
  std::vector<uint8_t> expression_bytes;

  // The owning function's raw DW_AT_frame_base, kept for fbreg interpretation.
  bool frame_base_present;
  std::string frame_base_section;
  uint64_t frame_base_offset;
  std::vector<uint8_t> frame_base_bytes;
};

// Interprets a non-empty location expression. Supports exactly one
// DW_OP_reg0..31, one DW_OP_breg0..31 with a complete SLEB128, or one
// DW_OP_fbreg with a complete SLEB128. FBREG needs the owning function's raw
// frame base; when it is absent or was not a constant expression this returns
// false with a diagnostic. Any other expression is described as "opaque".
bool DescribeLocationExpression(const std::vector<uint8_t>& expression,
                                bool has_frame_base,
                                const std::vector<uint8_t>& frame_base,
                                std::string* description,
                                VariableDiagnostic* diagnostic);

class VariableLocationAnalyzer {
 public:
  VariableLocationAnalyzer();
  ~VariableLocationAnalyzer();

  VariableLocationAnalyzer(const VariableLocationAnalyzer&) = delete;
  VariableLocationAnalyzer& operator=(const VariableLocationAnalyzer&) =
      delete;

  // Reads and parses the given ELF. Returns false with a diagnostic when the
  // file cannot be read or required debug sections are missing. The analyzer
  // keeps private copies of the debug sections, so it does not reference the
  // ElfReader after this call.
  bool Open(const std::string& path, VariableDiagnostic* diagnostic);

  // Returns every matching variable candidate for FUNCTION_NAME and
  // VARIABLE_NAME, sorted by variable DIE offset. A name that is not present
  // yields a single kNotFound result. Unsupported constructs that affect the
  // query (DW_AT_ranges, split DWARF, inlined subroutines,
  // abstract_origin/specification inheritance, unknown location forms) make
  // this return false with a diagnostic instead of inventing a result.
  bool Query(const std::string& function_name,
             const std::string& variable_name, uint64_t pc,
             std::vector<VariableLocationResult>* results,
             VariableDiagnostic* diagnostic) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace google_breakpad

#endif  // COMMON_DWARF_VARIABLE_LOCATION_H__
