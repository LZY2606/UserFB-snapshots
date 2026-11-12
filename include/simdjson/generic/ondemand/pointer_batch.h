#ifndef SIMDJSON_GENERIC_ONDEMAND_POINTER_BATCH_H

#ifndef SIMDJSON_CONDITIONAL_INCLUDE
#define SIMDJSON_GENERIC_ONDEMAND_POINTER_BATCH_H
#include "simdjson/generic/ondemand/base.h"
#include "simdjson/generic/implementation_simdjson_result_base.h"
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>
#endif // SIMDJSON_CONDITIONAL_INCLUDE

namespace simdjson {
namespace SIMDJSON_IMPLEMENTATION {
namespace ondemand {

/**
 * Optional execution counters for pointer_batch::execute().
 *
 * Every counter only observes work that the single forward walk actually
 * performs: containers and members that no requested pointer reaches are
 * skipped with the regular On-Demand skip logic and are not counted as
 * container entries or member reads.
 */
struct pointer_batch_statistics {
  /** Number of objects and arrays the walk entered (including the root container). */
  std::size_t container_entries{0};
  /** Number of object members whose key and colon were read. */
  std::size_t field_reads{0};
  /** Number of array elements the walk advanced through. */
  std::size_t array_advances{0};
};

/**
 * One entry of a pointer_batch execution result, in the same order as the
 * pointers handed to pointer_batch::compile().
 *
 * On success, error is SUCCESS and raw points at the verbatim bytes of the
 * requested value inside the document buffer: internal whitespace and string
 * escapes are preserved, and trailing whitespace after the value is excluded.
 * The view borrows the input buffer (not the parser's decode buffer), so it
 * stays valid until that buffer is destroyed, independently of any later
 * lookup or parse on the same parser.
 *
 * On failure, error holds the per-pointer error code and raw is empty.
 */
struct pointer_batch_result {
  error_code error{UNINITIALIZED};
  std::string_view raw{};
};

/**
 * A reusable, immutable batch of RFC 6901 JSON pointers.
 *
 * A compiled batch owns copies of the decoded pointer tokens, so mutating or
 * destroying the strings passed to compile() does not affect it. It carries no
 * per-document state: the same batch can be executed concurrently on documents
 * owned by different parsers. Cursors, result slots and work counters all live
 * inside the execute() call.
 *
 * Unlike repeated document::at_pointer() calls, executing a batch never rewinds
 * the document: requested values are reached in one forward walk, so containers
 * on a prefix shared by several pointers are scanned at most once. No DOM tree
 * and no generic value tree are built.
 */
class pointer_batch {
public:
  /**
   * Create an invalid batch. Exists so a variable can be declared and assigned
   * later, matching the convention of other On-Demand types.
   */
  simdjson_inline pointer_batch() noexcept = default;
  pointer_batch(pointer_batch &&other) noexcept = default;
  pointer_batch &operator=(pointer_batch &&other) noexcept = default;
  pointer_batch(const pointer_batch &) = delete;
  pointer_batch &operator=(const pointer_batch &) = delete;

  /**
   * Compile a batch of RFC 6901 pointers.
   *
   * The empty pointer ("") denotes the root value. A non-empty pointer must
   * start with '/'. The ~0 and ~1 escapes are decoded once here; any other
   * escape (~ at the end of a token or ~ followed by a character other than
   * 0 or 1) makes the whole compilation fail with INVALID_JSON_POINTER.
   *
   * Duplicate pointers are kept: the result vector contains one slot per
   * input pointer, in input order, including repeated slots.
   *
   * @returns The compiled batch, or INVALID_JSON_POINTER if any pointer is
   *          syntactically invalid.
   */
  simdjson_warn_unused static simdjson_result<pointer_batch> compile(
      std::initializer_list<std::string_view> pointers) noexcept;

  /**
   * @overload compile(InputIt first, InputIt last)
   *
   * Compiles a batch from any iterator range whose elements are convertible
   * to std::string_view (e.g. a std::vector<std::string>).
   */
  template <typename InputIt>
  simdjson_warn_unused static simdjson_result<pointer_batch> compile(
      InputIt first, InputIt last) noexcept;

  /** Number of result slots produced by execute(), equal to the input count. */
  simdjson_pure simdjson_inline std::size_t size() const noexcept;

  /** Whether this batch holds a successfully compiled plan. */
  simdjson_pure simdjson_inline bool valid() const noexcept;

  /**
   * Execute the batch on an unconsumed document in a single forward walk.
   *
   * The returned vector has one pointer_batch_result per compiled pointer, in
   * compile order. A failure local to one pointer only fills that slot; the
   * other pointers are still resolved. The per-pointer codes are:
   *         - NO_SUCH_FIELD: an object has no member with that (decoded) name,
   *           or a well-formed pointer continues below a scalar value;
   *         - INDEX_OUT_OF_BOUNDS: an array index is past the last element;
   *         - INVALID_JSON_POINTER: the next token is not a usable array index
   *           (leading zero, negative sign, non-numeric word or size_t
   *           overflow) while the walk has reached an array.
   * The same words stay ordinary keys in an object. A document error that the
   * library considers unrecoverable (e.g. TAPE_ERROR,
   * INCOMPLETE_ARRAY_OR_OBJECT) fails the whole call and the returned error
   * code must be treated as the batch having failed.
   */
  simdjson_warn_unused simdjson_inline simdjson_result<std::vector<pointer_batch_result>>
  execute(document &doc) const noexcept;

