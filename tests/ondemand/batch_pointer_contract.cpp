// Contract tests for ondemand::pointer_batch.
//
// This file has no main(): harness/main.c drives batch_pointer_contract() and
// counts the four registered groups. The batch walks each document in a single
// forward pass, so every assertion below runs against unconsumed documents.

#include "simdjson.h"
#include "case.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace simdjson;

namespace {

void check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "batch_pointer_contract failure: %s\n", message);
    std::abort();
  }
}

// Execute a freshly compiled plan and hand back the per-pointer slots.
std::vector<ondemand::pointer_batch_result>
run(ondemand::parser &parser, const padded_string &json,
    std::initializer_list<std::string_view> pointers) {
  ondemand::document doc;
  auto iterate_error = parser.iterate(json).get(doc);
  check(iterate_error == SUCCESS, "iterate succeeds");
  auto plan_result = ondemand::pointer_batch::compile(pointers);
  check(plan_result.error() == SUCCESS, "plan compiles");
  auto plan = std::move(plan_result).value_unsafe();
  std::vector<ondemand::pointer_batch_result> results;
  ondemand::pointer_batch_statistics stats;
  const error_code executed = plan.execute(doc, results, stats);
  check(executed == SUCCESS, "execute succeeds");
  check(doc.at_end(), "execute consumes the whole document");
  return results;
}

// Shared prefixes, ancestor/descendant raw bytes and verbatim formatting.
void prefix_raw_group() {
  ondemand::parser parser;

  // A root scalar addressed by the empty pointer.
  {
    auto json = "123"_padded;
    auto results = run(parser, json, {""});
    check(results.size() == 1, "one root slot");
    check(results[0].error == SUCCESS, "root scalar succeeds");
    check(results[0].raw == "123", "root scalar raw bytes");
  }

  // A long shared prefix is traversed once while several leaves below it,
  // ancestors and an empty container are all projected.
  {
    auto json =
        R"({"payload":{"envelope":{"data":{"a":1,"b":[10,20,{"deep":"x\ny"}],"c":{}}}}})"_padded;
    auto results = run(parser, json, {
        "/payload/envelope/data",
        "/payload/envelope/data/a",
        "/payload/envelope/data/b/2/deep",
        "/payload/envelope/data/c"});
    check(results[0].error == SUCCESS, "ancestor object succeeds");
    check(results[0].raw == R"({"a":1,"b":[10,20,{"deep":"x\ny"}],"c":{}})",
          "ancestor raw is the complete value, descendants included");
    check(results[1].raw == "1", "sibling leaf raw");
    check(results[2].raw == R"("x\ny")",
          "string raw keeps the escape and both quotes, borrowing input bytes");
    check(results[3].raw == "{}", "empty object raw");
  }

  // Internal whitespace is preserved inside a value; trailing whitespace is
  // not part of the raw bytes.
  {
    auto json = R"({"r": { "x" : [ 1 , 2 ] } } )"_padded;
    auto results = run(parser, json, {"/r", "/r/x/1", "/r/x"});
    check(results[0].raw == R"({ "x" : [ 1 , 2 ] })",
          "ancestor raw keeps internal whitespace only");
    check(results[0].raw.back() == '}', "no trailing whitespace after ancestor");
    check(results[1].raw == "2", "nested array element");
    check(results[2].raw == R"([ 1 , 2 ])", "nested array raw keeps whitespace");
  }

  // A root container addressed by the empty pointer.
  {
    auto json = R"([1, {"k":2}])"_padded;
    auto results = run(parser, json, {""});
    check(results[0].raw == R"([1, {"k":2}])", "root array raw via empty pointer");
  }

  contract_case("prefix-raw");
}

