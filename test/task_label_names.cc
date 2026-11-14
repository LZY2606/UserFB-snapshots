/*
 * Copyright 2026 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Task tests for the Label subsection (id 3) of the name custom section.
// The modules are built from raw bytes and only read/printed; nothing is
// instantiated or executed.

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "task_support/check.h"
#include "wabt/apply-names.h"
#include "wabt/binary-reader-ir.h"
#include "wabt/binary-reader.h"
#include "wabt/cast.h"
#include "wabt/error.h"
#include "wabt/feature.h"
#include "wabt/generate-names.h"
#include "wabt/ir.h"
#include "wabt/resolve-names.h"
#include "wabt/stream.h"
#include "wabt/wast-lexer.h"
#include "wabt/wast-parser.h"
#include "wabt/wat-writer.h"

namespace {

using Bytes = std::vector<uint8_t>;
using wabt::Index;

void U32(Bytes& b, uint32_t v) {
  do {
    uint8_t c = v & 127;
    v >>= 7;
    b.push_back(c | (v ? 128 : 0));
  } while (v);
}

void Str(Bytes& b, std::string_view s) {
  U32(b, static_cast<uint32_t>(s.size()));
  b.insert(b.end(), s.begin(), s.end());
}

void Section(Bytes& b, uint8_t id, const Bytes& payload) {
  b.push_back(id);
  U32(b, static_cast<uint32_t>(payload.size()));
  b.insert(b.end(), payload.begin(), payload.end());
}

void Append(Bytes& dst, const Bytes& src) {
  dst.insert(dst.end(), src.begin(), src.end());
}

Bytes& operator+=(Bytes& dst, const Bytes& src) {
  Append(dst, src);
  return dst;
}

// Opcodes used by the hand-built bodies.
constexpr uint8_t kBlock = 0x02;
constexpr uint8_t kLoop = 0x03;
constexpr uint8_t kIf = 0x04;
constexpr uint8_t kElse = 0x05;
constexpr uint8_t kEnd = 0x0b;
constexpr uint8_t kBr = 0x0c;
constexpr uint8_t kBrIf = 0x0d;
constexpr uint8_t kBrTable = 0x0e;
constexpr uint8_t kI32Const = 0x41;
constexpr uint8_t kVoidBlockType = 0x40;

struct LabelEntry {
  Index label_index;
  std::string name;  // empty means "no name provided"
};

struct FuncLabelTable {
  Index func_index;
  std::vector<LabelEntry> entries;
};

// Encodes the label subsection (id 3) including its id and size.
Bytes LabelSubsection(const std::vector<FuncLabelTable>& funcs) {
  Bytes p;
  U32(p, static_cast<uint32_t>(funcs.size()));
  for (const auto& f : funcs) {
    U32(p, f.func_index);
    U32(p, static_cast<uint32_t>(f.entries.size()));
    for (const auto& e : f.entries) {
      U32(p, e.label_index);
      Str(p, e.name);
    }
  }
  Bytes out;
  out.push_back(3);  // Label subsection id.
  U32(out, static_cast<uint32_t>(p.size()));
  out.insert(out.end(), p.begin(), p.end());
  return out;
}

// Wraps a payload as the "name" custom section.
Bytes NameSection(const Bytes& subsections) {
  Bytes name;
  Str(name, "name");
  name.insert(name.end(), subsections.begin(), subsections.end());
  Bytes out;
  Section(out, 0, name);
  return out;
}

Bytes FunctionNameSubsection(const std::vector<std::string>& names) {
  Bytes p;
  U32(p, static_cast<uint32_t>(names.size()));
  for (Index i = 0; i < names.size(); ++i) {
    U32(p, i);
    Str(p, names[i]);
  }
  Bytes out;
  out.push_back(1);  // Function subsection id.
  U32(out, static_cast<uint32_t>(p.size()));
  out.insert(out.end(), p.begin(), p.end());
  return out;
}

// Local subsection (id 2) naming one local of one function.
Bytes LocalNameSubsection(Index func_index,
                          Index local_index,
                          std::string_view local_name) {
  Bytes p;
  U32(p, 1);  // one function
  U32(p, func_index);
  U32(p, 1);  // one named local
  U32(p, local_index);
  Str(p, local_name);
  Bytes out;
  out.push_back(2);  // Local subsection id.
  U32(out, static_cast<uint32_t>(p.size()));
  out.insert(out.end(), p.begin(), p.end());
  return out;
}

// One function type () -> ().
Bytes TypeSection() {
  Bytes p;
  U32(p, 1);
  p.push_back(0x60);
  U32(p, 0);
  U32(p, 0);
  Bytes out;
  Section(out, 1, p);
  return out;
}

Bytes ImportSection() {
  Bytes p;
  U32(p, 1);  // one import
  p.push_back(3);
  p.insert(p.end(), {'e', 'n', 'v'});
  p.push_back(3);
  p.insert(p.end(), {'i', 'm', 'p'});
  p.push_back(0x00);  // function import
  U32(p, 0);          // type index
  Bytes out;
  Section(out, 2, p);
  return out;
}

Bytes FunctionSection(Index count) {
  Bytes p;
  U32(p, count);
  for (Index i = 0; i < count; ++i) {
    U32(p, 0);
  }
  Bytes out;
  Section(out, 3, p);
  return out;
}

Bytes CodeSection(const std::vector<Bytes>& bodies) {
  Bytes p;
  U32(p, static_cast<uint32_t>(bodies.size()));
  for (const auto& body : bodies) {
    U32(p, static_cast<uint32_t>(body.size()));
    p.insert(p.end(), body.begin(), body.end());
  }
  Bytes out;
  Section(out, 10, p);
  return out;
}

// Main function body. Structured nodes in binary open order:
//   0 block / 1 loop / 2 if / 3 block (true arm) / 4 loop (false arm)
// and it exercises br, br_if, br_table (entries + default) and branches to
// the implicit function exit.
Bytes MainBody() {
  Bytes b;
  U32(b, 0);  // no local declarations

  b.push_back(kBlock);
  b.push_back(kVoidBlockType);
  b.push_back(kLoop);
  b.push_back(kVoidBlockType);
  b.push_back(kBr);
  U32(b, 1);  // -> outer block (node 0)
  b.push_back(kBr);
  U32(b, 2);  // -> function exit
  b.push_back(kEnd);
  b.push_back(kEnd);

  b.push_back(kI32Const);
  U32(b, 0);
  b.push_back(kIf);
  b.push_back(kVoidBlockType);

  b.push_back(kBlock);
  b.push_back(kVoidBlockType);
  b.push_back(kBrIf);
  U32(b, 0);  // -> true-arm block (node 3)
  b.push_back(kEnd);

  b.push_back(kElse);

  b.push_back(kLoop);
  b.push_back(kVoidBlockType);
  b.push_back(kI32Const);
  U32(b, 0);
  b.push_back(kBrTable);
  U32(b, 2);   // two explicit targets
  U32(b, 0);   // target 0 -> false-arm loop (node 4)
  U32(b, 1);   // target 1 -> if (node 2)
  U32(b, 2);   // default -> function exit
  b.push_back(kEnd);

  b.push_back(kEnd);  // end if
  b.push_back(kEnd);  // end function
  return b;
}

// Four sibling blocks; used for partial naming / auto-name collisions.
Bytes FourBlockBody() {
  Bytes b;
  U32(b, 0);
  for (int i = 0; i < 4; ++i) {
    b.push_back(kBlock);
    b.push_back(kVoidBlockType);
    b.push_back(kEnd);
  }
  b.push_back(kEnd);
  return b;
}

// Three nested blocks; used for duplicate debug names.
Bytes NestedBlockBody() {
  Bytes b;
  U32(b, 0);
  for (int i = 0; i < 3; ++i) {
    b.push_back(kBlock);
    b.push_back(kVoidBlockType);
  }
  for (int i = 0; i < 3; ++i) {
    b.push_back(kEnd);
  }
  b.push_back(kEnd);
  return b;
}

// A body with one block containing a branch; no debug names attached.
Bytes PlainBody() {
  Bytes b;
  U32(b, 0);
  b.push_back(kBlock);
  b.push_back(kVoidBlockType);
  b.push_back(kBr);
  U32(b, 0);
  b.push_back(kEnd);
  b.push_back(kEnd);
  return b;
}

// Like PlainBody but declares one i32 local, so local-name handling can be
// exercised alongside label names.
Bytes LocalBlockBody() {
  Bytes b;
  U32(b, 1);   // one local declaration group
  U32(b, 1);   // containing one local
  b.push_back(0x7f);  // i32
  b.push_back(kBlock);
  b.push_back(kVoidBlockType);
  b.push_back(kBr);
  U32(b, 0);
  b.push_back(kEnd);
  b.push_back(kEnd);
  return b;
}

Bytes ModuleHeader() {
  return {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
}

// Full module with one function import (func 0) and one local function
// (func 1). The name section can be placed before or after the code section.
Bytes MainModule(bool early_name, const Bytes& name_subsections) {
  Bytes b = ModuleHeader();
  b += TypeSection();
  b += ImportSection();
  b += FunctionSection(1);
  Bytes name = NameSection(name_subsections);
  if (early_name) {
    b += name;
  }
  b += CodeSection({MainBody()});
  if (!early_name) {
    b += name;
  }
  return b;
}

Bytes SingleFuncModule(const Bytes& body, const Bytes& name_subsections) {
  Bytes b = ModuleHeader();
  b += TypeSection();
  b += FunctionSection(1);
  b += CodeSection({body});
  if (!name_subsections.empty()) {
    b += NameSection(name_subsections);
  }
  return b;
}

Bytes MainLabelSubsection() {
  return LabelSubsection(
      {{1,
        {{0, "outerB"},
         {1, "innerL"},
         {2, "cond"},
         {3, "thenB"},
         {4, "elseL"}}}});
}

wabt::ReadBinaryOptions ReadOpts(bool debug, bool fail_custom = true) {
  wabt::ReadBinaryOptions opts;
  opts.read_debug_names = debug;
  opts.fail_on_custom_section_error = fail_custom;
  return opts;
}

bool ReadModule(const Bytes& bytes,
                wabt::Module* module,
                wabt::Errors* errors,
                bool debug = true,
                bool fail_custom = true) {
  return wabt::Succeeded(wabt::ReadBinaryIr(
      "task-label-names.wasm", bytes.data(), bytes.size(),
      ReadOpts(debug, fail_custom), errors, module));
}

std::string ToWat(const wabt::Module& module) {
  wabt::MemoryStream stream;
  if (wabt::Failed(
          wabt::WriteWat(&stream, &module, wabt::WriteWatOptions{}))) {
    return "<wat failed>";
  }
  const auto& data = stream.output_buffer().data;
  return std::string(data.begin(), data.end());
}

std::string FullPipelineWat(const Bytes& bytes) {
  wabt::Module module;
  wabt::Errors errors;
  if (!ReadModule(bytes, &module, &errors)) {
    return "<read failed>";
  }
  if (wabt::Failed(wabt::GenerateNames(&module)) ||
      wabt::Failed(wabt::ApplyNames(&module))) {
    return "<name failed>";
  }
  return ToWat(module);
}

// Branch target identity: the structured-node open index, or -1 for the
// implicit function exit. Collected in instruction order for br, br_if and
// every br_table entry followed by its default.
struct BranchWalk {
  std::vector<int> targets;
  std::vector<std::string> labels;  // block labels in open order
};

void WalkExprs(wabt::ExprList& exprs,
               std::vector<int>& label_stack,
               BranchWalk& walk);

void WalkExpr(wabt::Expr* expr,
              std::vector<int>& label_stack,
              BranchWalk& walk) {
  switch (expr->type()) {
    case wabt::ExprType::Block: {
      auto* e = wabt::cast<wabt::BlockExpr>(expr);
      Index idx = static_cast<Index>(walk.labels.size());
      walk.labels.push_back(e->block.label);
      label_stack.push_back(static_cast<int>(idx));
      WalkExprs(e->block.exprs, label_stack, walk);
      label_stack.pop_back();
      break;
    }
    case wabt::ExprType::Loop: {
      auto* e = wabt::cast<wabt::LoopExpr>(expr);
      Index idx = static_cast<Index>(walk.labels.size());
      walk.labels.push_back(e->block.label);
      label_stack.push_back(static_cast<int>(idx));
      WalkExprs(e->block.exprs, label_stack, walk);
      label_stack.pop_back();
      break;
    }
    case wabt::ExprType::If: {
      auto* e = wabt::cast<wabt::IfExpr>(expr);
      Index idx = static_cast<Index>(walk.labels.size());
      walk.labels.push_back(e->true_.label);
      label_stack.push_back(static_cast<int>(idx));
      WalkExprs(e->true_.exprs, label_stack, walk);
      // The if label stays in scope across the else arm.
      WalkExprs(e->false_, label_stack, walk);
      label_stack.pop_back();
      break;
    }
    case wabt::ExprType::Br: {
      auto* e = wabt::cast<wabt::BrExpr>(expr);
      Index depth = e->var.is_index() ? e->var.index() : 0;
      walk.targets.push_back(
          label_stack[label_stack.size() - static_cast<size_t>(depth) - 1]);
      break;
    }
    case wabt::ExprType::BrIf: {
      auto* e = wabt::cast<wabt::BrIfExpr>(expr);
      Index depth = e->var.is_index() ? e->var.index() : 0;
      walk.targets.push_back(
          label_stack[label_stack.size() - static_cast<size_t>(depth) - 1]);
      break;
    }
    case wabt::ExprType::BrTable: {
      auto* e = wabt::cast<wabt::BrTableExpr>(expr);
      for (const wabt::Var& var : e->targets) {
        Index depth = var.is_index() ? var.index() : 0;
        walk.targets.push_back(
            label_stack[label_stack.size() - static_cast<size_t>(depth) - 1]);
      }
      const wabt::Var& dflt = e->default_target;
      Index depth = dflt.is_index() ? dflt.index() : 0;
      walk.targets.push_back(
          label_stack[label_stack.size() - static_cast<size_t>(depth) - 1]);
      break;
    }
    default:
      break;
  }
}

void WalkExprs(wabt::ExprList& exprs,
               std::vector<int>& label_stack,
               BranchWalk& walk) {
  for (wabt::Expr& expr : exprs) {
    WalkExpr(&expr, label_stack, walk);
  }
}

BranchWalk WalkModule(wabt::Module* module) {
  BranchWalk walk;
  for (wabt::Func* func : module->funcs) {
    std::vector<int> label_stack;
    label_stack.push_back(-1);  // implicit function label / exit
    WalkExprs(func->exprs, label_stack, walk);
  }
  return walk;
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

// Parses WAT back into a module and resolves its name variables.
std::unique_ptr<wabt::Module> ReparseWat(const std::string& wat,
                                         wabt::Errors* errors) {
  auto lexer = wabt::WastLexer::CreateBufferLexer("roundtrip.wat", wat.data(),
                                                  wat.size(), errors);
  wabt::WastParseOptions options(wabt::Features{});
  std::unique_ptr<wabt::Module> module;
  if (wabt::Failed(
          wabt::ParseWatModule(lexer.get(), &module, errors, &options))) {
    return nullptr;
  }
  if (wabt::Failed(wabt::ResolveNamesModule(module.get(), errors))) {
    return nullptr;
  }
  return module;
}

// Full round trip: binary -> names -> WAT -> reparse -> resolve, then compare
// branch target identities with a fresh numeric read of the same bytes.
bool RoundTripPreservesTargets(const Bytes& bytes) {
  wabt::Module raw;
  wabt::Errors raw_errors;
  if (!ReadModule(bytes, &raw, &raw_errors)) {
    return false;
  }
  BranchWalk raw_walk = WalkModule(&raw);

  std::string wat = FullPipelineWat(bytes);
  if (wat == "<read failed>") {
    return false;
  }
  wabt::Errors parse_errors;
  auto reparsed = ReparseWat(wat, &parse_errors);
  if (!reparsed) {
    return false;
  }
  BranchWalk text_walk = WalkModule(reparsed.get());

  return raw_walk.targets == text_walk.targets &&
         raw_walk.labels.size() == text_walk.labels.size();
}

// ---------------------------------------------------------------------------
// decode group: byte-level decoding, ordering and validation rules.
// ---------------------------------------------------------------------------

int DecodeImportPrefixAndOpenOrder() {
  Bytes bytes = MainModule(false, MainLabelSubsection());
  wabt::Module module;
  wabt::Errors errors;
  TASK_CHECK(ReadModule(bytes, &module, &errors));
  TASK_CHECK(errors.empty());
  // Import prefix: two funcs, index 0 is the import and 1 is the local one.
  TASK_CHECK(module.funcs.size() == 2);
  TASK_CHECK(module.num_func_imports == 1);

  BranchWalk walk = WalkModule(&module);
  TASK_CHECK(walk.labels.size() == 5);
  TASK_CHECK(walk.labels[0] == "$outerB");
  TASK_CHECK(walk.labels[1] == "$innerL");
  TASK_CHECK(walk.labels[2] == "$cond");
  // True-arm node participates in the same sequence before the false-arm one.
  TASK_CHECK(walk.labels[3] == "$thenB");
  TASK_CHECK(walk.labels[4] == "$elseL");

  // br 1, br 2, br_if 0, br_table [0, 1], default 2.
  TASK_CHECK(walk.targets.size() == 6);
  TASK_CHECK(walk.targets[0] == 0);  // br 1 -> block node 0
  TASK_CHECK(walk.targets[1] == -1);  // br 2 -> function exit
  TASK_CHECK(walk.targets[2] == 3);  // br_if 0 -> true-arm block
  TASK_CHECK(walk.targets[3] == 4);  // table[0] -> false-arm loop
  TASK_CHECK(walk.targets[4] == 2);  // table[1] -> if
  TASK_CHECK(walk.targets[5] == -1);  // default -> function exit
  return 0;
}

int DecodeEarlyAndLateNameSectionAgree() {
  Bytes sub = MainLabelSubsection();
  wabt::Module late_module;
  wabt::Module early_module;
  wabt::Errors errors;
  TASK_CHECK(ReadModule(MainModule(false, sub), &late_module, &errors));
  TASK_CHECK(ReadModule(MainModule(true, sub), &early_module, &errors));
  TASK_CHECK(errors.empty());

  BranchWalk late = WalkModule(&late_module);
  BranchWalk early = WalkModule(&early_module);
  TASK_CHECK(late.labels == early.labels);
  TASK_CHECK(late.targets == early.targets);

  TASK_CHECK(wabt::Succeeded(wabt::GenerateNames(&late_module)));
  TASK_CHECK(wabt::Succeeded(wabt::ApplyNames(&late_module)));
  TASK_CHECK(wabt::Succeeded(wabt::GenerateNames(&early_module)));
  TASK_CHECK(wabt::Succeeded(wabt::ApplyNames(&early_module)));
  TASK_CHECK(ToWat(late_module) == ToWat(early_module));
  return 0;
}

int DecodeReadDebugNamesFalseIgnoresLabels() {
  Bytes bytes = MainModule(false, MainLabelSubsection());
  wabt::Module module;
  wabt::Errors errors;
  TASK_CHECK(ReadModule(bytes, &module, &errors, /*debug=*/false));
  BranchWalk walk = WalkModule(&module);
  TASK_CHECK(walk.labels.size() == 5);
  for (const auto& label : walk.labels) {
    TASK_CHECK(label.empty());
  }
  // Numeric depths remain usable and point at the same nodes.
  TASK_CHECK(walk.targets.size() == 6);
  TASK_CHECK(walk.targets[1] == -1);
  return 0;
}

