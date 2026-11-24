#include <config.h>

#include "ExpressionGeneric.h"

#include "CGContext.h"
#include "CGOptions.h"
#include "Constant.h"
#include "Error.h"
#include "ExpressionAssign.h"
#include "ExpressionComma.h"
#include "ExpressionFuncall.h"
#include "ExpressionVariable.h"
#include "FactMgr.h"
#include "Function.h"
#include "FunctionInvocationUser.h"
#include "Type.h"
#include "Variable.h"
#include "VariableSelector.h"
#include "util.h"

#include <algorithm>
#include <sstream>

bool ExpressionGeneric::output_selected_only_ = false;

namespace {

std::size_t type_indirect_level(const Type *t) {
  return t ? static_cast<std::size_t>(t->get_indirect_level()) : 0;
}

// Structural identity.  Csmith's Type pointer identity is canonical for most
// built-ins, but deterministic tests may allocate another structurally
// identical type to stand in for a differently named typedef.
bool canonical_type_equal(const Type *a, const Type *b) {
  if (a == b)
    return true;
  if (!a || !b || a->eType != b->eType)
    return false;

  if (!a->dimensions.empty() || !b->dimensions.empty())
    return a->dimensions == b->dimensions;

  switch (a->eType) {
  case eTypeDesc::eSimple:
    // float must not collapse with a same-size signed integer.
    if (a->simple_type == eSimpleType::eFloat ||
        b->simple_type == eSimpleType::eFloat)
      return a->simple_type == b->simple_type;
    // Csmith prints same-width signed integers through the same stdint typedef.
    // C11 compatibility therefore follows signedness/size rather than the
    // internal enum spelling.  Char/short remain distinct by size and are not
    // promoted here.
    return a->is_signed() == b->is_signed() &&
           a->SizeInBytes() == b->SizeInBytes();
  case eTypeDesc::ePointer:
    return canonical_type_equal(a->ptr_type, b->ptr_type);
  case eTypeDesc::eStruct:
  case eTypeDesc::eUnion:
    // Struct/union compatibility is identity based in C and in Csmith's SID
    // model.  They are not supported as controlling types anyway.
    return a->sid == b->sid;
  }
  return false;
}

CVQualifiers normalized_qualifiers(const Type *t, CVQualifiers q) {
  const std::size_t expected = type_indirect_level(t) + 1;
  const auto &cs = q.get_consts();
  const auto &vs = q.get_volatiles();
  if (cs.size() == expected && vs.size() == expected)
    return q;

  std::vector<bool> nc(expected, false), nv(expected, false);
  for (std::size_t i = 0; i < expected; ++i) {
    // Best-effort alignment for hand-built qualifier vectors.  Ordinary
    // internal callers always provide an expected-length vector.
    if (i < cs.size())
      nc[i] = cs[i];
    if (i < vs.size())
      nv[i] = vs[i];
  }
  CVQualifiers result(nc, nv);
  result.wildcard = q.wildcard;
  result.accept_stricter = q.accept_stricter;
  return result;
}

bool qualifiers_equal_below_top(const Type *t, const CVQualifiers &cq,
                                const CVQualifiers &aq) {
  const std::size_t n = type_indirect_level(t) + 1;
  CVQualifiers c = normalized_qualifiers(t, cq);
  CVQualifiers a = normalized_qualifiers(t, aq);
  const auto &cc = c.get_consts();
  const auto &cv = c.get_volatiles();
  const auto &ac = a.get_consts();
  const auto &av = a.get_volatiles();
  if (cc.size() != n || cv.size() != n || ac.size() != n || av.size() != n)
    return false;
  // The last entry qualifies the outermost level (either the scalar itself or
  // the pointer variable).  It is ignored for the controlling expression.
  for (std::size_t i = 0; i + 1 < n; ++i) {
    if (cc[i] != ac[i] || cv[i] != av[i])
      return false;
  }
  return true;
}

bool all_qualifiers_equal(const Type *t, const CVQualifiers &a,
                          const CVQualifiers &b) {
  const std::size_t n = type_indirect_level(t) + 1;
  CVQualifiers x = normalized_qualifiers(t, a);
  CVQualifiers y = normalized_qualifiers(t, b);
  return x.get_consts() == y.get_consts() &&
         x.get_volatiles() == y.get_volatiles() && x.get_consts().size() == n;
}

void output_type_name(std::ostream &out, const Type *t,
                      const CVQualifiers &q) {
  if (!t) {
    out << "int";
    return;
  }
  CVQualifiers qf = normalized_qualifiers(t, q);
  const Type *base = t->get_base_type();
  const auto &cs = qf.get_consts();
  const auto &vs = qf.get_volatiles();
  if (!cs.empty() && cs[0])
    out << "const ";
  if (!vs.empty() && vs[0])
    out << "volatile ";
  if (base)
    base->Output(out);
  else
    t->Output(out);

  const std::size_t levels = type_indirect_level(t);
  for (std::size_t i = 1; i <= levels; ++i) {
    out << "*";
    if (i < cs.size() && cs[i])
      out << " const";
    if (i < vs.size() && vs[i])
      out << " volatile";
  }
}

const Type *choose_extra_assoc_type(const Type *control,
                                    std::size_t salt) {
  static const eSimpleType ints[] = {
      eSimpleType::eInt,  eSimpleType::eUInt,  eSimpleType::eChar,
      eSimpleType::eShort, eSimpleType::eLong, eSimpleType::eUShort};
  if ((salt & 3u) == 0 && CGOptions::pointers()) {
    const Type *base = &Type::get_simple_type(eSimpleType::eInt);
    return Type::find_pointer_type(base, true);
  }
  eSimpleType st = ints[salt % (sizeof(ints) / sizeof(ints[0]))];
  return &Type::get_simple_type(st);
}

} // namespace

