### jsoncons::jsonpath::function_mode

```cpp
#include <jsoncons_ext/jsonpath/jsonpath.hpp>
```

```cpp
enum class function_mode
{
    legacy,
    rfc9535
};
```

`function_mode` selects how the JSONPath functions `length`, `count` and `value`
are compiled and evaluated by [make_expression](make_expression.md).

- `legacy` is the historical jsoncons behavior: `count` is an alias for `length`,
  and functions operate on JSON values only. All `make_expression` overloads that
  do not take a `function_mode` argument behave as `legacy`.

- `rfc9535` aligns the three standard functions `length`, `count` and `value`
  with RFC 9535. Function arguments and intermediate results distinguish three
  kinds: node lists, JSON values, and Nothing. Nothing is an evaluation-time
  state of the expression engine; it is distinct from JSON null, from an empty
  array, and from an empty node list.

The `rfc9535` mode supports the existing jsoncons path syntax (child, index,
wildcard, slice, union, recursive descent and filter selectors) together with
the three standard functions. It does not claim conformance of the whole
jsoncons extension syntax with RFC 9535; in particular the regular expression
functions are not part of it.

#### The standard functions in rfc9535 mode

- `count(nodes)` takes a node list produced by a query and returns the number
  of nodes. Duplicate nodes are kept, and array values held by the nodes are
  not expanded: if member `a` has the value `[1,2,3]`, `count(@.a)` is `1`,
  because the query `@.a` selects a single node.

- `value(nodes)` takes a node list and returns the value of the node if the
  list contains exactly one node, otherwise Nothing.

- `length(value)` takes a value. For a string it returns the number of Unicode
  scalar values, for an array the number of elements, for an object the number
  of members. For any other input, including Nothing, it returns Nothing.

#### Compile-time kind checking

The kind of each function argument and intermediate result is checked when the
expression is compiled, not when it is evaluated against a document. A singular
query - one built from single member or single index selectors, such as `@.a`
or `@['a'][0]` - is implicitly converted to a value where a value is required.
Queries involving wildcards, recursive descent, slices, unions or filters are
never implicitly converted, even if they happen to match exactly one node at
runtime. Consequently `length(@.a)` is valid while `length(@.*)` and
`length(@[0:1])` fail to compile; `value(@.*)` and `count(@.*)` are valid while
`count(1)` and `count(length(@.a))` are not. Nested function calls propagate
the declared result kind of the inner function. Both the throwing and the
`std::error_code` overloads of [make_expression](make_expression.md) report
these errors at compile time.

A node list used directly as a filter test is true if and only if it is
non-empty, regardless of the values it contains. A value-typed function result
may not be used directly as a filter test: `$[?value(@.*)]` and
`$[?length(@.a)]` fail to compile in `rfc9535` mode.

#### Comparing with Nothing

Nothing compares equal to Nothing and unequal to every actual JSON value
(including JSON null). The strict comparisons `<` and `>` are false whenever
either side is Nothing. `!=` is the negation of `==`, `<=` combines `<` with
`==`, and `>=` combines `>` with `==`, so `<=` and `>=` are true when both
sides are Nothing.

### Examples

```cpp
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonpath/jsonpath.hpp>

using json = jsoncons::json;
namespace jsonpath = jsoncons::jsonpath;

int main()
{
    json doc = json::parse(R"([{"a":[1,2,3]},{"a":[]},{"a":null},{}])");

    // count the selected nodes, not the array elements
    auto expr = jsonpath::make_expression<json>("$[?count(@.a) == 1]",
                                                jsonpath::function_mode::rfc9535);
    json result = expr.evaluate(doc);

    std::cout << result << "\n";
}
```
Output:
```json
[{"a":[1,2,3]},{"a":[]},{"a":null}]
```