int DecodePlainModuleWithoutNames() {
  Bytes bytes = SingleFuncModule(PlainBody(), {});
  wabt::Module module;
  wabt::Errors errors;
  TASK_CHECK(ReadModule(bytes, &module, &errors));
  TASK_CHECK(errors.empty());
  BranchWalk walk = WalkModule(&module);
  TASK_CHECK(walk.labels.size() == 1);
  TASK_CHECK(walk.labels[0].empty());
  TASK_CHECK(walk.targets.size() == 1);
  TASK_CHECK(walk.targets[0] == 0);

  // Existing function/local naming still works alongside label handling.
  Bytes named = SingleFuncModule(
      PlainBody(), FunctionNameSubsection({"the_func"}));
  wabt::Module named_module;
  wabt::Errors named_errors;
  TASK_CHECK(ReadModule(named, &named_module, &named_errors));
  TASK_CHECK(named_module.funcs[0]->name == "$the_func");
  return 0;
}

int DecodeEmptyNameNamesNothing() {
  Bytes sub = LabelSubsection({{0, {{0, ""}, {1, "kept"}}}});
  Bytes bytes = SingleFuncModule(FourBlockBody(), sub);
  wabt::Module module;
  wabt::Errors errors;
  TASK_CHECK(ReadModule(bytes, &module, &errors));
  BranchWalk walk = WalkModule(&module);
  TASK_CHECK(walk.labels[0].empty());  // empty string -> no name
  TASK_CHECK(walk.labels[1] == "$kept");
  return 0;
}

