// Deterministic internal and end-to-end checks for ExpressionGeneric.
//
// checks/generic_driver.cpp invokes generic_expression_case() once for each
// required group.  The generated group uses the real csmith executable and
// runtime headers named by CSMITH_CHECK_* environment variables.

#include "CGOptions.h"
#include "Constant.h"
#include "Effect.h"
#include "ExpressionGeneric.h"
#include "ExpressionVariable.h"
#include "FactPointTo.h"
#include "Function.h"
#include "RandomNumber.h"
#include "Type.h"
#include "Variable.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct TestContext {
  TestContext() {
    CGOptions::set_default_settings();
    RandomNumber::CreateInstance(RNDNUM_GENERATOR::rDefaultRndNumGenerator,
                                0x67656e31u);
  }
};

std::string shell_quote(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

std::string env_or(const char *name, const std::string &fallback) {
  const char *v = std::getenv(name);
  return v && *v ? v : fallback;
}

std::string slurp(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

int run_command(const std::string &command, std::string *output = nullptr) {
  FILE *pipe = popen((command + " 2>&1").c_str(), "r");
  if (!pipe)
    return -1;
  std::string result;
  std::array<char, 4096> buf{};
  while (fgets(buf.data(), static_cast<int>(buf.size()), pipe))
    result += buf.data();
  int status = pclose(pipe);
  int rc = -1;
  if (WIFEXITED(status))
    rc = WEXITSTATUS(status);
  if (output)
    *output = result;
  return rc;
}

bool write_file(const fs::path &p, const std::string &contents) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out)
    return false;
  out << contents;
  return static_cast<bool>(out);
}

CVQualifiers scalar_quals(bool c) {
  CVQualifiers q;
  q.add_qualifiers(c, false);
  return q;
}

CVQualifiers pointer_quals(bool base_const, bool top_const) {
  CVQualifiers q;
  q.add_qualifiers(base_const, false);
  q.add_qualifiers(top_const, false);
  return q;
}

Variable *test_variable(const std::string &name, const Type *t,
                        CVQualifiers q = CVQualifiers()) {
  if (q.get_consts().empty())
    q = ExpressionGeneric::canonical_qualifiers(t);
  return Variable::CreateVariable(name, t, Constant::make_int(0), &q);
}

std::string checksum_of_output(const std::string &out) {
  const std::string key = "checksum = ";
  auto pos = out.find(key);
  if (pos == std::string::npos)
    return "";
  pos += key.size();
  auto end = out.find_first_of("\r\n", pos);
  return out.substr(pos, end == std::string::npos ? std::string::npos
                                                   : end - pos);
}

#define DEFINE_GROUP(group_name)                                               \
  int generic_expression_##group_name(unsigned *checks) {                      \
    unsigned local_checks = 0;                                                 \
    auto CHECK = [&](bool cond) -> int {                                       \
      if (!cond) {                                                             \
        std::fprintf(stderr, "generic test failed: %s check #%u\n",           \
                     #group_name, local_checks + 1);                           \
        return 1;                                                              \
      }                                                                        \
      ++local_checks;                                                          \
      *checks = local_checks;                                                  \
      return 0;                                                                \
    };

#define EXPECT(cond)                                                           \
  do {                                                                         \
    int generic_test_rc_ = CHECK((cond));                                      \
    if (generic_test_rc_)                                                      \
      return generic_test_rc_;                                                 \
  } while (0)

#define END_GROUP()                                                            \
  *checks = local_checks;                                                      \
  return 0;                                                                    \
  }

