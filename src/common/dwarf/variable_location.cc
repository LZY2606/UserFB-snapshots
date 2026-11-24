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

#include "common/dwarf/variable_location.h"

#include <stdio.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/dwarf/bytereader.h"
#include "common/dwarf/dwarf2reader.h"
#include "common/dwarf/elf_reader.h"

namespace google_breakpad {

namespace {

// Byte at which ELF EI_DATA lives.
const size_t kElfDataIndex = 5;

bool IsCallSiteTag(DwarfTag tag) {
  return tag == DW_TAG_call_site || tag == DW_TAG_call_site_parameter ||
         tag == DW_TAG_GNU_call_site ||
         tag == DW_TAG_GNU_call_site_parameter;
}

bool IsListForm(DwarfForm form) {
  return form == DW_FORM_sec_offset || form == DW_FORM_loclistx;
}

bool IsAddressForm(DwarfForm form) {
  return form == DW_FORM_addr || form == DW_FORM_addrx ||
         form == DW_FORM_addrx1 || form == DW_FORM_addrx2 ||
         form == DW_FORM_addrx3 || form == DW_FORM_addrx4 ||
         form == DW_FORM_GNU_addr_index;
}

std::string HexBytes(const std::vector<uint8_t>& bytes) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (uint8_t byte : bytes) {
    out.push_back(kDigits[byte >> 4]);
    out.push_back(kDigits[byte & 0x0f]);
  }
  return out;
}

// Decodes a complete SLEB128 at BYTES[*POS..]. Returns false if the value is
// truncated or does not fit in int64_t.
bool ReadSleb128(const std::vector<uint8_t>& bytes, size_t* pos, int64_t* out) {
  uint64_t result = 0;
  unsigned shift = 0;
  bool complete = false;
  while (*pos < bytes.size()) {
    const uint8_t byte = bytes[(*pos)++];
    if (shift < 64)
      result |= static_cast<uint64_t>(byte & 0x7f) << shift;
    shift += 7;
    if ((byte & 0x80) == 0) {
      complete = true;
      if (shift < 64 && (byte & 0x40))
        result |= ~static_cast<uint64_t>(0) << shift;
      break;
    }
    if (shift > 70)
      return false;
  }
  if (!complete)
    return false;
  *out = static_cast<int64_t>(result);
  return true;
}

// One DIE with the attributes this analysis cares about. The tree mirrors
// the DWARF DIE tree.
struct DieNode {
  DieNode() = default;

  uint64_t offset = 0;
  DwarfTag tag = DW_TAG_padding;
  std::string name;
  DieNode* parent = nullptr;
  std::vector<std::unique_ptr<DieNode>> children;

  bool has_low_pc = false;
  uint64_t low_pc = 0;
  bool has_high_pc = false;
  uint64_t high_pc = 0;
  DwarfForm high_pc_form = DW_FORM_data4;

  bool has_ranges = false;
  bool has_abstract_origin = false;
  bool has_specification = false;

  bool has_location = false;
  bool location_is_buffer = false;
  DwarfForm location_form = DW_FORM_sec_offset;
  uint64_t location_value = 0;
  std::vector<uint8_t> location_bytes;
  uint64_t location_section_offset = 0;

  bool has_frame_base = false;
  std::vector<uint8_t> frame_base_bytes;
  uint64_t frame_base_section_offset = 0;

  bool has_range = false;
  uint64_t range_begin = 0;
  uint64_t range_end = 0;

  // Resolves the DIE's own range. An address form stores an absolute
  // address in high_pc; any other form stores a length offset from low_pc.
  void ResolveRange() {
    if (!has_low_pc || !has_high_pc)
      return;
    has_range = true;
    range_begin = low_pc;
    if (IsAddressForm(high_pc_form))
      range_end = high_pc;
    else
      range_end = low_pc + high_pc;
  }
};

// Captures the DIE tree of one compilation unit for later queries. It never
// keeps pointers into the section buffers: location and frame base bytes are
// copied, and the section offsets are values.
class DieCollector : public Dwarf2Handler {
 public:
  DieCollector() = default;

