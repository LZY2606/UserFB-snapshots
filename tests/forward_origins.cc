// Regression tests for forward DW_AT_specification / DW_AT_abstract_origin
// resolution. Every case builds real DWARF bytes (task_support/fixture.h),
// writes an ELF that contains only debug sections, and drives the production
// ElfReader -> CompilationUnit / DIEDispatcher -> DwarfCUToModule -> Module
// chain through the reusable OfflineSymbolConsumer. No image is executed and
// no process memory is read.
//
// The three groups also produce analysis/symbols.txt and
// analysis/relations.tsv from their real analysis calls; the lifecycle
// group's final case writes analysis/relations.md.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "task_support/check.h"
#include "task_support/fixture.h"
#include "common/dwarf/dwarf_diag_consumer.h"
#include "common/module.h"

using google_breakpad::DeclarationHop;
using google_breakpad::Module;
using google_breakpad::OfflineAnalysis;
using google_breakpad::OfflineSymbolConsumer;
using google_breakpad::RelationRecord;
using sample::Bytes;
using sample::Image;

namespace {

std::string Hex(uint64_t value) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "0x%llx",
           static_cast<unsigned long long>(value));
  return std::string(buffer);
}

void MakeDir(const char* path) {
  struct stat st;
  if (stat(path, &st) != 0)
    mkdir(path, 0755);
}

// Write a generated image under the build directory and return its path.
std::string WriteImage(const Image& image, const char* name) {
  MakeDir(".task-build");
  MakeDir(".task-build/analysis-input");
  std::string path = std::string(".task-build/analysis-input/") + name;
  image.write(path.c_str());
  return path;
}

const RelationRecord* FindFunctionAt(const OfflineAnalysis& analysis,
                                     uint64_t address) {
  for (const RelationRecord& record : analysis.relations) {
    if (record.kind == "function" && !record.ranges.empty() &&
        record.ranges.front().address == address)
      return &record;
  }
  return nullptr;
}

const RelationRecord* FindInlineAt(const OfflineAnalysis& analysis,
                                   uint64_t address) {
  for (const RelationRecord& record : analysis.relations) {
    if (record.kind == "inline" && !record.ranges.empty() &&
        record.ranges.front().address == address)
      return &record;
  }
  return nullptr;
}

std::string FormatRanges(const std::vector<Module::Range>& ranges) {
  std::string out;
  for (size_t i = 0; i < ranges.size(); ++i) {
    if (i)
      out += ",";
    out += Hex(ranges[i].address) + "+" + Hex(ranges[i].size);
  }
  return out;
}

std::string ChainTargets(const RelationRecord& record) {
  std::string out;
  for (size_t i = 0; i < record.chain.size(); ++i) {
    if (i)
      out += "->";
    out += Hex(record.chain[i].target_offset);
  }
  return out;
}

std::string ChainAttributes(const RelationRecord& record) {
  static const char* kNames[] = {"specification", "abstract_origin"};
  std::string out;
  for (size_t i = 0; i < record.chain.size(); ++i) {
    if (i)
      out += "->";
    // DW_AT_specification == 0x47, DW_AT_abstract_origin == 0x31.
    out += (record.chain[i].attribute == 0x47) ? kNames[0] : kNames[1];
  }
  return out;
}

// Append one analysis result to the shared, self-owned analysis outputs.
// symbols.txt keeps the verbatim Module::Write format; relations.tsv is the
// separate, tab-separated relation record stream.
void EmitAnalysis(const OfflineAnalysis& analysis) {
  static bool initialized = false;
  MakeDir("analysis");
  if (!initialized) {
    std::ofstream("analysis/symbols.txt", std::ios::trunc);
    std::ofstream tsv("analysis/relations.tsv", std::ios::trunc);
    tsv << "sample\tkind\tcu_offset\tconcrete_die_offset\tranges\tcall_line\t"
           "chain_targets\tchain_attributes\tterminal_offset\tfinal_name\n";
    initialized = true;
  }

  std::ofstream symbols("analysis/symbols.txt", std::ios::app);
  symbols << analysis.symbols_text;

  std::ofstream tsv("analysis/relations.tsv", std::ios::app);
  for (const RelationRecord& record : analysis.relations) {
    tsv << record.sample_name << '\t' << record.kind << '\t'
        << Hex(record.cu_offset) << '\t'
        << Hex(record.concrete_die_offset) << '\t'
        << FormatRanges(record.ranges) << '\t'
        << record.call_site_line << '\t' << ChainTargets(record) << '\t'
        << ChainAttributes(record) << '\t'
        << Hex(record.terminal_offset) << '\t' << record.final_name << '\n';
  }
}

