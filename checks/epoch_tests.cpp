//===- epoch_tests.cpp - unit tests for the epoch schedule ---------------===//
//
// Registered through task_support/check.h.  The three groups "rounds",
// "lineage" and "resume" exercise the deterministic scheduler directly.
// Synthetic diagnostics are handed to the state interface only: no crashing
// target is launched and no trigger inputs are constructed.  The only
// subprocess used here is the always-succeeding benign fixture, which runs
// the real epoch fork mode.
//
//===----------------------------------------------------------------------===//

#include "lib/fuzzer/FuzzerEpoch.h"

// check.h declares the registration arrays with C++ linkage and no extern "C"
// block, while task_support/driver.c is compiled as C and looks up the plain C
// symbols.  Rename the header's external declarations so the definitions below
// can carry C linkage without a language-linkage conflict.
#define task_cases task_cases_cxx_decl
#define task_case_count task_case_count_cxx_decl
#include "task_support/check.h"
#undef task_cases
#undef task_case_count

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using fuzzer::EpochDiagnostic;
using fuzzer::EpochLineage;
using fuzzer::EpochParams;
using fuzzer::EpochSchedule;
using fuzzer::EpochStore;

static std::string Sha(char C) { return std::string(40, C); }

static const std::string A = Sha('a');
static const std::string B = Sha('b');
static const std::string C = Sha('c');
static const std::string D = Sha('d');
static const std::string E = Sha('e');

static EpochParams MakeParams(size_t Slots = 2, size_t Runs = 16,
                              size_t Rounds = 4) {
  EpochParams P;
  P.Slots = Slots;
  P.RunsPerSlot = Runs;
  P.Rounds = Rounds;
  return P;
}

static int AddRoots(EpochSchedule *S, const std::vector<std::string> &Roots) {
  for (const auto &R : Roots)
    TASK_CHECK(S->AddRoot(R));
  return 0;
}

static int MakeTempDir(std::string *Out) {
  const char *Base = getenv("TMPDIR");
  std::string Tmpl =
      std::string(Base && *Base ? Base : "/tmp") + "/epoch-test-XXXXXX";
  std::vector<char> Buf(Tmpl.begin(), Tmpl.end());
  Buf.push_back('\0');
  char *Dir = mkdtemp(Buf.data());
  TASK_CHECK(Dir != nullptr);
  *Out = std::string(Dir);
  return 0;
}

static const EpochLineage *FindLineage(const EpochSchedule &S,
                                       const std::string &Sha) {
  for (const auto &L : S.Lineage())
    if (L.Sha == Sha)
      return &L;
  return nullptr;
}

static const EpochDiagnostic *FindDiag(const EpochSchedule &S,
                                       const std::string &Key) {
  for (const auto &D : S.Diagnostics())
    if (D.Key == Key)
      return &D;
  return nullptr;
}

//===----------------------------------------------------------------------===//
// rounds group
//===----------------------------------------------------------------------===//

static int test_root_and_default_state() {
  EpochSchedule S;
  TASK_CHECK(S.Round() == 0);
  TASK_CHECK(S.AddRoot(A));
  TASK_CHECK(S.AddRoot(B));
  TASK_CHECK(!S.AddRoot(A));                       // Duplicate root.
  TASK_CHECK(!S.AddRoot("not-a-sha1"));            // Bad shape.
  TASK_CHECK(!S.AddRoot(std::string(40, 'A')));    // Uppercase rejected.
  TASK_CHECK(S.RootShas().size() == 2);
  TASK_CHECK(S.RootShas()[0] == A);                // Sorted.
  TASK_CHECK(S.BeginRound(0).empty());             // Zero slots rejected.
  return 0;
}

