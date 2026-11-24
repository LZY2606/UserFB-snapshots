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

#include "common/dwarf/dwarf_diag_consumer.h"

#include <map>
#include <sstream>
#include <utility>

#include "common/dwarf/bytereader.h"
#include "common/dwarf/dwarf2diehandler.h"
#include "common/dwarf/dwarf2reader.h"
#include "common/dwarf/elf_reader.h"
#include "common/dwarf_cu_to_module.h"
#include "common/symbol_data.h"

namespace google_breakpad {

namespace {

// Structural facts about one DIE, gathered purely from the
// CompilationUnit / DIEDispatcher attribute callbacks. No name resolution
// happens here: names are taken from the Module result afterwards.
struct DieEvidence {
  uint64_t cu_offset = 0;
  enum DwarfTag tag = DW_TAG_padding;
  // The DIE's outgoing specification / abstract-origin edge, if any.
  bool has_edge = false;
  uint64_t edge_target = 0;
  enum DwarfAttribute edge_attribute = DW_AT_sibling;
};

// Records reference edges for a non-root DIE and creates fresh handlers for
// its children, so the whole DIE tree is visited.
class EvidenceHandler : public DIEHandler {
 public:
  EvidenceHandler(std::map<uint64_t, DieEvidence>* dies, uint64_t cu_offset,
                  uint64_t offset)
      : dies_(dies), cu_offset_(cu_offset), offset_(offset) {}

  void ProcessAttributeReference(enum DwarfAttribute attr,
                                 enum DwarfForm /*form*/,
                                 uint64_t data) override {
    if (attr != DW_AT_specification && attr != DW_AT_abstract_origin)
      return;
    DieEvidence& info = (*dies_)[offset_];
    info.has_edge = true;
    info.edge_target = data;
    info.edge_attribute = attr;
  }

  // Visit the whole subtree; the DIEHandler default stops at this DIE.
  bool EndAttributes() override { return true; }
  DIEHandler* FindChildHandler(uint64_t offset,
                               enum DwarfTag tag) override {
    DieEvidence& info = (*dies_)[offset];
    info.cu_offset = cu_offset_;
    info.tag = tag;
    return new EvidenceHandler(dies_, cu_offset_, offset);
  }

 private:
  std::map<uint64_t, DieEvidence>* dies_;
  uint64_t cu_offset_;
  uint64_t offset_;
};

// Root handler for the evidence pass. It tags every DIE with its CU offset
// and DWARF tag and delegates edge collection to EvidenceHandler.
class EvidenceRootHandler : public RootDIEHandler {
 public:
  explicit EvidenceRootHandler(std::map<uint64_t, DieEvidence>* dies)
      : RootDIEHandler(true), dies_(dies), current_cu_(0), root_offset_(0) {}

  bool StartCompilationUnit(uint64_t offset, uint8_t /*address_size*/,
                            uint8_t /*offset_size*/, uint64_t /*cu_length*/,
                            uint8_t /*dwarf_version*/) override {
    current_cu_ = offset;
    return true;
  }

  bool StartRootDIE(uint64_t offset, enum DwarfTag tag) override {
    root_offset_ = offset;
    DieEvidence& info = (*dies_)[offset];
    info.cu_offset = current_cu_;
    info.tag = tag;
    return true;
  }

  void ProcessAttributeReference(enum DwarfAttribute attr,
                                 enum DwarfForm /*form*/,
                                 uint64_t data) override {
    if (attr != DW_AT_specification && attr != DW_AT_abstract_origin)
      return;
    DieEvidence& info = (*dies_)[root_offset_];
    info.has_edge = true;
    info.edge_target = data;
    info.edge_attribute = attr;
  }

  // Visit the root DIE's children (the subprograms); the default stops here.
  bool EndAttributes() override { return true; }

  DIEHandler* FindChildHandler(uint64_t offset, enum DwarfTag tag) override {
    DieEvidence& info = (*dies_)[offset];
    info.cu_offset = current_cu_;
    info.tag = tag;
    return new EvidenceHandler(dies_, current_cu_, offset);
  }

