// Copyright 2013-2026 Daniel Parker
// Distributed under the Boost license, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// See https://github.com/danielaparker/jsoncons for latest version

#ifndef JSONCONS_EXT_JSONPATH_JSONPATH_FUNCTION_MODE_HPP
#define JSONCONS_EXT_JSONPATH_JSONPATH_FUNCTION_MODE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <jsoncons/nonstd/compiler_support.hpp>
#include <jsoncons/nonstd/optional.hpp>
#include <jsoncons/config/jsoncons_config.hpp>
#include <jsoncons/json_decoder.hpp>
#include <jsoncons/json_parser.hpp>
#include <jsoncons/json_type.hpp>
#include <jsoncons/semantic_tag.hpp>
#include <jsoncons/utility/unicode_traits.hpp>

#include <jsoncons_ext/jsonpath/jsonpath_error.hpp>

namespace jsoncons {
namespace jsonpath {

    // Selects how the JSONPath functions 'length', 'count' and 'value' are
    // compiled and evaluated.
    enum class function_mode
    {
        legacy,   // Historical jsoncons behavior, 'count' is an alias for 'length'
        rfc9535   // Function semantics aligned with RFC 9535: function arguments and
                  // intermediate results distinguish node lists, JSON values and Nothing
    };

namespace detail {

    // The declared kind of a compiled expression, determined at compile time
    enum class rfc9535_kind
    {
        nodes,   // a node list produced by a query
        value,   // a JSON value (Nothing at evaluation time if absent)
        logical  // a boolean test result
    };

    // An evaluation time result: a node list, a JSON value, or Nothing.
    // Nothing is distinct from JSON null, from an empty array, and from
    // an empty node list.
    template <typename Json>
    struct rfc9535_value
    {
        enum class kind_type { nodes, value, nothing };

        kind_type kind{kind_type::nothing};
        std::vector<const Json*> nodes{};
        const Json* ptr{nullptr};
        Json owned{};

        static rfc9535_value make_nodes(std::vector<const Json*>&& nodes)
        {
            rfc9535_value val;
            val.kind = kind_type::nodes;
            val.nodes = std::move(nodes);
            return val;
        }

        static rfc9535_value make_value_ref(const Json* ptr)
        {
            rfc9535_value val;
            val.kind = kind_type::value;
            val.ptr = ptr;
            return val;
        }

        static rfc9535_value make_value(Json&& val)
        {
            rfc9535_value result;
            result.kind = kind_type::value;
            result.owned = std::move(val);
            return result;
        }

        static rfc9535_value make_nothing()
        {
            return rfc9535_value{};
        }

        const Json& as_json() const
        {
            return ptr != nullptr ? *ptr : owned;
        }
    };

    // A node list used as a test is true if and only if it is non-empty,
    // regardless of the values it contains.
    template <typename Json>
    bool rfc9535_truthy(const rfc9535_value<Json>& val)
    {
        switch (val.kind)
        {
            case rfc9535_value<Json>::kind_type::nodes:
                return !val.nodes.empty();
            case rfc9535_value<Json>::kind_type::value:
                return val.as_json().is_bool() && val.as_json().as_bool();
            default:
                return false;
        }
    }

    template <typename Json>
    bool rfc9535_less(const Json& lhs, const Json& rhs)
    {
        if (lhs.is_number() && rhs.is_number())
        {
            return lhs < rhs;
        }
        if (lhs.is_string() && rhs.is_string())
        {
            return lhs < rhs;
        }
        return false;
    }

    template <typename Json>
    class rfc9535_expr_base
    {
        rfc9535_kind kind_;
    public:
        explicit rfc9535_expr_base(rfc9535_kind kind)
            : kind_(kind)
        {
        }
        virtual ~rfc9535_expr_base() = default;

        rfc9535_kind kind() const
        {
            return kind_;
        }

        virtual rfc9535_value<Json> evaluate(const Json& root, const Json& current) const = 0;
    };

    template <typename Json>
    struct rfc9535_selector
    {
        using string_type = typename Json::string_type;

        enum class selector_type { name, index, wildcard, slice, filter };

        selector_type type{selector_type::wildcard};
        string_type name{};
        std::int64_t index{0};
        jsoncons::optional<std::int64_t> slice_start{};
        jsoncons::optional<std::int64_t> slice_stop{};
        std::int64_t slice_step{1};
        std::unique_ptr<rfc9535_expr_base<Json>> expr{};

        rfc9535_selector() = default;
        rfc9535_selector(rfc9535_selector&&) = default;
        rfc9535_selector& operator=(rfc9535_selector&&) = default;

        static rfc9535_selector make_name(string_type&& name)
        {
            rfc9535_selector selector;
            selector.type = selector_type::name;
            selector.name = std::move(name);
            return selector;
        }

        static rfc9535_selector make_index(std::int64_t index)
        {
            rfc9535_selector selector;
            selector.type = selector_type::index;
            selector.index = index;
            return selector;
        }

        static rfc9535_selector make_wildcard()
        {
            return rfc9535_selector{};
        }