static int test_assignment_priority_and_cycling() {
  EpochSchedule S;
  TASK_CHECK(AddRoots(&S, {C, A, B}) == 0);
  // Every root is unassigned: SHA1 order, cycled to fill all slots.
  std::vector<std::string> R0 = S.BeginRound(5);
  TASK_CHECK((R0 == std::vector<std::string>{A, B, C, A, B}));
  TASK_CHECK(S.Complete(0, {}, ""));
  TASK_CHECK(S.Complete(1, {D}, "")); // D discovered while B was the parent.
  TASK_CHECK(S.Complete(2, {}, ""));
  TASK_CHECK(S.Complete(3, {}, ""));
  TASK_CHECK(S.Complete(4, {}, ""));
  TASK_CHECK(S.CommitRound());
  TASK_CHECK(S.Round() == 1);

  // D has never been assigned, so it must lead the next round; the roots all
  // share the same age and stay in SHA1 order behind it.
  std::vector<std::string> R1 = S.BeginRound(4);
  TASK_CHECK((R1 == std::vector<std::string>{D, A, B, C}));
  for (size_t I = 0; I < 4; I++)
    TASK_CHECK(S.Complete(I, {}, ""));
  TASK_CHECK(S.CommitRound());

  // Everything was assigned last round, so ages are equal again.
  std::vector<std::string> R2 = S.BeginRound(3);
  TASK_CHECK((R2 == std::vector<std::string>{A, B, C}));
  return 0;
}

static int test_completion_order_does_not_matter() {
  EpochSchedule L; TASK_CHECK(AddRoots(&L, {A, B, C}) == 0);
  EpochSchedule R; TASK_CHECK(AddRoots(&R, {A, B, C}) == 0);
  const size_t Slots = 3;
  for (size_t RoundNo = 0; RoundNo < 3; RoundNo++) {
    std::vector<std::string> AL = L.BeginRound(Slots);
    std::vector<std::string> AR = R.BeginRound(Slots);
    TASK_CHECK(AL == AR); // Identical assignment before anything runs.

    // Same per-slot outcomes, but L reports 0,1,2 while R reports 2,1,0.
    auto Outcome = [&](size_t Slot) -> std::vector<std::string> {
      if (RoundNo == 0 && Slot == 0)
        return {D};
      if (RoundNo == 1 && Slot == 2)
        return {E};
      return {};
    };
    for (size_t Slot = 0; Slot < Slots; Slot++)
      TASK_CHECK(L.Complete(Slot, Outcome(Slot), ""));
    for (size_t Slot = Slots; Slot > 0; Slot--)
      TASK_CHECK(R.Complete(Slot - 1, Outcome(Slot - 1), ""));

    TASK_CHECK(L.CommitRound());
    TASK_CHECK(R.CommitRound());
    TASK_CHECK(L.Round() == R.Round());
    TASK_CHECK(L.LiveShas() == R.LiveShas());
    TASK_CHECK(L.Lineage().size() == R.Lineage().size());
  }
  // The next-round assignment must be identical regardless of finish order.
  TASK_CHECK(L.BeginRound(Slots) == R.BeginRound(Slots));
  return 0;
}

static int test_duplicate_and_partial_reports() {
  EpochSchedule S; TASK_CHECK(AddRoots(&S, {A, B}) == 0);
  TASK_CHECK(S.BeginRound(2).size() == 2);
  TASK_CHECK(S.Complete(0, {}, ""));
  TASK_CHECK(!S.Complete(0, {}, "")); // Duplicate completion.
  TASK_CHECK(!S.CommitRound());       // Not all slots are in.
  TASK_CHECK(S.Round() == 0);         // Committed state is untouched.
  TASK_CHECK(S.Fail(1));
  TASK_CHECK(!S.Fail(1));             // Duplicate failure report.
  TASK_CHECK(S.CommitRound());
  TASK_CHECK(S.Round() == 1);

  // A finished round cannot be committed or reported into twice.
  TASK_CHECK(!S.CommitRound());
  TASK_CHECK(!S.Complete(0, {}, ""));
  TASK_CHECK(!S.Fail(0));
  return 0;
}

static int test_children_wait_until_next_round() {
  EpochSchedule S; TASK_CHECK(AddRoots(&S, {A, B}) == 0);
  std::vector<std::string> Assigned = S.BeginRound(2);
  TASK_CHECK(Assigned[0] == A);
  TASK_CHECK(S.Complete(0, {D}, ""));
  TASK_CHECK(S.Complete(1, {}, ""));
  // The round is still open: another round may not start and the child cannot
  // influence the assignment that was frozen at BeginRound.
  TASK_CHECK(S.BeginRound(2).empty());
  TASK_CHECK(S.CommitRound());
  const EpochLineage *L = FindLineage(S, D);
  TASK_CHECK(L != nullptr);
  TASK_CHECK(L->BornRound == 0);
  TASK_CHECK(L->BornSlot == 0);
  TASK_CHECK(L->Parent == A);
  // The child only becomes assignable in the following round.
  std::vector<std::string> Next = S.BeginRound(2);
  TASK_CHECK(Next[0] == D); // Never assigned, so it goes first.
  return 0;
}