// Compile-time decoding, key matching, duplicate members and per-pointer errors.
void pointer_keys_group() {
  ondemand::parser parser;

  // The empty reference token names the empty-string key.
  {
    auto json = R"({"":{"k":2},"a":3})"_padded;
    auto results = run(parser, json, {"/", "//k", "/a"});
    check(results[0].raw == R"({"k":2})", "empty key member");
    check(results[1].raw == "2", "nested value under empty key");
    check(results[2].raw == "3", "ordinary sibling");
  }

  // Pointer ~0/~1 are decoded once at compile time, and document keys are
  // matched on their JSON-decoded names: literal "a" and its \u0061 spelling
  // are the same key and hit the first occurrence.
  {
    auto json = "{\"a/b\":1,\"a~b\":2,\"\\u0061\":3,\"~\":4,\"/\":5}"_padded;
    auto results = run(parser, json,
                       {"/a~1b", "/a~0b", "/a", "/~0", "/~1"});
    check(results[0].raw == "1", "~1 decodes to '/' at compile time");
    check(results[1].raw == "2", "~0 decodes to '~' at compile time");
    check(results[2].raw == "3", "escaped document key matches decoded name");
    check(results[3].raw == "4", "literal tilde key via ~0");
    check(results[4].raw == "5", "literal slash key via ~1");
  }

  // A valid escaped spelling of the requested name (\/ -> '/') matches after
  // one decode, while an invalid escape inside an *unrequested* member follows
  // the skip logic and never fails the batch or touches the decode buffer's
  // lifetime guarantees.
  {
    auto json = "{\"a\\/b\":6,\"bad\\q\":7,\"ok\":8}"_padded;
    ondemand::document doc;
    check(parser.iterate(json).get(doc) == SUCCESS, "iterate escaped doc");
    auto plan = ondemand::pointer_batch::compile(
                    {"/a~1b", "/ok", "/bad"})
                    .value_unsafe();
    std::vector<ondemand::pointer_batch_result> results;
    check(plan.execute(doc, results) == SUCCESS,
          "invalid escape in an unrequested member is not fatal");
    check(results[0].raw == "6", "valid \\/ escape matches the decoded slash key");
    check(results[1].raw == "8", "sibling still projected");
    check(results[2].error == NO_SUCH_FIELD,
          "an undecodable key can never equal a valid plan name");
    check(doc.at_end(), "document fully consumed");
  }

  // Duplicate members: the first one wins, and the second can never backfill
  // a descendant that the first member is missing.
  {
    auto json = R"({"k":{"x":1},"k":2,"j":7,"j":8})"_padded;
    auto results = run(parser, json, {"/k", "/k/x", "/k/y", "/j"});
    check(results[0].raw == R"({"x":1})", "first duplicate member is projected");
    check(results[1].raw == "1", "descendant comes from the first member");
    check(results[2].error == NO_SUCH_FIELD,
          "a later duplicate does not supply the missing descendant");
    check(results[3].raw == "7", "first duplicate scalar member wins");
  }

  // Array token rules: 0 is legal, and every malformed index word fails only
  // its own slot with INVALID_JSON_POINTER.
  {
    auto json = R"([10,20,30])"_padded;
    auto results = run(parser, json,
                       {"/0", "/2", "/3", "/01", "/-1", "/abc",
                        "/999999999999999999999999"});
    check(results[0].raw == "10", "index 0 is legal");
    check(results[1].raw == "30", "last element");
    check(results[2].error == INDEX_OUT_OF_BOUNDS, "index past the end");
    check(results[3].error == INVALID_JSON_POINTER, "leading zero");
    check(results[4].error == INVALID_JSON_POINTER, "negative sign");
    check(results[5].error == INVALID_JSON_POINTER, "non-numeric array word");
    check(results[6].error == INVALID_JSON_POINTER, "word overflowing size_t");
  }

  // The very same words are ordinary keys in an object.
  {
    auto json = R"({"01":1,"-1":2,"abc":3,"0":4})"_padded;
    auto results = run(parser, json, {"/01", "/-1", "/abc", "/0"});
    check(results[0].raw == "1" && results[1].raw == "2" &&
              results[2].raw == "3" && results[3].raw == "4",
          "array-invalid words stay ordinary object keys");
  }

  // Mixed successes and failures in one batch never interrupt each other.
  {
    auto json = R"({"o":[1,2],"a":{"k":1},"s":"v"})"_padded;
    auto results = run(parser, json,
                       {"/o/0", "/o/9", "/o/xy", "/a/missing",
                        "/a/k", "/s/deep", "/nowhere"});
    check(results[0].raw == "1", "successful slot beside failures");
    check(results[1].error == INDEX_OUT_OF_BOUNDS, "array out of bounds slot");
    check(results[2].error == INVALID_JSON_POINTER, "key word inside array");
    check(results[3].error == NO_SUCH_FIELD, "missing object field slot");
    check(results[4].raw == "1", "another successful slot");
    check(results[5].error == NO_SUCH_FIELD,
          "well-formed pointer below a scalar follows the single-pointer rule");
    check(results[6].error == NO_SUCH_FIELD, "missing top-level field");
  }

  // Compilation is all-or-nothing and independent of the caller's storage.
  {
    check(ondemand::pointer_batch::compile({"no-leading-slash"}).error() ==
              INVALID_JSON_POINTER,
          "missing leading slash fails compilation");
    check(ondemand::pointer_batch::compile({"/a~"}).error() ==
              INVALID_JSON_POINTER,
          "dangling tilde fails compilation");
    check(ondemand::pointer_batch::compile({"/a~2/b"}).error() ==
              INVALID_JSON_POINTER,
          "illegal escape fails the whole plan");
    check(ondemand::pointer_batch::compile({""}).error() == SUCCESS,
          "the empty pointer is valid");

    std::vector<std::string> owned{"/a/b", "/a/c"};
    auto plan_result =
        ondemand::pointer_batch::compile(owned.begin(), owned.end());
    check(plan_result.error() == SUCCESS, "range compile succeeds");
    auto plan = std::move(plan_result).value_unsafe();
    check(plan.size() == 2, "slot count matches the input range");
    owned.clear();
    owned.push_back("/overwritten");

    auto json = R"({"a":{"b":11,"c":22}})"_padded;
    ondemand::document doc;
    check(parser.iterate(json).get(doc) == SUCCESS, "iterate after strings moved");
    std::vector<ondemand::pointer_batch_result> results;
    check(plan.execute(doc, results) == SUCCESS, "compiled plan owns its tokens");
    check(results[0].raw == "11" && results[1].raw == "22",
          "mutating the source strings does not affect the plan");
  }

  // A document error the library calls unrecoverable fails the whole execute.
  {
    auto json = R"({"a":1 "b":2})"_padded;
    ondemand::document doc;
    check(parser.iterate(json).get(doc) == SUCCESS, "malformed doc still iterates");
    auto plan = ondemand::pointer_batch::compile({"/a", "/b"}).value_unsafe();
    std::vector<ondemand::pointer_batch_result> results;
    const error_code fatal = plan.execute(doc, results);
    check(is_fatal(fatal), "structural corruption is a fatal batch failure");
    check(!doc.is_alive(), "the document is unusable after the fatal error");
  }

  contract_case("pointer-keys");
}

