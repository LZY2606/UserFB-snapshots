//===- epoch_replay.cpp - two-process epoch schedule replay example ------===//
//
// A small client of fuzzer::EpochSchedule / fuzzer::EpochStore that does not
// fuzz anything: it drives the scheduler directly so the deterministic round
// machinery can be observed.  The first process replays the first half of the
// schedule, then launches a second, independent process that resumes from the
// atomically replaced state file and finishes the rest.  Both processes
// append the per-round slot assignments, the parent of every discovered child
// and the earliest diagnostic representatives to OUTPUT_DIR/transcript.txt.
//
// Usage: epoch-replay OUTPUT_DIR [resume]
//
//===----------------------------------------------------------------------===//

#include "lib/fuzzer/FuzzerEpoch.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

using fuzzer::EpochParams;
using fuzzer::EpochSchedule;
using fuzzer::EpochStore;

namespace {

const size_t kSlots = 2;
const size_t kRunsPerSlot = 8;
const size_t kTotalRounds = 4;
const size_t kFirstProcessRounds = 2;
const char *kDiagKey = "ERROR: synthetic example diagnostic";

std::string PaddedSha(char Fill, size_t Count) {
  return std::string(Count, Fill);
}

// Deterministic, canonical 40-char lowercase-hex identity for the synthetic
// child discovered in round RoundNo.
std::string ChildSha(size_t RoundNo) {
  char Buf[3];
  std::snprintf(Buf, sizeof(Buf), "%02zx", RoundNo + 1);
  return std::string(38, '0') + Buf;
}

int Run(const std::string &Dir, bool Resume) {
  const std::string StatePath = Dir + "/epoch.state";
  const std::string TranscriptPath = Dir + "/transcript.txt";

  const std::string Root1 = PaddedSha('1', 40);
  const std::string Root2 = PaddedSha('2', 40);

  EpochParams Params;
  Params.Slots = kSlots;
  Params.RunsPerSlot = kRunsPerSlot;
  Params.Rounds = kTotalRounds;

  // The example keeps everything synthetic, so the "on-disk corpus" identity
  // is just the roots plus every child this schedule can produce.
  std::vector<std::string> DiskShas = {Root1, Root2};
  for (size_t R = 0; R < kTotalRounds; R++)
    DiskShas.push_back(ChildSha(R));

  EpochStore Store(StatePath, DiskShas, Params);
  EpochSchedule Schedule;
  if (Resume) {
    std::string Error;
    if (!Store.Load(&Schedule, &Error)) {
      std::fprintf(stderr, "resume failed: %s\n", Error.c_str());
      return 1;
    }
  } else {
    if (!Schedule.AddRoot(Root1) || !Schedule.AddRoot(Root2)) {
      std::fprintf(stderr, "failed to register synthetic roots\n");
      return 1;
    }
    std::string Error;
    if (!Store.Save(Schedule, &Error)) {
      std::fprintf(stderr, "initial save failed: %s\n", Error.c_str());
      return 1;
    }
  }

  std::ofstream Transcript(TranscriptPath,
                           std::ios::out | std::ios::app);
  if (!Transcript) {
    std::fprintf(stderr, "cannot open %s\n", TranscriptPath.c_str());
    return 1;
  }
  Transcript << "PROCESS resume=" << (Resume ? 1 : 0)
             << " start_round=" << Schedule.Round() << '\n';

  const size_t StopAt = Resume ? kTotalRounds : kFirstProcessRounds;
  while (Schedule.Round() < StopAt) {
    const size_t RoundNo = Schedule.Round();
    std::vector<std::string> Assigned = Schedule.BeginRound(kSlots);
    if (Assigned.size() != kSlots) {
      std::fprintf(stderr, "BeginRound failed at round %zu\n", RoundNo);
      return 1;
    }
    for (size_t Slot = 0; Slot < kSlots; Slot++)
      Transcript << "ROUND " << RoundNo << " SLOT " << Slot
                 << " SEED " << Assigned[Slot] << '\n';

    // Slot 0 deterministically discovers one new child, whose parent is the
    // seed actually assigned to that slot.
    const std::string Child = ChildSha(RoundNo);
    if (!Schedule.Complete(0, {Child}, "")) {
      std::fprintf(stderr, "slot 0 report failed at round %zu\n", RoundNo);
      return 1;
    }
    Transcript << "ROUND " << RoundNo << " SLOT 0 CHILD " << Child
               << " PARENT " << Assigned[0] << '\n';

    // Slot 1 carries a synthetic diagnostic in rounds 1 and 3.  The state
    // interface keeps the earliest round/slot as the representative.
    const std::string Diag =
        (RoundNo == 1 || RoundNo == 3) ? std::string(kDiagKey) : std::string();
    if (!Schedule.Complete(1, {}, Diag)) {
      std::fprintf(stderr, "slot 1 report failed at round %zu\n", RoundNo);
      return 1;
    }

    if (!Schedule.CommitRound()) {
      std::fprintf(stderr, "commit failed at round %zu\n", RoundNo);
      return 1;
    }
    std::string Error;
    if (!Store.Save(Schedule, &Error)) {
      std::fprintf(stderr, "save failed: %s\n", Error.c_str());
      return 1;
    }
    Transcript << "ROUND " << RoundNo << " COMMITTED round="
               << Schedule.Round() << '\n';
  }

  // The second process finishes the schedule, so it is the one that records
  // the final diagnostic representatives (earliest round/slot, not the last
  // process to finish).
  if (Schedule.Round() == kTotalRounds) {
    for (const auto &D : Schedule.Diagnostics())
      Transcript << "DIAG_REP " << D.Key << " ROUND " << D.Round << " SLOT "
                 << D.Slot << '\n';
    for (const auto &L : Schedule.Lineage())
      if (!L.Parent.empty())
        Transcript << "LINEAGE " << L.Sha << " PARENT " << L.Parent
                   << " BORN_ROUND " << L.BornRound << " BORN_SLOT "
                   << L.BornSlot << '\n';
  }
  return 0;
}

} // namespace

int main(int Argc, char **Argv) {
  if (Argc < 2) {
    std::fprintf(stderr, "usage: %s OUTPUT_DIR [resume]\n", Argv[0]);
    return 1;
  }
  const std::string Dir = Argv[1];
  mkdir(Dir.c_str(), 0700); // Best effort; an existing dir is fine.
  const bool Resume = Argc >= 3 && std::string(Argv[2]) == "resume";

  if (int Rc = Run(Dir, Resume))
    return Rc;

  if (!Resume) {
    // Hand the same state file to a fully independent second process.  It
    // resumes from the last committed round and completes the schedule.
    const std::string Command =
        std::string(Argv[0]) + " '" + Dir + "' resume";
    int Status = std::system(Command.c_str());
    if (Status != 0) {
      std::fprintf(stderr, "resume process failed (status=%d)\n", Status);
      return 1;
    }
  }
  return 0;
}