// Build an ELF with three concrete subprograms citing one later origin. Two
// share address 0x4000 (a real same-address/same-name duplicate); the third
// sits at 0x4100 and must survive even though one of its sibling waiters is
// eliminated.
Image BuildDuplicateImage() {
  // Reuse fanout's abbreviation table:
  // 1 = compile_unit (stmt_list, name)
  // 2 = subprogram (low_pc, high_pc, abstract_origin)
  // 3 = subprogram (name, inline)
  Bytes a = {1, 0x11, 1, 0x13, 0x05, 0x03, 0x08, 0, 0,
             2, 0x2e, 0, 0x11, 0x01, 0x12, 0x06, 0x31, 0x13, 0, 0,
             3, 0x2e, 0, 0x03, 0x08, 0x20, 0x0b, 0, 0, 0};
  Bytes i;
  sample::put(i, 0, 4);       // unit length, patched later
  sample::put(i, 4, 2);       // DWARF version 4
  sample::put(i, 0, 4);       // abbrev offset
  sample::put(i, 8, 1);       // address size
  sample::put(i, 1, 1);       // root: abbrev 1
  sample::put(i, 0, 2);       // stmt_list (DW_FORM_data2 in abbrev 1)
  sample::text(i, "dup.cc");

  sample::put(i, 2, 1);
  sample::put(i, 0x4000, 8);
  sample::put(i, 0x10, 4);
  size_t ref_a = i.size();
  sample::put(i, 0, 4);

  sample::put(i, 2, 1);
  sample::put(i, 0x4000, 8);
  sample::put(i, 0x10, 4);
  size_t ref_b = i.size();
  sample::put(i, 0, 4);

  sample::put(i, 2, 1);
  sample::put(i, 0x4100, 8);
  sample::put(i, 0x10, 4);
  size_t ref_c = i.size();
  sample::put(i, 0, 4);

  // The origin is another child of the root DIE; a single trailing null
  // closes the root's child list.
  size_t origin = i.size();
  sample::put(i, 3, 1);       // origin: abbrev 3
  sample::text(i, "dup");
  sample::put(i, 1, 1);       // DW_AT_inline = inlined
  sample::put(i, 0, 1);       // end of root children

  sample::patch(i, ref_a, origin, 4);
  sample::patch(i, ref_b, origin, 4);
  sample::patch(i, ref_c, origin, 4);
  sample::patch(i, 0, i.size() - 4, 4);
  return sample::wrap(a, i);
}

// Two CUs. CU1 holds two concrete subprograms at the same address 0x6000,
// both citing one named declaration that only appears in CU2. Both concrete
// functions are already owned by the Module when CU2 is parsed, so the
// duplicate is retired through the late-wake path, not AddFunction. A third
// concrete function at 0x6100 cites a different declaration and must survive.
Image BuildCrossCuDuplicateImage() {
  // Same abbreviation table as sample::cross_cu(): abstract_origin uses
  // DW_FORM_ref_addr (4 bytes in DWARF4).
  Bytes a = {1, 0x11, 1, 0x13, 0x05, 0x03, 0x08, 0, 0,
             2, 0x2e, 0, 0x11, 0x01, 0x12, 0x06, 0x31, 0x10, 0, 0,
             3, 0x2e, 0, 0x03, 0x08, 0x20, 0x0b, 0, 0, 0};
  Bytes i;
  auto cu = [&](const char* name) {
    size_t at = i.size();
    sample::put(i, 0, 4);
    sample::put(i, 4, 2);
    sample::put(i, 0, 4);
    sample::put(i, 8, 1);
    sample::put(i, 1, 1);
    sample::put(i, 4, 2);  // stmt_list (data2)
    sample::text(i, name);
    return at;
  };

  size_t first = cu("one.cc");
  auto concrete = [&](uint64_t address) {
    sample::put(i, 2, 1);
    sample::put(i, address, 8);
    sample::put(i, 0x10, 4);
    size_t ref = i.size();
    sample::put(i, 0, 4);  // ref_addr, patched later
    return ref;
  };
  size_t ref_a = concrete(0x6000);
  size_t ref_b = concrete(0x6000);
  size_t ref_c = concrete(0x6100);
  sample::put(i, 0, 1);
  sample::patch(i, first, i.size() - first - 4, 4);

  size_t second = cu("two.cc");
  size_t shared_origin = i.size();
  sample::put(i, 3, 1);
  sample::text(i, "shared");
  sample::put(i, 1, 1);  // inline
  size_t other_origin = i.size();
  sample::put(i, 3, 1);
  sample::text(i, "other");
  sample::put(i, 1, 1);  // inline
  sample::put(i, 0, 1);
  sample::patch(i, second, i.size() - second - 4, 4);

  sample::patch(i, ref_a, shared_origin, 4);
  sample::patch(i, ref_b, shared_origin, 4);
  sample::patch(i, ref_c, other_origin, 4);
  return sample::wrap(a, i);
}

