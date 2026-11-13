#include "cases.h"

#include <cstdio>
#include <system_error>

#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonpath/jsonpath.hpp>

namespace {

using jsoncons::json;
namespace jsonpath = jsoncons::jsonpath;

#define CHECK(cond) \
    do { if (!(cond)) { std::printf("CHECK failed at line %d: %s\n", __LINE__, #cond); return 1; } } while (0)

json eval(const char* path, const json& doc)
{
    auto expr = jsonpath::make_expression<json>(path, jsonpath::function_mode::rfc9535);
    return expr.evaluate(doc);
}

bool compile_throws(const char* path)
{
    JSONCONS_TRY
    {
        auto expr = jsonpath::make_expression<json>(path, jsonpath::function_mode::rfc9535);
        (void)expr;
        return false;
    }
    JSONCONS_CATCH(const jsonpath::jsonpath_error&)
    {
        return true;
    }
}

bool compile_sets_ec(const char* path)
{
    std::error_code ec;
    auto expr = jsonpath::make_expression<json>(path, jsonpath::function_mode::rfc9535, ec);
    (void)expr;
    return static_cast<bool>(ec);
}

// Node lists: count/value/length distinguish node lists, values and Nothing
int test_node_lists()
{
    json doc = json::parse(R"([{"a":[1,2,3]},{"a":[]},{"a":null},{"a":1},{}])");

    // count counts selected nodes, it does not expand array values
    CHECK(eval("$[?count(@.a) == 1]", doc).size() == 4);
    CHECK(eval("$[?count(@.a) == 0]", doc).size() == 1);
    CHECK(eval("$[?count(@.a) == 3]", doc).size() == 0);

    // length measures the value held by the node
    {
        auto r = eval("$[?length(@.a) == 3]", doc);
        CHECK(r.size() == 1 && r[0]["a"].size() == 3);
    }
    {
        auto r = eval("$[?length(@.a) == 0]", doc);
        CHECK(r.size() == 1 && r[0]["a"].is_array() && r[0]["a"].empty());
    }
    // length of null, of a number, and of a missing member is Nothing
    CHECK(eval("$[?length(@.a) == 1]", doc).size() == 0);

    // value extracts the value of a single-node list
    {
        auto r = eval("$[?value(@.a) == null]", doc);
        CHECK(r.size() == 1 && r[0]["a"].is_null());
    }
    {
        auto r = eval("$[?value(@.a) == 1]", doc);
        CHECK(r.size() == 1 && r[0]["a"].as<int>() == 1);
    }

    // duplicate selectors keep duplicate nodes
    {
        json d2 = json::parse(R"({"a":[10,20]})");
        auto r = eval("count($.a[0,0])", d2);
        CHECK(r.size() == 1 && r[0].as<int>() == 2);
    }
    // order and duplicates of query results are preserved
    {
        json d2 = json::parse(R"({"a":[10,20]})");
        auto r = eval("$.a[1,0,1]", d2);
        CHECK(r.size() == 3 && r[0].as<int>() == 20 && r[1].as<int>() == 10 && r[2].as<int>() == 20);
    }
    // value of a multi-node list is Nothing
    {
        json d2 = json::parse(R"([{"a":1},{"a":1,"b":2}])");
        auto r = eval("$[?value(@.*) == 1]", d2);
        CHECK(r.size() == 1 && r[0].size() == 1);
    }
    // a node list used as a test is true when non-empty, even for false or null values
    {
        json d2 = json::parse(R"([{"a":false},{"a":null},{}])");
        CHECK(eval("$[?@.a]", d2).size() == 2);
    }
    // length counts Unicode scalars, including non-BMP characters
    {
        json d2 = json::parse("{\"s\":\"a\\uD83D\\uDE00b\"}");
        auto r = eval("length($.s)", d2);
        CHECK(r.size() == 1 && r[0].as<int>() == 3);
    }
    // nested filters: root and current node binding is unchanged
    {
        json d2 = json::parse(R"({"x":1,"items":[{"a":[{"b":1}]},{"a":[{"b":2}]}]})");
        auto r = eval("$.items[?@.a[?@.b == $.x]]", d2);
        CHECK(r.size() == 1 && r[0]["a"][0]["b"].as<int>() == 1);
    }
    // a compiled expression keeps no state between evaluations
    {
        auto expr = jsonpath::make_expression<json>("$[?count(@.a) == 1]", jsonpath::function_mode::rfc9535);
        json d1 = json::parse(R"([{"a":[1,2,3]},{}])");
        json d2 = json::parse(R"([{},{"a":[]}])");
        auto r1 = expr.evaluate(d1);
        auto r2 = expr.evaluate(d2);
        CHECK(r1.size() == 1 && r1[0]["a"].size() == 3);
        CHECK(r2.size() == 1 && r2[0]["a"].empty());
        CHECK(expr.evaluate(d1) == r1);
    }
    return 0;
}

// Static types: argument kinds are checked at compile time
int test_static_types()
{
    const char* bad[] = {
        "$[?length(@.*) == 1]",        // wildcard is not a singular query
        "$[?length(@[0:1]) == 1]",     // slice is not a singular query
        "$[?length(@..a) == 1]",       // recursive descent is not a singular query
        "$[?length(@['a','b']) == 1]", // union is not a singular query
        "$[?length(@[?@.b]) == 1]",    // filter is not a singular query
        "$[?count(1) == 1]",           // a literal is a value, not a node list
        "$[?count(length(@.a)) == 1]", // length declares a value result
        "$[?count(value(@.a)) == 1]",  // value declares a value result
        "$[?value(1) == 1]",           // value needs a node list
        "$[?value(@.*)]",              // value-typed result as a filter test
        "$[?length(@.a)]",             // value-typed result as a filter test
        "$[?count(@.a)]",              // value-typed result as a filter test
        "count(1)",
        "length(@.*)",
        "$[?@.* == 1]",                // non-singular query as a comparison operand
        "$[?length() == 1]",           // arity
        "$[?count(@.a, @.b) == 1]",    // arity
        "$[?unknown(@.a) == 1]"        // unknown function
    };
    for (auto path : bad)
    {
        CHECK(compile_throws(path));
        CHECK(compile_sets_ec(path));
    }

    const char* good[] = {
        "$[?length(@.a) == 1]",
        "$[?length(@['a']) == 1]",
        "$[?length(@.a[0]) == 1]",
        "$[?length(length(@.a)) == 1]",
        "$[?length(count(@.a)) == 1]",
        "$[?count(@.*) == 1]",
        "$[?count(@..a) == 1]",
        "$[?count(@[0:1]) == 1]",
        "$[?value(@.*) == 1]",
        "$[?value(@.a) == 1]",
        "$[?@.a == 1]",
        "$[?@.a]",
        "$[?@.*]",
        "$[?!@.a]",
        "$[?@.a == 1 && @.b == 2 || @.c]",
        "count($.a)",
        "value($.a)",
        "length($.a)",
        "length('abc')"
    };
    for (auto path : good)
    {
        CHECK(!compile_throws(path));
        CHECK(!compile_sets_ec(path));
    }

    // compile-time failure does not depend on the document, not even for an empty one
    {
        json empty = json::parse("{}");
        std::error_code ec;
        auto expr = jsonpath::make_expression<json>("$[?count(1) == 1]", jsonpath::function_mode::rfc9535, ec);
        CHECK(ec);
        CHECK(expr.evaluate(empty).empty());
    }
    return 0;
}

// Nothing comparisons: Nothing equals Nothing, and nothing else
int test_nothing_comparisons()
{
    json doc = json::parse(R"([{"a":1,"b":1},{"a":1,"b":2},{"a":null},{}])");

    // Nothing == Nothing
    CHECK(eval("$[?value(@.x) == value(@.y)]", doc).size() == 4);
    // Nothing is unequal to every actual JSON value, including null
    CHECK(eval("$[?value(@.x) == null]", doc).size() == 0);
    CHECK(eval("$[?value(@.x) == 1]", doc).size() == 0);
    // != is the negation of ==
    CHECK(eval("$[?value(@.x) != null]", doc).size() == 4);
    CHECK(eval("$[?value(@.x) != value(@.y)]", doc).size() == 0);
    // strict < and > are false when either side is Nothing
    CHECK(eval("$[?value(@.x) < 1]", doc).size() == 0);
    CHECK(eval("$[?value(@.x) > 1]", doc).size() == 0);
    CHECK(eval("$[?1 > value(@.x)]", doc).size() == 0);
    // <= and >= combine the strict comparison with ==
    CHECK(eval("$[?value(@.x) <= value(@.y)]", doc).size() == 4);
    CHECK(eval("$[?value(@.x) >= value(@.y)]", doc).size() == 4);
    CHECK(eval("$[?value(@.x) <= null]", doc).size() == 0);
    // JSON null is an actual value, distinct from Nothing
    CHECK(eval("$[?value(@.a) == null]", doc).size() == 1);
    // {a:1,b:2} (1 != 2) and {a:null} (null != Nothing) are selected,
    // {a:1,b:1} (1 == 1) and {} (Nothing == Nothing) are not
    CHECK(eval("$[?value(@.a) != value(@.b)]", doc).size() == 2);
    // value vs value comparisons; {} matches because Nothing == Nothing
    CHECK(eval("$[?value(@.a) == value(@.b)]", doc).size() == 2);
    CHECK(eval("$[?value(@.a) < 2]", doc).size() == 2);
    CHECK(eval("$[?value(@.a) <= 1]", doc).size() == 2);
    CHECK(eval("$[?value(@.b) > 1]", doc).size() == 1);
    // length of a missing member is Nothing
    CHECK(eval("$[?length(@.x) == length(@.y)]", doc).size() == 4);
    CHECK(eval("$[?length(@.x) == 0]", doc).size() == 0);
    return 0;
}

// Legacy functions: the historical behavior is unchanged
int test_legacy_functions()
{
    json doc = json::parse(R"([{"a":[1,2,3]},{"a":[]},{"a":null},{}])");

    // legacy count is an alias for length over the matched values
    {
        auto expr = jsonpath::make_expression<json>("$[?count(@.a) == 3]");
        auto r = expr.evaluate(doc);
        CHECK(r.size() == 1 && r[0]["a"].size() == 3);
    }
    {
        auto r = jsonpath::json_query(doc, "$[?length(@.a) == 0]");
        CHECK(r.size() == 1 && r[0]["a"].is_array());
    }
    // the explicit legacy mode overload keeps the old results
    {
        auto expr = jsonpath::make_expression<json>("$[?count(@.a) == 3]", jsonpath::function_mode::legacy);
        auto r = expr.evaluate(doc);
        CHECK(r.size() == 1 && r[0]["a"].size() == 3);
    }
    // legacy count expands the array held by the matched node, rfc9535 count does not
    {
        json d2 = json::parse(R"([{"a":[5]},{"a":[1,2]}])");
        auto legacy_expr = jsonpath::make_expression<json>("$[?count(@.a) == 1]");
        CHECK(legacy_expr.evaluate(d2).size() == 1);
        auto rfc_expr = jsonpath::make_expression<json>("$[?count(@.a) == 1]", jsonpath::function_mode::rfc9535);
        CHECK(rfc_expr.evaluate(d2).size() == 2);
    }
    // the legacy error_code overload still works
    {
        std::error_code ec;
        auto expr = jsonpath::make_expression<json>("$[?count(@.a) == 3]", ec);
        CHECK(!ec);
        CHECK(expr.evaluate(doc).size() == 1);
    }
    return 0;
}

} // namespace

void task_register_tests(void)
{
    task_case("NodeLists", test_node_lists);
    task_case("StaticTypes", test_static_types);
    task_case("NothingComparisons", test_nothing_comparisons);
    task_case("LegacyFunctions", test_legacy_functions);
}