bool ExpressionGeneric::supported_control_type(const Type *t) {
  if (!t)
    return false;
  // Arrays (including Csmith's encoded array dimensions) are explicitly
  // rejected.  Csmith does not have a separate function-designator Type
  // descriptor; function controls are avoided by the random factory and
  // explicit callers can mark such a node invalid through construction.
  if (!t->dimensions.empty())
    return false;
  if (t->eType == eTypeDesc::eStruct || t->eType == eTypeDesc::eUnion)
    return false;
  if (t->eType == eTypeDesc::eSimple)
    return t->simple_type != eSimpleType::eVoid;
  if (t->eType == eTypeDesc::ePointer)
    return t->ptr_type != nullptr; // object pointer subset
  return false;
}

bool ExpressionGeneric::supported_result_type(const Type &t) {
  if (t.eType == eTypeDesc::eSimple)
    return t.simple_type != eSimpleType::eVoid;
  return t.eType == eTypeDesc::ePointer && t.ptr_type != nullptr;
}

CVQualifiers ExpressionGeneric::canonical_qualifiers(const Type *t) {
  return normalized_qualifiers(t, CVQualifiers());
}

CVQualifiers
ExpressionGeneric::strip_top_level_qualifiers(const Type *t,
                                              CVQualifiers q) {
  q = normalized_qualifiers(t, q);
  auto cs = q.get_consts();
  auto vs = q.get_volatiles();
  if (!cs.empty())
    cs.back() = false;
  if (!vs.empty())
    vs.back() = false;
  CVQualifiers result(cs, vs);
  result.wildcard = q.wildcard;
  result.accept_stricter = q.accept_stricter;
  return result;
}

bool ExpressionGeneric::c11_types_compatible(
    const Type *control_type, const CVQualifiers &control_q,
    const Type *assoc_type, const CVQualifiers &assoc_q) {
  if (!canonical_type_equal(control_type, assoc_type))
    return false;
  if (!control_type)
    return false;
  // Top-level control CV is suppressed by lvalue conversion.  Pointer-target
  // CV entries remain in the comparison.
  CVQualifiers stripped_control =
      strip_top_level_qualifiers(control_type, control_q);
  return qualifiers_equal_below_top(control_type, stripped_control, assoc_q);
}

bool ExpressionGeneric::c11_types_compatible(const Type *control_type,
                                             const Type *assoc_type) {
  if (!control_type || !assoc_type ||
      !canonical_type_equal(control_type, assoc_type))
    return false;
  CVQualifiers qc = canonical_qualifiers(control_type);
  CVQualifiers qa = canonical_qualifiers(assoc_type);
  CVQualifiers sc = strip_top_level_qualifiers(control_type, qc);
  return qualifiers_equal_below_top(control_type, sc, qa);
}

ExpressionGeneric *ExpressionGeneric::create_checked(
    const Expression *control,
    std::vector<GenericAssociation> associations,
    const Expression *default_expr, std::string *error) {
  auto *node = new ExpressionGeneric(control, std::move(associations),
                                     default_expr);
  if (!node->valid()) {
    if (error)
      *error = node->construction_error();
    delete node;
    return nullptr;
  }
  if (error)
    error->clear();
  return node;
}