int DecodeFunctionAndLocalNamesStillWork() {
  // Function, local and label debug names are all read from the same name
  // section without interfering with one another.
  Bytes subsections;
  Append(subsections, FunctionNameSubsection({"the_func"}));
  Append(subsections, LocalNameSubsection(0, 0, "the_local"));
  Append(subsections, LabelSubsection({{0, {{0, "block"}}}}));
  Bytes bytes = SingleFuncModule(LocalBlockBody(), subsections);

  wabt::Module module;
  wabt::Errors errors;
  TASK_CHECK(ReadModule(bytes, &module, &errors));
  TASK_CHECK(errors.empty());
  TASK_CHECK(module.funcs[0]->name == "$the_func");
  TASK_CHECK(module.funcs[0]->bindings.FindIndex(std::string("$the_local")) ==
             0);

  BranchWalk walk = WalkModule(&module);
  TASK_CHECK(walk.labels.size() == 1);
  TASK_CHECK(walk.labels[0] == "$block");

  std::string wat = FullPipelineWat(bytes);
  TASK_CHECK(Contains(wat, "$the_func"));
  TASK_CHECK(Contains(wat, "$the_local"));
  TASK_CHECK(Contains(wat, "block $block"));
  return 0;
}

int DecodeDuplicateLabelIndexStrictAndLenient() {
  // Same label index twice in one function table.
  Bytes sub = LabelSubsection({{0, {{0, "a"}, {0, "b"}}}});
  Bytes bytes = SingleFuncModule(PlainBody(), sub);

  wabt::Module module;
  wabt::Errors errors;
  TASK_CHECK(!ReadModule(bytes, &module, &errors, true, true));
  TASK_CHECK(!errors.empty());
  bool positioned = false;
  bool mentioned = false;
  for (const auto& e : errors) {
    positioned |= e.loc.offset != wabt::kInvalidOffset;
    mentioned |= Contains(e.message, "duplicate label index");
  }
  TASK_CHECK(positioned);
  TASK_CHECK(mentioned);

  // Lenient mode downgrades custom-section problems to warnings.
  wabt::Module lenient;
  wabt::Errors lenient_errors;
  TASK_CHECK(ReadModule(bytes, &lenient, &lenient_errors, true, false));
  TASK_CHECK(!lenient_errors.empty());
  TASK_CHECK(lenient_errors[0].error_level == wabt::ErrorLevel::Warning);
  return 0;
}