 private:
  std::map<uint64_t, DieEvidence>* dies_;
  uint64_t current_cu_;
  uint64_t root_offset_;
};

// Walk the specification / abstract-origin edges recorded for
// CONCRETE_OFFSET and append one DeclarationHop per edge until reaching a
// DIE with no outgoing edge. Returns the terminal offset.
uint64_t BuildChain(const std::map<uint64_t, DieEvidence>& dies,
                    uint64_t concrete_offset,
                    std::vector<DeclarationHop>* chain) {
  uint64_t node = concrete_offset;
  const unsigned kMaxHops = 100000;
  unsigned hops = 0;
  while (hops < kMaxHops) {
    auto it = dies.find(node);
    if (it == dies.end() || !it->second.has_edge)
      break;
    DeclarationHop hop;
    hop.source_offset = node;
    hop.target_offset = it->second.edge_target;
    hop.attribute = it->second.edge_attribute;
    chain->push_back(hop);
    node = it->second.edge_target;
    ++hops;
  }
  return node;
}

const char* BaseName(const std::string& path) {
  size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path.c_str()
                                    : path.c_str() + slash + 1;
}

}  // namespace

// static
std::unique_ptr<OfflineAnalysis> OfflineSymbolConsumer::Analyze(
    const std::string& elf_path) {
  auto result = std::make_unique<OfflineAnalysis>();
  result->ok = false;

  ElfReader elf(elf_path);
  if (!elf.IsElf64File())
    return result;

  static const char* kSections[] = {".debug_abbrev", ".debug_info"};
  SectionMap sections;
  for (const char* name : kSections) {
    size_t size = 0;
    const char* data = elf.GetSectionByName(name, &size);
    if (!data || !size)
      return result;
    sections[name] =
        std::make_pair(reinterpret_cast<const uint8_t*>(data), size);
  }
  uint64_t info_size = sections[".debug_info"].second;

  Module module(BaseName(elf_path), "Linux", "x86_64", "0123456789ABCDEF");

  // First sweep: the production chain CompilationUnit -> DIEDispatcher ->
  // DwarfCUToModule -> Module. This is the only place names are resolved.
  DwarfCUToModule::FileContext file_context(elf_path, &module, true);
  for (const auto& entry : sections)
    file_context.AddSectionToSectionMap(entry.first, entry.second.first,
                                        entry.second.second);

  ByteReader production_reader(ENDIANNESS_LITTLE);
  uint64_t offset = 0;
  while (offset < info_size) {
    DwarfCUToModule::WarningReporter reporter(elf_path, offset);
    DwarfCUToModule root(&file_context, nullptr, nullptr, &reporter, true);
    DIEDispatcher dispatcher(&root);
    CompilationUnit cu(elf_path, file_context.section_map(), offset,
                       &production_reader, &dispatcher);
    uint64_t consumed = cu.Start();
    if (!consumed)
      return result;
    offset += consumed;
  }

  // Second sweep over the same bytes collecting only structural reference
  // evidence (DIE offsets, tags, edges). It deliberately does not compute
  // names, so it cannot mask a production name-resolution defect.
  std::map<uint64_t, DieEvidence> dies;
  ByteReader evidence_reader(ENDIANNESS_LITTLE);
  offset = 0;
  while (offset < info_size) {
    EvidenceRootHandler evidence_root(&dies);
    DIEDispatcher dispatcher(&evidence_root);
    CompilationUnit cu(elf_path, sections, offset, &evidence_reader,
                       &dispatcher);
    uint64_t consumed = cu.Start();
    if (!consumed)
      return result;
    offset += consumed;
  }

  // Build relation records from the authoritative Module results.
  std::vector<Module::Function*> functions;
  module.GetFunctions(&functions, functions.begin());
  const std::string sample = BaseName(elf_path);

  for (const Module::Function* function : functions) {
    RelationRecord record;
    record.sample_name = sample;
    record.kind = "function";
    record.concrete_die_offset = function->debug_info_offset;
    auto die = dies.find(function->debug_info_offset);
    record.cu_offset = die != dies.end() ? die->second.cu_offset : 0;
    record.ranges = function->ranges;
    record.call_site_line = -1;
    record.final_name = function->name.str();
    record.terminal_offset =
        BuildChain(dies, function->debug_info_offset, &record.chain);
    result->relations.push_back(std::move(record));
  }

  for (const Module::Function* function : functions) {
    for (const auto& inline_ptr : function->inlines) {
      const Module::Inline* in = inline_ptr.get();
      RelationRecord record;
      record.sample_name = sample;
      record.kind = "inline";
      record.concrete_die_offset = in->debug_info_offset;
      auto die = dies.find(in->debug_info_offset);
      record.cu_offset = die != dies.end() ? die->second.cu_offset : 0;
      record.ranges = in->ranges;
      record.call_site_line = in->call_site_line;
      record.final_name = in->origin->name.str();
      record.terminal_offset =
          BuildChain(dies, in->debug_info_offset, &record.chain);
      result->relations.push_back(std::move(record));
    }
  }

  std::ostringstream symbols;
  if (!module.Write(symbols, ALL_SYMBOL_DATA))
    return result;
  result->symbols_text = symbols.str();
  result->ok = true;
  return result;
}

}  // namespace google_breakpad