// One concrete subprogram cites an unnamed intermediate declaration, which
// itself specifies a later named terminal. At registration time the
// intermediate's outgoing hop does not exist yet, so the waiter must be
// re-resolved after the chain is extended (specification-only multi-hop).
Image BuildFunctionMultiHopImage() {
  // 1 = compile_unit (stmt_list data2, name)
  // 2 = concrete subprogram (low_pc, high_pc, specification)
  // 3 = named inline subprogram (name, inline)
  // 4 = specification-only subprogram (specification)
  Bytes a = {1, 0x11, 1, 0x13, 0x05, 0x03, 0x08, 0, 0,
             2, 0x2e, 0, 0x11, 0x01, 0x12, 0x06, 0x47, 0x13, 0, 0,
             3, 0x2e, 0, 0x03, 0x08, 0x20, 0x0b, 0, 0,
             4, 0x2e, 0, 0x47, 0x13, 0, 0, 0};
  Bytes i;
  sample::put(i, 0, 4);
  sample::put(i, 4, 2);
  sample::put(i, 0, 4);
  sample::put(i, 8, 1);
  sample::put(i, 1, 1);
  sample::put(i, 0, 2);  // stmt_list data2
  sample::text(i, "multihop.cc");

  sample::put(i, 2, 1);  // concrete
  sample::put(i, 0x7000, 8);
  sample::put(i, 0x10, 4);
  size_t concrete_ref = i.size();
  sample::put(i, 0, 4);  // -> intermediate, patched

  size_t intermediate = i.size();
  sample::put(i, 4, 1);  // intermediate, specification only
  size_t intermediate_ref = i.size();
  sample::put(i, 0, 4);  // -> terminal, patched

  size_t terminal = i.size();
  sample::put(i, 3, 1);  // terminal, named
  sample::text(i, "resolved");
  sample::put(i, 1, 1);  // inline

  sample::put(i, 0, 1);  // end of root children
  sample::patch(i, concrete_ref, intermediate, 4);
  sample::patch(i, intermediate_ref, terminal, 4);
  sample::patch(i, 0, i.size() - 4, 4);
  return sample::wrap(a, i);
}

// --- fanout group ---------------------------------------------------------

int FanoutForwardNames() {
  // Analyze the checked-in ELF sample directly; it carries only debug
  // sections, so this exercises ElfReader's section lookup too.
  const std::string path = "task_support/samples/fanout.elf";
  std::unique_ptr<OfflineAnalysis> analysis =
      OfflineSymbolConsumer::Analyze(path);
  TASK_CHECK(analysis && analysis->ok);

  // Both forward-referencing ranges resolve to the late declaration; the
  // self-named function keeps its own name.
  const RelationRecord* first = FindFunctionAt(*analysis, 0x1000);
  const RelationRecord* second = FindFunctionAt(*analysis, 0x1100);
  const RelationRecord* normal = FindFunctionAt(*analysis, 0x2000);
  TASK_CHECK(first && second && normal);
  TASK_CHECK(first->final_name == "late");
  TASK_CHECK(second->final_name == "late");
  TASK_CHECK(normal->final_name == "normal");

  // Two distinct concrete DIEs, both citing the same later origin.
  TASK_CHECK(first->concrete_die_offset == 0x2c);
  TASK_CHECK(second->concrete_die_offset == 0x3d);
  TASK_CHECK(normal->concrete_die_offset == 0x18);
  TASK_CHECK(first->chain.size() == 1 && second->chain.size() == 1);
  TASK_CHECK(first->chain[0].target_offset == 0x4e);
  TASK_CHECK(second->chain[0].target_offset == 0x4e);
  TASK_CHECK(first->terminal_offset == 0x4e &&
             second->terminal_offset == 0x4e);
  TASK_CHECK(first->chain[0].attribute == 0x31);  // abstract_origin

  // Ranges and the placeholder behaviour come from the concrete instances.
  TASK_CHECK(first->ranges.size() == 1 && first->ranges[0].size == 0x10);
  TASK_CHECK(analysis->symbols_text.find("<name omitted>") ==
             std::string::npos);
  TASK_CHECK(analysis->symbols_text.find("FUNC") != std::string::npos);

  EmitAnalysis(*analysis);
  return 0;
}