int DecodeOutOfOrderLabelIndex() {
  Bytes sub = LabelSubsection({{0, {{1, "a"}, {0, "b"}}}});
  Bytes bytes = SingleFuncModule(FourBlockBody(), sub);
  wabt::Module module;
  wabt::Errors errors;
  TASK_CHECK(!ReadModule(bytes, &module, &errors));
  bool mentioned = false;
  for (const auto& e : errors) {
    mentioned |= Contains(e.message, "label index out of order");
  }
  TASK_CHECK(mentioned);
  return 0;
}

int DecodeTruncatedLabelName() {
  // Hand-built label subsection declaring a 5-byte name that is cut off.
  Bytes sub;
  sub.push_back(3);  // Label subsection id.
  Bytes payload;
  U32(payload, 1);  // one function
  U32(payload, 0);  // function index 0
  U32(payload, 1);  // one label
  U32(payload, 0);  // label index 0
  U32(payload, 5);  // claims five name bytes...
  payload.push_back('x');
  payload.push_back('y');  // ...only two present
  U32(sub, static_cast<uint32_t>(payload.size()));
  sub.insert(sub.end(), payload.begin(), payload.end());

  Bytes bytes = ModuleHeader();
  bytes += TypeSection();
  bytes += FunctionSection(1);
  bytes += CodeSection({PlainBody()});
  bytes += NameSection(sub);

  wabt::Module module;
  wabt::Errors errors;
  TASK_CHECK(!ReadModule(bytes, &module, &errors));
  bool mentioned = false;
  for (const auto& e : errors) {
    mentioned |= Contains(e.message, "unable to read string");
  }
  TASK_CHECK(mentioned);

  wabt::Module lenient;
  wabt::Errors lenient_errors;
  TASK_CHECK(ReadModule(bytes, &lenient, &lenient_errors, true, false));
  return 0;
}