        static rfc9535_selector make_slice(const jsoncons::optional<std::int64_t>& start,
            const jsoncons::optional<std::int64_t>& stop, std::int64_t step)
        {
            rfc9535_selector selector;
            selector.type = selector_type::slice;
            selector.slice_start = start;
            selector.slice_stop = stop;
            selector.slice_step = step;
            return selector;
        }

        static rfc9535_selector make_filter(std::unique_ptr<rfc9535_expr_base<Json>>&& expr)
        {
            rfc9535_selector selector;
            selector.type = selector_type::filter;
            selector.expr = std::move(expr);
            return selector;
        }
    };

    template <typename Json>
    struct rfc9535_segment
    {
        bool recursive{false};
        std::vector<rfc9535_selector<Json>> selectors{};
    };

    template <typename Json>
    struct rfc9535_query
    {
        bool absolute{false};
        bool singular{true};
        std::vector<rfc9535_segment<Json>> segments{};
    };

    template <typename Json>
    void rfc9535_collect_descendants(const Json& node, std::vector<const Json*>& out)
    {
        out.push_back(std::addressof(node));
        if (node.is_array())
        {
            for (const auto& item : node.array_range())
            {
                rfc9535_collect_descendants(item, out);
            }
        }
        else if (node.is_object())
        {
            for (const auto& member : node.object_range())
            {
                rfc9535_collect_descendants(member.value(), out);
            }
        }
    }

    template <typename Json>
    void rfc9535_apply_slice(const rfc9535_selector<Json>& selector,
        const Json& input, std::vector<const Json*>& out)
    {
        if (!input.is_array() || selector.slice_step == 0)
        {
            return;
        }
        const std::int64_t n = static_cast<std::int64_t>(input.size());
        const std::int64_t step = selector.slice_step;
        auto normalize = [n](std::int64_t i)
        {
            return i >= 0 ? i : n + i;
        };
        auto clamp = [](std::int64_t v, std::int64_t lo, std::int64_t hi)
        {
            return v < lo ? lo : (v > hi ? hi : v);
        };
        if (step > 0)
        {
            std::int64_t start = selector.slice_start ? normalize(*selector.slice_start) : 0;
            std::int64_t stop = selector.slice_stop ? normalize(*selector.slice_stop) : n;
            start = clamp(start, 0, n);
            stop = clamp(stop, 0, n);
            for (std::int64_t i = start; i < stop; i += step)
            {
                out.push_back(std::addressof(input[static_cast<std::size_t>(i)]));
            }
        }
        else
        {
            std::int64_t start = selector.slice_start ? normalize(*selector.slice_start) : n - 1;
            std::int64_t stop = selector.slice_stop ? normalize(*selector.slice_stop) : -n - 1;
            start = clamp(start, -1, n - 1);
            stop = clamp(stop, -n - 1, n - 1);
            for (std::int64_t i = start; i > stop; i += step)
            {
                out.push_back(std::addressof(input[static_cast<std::size_t>(i)]));
            }
        }
    }

    template <typename Json>
    void rfc9535_apply_selector(const rfc9535_selector<Json>& selector,
        const Json& root, const Json& input, std::vector<const Json*>& out)
    {
        using selector_type = typename rfc9535_selector<Json>::selector_type;
        switch (selector.type)
        {
            case selector_type::name:
                if (input.is_object() && input.contains(selector.name))
                {
                    out.push_back(std::addressof(input.at(selector.name)));
                }
                break;
            case selector_type::index:
                if (input.is_array())
                {
                    const std::int64_t n = static_cast<std::int64_t>(input.size());
                    const std::int64_t i = selector.index < 0 ? n + selector.index : selector.index;
                    if (i >= 0 && i < n)
                    {
                        out.push_back(std::addressof(input[static_cast<std::size_t>(i)]));
                    }
                }
                break;
            case selector_type::wildcard:
                if (input.is_array())
                {
                    for (const auto& item : input.array_range())
                    {
                        out.push_back(std::addressof(item));
                    }
                }
                else if (input.is_object())
                {
                    for (const auto& member : input.object_range())
                    {
                        out.push_back(std::addressof(member.value()));
                    }
                }
                break;
            case selector_type::slice:
                rfc9535_apply_slice(selector, input, out);
                break;
            case selector_type::filter:
                if (input.is_array())
                {
                    for (const auto& item : input.array_range())
                    {
                        if (rfc9535_truthy(selector.expr->evaluate(root, item)))
                        {
                            out.push_back(std::addressof(item));
                        }
                    }
                }
                else if (input.is_object())
                {
                    for (const auto& member : input.object_range())
                    {
                        if (rfc9535_truthy(selector.expr->evaluate(root, member.value())))
                        {
                            out.push_back(std::addressof(member.value()));
                        }
                    }
                }
                break;
            default:
                break;
        }
    }