  bool StartCompilationUnit(uint64_t, uint8_t, uint8_t, uint64_t,
                            uint8_t) override {
    return true;
  }

  bool NeedSplitDebugInfo() override { return false; }

  bool StartDIE(uint64_t offset, enum DwarfTag tag) override {
    auto node = std::make_unique<DieNode>();
    node->offset = offset;
    node->tag = tag;
    if (root_ == nullptr) {
      // The first DIE of a unit is its root compilation unit DIE.
      root_ = std::move(node);
      stack_.push_back(root_.get());
      return true;
    }
    node->parent = stack_.back();
    DieNode* raw = node.get();
    stack_.back()->children.push_back(std::move(node));
    stack_.push_back(raw);
    return true;
  }

  void ProcessAttributeUnsigned(uint64_t offset, enum DwarfAttribute attr,
                                enum DwarfForm form, uint64_t data) override {
    DieNode* node = Current(offset);
    if (node == nullptr)
      return;
    switch (attr) {
      case DW_AT_low_pc:
        node->has_low_pc = true;
        node->low_pc = data;
        break;
      case DW_AT_high_pc:
        node->has_high_pc = true;
        node->high_pc = data;
        node->high_pc_form = form;
        break;
      case DW_AT_ranges:
        node->has_ranges = true;
        break;
      case DW_AT_location:
        if (IsListForm(form)) {
          node->has_location = true;
          node->location_is_buffer = false;
          node->location_form = form;
          node->location_value = data;
        } else if (form == DW_FORM_exprloc || form == DW_FORM_block ||
                   form == DW_FORM_block1 || form == DW_FORM_block2 ||
                   form == DW_FORM_block4) {
          // A block attribute is delivered through ProcessAttributeBufferAt.
        } else {
          node->has_location = true;
          node->location_is_buffer = false;
          node->location_form = form;
          node->location_value = data;
        }
        break;
      case DW_AT_frame_base:
        // Non-buffer frame bases (for example a constant value) cannot be
        // interpreted as a frame base expression, so they are ignored here.
        // fbreg will then report that the frame base is missing.
        break;
      case DW_AT_abstract_origin:
        node->has_abstract_origin = true;
        break;
      case DW_AT_specification:
        node->has_specification = true;
        break;
      default:
        break;
    }
  }

  void ProcessAttributeSigned(uint64_t offset, enum DwarfAttribute attr,
                              enum DwarfForm form, int64_t data) override {
    ProcessAttributeUnsigned(offset, attr, form,
                             static_cast<uint64_t>(data));
  }

  void ProcessAttributeReference(uint64_t offset, enum DwarfAttribute attr,
                                 enum DwarfForm, uint64_t) override {
    DieNode* node = Current(offset);
    if (node == nullptr)
      return;
    if (attr == DW_AT_abstract_origin)
      node->has_abstract_origin = true;
    else if (attr == DW_AT_specification)
      node->has_specification = true;
  }

  void ProcessAttributeString(uint64_t offset, enum DwarfAttribute attr,
                              enum DwarfForm, const std::string& data) override {
    if (attr != DW_AT_name)
      return;
    DieNode* node = Current(offset);
    if (node != nullptr)
      node->name = data;
  }

  void ProcessAttributeBufferAt(uint64_t offset, enum DwarfAttribute attr,
                                enum DwarfForm form, const uint8_t* data,
                                uint64_t len,
                                uint64_t section_offset) override {
    DieNode* node = Current(offset);
    if (node == nullptr)
      return;
    if (attr == DW_AT_location) {
      node->has_location = true;
      node->location_is_buffer = true;
      node->location_form = form;
      node->location_section_offset = section_offset;
      node->location_bytes.assign(data, data + len);
    } else if (attr == DW_AT_frame_base) {
      node->has_frame_base = true;
      node->frame_base_section_offset = section_offset;
      node->frame_base_bytes.assign(data, data + len);
    }
  }

  void EndDIE(uint64_t offset) override {
    if (stack_.size() <= 1)
      return;
    DieNode* node = stack_.back();
    if (node->offset != offset)
      return;
    node->ResolveRange();
    stack_.pop_back();
  }