ExpressionGeneric *ExpressionGeneric::create_checked(
    const Expression &control,
    std::vector<GenericAssociation> associations,
    const Expression &default_expr, std::string *error) {
  return create_checked(&control, std::move(associations), &default_expr,
                        error);
}

ExpressionGeneric *ExpressionGeneric::create_checked(
    const Expression &control,
    std::vector<GenericAssociation> associations, std::string *error) {
  return create_checked(&control, std::move(associations), nullptr, error);
}

ExpressionGeneric *ExpressionGeneric::create_from_pairs(
    const Expression *control,
    const std::vector<TypeExpressionPair> &associations,
    const Expression *default_expr, std::string *error) {
  std::vector<GenericAssociation> converted;
  converted.reserve(associations.size());
  for (const auto &a : associations)
    converted.emplace_back(a.first, a.second,
                           canonical_qualifiers(a.first));
  return create_checked(control, std::move(converted), default_expr, error);
}

ExpressionGeneric *ExpressionGeneric::create_from_pairs(
    const Expression *control,
    const std::vector<std::pair<const Type *, Expression *>> &associations,
    Expression *default_expr, std::string *error) {
  std::vector<GenericAssociation> converted;
  converted.reserve(associations.size());
  for (const auto &a : associations)
    converted.emplace_back(a.first, a.second,
                           canonical_qualifiers(a.first));
  return create_checked(control, std::move(converted), default_expr,
                        error);
}

ExpressionGeneric::ExpressionGeneric(
    const Expression *control,
    std::vector<GenericAssociation> associations,
    const Expression *default_expr)
    : Expression(eTermType::eGeneric), control_(control),
      associations_(std::move(associations)),
      default_expr_(default_expr), selected_(npos),
      selected_expr_(nullptr), valid_(true) {
  validate_and_select();
}

ExpressionGeneric::ExpressionGeneric(const ExpressionGeneric &other)
    : Expression(other), control_(other.control_->clone()),
      default_expr_(other.default_expr_ ? other.default_expr_->clone()
                                        : nullptr),
      selected_(other.selected_), selected_expr_(nullptr),
      valid_(other.valid_), error_(other.error_) {
  associations_.reserve(other.associations_.size());
  for (const auto &a : other.associations_) {
    associations_.push_back(
        GenericAssociation(a.type, a.expr ? a.expr->clone() : nullptr,
                           a.qualifiers));
  }
  if (selected_ != npos && selected_ < associations_.size())
    selected_expr_ = associations_[selected_].expr;
  else
    selected_expr_ = default_expr_;
}

ExpressionGeneric::~ExpressionGeneric(void) {
  delete control_;
  for (auto &a : associations_)
    delete a.expr;
  delete default_expr_;
}

Expression *ExpressionGeneric::clone(void) const {
  return new ExpressionGeneric(*this);
}

void ExpressionGeneric::validate_and_select(void) {
  auto fail = [&](const std::string &msg) {
    valid_ = false;
    error_ = msg;
    selected_ = npos;
    selected_expr_ = default_expr_;
    if (!selected_expr_ && !associations_.empty())
      selected_expr_ = associations_[0].expr;
  };

  if (!control_) {
    fail("generic selection requires a controlling expression");
    return;
  }
  const Type &ct = control_->get_type();
  if (const auto *ev =
          dynamic_cast<const ExpressionVariable *>(control_)) {
    const Variable *v = ev->get_var();
    if (v && v->isArray) {
      fail("array controlling expressions are not supported");
      return;
    }
  }
  if (!supported_control_type(&ct)) {
    fail("unsupported _Generic controlling expression type");
    return;
  }

  for (const auto &a : associations_) {
    if (!a.type || !a.expr) {
      fail("generic association is missing a type or expression");
      return;
    }
    if (!supported_result_type(a.expr->get_type())) {
      fail("generic association result has an unsupported type");
      return;
    }
  }
  if (default_expr_ && !supported_result_type(default_expr_->get_type())) {
    fail("generic default result has an unsupported type");
    return;
  }

  // Duplicate associations by C11 structural compatibility.  String type
  // names are deliberately never used as identity.
  for (std::size_t i = 0; i < associations_.size(); ++i) {
    for (std::size_t j = i + 1; j < associations_.size(); ++j) {
      const auto &a = associations_[i];
      const auto &b = associations_[j];
      if (canonical_type_equal(a.type, b.type) &&
          all_qualifiers_equal(a.type, a.qualifiers, b.qualifiers)) {
        fail("duplicate compatible _Generic association types");
        return;
      }
    }
  }

  CVQualifiers cq = control_->get_qualifiers();
  for (std::size_t i = 0; i < associations_.size(); ++i) {
    const auto &a = associations_[i];
    if (c11_types_compatible(&ct, cq, a.type, a.qualifiers)) {
      selected_ = i;
      selected_expr_ = a.expr;
      valid_ = true;
      error_.clear();
      return;
    }
  }

  if (default_expr_) {
    selected_ = npos;
    selected_expr_ = default_expr_;
    valid_ = true;
    error_.clear();
    return;
  }

  fail("_Generic has no matching association and no default");
}