// Plans are immutable and carry no per-document state: reuse across documents,
// across parsers and across threads, with result views tied to the input bytes.
void document_reuse_group() {
  const padded_string json1 =
      R"({"id":"first","payload":{"v":1}})"_padded;
  const padded_string json2 =
      R"({"id":"second","payload":{"v":2}})"_padded;
  const padded_string json3 =
      R"({"id":"third","payload":{"v":3}})"_padded;

  auto plan_result =
      ondemand::pointer_batch::compile({"/id", "/payload/v"});
  check(plan_result.error() == SUCCESS, "shared plan compiles");
  const auto plan = std::move(plan_result).value_unsafe();

  ondemand::parser parser1;
  std::vector<ondemand::pointer_batch_result> first_results;
  {
    ondemand::document doc;
    check(parser1.iterate(json1).get(doc) == SUCCESS, "iterate json1");
    check(plan.execute(doc, first_results) == SUCCESS, "first execution");
  }
  check(first_results[0].raw == "\"first\"", "first id captured");
  check(first_results[1].raw == "1", "first payload captured");

  // The same parser moves on to another still-living input, and a second
  // parser works a different document in parallel with the saved views.
  {
    ondemand::document doc3;
    check(parser1.iterate(json3).get(doc3) == SUCCESS, "parser re-iterates");
    std::vector<ondemand::pointer_batch_result> third_results;
    check(plan.execute(doc3, third_results) == SUCCESS, "reused plan on new doc");
    check(third_results[0].raw == "\"third\"", "new document id");

    ondemand::parser parser2;
    ondemand::document doc2;
    check(parser2.iterate(json2).get(doc2) == SUCCESS, "second parser iterates");
    std::vector<ondemand::pointer_batch_result> second_results;
    check(plan.execute(doc2, second_results) == SUCCESS, "second parser executes");
    check(second_results[0].raw == "\"second\"", "second document id");

    // The earlier views still name bytes of the original, still-alive buffer.
    check(first_results[0].raw == "\"first\"",
          "saved view survives later parses on the same parser");
    check(first_results[1].raw == "1",
          "saved leaf view survives a second parser's execution");
  }

  // Concurrent executions on independent parsers share nothing mutable.
  constexpr int thread_count = 4;
  const std::string json3_source(json3.data(), json3.size());
  std::vector<std::thread> workers;
  std::vector<int> ok(thread_count, 0);
  for (int t = 0; t < thread_count; ++t) {
    workers.emplace_back([&, t] {
      ondemand::parser local_parser;
      // Each thread owns its copy of the input and its own result storage.
      padded_string local_json(json3_source);
      ondemand::document doc;
      if (local_parser.iterate(local_json).get(doc) != SUCCESS) { return; }
      std::vector<ondemand::pointer_batch_result> local_results;
      ondemand::pointer_batch_statistics local_stats;
      if (plan.execute(doc, local_results, local_stats) != SUCCESS) { return; }
      if (local_results[0].raw == "\"third\"" && local_results[1].raw == "3") {
        ok[t] = 1;
      }
    });
  }
  for (auto &worker : workers) { worker.join(); }
  for (int t = 0; t < thread_count; ++t) {
    check(ok[t] == 1, "parallel execution on independent parsers succeeds");
  }

  // In a document_stream the next document's tokens are already indexed: a
  // container raw view must still end at this document's own closing token.
  {
    auto stream_json =
        R"({"a":1,"b":[2,3]} {"x":  4 })"_padded;
    auto stream_plan =
        ondemand::pointer_batch::compile({"", "/b", "/b/1", "/a"}).value_unsafe();
    ondemand::parser stream_parser;
    auto stream = std::move(stream_parser.iterate_many(stream_json)).value_unsafe();
    int document_index = 0;
    for (auto ref_result : stream) {
      check(ref_result.error() == SUCCESS, "stream document available");
      ondemand::document_reference ref =
          std::move(ref_result).value_unsafe();
      std::vector<ondemand::pointer_batch_result> stream_results;
      check(stream_plan.execute(ref, stream_results) == SUCCESS,
            "batch executes on a streamed document reference");
      if (document_index == 0) {
        check(stream_results[0].raw == R"({"a":1,"b":[2,3]})",
              "streamed root raw stops at its own closing brace");
        check(stream_results[1].raw == "[2,3]", "streamed nested array raw");
        check(stream_results[2].raw == "3", "streamed nested element raw");
        check(stream_results[3].raw == "1", "streamed sibling raw");
      } else {
        check(stream_results[0].raw == R"({"x":  4 })",
              "second streamed document keeps its own whitespace");
        check(stream_results[1].error == NO_SUCH_FIELD &&
                  stream_results[2].error == NO_SUCH_FIELD &&
                  stream_results[3].error == NO_SUCH_FIELD,
              "missing fields in the next streamed document");
      }
      ++document_index;
    }
    check(document_index == 2, "both streamed documents were walked");
  }

  contract_case("document-reuse");
}