//===----------------------------------------------------------------------===//
// lineage group
//===----------------------------------------------------------------------===//

static int test_first_discovery_records_parent() {
  EpochSchedule S; TASK_CHECK(AddRoots(&S, {A, B, C}) == 0);
  TASK_CHECK(S.BeginRound(3).size() == 3);
  TASK_CHECK(S.Complete(0, {}, ""));
  TASK_CHECK(S.Complete(1, {}, ""));
  TASK_CHECK(S.Complete(2, {D}, "")); // Parent of slot 2 is C.
  TASK_CHECK(S.CommitRound());
  const EpochLineage *L = FindLineage(S, D);
  TASK_CHECK(L != nullptr);
  TASK_CHECK(L->Parent == C);
  TASK_CHECK(L->BornRound == 0);
  TASK_CHECK(L->BornSlot == 2);
  return 0;
}

static int test_repeated_discovery_keeps_earliest() {
  EpochSchedule S; TASK_CHECK(AddRoots(&S, {A, B}) == 0);
  TASK_CHECK(S.BeginRound(2).size() == 2);
  TASK_CHECK(S.Complete(0, {D}, "")); // Round 0, slot 0, parent A.
  TASK_CHECK(S.Complete(1, {}, ""));
  TASK_CHECK(S.CommitRound());

  TASK_CHECK(S.BeginRound(2).size() == 2);
  TASK_CHECK(S.Complete(0, {}, ""));
  TASK_CHECK(S.Complete(1, {D}, "")); // Same bytes rediscovered under B.
  TASK_CHECK(S.CommitRound());

  const EpochLineage *L = FindLineage(S, D);
  TASK_CHECK(L != nullptr);
  TASK_CHECK(L->Parent == A);
  TASK_CHECK(L->BornRound == 0);
  TASK_CHECK(L->BornSlot == 0);
  TASK_CHECK(S.LiveShas().size() == 3); // No duplicate entry.
  return 0;
}

static int test_failures_contribute_nothing() {
  EpochSchedule S; TASK_CHECK(AddRoots(&S, {A, B, C}) == 0);
  TASK_CHECK(S.BeginRound(3).size() == 3);
  TASK_CHECK(S.Complete(0, {}, ""));
  // Classified failure: even children handed in with it must be discarded.
  TASK_CHECK(S.Complete(1, {D}, "ERROR: synthetic classified failure"));
  // Unclassified failure: completed, but no coverage, files or diagnostic.
  TASK_CHECK(S.Fail(2));
  TASK_CHECK(S.CommitRound());

  TASK_CHECK(FindLineage(S, D) == nullptr);
  TASK_CHECK(S.LiveShas().size() == 3);
  const EpochDiagnostic *Diag =
      FindDiag(S, "ERROR: synthetic classified failure");
  TASK_CHECK(Diag != nullptr);
  TASK_CHECK(Diag->Round == 0 && Diag->Slot == 1);
  TASK_CHECK(S.Diagnostics().size() == 1); // The Fail() slot adds no key.
  return 0;
}

static int test_diagnostic_representative_is_earliest() {
  const std::string Key = "runtime error: synthetic key";
  EpochSchedule S; TASK_CHECK(AddRoots(&S, {A, B}) == 0);
  // Round 0: key first appears at slot 1.
  TASK_CHECK(S.BeginRound(2).size() == 2);
  TASK_CHECK(S.Complete(0, {}, ""));
  TASK_CHECK(S.Complete(1, {}, Key));
  TASK_CHECK(S.CommitRound());
  // Round 1: the same key shows up at slot 0; the representative must stay.
  TASK_CHECK(S.BeginRound(2).size() == 2);
  TASK_CHECK(S.Complete(0, {}, Key));
  TASK_CHECK(S.Complete(1, {}, ""));
  TASK_CHECK(S.CommitRound());

  const EpochDiagnostic *Diag = FindDiag(S, Key);
  TASK_CHECK(Diag != nullptr);
  TASK_CHECK(Diag->Round == 0 && Diag->Slot == 1);
  TASK_CHECK(S.Diagnostics().size() == 1);
  return 0;
}