// --- convergence group ----------------------------------------------------

int InlineConvergenceNames() {
  Image image = sample::inline_convergence();
  std::string path = WriteImage(image, "inline-convergence.elf");
  std::unique_ptr<OfflineAnalysis> analysis =
      OfflineSymbolConsumer::Analyze(path);
  TASK_CHECK(analysis && analysis->ok);

  // One host function with two concrete inline instances preserved.
  const RelationRecord* host = FindFunctionAt(*analysis, 0x5000);
  const RelationRecord* inline_a = FindInlineAt(*analysis, 0x5010);
  const RelationRecord* inline_b = FindInlineAt(*analysis, 0x5020);
  TASK_CHECK(host && inline_a && inline_b);
  TASK_CHECK(host->final_name == "host");
  TASK_CHECK(inline_a != inline_b);  // same name later, distinct instances

  // The instances keep their own ranges and call-site lines.
  TASK_CHECK(inline_a->call_site_line == 10);
  TASK_CHECK(inline_b->call_site_line == 20);
  TASK_CHECK(inline_a->ranges[0].size == 4 &&
             inline_b->ranges[0].size == 4);
  TASK_CHECK(inline_a->concrete_die_offset == 0x2a);
  TASK_CHECK(inline_b->concrete_die_offset == 0x3c);

  // Two-hop chains through different intermediate declarations converge on
  // the same terminal DIE and name.
  TASK_CHECK(inline_a->chain.size() == 2);
  TASK_CHECK(inline_a->chain[0].target_offset == 0x4f);
  TASK_CHECK(inline_a->chain[1].target_offset == 0x5b);
  TASK_CHECK(inline_b->chain.size() == 2);
  TASK_CHECK(inline_b->chain[0].target_offset == 0x55);
  TASK_CHECK(inline_b->chain[1].target_offset == 0x5b);
  TASK_CHECK(inline_a->terminal_offset == 0x5b &&
             inline_b->terminal_offset == 0x5b);
  TASK_CHECK(inline_a->final_name == "late_inline" &&
             inline_b->final_name == "late_inline");

  // Serialization keeps a single merged origin and no placeholder.
  const std::string& symbols = analysis->symbols_text;
  size_t first_origin = symbols.find("INLINE_ORIGIN");
  TASK_CHECK(first_origin != std::string::npos);
  TASK_CHECK(symbols.find("INLINE_ORIGIN", first_origin + 1) ==
             std::string::npos);
  TASK_CHECK(symbols.find("late_inline") != std::string::npos);
  TASK_CHECK(symbols.find("<name omitted>") == std::string::npos);

  EmitAnalysis(*analysis);
  return 0;
}

int FunctionSpecificationMultiHop() {
  Image image = BuildFunctionMultiHopImage();
  std::string path = WriteImage(image, "function-multihop.elf");
  std::unique_ptr<OfflineAnalysis> analysis =
      OfflineSymbolConsumer::Analyze(path);
  TASK_CHECK(analysis && analysis->ok);

  // The concrete function cites the intermediate first; the intermediate
  // gains its specification hop to the named terminal only afterwards. The
  // waiter must be re-resolved rather than stuck at the registration root.
  const RelationRecord* function = FindFunctionAt(*analysis, 0x7000);
  TASK_CHECK(function != nullptr);
  TASK_CHECK(function->final_name == "resolved");
  TASK_CHECK(function->chain.size() == 2);
  TASK_CHECK(function->chain[0].attribute == 0x47);  // specification
  TASK_CHECK(function->chain[1].attribute == 0x47);
  TASK_CHECK(analysis->symbols_text.find("<name omitted>") ==
             std::string::npos);
  return 0;
}

// --- lifecycle group ------------------------------------------------------

