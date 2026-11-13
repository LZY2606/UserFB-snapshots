// Target checks for re2::ComparePolicies().
//
// Each group drives the public policy-difference API declared in
// re2/policy_diff.h and makes real assertions through CHECK_CHECK.  Where the
// shortest-witness property is checked, an independent brute-force pass runs
// RE2::FullMatch over every string up to a small length bound on a small
// alphabet and computes the first disagreement itself; the completeness group
// additionally pins witnesses longer than that bound, which a finite
// enumeration could never produce.

#include <pthread.h>

#include <functional>
#include <string>
#include <vector>

#include "checks/harness.h"
#include "re2/policy_diff.h"
#include "re2/re2.h"

using re2::PolicyDiffResult;
using re2::PolicyRule;
using re2::RE2;

namespace {

RE2::Options Latin1Options() {
  RE2::Options options;
  options.set_encoding(RE2::Options::EncodingLatin1);
  options.set_log_errors(false);
  return options;
}

// A policy kept as both the public rule type and precompiled RE2 objects, so
// the brute-force oracle performs ordinary FullMatch calls in rule order.
struct Policy {
  std::vector<PolicyRule> rules;
  std::vector<RE2*> compiled;
  int default_action;

  Policy() : default_action(0) {}
  ~Policy() {
    for (size_t i = 0; i < compiled.size(); i++)
      delete compiled[i];
  }

  void Add(const std::string& pattern, int action) {
    rules.push_back(PolicyRule(pattern, action));
    compiled.push_back(new RE2(pattern, Latin1Options()));
  }

  // Returns the action assigned to s and writes the first matching rule
  // index (-1 for the default rule), exactly as a rule-by-rule
  // RE2::FullMatch scan would.
  int Evaluate(const std::string& s, int* rule_index) const {
    for (size_t i = 0; i < compiled.size(); i++) {
      if (RE2::FullMatch(s, *compiled[i])) {
        *rule_index = static_cast<int>(i);
        return rules[i].action;
      }
    }
    *rule_index = -1;
    return default_action;
  }
};

std::string Escape(const std::string& s) {
  std::string out;
  char buf[8];
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 32 && c < 127) {
      out.push_back(static_cast<char>(c));
    } else {
      snprintf(buf, sizeof buf, "\\x%02x", c);
      out += buf;
    }
  }
  return out;
}

// Re-runs RE2::FullMatch over the rules on the witness and checks that the
// reported rule indices and actions are exactly the first-match results.
void CheckWitnessReplay(const PolicyDiffResult& r,
                        const Policy& left, const Policy& right) {
  int left_index = -2;
  int right_index = -2;
  int left_action = left.Evaluate(r.witness, &left_index);
  int right_action = right.Evaluate(r.witness, &right_index);
  CHECK_CHECK(left_index == r.left_rule);
  CHECK_CHECK(right_index == r.right_rule);
  CHECK_CHECK(left_action == r.left_action);
  CHECK_CHECK(right_action == r.right_action);
  CHECK_CHECK(left_action != right_action);
}

// Enumerates every string of length 0..max_length over alphabet and returns
// the shortest (then unsigned-lexicographically smallest) string on which the
// two policies act differently.  Sets *found accordingly.
std::string BruteFirstDifference(const Policy& left, const Policy& right,
                                 const std::vector<int>& alphabet,
                                 int max_length, bool* found) {
  std::string best;
  *found = false;
  std::string s;
  // Iterative recursion through a lambda.
  std::function<void(int)> recurse;
  recurse = [&](int depth) {
    int li, ri;
    if (left.Evaluate(s, &li) != right.Evaluate(s, &ri)) {
      if (!*found || s.size() < best.size() ||
          (s.size() == best.size() && s < best)) {
        best = s;
        *found = true;
      }
    }
    if (depth == max_length)
      return;
    for (size_t i = 0; i < alphabet.size(); i++) {
      s.push_back(static_cast<char>(alphabet[i]));
      recurse(depth + 1);
      s.pop_back();
    }
  };
  recurse(0);
  return best;
}