static int test_lineage_survives_elimination() {
  std::string Dir; TASK_CHECK(MakeTempDir(&Dir) == 0);
  EpochParams P = MakeParams(2, 8, 3);
  EpochSchedule S; TASK_CHECK(AddRoots(&S, {A, B}) == 0);
  TASK_CHECK(S.BeginRound(2).size() == 2);
  TASK_CHECK(S.Complete(0, {D}, ""));
  TASK_CHECK(S.Complete(1, {}, ""));
  TASK_CHECK(S.CommitRound());
  TASK_CHECK(FindLineage(S, D) != nullptr);

  std::string StatePath = Dir + "/state";
  EpochStore Store(StatePath, {A, B, D}, P);
  std::string Err;
  TASK_CHECK(Store.Save(S, &Err));

  // Simulate D being shrunk away: it is gone from the on-disk corpus while
  // the roots remain.  The load must succeed and keep D's old lineage.
  EpochSchedule Reloaded;
  EpochStore SmallerStore(StatePath, {A, B}, P);
  TASK_CHECK(SmallerStore.Load(&Reloaded, &Err));
  TASK_CHECK(Reloaded.Round() == 1);
  std::vector<std::string> Live = Reloaded.LiveShas();
  TASK_CHECK(std::find(Live.begin(), Live.end(), D) == Live.end());
  const EpochLineage *L = FindLineage(Reloaded, D);
  TASK_CHECK(L != nullptr);
  TASK_CHECK(L->Parent == A && L->BornRound == 0 && L->BornSlot == 0);
  return 0;
}

//===----------------------------------------------------------------------===//
// resume group
//===----------------------------------------------------------------------===//

static int test_save_load_roundtrip_and_continue() {
  std::string Dir; TASK_CHECK(MakeTempDir(&Dir) == 0);
  EpochParams P = MakeParams(2, 8, 4);
  std::string StatePath = Dir + "/state";

  EpochSchedule S; TASK_CHECK(AddRoots(&S, {A, B}) == 0);
  EpochStore Store(StatePath, {A, B}, P);
  TASK_CHECK(!Store.Exists());
  TASK_CHECK(Store.Save(S));
  TASK_CHECK(Store.Exists());

  TASK_CHECK(S.BeginRound(2).size() == 2);
  TASK_CHECK(S.Complete(0, {D}, ""));
  TASK_CHECK(S.Complete(1, {}, "ERROR: synthetic roundtrip failure"));
  TASK_CHECK(S.CommitRound());
  std::vector<std::string> Disk = S.LiveShas();
  EpochStore Store1(StatePath, Disk, P);
  TASK_CHECK(Store1.Save(S));

  EpochSchedule Loaded;
  TASK_CHECK(Store1.Load(&Loaded));
  TASK_CHECK(Loaded.Round() == 1);
  TASK_CHECK(Loaded.LiveShas() == S.LiveShas());
  TASK_CHECK(Loaded.Lineage().size() == S.Lineage().size());
  TASK_CHECK(Loaded.Diagnostics().size() == 1);

  // Normal continuation: the resumed object keeps scheduling round 2.
  TASK_CHECK(Loaded.BeginRound(2).size() == 2);
  TASK_CHECK(Loaded.Complete(0, {}, ""));
  TASK_CHECK(Loaded.Complete(1, {}, ""));
  TASK_CHECK(Loaded.CommitRound());
  TASK_CHECK(Loaded.Round() == 2);
  EpochStore Store2(StatePath, Loaded.LiveShas(), P);
  TASK_CHECK(Store2.Save(Loaded));

  EpochSchedule Again;
  TASK_CHECK(Store2.Load(&Again));
  TASK_CHECK(Again.Round() == 2);
  TASK_CHECK(Again.Diagnostics().size() == 1);
  return 0;
}