const Type &ExpressionGeneric::get_type(void) const {
  if (selected_expr_)
    return selected_expr_->get_type();
  return Type::get_simple_type(eSimpleType::eInt);
}

CVQualifiers ExpressionGeneric::get_qualifiers(void) const {
  return selected_expr_ ? selected_expr_->get_qualifiers()
                        : canonical_qualifiers(&get_type());
}

void ExpressionGeneric::get_eval_to_subexps(
    vector<const Expression *> &subs) const {
  // Evaluation-source queries follow only the selected runtime branch.
  if (selected_expr_)
    selected_expr_->get_eval_to_subexps(subs);
}

void ExpressionGeneric::get_called_funcs(
    std::vector<const FunctionInvocationUser *> &funcs) const {
  // Every branch is syntactically present, so retain calls from all branches.
  if (control_)
    control_->get_called_funcs(funcs);
  for (const auto &a : associations_)
    a.expr->get_called_funcs(funcs);
  if (default_expr_)
    default_expr_->get_called_funcs(funcs);
}

bool ExpressionGeneric::visit_facts(vector<const Fact *> &inputs,
                                    CGContext &cg_context) const {
  // C11 evaluates neither the controlling expression nor a non-selected
  // association.  Propagate facts/effects solely from the selected branch.
  if (!selected_expr_)
    return log_analysis_fail("ExpressionGeneric without selected branch");
  if (!selected_expr_->visit_facts(inputs, cg_context))
    return log_analysis_fail("ExpressionGeneric selected branch");
  return true;
}

bool ExpressionGeneric::has_uncertain_call_recursive(void) const {
  // Syntactic uncertainty from unselected branches is safe to retain; the
  // runtime fact path itself only visits the selected branch.
  if (control_ && control_->has_uncertain_call_recursive())
    return true;
  for (const auto &a : associations_) {
    if (a.expr->has_uncertain_call_recursive())
      return true;
  }
  return default_expr_ && default_expr_->has_uncertain_call_recursive();
}

bool ExpressionGeneric::use_var(const Variable *v) const {
  if (control_ && control_->use_var(v))
    return true;
  for (const auto &a : associations_) {
    if (a.expr->use_var(v))
      return true;
  }
  return default_expr_ && default_expr_->use_var(v);
}

std::vector<const ExpressionVariable *>
ExpressionGeneric::get_dereferenced_ptrs(void) const {
  if (selected_expr_)
    return selected_expr_->get_dereferenced_ptrs();
  return {};
}

void ExpressionGeneric::get_referenced_ptrs(
    std::vector<const Variable *> &ptrs) const {
  // Preserve syntactic pointer references from every printed branch.
  if (control_)
    control_->get_referenced_ptrs(ptrs);
  for (const auto &a : associations_)
    a.expr->get_referenced_ptrs(ptrs);
  if (default_expr_)
    default_expr_->get_referenced_ptrs(ptrs);
}

unsigned int ExpressionGeneric::get_complexity(void) const {
  unsigned int n = control_ ? control_->get_complexity() : 0;
  for (const auto &a : associations_)
    n += a.expr->get_complexity();
  if (default_expr_)
    n += default_expr_->get_complexity();
  return n + 1;
}

void ExpressionGeneric::output_association_type(
    std::ostream &out, const GenericAssociation &assoc) const {
  output_type_name(out, assoc.type, assoc.qualifiers);
}

void ExpressionGeneric::OutputSelectedOnly(std::ostream &out) const {
  output_cast(out);
  if (selected_expr_)
    selected_expr_->Output(out);
  else
    out << "0";
}

std::string ExpressionGeneric::selected_only_string(void) const {
  std::ostringstream oss;
  OutputSelectedOnly(oss);
  return oss.str();
}