    // Evaluation is stateless: all nodes and temporary values are produced
    // fresh on each call, so a compiled expression can be reused across documents.
    template <typename Json>
    std::vector<const Json*> rfc9535_evaluate_query(const rfc9535_query<Json>& query,
        const Json& root, const Json& current)
    {
        std::vector<const Json*> nodes;
        nodes.push_back(std::addressof(query.absolute ? root : current));
        std::vector<const Json*> next;
        for (const auto& segment : query.segments)
        {
            next.clear();
            for (const Json* node : nodes)
            {
                if (segment.recursive)
                {
                    std::vector<const Json*> descendants;
                    rfc9535_collect_descendants(*node, descendants);
                    for (const Json* descendant : descendants)
                    {
                        for (const auto& selector : segment.selectors)
                        {
                            rfc9535_apply_selector(selector, root, *descendant, next);
                        }
                    }
                }
                else
                {
                    for (const auto& selector : segment.selectors)
                    {
                        rfc9535_apply_selector(selector, root, *node, next);
                    }
                }
            }
            nodes.swap(next);
        }
        return nodes;
    }

    template <typename Json>
    class rfc9535_query_expr final : public rfc9535_expr_base<Json>
    {
        rfc9535_query<Json> query_;
    public:
        explicit rfc9535_query_expr(rfc9535_query<Json>&& query)
            : rfc9535_expr_base<Json>(rfc9535_kind::nodes), query_(std::move(query))
        {
        }

        bool is_singular() const
        {
            return query_.singular;
        }

        rfc9535_query<Json>& query()
        {
            return query_;
        }

        rfc9535_value<Json> evaluate(const Json& root, const Json& current) const override
        {
            return rfc9535_value<Json>::make_nodes(rfc9535_evaluate_query(query_, root, current));
        }
    };

    // Implicit conversion of a singular query to a value
    template <typename Json>
    class rfc9535_singular_expr final : public rfc9535_expr_base<Json>
    {
        rfc9535_query<Json> query_;
    public:
        explicit rfc9535_singular_expr(rfc9535_query<Json>&& query)
            : rfc9535_expr_base<Json>(rfc9535_kind::value), query_(std::move(query))
        {
        }

        rfc9535_value<Json> evaluate(const Json& root, const Json& current) const override
        {
            auto nodes = rfc9535_evaluate_query(query_, root, current);
            if (nodes.size() == 1)
            {
                return rfc9535_value<Json>::make_value_ref(nodes[0]);
            }
            return rfc9535_value<Json>::make_nothing();
        }
    };

    template <typename Json>
    class rfc9535_literal_expr final : public rfc9535_expr_base<Json>
    {
        Json value_;
    public:
        explicit rfc9535_literal_expr(Json&& value)
            : rfc9535_expr_base<Json>(rfc9535_kind::value), value_(std::move(value))
        {
        }

        rfc9535_value<Json> evaluate(const Json&, const Json&) const override
        {
            return rfc9535_value<Json>::make_value_ref(std::addressof(value_));
        }
    };

    enum class rfc9535_function_id { length, count, value };

    template <typename Json>
    class rfc9535_function_expr final : public rfc9535_expr_base<Json>
    {
        rfc9535_function_id function_id_;
        std::unique_ptr<rfc9535_expr_base<Json>> arg_;
    public:
        rfc9535_function_expr(rfc9535_function_id function_id,
            std::unique_ptr<rfc9535_expr_base<Json>>&& arg)
            : rfc9535_expr_base<Json>(rfc9535_kind::value),
              function_id_(function_id), arg_(std::move(arg))
        {
        }

        rfc9535_value<Json> evaluate(const Json& root, const Json& current) const override
        {
            auto arg = arg_->evaluate(root, current);
            switch (function_id_)
            {
                case rfc9535_function_id::count:
                    // The argument is a node list; duplicate nodes are kept and
                    // array values held by the nodes are not expanded.
                    return rfc9535_value<Json>::make_value(
                        Json(static_cast<std::uint64_t>(arg.nodes.size()), semantic_tag::none));
                case rfc9535_function_id::value:
                    if (arg.kind == rfc9535_value<Json>::kind_type::nodes && arg.nodes.size() == 1)
                    {
                        return rfc9535_value<Json>::make_value_ref(arg.nodes[0]);
                    }
                    return rfc9535_value<Json>::make_nothing();
                default: // rfc9535_function_id::length
                {
                    if (arg.kind != rfc9535_value<Json>::kind_type::value)
                    {
                        return rfc9535_value<Json>::make_nothing();
                    }
                    const Json& val = arg.as_json();
                    if (val.is_string())
                    {
                        auto sv = val.as_string_view();
                        return rfc9535_value<Json>::make_value(Json(
                            static_cast<std::uint64_t>(unicode_traits::count_codepoints(sv.data(), sv.size())),
                            semantic_tag::none));
                    }
                    if (val.is_array() || val.is_object())
                    {
                        return rfc9535_value<Json>::make_value(
                            Json(static_cast<std::uint64_t>(val.size()), semantic_tag::none));
                    }
                    return rfc9535_value<Json>::make_nothing();
                }
            }
        }
    };

    enum class rfc9535_comparison_op { eq, ne, lt, lte, gt, gte };

