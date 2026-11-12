#ifndef SIMDJSON_GENERIC_ONDEMAND_POINTER_BATCH_INL_H

#ifndef SIMDJSON_CONDITIONAL_INCLUDE
#define SIMDJSON_GENERIC_ONDEMAND_POINTER_BATCH_INL_H
#include "simdjson/generic/ondemand/base.h"
#include "simdjson/generic/jsoncharutils.h"
#include "simdjson/generic/ondemand/document.h"
#include "simdjson/generic/ondemand/json_iterator.h"
#include "simdjson/generic/ondemand/json_type.h"
#include "simdjson/generic/ondemand/pointer_batch.h"
#include "simdjson/generic/ondemand/raw_json_string.h"
#include "simdjson/generic/ondemand/value_iterator.h"
#include "simdjson/generic/implementation_simdjson_result_base-inl.h"
#endif // SIMDJSON_CONDITIONAL_INCLUDE

#include <limits>

namespace simdjson {
namespace SIMDJSON_IMPLEMENTATION {
namespace ondemand {

namespace pointer_batch_detail {

/**
 * Decode one RFC 6901 reference token: ~0 -> '~', ~1 -> '/'.
 * Any other use of '~' is a syntax error that fails the whole compilation.
 */
simdjson_warn_unused simdjson_inline error_code
decode_token(std::string_view raw, std::string &out) noexcept {
  out.clear();
  out.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '~') {
      if (i + 1 >= raw.size()) { return INVALID_JSON_POINTER; }
      const char escaped = raw[i + 1];
      if (escaped == '0') {
        out.push_back('~');
        ++i;
      } else if (escaped == '1') {
        out.push_back('/');
        ++i;
      } else {
        return INVALID_JSON_POINTER;
      }
    } else {
      out.push_back(raw[i]);
    }
  }
  return SUCCESS;
}

/**
 * Per-execution state. Everything a run needs lives here rather than in the
 * compiled plan, so the same plan can be executed concurrently on documents
 * owned by different parsers.
 */
struct exec_state {
  std::vector<pointer_batch_result> &results;
  pointer_batch_statistics &stats;
  /** Flat per-node match flags; plan_node::seen_offset gives each node its slice. */
  std::vector<unsigned char> &seen;

  simdjson_inline void fill(std::size_t slot, error_code error) noexcept {
    // A slot is resolved exactly once: first-occurrence and nearer-to-root
    // decisions always win over later attempts.
    if (results[slot].error == UNINITIALIZED) {
      results[slot].error = error;
    }
  }

  simdjson_inline void fill_all(const std::vector<std::size_t> &slots,
                                error_code error) noexcept {
    for (std::size_t slot : slots) { fill(slot, error); }
  }

