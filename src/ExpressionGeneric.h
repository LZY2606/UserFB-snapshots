// -*- mode: C++ -*-
//
// C11 _Generic expression node.  Unlike ordinary branching expressions, only
// the branch selected by the controlling expression is evaluated at runtime
// (and during fact propagation).  All branches remain syntactic children so
// that their declarations and output are retained.

#ifndef EXPRESSION_GENERIC_H
#define EXPRESSION_GENERIC_H

#include "CVQualifiers.h"
#include "Expression.h"
#include "Type.h"

#include <cstddef>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

struct GenericAssociation {
  GenericAssociation(void) : type(nullptr), expr(nullptr) {}

  GenericAssociation(const Type *t, const Expression *e,
                     CVQualifiers q = CVQualifiers())
      : type(t), qualifiers(std::move(q)), expr(e) {}

  GenericAssociation(const Type *t, Expression *e,
                     CVQualifiers q = CVQualifiers())
      : type(t), qualifiers(std::move(q)), expr(e) {}

  const Type *type;
  CVQualifiers qualifiers;
  const Expression *expr;
};

class ExpressionGeneric : public Expression {
public:
  using Association = GenericAssociation;

  static ExpressionGeneric *make_random(CGContext &cg_context,
                                        const Type *type,
                                        const CVQualifiers *qfer = nullptr);

  // Explicit deterministic construction entry.  Invalid constructs yield
  // nullptr and, when supplied, a human-readable reason.
  static ExpressionGeneric *
  create_checked(const Expression *control,
                 std::vector<GenericAssociation> associations,
                 const Expression *default_expr = nullptr,
                 std::string *error = nullptr);

  using TypeExpressionPair = std::pair<const Type *, const Expression *>;
  static ExpressionGeneric *
  create_from_pairs(const Expression *control,
                    const std::vector<TypeExpressionPair> &associations,
                    const Expression *default_expr = nullptr,
                    std::string *error = nullptr);
  static ExpressionGeneric *
  create_from_pairs(
      const Expression *control,
      const std::vector<std::pair<const Type *, Expression *>> &associations,
      Expression *default_expr = nullptr, std::string *error = nullptr);
  static ExpressionGeneric *
  create_checked(const Expression &control,
                 std::vector<GenericAssociation> associations,
                 const Expression &default_expr,
                 std::string *error = nullptr);
  static ExpressionGeneric *
  create_checked(const Expression &control,
                 std::vector<GenericAssociation> associations,
                 std::string *error = nullptr);

  // Short aliases for deterministic/test callers.
  static ExpressionGeneric *
  construct(const Expression *control,
            std::vector<GenericAssociation> associations,
            const Expression *default_expr = nullptr) {
    return create_checked(control, std::move(associations), default_expr);
  }
  static ExpressionGeneric *
  make_checked(const Expression *control,
               std::vector<GenericAssociation> associations,
               const Expression *default_expr = nullptr,
               std::string *error = nullptr) {
    return create_checked(control, std::move(associations), default_expr,
                          error);
  }

  ExpressionGeneric(const Expression *control,
                    std::vector<GenericAssociation> associations,
                    const Expression *default_expr = nullptr);
  virtual ~ExpressionGeneric(void) override;

  virtual Expression *clone(void) const override;

  virtual const Type &get_type(void) const override;
  virtual CVQualifiers get_qualifiers(void) const override;

  virtual void
  get_eval_to_subexps(vector<const Expression *> &subs) const override;

  virtual void get_called_funcs(
      std::vector<const FunctionInvocationUser *> &funcs) const override;

  virtual bool visit_facts(vector<const Fact *> &inputs,
                           CGContext &cg_context) const override;

  virtual bool has_uncertain_call_recursive(void) const override;
  virtual bool use_var(const Variable *v) const override;

