// Projects a handful of collected fields out of an offline-captured JSON
// response with ondemand::pointer_batch. The response prefixes are long and
// shared by every requested field: a single forward walk reaches them all,
// never rewinding, and each result keeps the verbatim bytes of its value so a
// later cleaning stage can parse (or reject) the raw fragments on its own.

#include <cstdio>
#include <string>
#include <vector>

#include "simdjson.h"

using namespace simdjson;

int main() {
  // One captured response: a deeply nested envelope with only a few fields we
  // care about, plus an ancestor we want to keep whole.
  auto response = R"({
    "status": 200,
    "response": {
      "payload": {
        "envelope": {
          "request_id": "req-7f31",
          "record": {
            "id": 1042,
            "name": "capture-01",
            "tags": ["alpha", "beta"],
            "signature": "a+b=c~d",
            "owner": { "team": "ingest", "on_call": true }
          },
          "trace": { "sampled": false, "latency_ms": 17 }
        }
      }
    }
  })"_padded;

  // The plan is compiled once. It owns decoded copies of every RFC 6901
  // pointer and can be reused for every response with the same shape.
  auto plan_result = ondemand::pointer_batch::compile({
      "",
      "/response/payload/envelope/request_id",
      "/response/payload/envelope/record/id",
      "/response/payload/envelope/record/name",
      "/response/payload/envelope/record/tags/0",
      "/response/payload/envelope/record/owner/team",
      "/response/payload/envelope/trace/latency_ms",
      "/response/payload/envelope/record/missing"});
  if (plan_result.error()) {
    std::fprintf(stderr, "failed to compile pointer batch: %s\n",
                 error_message(plan_result.error()));
    return 1;
  }
  const auto plan = std::move(plan_result).value_unsafe();

  ondemand::parser parser;
  ondemand::document doc;
  auto error = parser.iterate(response).get(doc);
  if (error) {
    std::fprintf(stderr, "failed to iterate response: %s\n",
                 error_message(error));
    return 1;
  }

  std::vector<ondemand::pointer_batch_result> projected;
  ondemand::pointer_batch_statistics stats;
  error = plan.execute(doc, projected, stats);
  if (error) {
    std::fprintf(stderr, "unrecoverable document error: %s\n",
                 error_message(error));
    return 1;
  }

  // The result order matches the compile order; per-pointer failures (here,
  // the absent "missing" field) never interrupt the other projections.
  const char *labels[] = {
      "root",       "request_id",      "id",       "name",
      "tags[0]",    "owner.team",      "latency",  "missing"};
  for (std::size_t i = 0; i < projected.size(); ++i) {
    if (projected[i].error == SUCCESS) {
      std::printf("%-12s -> %.*s\n", labels[i],
                  static_cast<int>(projected[i].raw.size()),
                  projected[i].raw.data());
    } else {
      std::printf("%-12s -> <%s>\n", labels[i],
                  error_message(projected[i].error));
    }
  }

  // The walk touched each shared container once instead of rewinding per
  // pointer, and skipped everything no pointer named.
  std::printf("work: %zu container entries, %zu field reads, %zu array advances\n",
              stats.container_entries, stats.field_reads,
              stats.array_advances);
  return 0;
}
