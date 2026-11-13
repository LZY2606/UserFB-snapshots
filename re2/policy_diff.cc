// Copyright 2026 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Exact equivalence checking for ordered full-match policies.
//
// For every rule pattern we reuse RE2's own parser and compiler
// (Regexp::Parse + Regexp::CompileToProg) and then construct, lazily and on
// demand, a subset DFA over the instructions of the compiled Prog.  The
// subset construction mirrors DFA::AddToQueue / DFA::RunStateOnByte,
// including list fall-through, the empty-width context flags (^, $, \A, \z,
// \b and \B) and the imaginary end-of-text position, so ordinary RE2 matching
// and this analysis interpret each pattern identically.
//
// A product DFA walks the subset DFAs of all rules of both policies at once.
// Each product state remembers the first matching rule on each side (or the
// default action).  BFS over real byte transitions, in ascending byte order,
// yields a shortest, lexicographically smallest witness whenever the two
// actions disagree.  No string is enumerated to drive the analysis and no
// Prog's own DFA cache is touched, so normal matching and repeated or
// concurrent analyses keep their usual behaviour.

#include "re2/policy_diff.h"

#include <stdint.h>

#include <algorithm>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "re2/prog.h"
#include "re2/re2.h"
#include "re2/regexp.h"

namespace re2 {

namespace {

// One compiled rule.  The object owns its Prog.
struct CompiledRule {
  std::string pattern;
  int action;
  Prog* prog;
};

// A state of the subset automaton for a single Prog.
//
// ids is the sorted set of flat Prog instruction ids that remain live after
// consuming the input prefix.  ByteRange, EmptyWidth and Match instructions
// are all retained; Nop/Capture/Fail instructions disappear into the
// epsilon closure.  last_word says whether the last real byte was a word
// character and after_flag carries the empty-width flags active at the
// current position (only kEmptyBeginLine can survive a real byte); together
// they hold exactly the history that \b, \B and the line anchors need.
struct NfaState {
  std::vector<int> ids;
  bool last_word;
  uint32_t after_flag;

  bool operator==(const NfaState& other) const {
    return last_word == other.last_word &&
           after_flag == other.after_flag && ids == other.ids;
  }
};

struct NfaStateHash {
  size_t operator()(const NfaState& s) const {
    size_t h = s.after_flag * 0x9e3779b97f4a7c15ULL;
    h ^= s.last_word ? 0x123456789abcdef0ULL : 0x0fedcba987654321ULL;
    for (int id : s.ids) {
      h ^= static_cast<size_t>(id) + 0x9e3779b97f4a7c15ULL + (h << 6) +
           (h >> 2);
    }
    return h;
  }
};

// Lazily explored subset automaton for one compiled pattern.  Each
// ComparePolicies() call builds its own instances, so no locking is needed.
class PatternAutomaton {
 public:
  explicit PatternAutomaton(Prog* prog) : prog_(prog) {
    NfaState start;
    start.last_word = false;
    // At the beginning of the text both \A and ^ are active.
    start.after_flag = kEmptyBeginText | kEmptyBeginLine;
    std::unordered_set<int> seen;
    Closure(prog_->start(), start.after_flag, &start.ids, &seen);
    Canonicalize(&start.ids);
    start_id_ = Intern(start);

    NfaState dead;
    dead.last_word = false;
    dead.after_flag = 0;
    dead_id_ = Intern(dead);  // empty id set: the unique dead state
  }

  int start() const { return start_id_; }
  bool is_dead(int sid) const { return sid == dead_id_; }