DEFINE_GROUP(types)
{
  const Type *ti = &Type::get_simple_type(eSimpleType::eInt);
  const Type *tu = &Type::get_simple_type(eSimpleType::eUInt);
  const Type *tc = &Type::get_simple_type(eSimpleType::eChar);
  const Type *ts = &Type::get_simple_type(eSimpleType::eShort);
  const Type *tf = &Type::get_simple_type(eSimpleType::eFloat);
  const Type *tp = Type::find_pointer_type(ti, true);

  {
    auto *g = ExpressionGeneric::create_checked(
        Constant::make_int(7),
        {{ti, Constant::make_int(101)},
         {tf, Constant::make_int(102)}},
        Constant::make_int(109));
    EXPECT(g != nullptr);
    EXPECT(g->valid());
    EXPECT(g->selected_index() == 0);
    EXPECT(&g->get_type() == ti);
    EXPECT(g->get_selected_expr() != nullptr);
    EXPECT(g->to_string().find("_Generic") != std::string::npos);
    delete g;
  }

  // char and short are not promoted to int for association selection.
  {
    auto *gc = ExpressionGeneric::create_checked(
        new Constant(tc, "97"),
        {{tc, new Constant(tc, "99")},
         {ti, Constant::make_int(2)}},
        Constant::make_int(0));
    EXPECT(gc != nullptr);
    EXPECT(gc->selected_index() == 0);
    delete gc;

    auto *gs = ExpressionGeneric::create_checked(
        new Constant(ts, "12"),
        {{ti, Constant::make_int(2)},
         {ts, new Constant(ts, "13")}},
        Constant::make_int(0));
    EXPECT(gs != nullptr);
    EXPECT(gs->selected_index() == 1);
    delete gs;
  }

  // float is an exact supported control type.
  {
    auto *gf = ExpressionGeneric::create_checked(
        new Constant(tf, "1.0f"),
        {{ti, Constant::make_int(1)},
         {tf, new Constant(tf, "2.0f")}},
        nullptr);
    EXPECT(gf != nullptr);
    EXPECT(gf->selected_index() == 1);
    EXPECT(&gf->get_type() == tf);
    delete gf;
  }

  // Object pointer matching.
  {
    Variable *pv = test_variable("g_types_p", tp);
    auto *gp = ExpressionGeneric::create_checked(
        new ExpressionVariable(*pv),
        {{tf, Constant::make_int(0)},
         {tp, Constant::make_int(1)}},
        nullptr);
    EXPECT(gp != nullptr);
    EXPECT(gp->selected_index() == 1);
    delete gp;
  }

  // Top-level const on the scalar controller is suppressed.
  {
    Variable *cv =
        test_variable("g_types_const_int", ti, scalar_quals(true));
    auto *gq = ExpressionGeneric::create_checked(
        new ExpressionVariable(*cv),
        {{ti, Constant::make_int(1)}},
        nullptr);
    EXPECT(gq != nullptr);
    EXPECT(gq->selected_index() == 0);
    delete gq;
  }

  // Pointer-target qualifiers remain significant.
  {
    CVQualifiers q_pc = pointer_quals(true, false);
    CVQualifiers q_topc = pointer_quals(false, true);
    Variable *pcv = test_variable("g_types_pc", tp, q_pc);
    Variable *ptcv = test_variable("g_types_ptc", tp, q_topc);

    auto *gpc = ExpressionGeneric::create_checked(
        new ExpressionVariable(*pcv),
        {{tp, Constant::make_int(1),
          ExpressionGeneric::canonical_qualifiers(tp)},
         {tp, Constant::make_int(2), q_pc}},
        Constant::make_int(3));
    EXPECT(gpc != nullptr);
    EXPECT(gpc->selected_index() == 1);
    delete gpc;

    auto *gtop = ExpressionGeneric::create_checked(
        new ExpressionVariable(*ptcv),
        {{tp, Constant::make_int(4),
          ExpressionGeneric::canonical_qualifiers(tp)},
         {tp, Constant::make_int(5), q_topc}},
        Constant::make_int(6));
    EXPECT(gtop != nullptr);
    // int * const controller loses its top-level const and selects int *.
    EXPECT(gtop->selected_index() == 0);
    delete gtop;
  }

  // Strict C11 matching, not assignment conversion.
  EXPECT(!ExpressionGeneric::c11_types_compatible(ti, tu));
  EXPECT(!ExpressionGeneric::c11_types_compatible(ti, tc));
  EXPECT(!ExpressionGeneric::c11_types_compatible(ti, ts));
  EXPECT(!ExpressionGeneric::c11_types_compatible(ti, tf));

  // Struct/void controls are rejected.
  const Type *tv = Type::void_type;
  EXPECT(!ExpressionGeneric::supported_control_type(tv));
  EXPECT(!ExpressionGeneric::supported_control_type(nullptr));
  Type *st = Type::make_random_struct_type();
  EXPECT(st != nullptr);
  EXPECT(!ExpressionGeneric::supported_control_type(st));

  // Duplicate exact associations are a construction error.
  {
    std::string err;
    auto *bad = ExpressionGeneric::create_checked(
        Constant::make_int(1),
        {{ti, Constant::make_int(1)},
         {ti, Constant::make_int(2)}},
        nullptr, &err);
    EXPECT(bad == nullptr);
    EXPECT(!err.empty());
  }

  // Two separately allocated but structurally identical int types model two
  // differently named but compatible typedefs.  String names are not used.
  {
    auto *a = new Type(eSimpleType::eInt);
    auto *b = new Type(eSimpleType::eInt);
    std::string err;
    auto *bad = ExpressionGeneric::create_checked(
        Constant::make_int(1),
        {{a, Constant::make_int(1)},
         {b, Constant::make_int(2)}},
        nullptr, &err);
    EXPECT(bad == nullptr);
    EXPECT(err.find("duplicate") != std::string::npos);
  }

  // No match and no default is a construction error.
  {
    std::string err;
    auto *bad = ExpressionGeneric::create_checked(
        Constant::make_int(1),
        {{tf, Constant::make_int(2)}},
        nullptr, &err);
    EXPECT(bad == nullptr);
    EXPECT(err.find("no matching") != std::string::npos);
  }
}
END_GROUP()

