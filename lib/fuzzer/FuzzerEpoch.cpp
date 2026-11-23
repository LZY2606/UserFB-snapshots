//===- FuzzerEpoch.cpp - deterministic round schedule ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "FuzzerEpoch.h"
#include "FuzzerIO.h"

#include <algorithm>
#include <fstream>
#include <numeric>
#include <sstream>

namespace fuzzer {

static const char kStateMagic[] = "LIBFUZZER_EPOCH_STATE_V1";

bool IsCanonicalSha1(const std::string &Sha) {
  if (Sha.size() != 40)
    return false;
  for (char C : Sha)
    if (!((C >= '0' && C <= '9') || (C >= 'a' && C <= 'f')))
      return false;
  return true;
}

// Age of an entry: complete rounds since its last assignment.  Inputs that
// have never been assigned are treated as infinitely old so they go first.
static long EntryAge(size_t CurrentRound, size_t LastRound) {
  return LastRound == (size_t)-1 ? (long)CurrentRound + 1
                                 : (long)CurrentRound - (long)LastRound;
}

static bool ContainsSha(const std::vector<std::string> &Sorted,
                        const std::string &Sha) {
  return std::binary_search(Sorted.begin(), Sorted.end(), Sha);
}

EpochSchedule::Entry *EpochSchedule::Find(const std::string &Sha) {
  auto It = std::lower_bound(
      Entries.begin(), Entries.end(), Sha,
      [](const Entry &E, const std::string &S) { return E.Sha < S; });
  return It == Entries.end() || It->Sha != Sha ? nullptr : &*It;
}

const EpochSchedule::Entry *
EpochSchedule::Find(const std::string &Sha) const {
  return const_cast<EpochSchedule *>(this)->Find(Sha);
}

void EpochSchedule::ResetPending() {
  PendingAssign.clear();
  PendingDone.clear();
  PendingFailed.clear();
  PendingDiag.clear();
  PendingChildren.clear();
}

bool EpochSchedule::AddRoot(const std::string &Sha) {
  if (InRound || CurrentRound != 0 || !IsCanonicalSha1(Sha))
    return false;
  if (Find(Sha))
    return false;
  Entry E;
  E.Sha = Sha;
  E.IsRoot = true;
  Entries.insert(std::upper_bound(Entries.begin(), Entries.end(), Sha,
                                  [](const std::string &S, const Entry &En) {
                                    return S < En.Sha;
                                  }),
                 E);
  EpochLineage L;
  L.Sha = Sha;
  History.insert(std::upper_bound(History.begin(), History.end(), Sha,
                                  [](const std::string &S,
                                     const EpochLineage &Li) {
                                    return S < Li.Sha;
                                  }),
                 L);
  return true;
}

std::vector<std::string> EpochSchedule::BeginRound(size_t Slots) {
  if (InRound || Slots == 0 || Entries.empty())
    return {};
  std::vector<size_t> Order(Entries.size());
  std::iota(Order.begin(), Order.end(), 0);
  std::stable_sort(Order.begin(), Order.end(), [&](size_t X, size_t Y) {
    long Ax = EntryAge(CurrentRound, Entries[X].LastRound);
    long Ay = EntryAge(CurrentRound, Entries[Y].LastRound);
    if (Ax != Ay)
      return Ax > Ay;
    return Entries[X].Sha < Entries[Y].Sha;
  });
  SlotCount = Slots;
  PendingAssign.resize(Slots);
  for (size_t S = 0; S < Slots; S++)
    PendingAssign[S] = Entries[Order[S % Entries.size()]].Sha;
  PendingDone.assign(Slots, false);
  PendingFailed.assign(Slots, false);
  PendingDiag.assign(Slots, "");
  PendingChildren.assign(Slots, {});
  InRound = true;
  return PendingAssign;
}

bool EpochSchedule::RecordReport(size_t Slot, bool Failed,
                                 const std::vector<std::string> &Children,
                                 const std::string &Diagnostic) {
  if (!InRound || Slot >= SlotCount || PendingDone[Slot])
    return false;
  PendingDone[Slot] = true;
  PendingFailed[Slot] = Failed;
  PendingDiag[Slot] = Diagnostic;
  PendingChildren[Slot] = Children;
  return true;
}

bool EpochSchedule::Complete(size_t Slot,
                             const std::vector<std::string> &Children,
                             const std::string &Diagnostic) {
  if (!InRound || Slot >= SlotCount || PendingDone[Slot])
    return false;
  for (const auto &Sha : Children)
    if (!IsCanonicalSha1(Sha))
      return false;
  return RecordReport(Slot, !Diagnostic.empty(), Children, Diagnostic);
}

bool EpochSchedule::Fail(size_t Slot) {
  if (!InRound || Slot >= SlotCount || PendingDone[Slot])
    return false;
  // A failure without a classification key is still a completed slot, but it
  // contributes neither coverage nor new files.
  return RecordReport(Slot, /*Failed=*/true, {}, "");
}

bool EpochSchedule::CommitRound() {
  if (!InRound)
    return false;
  for (bool Done : PendingDone)
    if (!Done)
      return false; // Partial rounds never touch committed state.

  for (size_t Slot = 0; Slot < SlotCount; Slot++) {
    const std::string ParentSha = PendingAssign[Slot];
    Entry *Parent = Find(ParentSha);
    if (!Parent)
      return false;
    Parent->LastRound = CurrentRound;
    Parent->AssignedCount++;

    if (PendingFailed[Slot]) {
      if (!PendingDiag[Slot].empty()) {
        const std::string &Key = PendingDiag[Slot];
        auto It = std::lower_bound(
            DiagStore.begin(), DiagStore.end(), Key,
            [](const EpochDiagnostic &D, const std::string &K) {
              return D.Key < K;
            });
        if (It == DiagStore.end() || It->Key != Key) {
          // The representative is the earliest round/slot, which is simply
          // the first one committed for this key -- never the fastest worker.
          EpochDiagnostic D;
          D.Key = Key;
          D.Round = CurrentRound;
          D.Slot = Slot;
          DiagStore.insert(It, D);
        }
      }
      continue; // Failed slots contribute no coverage and no new files.
    }

    for (const auto &ChildSha : PendingChildren[Slot]) {
      auto HistIt = std::lower_bound(
          History.begin(), History.end(), ChildSha,
          [](const EpochLineage &L, const std::string &S) {
            return L.Sha < S;
          });
      if (HistIt == History.end() || HistIt->Sha != ChildSha) {
        // First discovery of these bytes: remember the actual parent seed and
        // the round/slot where it appeared.
        EpochLineage L;
        L.Sha = ChildSha;
        L.Parent = ParentSha;
        L.BornRound = CurrentRound;
        L.BornSlot = Slot;
        History.insert(HistIt, L);

        Entry E;
        E.Sha = ChildSha;
        E.Parent = ParentSha;
        E.BornRound = CurrentRound;
        E.BornSlot = Slot;
        Entries.insert(std::upper_bound(Entries.begin(), Entries.end(),
                                        ChildSha,
                                        [](const std::string &S,
                                           const Entry &En) {
                                          return S < En.Sha;
                                        }),
                       E);
      }
      // Repeated discovery of identical bytes keeps the earliest lineage.
    }
  }

  CurrentRound++;
  InRound = false;
  ResetPending();
  return true;
}

std::vector<std::string> EpochSchedule::RootShas() const {
  std::vector<std::string> Res;
  for (const auto &E : Entries)
    if (E.IsRoot)
      Res.push_back(E.Sha);
  return Res;
}

std::vector<std::string> EpochSchedule::LiveShas() const {
  std::vector<std::string> Res;
  for (const auto &E : Entries)
    Res.push_back(E.Sha);
  return Res;
}

const std::vector<EpochLineage> &EpochSchedule::Lineage() const {
  // History is retained for every file ever discovered, even if the live
  // corpus later shrinks or eliminates it.
  return History;
}

const std::vector<EpochDiagnostic> &EpochSchedule::Diagnostics() const {
  return DiagStore;
}

namespace {

bool ParseSize(const std::string &S, size_t *Out) {
  if (S.empty())
    return false;
  size_t V = 0;
  for (char C : S) {
    if (C < '0' || C > '9')
      return false;
    V = V * 10 + (size_t)(C - '0');
  }
  *Out = V;
  return true;
}

std::string NextToken(std::istringstream &ISS) {
  std::string Tok;
  ISS >> Tok;
  return Tok;
}

struct HistRec {
  std::string Sha, Parent;
  size_t BornRound = 0, BornSlot = 0;
};

} // namespace

bool EpochSchedule::ExportText(std::ostream &OS,
                               const EpochParams &Params) const {
  if (InRound || Params.Slots == 0 || Params.RunsPerSlot == 0 ||
      Params.Rounds == 0)
    return false;
  OS << kStateMagic << '\n';
  OS << "PARAM " << Params.Slots << ' ' << Params.RunsPerSlot << ' '
     << Params.Rounds << ' ' << CurrentRound << '\n';
  for (const auto &E : Entries)
    if (E.IsRoot)
      OS << "ROOT " << E.Sha << '\n';
  for (const auto &L : History)
    if (!L.Parent.empty()) // Roots are covered by ROOT lines.
      OS << "HIST " << L.Sha << ' ' << L.Parent << ' ' << L.BornRound << ' '
         << L.BornSlot << '\n';
  for (const auto &E : Entries)
    OS << "NODE " << E.Sha << ' ' << (E.IsRoot ? 1 : 0) << ' '
       << (E.Parent.empty() ? "-" : E.Parent) << ' ' << E.BornRound << ' '
       << E.BornSlot << ' '
       << (E.LastRound == (size_t)-1 ? std::string("-1")
                                     : std::to_string(E.LastRound))
       << ' ' << E.AssignedCount << '\n';
  for (const auto &D : DiagStore)
    OS << "DIAG " << D.Round << ' ' << D.Slot << ' ' << D.Key << '\n';
  OS << "END\n";
  return static_cast<bool>(OS);
}

bool EpochSchedule::ImportText(std::istream &IS,
                               const std::vector<std::string> &DiskShas,
                               const EpochParams &Params,
                               EpochSchedule *Out, std::string *Error) {
  auto Fail = [&](const std::string &Msg) -> bool {
    if (Error)
      *Error = Msg;
    return false;
  };
  if (!Out || Params.Slots == 0 || Params.RunsPerSlot == 0 ||
      Params.Rounds == 0)
    return Fail("bad import parameters");

  std::string Line;
  if (!std::getline(IS, Line) || Line != kStateMagic)
    return Fail("bad state magic");

  std::vector<std::string> Roots;
  std::vector<HistRec> Hists;
  std::vector<Entry> Nodes;
  std::vector<EpochDiagnostic> Diags;
  size_t Committed = (size_t)-1;
  bool SawParam = false, SawEnd = false;

  while (std::getline(IS, Line)) {
    if (Line == "END") {
      SawEnd = true;
      break;
    }
    std::istringstream ISS(Line);
    std::string Tag;
    ISS >> Tag;
    if (Tag == "PARAM") {
      if (SawParam)
        return Fail("duplicate PARAM");
      SawParam = true;
      size_t Slots, Runs, Rounds, RoundNum;
      std::string A = NextToken(ISS), B = NextToken(ISS),
                  C = NextToken(ISS), D = NextToken(ISS);
      if (!ParseSize(A, &Slots) || !ParseSize(B, &Runs) ||
          !ParseSize(C, &Rounds) || !ParseSize(D, &RoundNum))
        return Fail("bad PARAM line");
      if (Slots != Params.Slots || Runs != Params.RunsPerSlot ||
          Rounds != Params.Rounds)
        return Fail("schedule parameters do not match the state file");
      Committed = RoundNum;
    } else if (Tag == "ROOT") {
      std::string Sha = NextToken(ISS);
      if (!IsCanonicalSha1(Sha) || ContainsSha(Roots, Sha))
        return Fail("bad or duplicate ROOT");
      Roots.insert(std::upper_bound(Roots.begin(), Roots.end(), Sha), Sha);
    } else if (Tag == "HIST") {
      HistRec H;
      std::string R = NextToken(ISS), P = NextToken(ISS),
                  Br = NextToken(ISS), Bs = NextToken(ISS);
      if (!IsCanonicalSha1(R) || !IsCanonicalSha1(P) ||
          !ParseSize(Br, &H.BornRound) || !ParseSize(Bs, &H.BornSlot) ||
          H.BornSlot >= Params.Slots)
        return Fail("bad HIST line");
      H.Sha = R;
      H.Parent = P;
      auto HistIt =
          std::lower_bound(Hists.begin(), Hists.end(), H.Sha,
                           [](const HistRec &Hr, const std::string &S) {
                             return Hr.Sha < S;
                           });
      if (HistIt != Hists.end() && HistIt->Sha == H.Sha)
        return Fail("duplicate HIST");
      Hists.insert(HistIt, H);
    } else if (Tag == "NODE") {
      Entry E;
      std::string Sha = NextToken(ISS), IsRoot = NextToken(ISS),
                  Parent = NextToken(ISS), Br = NextToken(ISS),
                  Bs = NextToken(ISS), Lr = NextToken(ISS),
                  Ac = NextToken(ISS);
      if (!IsCanonicalSha1(Sha) || (IsRoot != "0" && IsRoot != "1") ||
          !ParseSize(Br, &E.BornRound) || !ParseSize(Bs, &E.BornSlot) ||
          !ParseSize(Ac, &E.AssignedCount) || E.BornSlot >= Params.Slots)
        return Fail("bad NODE line");
      if (Lr != "-1") {
        if (!ParseSize(Lr, &E.LastRound))
          return Fail("bad NODE last round");
      } else {
        E.LastRound = (size_t)-1;
      }
      E.Sha = Sha;
      E.IsRoot = IsRoot == "1";
      if (Parent == "-") {
        E.Parent.clear();
      } else {
        if (!IsCanonicalSha1(Parent))
          return Fail("bad NODE parent");
        E.Parent = Parent;
      }
      auto NodeIt =
          std::lower_bound(Nodes.begin(), Nodes.end(), E.Sha,
                           [](const Entry &En, const std::string &S) {
                             return En.Sha < S;
                           });
      if (NodeIt != Nodes.end() && NodeIt->Sha == E.Sha)
        return Fail("duplicate NODE");
      Nodes.insert(NodeIt, E);
    } else if (Tag == "DIAG") {
      EpochDiagnostic D;
      std::string R = NextToken(ISS), S = NextToken(ISS);
      if (!ParseSize(R, &D.Round) || !ParseSize(S, &D.Slot) ||
          D.Slot >= Params.Slots)
        return Fail("bad DIAG line");
      std::string Rest;
      std::getline(ISS, Rest);
      if (!Rest.empty() && Rest[0] == ' ')
        Rest.erase(0, 1);
      if (Rest.empty())
        return Fail("empty DIAG key");
      D.Key = Rest;
      auto DiagIt =
          std::lower_bound(Diags.begin(), Diags.end(), D.Key,
                           [](const EpochDiagnostic &Di,
                              const std::string &K) { return Di.Key < K; });
      if (DiagIt != Diags.end() && DiagIt->Key == D.Key)
        return Fail("duplicate DIAG");
      Diags.insert(DiagIt, D);
    } else {
      return Fail("unknown state record");
    }
  }
  if (!SawParam || !SawEnd || Roots.empty())
    return Fail("truncated state file");
  if (std::getline(IS, Line))
    return Fail("trailing data after END");
  if (Committed > Params.Rounds)
    return Fail("committed round count exceeds the schedule");

  // The initial corpus identity must still be on disk.
  for (const auto &Sha : Roots)
    if (!ContainsSha(DiskShas, Sha))
      return Fail("initial corpus does not match the state file");

  auto HasHist = [&](const std::string &Sha) {
    auto It = std::lower_bound(Hists.begin(), Hists.end(), Sha,
                               [](const HistRec &Hr, const std::string &S) {
                                 return Hr.Sha < S;
                               });
    return It != Hists.end() && It->Sha == Sha;
  };

  // Cross-validate records before touching *Out.
  for (const auto &H : Hists) {
    if (!ContainsSha(Roots, H.Parent) && !HasHist(H.Parent))
      return Fail("HIST parent is unknown");
    if (H.BornRound >= Committed)
      return Fail("HIST born in an uncommitted round");
  }
  std::vector<Entry> LiveNodes;
  for (const auto &E : Nodes) {
    if (E.IsRoot) {
      if (!ContainsSha(Roots, E.Sha) || !E.Parent.empty())
        return Fail("inconsistent root NODE");
    } else {
      auto It = std::lower_bound(Hists.begin(), Hists.end(), E.Sha,
                                 [](const HistRec &H, const std::string &S) {
                                   return H.Sha < S;
                                 });
      if (It == Hists.end() || It->Sha != E.Sha || It->Parent != E.Parent ||
          It->BornRound != E.BornRound || It->BornSlot != E.BornSlot)
        return Fail("NODE without matching HIST");
    }
    if (E.LastRound != (size_t)-1 && E.LastRound >= Committed)
      return Fail("NODE assigned in an uncommitted round");
    if (!ContainsSha(DiskShas, E.Sha)) {
      if (E.IsRoot)
        return Fail("initial corpus does not match the state file");
      continue; // Eliminated from the live corpus; lineage is kept in HIST.
    }
    LiveNodes.push_back(E);
  }
  for (const auto &D : Diags)
    if (D.Round >= Committed)
      return Fail("DIAG from an uncommitted round");

  EpochSchedule Res;
  Res.SlotCount = Params.Slots;
  Res.CurrentRound = Committed;
  Res.InRound = false;
  Res.ResetPending();
  Res.Entries = std::move(LiveNodes);
  Res.DiagStore = std::move(Diags);
  for (const auto &Sha : Roots) {
    EpochLineage L;
    L.Sha = Sha;
    Res.History.push_back(L);
  }
  for (const auto &H : Hists) {
    EpochLineage L;
    L.Sha = H.Sha;
    L.Parent = H.Parent;
    L.BornRound = H.BornRound;
    L.BornSlot = H.BornSlot;
    Res.History.insert(std::upper_bound(Res.History.begin(),
                                        Res.History.end(), L.Sha,
                                        [](const std::string &S,
                                           const EpochLineage &Li) {
                                          return S < Li.Sha;
                                        }),
                       L);
  }
  *Out = std::move(Res);
  return true;
}

EpochStore::EpochStore(std::string Path, std::vector<std::string> DiskShas,
                       EpochParams Params)
    : StatePath(std::move(Path)), Params(Params) {
  std::sort(DiskShas.begin(), DiskShas.end());
  DiskShas.erase(std::unique(DiskShas.begin(), DiskShas.end()),
                 DiskShas.end());
  this->DiskShas = std::move(DiskShas);
}

bool EpochStore::Exists() const { return IsFile(StatePath); }

bool EpochStore::Load(EpochSchedule *Out, std::string *Error) const {
  if (!Exists()) {
    if (Error)
      *Error = "state file does not exist";
    return false;
  }
  std::ifstream IS(StatePath, std::ios::binary);
  if (!IS) {
    if (Error)
      *Error = "state file is unreadable";
    return false;
  }
  return EpochSchedule::ImportText(IS, DiskShas, Params, Out, Error);
}

bool EpochStore::Save(const EpochSchedule &Schedule,
                      std::string *Error) const {
  std::string TmpPath = StatePath + ".tmp";
  {
    std::ofstream OS(TmpPath, std::ios::binary | std::ios::trunc);
    if (!OS || !Schedule.ExportText(OS, Params)) {
      RemoveFile(TmpPath);
      if (Error)
        *Error = "failed to write state file";
      return false;
    }
  }
  // rename(2) atomically replaces the previous committed state.
  RenameFile(TmpPath, StatePath);
  if (!IsFile(StatePath)) {
    if (Error)
      *Error = "failed to replace state file";
    return false;
  }
  return true;
}

} // namespace fuzzer