int DecodeFunctionIndexOutOfRange() {
  Bytes sub = LabelSubsection({{9, {{0, "a"}}}});
  Bytes bytes = SingleFuncModule(PlainBody(), sub);
  wabt::Module module;
  wabt::Errors errors;
  TASK_CHECK(!ReadModule(bytes, &module, &errors));
  bool mentioned = false;
  for (const auto& e : errors) {
    mentioned |= Contains(e.message, "invalid function index");
  }
  TASK_CHECK(mentioned);
  return 0;
}

int DecodeDuplicateFunctionIndex() {
  // Two label groups for the same function index: a structural ordering
  // error reported with a position.
  Bytes payload;
  U32(payload, 2);  // two function groups
  U32(payload, 0);  // func index 0
  U32(payload, 1);  // one label
  U32(payload, 0);
  Str(payload, "a");
  U32(payload, 0);  // func index 0 again (duplicate)
  U32(payload, 1);
  U32(payload, 0);
  Str(payload, "b");
  Bytes sub;
  sub.push_back(3);
  U32(sub, static_cast<uint32_t>(payload.size()));
  sub.insert(sub.end(), payload.begin(), payload.end());
  Bytes bytes = SingleFuncModule(PlainBody(), sub);

  wabt::Module module;
  wabt::Errors errors;
  TASK_CHECK(!ReadModule(bytes, &module, &errors));
  bool positioned = false;
  bool mentioned = false;
  for (const auto& e : errors) {
    positioned |= e.loc.offset != wabt::kInvalidOffset;
    mentioned |= Contains(e.message, "duplicate label function index");
  }
  TASK_CHECK(positioned);
  TASK_CHECK(mentioned);
  return 0;
}