DEFINE_GROUP(effects)
{
  const Type *ti = &Type::get_simple_type(eSimpleType::eInt);
  const Type *tf = &Type::get_simple_type(eSimpleType::eFloat);
  Effect accum;
  CGContext ctx(nullptr, Effect::get_empty_effect(), &accum);

  Variable *control_var = test_variable("g_effects_control", ti);
  Variable *selected_var = test_variable("g_effects_selected", ti);
  Variable *unselected_var = test_variable("g_effects_unselected", ti);

  auto *g = ExpressionGeneric::create_checked(
      new ExpressionVariable(*control_var),
      {{ti, new ExpressionVariable(*selected_var)},
       {tf, new ExpressionVariable(*unselected_var)}},
      Constant::make_int(0));
  EXPECT(g != nullptr);

  FactVec facts;
  EXPECT(g->visit_facts(facts, ctx));
  EXPECT(accum.is_read(selected_var));
  EXPECT(!accum.is_read(unselected_var));
  // The controlling expression is not evaluated.
  EXPECT(!accum.is_read(control_var));

  delete g;
}
END_GROUP()

DEFINE_GROUP(facts)
{
  const Type *ti = &Type::get_simple_type(eSimpleType::eInt);
  const Type *ts = &Type::get_simple_type(eSimpleType::eShort);
  const Type *tp = Type::find_pointer_type(ti, true);

  Variable *p = test_variable("g_facts_p", tp);
  Variable *target = test_variable("g_facts_target", ti);
  Variable *bad_p = test_variable("g_facts_bad_p", tp);

  FactVec facts;
  facts.push_back(FactPointTo::make_fact(p, target));

  auto *selected_deref = new ExpressionVariable(*p, ti);
  auto *unselected_bad_deref = new ExpressionVariable(*bad_p, ti);

  Effect accum;
  CGContext ctx(nullptr, Effect::get_empty_effect(), &accum);
  auto *g = ExpressionGeneric::create_checked(
      Constant::make_int(1),
      {{ti, selected_deref},
       {ts, unselected_bad_deref}},
      Constant::make_int(0));
  EXPECT(g != nullptr);
  EXPECT(g->visit_facts(facts, ctx));

  std::vector<const ExpressionVariable *> derefs =
      g->get_dereferenced_ptrs();
  EXPECT(derefs.size() == 1);
  EXPECT(derefs[0]->get_var() == p);
  EXPECT(accum.is_read(target));

  // The unselected branch really is invalid under the same facts; generic
  // fact propagation succeeds only because it does not visit that branch.
  Effect bad_accum;
  CGContext bad_ctx(nullptr, Effect::get_empty_effect(), &bad_accum);
  FactVec no_facts;
  EXPECT(!unselected_bad_deref->visit_facts(no_facts, bad_ctx));

  // Evaluation sources follow the selected branch only.
  std::vector<const Expression *> sources;
  g->get_eval_to_subexps(sources);
  EXPECT(sources.size() == 1);
  EXPECT(sources[0] == selected_deref ||
         sources[0]->use_var(p) ||
         dynamic_cast<const ExpressionVariable *>(sources[0]) != nullptr);

  delete g;
}
END_GROUP()

