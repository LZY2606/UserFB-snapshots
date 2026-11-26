#include "task_support/check.h"
#include "task_support/fixture.h"

#include "wabt/apply-names.h"
#include "wabt/binary-reader-ir.h"
#include "wabt/binary-reader.h"
#include "wabt/cast.h"
#include "wabt/generate-names.h"
#include "wabt/ir.h"
#include "wabt/resolve-names.h"
#include "wabt/stream.h"
#include "wabt/wast-lexer.h"
#include "wabt/wast-parser.h"
#include "wabt/wat-writer.h"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using fixture::Bytes;
using wabt::ApplyNames;
using wabt::BrExpr;
using wabt::BrIfExpr;
using wabt::BrTableExpr;
using wabt::Errors;
using wabt::Expr;
using wabt::ExprList;
using wabt::ExprType;
using wabt::Failed;
using wabt::Func;
using wabt::GenerateNames;
using wabt::IfExpr;
using wabt::Index;
using wabt::MemoryStream;
using wabt::Module;
using wabt::ParseWatModule;
using wabt::ReadBinaryIr;
using wabt::ReadBinaryOptions;
using wabt::ResolveNamesModule;
using wabt::Result;
using wabt::Succeeded;
using wabt::Var;
using wabt::WastLexer;
using wabt::WastParseOptions;
using wabt::WriteWat;
using wabt::WriteWatOptions;
using wabt::cast;