int DecodeLabelIndexOutOfRange() {
  // Structurally valid table, but the function only has one structured node.
  Bytes sub = LabelSubsection({{0, {{7, "far"}}}});
  Bytes bytes = SingleFuncModule(PlainBody(), sub);
  wabt::Module module;
  wabt::Errors errors;
  TASK_CHECK(!ReadModule(bytes, &module, &errors));
  bool positioned = false;
  bool mentioned = false;
  for (const auto& e : errors) {
    positioned |= e.loc.offset != wabt::kInvalidOffset;
    mentioned |= Contains(e.message, "invalid label index");
  }
  TASK_CHECK(positioned);
  TASK_CHECK(mentioned);

  // Lenient application skips the bad entry but keeps the module readable.
  wabt::Module lenient;
  wabt::Errors lenient_errors;
  TASK_CHECK(ReadModule(bytes, &lenient, &lenient_errors, true, false));
  TASK_CHECK(lenient_errors.empty());
  BranchWalk walk = WalkModule(&lenient);
  TASK_CHECK(walk.labels[0].empty());
  return 0;
}

int DecodeImportedFunctionLabelTableRejected() {
  // The import occupies function index 0 and must not carry label entries.
  Bytes sub = LabelSubsection({{0, {{0, "nope"}}}});
  Bytes bytes = MainModule(false, sub);
  wabt::Module module;
  wabt::Errors errors;
  TASK_CHECK(!ReadModule(bytes, &module, &errors));
  bool positioned = false;
  bool mentioned = false;
  for (const auto& e : errors) {
    positioned |= e.loc.offset != wabt::kInvalidOffset;
    mentioned |= Contains(e.message, "cannot have a non-empty label");
  }
  TASK_CHECK(positioned);
  TASK_CHECK(mentioned);

  wabt::Module lenient;
  wabt::Errors lenient_errors;
  TASK_CHECK(ReadModule(bytes, &lenient, &lenient_errors, true, false));
  return 0;
}

int DecodeFailureDoesNotAffectNextRead() {
  Bytes bad = SingleFuncModule(PlainBody(),
                               LabelSubsection({{0, {{9, "x"}}}}));
  wabt::Module bad_module;
  wabt::Errors bad_errors;
  TASK_CHECK(!ReadModule(bad, &bad_module, &bad_errors));

  // A fresh, independent read of a valid module still succeeds.
  wabt::Module good;
  wabt::Errors good_errors;
  TASK_CHECK(ReadModule(MainModule(false, MainLabelSubsection()), &good,
                        &good_errors));
  TASK_CHECK(good_errors.empty());
  BranchWalk walk = WalkModule(&good);
  TASK_CHECK(walk.labels.size() == 5);
  TASK_CHECK(walk.labels[0] == "$outerB");
  return 0;
}

// ---------------------------------------------------------------------------
// identity group: naming rules, disambiguation and lifetime.
// ---------------------------------------------------------------------------

int IdentityNestedDuplicateNames() {
  Bytes sub = LabelSubsection(
      {{0, {{0, "dup"}, {1, "dup"}, {2, "dup"}}}});
  Bytes bytes = SingleFuncModule(NestedBlockBody(), sub);
  wabt::Module module;
  wabt::Errors errors;
  TASK_CHECK(ReadModule(bytes, &module, &errors));
  BranchWalk walk = WalkModule(&module);
  TASK_CHECK(walk.labels.size() == 3);
  // First keeps the original name; later ones get the smallest free suffix.
  TASK_CHECK(walk.labels[0] == "$dup");
  TASK_CHECK(walk.labels[1] == "$dup.1");
  TASK_CHECK(walk.labels[2] == "$dup.2");
  return 0;
}

int IdentityPartialNamingAndAutoCollision() {
  // Node 1 borrows the name that auto-generation would otherwise hand to
  // node 0 ("$B0"); node 2 has a debug name; nodes 0 and 3 are unnamed.
  Bytes sub = LabelSubsection(
      {{0, {{1, "B0"}, {2, "z"}}}});
  Bytes bytes = SingleFuncModule(FourBlockBody(), sub);
  wabt::Module module;
  wabt::Errors errors;
  TASK_CHECK(ReadModule(bytes, &module, &errors));
  TASK_CHECK(wabt::Succeeded(wabt::GenerateNames(&module)));

  BranchWalk walk = WalkModule(&module);
  TASK_CHECK(walk.labels.size() == 4);
  // The debug name is preserved verbatim and blocks the automatic name.
  TASK_CHECK(walk.labels[1] == "$B0");
  TASK_CHECK(walk.labels[2] == "$z");
  TASK_CHECK(!walk.labels[0].empty());
  TASK_CHECK(walk.labels[0] != "$B0");
  TASK_CHECK(walk.labels[0] == "$B0_1");
  TASK_CHECK(walk.labels[3] == "$B3");

  // Every label in the function stays unique.
  std::vector<std::string> sorted = walk.labels;
  std::sort(sorted.begin(), sorted.end());
  TASK_CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
  return 0;
}