// Runs an enumeration-based equivalence/witness check on one policy pair.
void CheckPairByEnumeration(const Policy& left, const Policy& right,
                            const std::vector<int>& alphabet,
                            int max_length, bool expect_equivalent) {
  bool brute_found = false;
  std::string brute_witness =
      BruteFirstDifference(left, right, alphabet, max_length, &brute_found);

  PolicyDiffResult r = re2::ComparePolicies(left.rules, left.default_action,
                                            right.rules,
                                            right.default_action);
  if (expect_equivalent) {
    CHECK_CHECK(!brute_found);
    CHECK_CHECK(r.status == re2::Equivalent);
    CHECK_CHECK(r.witness.empty());
    return;
  }
  CHECK_CHECK(brute_found);
  CHECK_CHECK(r.status == re2::Different);
  CHECK_CHECK(r.witness == brute_witness);
  CheckWitnessReplay(r, left, right);
}

struct ThreadArg {
  const Policy* left;
  const Policy* right;
  std::string expected_witness;
  bool ok;
};

void* ThreadMain(void* opaque) {
  ThreadArg* arg = static_cast<ThreadArg*>(opaque);
  for (int i = 0; i < 25; i++) {
    PolicyDiffResult r = re2::ComparePolicies(
        arg->left->rules, arg->left->default_action,
        arg->right->rules, arg->right->default_action);
    arg->ok = (r.status == re2::Different &&
               r.witness == arg->expected_witness);
    if (!arg->ok)
      break;
  }
  return NULL;
}

}  // namespace