static int test_uncommitted_round_is_not_persisted() {
  std::string Dir; TASK_CHECK(MakeTempDir(&Dir) == 0);
  EpochParams P = MakeParams(2, 8, 2);
  std::string StatePath = Dir + "/state";
  EpochSchedule S; TASK_CHECK(AddRoots(&S, {A, B}) == 0);
  EpochStore Store(StatePath, {A, B}, P);
  TASK_CHECK(Store.Save(S));

  TASK_CHECK(S.BeginRound(2).size() == 2);
  TASK_CHECK(S.Complete(0, {D}, ""));
  TASK_CHECK(!S.CommitRound()); // Slot 1 never reported.
  TASK_CHECK(S.Round() == 0);

  // The state on disk still describes round 0 and knows nothing about D.
  EpochSchedule Reloaded;
  TASK_CHECK(Store.Load(&Reloaded));
  TASK_CHECK(Reloaded.Round() == 0);
  TASK_CHECK(FindLineage(Reloaded, D) == nullptr);

  // The unfinished round is redone from scratch in the new process.
  std::vector<std::string> Assigned = Reloaded.BeginRound(2);
  TASK_CHECK(Assigned.size() == 2);
  return 0;
}

static int test_corrupt_and_truncated_state_rejected() {
  std::string Dir; TASK_CHECK(MakeTempDir(&Dir) == 0);
  EpochParams P = MakeParams(2, 8, 2);
  std::string StatePath = Dir + "/state";
  EpochSchedule S; TASK_CHECK(AddRoots(&S, {A, B}) == 0);
  EpochStore Store(StatePath, {A, B}, P);
  TASK_CHECK(Store.Save(S));

  std::ifstream GoodFile(StatePath, std::ios::binary);
  const std::string Good((std::istreambuf_iterator<char>(GoodFile)),
                         std::istreambuf_iterator<char>());
  TASK_CHECK(!Good.empty());

  std::string Cases[] = {"garbage\n", Good.substr(0, Good.size() / 2),
                         Good + "PARAM 1 1 1 1\n"};
  for (const auto &Bad : Cases) {
    std::ofstream OS(StatePath, std::ios::binary | std::ios::trunc);
    OS << Bad;
    OS.close();
    EpochSchedule Reloaded;
    std::string Err;
    TASK_CHECK(!Store.Load(&Reloaded, &Err));
    TASK_CHECK(!Err.empty());
  }

  // A readable but self-inconsistent file is rejected as well.
  std::string Tampered = Good;
  std::string::size_type Pos = Tampered.find("ROOT ");
  TASK_CHECK(Pos != std::string::npos);
  Tampered.replace(Pos, 4, "BOOM");
  std::ofstream OS(StatePath, std::ios::binary | std::ios::trunc);
  OS << Tampered;
  OS.close();
  EpochSchedule Reloaded;
  TASK_CHECK(!Store.Load(&Reloaded));
  return 0;
}

static int test_identity_mismatches_rejected() {
  std::string Dir; TASK_CHECK(MakeTempDir(&Dir) == 0);
  EpochParams P = MakeParams(2, 8, 3);
  std::string StatePath = Dir + "/state";
  EpochSchedule S; TASK_CHECK(AddRoots(&S, {A, B}) == 0);
  EpochStore Store(StatePath, {A, B}, P);
  TASK_CHECK(Store.Save(S));

  EpochSchedule Reloaded;
  // Different slot count, run budget and round count each fail on their own.
  TASK_CHECK(!EpochStore(StatePath, {A, B}, MakeParams(3, 8, 3)).Load(&Reloaded));
  TASK_CHECK(!EpochStore(StatePath, {A, B}, MakeParams(2, 9, 3)).Load(&Reloaded));
  TASK_CHECK(!EpochStore(StatePath, {A, B}, MakeParams(2, 8, 4)).Load(&Reloaded));
  // A missing root file breaks the initial-corpus identity.
  TASK_CHECK(!EpochStore(StatePath, {A}, P).Load(&Reloaded));
  // An unrelated extra on-disk file does not invalidate the identity.
  TASK_CHECK(EpochStore(StatePath, {A, B, D}, P).Load(&Reloaded));
  return 0;
}