int CrossCuDistinctDeclarations() {
  // Analyze the checked-in two-CU ELF sample directly.
  const std::string path = "task_support/samples/cross-cu.elf";
  std::unique_ptr<OfflineAnalysis> analysis =
      OfflineSymbolConsumer::Analyze(path);
  TASK_CHECK(analysis && analysis->ok);

  // Same address, two distinct declarations in a later CU: both survive.
  const RelationRecord* alpha = nullptr;
  const RelationRecord* beta = nullptr;
  unsigned at_3000 = 0;
  for (const RelationRecord& record : analysis->relations) {
    if (record.kind != "function" || record.ranges.empty() ||
        record.ranges.front().address != 0x3000)
      continue;
    ++at_3000;
    if (record.final_name == "Alpha")
      alpha = &record;
    if (record.final_name == "Beta")
      beta = &record;
  }
  TASK_CHECK(at_3000 == 2);
  TASK_CHECK(alpha && beta);
  TASK_CHECK(alpha->concrete_die_offset == 0x17);
  TASK_CHECK(beta->concrete_die_offset == 0x28);

  // Offsets are .debug_info section coordinates: the targets live in the
  // second CU (header at 0x3a) and must not be reported CU-relative.
  TASK_CHECK(alpha->terminal_offset == 0x52);
  TASK_CHECK(beta->terminal_offset == 0x5a);
  TASK_CHECK(alpha->terminal_offset > 0x3a && beta->terminal_offset > 0x3a);
  TASK_CHECK(alpha->cu_offset == 0 && beta->cu_offset == 0);
  TASK_CHECK(analysis->symbols_text.find("Alpha") != std::string::npos);
  TASK_CHECK(analysis->symbols_text.find("Beta") != std::string::npos);

  EmitAnalysis(*analysis);

  // All three designated inputs have now been emitted; write the narrative.
  std::ofstream md("analysis/relations.md", std::ios::trunc);
  md << "# Forward-origin relations\n\n"
     << "All offsets are absolute `.debug_info` section offsets, never "
        "CU-relative.\n\n"
     << "## fanout.elf\n"
     << "Concrete subprograms at section offsets 0x2c (range 0x1000) and "
        "0x3d (range 0x1100)\n"
     << "each carry a DW_AT_abstract_origin pointing at the later DIE 0x4e, "
        "which is the\n"
     << "inline declaration named `late`. Both concrete instances are "
        "waiters of the same\n"
     << "declaration and both resolve to `late`; the self-named DIE 0x18 at "
        "range 0x2000\n"
     << "keeps its own name `normal`. The old code keyed one function per "
        "forward target, so\n"
     << "the first waiter was overwritten by the second and stayed "
        "`<name omitted>`.\n\n"
     << "## cross-cu.elf\n"
     << "Two concrete subprograms 0x17 and 0x28 share virtual address "
        "0x3000 but cite two\n"
     << "different declarations in the second CU (header at 0x3a): 0x52 "
        "`Alpha` and 0x5a\n"
     << "`Beta`. Equal addresses with unresolved names must not be collapsed, "
        "so both\n"
     << "functions are retained. The old comparator ordered functions by "
        "address then name,\n"
     << "merging the two placeholder-named entries before either declaration "
        "was parsed.\n\n"
     << "## inline_convergence\n"
     << "Inline instances 0x2a (call line 10, range 0x5010) and 0x3c (call "
        "line 20, range\n"
     << "0x5020) point through distinct intermediate subprograms 0x4f and "
        "0x55, each of\n"
     << "which has a DW_AT_specification to terminal DIE 0x5b named "
        "`late_inline`. Both\n"
     << "concrete instances and both chains remain in relations.tsv even "
        "though they share\n"
     << "one terminal: equal names do not make them the same concrete "
        "instance. The symbol\n"
     << "output still merges the two InlineOrigin objects by name into one "
        "INLINE_ORIGIN\n"
     << "record, and every previously handed-out InlineOrigin pointer stays "
        "valid.\n";
  return 0;
}