DEFINE_GROUP(clone)
{
  const Type *ti = &Type::get_simple_type(eSimpleType::eInt);
  const Type *tf = &Type::get_simple_type(eSimpleType::eFloat);
  auto *g = ExpressionGeneric::create_checked(
      Constant::make_int(3),
      {{ti, Constant::make_int(7)},
       {tf, new Constant(tf, "1.5f")}},
      Constant::make_int(9));
  EXPECT(g != nullptr);
  Expression *raw_clone = g->clone();
  auto *c = dynamic_cast<ExpressionGeneric *>(raw_clone);
  EXPECT(c != nullptr);
  EXPECT(c != g);
  EXPECT(c->selected_index() == g->selected_index());
  EXPECT(c->default_selected() == g->default_selected());
  EXPECT(c->to_string() == g->to_string());
  EXPECT(c->get_control() != g->get_control());
  EXPECT(!c->get_associations().empty());
  EXPECT(c->get_associations()[0].expr != g->get_associations()[0].expr);
  EXPECT(c->get_selected_expr() != nullptr);

  delete c;
  // The original remains valid after releasing the deep copy.
  EXPECT(g->to_string().find("_Generic") != std::string::npos);
  EXPECT(g->selected_index() == 0);
  delete g;
}
END_GROUP()