    template <typename Json>
    class rfc9535_comparison_expr final : public rfc9535_expr_base<Json>
    {
        rfc9535_comparison_op op_;
        std::unique_ptr<rfc9535_expr_base<Json>> lhs_;
        std::unique_ptr<rfc9535_expr_base<Json>> rhs_;
    public:
        rfc9535_comparison_expr(rfc9535_comparison_op op,
            std::unique_ptr<rfc9535_expr_base<Json>>&& lhs,
            std::unique_ptr<rfc9535_expr_base<Json>>&& rhs)
            : rfc9535_expr_base<Json>(rfc9535_kind::logical),
              op_(op), lhs_(std::move(lhs)), rhs_(std::move(rhs))
        {
        }

        rfc9535_value<Json> evaluate(const Json& root, const Json& current) const override
        {
            auto lhs = lhs_->evaluate(root, current);
            auto rhs = rhs_->evaluate(root, current);
            const bool lhs_nothing = lhs.kind == rfc9535_value<Json>::kind_type::nothing;
            const bool rhs_nothing = rhs.kind == rfc9535_value<Json>::kind_type::nothing;
            // Nothing equals Nothing, and is unequal to every actual JSON value
            const bool equal = (lhs_nothing || rhs_nothing) ? (lhs_nothing && rhs_nothing)
                : (lhs.as_json() == rhs.as_json());
            bool result;
            switch (op_)
            {
                case rfc9535_comparison_op::eq:
                    result = equal;
                    break;
                case rfc9535_comparison_op::ne:
                    result = !equal;
                    break;
                case rfc9535_comparison_op::lt:
                    result = !lhs_nothing && !rhs_nothing && rfc9535_less(lhs.as_json(), rhs.as_json());
                    break;
                case rfc9535_comparison_op::gt:
                    result = !lhs_nothing && !rhs_nothing && rfc9535_less(rhs.as_json(), lhs.as_json());
                    break;
                case rfc9535_comparison_op::lte:
                    result = equal || (!lhs_nothing && !rhs_nothing && rfc9535_less(lhs.as_json(), rhs.as_json()));
                    break;
                default: // rfc9535_comparison_op::gte
                    result = equal || (!lhs_nothing && !rhs_nothing && rfc9535_less(rhs.as_json(), lhs.as_json()));
                    break;
            }
            return rfc9535_value<Json>::make_value(Json(result, semantic_tag::none));
        }
    };

    enum class rfc9535_logical_op { op_and, op_or };

    template <typename Json>
    class rfc9535_logical_expr final : public rfc9535_expr_base<Json>
    {
        rfc9535_logical_op op_;
        std::unique_ptr<rfc9535_expr_base<Json>> lhs_;
        std::unique_ptr<rfc9535_expr_base<Json>> rhs_;
    public:
        rfc9535_logical_expr(rfc9535_logical_op op,
            std::unique_ptr<rfc9535_expr_base<Json>>&& lhs,
            std::unique_ptr<rfc9535_expr_base<Json>>&& rhs)
            : rfc9535_expr_base<Json>(rfc9535_kind::logical),
              op_(op), lhs_(std::move(lhs)), rhs_(std::move(rhs))
        {
        }

        rfc9535_value<Json> evaluate(const Json& root, const Json& current) const override
        {
            const bool lhs = rfc9535_truthy(lhs_->evaluate(root, current));
            const bool result = op_ == rfc9535_logical_op::op_and
                ? (lhs && rfc9535_truthy(rhs_->evaluate(root, current)))
                : (lhs || rfc9535_truthy(rhs_->evaluate(root, current)));
            return rfc9535_value<Json>::make_value(Json(result, semantic_tag::none));
        }
    };

    template <typename Json>
    class rfc9535_not_expr final : public rfc9535_expr_base<Json>
    {
        std::unique_ptr<rfc9535_expr_base<Json>> operand_;
    public:
        explicit rfc9535_not_expr(std::unique_ptr<rfc9535_expr_base<Json>>&& operand)
            : rfc9535_expr_base<Json>(rfc9535_kind::logical), operand_(std::move(operand))
        {
        }

        rfc9535_value<Json> evaluate(const Json& root, const Json& current) const override
        {
            return rfc9535_value<Json>::make_value(
                Json(!rfc9535_truthy(operand_->evaluate(root, current)), semantic_tag::none));
        }
    };

    template <typename Json>
    class rfc9535_expression_if
    {
    public:
        virtual ~rfc9535_expression_if() = default;
        virtual Json evaluate(const Json& root) const = 0;
    };

    template <typename Json>
    class rfc9535_expression final : public rfc9535_expression_if<Json>
    {
        std::unique_ptr<rfc9535_expr_base<Json>> expr_;
    public:
        explicit rfc9535_expression(std::unique_ptr<rfc9535_expr_base<Json>>&& expr)
            : expr_(std::move(expr))
        {
        }

        Json evaluate(const Json& root) const override
        {
            Json result(json_array_arg, semantic_tag::none);
            if (expr_)
            {
                auto val = expr_->evaluate(root, root);
                switch (val.kind)
                {
                    case rfc9535_value<Json>::kind_type::nodes:
                        for (const Json* ptr : val.nodes)
                        {
                            result.push_back(*ptr);
                        }
                        break;
                    case rfc9535_value<Json>::kind_type::value:
                        result.push_back(val.as_json());
                        break;
                    default:
                        break;
                }
            }
            return result;
        }
    };

