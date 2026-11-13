// Copyright 2026 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifndef RE2_POLICY_DIFF_H_
#define RE2_POLICY_DIFF_H_

// Policy equivalence checking.
//
// A policy is an ordered list of rules plus a default action.  An input byte
// string is tested against the rules in order: the first rule whose pattern
// fully matches the whole string supplies the action; if no rule matches, the
// default action applies.  ComparePolicies() decides, for EVERY possible byte
// string (including the empty string and strings containing embedded NUL
// bytes), whether two policies produce the same action, and on a disagreement
// produces a shortest counterexample.
//
// This is an exact decision procedure built from the same compiled regexp
// programs (re2::Prog) that ordinary RE2 matching uses, not a bounded search
// over sample strings.
//
// Usage:
//
//   std::vector<re2::PolicyRule> left, right;
//   left.push_back(re2::PolicyRule("admin/.*", /*action=*/0));
//   left.push_back(re2::PolicyRule(".*", 1));
//   right.push_back(re2::PolicyRule(".*", 1));
//
//   re2::PolicyDiffResult r =
//       re2::ComparePolicies(left, /*left_default=*/1,
//                            right, /*right_default=*/1);
//   if (r.status == re2::Equivalent) {
//     // The two policies act identically on every byte string.
//   } else if (r.status == re2::Different) {
//     // r.witness is a shortest (then lexicographically smallest) string on
//     // which the first-match rule on each side assigns different actions.
//     // Re-running RE2::FullMatch over the rules reproduces r.left_rule and
//     // r.right_rule; -1 means the default rule was selected.
//   } else {  // re2::Invalid
//     // r.error names the side and the zero-based rule index whose pattern
//     // failed to parse.  No equivalence decision was made.
//   }
//
// Semantics and limitations:
//
//   - Patterns use RE2's default syntax with EncodingLatin1, so the input
//     alphabet is the 256 raw byte values; all other options stay at their
//     defaults.  Inline flag switches such as (?i), (?m) and (?s) are
//     honored, while capturing groups do not affect action selection.
//   - The decision covers all finite byte strings, not merely strings up to
//     some length: a witness may be longer than any practical sample bound.
//   - Rules carrying the same action are interchangeable for equivalence:
//     reordering them, or adding a rule that can never change the selected
//     action (e.g. one fully shadowed by an earlier rule), yields
//     Equivalent.  Only the resulting action function matters.
//   - Both the empty rule table and rules that match the empty language are
//     normal inputs.
//   - The analysis builds its own lazily explored subset automata from fresh
//     Prog objects; it never enumerates strings, never calls
//     Prog::BuildEntireDFA(), and never borrows pointers from an RE2 DFA
//     cache.  Ordinary RE2 matching, concurrent independent analyses and
//     repeated analyses of the same rules therefore keep their usual
//     behaviour.  ComparePolicies() itself is not synchronized; distinct
//     calls that do not share mutable user objects may run concurrently.
//   - Cost is proportional to the number of reachable subset/product
//     states, which is bounded by the product of the per-pattern subset DFA
//     sizes; pathological patterns can therefore still demand substantial
//     time and memory.


#include <string>
#include <vector>

#include "re2/re2.h"

namespace re2 {

// One ordered policy rule: a full-match pattern and the action it assigns.
struct PolicyRule {
  PolicyRule() : action(0) {}
  PolicyRule(const std::string& pat, int act)
      : pattern(pat), action(act) {}

  std::string pattern;
  int action;
};

// Outcome of ComparePolicies().
enum PolicyDiffStatus {
  Equivalent = 0,  // both policies assign the same action to every possible
                   // byte string.
  Different,       // some byte string gets different actions;
                   // witness_/rule indices/actions describe one.
  Invalid,         // a rule pattern failed to parse; see error.
};

// Result of a comparison.  Rule indices are zero-based; an index of -1
// identifies the default rule (the action taken when no pattern matches).
struct PolicyDiffResult {
  PolicyDiffResult()
      : status(Equivalent),
        left_rule(-1),
        right_rule(-1),
        left_action(0),
        right_action(0) {}

  PolicyDiffStatus status;

  // Valid when status == Different: a raw byte string on which the two
  // policies disagree.  May be empty or contain embedded NUL bytes; use size()
  // rather than strlen().  It is a shortest possible witness and, among
  // witnesses of that length, the lexicographically smallest one under
  // unsigned byte comparison.
  std::string witness;

  // Valid when status == Different: the first matching rule index for
  // witness on each side (-1 for the default rule), and the actions those
  // rules assign.  Running RE2::FullMatch over the rules again reproduces
  // exactly these indices.
  int left_rule;
  int right_rule;
  int left_action;
  int right_action;

  // Valid when status == Invalid: a human-readable explanation naming
  // the side (left/right) and the zero-based rule index whose pattern is
  // invalid, together with RE2's parse error.  A pattern that fails to parse
  // is never treated as matching the empty language.
  std::string error;
};

// Compares two policies for equivalence over all byte strings.
//
// Each rule pattern is parsed with the default RE2 options except that
// RE2::Options::EncodingLatin1 is selected, so the input alphabet is the 256
// raw byte values.  Inline flag switches such as (?i), (?m) and (?s) take
// effect as usual; capturing groups do not influence action selection.
//
// On success the returned object has status Equivalent or Different.  If any
// pattern is invalid, the first invalid pattern is reported with status
// Invalid and no comparison is performed.
PolicyDiffResult ComparePolicies(const std::vector<PolicyRule>& left,
                                 int left_default,
                                 const std::vector<PolicyRule>& right,
                                 int right_default);

}  // namespace re2

#endif  // RE2_POLICY_DIFF_H_