int IdentityAutoNameAvoidsLaterDebugName() {
  // The colliding debug name sits on a node visited *after* the unnamed one,
  // so the reservation must be computed before generation starts.
  Bytes sub = LabelSubsection({{0, {{2, "B0"}}}});
  Bytes bytes = SingleFuncModule(FourBlockBody(), sub);
  wabt::Module module;
  wabt::Errors errors;
  TASK_CHECK(ReadModule(bytes, &module, &errors));
  TASK_CHECK(wabt::Succeeded(wabt::GenerateNames(&module)));
  BranchWalk walk = WalkModule(&module);
  TASK_CHECK(walk.labels[2] == "$B0");
  TASK_CHECK(walk.labels[0] != "$B0");
  TASK_CHECK(!Contains(walk.labels[0], "."));  // auto uses its own _N scheme
  return 0;
}

int IdentityNamingSurvivesInputRelease() {
  wabt::Module module;
  wabt::Errors errors;
  {
    Bytes bytes = MainModule(false, MainLabelSubsection());
    TASK_CHECK(ReadModule(bytes, &module, &errors));
  }
  // The input byte vector is gone; copied names must still drive output.
  TASK_CHECK(errors.empty());
  TASK_CHECK(wabt::Succeeded(wabt::GenerateNames(&module)));
  TASK_CHECK(wabt::Succeeded(wabt::ApplyNames(&module)));
  std::string wat = ToWat(module);
  TASK_CHECK(Contains(wat, "$outerB"));
  TASK_CHECK(Contains(wat, "$innerL"));
  TASK_CHECK(Contains(wat, "$elseL"));
  return 0;
}

int IdentityNoGenerationKeepsNumericDepths() {
  // Without GenerateNames, unnamed nodes stay unnamed and numeric br depths
  // are still printed and usable.
  Bytes bytes = SingleFuncModule(PlainBody(), {});
  wabt::Module module;
  wabt::Errors errors;
  TASK_CHECK(ReadModule(bytes, &module, &errors));
  TASK_CHECK(wabt::Succeeded(wabt::ApplyNames(&module)));
  std::string wat = ToWat(module);
  TASK_CHECK(Contains(wat, "br 0"));
  TASK_CHECK(!Contains(wat, "$B0"));
  return 0;
}

int IdentityCharacterConversionHabit() {
  // Debug names use the same "$" prefix and invalid-char rewriting as the
  // existing function/local names.
  Bytes sub = LabelSubsection({{0, {{0, "a b"}}}});
  Bytes bytes = SingleFuncModule(PlainBody(), sub);
  wabt::Module module;
  wabt::Errors errors;
  TASK_CHECK(ReadModule(bytes, &module, &errors));
  BranchWalk walk = WalkModule(&module);
  TASK_CHECK(walk.labels[0] == "$a b");
  std::string wat = ToWat(module);
  TASK_CHECK(Contains(wat, "$a_b"));
  return 0;
}

// ---------------------------------------------------------------------------
// integration group: full ReadBinaryIr -> Generate/Apply -> WriteWat ->
// reparse -> ResolveNames pipeline and branch target identity.
// ---------------------------------------------------------------------------

int IntegrationMainRoundTrip() {
  Bytes bytes = MainModule(false, MainLabelSubsection());
  std::string wat = FullPipelineWat(bytes);
  TASK_CHECK(Contains(wat, "block $outerB"));
  TASK_CHECK(Contains(wat, "loop $innerL"));
  TASK_CHECK(Contains(wat, "if $cond"));
  TASK_CHECK(Contains(wat, "block $thenB"));
  TASK_CHECK(Contains(wat, "loop $elseL"));
  // Function-exit branches remain numeric and are not rebound.
  TASK_CHECK(Contains(wat, "br 2"));
  TASK_CHECK(Contains(wat, "br_table $elseL $cond 2"));
  TASK_CHECK(Contains(wat, "br_if $thenB"));
  TASK_CHECK(Contains(wat, "br $outerB"));

  wabt::Errors errors;
  auto reparsed = ReparseWat(wat, &errors);
  TASK_CHECK(reparsed != nullptr);
  TASK_CHECK(errors.empty());

  TASK_CHECK(RoundTripPreservesTargets(bytes));
  return 0;
}

int IntegrationEarlyNameSectionRoundTrip() {
  Bytes bytes = MainModule(true, MainLabelSubsection());
  std::string wat = FullPipelineWat(bytes);
  TASK_CHECK(Contains(wat, "$outerB"));
  TASK_CHECK(RoundTripPreservesTargets(bytes));

  // Both name-section positions yield the same textual module.
  TASK_CHECK(wat == FullPipelineWat(MainModule(false, MainLabelSubsection())));
  return 0;
}

