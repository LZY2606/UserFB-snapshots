#include "btf_test_api.hpp"
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <limits>
static std::map<std::string, btf_case_fn> cases;
static unsigned checks = 0;
static bool failed = false;
void btf_add_case(const char* name, btf_case_fn fn) {
  if (!name || !fn || !cases.emplace(name, fn).second)
    throw std::runtime_error("invalid or duplicate case registration");
}
void btf_expect(bool condition, const char* message) {
  if (!condition) { failed = true; throw std::runtime_error(message ? message : "assertion failed"); }
  if (checks == std::numeric_limits<unsigned>::max())
    throw std::runtime_error("assertion counter overflow");
  ++checks;
}
int main() {
  const char* groups[] = {"cycles", "distinctions", "ext_refs", "malformed", "roundtrip", "depth"};
  try {
    register_btf_compaction_cases();
    if (cases.size() != 6) throw std::runtime_error("exactly six registered groups required");
    for (auto group : groups) if (!cases.count(group)) throw std::runtime_error("missing required group");
    unsigned total = 0;
    for (auto group : groups) {
      checks = 0; failed = false;
      cases.at(group)();
      if (failed) throw std::runtime_error("an assertion failed inside the group");
      if (!checks) throw std::runtime_error("group completed without an assertion");
      total += checks;
      std::printf("PASS %s checks=%u\n", group, checks);
    }
    std::printf("BTF_COMPACTION_OK groups=6 checks=%u\n", total);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "BTF_TEST_FAILED %s\n", e.what());
    return 1;
  }
}
