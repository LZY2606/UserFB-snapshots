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

// OfflineSymbolConsumer turns an ELF that only needs to carry DWARF debug
// sections into (a) Breakpad symbol text produced by the production
// ElfReader -> CompilationUnit / DIEDispatcher -> DwarfCUToModule -> Module
// chain and (b) self-owned relation records describing how concrete
// functions and inline instances reach their terminal declarations through
// DW_AT_specification / DW_AT_abstract_origin chains.
//
// The consumer never executes the image and never reads process memory. It
// only maps the file's debug sections. Every offset reported here is an
// absolute .debug_info section offset ("section coordinate"), never a
// CU-relative offset. Final names and function/inline results are read back
// from the populated Module; the side channel only collects the structural
// reference evidence (DIE offsets, edges, ranges, call lines) through the
// existing CompilationUnit / DIEDispatcher callbacks. It never invents a
// declaration name from an address or from the sample name.

#ifndef COMMON_DWARF_DWARF_DIAG_CONSUMER_H__
#define COMMON_DWARF_DWARF_DIAG_CONSUMER_H__

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "common/dwarf/dwarf2enums.h"
#include "common/module.h"

namespace google_breakpad {

// One hop of a specification / abstract-origin chain. Both offsets are
// .debug_info section offsets.
struct DeclarationHop {
  // The DIE carrying the DW_AT_specification / DW_AT_abstract_origin link.
  uint64_t source_offset;
  // The DIE cited by that attribute.
  uint64_t target_offset;
  // Which attribute produced the edge.
  enum DwarfAttribute attribute;
};

// One relation row: a concrete function or a concrete inline instance and
// the declaration chain it depends on. The struct owns all of its data and
// can outlive the Module that produced it.
struct RelationRecord {
  // Base name of the ELF that was analyzed.
  std::string sample_name;

  // "function" for a DW_TAG_subprogram concrete instance; "inline" for a
  // DW_TAG_inlined_subroutine instance.
  std::string kind;

  // Section offset of the compilation unit header that contains the
  // concrete DIE.
  uint64_t cu_offset;

  // Section offset of the concrete DW_TAG_subprogram /
  // DW_TAG_inlined_subroutine DIE.
  uint64_t concrete_die_offset;

  // Ranges of the concrete instance, copied from the Module result.
  std::vector<Module::Range> ranges;

  // DW_AT_call_line for an inline instance, or -1 for a function.
  int call_site_line;

  // Resolved name taken verbatim from the Module (the Function name or the
  // Inline's InlineOrigin name). May be "<name omitted>" when no
  // declaration on the chain supplies a name.
  std::string final_name;

  // Citation-order hops from the concrete DIE toward the terminal
  // declaration. Empty when the DIE has no specification / abstract origin.
  std::vector<DeclarationHop> chain;

  // Section offset of the terminal declaration, or the concrete DIE's own
  // offset when it has no outgoing link.
  uint64_t terminal_offset;
};

// Complete result of an offline analysis. Both fields own their storage.
struct OfflineAnalysis {
  // Exact output of Module::Write(ALL_SYMBOL_DATA); the Breakpad symbol
  // format is unchanged.
  std::string symbols_text;

  // One record per concrete function and per concrete inline instance.
  std::vector<RelationRecord> relations;

  // False if the ELF could not be parsed as a supported 64-bit image or its
  // debug sections were missing.
  bool ok;
};

class OfflineSymbolConsumer {
 public:
  // Analyze the ELF at ELF_PATH through the production read chain and return
  // owned symbol text and relation records. Never executes the image.
  static std::unique_ptr<OfflineAnalysis> Analyze(
      const std::string& elf_path);
};

}  // namespace google_breakpad

#endif  // COMMON_DWARF_DWARF_DIAG_CONSUMER_H__