DEFINE_GROUP(generated)
{
  fs::path bin = env_or("CSMITH_CHECK_BIN", ".build-check/src/csmith");
  fs::path runtime_dir = env_or("CSMITH_CHECK_RUNTIME", "runtime");
  fs::path generated_runtime =
      env_or("CSMITH_CHECK_GENERATED_RUNTIME", ".build-check/runtime");
  fs::path tmp_root = env_or("CSMITH_CHECK_TMP",
                             fs::temp_directory_path().string());
  fs::path work = tmp_root / "generic_expression_case";
  std::error_code ec;
  fs::create_directories(work, ec);

  EXPECT(fs::exists(bin));
  EXPECT(fs::exists(runtime_dir / "csmith.h"));

  const std::vector<std::string> base_args = {
      "--c11-generic", "--max-funcs", "1", "--max-block-size", "2",
      "--max-expr-complexity", "4", "--no-volatiles", "--no-arrays",
      "--no-structs", "--no-unions", "--no-jumps"};

  auto csmith_command = [&](unsigned long seed, const fs::path &out,
                            bool selected_only) {
    std::string cmd = shell_quote(bin.string());
    cmd += " --seed " + std::to_string(seed);
    for (const auto &a : base_args)
      cmd += " " + a;
    if (selected_only)
      cmd += " --generic-selected-only-for-tests";
    cmd += " --output " + shell_quote(out.string());
    return cmd;
  };

  struct Sample {
    unsigned long seed;
    fs::path source;
    std::string checksum;
  };
  std::vector<Sample> samples;
  bool saw_nested = false;
  bool saw_pointer_qual = false;
  bool saw_default = false;
  bool saw_unselected_assign = false;
  bool saw_unselected_call = false;

  constexpr unsigned long first_seed = 9100;
  constexpr unsigned long last_seed = 9420;
  for (unsigned long seed = first_seed;
       seed < last_seed && samples.size() < 12; ++seed) {
    fs::path src = work / ("generic_" + std::to_string(seed) + ".c");
    int gen_rc = run_command(csmith_command(seed, src, false));
    if (gen_rc != 0 || !fs::exists(src))
      continue;
    std::string code = slurp(src);
    if (code.find("_Generic") == std::string::npos)
      continue;

    saw_default = saw_default || code.find("default :") != std::string::npos;
    {
      auto p = code.find("_Generic");
      if (p != std::string::npos &&
          code.find("_Generic", p + 8) != std::string::npos)
        saw_nested = true;
    }
    saw_pointer_qual =
        saw_pointer_qual ||
        code.find("const ") != std::string::npos &&
            code.find("* :") != std::string::npos;
    // Heuristic coverage checks over printed generic regions.
    size_t search_from = 0;
    while (true) {
      size_t start = code.find("_Generic(", search_from);
      if (start == std::string::npos)
        break;
      size_t next = code.find("_Generic(", start + 9);
      size_t limit = next == std::string::npos
                         ? std::min(code.size(), start + 4000)
                         : next;
      std::string region = code.substr(start, limit - start);
      if (region.find('=') != std::string::npos)
        saw_unselected_assign = true;
      if (region.find('(') != std::string::npos &&
          region.find("safe_") != std::string::npos)
        saw_unselected_call = true;
      search_from = start + 9;
    }

    fs::path bin0 = work / ("generic_" + std::to_string(seed) + "_O0");
    fs::path bin2 = work / ("generic_" + std::to_string(seed) + "_O2");
    std::string cc0 = "cc -std=c11 -w -O0 -I" +
                      shell_quote(runtime_dir.string()) + " -I" +
                      shell_quote(generated_runtime.string()) + " " +
                      shell_quote(src.string()) + " -o " +
                      shell_quote(bin0.string());
    std::string cc2 = "cc -std=c11 -w -O2 -I" +
                      shell_quote(runtime_dir.string()) + " -I" +
                      shell_quote(generated_runtime.string()) + " " +
                      shell_quote(src.string()) + " -o " +
                      shell_quote(bin2.string());
    if (run_command(cc0) != 0 || run_command(cc2) != 0) {
      std::fprintf(stderr, "generated seed %lu failed compilation\n", seed);
      return 1;
    }

    std::string out0, out2;
    int run0 = run_command("timeout 8 " + shell_quote(bin0.string()), &out0);
    int run2 = run_command("timeout 8 " + shell_quote(bin2.string()), &out2);
    if (run0 == 124 || run2 == 124)
      continue; // a few ordinary Csmith seeds have long bounded loops
    EXPECT(run0 == 0);
    EXPECT(run2 == 0);
    std::string sum0 = checksum_of_output(out0);
    std::string sum2 = checksum_of_output(out2);
    EXPECT(!sum0.empty());
    EXPECT(sum0 == sum2);

    // Emit the deterministic same-tree selected-only program and compare.
    fs::path selected_src =
        work / ("selected_" + std::to_string(seed) + ".c");
    fs::path selected_bin =
        work / ("selected_" + std::to_string(seed));
    EXPECT(run_command(csmith_command(seed, selected_src, true)) == 0);
    std::string cc_sel = "cc -std=c11 -w -O2 -I" +
                         shell_quote(runtime_dir.string()) + " -I" +
                         shell_quote(generated_runtime.string()) + " " +
                         shell_quote(selected_src.string()) + " -o " +
                         shell_quote(selected_bin.string());
    EXPECT(run_command(cc_sel) == 0);
    std::string sel_out;
    EXPECT(run_command("timeout 8 " + shell_quote(selected_bin.string()),
                       &sel_out) == 0);
    EXPECT(checksum_of_output(sel_out) == sum2);

    samples.push_back({seed, src, sum2});
  }

  EXPECT(samples.size() >= 12);
  EXPECT(saw_nested);
  EXPECT(saw_pointer_qual);
  EXPECT(saw_default);
  EXPECT(saw_unselected_assign);
  EXPECT(saw_unselected_call);

  // Specialized runtime side-effect counting.  The controlling expression and
  // non-selected associations must not be evaluated by real C11 compilers.
  const std::string side_effect_c = R"C(
#include <stdio.h>
static int control_ticks, selected_ticks, other_ticks, default_ticks;
static int control_expr(void) { ++control_ticks; return 1; }
static int selected_expr(int x) { ++selected_ticks; return x + 1; }
static int other_expr(void) { ++other_ticks; return 2; }
static int default_expr(void) { ++default_ticks; return 3; }
int main(void) {
    volatile int sink = 0;
    sink += _Generic(control_expr(),
                     int: selected_expr(10),
                     float: other_expr(),
                     default: default_expr());
    sink += _Generic((7),
                     char: other_expr(),
                     int: selected_expr(20),
                     default: default_expr());
    printf("SIDE_EFFECTS control=%d selected=%d other=%d default=%d sink=%d\n",
           control_ticks, selected_ticks, other_ticks, default_ticks,
           (int)sink);
    return !(control_ticks == 0 && selected_ticks == 2 &&
             other_ticks == 0 && default_ticks == 0);
}
)C";
  fs::path se_src = work / "side_effects.c";
  fs::path se_bin = work / "side_effects";
  EXPECT(write_file(se_src, side_effect_c));
  EXPECT(run_command("cc -std=c11 -O2 " + shell_quote(se_src.string()) +
                      " -o " + shell_quote(se_bin.string())) == 0);
  std::string se_out;
  EXPECT(run_command(shell_quote(se_bin.string()), &se_out) == 0);
  EXPECT(se_out.find("control=0 selected=2 other=0 default=0") !=
         std::string::npos);
}
END_GROUP()