static int test_benign_fixture_runs_epoch_fork_mode() {
  TASK_CHECK(access(".task-build/benign", X_OK) == 0);
  std::string Dir1; TASK_CHECK(MakeTempDir(&Dir1) == 0);
  std::string Dir2; TASK_CHECK(MakeTempDir(&Dir2) == 0);
  std::string Corpus1 = Dir1 + "/corpus";
  std::string Corpus2 = Dir2 + "/corpus";
  mkdir(Corpus1.c_str(), 0700);
  mkdir(Corpus2.c_str(), 0700);
  std::ofstream(Corpus1 + "/a") << "A";
  std::ofstream(Corpus1 + "/b") << "B";
  std::ofstream(Corpus2 + "/a") << "A";
  std::ofstream(Corpus2 + "/b") << "B";
  const std::string State1 = Dir1 + "/state";
  const std::string State2 = Dir2 + "/state";

  auto Run = [&](const std::string &State, const std::string &Corpus) -> int {
    std::string Cmd = ".task-build/benign -fork=2 -schedule_rounds=2 "
                      "-schedule_runs=12 -seed=1 -max_len=4 -reload=0 "
                      "-schedule_state='" +
                      State + "' '" + Corpus + "' >/dev/null 2>&1";
    int St = std::system(Cmd.c_str());
    TASK_CHECK(WIFEXITED(St) && WEXITSTATUS(St) == 0);
    return 0;
  };
  TASK_CHECK(Run(State1, Corpus1) == 0);
  TASK_CHECK(Run(State2, Corpus2) == 0);

  // Same inputs, rounds and budgets must yield byte-identical state, with no
  // temporary paths embedded in the identity.
  std::ifstream F1(State1, std::ios::binary), F2(State2, std::ios::binary);
  std::string S1((std::istreambuf_iterator<char>(F1)),
                 std::istreambuf_iterator<char>());
  std::string S2((std::istreambuf_iterator<char>(F2)),
                 std::istreambuf_iterator<char>());
  TASK_CHECK(!S1.empty() && S1 == S2);
  TASK_CHECK(S1.find("LIBFUZZER_EPOCH_STATE_V1") == 0);
  TASK_CHECK(S1.find("PARAM 2 12 2 2\n") != std::string::npos);

  // Re-running against the same state resumes at the committed round count.
  TASK_CHECK(Run(State1, Corpus1) == 0);
  std::ifstream F3(State1, std::ios::binary);
  std::string S3((std::istreambuf_iterator<char>(F3)),
                 std::istreambuf_iterator<char>());
  TASK_CHECK(S3 == S1);
  return 0;
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

extern "C" const struct task_case task_cases[] = {
    // rounds
    {"rounds", "root_and_default_state", test_root_and_default_state},
    {"rounds", "assignment_priority_and_cycling",
     test_assignment_priority_and_cycling},
    {"rounds", "completion_order_does_not_matter",
     test_completion_order_does_not_matter},
    {"rounds", "duplicate_and_partial_reports",
     test_duplicate_and_partial_reports},
    {"rounds", "children_wait_until_next_round",
     test_children_wait_until_next_round},
    // lineage
    {"lineage", "first_discovery_records_parent",
     test_first_discovery_records_parent},
    {"lineage", "repeated_discovery_keeps_earliest",
     test_repeated_discovery_keeps_earliest},
    {"lineage", "failures_contribute_nothing",
     test_failures_contribute_nothing},
    {"lineage", "diagnostic_representative_is_earliest",
     test_diagnostic_representative_is_earliest},
    {"lineage", "lineage_survives_elimination",
     test_lineage_survives_elimination},
    // resume
    {"resume", "save_load_roundtrip_and_continue",
     test_save_load_roundtrip_and_continue},
    {"resume", "uncommitted_round_is_not_persisted",
     test_uncommitted_round_is_not_persisted},
    {"resume", "corrupt_and_truncated_state_rejected",
     test_corrupt_and_truncated_state_rejected},
    {"resume", "identity_mismatches_rejected",
     test_identity_mismatches_rejected},
    {"resume", "benign_fixture_runs_epoch_fork_mode",
     test_benign_fixture_runs_epoch_fork_mode},
};

extern "C" const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