  simdjson_inline void record_raw(std::size_t slot,
                                  std::string_view raw) noexcept {
    if (results[slot].error == UNINITIALIZED) {
      results[slot].error = SUCCESS;
      results[slot].raw = raw;
    }
  }
};

/**
 * Raw bytes of a scalar value that has just been consumed, mirroring
 * document::raw_json(): the view ends at the closing quote (strings) or at
 * the first structural/whitespace delimiter, never on trailing whitespace.
 * The bytes come straight from the input buffer.
 */
simdjson_inline std::string_view
scalar_raw(const uint8_t *start, const uint8_t *next_token) noexcept {
  const uint8_t *final_point = start + 1;
  if (*start == '"') {
    while (final_point < next_token) {
      const uint8_t c = *final_point++;
      if (c == '\\') {
        ++final_point;
      } else if (c == '"') {
        break;
      }
    }
    if (final_point > next_token) { final_point = next_token; }
  } else {
    while (final_point < next_token &&
           !jsoncharutils::is_structural_or_whitespace(*final_point) &&
           *final_point != '"' && *final_point != 0x1E && *final_point != 0) {
      ++final_point;
    }
  }
  return std::string_view(reinterpret_cast<const char *>(start),
                          static_cast<std::size_t>(final_point - start));
}

/**
 * Raw bytes of a fully consumed container. Outside a document_stream the
 * iterator's previous structural token is the closing bracket (the same
 * assumption object::raw_json() makes), which is an O(1) answer. In streaming
 * mode the next document's tokens may already be indexed, so locate the
 * closing token by scanning the input bytes from the recorded start instead:
 * the depth walk that just finished guarantees the matching '}' or ']' and
 * strings cannot hide a structural byte from the accounting.
 */
simdjson_inline std::string_view
container_raw(const value_iterator &vi, const uint8_t *start) noexcept {
  if (!vi.json_iter().streaming()) {
    const uint8_t *end = vi.json_iter().peek(-1) + 1;
    return std::string_view(reinterpret_cast<const char *>(start),
                            static_cast<std::size_t>(end - start));
  }
  const uint8_t *cursor = start + 1;
  int depth = 1;
  while (depth > 0) {
    const uint8_t c = *cursor++;
    if (c == '"') {
      while (true) {
        const uint8_t inner = *cursor++;
        if (inner == '\\') {
          ++cursor;
        } else if (inner == '"') {
          break;
        }
      }
    } else if (c == '{' || c == '[') {
      ++depth;
    } else if (c == '}' || c == ']') {
      --depth;
    }
  }
  return std::string_view(reinterpret_cast<const char *>(start),
                          static_cast<std::size_t>(cursor - start));
}

/**
 * Walk one value described by plan node node_idx. On entry vi points at the
 * first token of the value (at_start). On return the value has been fully
 * consumed, leaving the iterator at its parent's separator, exactly as a
 * normal On-Demand iteration would. Only fatal document errors escape;
 * pointer-local failures are written into the matching result slots.
 */
simdjson_warn_unused simdjson_inline error_code
walk(value_iterator vi, std::size_t node_idx, const pointer_batch &plan,
     exec_state &state) noexcept {
  const pointer_batch::plan_node &node = plan.node(node_idx);
  const bool is_result = !node.result_slots.empty();

  json_type type;
  SIMDJSON_TRY(vi.type().get(type));
  const uint8_t * const start = vi.peek_start();

  if (type == json_type::object || type == json_type::array) {
    const bool in_object = type == json_type::object;

    // No requested pointer continues through this container: skip it with
    // the regular skip logic, capturing its raw bytes at the closing token.
    if (node.edges.empty()) {
      SIMDJSON_TRY(vi.json_iter().skip_value(vi.depth() - 1));
      if (is_result) {
        const std::string_view raw = container_raw(vi, start);
        for (std::size_t slot : node.result_slots) { state.record_raw(slot, raw); }
      }
      return SUCCESS;
    }

    unsigned char * const seen = state.seen.data() + node.seen_offset;

    if (in_object) {
      bool has_field;
      if (vi.depth() == 1) {
        SIMDJSON_TRY(vi.start_root_object().get(has_field));
      } else {
        SIMDJSON_TRY(vi.start_object().get(has_field));
      }
      state.stats.container_entries++;

      while (has_field) {
        state.stats.field_reads++;
        raw_json_string raw_key;
        SIMDJSON_TRY(vi.field_key().get(raw_key));
        SIMDJSON_TRY(vi.field_value());

        // First the exact same raw byte comparison find_field() uses: a
        // document key without escapes matches a decoded plan name directly,
        // so the common path never touches the decode buffer and an
        // unrequested member is validated no further than the existing skip
        // logic would validate it.
        std::size_t edge_idx = node.edges.size();
        for (std::size_t e = 0; e < node.edges.size(); ++e) {
          if (!seen[e] && vi.key_matches(raw_key, node.edges[e].token.name)) {
            edge_idx = e;
            break;
          }
        }

        // A raw miss may still be an escaped spelling ("a" vs "a"):
        // decode the document key once and compare decoded names. A decode
        // error (STRING_ERROR) means the key cannot equal any valid decoded
        // plan name, so it behaves as an unrequested member and is skipped.
        if (edge_idx == node.edges.size()) {
          auto * const saved_buf_loc = vi.json_iter().string_buf_loc();
          auto decoded = vi.json_iter().unescape(raw_key, false);
          if (!decoded.error()) {
            const std::string_view decoded_key = decoded.value_unsafe();
            for (std::size_t e = 0; e < node.edges.size(); ++e) {
              if (!seen[e] && node.edges[e].token.name == decoded_key) {
                edge_idx = e;
                break;
              }
            }
          }
          vi.json_iter().string_buf_loc() = saved_buf_loc;
        }

        if (edge_idx == node.edges.size()) {
          // Unrequested member: keep the existing value-skip behavior.
          SIMDJSON_TRY(vi.skip_field_value());
        } else {
          seen[edge_idx] = 1;
          SIMDJSON_TRY(walk(vi.child(), node.edges[edge_idx].child, plan, state));
        }

        bool has_next;
        SIMDJSON_TRY(vi.has_next_field().get(has_next));
        has_field = has_next;
      }

      // Members that never appeared cannot satisfy any pointer below them.
      for (std::size_t e = 0; e < node.edges.size(); ++e) {
        if (!seen[e]) {
          state.fill_all(plan.node(node.edges[e].child).subtree_slots,
                         NO_SUCH_FIELD);
        }
      }
    } else {
      // A non-index token can never match an array element.
      for (std::size_t e = 0; e < node.edges.size(); ++e) {
        if (node.edges[e].array_kind != pointer_batch::token_array_kind::array_index) {
          state.fill_all(plan.node(node.edges[e].child).subtree_slots,
                         INVALID_JSON_POINTER);
          seen[e] = 1;
        }
      }

      bool has_element;
      if (vi.depth() == 1) {
        SIMDJSON_TRY(vi.start_root_array().get(has_element));
      } else {
        SIMDJSON_TRY(vi.start_array().get(has_element));
      }
      state.stats.container_entries++;

      std::size_t element_index = 0;
      while (has_element) {
        state.stats.array_advances++;
        std::size_t edge_idx = node.edges.size();
        for (std::size_t e = 0; e < node.edges.size(); ++e) {
          if (!seen[e] &&
              node.edges[e].array_kind == pointer_batch::token_array_kind::array_index &&
              node.edges[e].array_index_value == element_index) {
            edge_idx = e;
            break;
          }
        }

        if (edge_idx == node.edges.size()) {
          // Unrequested element: consume it from the array's own iterator with
          // the same skip call as array_iterator::operator++ (the child is one
          // level deeper than vi, so skip_child actually moves).
          const token_position child_start = vi.child().position();
          SIMDJSON_TRY(vi.skip_child(child_start));
        } else {
          seen[edge_idx] = 1;
          SIMDJSON_TRY(walk(vi.child(), node.edges[edge_idx].child, plan, state));
        }

        bool has_next;
        SIMDJSON_TRY(vi.has_next_element().get(has_next));
        has_element = has_next;
        ++element_index;
      }

      // Indexes past the end are out of bounds, mirroring array::at().
      for (std::size_t e = 0; e < node.edges.size(); ++e) {
        if (!seen[e]) {
          state.fill_all(plan.node(node.edges[e].child).subtree_slots,
                         INDEX_OUT_OF_BOUNDS);
        }
      }
    }

    if (is_result) {
      // Record the ancestor's full raw bytes from its first byte: scanning to
      // its own closing token keeps this correct in a document_stream, where
      // the following document's tokens are already indexed. The descendants
      // were consumed while walking, so they are not traversed again.
      const std::string_view raw = container_raw(vi, start);
      for (std::size_t slot : node.result_slots) { state.record_raw(slot, raw); }
    }
    return SUCCESS;
  }

  // Scalar value. A scalar cannot hold any descendant: report the same
  // NO_SUCH_FIELD that the single-pointer API gives for a well-formed pointer
  // below a scalar.
  if (!node.edges.empty()) {
    for (const auto &edge : node.edges) {
      state.fill_all(plan.node(edge.child).subtree_slots, NO_SUCH_FIELD);
    }
  }
  SIMDJSON_TRY(vi.json_iter().skip_value(vi.depth() - 1));
  if (is_result) {
    const std::string_view raw = scalar_raw(start, vi.json_iter().unsafe_pointer());
    for (std::size_t slot : node.result_slots) { state.record_raw(slot, raw); }
  }
  return SUCCESS;
}

} // namespace pointer_batch_detail

simdjson_inline pointer_batch::token_array_kind
pointer_batch::classify_array_token(const std::string &name,
                                    std::size_t &index_value) noexcept {
  if (name.empty() || name[0] == '-') {
    return name.empty() ? token_array_kind::object_key
                        : token_array_kind::array_invalid;
  }
  std::size_t value = 0;
  for (std::size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    if (c < '0' || c > '9') {
      return token_array_kind::object_key;
    }
    if (i > 0 && name[0] == '0') {
      return token_array_kind::array_invalid;
    }
    const unsigned digit = static_cast<unsigned>(c - '0');
    if (value > ((std::numeric_limits<std::size_t>::max)() - digit) / 10) {
      return token_array_kind::array_invalid;
    }
    value = value * 10 + digit;
  }
  index_value = value;
  return token_array_kind::array_index;
}

simdjson_inline pointer_batch::pointer_batch(std::vector<plan_node> &&trie,
                                             std::size_t slots) noexcept
  : nodes{std::move(trie)}, slot_count{slots} {}

simdjson_pure simdjson_inline std::size_t pointer_batch::size() const noexcept {
  return slot_count;
}

simdjson_pure simdjson_inline bool pointer_batch::valid() const noexcept {
  // A compiled plan always owns at least the root trie node, even when the
  // input pointer list was empty; a default-constructed batch owns nothing.
  return !nodes.empty();
}

template <typename InputIt>
simdjson_warn_unused simdjson_inline simdjson_result<pointer_batch>
pointer_batch::compile_range(InputIt first, InputIt last) noexcept {
  std::vector<plan_node> trie(1);
  std::size_t slot_count = 0;

  for (InputIt it = first; it != last; ++it) {
    const std::string_view pointer = *it;
    std::size_t node_idx = 0;
    trie[node_idx].subtree_slots.push_back(slot_count);

    if (!pointer.empty()) {
      if (pointer[0] != '/') { return INVALID_JSON_POINTER; }

      std::size_t pos = 1;
      while (pos <= pointer.size()) {
        const std::size_t slash = pointer.find('/', pos);
        const std::string_view raw_token =
            pointer.substr(pos, slash == std::string_view::npos
                                  ? std::string_view::npos
                                  : slash - pos);

        std::string name;
        SIMDJSON_TRY(pointer_batch_detail::decode_token(raw_token, name));

        std::size_t array_index_value = 0;
        const token_array_kind array_kind =
            classify_array_token(name, array_index_value);

        std::size_t edge_idx = trie[node_idx].edges.size();
        for (std::size_t e = 0; e < trie[node_idx].edges.size(); ++e) {
          if (trie[node_idx].edges[e].token.name == name) {
            edge_idx = e;
            break;
          }
        }
        if (edge_idx == trie[node_idx].edges.size()) {
          const std::size_t child_idx = trie.size();
          trie.emplace_back();
          trie[node_idx].edges.push_back(
              plan_edge{plan_token{std::move(name)}, child_idx, array_kind,
                        array_index_value});
          node_idx = child_idx;
        } else {
          node_idx = trie[node_idx].edges[edge_idx].child;
        }
        trie[node_idx].subtree_slots.push_back(slot_count);

        if (slash == std::string_view::npos) { break; }
        pos = slash + 1;
      }
    }
    trie[node_idx].result_slots.push_back(slot_count);
    ++slot_count;
  }

  // Lay out the per-node match flags as one flat buffer allocated once per
  // execution instead of one heap allocation per visited container.
  std::size_t seen_total = 0;
  for (plan_node &planned : trie) {
    planned.seen_offset = seen_total;
    seen_total += planned.edges.size();
  }

  pointer_batch compiled(std::move(trie), slot_count);
  compiled.seen_size = seen_total;
  return compiled;
}

simdjson_warn_unused simdjson_inline simdjson_result<pointer_batch>
pointer_batch::compile(std::initializer_list<std::string_view> pointers) noexcept {
  return compile_range(pointers.begin(), pointers.end());
}

template <typename InputIt>
simdjson_warn_unused simdjson_inline simdjson_result<pointer_batch>
pointer_batch::compile(InputIt first, InputIt last) noexcept {
  return compile_range(first, last);
}

simdjson_warn_unused simdjson_inline error_code
pointer_batch::execute(document &doc,
                       std::vector<pointer_batch_result> &results,
                       pointer_batch_statistics &stats) const noexcept {
  results.assign(slot_count, pointer_batch_result{});
  stats = pointer_batch_statistics{};
  if (slot_count == 0) { return SUCCESS; }

  // One allocation covers the match flags of every plan node; each execution
  // owns this buffer, so concurrent runs never share mutable state.
  std::vector<unsigned char> seen(seen_size, 0);
  pointer_batch_detail::exec_state state{results, stats, seen};
  // get_root_value_iterator() does not consume or rewind anything: the
  // document must simply still be at its root, as the contract requires.
  value_iterator root = doc.get_root_value_iterator();
  const error_code fatal = pointer_batch_detail::walk(root, 0, *this, state);
  if (fatal) {
    // An unrecoverable document error invalidates the whole batch: the
    // partially filled slots are not a successful execution. Traversal
    // helpers such as has_next_field() only record the error in the
    // iterator (like find_field_raw(), whose callers abandon explicitly), so
    // mark the document unusable here as well.
    doc.iter.abandon();
    return fatal;
  }

  // Defensive safety net: every slot is reachable through the plan trie, so
  // this should never trigger.
  for (auto &slot : results) {
    if (slot.error == UNINITIALIZED) { slot.error = UNEXPECTED_ERROR; }
  }
  return SUCCESS;
}

simdjson_warn_unused simdjson_inline error_code
pointer_batch::execute(document &doc,
                       std::vector<pointer_batch_result> &results) const noexcept {
  pointer_batch_statistics stats;
  return execute(doc, results, stats);
}

simdjson_warn_unused simdjson_inline error_code
pointer_batch::execute(document_reference &doc,
                       std::vector<pointer_batch_result> &results,
                       pointer_batch_statistics &stats) const noexcept {
  return execute(static_cast<document &>(doc), results, stats);
}

simdjson_warn_unused simdjson_inline error_code
pointer_batch::execute(document_reference &doc,
                       std::vector<pointer_batch_result> &results) const noexcept {
  return execute(static_cast<document &>(doc), results);
}

simdjson_warn_unused simdjson_inline
simdjson_result<std::vector<pointer_batch_result>>
pointer_batch::execute(document_reference &doc) const noexcept {
  return execute(static_cast<document &>(doc));
}

simdjson_warn_unused simdjson_inline
simdjson_result<std::vector<pointer_batch_result>>
pointer_batch::execute(document &doc) const noexcept {
  std::vector<pointer_batch_result> results;
  pointer_batch_statistics stats;
  const error_code error = execute(doc, results, stats);
  if (error) { return error; }
  return std::move(results);
}

simdjson_inline const pointer_batch::plan_node &
pointer_batch::node(std::size_t index) const noexcept {
  return nodes[index];
}

} // namespace ondemand
} // namespace SIMDJSON_IMPLEMENTATION
} // namespace simdjson

namespace simdjson {

simdjson_inline simdjson_result<SIMDJSON_IMPLEMENTATION::ondemand::pointer_batch>::simdjson_result(
    SIMDJSON_IMPLEMENTATION::ondemand::pointer_batch &&value) noexcept
  : SIMDJSON_IMPLEMENTATION::implementation_simdjson_result_base<
        SIMDJSON_IMPLEMENTATION::ondemand::pointer_batch>(std::move(value)) {}

simdjson_inline simdjson_result<SIMDJSON_IMPLEMENTATION::ondemand::pointer_batch>::simdjson_result(
    error_code error) noexcept
  : SIMDJSON_IMPLEMENTATION::implementation_simdjson_result_base<
        SIMDJSON_IMPLEMENTATION::ondemand::pointer_batch>(error) {}

} // namespace simdjson

#endif // SIMDJSON_GENERIC_ONDEMAND_POINTER_BATCH_INL_H
