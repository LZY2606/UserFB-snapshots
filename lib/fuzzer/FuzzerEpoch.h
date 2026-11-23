//===- FuzzerEpoch.h - deterministic round schedule -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// A deterministic, resumable round schedule used by the optional epoch fork
// mode.  Assignment decisions depend only on committed rounds: the order in
// which parallel workers finish never changes what the next round looks like.
//===----------------------------------------------------------------------===//

#ifndef LLVM_FUZZER_EPOCH_H
#define LLVM_FUZZER_EPOCH_H

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace fuzzer {

// Fixed parameters of an epoch replay.  They are part of the on-disk identity
// and a state file created with different parameters is rejected on resume.
struct EpochParams {
  size_t Slots = 0;        // -fork value, i.e. seeds per round.
  size_t RunsPerSlot = 0;  // -schedule_runs value.
  size_t Rounds = 0;       // -schedule_rounds value.
};

struct EpochLineage {
  std::string Sha;
  std::string Parent;      // Empty for roots.
  size_t BornRound = 0;
  size_t BornSlot = 0;
};

struct EpochDiagnostic {
  std::string Key;
  size_t Round = 0;
  size_t Slot = 0;
};

// Returns true iff Sha is 40 lowercase hexadecimal characters, the canonical
// textual form of a SHA1 digest.
bool IsCanonicalSha1(const std::string &Sha);

class EpochSchedule {
public:
  EpochSchedule() = default;

  // Registers an initial corpus identity.  Roots must be canonical SHA1
  // strings and duplicates are rejected.  Roots can only be added before the
  // first round starts.
  bool AddRoot(const std::string &Sha);

  // Freezes the current corpus and returns the SHA assigned to every slot of
  // the upcoming round.  Never-assigned inputs go first; the rest is ordered by
  // the number of complete rounds since its last assignment (descending) and
  // SHA1 (ascending), and the list cycles until every slot is filled.
  // Returns an empty vector without changing committed state on error.
  std::vector<std::string> BeginRound(size_t Slots);

  // Reports a slot result.  An empty diagnostic means success; a non-empty one
  // is the classification key (full first log line) of a classified failure.
  // Children are canonical SHA1 strings of newly discovered inputs.
  // Rejected reports (bad slot, duplicate report, bad SHA) change nothing.
  bool Complete(size_t Slot, const std::vector<std::string> &Children,
                const std::string &Diagnostic);

  // Reports a failure without a classification key.  Such a slot is still
  // completed, but contributes neither coverage nor new files.
  bool Fail(size_t Slot);

  // Commits a round once every slot has reported.  Only a successful commit
  // advances Round(); anything less leaves the committed state untouched.
  bool CommitRound();

  // Number of successfully committed rounds (0 for a fresh object).
  size_t Round() const { return CurrentRound; }

  // Read-only inspection, sorted by SHA / diagnostic key.  Lineage and
  // diagnostics are returned by reference so callers can hold pointers into
  // them; the references stay valid until the schedule is modified.
  std::vector<std::string> RootShas() const;
  std::vector<std::string> LiveShas() const;
  const std::vector<EpochLineage> &Lineage() const;
  const std::vector<EpochDiagnostic> &Diagnostics() const;

  // Textual (de)serialization of the committed state.  Uncommitted rounds are
  // never exported.  Import rejects unknown versions, malformed lines, roots or
  // live nodes missing from DiskShas, parameters that do not match, and
  // internal inconsistencies, in which case *Out is left untouched.
  bool ExportText(std::ostream &OS, const EpochParams &Params) const;
  static bool ImportText(std::istream &IS,
                         const std::vector<std::string> &DiskShas,
                         const EpochParams &Params, EpochSchedule *Out,
                         std::string *Error);

private:
  struct Entry {
    std::string Sha;
    bool IsRoot = false;
    std::string Parent;            // Empty for roots.
    size_t BornRound = 0;
    size_t BornSlot = 0;
    size_t LastRound = (size_t)-1; // Never assigned.
    size_t AssignedCount = 0;
  };

  bool RecordReport(size_t Slot, bool Failed,
                    const std::vector<std::string> &Children,
                    const std::string &Diagnostic);
  Entry *Find(const std::string &Sha);
  const Entry *Find(const std::string &Sha) const;
  void ResetPending();

  size_t CurrentRound = 0;
  size_t SlotCount = 0;
  bool InRound = false;
  std::vector<Entry> Entries;             // Live corpus, kept sorted by SHA.
  std::vector<EpochLineage> History;      // Every file ever seen, sorted by SHA.
  std::vector<EpochDiagnostic> DiagStore;
  std::vector<std::string> PendingAssign; // Indexed by slot.
  std::vector<bool> PendingDone;
  std::vector<bool> PendingFailed;
  std::vector<std::string> PendingDiag;
  std::vector<std::vector<std::string>> PendingChildren;
};

// On-disk manager for an EpochSchedule state file.  Every save atomically
// replaces the previous file with one committed round as its unit.  Random
// temporary directory paths are never part of the stored identity.
class EpochStore {
public:
  // DiskShas are the content SHA1 values currently present in the local
  // corpus directory.
  EpochStore(std::string Path, std::vector<std::string> DiskShas,
             EpochParams Params);

  bool Exists() const;
  // False (with *Error set) when the file is missing, unreadable, corrupt or
  // incompatible with the on-disk corpus/parameters.  The old file is never
  // modified by a failed load.
  bool Load(EpochSchedule *Out, std::string *Error = nullptr) const;
  // False (with *Error set) without touching the previous file on failure.
  bool Save(const EpochSchedule &Schedule, std::string *Error = nullptr) const;

  const std::string &Path() const { return StatePath; }

private:
  std::string StatePath;
  std::vector<std::string> DiskShas;
  EpochParams Params;
};

} // namespace fuzzer

#endif // LLVM_FUZZER_EPOCH_H