namespace {

constexpr Index kExitIdentity = std::numeric_limits<Index>::max() - 1;
constexpr Index kBadIdentity = std::numeric_limits<Index>::max();

struct LabelEntry {
  Index index;
  std::string name;
};

struct LabelGroup {
  Index func_index;
  std::vector<LabelEntry> labels;
};

void AppendRaw(Bytes& out, std::initializer_list<uint8_t> bytes) {
  out.insert(out.end(), bytes.begin(), bytes.end());
}

Bytes NameSubsection(uint8_t subsection_type, const Bytes& payload) {
  Bytes out;
  out.push_back(subsection_type);
  fixture::u32(out, static_cast<uint32_t>(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

Bytes LabelSubsection(const std::vector<LabelGroup>& groups) {
  Bytes payload;
  fixture::u32(payload, static_cast<uint32_t>(groups.size()));
  for (const LabelGroup& group : groups) {
    fixture::u32(payload, group.func_index);
    fixture::u32(payload, static_cast<uint32_t>(group.labels.size()));
    for (const LabelEntry& label : group.labels) {
      fixture::u32(payload, label.index);
      fixture::str(payload, label.name);
    }
  }
  return NameSubsection(3, payload);
}

Bytes CustomNameSection(const Bytes& subsections) {
  Bytes name_payload;
  fixture::str(name_payload, "name");
  name_payload.insert(name_payload.end(), subsections.begin(),
                      subsections.end());
  Bytes section;
  fixture::section(section, 0, name_payload);
  return section;
}

Bytes ModulePrefix(bool with_import) {
  Bytes out = {0, 'a', 's', 'm', 1, 0, 0, 0};

  Bytes type_payload = {1, 0x60, 0, 0};
  fixture::section(out, 1, type_payload);

  if (with_import) {
    Bytes import_payload;
    fixture::u32(import_payload, 1);
    fixture::str(import_payload, "m");
    fixture::str(import_payload, "f");
    import_payload.push_back(0x00);  // external function
    fixture::u32(import_payload, 0);
    fixture::section(out, 2, import_payload);
  }

  Bytes function_payload = {1, 0};  // one local function using type 0
  fixture::section(out, 3, function_payload);
  return out;
}

Bytes CodeSection(const Bytes& body) {
  Bytes entry;
  fixture::u32(entry, static_cast<uint32_t>(body.size()));
  entry.insert(entry.end(), body.begin(), body.end());

  Bytes code_payload;
  fixture::u32(code_payload, 1);
  code_payload.insert(code_payload.end(), entry.begin(), entry.end());

  Bytes section;
  fixture::section(section, 10, code_payload);
  return section;
}

Bytes MakeModule(const Bytes& body,
                 const Bytes& name_subsections,
                 bool early,
                 bool with_import = true) {
  Bytes out = ModulePrefix(with_import);
  Bytes name_section = CustomNameSection(name_subsections);
  if (early) {
    out.insert(out.end(), name_section.begin(), name_section.end());
  }
  Bytes code = CodeSection(body);
  out.insert(out.end(), code.begin(), code.end());
  if (!early) {
    out.insert(out.end(), name_section.begin(), name_section.end());
  }
  return out;
}

Result ReadBytes(const Bytes& bytes,
                 Module* module,
                 Errors* errors,
                 bool read_debug_names = true,
                 bool fail_on_custom_section_error = true) {
  ReadBinaryOptions options;
  options.read_debug_names = read_debug_names;
  options.fail_on_custom_section_error = fail_on_custom_section_error;
  return ReadBinaryIr("task-label-names.wasm", bytes.data(), bytes.size(),
                      options, errors, module);
}

void CollectLabels(ExprList& exprs, std::vector<std::string*>* labels) {
  for (Expr& expr : exprs) {
    switch (expr.type()) {
      case ExprType::Block: {
        auto& block_expr = *cast<wabt::BlockExpr>(&expr);
        labels->push_back(&block_expr.block.label);
        CollectLabels(block_expr.block.exprs, labels);
        break;
      }
      case ExprType::Loop: {
        auto& loop_expr = *cast<wabt::LoopExpr>(&expr);
        labels->push_back(&loop_expr.block.label);
        CollectLabels(loop_expr.block.exprs, labels);
        break;
      }
      case ExprType::If: {
        auto& if_expr = *cast<IfExpr>(&expr);
        labels->push_back(&if_expr.true_.label);
        CollectLabels(if_expr.true_.exprs, labels);
        CollectLabels(if_expr.false_, labels);
        break;
      }
      default:
        break;
    }
  }
}

std::vector<std::string*> CollectLabels(Func* func) {
  std::vector<std::string*> labels;
  CollectLabels(func->exprs, &labels);
  return labels;
}

struct BranchItem {
  Var* var;
  Index depth;
  Index target;
  bool is_default;
};

struct Topology {
  std::vector<std::string*> labels;
  std::vector<BranchItem> branches;
};

Index TargetForDepth(Index depth, const std::vector<Index>& stack) {
  if (depth < stack.size()) {
    return stack[stack.size() - 1 - depth];
  }
  if (depth == stack.size()) {
    return kExitIdentity;
  }
  return kBadIdentity;
}

void AddBranchItem(Var* var,
                   bool is_default,
                   std::vector<Index>* stack,
                   Topology* topology) {
  Index depth = var->index();
  topology->branches.push_back(
      {var, depth, TargetForDepth(depth, *stack), is_default});
}

void AnalyzeExprs(ExprList& exprs,
                  std::vector<Index>* stack,
                  Topology* topology) {
  for (Expr& expr : exprs) {
    switch (expr.type()) {
      case ExprType::Block: {
        auto& block_expr = *cast<wabt::BlockExpr>(&expr);
        Index id = static_cast<Index>(topology->labels.size());
        topology->labels.push_back(&block_expr.block.label);
        stack->push_back(id);
        AnalyzeExprs(block_expr.block.exprs, stack, topology);
        stack->pop_back();
        break;
      }
      case ExprType::Loop: {
        auto& loop_expr = *cast<wabt::LoopExpr>(&expr);
        Index id = static_cast<Index>(topology->labels.size());
        topology->labels.push_back(&loop_expr.block.label);
        stack->push_back(id);
        AnalyzeExprs(loop_expr.block.exprs, stack, topology);
        stack->pop_back();
        break;
      }
      case ExprType::If: {
        auto& if_expr = *cast<IfExpr>(&expr);
        Index id = static_cast<Index>(topology->labels.size());
        topology->labels.push_back(&if_expr.true_.label);
        stack->push_back(id);
        AnalyzeExprs(if_expr.true_.exprs, stack, topology);
        AnalyzeExprs(if_expr.false_, stack, topology);
        stack->pop_back();
        break;
      }
      case ExprType::Br:
        AddBranchItem(&cast<BrExpr>(&expr)->var, false, stack, topology);
        break;
      case ExprType::BrIf:
        AddBranchItem(&cast<BrIfExpr>(&expr)->var, false, stack, topology);
        break;
      case ExprType::BrTable: {
        auto& br_table = *cast<BrTableExpr>(&expr);
        for (Var& target : br_table.targets) {
          AddBranchItem(&target, false, stack, topology);
        }
        AddBranchItem(&br_table.default_target, true, stack, topology);
        break;
      }
      default:
        break;
    }
  }
}

Topology Analyze(Func* func) {
  Topology topology;
  std::vector<Index> stack;
  AnalyzeExprs(func->exprs, &stack, &topology);
  return topology;
}

std::string WriteModuleToWat(Module* module) {
  MemoryStream stream;
  WriteWatOptions options;
  Result result = WriteWat(&stream, module, options);
  if (Failed(result)) {
    return {};
  }
  const auto& data = stream.output_buffer().data;
  return std::string(data.begin(), data.end());
}

std::unique_ptr<Module> ParseWat(const std::string& wat, Errors* errors) {
  auto lexer = WastLexer::CreateBufferLexer(
      "task.wat", wat.data(), wat.size(), errors);
  if (!lexer) {
    return nullptr;
  }
  WastParseOptions options(wabt::Features{});
  std::unique_ptr<Module> module;
  Result result = ParseWatModule(lexer.get(), &module, errors, &options);
  if (Failed(result)) {
    return nullptr;
  }
  if (Failed(ResolveNamesModule(module.get(), errors))) {
    return nullptr;
  }
  return module;
}

bool ContainsPositionedError(const Errors& errors) {
  for (const wabt::Error& error : errors) {
    if (error.error_level == wabt::ErrorLevel::Error &&
        error.loc.offset != wabt::kInvalidOffset) {
      return true;
    }
  }
  return false;
}

// block label 0, if label 1, then-arm block label 2, else-arm block label 3.
Bytes TwoArmBody() {
  Bytes body = {0x00};
  AppendRaw(body, {0x02, 0x40});             // block 0
  AppendRaw(body, {0x04, 0x40});             // if 1
  AppendRaw(body, {0x02, 0x40, 0x0c, 0x00, 0x0b});  // then block 2, br 0, end
  body.push_back(0x05);                      // else
  AppendRaw(body, {0x02, 0x40, 0x0c, 0x00, 0x0b});  // else block 3
  AppendRaw(body, {0x0b, 0x0b, 0x0b});  // if, outer block, function
  return body;
}

Bytes TwoArmNames() {
  return LabelSubsection({{1,
                           {{0, "root"},
                            {1, "choice"},
                            {2, "then_inner"},
                            {3, "else_inner"}}}});
}

// Three nested blocks.  br_if and br_table use depths 0..3; depth 3 is the
// implicit function exit.
Bytes NestedBranchBody() {
  Bytes body = {0x00};
  AppendRaw(body, {0x02, 0x40});  // label 0
  AppendRaw(body, {0x02, 0x40});  // label 1
  AppendRaw(body, {0x02, 0x40});  // label 2
  AppendRaw(body, {0x0d, 0x01});  // br_if depth 1 -> label 1
  AppendRaw(body, {0x0e, 0x03, 0x00, 0x01, 0x02, 0x03});
  body.push_back(0x0b);           // end label 2
  AppendRaw(body, {0x0c, 0x00});  // br 0 -> label 1
  body.push_back(0x0b);           // end label 1
  AppendRaw(body, {0x0c, 0x00});  // br 0 -> label 0
  body.push_back(0x0b);           // end label 0
  body.push_back(0x0b);           // end function
  return body;
}

Bytes NestedNames() {
  return LabelSubsection(
      {{1, {{0, "same"}, {1, "same"}, {2, "same"}}}});
}

Bytes AutoConflictBody() {
  return {
      0x00,
      0x02, 0x40, 0x0b,              // unnamed block, would be $B0
      0x02, 0x40, 0x0c, 0x00, 0x0b,  // debug $B0 block and br 0
      0x0b,
  };
}

Bytes AutoConflictNames() {
  return LabelSubsection({{1, {{1, "B0"}}}});
}

int CheckTwoArmRead(bool early) {
  Bytes bytes = MakeModule(TwoArmBody(), TwoArmNames(), early);
  Module module;
  Errors errors;
  Result read_result = ReadBytes(bytes, &module, &errors);
  if (Failed(read_result)) {
    for (const wabt::Error& error : errors) {
      fprintf(stderr, "read error offset=%zu: %s\n", error.loc.offset,
              error.message.c_str());
    }
  }
  TASK_CHECK(Succeeded(read_result));
  TASK_CHECK(errors.empty());
  TASK_CHECK(module.num_func_imports == 1);
  TASK_CHECK(module.funcs.size() == 2);
  std::vector<std::string*> labels = CollectLabels(module.funcs[1]);
  TASK_CHECK(labels.size() == 4);
  TASK_CHECK(*labels[0] == "$root");
  TASK_CHECK(*labels[1] == "$choice");
  TASK_CHECK(*labels[2] == "$then_inner");
  TASK_CHECK(*labels[3] == "$else_inner");
  return 0;
}

int CheckMalformedRead(const Bytes& label_payload) {
  Bytes subs = NameSubsection(3, label_payload);
  Bytes bytes = MakeModule({0x00, 0x0b}, subs, false);
  Module module;
  Errors errors;
  Result result = ReadBytes(bytes, &module, &errors);
  TASK_CHECK(Failed(result));
  TASK_CHECK(ContainsPositionedError(errors));

  // A failed independent read must not poison later reads.
  Module good_module;
  Errors good_errors;
  Bytes good = MakeModule(TwoArmBody(), TwoArmNames(), true);
  TASK_CHECK(Succeeded(ReadBytes(good, &good_module, &good_errors)));
  TASK_CHECK(good_errors.empty());
  TASK_CHECK(CollectLabels(good_module.funcs[1]).size() == 4);
  return 0;
}

int CheckWatRoundtrip(const Bytes& bytes) {
  Module module;
  Errors errors;
  TASK_CHECK(Succeeded(ReadBytes(bytes, &module, &errors)));
  TASK_CHECK(errors.empty());

  Topology original = Analyze(module.funcs[1]);
  TASK_CHECK(original.labels.size() >= 2);
  for (const BranchItem& item : original.branches) {
    TASK_CHECK(item.var->is_index());
    TASK_CHECK(item.target != kBadIdentity);
  }

  TASK_CHECK(Succeeded(GenerateNames(&module)));
  TASK_CHECK(Succeeded(ApplyNames(&module)));

  for (const BranchItem& item : original.branches) {
    if (item.target == kExitIdentity) {
      TASK_CHECK(item.var->is_index());
      TASK_CHECK(item.var->index() == item.depth);
    } else {
      TASK_CHECK(item.var->is_name());
      TASK_CHECK(item.target < original.labels.size());
      TASK_CHECK(item.var->name() == *original.labels[item.target]);
    }
  }

  std::string wat = WriteModuleToWat(&module);
  TASK_CHECK(!wat.empty());
  TASK_CHECK(wat.find("block $") != std::string::npos);

  Errors parse_errors;
  std::unique_ptr<Module> reparsed = ParseWat(wat, &parse_errors);
  if (!reparsed) {
    for (const wabt::Error& error : parse_errors) {
      fprintf(stderr, "reparse error: %s\n", error.message.c_str());
    }
  }
  TASK_CHECK(reparsed != nullptr);
  TASK_CHECK(Succeeded(ResolveNamesModule(reparsed.get(), &parse_errors)));
  TASK_CHECK(parse_errors.empty());
  TASK_CHECK(reparsed->funcs.size() == module.funcs.size());

  Topology roundtrip = Analyze(reparsed->funcs[1]);
  TASK_CHECK(roundtrip.labels.size() == original.labels.size());
  TASK_CHECK(roundtrip.branches.size() == original.branches.size());
  for (size_t i = 0; i < original.branches.size(); ++i) {
    const BranchItem& before = original.branches[i];
    const BranchItem& after = roundtrip.branches[i];
    TASK_CHECK(after.var->is_index());
    TASK_CHECK(after.depth == before.depth);
    TASK_CHECK(after.target == before.target);
    if (before.target != kExitIdentity) {
      TASK_CHECK(*roundtrip.labels[after.target] ==
                 *original.labels[before.target]);
    }
  }
  return 0;
}

}  // namespace

int decode_label_names(void) {
  TASK_CHECK(CheckTwoArmRead(true) == 0);
  TASK_CHECK(CheckTwoArmRead(false) == 0);

  // Function and local debug names retain their established behavior.
  Bytes function_name_payload;
  fixture::u32(function_name_payload, 1);
  fixture::u32(function_name_payload, 1);
  fixture::str(function_name_payload, "named_func");
  Bytes local_name_payload;
  fixture::u32(local_name_payload, 1);
  fixture::u32(local_name_payload, 1);
  fixture::u32(local_name_payload, 1);
  fixture::u32(local_name_payload, 0);
  fixture::str(local_name_payload, "named_local");
  Bytes name_subs = NameSubsection(1, function_name_payload);
  Bytes local_sub = NameSubsection(2, local_name_payload);
  name_subs.insert(name_subs.end(), local_sub.begin(), local_sub.end());
  Bytes body_with_local = {0x01, 0x01, 0x7f, 0x0b};
  Bytes named_bytes = MakeModule(body_with_local, name_subs, false);
  Module named_module;
  Errors named_errors;
  TASK_CHECK(Succeeded(ReadBytes(named_bytes, &named_module, &named_errors)));
  TASK_CHECK(named_errors.empty());
  TASK_CHECK(named_module.funcs[1]->name == "$named_func");
  TASK_CHECK(named_module.funcs[1]->bindings.count("$named_local") == 1);

  // read_debug_names=false preserves the old ignore-label behavior.
  Module ignored_module;
  Errors ignored_errors;
  Bytes ignored_bytes = MakeModule(TwoArmBody(), TwoArmNames(), false);
  TASK_CHECK(Succeeded(ReadBytes(ignored_bytes, &ignored_module,
                                 &ignored_errors, false)));
  TASK_CHECK(ignored_errors.empty());
  for (const std::string* label : CollectLabels(ignored_module.funcs[1])) {
    TASK_CHECK(label->empty());
  }
  TASK_CHECK(Succeeded(ApplyNames(&ignored_module)));
  Topology ignored_topology = Analyze(ignored_module.funcs[1]);
  for (const BranchItem& item : ignored_topology.branches) {
    TASK_CHECK(item.var->is_index());
  }

  // An ordinary unnamed module is readable on its own.
  Module plain_module;
  Errors plain_errors;
  Bytes plain_bytes = ModulePrefix(false);
  Bytes plain_body = {0x00, 0x02, 0x40, 0x03, 0x40, 0x0c, 0x01,
                      0x0b, 0x0b, 0x0b};
  Bytes plain_code = CodeSection(plain_body);
  plain_bytes.insert(plain_bytes.end(), plain_code.begin(), plain_code.end());
  TASK_CHECK(Succeeded(ReadBytes(plain_bytes, &plain_module, &plain_errors)));
  TASK_CHECK(plain_errors.empty());
  TASK_CHECK(CollectLabels(plain_module.funcs[0]).size() == 2);
  return 0;
}

int decode_label_errors(void) {
  // Duplicate and reversed function-group indices.
  Bytes duplicate_funcs;
  fixture::u32(duplicate_funcs, 2);
  fixture::u32(duplicate_funcs, 1);
  fixture::u32(duplicate_funcs, 0);
  fixture::u32(duplicate_funcs, 1);
  fixture::u32(duplicate_funcs, 0);
  TASK_CHECK(CheckMalformedRead(duplicate_funcs) == 0);

  Bytes reversed_funcs;
  fixture::u32(reversed_funcs, 2);
  fixture::u32(reversed_funcs, 1);
  fixture::u32(reversed_funcs, 0);
  fixture::u32(reversed_funcs, 0);
  fixture::u32(reversed_funcs, 0);
  TASK_CHECK(CheckMalformedRead(reversed_funcs) == 0);

  // Duplicate and reversed label indices.
  Bytes duplicate_labels;
  fixture::u32(duplicate_labels, 1);
  fixture::u32(duplicate_labels, 1);
  fixture::u32(duplicate_labels, 2);
  fixture::u32(duplicate_labels, 0);
  fixture::str(duplicate_labels, "a");
  fixture::u32(duplicate_labels, 0);
  fixture::str(duplicate_labels, "b");
  TASK_CHECK(CheckMalformedRead(duplicate_labels) == 0);

  Bytes reversed_labels;
  fixture::u32(reversed_labels, 1);
  fixture::u32(reversed_labels, 1);
  fixture::u32(reversed_labels, 2);
  fixture::u32(reversed_labels, 1);
  fixture::str(reversed_labels, "b");
  fixture::u32(reversed_labels, 0);
  fixture::str(reversed_labels, "a");
  TASK_CHECK(CheckMalformedRead(reversed_labels) == 0);

  // Truncated UTF-8 name payload (declared length five, two bytes present).
  Bytes truncated_name;
  fixture::u32(truncated_name, 1);
  fixture::u32(truncated_name, 1);
  fixture::u32(truncated_name, 1);
  fixture::u32(truncated_name, 0);
  fixture::u32(truncated_name, 5);
  truncated_name.push_back('a');
  truncated_name.push_back('b');
  TASK_CHECK(CheckMalformedRead(truncated_name) == 0);

  // Out-of-bounds function index.
  Bytes bad_func;
  fixture::u32(bad_func, 1);
  fixture::u32(bad_func, 99);
  fixture::u32(bad_func, 0);
  TASK_CHECK(CheckMalformedRead(bad_func) == 0);

  // Out-of-bounds label index is checked against the opened block/loop/if
  // sequence after code has been read.
  Bytes bad_label;
  fixture::u32(bad_label, 1);
  fixture::u32(bad_label, 1);
  fixture::u32(bad_label, 1);
  fixture::u32(bad_label, 99);
  fixture::str(bad_label, "too_late");
  TASK_CHECK(CheckMalformedRead(bad_label) == 0);

  // Imported functions cannot own a non-empty label table.
  Bytes import_labels;
  fixture::u32(import_labels, 1);
  fixture::u32(import_labels, 0);
  fixture::u32(import_labels, 1);
  fixture::u32(import_labels, 0);
  fixture::str(import_labels, "imported");
  TASK_CHECK(CheckMalformedRead(import_labels) == 0);

  // With custom-section failures demoted to warnings, an out-of-range label
  // does not prevent the independent read from succeeding.
  Bytes warning_module_bytes = MakeModule({0x00, 0x0b}, bad_label, false);
  Module warning_module;
  Errors warning_errors;
  TASK_CHECK(Succeeded(ReadBytes(warning_module_bytes, &warning_module,
                                 &warning_errors, true, false)));
  return 0;
}

int identity_duplicate_and_exit(void) {
  Bytes bytes = MakeModule(NestedBranchBody(), NestedNames(), false);
  Module module;
  Errors errors;
  TASK_CHECK(Succeeded(ReadBytes(bytes, &module, &errors)));
  TASK_CHECK(errors.empty());

  Topology original = Analyze(module.funcs[1]);
  TASK_CHECK(original.labels.size() == 3);
  // br_if, three br_table entries, br_table default, then two br instructions.
  TASK_CHECK(original.branches.size() == 7);
  TASK_CHECK(*original.labels[0] == "$same");
  TASK_CHECK(*original.labels[1] == "$same.1");
  TASK_CHECK(*original.labels[2] == "$same.2");

  TASK_CHECK(Succeeded(GenerateNames(&module)));
  TASK_CHECK(Succeeded(ApplyNames(&module)));

  Index expected_targets[] = {1, 2, 1, 0, kExitIdentity, 1, 0};
  for (size_t i = 0; i < original.branches.size(); ++i) {
    TASK_CHECK(original.branches[i].target == expected_targets[i]);
    const BranchItem& item = original.branches[i];
    if (item.target == kExitIdentity) {
      TASK_CHECK(item.var->is_index());
      TASK_CHECK(item.var->index() == 3);
    } else {
      TASK_CHECK(item.var->is_name());
      TASK_CHECK(item.var->name() == *original.labels[item.target]);
    }
  }
  return 0;
}

int identity_partial_without_generate(void) {
  Bytes names = LabelSubsection({{1, {{0, "outer"}}}});
  Bytes bytes = MakeModule(NestedBranchBody(), names, true);
  Module module;
  Errors errors;
  TASK_CHECK(Succeeded(ReadBytes(bytes, &module, &errors)));
  TASK_CHECK(errors.empty());

  Topology original = Analyze(module.funcs[1]);
  TASK_CHECK(original.labels.size() == 3);
  TASK_CHECK(*original.labels[0] == "$outer");
  TASK_CHECK(original.labels[1]->empty());
  TASK_CHECK(original.labels[2]->empty());
  TASK_CHECK(Succeeded(ApplyNames(&module)));

  for (const BranchItem& item : original.branches) {
    if (item.target == 0) {
      TASK_CHECK(item.var->is_name());
      TASK_CHECK(item.var->name() == "$outer");
    } else {
      TASK_CHECK(item.var->is_index());
    }
  }

  std::string wat = WriteModuleToWat(&module);
  TASK_CHECK(wat.find("block $outer") != std::string::npos);
  Errors parse_errors;
  std::unique_ptr<Module> reparsed = ParseWat(wat, &parse_errors);
  TASK_CHECK(reparsed != nullptr);
  TASK_CHECK(parse_errors.empty());
  Topology roundtrip = Analyze(reparsed->funcs[1]);
  TASK_CHECK(roundtrip.branches.size() == original.branches.size());
  for (size_t i = 0; i < original.branches.size(); ++i) {
    TASK_CHECK(roundtrip.branches[i].target ==
               original.branches[i].target);
    TASK_CHECK(roundtrip.branches[i].depth == original.branches[i].depth);
  }
  return 0;
}

int integration_two_arm_positions(void) {
  TASK_CHECK(CheckWatRoundtrip(MakeModule(TwoArmBody(), TwoArmNames(), true)) ==
             0);
  TASK_CHECK(CheckWatRoundtrip(MakeModule(TwoArmBody(), TwoArmNames(), false)) ==
             0);

  Module released_module;
  {
    Bytes bytes = MakeModule(NestedBranchBody(), NestedNames(), true);
    Errors errors;
    TASK_CHECK(Succeeded(ReadBytes(bytes, &released_module, &errors)));
  }
  TASK_CHECK(Succeeded(GenerateNames(&released_module)));
  TASK_CHECK(Succeeded(ApplyNames(&released_module)));
  std::string wat = WriteModuleToWat(&released_module);
  TASK_CHECK(wat.find("$same.2") != std::string::npos);
  TASK_CHECK(wat.find("br_table $same.2 $same.1 $same 3") !=
             std::string::npos);
  return 0;
}

int integration_duplicate_late(void) {
  TASK_CHECK(CheckWatRoundtrip(
                 MakeModule(NestedBranchBody(), NestedNames(), false)) == 0);

  // Partial names plus GenerateNames must avoid a debug name occurring later
  // in opening order.
  Bytes conflict_bytes =
      MakeModule(AutoConflictBody(), AutoConflictNames(), false);
  Module module;
  Errors errors;
  TASK_CHECK(Succeeded(ReadBytes(conflict_bytes, &module, &errors)));
  TASK_CHECK(errors.empty());
  std::vector<std::string*> labels = CollectLabels(module.funcs[1]);
  TASK_CHECK(labels.size() == 2);
  TASK_CHECK(labels[0]->empty());
  TASK_CHECK(*labels[1] == "$B0");
  TASK_CHECK(Succeeded(GenerateNames(&module)));
  TASK_CHECK(*labels[0] != "$B0");
  TASK_CHECK(*labels[0] != *labels[1]);
  TASK_CHECK(*labels[0] == "$B0_1");
  TASK_CHECK(Succeeded(ApplyNames(&module)));

  std::string wat = WriteModuleToWat(&module);
  TASK_CHECK(wat.find("block $B0_1") != std::string::npos);
  TASK_CHECK(wat.find("block $B0\n") != std::string::npos ||
             wat.find("block $B0 ") != std::string::npos);
  Errors parse_errors;
  std::unique_ptr<Module> reparsed = ParseWat(wat, &parse_errors);
  TASK_CHECK(reparsed != nullptr);
  TASK_CHECK(parse_errors.empty());
  return 0;
}

int integration_name_conversion(void) {
  Bytes body = {0x00, 0x02, 0x40, 0x0c, 0x00, 0x0b, 0x0b};
  Bytes names = LabelSubsection({{1, {{0, "a b"}}}});
  Bytes bytes = MakeModule(body, names, false);

  Module module;
  Errors errors;
  TASK_CHECK(Succeeded(ReadBytes(bytes, &module, &errors)));
  TASK_CHECK(errors.empty());
  TASK_CHECK(Succeeded(ApplyNames(&module)));

  std::string wat = WriteModuleToWat(&module);
  TASK_CHECK(wat.find("block $a_b") != std::string::npos);
  TASK_CHECK(wat.find("br $a_b") != std::string::npos);

  Errors parse_errors;
  std::unique_ptr<Module> reparsed = ParseWat(wat, &parse_errors);
  TASK_CHECK(reparsed != nullptr);
  TASK_CHECK(parse_errors.empty());
  return 0;
}

const task_case task_cases[] = {
    {"decode", "label-names-and-established-debug-names", decode_label_names},
    {"decode", "malformed-label-name-sections", decode_label_errors},
    {"identity", "duplicate-names-and-function-exit",
     identity_duplicate_and_exit},
    {"identity", "partial-names-without-generation",
     identity_partial_without_generate},
    {"integration", "two-arm-opening-order-both-positions",
     integration_two_arm_positions},
    {"integration", "duplicate-auto-conflict-and-wat-resolve",
     integration_duplicate_late},
    {"integration", "debug-name-character-conversion",
     integration_name_conversion},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