    template <typename Json>
    class rfc9535_parser
    {
        using char_type = typename Json::char_type;
        using string_type = typename Json::string_type;
        using string_view_type = typename Json::string_view_type;
        using expr_ptr = std::unique_ptr<rfc9535_expr_base<Json>>;
        using selector_type = typename rfc9535_selector<Json>::selector_type;

        const char_type* p_;
        const char_type* end_;
        std::size_t column_;
    public:
        explicit rfc9535_parser(const string_view_type& path)
            : p_(path.data()), end_(path.data() + path.size()), column_(1)
        {
        }

        expr_ptr parse()
        {
            skip_ws();
            if (p_ == end_)
            {
                fail(jsonpath_errc::unexpected_eof);
            }
            expr_ptr expr;
            if (*p_ == '$' || *p_ == '@')
            {
                expr = parse_query_expr();
            }
            else
            {
                string_type word = parse_identifier();
                if (word.empty() || p_ == end_ || *p_ != '(')
                {
                    fail(jsonpath_errc::expected_root_or_function);
                }
                expr = parse_function_call(word);
            }
            skip_ws();
            if (p_ != end_)
            {
                fail(jsonpath_errc::syntax_error);
            }
            return expr;
        }
    private:
        void advance()
        {
            ++p_;
            ++column_;
        }