  // State reached by reading real byte c (0..255) from sid, built on demand.
  int Go(int sid, int c) {
    TransitionKey key{sid, c};
    auto it = trans_.find(key);
    if (it != trans_.end())
      return it->second;

    const NfaState& s = states_[sid];

    // Flags active immediately before c.
    uint32_t before = s.after_flag;
    if (c == '\n')
      before |= kEmptyEndLine;  // implicit $ before a newline
    bool is_word = Prog::IsWordChar(static_cast<uint8_t>(c));
    before |= (is_word == s.last_word) ? kEmptyNonWordBoundary
                                       : kEmptyWordBoundary;

    // Re-run any EmptyWidth threads that are satisfied at this position,
    // then let the surviving ByteRange threads consume c.
    std::vector<int> queued;
    std::unordered_set<int> seen;
    ExpandForFlags(s.ids, before, &queued, &seen);

    uint32_t after = (c == '\n') ? kEmptyBeginLine : 0;
    std::vector<int> next_ids;
    seen.clear();
    for (int id : queued) {
      Prog::Inst* ip = prog_->inst(id);
      if (ip->opcode() == kInstByteRange && ip->Matches(c))
        Closure(ip->out(), after, &next_ids, &seen);
    }

    NfaState ns;
    ns.ids.swap(next_ids);
    ns.last_word = is_word;
    ns.after_flag = after;
    Canonicalize(&ns.ids);
    int nsid = Intern(ns);
    trans_.emplace(key, nsid);
    return nsid;
  }

  // Whether the pattern fully matches the input prefix leading to sid.
  // Mirrors DFA::RunStateOnByte(..., kByteEndText): end-of-text supplies
  // $ and \z, the word boundary reflects the final real byte, and every
  // Match instruction reachable under those flags accepts (an explicit
  // end anchor is enforced on real bytes, never at end-of-text itself).
  bool Accepts(int sid) {
    auto it = accept_.find(sid);
    if (it != accept_.end())
      return it->second;

    const NfaState& s = states_[sid];
    uint32_t flag = s.after_flag | kEmptyEndLine | kEmptyEndText;
    flag |= s.last_word ? kEmptyWordBoundary : kEmptyNonWordBoundary;

    std::vector<int> reached;
    std::unordered_set<int> seen;
    ExpandForFlags(s.ids, flag, &reached, &seen);
    bool ok = false;
    for (int id : reached) {
      if (prog_->inst(id)->opcode() == kInstMatch) {
        ok = true;
        break;
      }
    }
    accept_.emplace(sid, ok);
    return ok;
  }

 private:
  struct TransitionKey {
    int sid;
    int c;
    bool operator==(const TransitionKey& other) const {
      return sid == other.sid && c == other.c;
    }
  };
  struct TransitionKeyHash {
    size_t operator()(const TransitionKey& k) const {
      return (static_cast<size_t>(k.sid) << 9) ^
             static_cast<size_t>(k.c + 1);
    }
  };

  static void Canonicalize(std::vector<int>* ids) {
    std::sort(ids->begin(), ids->end());
    ids->erase(std::unique(ids->begin(), ids->end()), ids->end());
  }

  int Intern(const NfaState& s) {
    auto it = index_.find(s);
    if (it != index_.end())
      return it->second;
    int id = static_cast<int>(states_.size());
    states_.push_back(s);
    index_.emplace(states_.back(), id);
    return id;
  }

  // Follows epsilon transitions from id under the active empty-width flags,
  // appending surviving ByteRange/EmptyWidth/Match instructions to out.
  // Flat instruction lists fall through from one instruction to the next
  // until a last() instruction, exactly as in DFA::AddToQueue.
  void Closure(int id, uint32_t flag,
               std::vector<int>* out, std::unordered_set<int>* seen) {
    std::vector<int> stk;
    stk.push_back(id);
    while (!stk.empty()) {
      int cur = stk.back();
      stk.pop_back();
      if (cur == 0 || !seen->insert(cur).second)
        continue;  // instruction 0 is the canonical Fail

      Prog::Inst* ip = prog_->inst(cur);
      switch (ip->opcode()) {
        case kInstNop:
        case kInstCapture:
          if (!ip->last())
            stk.push_back(cur + 1);
          stk.push_back(ip->out());
          break;

        case kInstEmptyWidth:
          // The instruction stays live in the resulting state so that it
          // can be re-evaluated at a later position with new context flags;
          // when the current flags already satisfy it, follow its edge too.
          out->push_back(cur);
          if (!ip->last())
            stk.push_back(cur + 1);
          if ((ip->empty() & ~flag) == 0)
            stk.push_back(ip->out());
          break;

        case kInstAltMatch:
          // As in DFA::AddToQueue: an AltMatch only hands control to the
          // next instruction in its list; its two edges are not epsilon
          // moves here.
          stk.push_back(cur + 1);
          break;

        case kInstByteRange:
        case kInstMatch:
          out->push_back(cur);
          if (!ip->last())
            stk.push_back(cur + 1);
          break;

        case kInstFail:
          break;

        default:
          // Flat programs contain no plain Alt instructions: they were fully
          // expanded while Prog::Flatten() ran (AltMatch is handled above).
          break;
      }
    }
  }