int DuplicateRetirement() {
  Image image = BuildDuplicateImage();
  std::string path = WriteImage(image, "duplicate.elf");
  std::unique_ptr<OfflineAnalysis> analysis =
      OfflineSymbolConsumer::Analyze(path);
  TASK_CHECK(analysis && analysis->ok);

  // The two 0x4000 instances are a real same-address/same-name duplicate: one
  // survives. The 0x4100 waiter is a different concrete instance and stays.
  TASK_CHECK(FindFunctionAt(*analysis, 0x4000) != nullptr);
  TASK_CHECK(FindFunctionAt(*analysis, 0x4100) != nullptr);
  unsigned functions = 0;
  for (const RelationRecord& record : analysis->relations)
    if (record.kind == "function")
      ++functions;
  TASK_CHECK(functions == 2);
  TASK_CHECK(FindFunctionAt(*analysis, 0x4000)->final_name == "dup");
  TASK_CHECK(FindFunctionAt(*analysis, 0x4100)->final_name == "dup");
  TASK_CHECK(analysis->symbols_text.find("<name omitted>") ==
             std::string::npos);
  return 0;
}

int CrossCuLateDuplicateRetirement() {
  Image image = BuildCrossCuDuplicateImage();
  std::string path = WriteImage(image, "cross-cu-duplicate.elf");
  std::unique_ptr<OfflineAnalysis> analysis =
      OfflineSymbolConsumer::Analyze(path);
  TASK_CHECK(analysis && analysis->ok);

  // Both 0x6000 waiters are already Module-owned when CU2 names them. The
  // late wake retires exactly one same-address/same-name duplicate while the
  // distinct 0x6100 concrete function (different declaration) is untouched.
  unsigned at_6000 = 0;
  for (const RelationRecord& record : analysis->relations) {
    if (record.kind == "function" && !record.ranges.empty() &&
        record.ranges.front().address == 0x6000)
      ++at_6000;
  }
  TASK_CHECK(at_6000 == 1);
  const RelationRecord* survivor = FindFunctionAt(*analysis, 0x6000);
  const RelationRecord* other = FindFunctionAt(*analysis, 0x6100);
  TASK_CHECK(survivor && other);
  TASK_CHECK(survivor->final_name == "shared");
  TASK_CHECK(other->final_name == "other");
  TASK_CHECK(analysis->symbols_text.find("<name omitted>") ==
             std::string::npos);
  // Two concrete DIEs still cited the shared declaration in the evidence;
  // retirement removes a Module function, not the reference record.
  return 0;
}

int InlineOriginPointerLifecycle() {
  Module module("lifecycle", "Linux", "x86_64", "id");
  Module::InlineOriginMap& origins = module.inline_origin_maps["life"];
  auto pool = [&](const char* s) {
    return module.AddStringToPool(s);
  };

  // Two origins are created separately, before their chains converge.
  origins.SetReference(10, 10);
  Module::InlineOrigin* first =
      origins.GetOrCreateInlineOrigin(10, pool("<name omitted>"));
  origins.SetReference(20, 20);
  Module::InlineOrigin* second =
      origins.GetOrCreateInlineOrigin(20, pool("<name omitted>"));
  TASK_CHECK(first != nullptr && second != nullptr && first != second);

  // Both chains are now extended onto the same terminal declaration.
  origins.SetReference(10, 30);
  origins.SetReference(20, 30);
  origins.SetReference(30, 30);
  Module::InlineOrigin* terminal =
      origins.GetOrCreateInlineOrigin(30, pool("late_inline"));
  TASK_CHECK(terminal != nullptr);

  // Re-resolving through either entry yields the named terminal, and the
  // pointers handed out earlier remain valid for the life of the Module.
  TASK_CHECK(origins.GetOrCreateInlineOrigin(10, pool("<name omitted>")) ==
             terminal);
  TASK_CHECK(origins.GetOrCreateInlineOrigin(20, pool("<name omitted>")) ==
             terminal);
  TASK_CHECK(first->name == "late_inline");
  TASK_CHECK(second->name == "late_inline");
  TASK_CHECK(terminal->name == "late_inline");
  return 0;
}

}  // namespace

const struct task_case task_cases[] = {
    {"fanout", "forward_abstract_origin_fanout", FanoutForwardNames},
    {"convergence", "two_hop_inline_convergence", InlineConvergenceNames},
    {"convergence", "function_specification_multihop",
     FunctionSpecificationMultiHop},
    {"lifecycle", "cross_cu_same_address_distinct_names",
     CrossCuDistinctDeclarations},
    {"lifecycle", "real_duplicate_retirement", DuplicateRetirement},
    {"lifecycle", "cross_cu_late_duplicate_retirement",
     CrossCuLateDuplicateRetirement},
    {"lifecycle", "inline_origin_pointer_lifecycle",
     InlineOriginPointerLifecycle},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