  /**
   * @overload execute(document &doc, std::vector<pointer_batch_result> &results)
   *
   * Writes the slots into the caller-provided vector (resized as needed) and
   * returns the fatal error code, or SUCCESS.
   */
  simdjson_warn_unused simdjson_inline error_code
  execute(document &doc, std::vector<pointer_batch_result> &results) const noexcept;

  /**
   * @overload execute(document &doc, std::vector<pointer_batch_result> &results,
   *                  pointer_batch_statistics &stats)
   *
   * Additionally reports the work performed by the single forward walk.
   */
  simdjson_warn_unused simdjson_inline error_code
  execute(document &doc, std::vector<pointer_batch_result> &results,
          pointer_batch_statistics &stats) const noexcept;

  /**
   * @overload execute(document_reference &doc, ...)
   *
   * Convenience for document_stream iteration: forwards to the referenced
   * document, whose input must stay alive for the returned raw views.
   */
  simdjson_warn_unused simdjson_inline error_code
  execute(document_reference &doc, std::vector<pointer_batch_result> &results,
          pointer_batch_statistics &stats) const noexcept;
  simdjson_warn_unused simdjson_inline error_code
  execute(document_reference &doc,
          std::vector<pointer_batch_result> &results) const noexcept;
  simdjson_warn_unused simdjson_inline simdjson_result<std::vector<pointer_batch_result>>
  execute(document_reference &doc) const noexcept;

private:
  /** One decoded RFC 6901 reference token. */
  struct plan_token {
    /** Decoded token text (the key to use in an object context). */
    std::string name;
  };

  /** How a token behaves when the walk reaches an array. */
  enum class token_array_kind : unsigned char {
    /** Non-numeric token: a plain object key, INVALID_JSON_POINTER in an array. */
    object_key,
    /** Decimal index that fits in size_t (0 is a legal index). */
    array_index,
    /** Leading zero, negative sign, non-numeric array word or overflow: invalid in an array. */
    array_invalid
  };

  /** An edge of the plan trie: one reference token leading to a child node. */
  struct plan_edge {
    plan_token token;
    /** Index of the child node in pointer_batch::nodes. */
    std::size_t child;
    /** Array-context classification of the token, decided once at compile time. */
    token_array_kind array_kind{token_array_kind::object_key};
    /** Parsed index when array_kind == array_index. */
    std::size_t array_index_value{0};
  };

  /** A node of the plan trie. */
  struct plan_node {
    std::vector<plan_edge> edges;
    /** Input slots whose pointer ends exactly at this node (duplicates kept). */
    std::vector<std::size_t> result_slots;
    /** Every input slot beneath this node (used to prune unreachable subtrees). */
    std::vector<std::size_t> subtree_slots;
    /** Start of this node's per-execution match flags in pointer_batch's flat buffer. */
    std::size_t seen_offset{0};
  };

  std::vector<plan_node> nodes;
  std::size_t slot_count{0};
  /** Total length of the flat per-node match flag buffer. */
  std::size_t seen_size{0};

  simdjson_inline explicit pointer_batch(std::vector<plan_node> &&trie,
                                         std::size_t slots) noexcept;

  /** Accessor for the execution engine, which lives in the -inl header. */
  simdjson_inline const plan_node &node(std::size_t index) const noexcept;

  /** Array-context classification of one decoded token, decided at compile time. */
  static token_array_kind classify_array_token(const std::string &name,
                                               std::size_t &index_value) noexcept;

  template <typename InputIt>
  simdjson_warn_unused static simdjson_result<pointer_batch> compile_range(
      InputIt first, InputIt last) noexcept;

  friend simdjson_warn_unused error_code pointer_batch_detail::walk(
      value_iterator vi, std::size_t node_idx, const pointer_batch &plan,
      pointer_batch_detail::exec_state &state) noexcept;
  friend struct simdjson_result<pointer_batch>;
};

} // namespace ondemand
} // namespace SIMDJSON_IMPLEMENTATION
} // namespace simdjson

namespace simdjson {

template<>
struct simdjson_result<SIMDJSON_IMPLEMENTATION::ondemand::pointer_batch>
    : public SIMDJSON_IMPLEMENTATION::implementation_simdjson_result_base<
          SIMDJSON_IMPLEMENTATION::ondemand::pointer_batch> {
public:
  simdjson_inline simdjson_result(SIMDJSON_IMPLEMENTATION::ondemand::pointer_batch &&value) noexcept;
  simdjson_inline simdjson_result(error_code error) noexcept;
  simdjson_inline simdjson_result() noexcept {}
};

} // namespace simdjson

#endif // SIMDJSON_GENERIC_ONDEMAND_POINTER_BATCH_H