  // Feeds the live threads of a state through the empty-width checks active
  // at the current position.  Non-EmptyWidth threads pass through unchanged.
  void ExpandForFlags(const std::vector<int>& ids, uint32_t flag,
                      std::vector<int>* out,
                      std::unordered_set<int>* seen) {
    for (int id : ids) {
      Prog::Inst* ip = prog_->inst(id);
      if (ip->opcode() == kInstEmptyWidth) {
        if ((ip->empty() & ~flag) == 0)
          Closure(ip->out(), flag, out, seen);
      } else if (seen->insert(id).second) {
        out->push_back(id);
      }
    }
  }

  Prog* prog_;
  int start_id_;
  int dead_id_;
  std::vector<NfaState> states_;
  std::unordered_map<NfaState, int, NfaStateHash> index_;
  std::unordered_map<TransitionKey, int, TransitionKeyHash> trans_;
  std::unordered_map<int, bool> accept_;
};

// Record kept for every product state reached by the BFS.
struct ProductRecord {
  std::vector<int> states;  // one NFA state id per rule, left then right
  int left_rule;            // first matching left rule, -1 for default
  int right_rule;
  int parent;               // product state id of the parent, -1 at the root
  int byte;                 // byte on the edge from parent, -1 at the root
};

struct ProductKeyHash {
  size_t operator()(const std::vector<int>& v) const {
    size_t h = 0xcbf29ce484222325ULL;
    for (int x : v) {
      h ^= static_cast<size_t>(x) + 0x9e3779b97f4a7c15ULL + (h << 6) +
           (h >> 2);
    }
    return h;
  }
};

// First rule (local index within [begin,end)) whose automaton accepts the
// configuration in states; -1 when none accepts.
int FirstMatch(const std::vector<PatternAutomaton*>& autos,
               const std::vector<int>& states, int begin, int end) {
  for (int i = begin; i < end; i++) {
    int sid = states[i];
    if (!autos[i]->is_dead(sid) && autos[i]->Accepts(sid))
      return i - begin;
  }
  return -1;
}

// Parses and compiles every rule with Latin-1 encoding and otherwise default
// options.  On failure sets *error naming the side and rule index.
bool CompilePolicy(const char* side,
                   const std::vector<PolicyRule>& rules,
                   std::vector<CompiledRule>* compiled,
                   std::string* error) {
  RE2::Options options;
  options.set_encoding(RE2::Options::EncodingLatin1);
  options.set_log_errors(false);

  for (size_t i = 0; i < rules.size(); i++) {
    RegexpStatus status;
    Regexp* re = Regexp::Parse(
        rules[i].pattern,
        static_cast<Regexp::ParseFlags>(options.ParseFlags()),
        &status);
    if (re == NULL) {
      *error = std::string("invalid ") + side + " rule at index " +
               std::to_string(i) + " (pattern \"" + rules[i].pattern +
               "\"): " + status.Text();
      return false;
    }
    Prog* prog = re->CompileToProg(options.max_mem());
    re->Decref();
    if (prog == NULL) {
      *error = std::string("invalid ") + side + " rule at index " +
               std::to_string(i) + " (pattern \"" + rules[i].pattern +
               "\"): compilation failed";
      return false;
    }
    CompiledRule cr;
    cr.pattern = rules[i].pattern;
    cr.action = rules[i].action;
    cr.prog = prog;
    compiled->push_back(cr);
  }
  return true;
}

void DeletePrograms(std::vector<CompiledRule>* compiled) {
  for (size_t i = 0; i < compiled->size(); i++)
    delete (*compiled)[i].prog;
  compiled->clear();
}

}  // namespace

PolicyDiffResult ComparePolicies(const std::vector<PolicyRule>& left,
                                 int left_default,
                                 const std::vector<PolicyRule>& right,
                                 int right_default) {
  PolicyDiffResult result;

  std::vector<CompiledRule> cl, cr;
  std::string error;
  if (!CompilePolicy("left", left, &cl, &error) ||
      !CompilePolicy("right", right, &cr, &error)) {
    DeletePrograms(&cl);
    DeletePrograms(&cr);
    result.status = Invalid;
    result.error = error;
    return result;
  }

  const size_t nl = cl.size();
  const size_t nr = cr.size();
  const size_t total = nl + nr;

  // Automatons 0..nl-1 cover the left rules, nl..total-1 the right ones.
  std::vector<PatternAutomaton*> autos(total, NULL);
  for (size_t i = 0; i < nl; i++)
    autos[i] = new PatternAutomaton(cl[i].prog);
  for (size_t i = 0; i < nr; i++)
    autos[nl + i] = new PatternAutomaton(cr[i].prog);

  // BFS over the product automaton.  Edges leave each state in ascending
  // byte order, so the first differing state reached gives a shortest
  // witness and, among shortest ones, the unsigned-lexicographically
  // smallest witness.
  std::unordered_map<std::vector<int>, int, ProductKeyHash> seen;
  std::vector<ProductRecord> records;
  std::deque<int> queue;

  ProductRecord root;
  root.states.resize(total);
  for (size_t i = 0; i < total; i++)
    root.states[i] = autos[i]->start();
  root.left_rule = FirstMatch(autos, root.states, 0, nl);
  root.right_rule = FirstMatch(autos, root.states, nl, total);
  root.parent = -1;
  root.byte = -1;
  seen.emplace(root.states, 0);
  records.push_back(root);
  queue.push_back(0);

  int diff_state = -1;

  // A difference on the empty input is a zero-length witness.
  int root_left_action =
      root.left_rule < 0 ? left_default : cl[root.left_rule].action;
  int root_right_action =
      root.right_rule < 0 ? right_default : cr[root.right_rule].action;
  if (root_left_action != root_right_action)
    diff_state = 0;

  while (!queue.empty() && diff_state < 0) {
    int cur_id = queue.front();
    queue.pop_front();
    // Copy the state vector: appending to records below can reallocate and
    // invalidate references into it.
    const std::vector<int> cur_states = records[cur_id].states;

    for (int c = 0; c < 256 && diff_state < 0; c++) {
      std::vector<int> next(total);
      for (size_t i = 0; i < total; i++)
        next[i] = autos[i]->Go(cur_states[i], c);
      if (seen.find(next) != seen.end())
        continue;

      ProductRecord rec;
      rec.states = next;
      rec.left_rule = FirstMatch(autos, next, 0, nl);
      rec.right_rule = FirstMatch(autos, next, nl, total);
      rec.parent = cur_id;
      rec.byte = c;

      int la = rec.left_rule < 0 ? left_default
                                 : cl[rec.left_rule].action;
      int ra = rec.right_rule < 0 ? right_default
                                  : cr[rec.right_rule].action;

      int next_id = static_cast<int>(records.size());
      seen.emplace(rec.states, next_id);
      records.push_back(rec);
      queue.push_back(next_id);
      if (la != ra)
        diff_state = next_id;
    }
  }

  if (diff_state >= 0) {
    const ProductRecord& hit = records[diff_state];

    std::string witness;
    for (int sid = diff_state; records[sid].parent != -1;
         sid = records[sid].parent)
      witness.push_back(static_cast<char>(records[sid].byte));
    std::reverse(witness.begin(), witness.end());

    result.status = Different;
    result.witness = witness;
    result.left_rule = hit.left_rule;
    result.right_rule = hit.right_rule;
    result.left_action =
        hit.left_rule < 0 ? left_default : cl[hit.left_rule].action;
    result.right_action =
        hit.right_rule < 0 ? right_default : cr[hit.right_rule].action;
  } else {
    result.status = Equivalent;
  }

  for (size_t i = 0; i < autos.size(); i++)
    delete autos[i];
  DeletePrograms(&cl);
  DeletePrograms(&cr);
  return result;
}

}  // namespace re2