// Deterministic work accounting: one structural pass, shared prefixes entered
// once, ancestor capture free of extra scans, counters linear in the document.
void linear_work_group() {
  ondemand::parser parser;
  constexpr std::size_t element_count = 10;

  auto build_document = [](std::size_t n) {
    std::string text = R"({"p":{"q":{"arr":[)";
    for (std::size_t i = 0; i < n; ++i) {
      if (i) { text += ','; }
      text += R"({"id":)" + std::to_string(i) + '}';
    }
    text += R"(],"leaf":"v"}}})";
    return padded_string(text);
  };

  const auto execute_with = [&](const padded_string &json,
                                std::initializer_list<std::string_view> pointers,
                                ondemand::pointer_batch_statistics &stats) {
    ondemand::document doc;
    check(parser.iterate(json).get(doc) == SUCCESS, "iterate work document");
    auto plan_result = ondemand::pointer_batch::compile(pointers);
    check(plan_result.error() == SUCCESS, "work plan compiles");
    auto plan = std::move(plan_result).value_unsafe();
    std::vector<ondemand::pointer_batch_result> results;
    check(plan.execute(doc, results, stats) == SUCCESS, "work plan executes");
    return results;
  };

  const std::initializer_list<std::string_view> forward_pointers = {
      "/p/q/arr/2/id", "/p/q/arr/5/id", "/p/q/leaf",
      "/p/q", "/p/q/arr", ""};
  std::initializer_list<std::string_view> reverse_pointers = {
      "", "/p/q/arr", "/p/q", "/p/q/leaf",
      "/p/q/arr/5/id", "/p/q/arr/2/id"};

  const padded_string json = build_document(element_count);
  ondemand::pointer_batch_statistics forward_stats;
  ondemand::pointer_batch_statistics reverse_stats;
  auto forward_results = execute_with(json, forward_pointers, forward_stats);
  auto reverse_results = execute_with(json, reverse_pointers, reverse_stats);

  // Request order must not influence the work: the walk follows document order.
  check(forward_stats.container_entries == reverse_stats.container_entries &&
            forward_stats.field_reads == reverse_stats.field_reads &&
            forward_stats.array_advances == reverse_stats.array_advances,
        "reverse requests do not rescan anything");
  check(forward_results[0].raw == "2" && forward_results[1].raw == "5",
        "leaves resolve regardless of request order");

  // Exact counts for the hand-built document: root, p, q, arr and the two
  // entered element objects are the only containers; members are read once.
  check(forward_stats.container_entries == 6,
        "root, p, q, arr and two matched element objects are entered once");
  check(forward_stats.field_reads == 6,
        "p, q, arr, leaf and the two matched id members are read once");
  check(forward_stats.array_advances == element_count,
        "every array element is advanced through exactly once");

  // Capturing ancestors adds no work compared with the leaf-only plan.
  ondemand::pointer_batch_statistics leaf_stats;
  execute_with(json, {"/p/q/arr/2/id", "/p/q/arr/5/id", "/p/q/leaf"},
               leaf_stats);
  check(leaf_stats.container_entries == forward_stats.container_entries &&
            leaf_stats.field_reads == forward_stats.field_reads &&
            leaf_stats.array_advances == forward_stats.array_advances,
        "recording ancestor raw bytes costs zero extra traversal");

  // The counters grow with the document structure, not with a fixed budget.
  const padded_string bigger = build_document(element_count * 2);
  ondemand::pointer_batch_statistics bigger_stats;
  execute_with(bigger, forward_pointers, bigger_stats);
  check(bigger_stats.array_advances == element_count * 2 &&
            bigger_stats.container_entries == forward_stats.container_entries &&
            bigger_stats.field_reads == forward_stats.field_reads,
        "only the array work grows with the array size");

  // A root-only projection skips the document without entering any container.
  ondemand::pointer_batch_statistics root_stats;
  execute_with(json, {""}, root_stats);
  check(root_stats.container_entries == 0 &&
            root_stats.field_reads == 0 &&
            root_stats.array_advances == 0,
        "root raw capture enters nothing");

  // Projecting an object's raw never reads the members below it.
  ondemand::pointer_batch_statistics ancestor_stats;
  execute_with(json, {"/p/q"}, ancestor_stats);
  check(ancestor_stats.container_entries == 2 &&
            ancestor_stats.field_reads == 2 &&
            ancestor_stats.array_advances == 0,
        "an ancestor raw uses the skip logic instead of rescanning descendants");

  contract_case("linear-work");
}

} // namespace

void batch_pointer_contract(void) {
  prefix_raw_group();
  pointer_keys_group();
  document_reuse_group();
  linear_work_group();
  std::printf("BATCH_POINTER_CONTRACT_OK\n");
}