  virtual std::vector<const ExpressionVariable *>
  get_dereferenced_ptrs(void) const override;
  virtual void
  get_referenced_ptrs(std::vector<const Variable *> &ptrs) const override;

  virtual unsigned int get_complexity(void) const override;

  virtual void Output(std::ostream &out) const override;
  virtual void indented_output(std::ostream &out, int indent) const override;

  // Deterministic inspection interface.
  const Expression *get_control(void) const { return control_; }
  const Expression *get_control_expr(void) const { return control_; }
  const Expression *get_controlling_expr(void) const { return control_; }
  const Expression *get_controlling_expression(void) const {
    return control_;
  }
  const std::vector<GenericAssociation> &associations(void) const {
    return associations_;
  }
  const std::vector<GenericAssociation> &get_associations(void) const {
    return associations_;
  }
  std::size_t get_num_associations(void) const {
    return associations_.size();
  }
  std::size_t association_count(void) const { return associations_.size(); }
  const Type *get_association_type(std::size_t i) const {
    return i < associations_.size() ? associations_[i].type : nullptr;
  }
  const Expression *get_association_expr(std::size_t i) const {
    return i < associations_.size() ? associations_[i].expr : nullptr;
  }
  const Expression *get_default(void) const { return default_expr_; }
  const Expression *get_default_expr(void) const { return default_expr_; }
  const Expression *get_default_association(void) const {
    return default_expr_;
  }
  bool has_default(void) const { return default_expr_ != nullptr; }

  // Index of the matching type association, or npos when default is selected.
  std::size_t selected_index(void) const { return selected_; }
  std::size_t get_selected_index(void) const { return selected_; }
  std::size_t selected_association_index(void) const { return selected_; }
  bool default_selected(void) const {
    return selected_ == npos && default_expr_ != nullptr;
  }
  const Expression *get_selected_expr(void) const { return selected_expr_; }
  const Expression *get_selected(void) const { return selected_expr_; }
  const Expression *selected_expression(void) const {
    return selected_expr_;
  }

  bool valid(void) const { return valid_; }
  bool is_valid(void) const { return valid_; }
  const std::string &construction_error(void) const { return error_; }
  const std::string &get_construction_error(void) const { return error_; }

  void OutputSelectedOnly(std::ostream &out) const;
  std::string selected_only_string(void) const;

  // Test/output hook used to compare a generated tree with its selected branch.
  static void set_output_selected_only(bool b) {
    output_selected_only_ = b;
  }
  static bool output_selected_only(void) { return output_selected_only_; }

  // Strict C11 compatibility.  Top-level control qualifiers are removed by
  // these helpers; qualifiers below the first pointer level remain significant.
  static bool supported_control_type(const Type *t);
  static bool supported_result_type(const Type &t);
  static CVQualifiers canonical_qualifiers(const Type *t);
  static CVQualifiers strip_top_level_qualifiers(const Type *t,
                                                 CVQualifiers q);
  static bool c11_types_compatible(const Type *control_type,
                                   const CVQualifiers &control_q,
                                   const Type *assoc_type,
                                   const CVQualifiers &assoc_q);
  static bool c11_types_compatible(const Type *control_type,
                                   const Type *assoc_type);
  static bool types_are_compatible(const Type *a, const Type *b) {
    return c11_types_compatible(a, b);
  }

  static constexpr std::size_t npos = static_cast<std::size_t>(-1);

private:
  ExpressionGeneric(const ExpressionGeneric &other);
  ExpressionGeneric &operator=(const ExpressionGeneric &) = delete;

  void validate_and_select(void);
  void output_association_type(std::ostream &out,
                               const GenericAssociation &assoc) const;

  const Expression *control_;
  std::vector<GenericAssociation> associations_;
  const Expression *default_expr_;
  std::size_t selected_;
  const Expression *selected_expr_;
  bool valid_;
  std::string error_;

  static bool output_selected_only_;
};

#endif // EXPRESSION_GENERIC_H