void ExpressionGeneric::Output(std::ostream &out) const {
  output_cast(out);
  if (output_selected_only_) {
    OutputSelectedOnly(out);
    return;
  }

  out << "_Generic((";
  if (control_)
    control_->Output(out);
  else
    out << "0";
  out << ")";

  for (const auto &a : associations_) {
    out << ", ";
    output_association_type(out, a);
    out << " : (";
    a.expr->Output(out);
    out << ")";
  }
  if (default_expr_) {
    out << ", default : (";
    default_expr_->Output(out);
    out << ")";
  }
  out << ")";
}

void ExpressionGeneric::indented_output(std::ostream &out, int indent) const {
  output_tab(out, indent);
  Output(out);
}

/*
 * Random construction.  Non-selected branches are generated against fact and
 * effect snapshots, then rolled back for analysis purposes.  The expressions,
 * variables and callees themselves remain owned by the tree/global tables, so
 * all printed declarations stay available.
 */
ExpressionGeneric *ExpressionGeneric::make_random(CGContext &cg_context,
                                                  const Type *requested,
                                                  const CVQualifiers *) {
  FactMgr *fm = get_fact_mgr(&cg_context);

  // Choose a supported controlling type.  Prefer existing canonical simple
  // types and the canonical int object-pointer type.
  static const eSimpleType control_simples[] = {
      eSimpleType::eInt,   eSimpleType::eUInt,  eSimpleType::eChar,
      eSimpleType::eShort, eSimpleType::eLong,  eSimpleType::eUShort,
      eSimpleType::eFloat};
  std::size_t kind = rnd_upto(7);
  const Type *control_type;
  CVQualifiers control_q;
  if (kind == 6 && !CGOptions::enable_float())
    control_type = get_int_type();
  else if (kind < 6 || (kind == 6 && CGOptions::enable_float()))
    control_type = &Type::get_simple_type(
        control_simples[kind < 7 ? kind : 0]);
  else
    control_type = get_int_type();

  if ((rnd_upto(4) == 0) && CGOptions::pointers()) {
    control_type = Type::find_pointer_type(get_int_type(), true);
    control_q = canonical_qualifiers(control_type);
    if (CGOptions::consts() && rnd_flipcoin(40)) {
      // Pointer-target qualifier: must participate in compatibility.
      control_q.set_const(true, 0);
    }
  } else {
    control_q = canonical_qualifiers(control_type);
  }

  FactVec saved_facts;
  Effect saved_accum = cg_context.get_accum_effect();
  Effect saved_stm = cg_context.get_effect_stm();
  if (fm)
    saved_facts = fm->global_facts;

  // Build an exact-type global controller.  Going through flexible variable
  // selection can produce an ExpressionVariable whose modeled type differs
  // from the type printed after dereference; that is safe in ordinary Csmith
  // contexts but invalid for exact _Generic matching.  A fresh global gives
  // the node and C compiler one identical static type.
  CVQualifiers var_q = control_q;
  if (var_q.get_consts().empty())
    var_q = canonical_qualifiers(control_type);
  Expression *init = Constant::make_random(control_type);
  Variable *control_var = VariableSelector::new_variable(
      gensym("g_generic_ctrl_"), control_type, init, &var_q);
  VariableSelector::GetGlobalVariables()->push_back(control_var);
  if (fm)
    fm->add_new_var_fact_and_update_inout_maps(nullptr,
                                               control_var->get_collective());
  Expression *control = new ExpressionVariable(*control_var);

  // The controlling expression is never evaluated: discard read effects/facts
  // accumulated while selecting/building it.  restore_facts retains facts for
  // newly created declarations but drops speculative evaluation changes.
  if (fm)
    fm->restore_facts(saved_facts);
  cg_context.reset_effect_accum(saved_accum);
  cg_context.reset_effect_stm(saved_stm);

  if (!control)
    return nullptr;

  // Recover the actual static type/qualifiers (ExpressionVariable can model a
  // dereference even when asked for a type).
  control_type = &control->get_type();
  control_q = control->get_qualifiers();
  if (!supported_control_type(control_type)) {
    delete control;
    return nullptr;
  }

  const Type *result_type = requested;
  if (!result_type || !supported_result_type(*result_type))
    result_type = get_int_type();

  // Random nodes always retain a default as a compiler-visible safety net for
  // Csmith's flexible type derivations.  Explicit deterministic construction
  // still exercises exact matching and the no-default construction error.
  const bool use_default = true;
  const bool matching_assoc = rnd_flipcoin(70);
  const unsigned extra_count = 2 + rnd_upto(3); // 2..4 extras

  std::vector<GenericAssociation> assocs;
  assocs.reserve(extra_count + 1);

  auto is_duplicate_type = [&](const Type *t,
                               const CVQualifiers &q) -> bool {
    for (const auto &a : assocs) {
      if (canonical_type_equal(a.type, t) &&
          all_qualifiers_equal(t, a.qualifiers, q))
        return true;
    }
    return false;
  };

  auto make_branch_expr = [&](const Type *branch_type,
                              bool selected_branch) -> Expression * {
    FactVec branch_facts;
    Effect branch_accum = cg_context.get_accum_effect();
    Effect branch_stm = cg_context.get_effect_stm();
    if (fm)
      branch_facts = fm->global_facts;

    Expression *e = Expression::make_random(
        cg_context, branch_type, nullptr, false, false);
    if (!e || !supported_result_type(e->get_type())) {
      if (e)
        delete e;
      Error::set_error(SUCCESS);
      e = Constant::make_random(branch_type);
    }
    if (!selected_branch) {
      if (fm)
        fm->restore_facts(branch_facts);
      cg_context.reset_effect_accum(branch_accum);
      cg_context.reset_effect_stm(branch_stm);
    }
    return e;
  };

  // Put the matching association at a varied position.
  const std::size_t match_pos = rnd_upto(extra_count + 1);
  bool inserted_match = false;
  for (std::size_t i = 0; i <= extra_count; ++i) {
    if (matching_assoc && i == match_pos) {
      CVQualifiers aq =
          strip_top_level_qualifiers(control_type, control_q);
      // The stored association should have a normal top level; stripping set
      // the outermost CV to false, which is exactly the selectable type name.
      Expression *e = make_branch_expr(result_type, true);
      assocs.push_back(GenericAssociation(control_type, e, aq));
      inserted_match = true;
      continue;
    }

    std::size_t salt = i + static_cast<std::size_t>(rnd_upto(100));
    const Type *at = nullptr;
    CVQualifiers aq;
    for (unsigned attempt = 0; attempt < 12; ++attempt) {
      const Type *candidate = choose_extra_assoc_type(
          control_type, salt + attempt * 7);
      CVQualifiers cq = canonical_qualifiers(candidate);
      CVQualifiers match_q =
          strip_top_level_qualifiers(control_type, control_q);
      if (matching_assoc &&
          c11_types_compatible(control_type, control_q, candidate, cq))
        continue;
      (void)match_q;
      if (!is_duplicate_type(candidate, cq)) {
        at = candidate;
        aq = cq;
        break;
      }
    }
    if (!at) {
      static const eSimpleType fallback_simples[] = {
          eSimpleType::eInt,  eSimpleType::eUInt, eSimpleType::eChar,
          eSimpleType::eShort};
      for (eSimpleType st : fallback_simples) {
        const Type *candidate = &Type::get_simple_type(st);
        CVQualifiers cq = canonical_qualifiers(candidate);
        if (matching_assoc &&
            c11_types_compatible(control_type, control_q, candidate, cq))
          continue;
        if (!is_duplicate_type(candidate, cq)) {
          at = candidate;
          aq = cq;
          break;
        }
      }
    }
    if (!at)
      continue;
    (void)salt;
    // Every printed association expression is generated with the type the
    // parent expects.  This keeps the tree well-typed even when Csmith's
    // flexible derivations make the compiler choose a different legal
    // association than the internal structural predictor.
    Expression *e = make_branch_expr(result_type, false);
    assocs.push_back(GenericAssociation(at, e, aq));
  }

  Expression *dflt = nullptr;
  if (use_default) {
    // If there is no matching association, default is selected and must carry
    // the requested result type.  Otherwise it is an unselected branch.
    dflt = make_branch_expr(result_type, !matching_assoc);
  }

  std::string error;
  ExpressionGeneric *node =
      create_checked(control, std::move(assocs), dflt, &error);
  if (!node) {
    // create_checked() destroyed the invalid node and all owned children.
    // Build a minimal valid generic rather than returning null through
    // Expression's term-type post-processing.
    Error::set_error(SUCCESS);
    return create_checked(
        Constant::make_int(0),
        {{get_int_type(), Constant::make_int(0)}},
        Constant::make_int(0));
  }
  (void)inserted_match;
  return node;
}