  std::unique_ptr<DieNode> TakeRoot() { return std::move(root_); }

 private:
  // Attributes always belong to the most recently started DIE.
  DieNode* Current(uint64_t) { return stack_.empty() ? nullptr : stack_.back(); }

  std::unique_ptr<DieNode> root_;
  std::vector<DieNode*> stack_;
};

struct UnitData {
  uint64_t cu_offset = 0;
  LocationListContext context;
  std::unique_ptr<DieNode> root;
};

struct SectionStorage {
  std::map<std::string, std::vector<uint8_t>> bytes;
  SectionMap map;
};

bool Contains(const DieNode& node, uint64_t pc) {
  return node.has_range && node.range_begin <= pc && pc < node.range_end;
}

void CollectVariablePath(const DieNode* function_node,
                         const DieNode* target,
                         std::vector<const DieNode*>* path) {
  for (const DieNode* node = target; node != nullptr; node = node->parent) {
    path->push_back(node);
    if (node == function_node)
      break;
  }
  std::reverse(path->begin(), path->end());
}

bool SubtreeHasInlinedSubroutine(const DieNode& node) {
  if (node.tag == DW_TAG_inlined_subroutine)
    return true;
  for (const auto& child : node.children) {
    if (SubtreeHasInlinedSubroutine(*child))
      return true;
  }
  return false;
}

}  // namespace

bool DescribeLocationExpression(const std::vector<uint8_t>& expression,
                                bool has_frame_base,
                                const std::vector<uint8_t>& frame_base,
                                std::string* description,
                                VariableDiagnostic* diagnostic) {
  if (description == nullptr)
    return false;
  description->clear();
  if (expression.empty())
    return false;

  const uint8_t opcode = expression[0];
  if (opcode >= DW_OP_reg0 && opcode <= DW_OP_reg31) {
    // Only an expression that is exactly this one opcode names a register.
    if (expression.size() == 1) {
      *description = "reg:" + std::to_string(opcode - DW_OP_reg0);
      return true;
    }
    *description = "opaque";
    return true;
  }

  if (opcode >= DW_OP_breg0 && opcode <= DW_OP_breg31) {
    size_t pos = 1;
    int64_t offset = 0;
    if (pos < expression.size() && ReadSleb128(expression, &pos, &offset) &&
        pos == expression.size()) {
      *description = "breg:" + std::to_string(opcode - DW_OP_breg0) + ":" +
                     std::to_string(offset);
      return true;
    }
    // A breg opcode with a malformed operand is still data we can show, but
    // we cannot attribute a register offset to it.
    *description = "opaque";
    return true;
  }

  if (opcode == DW_OP_fbreg) {
    size_t pos = 1;
    int64_t offset = 0;
    if (!ReadSleb128(expression, &pos, &offset) || pos != expression.size()) {
      // Not a single fbreg with a complete operand; describe it byte for
      // byte instead of guessing an offset.
      *description = "opaque";
      return true;
    }
    if (!has_frame_base || frame_base.empty()) {
      if (diagnostic != nullptr) {
        diagnostic->section = ".debug_info";
        diagnostic->offset = 0;
        diagnostic->message =
            "DW_OP_fbreg needs the owning function's frame_base expression";
      }
      return false;
    }
    *description = "fbreg:" + std::to_string(offset) +
                   ":frame=" + HexBytes(frame_base);
    return true;
  }

  *description = "opaque";
  return true;
}

struct VariableLocationAnalyzer::Impl {
  std::string path;
  Endianness endianness = ENDIANNESS_LITTLE;
  SectionStorage sections;
  std::vector<UnitData> units;
};

VariableLocationAnalyzer::VariableLocationAnalyzer() : impl_(new Impl) {}

VariableLocationAnalyzer::~VariableLocationAnalyzer() = default;