int IntegrationPartialNamingRoundTrip() {
  Bytes sub = LabelSubsection({{0, {{1, "B0"}, {2, "z"}}}});
  Bytes bytes = SingleFuncModule(FourBlockBody(), sub);
  std::string wat = FullPipelineWat(bytes);
  TASK_CHECK(Contains(wat, "$B0"));
  TASK_CHECK(Contains(wat, "$z"));

  wabt::Errors errors;
  auto reparsed = ReparseWat(wat, &errors);
  TASK_CHECK(reparsed != nullptr);
  TASK_CHECK(errors.empty());
  TASK_CHECK(RoundTripPreservesTargets(bytes));
  return 0;
}

int IntegrationDuplicateNamesRoundTrip() {
  Bytes sub = LabelSubsection(
      {{0, {{0, "dup"}, {1, "dup"}, {2, "dup"}}}});
  Bytes bytes = SingleFuncModule(NestedBlockBody(), sub);
  std::string wat = FullPipelineWat(bytes);
  TASK_CHECK(Contains(wat, "block $dup\n"));
  TASK_CHECK(Contains(wat, "$dup.1"));
  TASK_CHECK(Contains(wat, "$dup.2"));

  wabt::Errors errors;
  auto reparsed = ReparseWat(wat, &errors);
  TASK_CHECK(reparsed != nullptr);
  TASK_CHECK(RoundTripPreservesTargets(bytes));
  return 0;
}

int IntegrationPlainModuleRoundTrip() {
  // Unnamed control stays fully usable through the whole pipeline.
  Bytes bytes = SingleFuncModule(PlainBody(), {});
  std::string wat = FullPipelineWat(bytes);
  TASK_CHECK(Contains(wat, "block $B0"));
  TASK_CHECK(Contains(wat, "br $B0"));
  TASK_CHECK(RoundTripPreservesTargets(bytes));
  return 0;
}

int IntegrationErrorThenValidRoundTrip() {
  Bytes bad = SingleFuncModule(PlainBody(),
                               LabelSubsection({{0, {{9, "x"}}}}));
  wabt::Module bad_module;
  wabt::Errors bad_errors;
  TASK_CHECK(!ReadModule(bad, &bad_module, &bad_errors));

  // The next independent module still runs the full pipeline correctly.
  Bytes good = MainModule(false, MainLabelSubsection());
  std::string wat = FullPipelineWat(good);
  TASK_CHECK(Contains(wat, "$outerB"));
  TASK_CHECK(RoundTripPreservesTargets(good));
  return 0;
}

}  // namespace

#define TASK_CASE(group, name, fn) \
  { group, name, fn }

const struct task_case task_cases[] = {
    TASK_CASE("decode", "import_prefix_and_open_order",
              DecodeImportPrefixAndOpenOrder),
    TASK_CASE("decode", "early_and_late_name_section_agree",
              DecodeEarlyAndLateNameSectionAgree),
    TASK_CASE("decode", "read_debug_names_false_ignores_labels",
              DecodeReadDebugNamesFalseIgnoresLabels),
    TASK_CASE("decode", "plain_module_without_names",
              DecodePlainModuleWithoutNames),
    TASK_CASE("decode", "function_and_local_names_still_work",
              DecodeFunctionAndLocalNamesStillWork),
    TASK_CASE("decode", "empty_name_names_nothing", DecodeEmptyNameNamesNothing),
    TASK_CASE("decode", "duplicate_label_index",
              DecodeDuplicateLabelIndexStrictAndLenient),
    TASK_CASE("decode", "out_of_order_label_index", DecodeOutOfOrderLabelIndex),
    TASK_CASE("decode", "truncated_label_name", DecodeTruncatedLabelName),
    TASK_CASE("decode", "function_index_out_of_range",
              DecodeFunctionIndexOutOfRange),
    TASK_CASE("decode", "duplicate_function_index",
              DecodeDuplicateFunctionIndex),
    TASK_CASE("decode", "label_index_out_of_range",
              DecodeLabelIndexOutOfRange),
    TASK_CASE("decode", "imported_function_label_table_rejected",
              DecodeImportedFunctionLabelTableRejected),
    TASK_CASE("decode", "failure_does_not_affect_next_read",
              DecodeFailureDoesNotAffectNextRead),

    TASK_CASE("identity", "nested_duplicate_names",
              IdentityNestedDuplicateNames),
    TASK_CASE("identity", "partial_naming_and_auto_collision",
              IdentityPartialNamingAndAutoCollision),
    TASK_CASE("identity", "auto_name_avoids_later_debug_name",
              IdentityAutoNameAvoidsLaterDebugName),
    TASK_CASE("identity", "naming_survives_input_release",
              IdentityNamingSurvivesInputRelease),
    TASK_CASE("identity", "no_generation_keeps_numeric_depths",
              IdentityNoGenerationKeepsNumericDepths),
    TASK_CASE("identity", "character_conversion_habit",
              IdentityCharacterConversionHabit),

    TASK_CASE("integration", "main_round_trip", IntegrationMainRoundTrip),
    TASK_CASE("integration", "early_name_section_round_trip",
              IntegrationEarlyNameSectionRoundTrip),
    TASK_CASE("integration", "partial_naming_round_trip",
              IntegrationPartialNamingRoundTrip),
    TASK_CASE("integration", "duplicate_names_round_trip",
              IntegrationDuplicateNamesRoundTrip),
    TASK_CASE("integration", "plain_module_round_trip",
              IntegrationPlainModuleRoundTrip),
    TASK_CASE("integration", "error_then_valid_round_trip",
              IntegrationErrorThenValidRoundTrip),
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
