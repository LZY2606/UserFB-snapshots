# C11 `_Generic` expression support

`ExpressionGeneric` is Csmith's structured expression node for C11 generic
selection.  It is a normal `Expression`, so it participates in type
derivation, cloning, depth/complexity accounting, fact propagation, symbol
collection, and output.  It is not an output-only macro wrapper.

Enable generation with:

```sh
csmith --c11-generic [other Csmith options]
```

The option defaults to off.  When off, the expression probability tables are
unchanged, no generic random choices are made, and an unchanged seed/options
command keeps its previous output.  `--c11-generic` is mutually exclusive
with `--lang-cpp`; Csmith reports the option conflict before creating a
generator.

## C++ construction/inspection example

The implementation and its tests use C++20 and may include Csmith's internal
headers directly:

```cpp
#include "Constant.h"
#include "ExpressionGeneric.h"
#include "Type.h"

#include <vector>

std::string make_int_generic() {
  const Type *int_type = &Type::get_simple_type(eSimpleType::eInt);
  const Type *float_type = &Type::get_simple_type(eSimpleType::eFloat);

  std::vector<GenericAssociation> associations = {
      {int_type,   Constant::make_int(101)},
      {float_type, new Constant(float_type, "1.0f")},
  };

  // create_checked() returns nullptr (and optionally an error) for duplicate
  // compatible associations or a selection that has no match and no default.
  ExpressionGeneric *g = ExpressionGeneric::create_checked(
      Constant::make_int(7),       // controlling expression
      std::move(associations),     // ordered type associations
      Constant::make_int(0));      // optional default

  if (g == nullptr)
    return "invalid generic node";

  // Static result type is the selected branch's type, not a forced int cast.
  const Type &result = g->get_type();
  const Expression *selected = g->get_selected_expr();
  (void)result;
  (void)selected;

  std::string c_code = g->to_string();
  delete g;
  return c_code;
}
```

The printed form is:

```c
_Generic((control), type-name : (selected-when-type-matches), ...,
         default : (default-expression))
```

`clone()` performs a deep copy of the controlling expression, all
association expressions, and the optional default.

## Evaluation and facts

C11 evaluates neither the controlling expression nor non-selected
associations.  `ExpressionGeneric` follows that rule for analysis:

- `visit_facts()` visits only the selected branch.
- `get_eval_to_subexps()` returns only the selected branch's evaluation
  sources.
- `get_dereferenced_ptrs()` returns dereferences from the selected branch.
- point-to and union fact transfer descend into the selected branch.

All branches are still printed and traversed for syntactic references
(`Output`, `use_var()`, `get_called_funcs()`, and
`get_referenced_ptrs()`), so variables, functions, and declarations used only
by an unselected branch are retained.  Assignments and calls in the selected
branch are checked and merged using the ordinary Csmith rules.

## C11 type subset

Supported controlling expression types:

- integer types represented by Csmith's non-void simple types, including
  `char`, `short`, `int`, `long`, and their unsigned/fixed-width forms;
- `float`;
- object pointer types.

Explicitly rejected controls:

- arrays;
- function designators/function types;
- `struct` and `union` values;
- variably modified types.

Association/result expressions use Csmith's existing non-void scalar and
object-pointer expression subset.

Matching uses C11 compatible-type rules rather than assignment/conversion
rules.  In particular:

- `char` and `short` are not promoted to `int` merely because they are used
  in a generic selection;
- the top-level `const`/`volatile` qualification of the controlling
  expression is suppressed by lvalue conversion and does not participate in
  matching;
- qualifiers on a pointer's referenced object remain significant;
- structurally identical types reached through different typedef names are
  compatible and therefore count as duplicate associations;
- type identity is structural (`Type` kind, signedness/width, pointer chain,
  and relevant qualifiers), never a comparison of printed type-name strings.

At most one `default` is allowed.  A node with no compatible association and
no default is a construction error.

## Test hook

`--generic-selected-only-for-tests` is a deterministic test-only output
switch.  With the same seed and options as a normal
`--c11-generic` run, every `ExpressionGeneric` is printed as its selected
branch only.  This lets `tests/generic_expression.cpp` compare checksums of
the full program and the corresponding selected-branch program.