bool VariableLocationAnalyzer::Open(const std::string& path,
                                    VariableDiagnostic* diagnostic) {
  impl_->path = path;
  impl_->units.clear();
  impl_->sections.bytes.clear();
  impl_->sections.map.clear();

  ElfReader elf(path);
  if (!elf.IsElf32File() && !elf.IsElf64File()) {
    if (diagnostic != nullptr) {
      diagnostic->section = "";
      diagnostic->offset = 0;
      diagnostic->message = "not an ELF file: " + path;
    }
    return false;
  }

  // Record the file's byte order for the ByteReader snapshots.
  {
    std::ifstream file(path, std::ios::binary);
    char ident[16] = {0};
    if (!file || !file.read(ident, sizeof(ident))) {
      if (diagnostic != nullptr) {
        diagnostic->section = "";
        diagnostic->offset = 0;
        diagnostic->message = "cannot read ELF identification";
      }
      return false;
    }
    impl_->endianness =
        static_cast<uint8_t>(ident[kElfDataIndex]) == 2 ? ENDIANNESS_BIG
                                                        : ENDIANNESS_LITTLE;
  }

  for (uint64_t i = 1; i < elf.GetNumSections(); ++i) {
    const char* name = elf.GetSectionName(static_cast<int>(i));
    if (name == nullptr || strncmp(name, ".debug_", 7) != 0)
      continue;
    size_t size = 0;
    const char* data = elf.GetSectionByIndex(static_cast<int>(i), &size);
    if (data == nullptr) {
      if (diagnostic != nullptr) {
        diagnostic->section = name;
        diagnostic->offset = 0;
        diagnostic->message = "cannot read debug section";
      }
      return false;
    }
    std::vector<uint8_t>& copy = impl_->sections.bytes[name];
    copy.assign(reinterpret_cast<const uint8_t*>(data),
                reinterpret_cast<const uint8_t*>(data) + size);
  }

  if (impl_->sections.bytes.find(".debug_info") ==
          impl_->sections.bytes.end() ||
      impl_->sections.bytes.find(".debug_abbrev") ==
          impl_->sections.bytes.end()) {
    if (diagnostic != nullptr) {
      diagnostic->section =
          impl_->sections.bytes.count(".debug_info") ? ".debug_abbrev"
                                                     : ".debug_info";
      diagnostic->offset = 0;
      diagnostic->message = "required debug section is missing";
    }
    return false;
  }

  // The copies live in std::map nodes, so pointers stay valid.
  for (auto& entry : impl_->sections.bytes) {
    impl_->sections.map[entry.first] =
        std::make_pair(entry.second.data(), entry.second.size());
  }

  const uint64_t info_size = impl_->sections.bytes[".debug_info"].size();
  uint64_t offset = 0;
  while (offset < info_size) {
    ByteReader reader(impl_->endianness);
    DieCollector collector;
    CompilationUnit unit(path, impl_->sections.map, offset, &reader, &collector);
    const uint64_t length = unit.Start();
    if (length == 0 || length > info_size - offset) {
      if (diagnostic != nullptr) {
        diagnostic->section = ".debug_info";
        diagnostic->offset = offset;
        diagnostic->message = "cannot parse compilation unit";
      }
      return false;
    }
    UnitData data;
    data.cu_offset = offset;
    data.context = unit.GetLocationListContext();
    data.root = collector.TakeRoot();
    impl_->units.push_back(std::move(data));
    offset += length;
  }

  if (impl_->units.empty()) {
    if (diagnostic != nullptr) {
      diagnostic->section = ".debug_info";
      diagnostic->offset = 0;
      diagnostic->message = "no compilation units";
    }
    return false;
  }
  return true;
}