        void skip_ws()
        {
            while (p_ != end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n'))
            {
                advance();
            }
        }

        void fail(jsonpath_errc ec)
        {
            JSONCONS_THROW(jsonpath_error(make_error_code(ec), std::size_t(1), column_));
        }

        static bool is_identifier_char(char_type c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9') || c == '_'
                || static_cast<typename std::make_unsigned<char_type>::type>(c) > 127;
        }

        string_type parse_identifier()
        {
            string_type word;
            while (p_ != end_ && is_identifier_char(*p_))
            {
                word.push_back(*p_);
                advance();
            }
            return word;
        }

        expr_ptr parse_query_expr()
        {
            rfc9535_query<Json> query;
            query.absolute = (*p_ == '$');
            advance();
            bool singular = true;
            while (p_ != end_)
            {
                if (*p_ == '.')
                {
                    advance();
                    if (p_ != end_ && *p_ == '.')
                    {
                        advance();
                        singular = false;
                        rfc9535_segment<Json> segment;
                        segment.recursive = true;
                        if (p_ != end_ && *p_ == '*')
                        {
                            advance();
                            segment.selectors.push_back(rfc9535_selector<Json>::make_wildcard());
                        }
                        else if (p_ != end_ && *p_ == '[')
                        {
                            advance();
                            parse_bracketed(segment.selectors);
                        }
                        else
                        {
                            string_type name = parse_identifier();
                            if (name.empty())
                            {
                                fail(jsonpath_errc::expected_relative_path);
                            }
                            segment.selectors.push_back(rfc9535_selector<Json>::make_name(std::move(name)));
                        }
                        query.segments.push_back(std::move(segment));
                    }
                    else if (p_ != end_ && *p_ == '*')
                    {
                        advance();
                        singular = false;
                        rfc9535_segment<Json> segment;
                        segment.selectors.push_back(rfc9535_selector<Json>::make_wildcard());
                        query.segments.push_back(std::move(segment));
                    }
                    else
                    {
                        string_type name = parse_identifier();
                        if (name.empty())
                        {
                            fail(jsonpath_errc::expected_relative_path);
                        }
                        rfc9535_segment<Json> segment;
                        segment.selectors.push_back(rfc9535_selector<Json>::make_name(std::move(name)));
                        query.segments.push_back(std::move(segment));
                    }
                }
                else if (*p_ == '[')
                {
                    advance();
                    rfc9535_segment<Json> segment;
                    parse_bracketed(segment.selectors);
                    if (segment.selectors.size() != 1 ||
                        (segment.selectors[0].type != selector_type::name &&
                         segment.selectors[0].type != selector_type::index))
                    {
                        singular = false;
                    }
                    query.segments.push_back(std::move(segment));
                }
                else
                {
                    break;
                }
            }
            query.singular = singular;
            return jsoncons::make_unique<rfc9535_query_expr<Json>>(std::move(query));
        }

        void parse_bracketed(std::vector<rfc9535_selector<Json>>& selectors)
        {
            for (;;)
            {
                skip_ws();
                if (p_ == end_)
                {
                    fail(jsonpath_errc::unexpected_eof);
                }
                const char_type c = *p_;
                if (c == '\'' || c == '\"')
                {
                    selectors.push_back(rfc9535_selector<Json>::make_name(parse_quoted_string()));
                }
                else if (c == '*')
                {
                    advance();
                    selectors.push_back(rfc9535_selector<Json>::make_wildcard());
                }
                else if (c == '?')
                {
                    advance();
                    expr_ptr expr = parse_filter_or();
                    if (expr->kind() == rfc9535_kind::value)
                    {
                        fail(jsonpath_errc::invalid_expression_type);
                    }
                    selectors.push_back(rfc9535_selector<Json>::make_filter(std::move(expr)));
                }
                else if (c == ':' || c == '-' || (c >= '0' && c <= '9'))
                {
                    parse_index_or_slice(selectors);
                }
                else
                {
                    fail(jsonpath_errc::expected_bracket_specifier_or_union);
                }
                skip_ws();
                if (p_ == end_)
                {
                    fail(jsonpath_errc::unexpected_eof);
                }
                if (*p_ == ',')
                {
                    advance();
                    continue;
                }
                if (*p_ == ']')
                {
                    advance();
                    break;
                }
                fail(jsonpath_errc::expected_comma_or_rbracket);
            }
        }

        void parse_index_or_slice(std::vector<rfc9535_selector<Json>>& selectors)
        {
            auto start = parse_optional_integer();
            skip_ws();
            if (p_ != end_ && *p_ == ':')
            {
                advance();
                auto stop = parse_optional_integer();
                skip_ws();
                jsoncons::optional<std::int64_t> step;
                if (p_ != end_ && *p_ == ':')
                {
                    advance();
                    step = parse_optional_integer();
                    if (step && *step == 0)
                    {
                        fail(jsonpath_errc::step_cannot_be_zero);
                    }
                }
                selectors.push_back(rfc9535_selector<Json>::make_slice(start, stop,
                    step ? *step : 1));
            }
            else
            {
                if (!start)
                {
                    fail(jsonpath_errc::invalid_number);
                }
                selectors.push_back(rfc9535_selector<Json>::make_index(*start));
            }
        }

        jsoncons::optional<std::int64_t> parse_optional_integer()
        {
            skip_ws();
            bool negative = false;
            if (p_ != end_ && *p_ == '-')
            {
                negative = true;
                advance();
            }
            if (p_ == end_ || *p_ < '0' || *p_ > '9')
            {
                if (negative)
                {
                    fail(jsonpath_errc::invalid_number);
                }
                return jsoncons::optional<std::int64_t>();
            }
            std::int64_t n = 0;
            while (p_ != end_ && *p_ >= '0' && *p_ <= '9')
            {
                n = n * 10 + (*p_ - '0');
                advance();
            }
            return jsoncons::optional<std::int64_t>(negative ? -n : n);
        }

        expr_ptr parse_filter_or()
        {
            expr_ptr lhs = parse_filter_and();
            for (;;)
            {
                skip_ws();
                if (p_ != end_ && *p_ == '|' && p_ + 1 != end_ && *(p_ + 1) == '|')
                {
                    advance();
                    advance();
                    expr_ptr rhs = parse_filter_and();
                    lhs = jsoncons::make_unique<rfc9535_logical_expr<Json>>(
                        rfc9535_logical_op::op_or, std::move(lhs), std::move(rhs));
                }
                else
                {
                    break;
                }
            }
            return lhs;
        }

        expr_ptr parse_filter_and()
        {
            expr_ptr lhs = parse_filter_unary();
            for (;;)
            {
                skip_ws();
                if (p_ != end_ && *p_ == '&' && p_ + 1 != end_ && *(p_ + 1) == '&')
                {
                    advance();
                    advance();
                    expr_ptr rhs = parse_filter_unary();
                    lhs = jsoncons::make_unique<rfc9535_logical_expr<Json>>(
                        rfc9535_logical_op::op_and, std::move(lhs), std::move(rhs));
                }
                else
                {
                    break;
                }
            }
            return lhs;
        }

        expr_ptr parse_filter_unary()
        {
            skip_ws();
            if (p_ == end_)
            {
                fail(jsonpath_errc::unexpected_eof);
            }
            if (*p_ == '!')
            {
                advance();
                expr_ptr operand = parse_filter_unary();
                return jsoncons::make_unique<rfc9535_not_expr<Json>>(std::move(operand));
            }
            if (*p_ == '(')
            {
                advance();
                expr_ptr expr = parse_filter_or();
                skip_ws();
                if (p_ == end_ || *p_ != ')')
                {
                    fail(jsonpath_errc::expected_rparen);
                }
                advance();
                return expr;
            }
            return parse_filter_test();
        }

        expr_ptr parse_filter_test()
        {
            expr_ptr lhs = parse_operand();
            skip_ws();
            if (p_ != end_)
            {
                rfc9535_comparison_op op = rfc9535_comparison_op::eq;
                bool is_comparison = true;
                if (*p_ == '=' && p_ + 1 != end_ && *(p_ + 1) == '=')
                {
                    op = rfc9535_comparison_op::eq;
                    advance();
                    advance();
                }
                else if (*p_ == '!' && p_ + 1 != end_ && *(p_ + 1) == '=')
                {
                    op = rfc9535_comparison_op::ne;
                    advance();
                    advance();
                }
                else if (*p_ == '<' && p_ + 1 != end_ && *(p_ + 1) == '=')
                {
                    op = rfc9535_comparison_op::lte;
                    advance();
                    advance();
                }
                else if (*p_ == '>' && p_ + 1 != end_ && *(p_ + 1) == '=')
                {
                    op = rfc9535_comparison_op::gte;
                    advance();
                    advance();
                }
                else if (*p_ == '<')
                {
                    op = rfc9535_comparison_op::lt;
                    advance();
                }
                else if (*p_ == '>')
                {
                    op = rfc9535_comparison_op::gt;
                    advance();
                }
                else
                {
                    is_comparison = false;
                }
                if (is_comparison)
                {
                    lhs = try_make_value(std::move(lhs));
                    if (!lhs)
                    {
                        fail(jsonpath_errc::invalid_expression_type);
                    }
                    expr_ptr rhs = try_make_value(parse_operand());
                    if (!rhs)
                    {
                        fail(jsonpath_errc::invalid_expression_type);
                    }
                    return jsoncons::make_unique<rfc9535_comparison_expr<Json>>(op,
                        std::move(lhs), std::move(rhs));
                }
            }
            // A bare operand in test position must be a node list (existence)
            // or a logical expression; a value-typed result is not allowed.
            if (lhs->kind() == rfc9535_kind::value)
            {
                fail(jsonpath_errc::invalid_expression_type);
            }
            return lhs;
        }

        expr_ptr parse_operand()
        {
            skip_ws();
            if (p_ == end_)
            {
                fail(jsonpath_errc::unexpected_eof);
            }
            const char_type c = *p_;
            if (c == '$' || c == '@')
            {
                return parse_query_expr();
            }
            if (c == '\'' || c == '\"')
            {
                return jsoncons::make_unique<rfc9535_literal_expr<Json>>(
                    Json(parse_quoted_string(), semantic_tag::none));
            }
            if (c == '-' || (c >= '0' && c <= '9'))
            {
                return jsoncons::make_unique<rfc9535_literal_expr<Json>>(parse_number());
            }
            if (is_identifier_char(c))
            {
                string_type word = parse_identifier();
                if (word == JSONCONS_CSTRING_CONSTANT(char_type, "true"))
                {
                    return jsoncons::make_unique<rfc9535_literal_expr<Json>>(Json(true, semantic_tag::none));
                }
                if (word == JSONCONS_CSTRING_CONSTANT(char_type, "false"))
                {
                    return jsoncons::make_unique<rfc9535_literal_expr<Json>>(Json(false, semantic_tag::none));
                }
                if (word == JSONCONS_CSTRING_CONSTANT(char_type, "null"))
                {
                    return jsoncons::make_unique<rfc9535_literal_expr<Json>>(Json(null_type(), semantic_tag::none));
                }
                if (p_ != end_ && *p_ == '(')
                {
                    return parse_function_call(word);
                }
                fail(jsonpath_errc::syntax_error);
            }
            fail(jsonpath_errc::syntax_error);
            return nullptr;
        }

        expr_ptr parse_function_call(const string_type& name)
        {
            // assumes *p_ == '('
            advance();
            skip_ws();
            if (p_ != end_ && *p_ == ')')
            {
                fail(jsonpath_errc::invalid_arity);
            }
            expr_ptr arg = parse_operand();
            skip_ws();
            if (p_ == end_)
            {
                fail(jsonpath_errc::unexpected_eof);
            }
            if (*p_ == ',')
            {
                fail(jsonpath_errc::invalid_arity);
            }
            if (*p_ != ')')
            {
                fail(jsonpath_errc::expected_rparen);
            }
            advance();

            if (name == JSONCONS_CSTRING_CONSTANT(char_type, "count"))
            {
                if (arg->kind() != rfc9535_kind::nodes)
                {
                    fail(jsonpath_errc::invalid_function_argument_type);
                }
                return jsoncons::make_unique<rfc9535_function_expr<Json>>(
                    rfc9535_function_id::count, std::move(arg));
            }
            if (name == JSONCONS_CSTRING_CONSTANT(char_type, "value"))
            {
                if (arg->kind() != rfc9535_kind::nodes)
                {
                    fail(jsonpath_errc::invalid_function_argument_type);
                }
                return jsoncons::make_unique<rfc9535_function_expr<Json>>(
                    rfc9535_function_id::value, std::move(arg));
            }
            if (name == JSONCONS_CSTRING_CONSTANT(char_type, "length"))
            {
                arg = try_make_value(std::move(arg));
                if (!arg)
                {
                    fail(jsonpath_errc::invalid_function_argument_type);
                }
                return jsoncons::make_unique<rfc9535_function_expr<Json>>(
                    rfc9535_function_id::length, std::move(arg));
            }
            fail(jsonpath_errc::unknown_function);
            return nullptr;
        }

        // Implicit conversion to a value: only a singular query, one built
        // from single member or single index selectors, may convert.
        expr_ptr try_make_value(expr_ptr&& expr)
        {
            if (expr->kind() == rfc9535_kind::value)
            {
                return std::move(expr);
            }
            if (expr->kind() == rfc9535_kind::nodes)
            {
                auto* query_expr = static_cast<rfc9535_query_expr<Json>*>(expr.get());
                if (query_expr->is_singular())
                {
                    return jsoncons::make_unique<rfc9535_singular_expr<Json>>(
                        std::move(query_expr->query()));
                }
            }
            return nullptr;
        }

        Json parse_number()
        {
            string_type buffer;
            bool has_digits = false;
            if (p_ != end_ && *p_ == '-')
            {
                buffer.push_back(*p_);
                advance();
            }
            while (p_ != end_)
            {
                const char_type c = *p_;
                if (c >= '0' && c <= '9')
                {
                    has_digits = true;
                    buffer.push_back(c);
                    advance();
                }
                else if (c == '.' || c == 'e' || c == 'E')
                {
                    buffer.push_back(c);
                    advance();
                }
                else if ((c == '+' || c == '-') && !buffer.empty()
                    && (buffer.back() == 'e' || buffer.back() == 'E'))
                {
                    buffer.push_back(c);
                    advance();
                }
                else
                {
                    break;
                }
            }
            if (!has_digits)
            {
                fail(jsonpath_errc::invalid_number);
            }
            json_decoder<Json> decoder;
            basic_json_parser<char_type> parser;
            std::error_code ec;
            parser.update(buffer.data(), buffer.size());
            parser.parse_some(decoder, ec);
            if (ec)
            {
                fail(jsonpath_errc::invalid_number);
            }
            parser.finish_parse(decoder, ec);
            if (ec)
            {
                fail(jsonpath_errc::invalid_number);
            }
            return decoder.get_result();
        }

        string_type parse_quoted_string()
        {
            string_type result;
            const char_type quote = *p_;
            advance();
            while (p_ != end_ && *p_ != quote)
            {
                if (*p_ == '\\')
                {
                    advance();
                    if (p_ == end_)
                    {
                        fail(jsonpath_errc::unexpected_eof);
                    }
                    switch (*p_)
                    {
                        case '\"': result.push_back('\"'); advance(); break;
                        case '\'': result.push_back('\''); advance(); break;
                        case '\\': result.push_back('\\'); advance(); break;
                        case '/': result.push_back('/'); advance(); break;
                        case 'b': result.push_back('\b'); advance(); break;
                        case 'f': result.push_back('\f'); advance(); break;
                        case 'n': result.push_back('\n'); advance(); break;
                        case 'r': result.push_back('\r'); advance(); break;
                        case 't': result.push_back('\t'); advance(); break;
                        case 'u':
                        {
                            advance();
                            uint32_t cp = parse_hex4();
                            if (unicode_traits::is_high_surrogate(cp))
                            {
                                if (p_ == end_ || *p_ != '\\')
                                {
                                    fail(jsonpath_errc::invalid_codepoint);
                                }
                                advance();
                                if (p_ == end_ || *p_ != 'u')
                                {
                                    fail(jsonpath_errc::invalid_codepoint);
                                }
                                advance();
                                uint32_t cp2 = parse_hex4();
                                uint32_t codepoint = 0x10000 + ((cp & 0x3FF) << 10) + (cp2 & 0x3FF);
                                unicode_traits::convert(&codepoint, 1, result);
                            }
                            else
                            {
                                unicode_traits::convert(&cp, 1, result);
                            }
                            break;
                        }
                        default:
                            fail(jsonpath_errc::illegal_escaped_character);
                    }
                }
                else
                {
                    result.push_back(*p_);
                    advance();
                }
            }
            if (p_ == end_)
            {
                fail(jsonpath_errc::unexpected_eof);
            }
            advance(); // closing quote
            return result;
        }

        uint32_t parse_hex4()
        {
            uint32_t cp = 0;
            for (int i = 0; i < 4; ++i)
            {
                if (p_ == end_)
                {
                    fail(jsonpath_errc::unexpected_eof);
                }
                cp = append_to_codepoint(cp, *p_);
                advance();
            }
            return cp;
        }

        uint32_t append_to_codepoint(uint32_t cp, char_type c)
        {
            cp *= 16;
            if (c >= '0' && c <= '9')
            {
                cp += static_cast<uint32_t>(c - '0');
            }
            else if (c >= 'a' && c <= 'f')
            {
                cp += static_cast<uint32_t>(c - 'a' + 10);
            }
            else if (c >= 'A' && c <= 'F')
            {
                cp += static_cast<uint32_t>(c - 'A' + 10);
            }
            else
            {
                fail(jsonpath_errc::invalid_codepoint);
            }
            return cp;
        }
    };

    template <typename Json>
    std::unique_ptr<rfc9535_expression_if<Json>> make_rfc9535_expression(
        const typename Json::string_view_type& path)
    {
        rfc9535_parser<Json> parser(path);
        return jsoncons::make_unique<rfc9535_expression<Json>>(parser.parse());
    }

    template <typename Json>
    std::unique_ptr<rfc9535_expression_if<Json>> make_rfc9535_expression(
        const typename Json::string_view_type& path, std::error_code& ec)
    {
        JSONCONS_TRY
        {
            rfc9535_parser<Json> parser(path);
            return jsoncons::make_unique<rfc9535_expression<Json>>(parser.parse());
        }
        JSONCONS_CATCH(const jsonpath_error& e)
        {
            ec = e.code();
            return jsoncons::make_unique<rfc9535_expression<Json>>(
                std::unique_ptr<rfc9535_expr_base<Json>>{});
        }
    }

} // namespace detail
} // namespace jsonpath
} // namespace jsoncons

#endif // JSONCONS_EXT_JSONPATH_JSONPATH_FUNCTION_MODE_HPP