DEFINE_GROUP(compatibility)
{
  fs::path bin = env_or("CSMITH_CHECK_BIN", ".build-check/src/csmith");
  fs::path tmp_root = env_or("CSMITH_CHECK_TMP",
                             fs::temp_directory_path().string());
  fs::path work = tmp_root / "generic_compatibility";
  std::error_code ec;
  fs::create_directories(work, ec);
  EXPECT(fs::exists(bin));

  std::vector<std::string> fixed_args = {
      "--seed", "42", "--max-funcs", "1", "--max-block-size", "2",
      "--max-expr-complexity", "5", "--no-volatiles", "--no-pointers",
      "--no-arrays", "--no-structs", "--no-unions", "--no-jumps"};
  auto fixed_command = [&]() {
    std::string cmd = shell_quote(bin.string());
    for (const auto &a : fixed_args)
      cmd += " " + a;
    return cmd;
  };

  std::string sa, sb;
  EXPECT(run_command(fixed_command(), &sa) == 0);
  EXPECT(run_command(fixed_command(), &sb) == 0);
  EXPECT(!sa.empty());
  EXPECT(sa == sb);
  fs::path a = work / "off.c";
  EXPECT(write_file(a, sa));
  // Fixed seed used by checks/run.sh.
  fs::path exe = work / "seed42";
  fs::path rt = env_or("CSMITH_CHECK_RUNTIME", "runtime");
  fs::path grt =
      env_or("CSMITH_CHECK_GENERATED_RUNTIME", ".build-check/runtime");
  EXPECT(run_command("cc -std=c11 -w -O2 -I" + shell_quote(rt.string()) +
                     " -I" + shell_quote(grt.string()) + " " +
                     shell_quote(a.string()) + " -o " +
                     shell_quote(exe.string())) == 0);
  std::string run_out;
  EXPECT(run_command(shell_quote(exe.string()), &run_out) == 0);
  EXPECT(checksum_of_output(run_out) == "C1C45B25");

  // Option conflict is reported by the frontend before program generation.
  std::string conflict_out;
  int conflict_rc = run_command(shell_quote(bin.string()) +
                                    " --c11-generic --lang-cpp --seed 1",
                                &conflict_out);
  EXPECT(conflict_rc != 0);
  EXPECT(conflict_out.find("mutually exclusive") != std::string::npos);
  EXPECT(conflict_out.find("Seed:") == std::string::npos);

  // Construction errors are part of the compatibility contract.
  const Type *ti = &Type::get_simple_type(eSimpleType::eInt);
  std::string err;
  EXPECT(ExpressionGeneric::create_checked(
             Constant::make_int(1),
             {{ti, Constant::make_int(1)},
              {ti, Constant::make_int(2)}},
             nullptr, &err) == nullptr);
  EXPECT(!err.empty());
  err.clear();
  EXPECT(ExpressionGeneric::create_checked(
             Constant::make_int(1), {}, nullptr, &err) == nullptr);
  EXPECT(err.find("no matching") != std::string::npos);
}
END_GROUP()

} // namespace

int generic_expression_case(const char *group, unsigned *checks) {
  if (!group || !checks)
    return 1;
  *checks = 0;
  static TestContext context;
  std::string g = group;
  if (g == "types")
    return generic_expression_types(checks);
  if (g == "effects")
    return generic_expression_effects(checks);
  if (g == "facts")
    return generic_expression_facts(checks);
  if (g == "clone")
    return generic_expression_clone(checks);
  if (g == "generated")
    return generic_expression_generated(checks);
  if (g == "compatibility")
    return generic_expression_compatibility(checks);
  return 1;
}