bool VariableLocationAnalyzer::Query(const std::string& function_name,
                                     const std::string& variable_name,
                                     uint64_t pc,
                                     std::vector<VariableLocationResult>* results,
                                     VariableDiagnostic* diagnostic) const {
  if (results == nullptr)
    return false;
  results->clear();
  if (diagnostic != nullptr)
    *diagnostic = VariableDiagnostic();

  struct Candidate {
    const UnitData* unit;
    const DieNode* function_node;
    const DieNode* variable_node;
  };
  std::vector<Candidate> candidates;

  for (const UnitData& unit : impl_->units) {
    if (unit.root == nullptr)
      continue;  // Type units and skipped units carry no variable DIEs.
    // The query is rejected when the unit itself uses constructs this
    // analysis does not implement, rather than guessing a result.
    for (const auto& child : unit.root->children) {
      if (child->tag != DW_TAG_subprogram || child->name != function_name)
        continue;
      if (!child->has_range)
        continue;
      if (child->has_abstract_origin || child->has_specification ||
          child->has_ranges) {
        if (diagnostic != nullptr) {
          diagnostic->section = ".debug_info";
          diagnostic->offset = child->offset;
          diagnostic->message =
              "subprogram uses unsupported ranges/abstract_origin/"
              "specification inheritance";
        }
        return false;
      }
      if (SubtreeHasInlinedSubroutine(*child)) {
        if (diagnostic != nullptr) {
          diagnostic->section = ".debug_info";
          diagnostic->offset = child->offset;
          diagnostic->message = "inlined subroutines are not supported";
        }
        return false;
      }
    }
  }

  for (const UnitData& unit : impl_->units) {
    if (unit.root == nullptr)
      continue;
    for (const auto& child : unit.root->children) {
      if (child->tag != DW_TAG_subprogram || child->name != function_name ||
          !child->has_range)
        continue;
      std::vector<const DieNode*> pending;
      for (const auto& grandchild : child->children)
        pending.push_back(grandchild.get());
      while (!pending.empty()) {
        const DieNode* node = pending.back();
        pending.pop_back();
        if (IsCallSiteTag(node->tag))
          continue;  // Call-site entries never describe variables.
        if (node->tag == DW_TAG_variable ||
            node->tag == DW_TAG_formal_parameter) {
          if (node->name == variable_name)
            candidates.push_back({&unit, child.get(), node});
          continue;
        }
        if (node->tag == DW_TAG_subprogram)
          continue;  // Nested functions own their variables.
        for (const auto& grandchild : node->children)
          pending.push_back(grandchild.get());
      }
    }
  }

  if (candidates.empty()) {
    VariableLocationResult not_found;
    not_found.cu_offset = 0;
    not_found.function_die = 0;
    not_found.variable_die = 0;
    not_found.status = VariableLocationStatus::kNotFound;
    not_found.has_location_attribute = false;
    results->push_back(std::move(not_found));
    return true;
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& lhs, const Candidate& rhs) {
              return lhs.variable_node->offset < rhs.variable_node->offset;
            });

  for (const Candidate& candidate : candidates) {
    VariableLocationResult result;
    result.cu_offset = candidate.unit->cu_offset;
    result.function_die = candidate.function_node->offset;
    result.variable_die = candidate.variable_node->offset;

    std::vector<const DieNode*> path;
    CollectVariablePath(candidate.function_node, candidate.variable_node,
                        &path);
    for (const DieNode* node : path) {
      if (node->has_ranges) {
        if (diagnostic != nullptr) {
          diagnostic->section = ".debug_info";
          diagnostic->offset = node->offset;
          diagnostic->message = "DW_AT_ranges is not supported";
        }
        return false;
      }
      if (node->has_abstract_origin || node->has_specification) {
        if (diagnostic != nullptr) {
          diagnostic->section = ".debug_info";
          diagnostic->offset = node->offset;
          diagnostic->message =
              "abstract_origin/specification inheritance is not supported";
        }
        return false;
      }
    }

    // Scopes: the function plus every ancestor lexical block that has its own
    // range, in ancestor order. Blocks without a range inherit their parent's
    // range and are transparent.
    bool in_scope = Contains(*candidate.function_node, pc);
    for (const DieNode* node : path) {
      if (node->tag != DW_TAG_lexical_block)
        continue;
      if (!node->has_range)
        continue;
      VariableScopeRange range;
      range.die_offset = node->offset;
      range.begin = node->range_begin;
      range.end = node->range_end;
      result.scopes.push_back(range);
      if (!Contains(*node, pc))
        in_scope = false;
    }
    VariableScopeRange function_range;
    function_range.die_offset = candidate.function_node->offset;
    function_range.begin = candidate.function_node->range_begin;
    function_range.end = candidate.function_node->range_end;
    result.scopes.insert(result.scopes.begin(), function_range);

    if (!in_scope) {
      result.status = VariableLocationStatus::kOutOfScope;
      if (candidate.variable_node->has_location) {
        result.has_location_attribute = true;
        result.location_form = candidate.variable_node->location_form;
        result.location_value = candidate.variable_node->location_value;
        result.location_is_buffer = candidate.variable_node->location_is_buffer;
      }
      results->push_back(std::move(result));
      continue;
    }

    const DieNode& variable = *candidate.variable_node;
    if (!variable.has_location) {
      result.status = VariableLocationStatus::kUnavailable;
      results->push_back(std::move(result));
      continue;
    }
    result.has_location_attribute = true;
    result.location_form = variable.location_form;
    result.location_value = variable.location_value;
    result.location_is_buffer = variable.location_is_buffer;

    const DieNode& function = *candidate.function_node;
    if (function.has_frame_base) {
      result.frame_base_present = true;
      result.frame_base_section = ".debug_info";
      result.frame_base_offset = function.frame_base_section_offset;
      result.frame_base_bytes = function.frame_base_bytes;
    }

    std::vector<uint8_t> expression;
    bool have_expression = false;
    if (variable.location_is_buffer) {
      expression = variable.location_bytes;
      have_expression = true;
      result.has_expression = true;
      result.expression_section = ".debug_info";
      result.expression_offset = variable.location_section_offset;
    } else if (IsListForm(variable.location_form)) {
      LocationListReader reader(impl_->sections.map, candidate.unit->context);
      std::vector<LocationEntry> entries;
      if (!reader.ReadLocations(variable.location_form, variable.location_value,
                                &entries)) {
        if (diagnostic != nullptr) {
          const LocationListDiagnostic& list_diagnostic = reader.diagnostic();
          diagnostic->section = list_diagnostic.section;
          diagnostic->offset = list_diagnostic.offset;
          diagnostic->message = list_diagnostic.message;
        }
        return false;
      }
      const LocationEntry* selected = nullptr;
      std::vector<uint8_t> selected_expression;
      if (LocationListReader::SelectLocation(entries, pc, &selected_expression,
                                             &selected)) {
        have_expression = true;
        expression = selected_expression;
        result.has_entry = true;
        result.entry_section = selected->section;
        result.entry_offset = selected->entry_offset;
        result.has_expression = true;
        result.expression_section = selected->section;
        result.expression_offset = selected->expression_offset;
      }
    } else {
      if (diagnostic != nullptr) {
        diagnostic->section = ".debug_info";
        diagnostic->offset = variable.offset;
        diagnostic->message = "unsupported DW_AT_location form";
      }
      return false;
    }

    if (!have_expression) {
      // The list is a hole at this PC: no entry applies.
      result.status = VariableLocationStatus::kUnavailable;
      results->push_back(std::move(result));
      continue;
    }

    if (expression.empty()) {
      // An explicitly matched empty expression means "no location"; it does
      // not fall back to a default entry.
      result.status = VariableLocationStatus::kUnavailable;
      results->push_back(std::move(result));
      continue;
    }

    result.expression_bytes = expression;
    std::string description;
    VariableDiagnostic describe_diagnostic;
    if (!DescribeLocationExpression(expression, result.frame_base_present,
                                    result.frame_base_bytes, &description,
                                    &describe_diagnostic)) {
      if (diagnostic != nullptr) {
        *diagnostic = describe_diagnostic;
        if (diagnostic->section == ".debug_info" &&
            diagnostic->offset == 0 && result.has_expression) {
          diagnostic->section = result.expression_section;
          diagnostic->offset = result.expression_offset;
        }
      }
      return false;
    }
    result.description = description;
    result.status = VariableLocationStatus::kDescribed;
    results->push_back(std::move(result));
  }
  return true;
}

}  // namespace google_breakpad