void check_case(const char* group) {
  const std::string g = group;

  if (g == "priority") {
    // The documented admin/ example: a rejecting rule in front of a catch-all
    // must surface a real action-changing path.
    {
      Policy left, right;
      left.Add("admin/.*", 0);
      left.Add(".*", 1);
      left.default_action = 1;
      right.Add(".*", 1);
      right.default_action = 1;

      PolicyDiffResult r = re2::ComparePolicies(
          left.rules, left.default_action, right.rules, right.default_action);
      CHECK_CHECK(r.status == re2::Different);
      CHECK_CHECK(r.witness == "admin/");
      CHECK_CHECK(r.left_rule == 0);
      CHECK_CHECK(r.right_rule == 0);
      CheckWitnessReplay(r, left, right);
    }

    // Reordering rules that carry the same action does not change anything.
    {
      Policy left, right;
      left.Add("a", 2);
      left.Add(".*", 2);
      right.Add(".*", 2);
      right.Add("a", 2);
      PolicyDiffResult r = re2::ComparePolicies(
          left.rules, 7, right.rules, 7);
      CHECK_CHECK(r.status == re2::Equivalent);
    }

    // A rule fully shadowed by an earlier catch-all is harmless.
    {
      Policy left, right;
      left.Add(".*", 7);
      left.Add("a", 9);  // never reached
      right.Add(".*", 7);
      PolicyDiffResult r = re2::ComparePolicies(
          left.rules, 3, right.rules, 3);
      CHECK_CHECK(r.status == re2::Equivalent);
    }

    // First-match ordering decides between two rules matching the same
    // string; the reported index is the first FullMatch hit.
    {
      Policy left, right;
      left.Add("a", 1);
      left.Add("a", 2);
      right.Add("a", 2);
      PolicyDiffResult r = re2::ComparePolicies(
          left.rules, 0, right.rules, 0);
      CHECK_CHECK(r.status == re2::Different);
      CHECK_CHECK(r.witness == "a");
      CHECK_CHECK(r.left_rule == 0);
      CHECK_CHECK(r.right_rule == 0);
      CHECK_CHECK(r.left_action == 1);
      CHECK_CHECK(r.right_action == 2);
      CheckWitnessReplay(r, left, right);
    }
  }

  if (g == "assertions") {
    // Boundary assertions and rule shadowing on a single ordered chain.
    // The catch-all shadows nothing structurally but defines the default
    // action on every non-matching string; brute force over a small alphabet
    // checks the whole chain including ^, $, \b and newline handling.
    Policy left, right;
    left.Add("(?m)^a$", 1);
    left.Add("a\\b", 2);
    left.Add(".*", 3);
    left.default_action = 5;
    right.Add(".*", 3);
    right.default_action = 5;

    const std::vector<int> alphabet = {'a', 'b', '\n'};
    CheckPairByEnumeration(left, right, alphabet, 4, false);

    // Explicit \A, \z and \B participate correctly, again on one chain with
    // an ordered fallback rule.
    Policy p, q;
    p.Add("\\Aa\\z", 4);
    p.Add("a\\B", 6);
    p.Add("a*", 8);
    p.default_action = 2;
    q.Add("a*", 8);
    q.default_action = 2;
    CheckPairByEnumeration(p, q, alphabet, 4, false);

    // Anchored policies that genuinely agree must be reported equivalent
    // rather than producing a spurious assertion-based witness.  Over an
    // alphabet without newlines, multiline line anchors add no distinction.
    Policy e1, e2;
    e1.Add("(?m)^[ab]*$", 1);
    e2.Add("[ab]*", 1);
    const std::vector<int> no_newline = {'a', 'b'};
    CheckPairByEnumeration(e1, e2, no_newline, 4, true);
  }

  if (g == "shortest") {
    // Finite enumeration on a small alphabet is the independent oracle for
    // the shortest and lexicographically smallest witness.
    const std::vector<int> alphabet = {'a', 'b', 0};
    Policy p1, p2, p3, p4, p5;
    p1.Add("a+", 1);
    p1.default_action = 0;
    p2.Add("ab", 1);
    p2.default_action = 0;
    p3.Add("[ab]", 2);
    p3.Add("b", 1);  // shadowed on b only by ordering semantics
    p3.default_action = 9;
    p4.Add("a*", 1);
    p4.default_action = 0;
    p5.Add(".*", 1);
    p5.default_action = 0;

    CheckPairByEnumeration(p1, p2, alphabet, 3, false);
    CheckPairByEnumeration(p2, p3, alphabet, 3, false);
    CheckPairByEnumeration(p3, p4, alphabet, 3, false);
    CheckPairByEnumeration(p4, p5, alphabet, 3, false);

    // Two formulations of the same language agree on every string.
    Policy same1, same2;
    same1.Add("[ab]*", 1);
    same2.Add("(?:a|b)*", 1);
    CheckPairByEnumeration(same1, same2, alphabet, 3, true);

    // A difference visible on the empty string gives a zero-length witness.
    {
      Policy left, right;
      left.Add("(?:)", 5);
      left.default_action = 0;
      right.default_action = 0;
      PolicyDiffResult r = re2::ComparePolicies(
          left.rules, left.default_action, right.rules, right.default_action);
      CHECK_CHECK(r.status == re2::Different);
      CHECK_CHECK(r.witness.empty());
      CHECK_CHECK(r.witness.size() == 0);
      CheckWitnessReplay(r, left, right);
    }

    // A NUL byte is a real, embeddable witness byte and the smallest
    // unsigned byte among the one-byte candidates.
    {
      Policy left, right;
      left.Add("[\\x00a]", 0);
      left.default_action = 1;
      right.default_action = 1;
      PolicyDiffResult r = re2::ComparePolicies(
          left.rules, left.default_action, right.rules, right.default_action);
      CHECK_CHECK(r.status == re2::Different);
      CHECK_CHECK(r.witness.size() == 1);
      CHECK_CHECK(static_cast<unsigned char>(r.witness[0]) == 0);
      CheckWitnessReplay(r, left, right);
    }
  }

  if (g == "completeness") {
    // Exact-length witnesses longer than any small-sample enumeration bound.
    {
      const int n = 60;
      std::string pattern = "a{" + std::to_string(n) + "}";
      Policy left, right;
      left.Add(pattern, 0);
      left.default_action = 1;
      right.default_action = 1;
      PolicyDiffResult r = re2::ComparePolicies(
          left.rules, left.default_action, right.rules, right.default_action);
      CHECK_CHECK(r.status == re2::Different);
      CHECK_CHECK(r.witness == std::string(n, 'a'));
      CheckWitnessReplay(r, left, right);
    }

    // A repeating two-byte unit yields a 60-byte witness with exact content.
    {
      Policy left, right;
      left.Add("(?:ab){30}", 0);
      left.default_action = 1;
      right.default_action = 1;
      PolicyDiffResult r = re2::ComparePolicies(
          left.rules, left.default_action, right.rules, right.default_action);
      std::string expected;
      for (int i = 0; i < 30; i++)
        expected += "ab";
      CHECK_CHECK(r.status == re2::Different);
      CHECK_CHECK(r.witness == expected);
      CHECK_CHECK(r.witness.size() == 60);
      CheckWitnessReplay(r, left, right);
    }

    // Equivalence is decided over ALL strings, not up to a bound: different
    // syntax for the same language with the same action is equivalent.
    {
      Policy left, right;
      left.Add("[ab]*", 1);
      right.Add("(?:a|b)*", 1);
      PolicyDiffResult r = re2::ComparePolicies(
          left.rules, 4, right.rules, 4);
      CHECK_CHECK(r.status == re2::Equivalent);

      Policy e1, e2;
      e1.Add("a*", 1);
      e2.Add("(?:a)*", 1);
      PolicyDiffResult s = re2::ComparePolicies(
          e1.rules, 4, e2.rules, 4);
      CHECK_CHECK(s.status == re2::Equivalent);
    }

    // Both rule tables empty: only the defaults act, so differing defaults
    // disagree already on the empty string.
    {
      Policy left, right;
      left.default_action = 1;
      right.default_action = 2;
      PolicyDiffResult r = re2::ComparePolicies(
          left.rules, left.default_action, right.rules, right.default_action);
      CHECK_CHECK(r.status == re2::Different);
      CHECK_CHECK(r.witness.empty());
      CHECK_CHECK(r.left_rule == -1);
      CHECK_CHECK(r.right_rule == -1);
    }
  }

  if (g == "isolation") {
    // An invalid pattern is reported per side and index, never silently
    // treated as the empty language.
    {
      Policy bad, good;
      bad.Add("[a", 1);  // missing closing bracket
      good.Add(".*", 1);
      PolicyDiffResult left_bad = re2::ComparePolicies(
          bad.rules, 0, good.rules, 0);
      CHECK_CHECK(left_bad.status == re2::Invalid);
      CHECK_CHECK(left_bad.error.find("left") != std::string::npos);
      CHECK_CHECK(left_bad.error.find("index 0") != std::string::npos);

      PolicyDiffResult right_bad = re2::ComparePolicies(
          good.rules, 0, bad.rules, 0);
      CHECK_CHECK(right_bad.status == re2::Invalid);
      CHECK_CHECK(right_bad.error.find("right") != std::string::npos);
      CHECK_CHECK(right_bad.error.find("index 0") != std::string::npos);
    }

    // Repeated analysis of the same rules is stable.
    {
      Policy left, right;
      left.Add("a{7}", 0);
      left.default_action = 1;
      right.default_action = 1;
      std::string first;
      for (int i = 0; i < 10; i++) {
        PolicyDiffResult r = re2::ComparePolicies(
            left.rules, left.default_action, right.rules, right.default_action);
        CHECK_CHECK(r.status == re2::Different);
        if (i == 0)
          first = r.witness;
        else
          CHECK_CHECK(r.witness == first);
      }
      CHECK_CHECK(first == std::string(7, 'a'));
    }

    // Ordinary RE2 matching behaves identically around analyses.
    {
      RE2 re("admin/.*", Latin1Options());
      CHECK_CHECK(RE2::FullMatch("admin/x", re));
      CHECK_CHECK(!RE2::FullMatch("user/x", re));
      Policy left, right;
      left.Add("admin/.*", 0);
      left.Add(".*", 1);
      left.default_action = 1;
      right.Add(".*", 1);
      right.default_action = 1;
      PolicyDiffResult r = re2::ComparePolicies(
          left.rules, left.default_action, right.rules, right.default_action);
      CHECK_CHECK(r.status == re2::Different);
      CHECK_CHECK(RE2::FullMatch("admin/x", re));
      CHECK_CHECK(!RE2::FullMatch("user/x", re));
    }

    // Concurrent independent analyses each return the correct witness.
    {
      Policy left, right;
      left.Add("admin/.*", 0);
      left.Add(".*", 1);
      left.default_action = 1;
      right.Add(".*", 1);
      right.default_action = 1;

      const int kThreads = 4;
      pthread_t threads[kThreads];
      ThreadArg args[kThreads];
      for (int i = 0; i < kThreads; i++) {
        args[i].left = &left;
        args[i].right = &right;
        args[i].expected_witness = "admin/";
        args[i].ok = false;
        pthread_create(&threads[i], NULL, ThreadMain, &args[i]);
      }
      for (int i = 0; i < kThreads; i++) {
        pthread_join(threads[i], NULL);
        CHECK_CHECK(args[i].ok);
      }
    }
  }
}
