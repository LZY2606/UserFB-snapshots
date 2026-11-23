//===- FuzzerFork.cpp - run fuzzing in separate subprocesses --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Spawn and orchestrate separate fuzzing processes.
//===----------------------------------------------------------------------===//

#include "FuzzerCommand.h"
#include "FuzzerEpoch.h"
#include "FuzzerFork.h"
#include "FuzzerIO.h"
#include "FuzzerInternal.h"
#include "FuzzerMerge.h"
#include "FuzzerSHA1.h"
#include "FuzzerTracePC.h"
#include "FuzzerUtil.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>

namespace fuzzer {

struct Stats {
  size_t number_of_executed_units = 0;
  size_t peak_rss_mb = 0;
  size_t average_exec_per_sec = 0;
};

static Stats ParseFinalStatsFromLog(const std::string &LogPath) {
  std::ifstream In(LogPath);
  std::string Line;
  Stats Res;
  struct {
    const char *Name;
    size_t *Var;
  } NameVarPairs[] = {
      {"stat::number_of_executed_units:", &Res.number_of_executed_units},
      {"stat::peak_rss_mb:", &Res.peak_rss_mb},
      {"stat::average_exec_per_sec:", &Res.average_exec_per_sec},
      {nullptr, nullptr},
  };
  while (std::getline(In, Line, '\n')) {
    if (Line.find("stat::") != 0) continue;
    std::istringstream ISS(Line);
    std::string Name;
    size_t Val;
    ISS >> Name >> Val;
    for (size_t i = 0; NameVarPairs[i].Name; i++)
      if (Name == NameVarPairs[i].Name)
        *NameVarPairs[i].Var = Val;
  }
  return Res;
}

struct FuzzJob {
  // Inputs.
  Command Cmd;
  std::string CorpusDir;
  std::string FeaturesDir;
  std::string LogPath;
  std::string SeedListPath;
  std::string CFPath;
  size_t      JobId;

  int         DftTimeInSeconds = 0;

  // Fuzzing Outputs.
  int ExitCode;

  ~FuzzJob() {
    RemoveFile(CFPath);
    RemoveFile(LogPath);
    RemoveFile(SeedListPath);
    RmDirRecursive(CorpusDir);
    RmDirRecursive(FeaturesDir);
  }
};

struct GlobalEnv {
  Vector<std::string> Args;
  Vector<std::string> CorpusDirs;
  std::string MainCorpusDir;
  std::string TempDir;
  std::string DFTDir;
  std::string DataFlowBinary;
  Set<uint32_t> Features, Cov;
  Set<std::string> FilesWithDFT;
  Vector<std::string> Files;
  Random *Rand;
  std::chrono::system_clock::time_point ProcessStartTime;
  int Verbosity = 0;

  size_t NumTimeouts = 0;
  size_t NumOOMs = 0;
  size_t NumCrashes = 0;


  size_t NumRuns = 0;

  std::string StopFile() { return DirPlusFile(TempDir, "STOP"); }

  size_t secondsSinceProcessStartUp() const {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now() - ProcessStartTime)
        .count();
  }

  FuzzJob *CreateNewJob(size_t JobId) {
    Command Cmd(Args);
    Cmd.removeFlag("fork");
    Cmd.removeFlag("runs");
    Cmd.removeFlag("collect_data_flow");
    for (auto &C : CorpusDirs) // Remove all corpora from the args.
      Cmd.removeArgument(C);
    Cmd.addFlag("reload", "0");  // working in an isolated dir, no reload.
    Cmd.addFlag("print_final_stats", "1");
    Cmd.addFlag("print_funcs", "0");  // no need to spend time symbolizing.
    Cmd.addFlag("max_total_time", std::to_string(std::min((size_t)300, JobId)));
    Cmd.addFlag("stop_file", StopFile());
    if (!DataFlowBinary.empty()) {
      Cmd.addFlag("data_flow_trace", DFTDir);
      if (!Cmd.hasFlag("focus_function"))
        Cmd.addFlag("focus_function", "auto");
    }
    auto Job = new FuzzJob;
    std::string Seeds;
    if (size_t CorpusSubsetSize =
            std::min(Files.size(), (size_t)sqrt(Files.size() + 2))) {
      auto Time1 = std::chrono::system_clock::now();
      for (size_t i = 0; i < CorpusSubsetSize; i++) {
        auto &SF = Files[Rand->SkewTowardsLast(Files.size())];
        Seeds += (Seeds.empty() ? "" : ",") + SF;
        CollectDFT(SF);
      }
      auto Time2 = std::chrono::system_clock::now();
      Job->DftTimeInSeconds = duration_cast<seconds>(Time2 - Time1).count();
    }
    if (!Seeds.empty()) {
      Job->SeedListPath =
          DirPlusFile(TempDir, std::to_string(JobId) + ".seeds");
      WriteToFile(Seeds, Job->SeedListPath);
      Cmd.addFlag("seed_inputs", "@" + Job->SeedListPath);
    }
    Job->LogPath = DirPlusFile(TempDir, std::to_string(JobId) + ".log");
    Job->CorpusDir = DirPlusFile(TempDir, "C" + std::to_string(JobId));
    Job->FeaturesDir = DirPlusFile(TempDir, "F" + std::to_string(JobId));
    Job->CFPath = DirPlusFile(TempDir, std::to_string(JobId) + ".merge");
    Job->JobId = JobId;


    Cmd.addArgument(Job->CorpusDir);
    Cmd.addFlag("features_dir", Job->FeaturesDir);

    for (auto &D : {Job->CorpusDir, Job->FeaturesDir}) {
      RmDirRecursive(D);
      MkDir(D);
    }

    Cmd.setOutputFile(Job->LogPath);
    Cmd.combineOutAndErr();

    Job->Cmd = Cmd;

    if (Verbosity >= 2)
      Printf("Job %zd/%p Created: %s\n", JobId, Job,
             Job->Cmd.toString().c_str());
    // Start from very short runs and gradually increase them.
    return Job;
  }

  void RunOneMergeJob(FuzzJob *Job) {
    auto Stats = ParseFinalStatsFromLog(Job->LogPath);
    NumRuns += Stats.number_of_executed_units;

    Vector<SizedFile> TempFiles, MergeCandidates;
    // Read all newly created inputs and their feature sets.
    // Choose only those inputs that have new features.
    GetSizedFilesFromDir(Job->CorpusDir, &TempFiles);
    std::sort(TempFiles.begin(), TempFiles.end());
    for (auto &F : TempFiles) {
      auto FeatureFile = F.File;
      FeatureFile.replace(0, Job->CorpusDir.size(), Job->FeaturesDir);
      auto FeatureBytes = FileToVector(FeatureFile, 0, false);
      assert((FeatureBytes.size() % sizeof(uint32_t)) == 0);
      Vector<uint32_t> NewFeatures(FeatureBytes.size() / sizeof(uint32_t));
      memcpy(NewFeatures.data(), FeatureBytes.data(), FeatureBytes.size());
      for (auto Ft : NewFeatures) {
        if (!Features.count(Ft)) {
          MergeCandidates.push_back(F);
          break;
        }
      }
    }
    // if (!FilesToAdd.empty() || Job->ExitCode != 0)
    Printf("#%zd: cov: %zd ft: %zd corp: %zd exec/s %zd "
           "oom/timeout/crash: %zd/%zd/%zd time: %zds job: %zd dft_time: %d\n",
           NumRuns, Cov.size(), Features.size(), Files.size(),
           Stats.average_exec_per_sec, NumOOMs, NumTimeouts, NumCrashes,
           secondsSinceProcessStartUp(), Job->JobId, Job->DftTimeInSeconds);

    if (MergeCandidates.empty()) return;

    Vector<std::string> FilesToAdd;
    Set<uint32_t> NewFeatures, NewCov;
    CrashResistantMerge(Args, {}, MergeCandidates, &FilesToAdd, Features,
                        &NewFeatures, Cov, &NewCov, Job->CFPath, false);
    for (auto &Path : FilesToAdd) {
      auto U = FileToVector(Path);
      auto NewPath = DirPlusFile(MainCorpusDir, Hash(U));
      WriteToFile(U, NewPath);
      Files.push_back(NewPath);
    }
    Features.insert(NewFeatures.begin(), NewFeatures.end());
    Cov.insert(NewCov.begin(), NewCov.end());
    for (auto Idx : NewCov)
      if (auto *TE = TPC.PCTableEntryByIdx(Idx))
        if (TPC.PcIsFuncEntry(TE))
          PrintPC("  NEW_FUNC: %p %F %L\n", "",
                  TPC.GetNextInstructionPc(TE->PC));

  }


  void CollectDFT(const std::string &InputPath) {
    if (DataFlowBinary.empty()) return;
    if (!FilesWithDFT.insert(InputPath).second) return;
    Command Cmd(Args);
    Cmd.removeFlag("fork");
    Cmd.removeFlag("runs");
    Cmd.addFlag("data_flow_trace", DFTDir);
    Cmd.addArgument(InputPath);
    for (auto &C : CorpusDirs) // Remove all corpora from the args.
      Cmd.removeArgument(C);
    Cmd.setOutputFile(DirPlusFile(TempDir, "dft.log"));
    Cmd.combineOutAndErr();
    // Printf("CollectDFT: %s\n", Cmd.toString().c_str());
    ExecuteCommand(Cmd);
  }

};

struct JobQueue {
  std::queue<FuzzJob *> Qu;
  std::mutex Mu;
  std::condition_variable Cv;

  void Push(FuzzJob *Job) {
    {
      std::lock_guard<std::mutex> Lock(Mu);
      Qu.push(Job);
    }
    Cv.notify_one();
  }
  FuzzJob *Pop() {
    std::unique_lock<std::mutex> Lk(Mu);
    // std::lock_guard<std::mutex> Lock(Mu);
    Cv.wait(Lk, [&]{return !Qu.empty();});
    assert(!Qu.empty());
    auto Job = Qu.front();
    Qu.pop();
    return Job;
  }
};

void WorkerThread(JobQueue *FuzzQ, JobQueue *MergeQ) {
  while (auto Job = FuzzQ->Pop()) {
    // Printf("WorkerThread: job %p\n", Job);
    Job->ExitCode = ExecuteCommand(Job->Cmd);
    MergeQ->Push(Job);
  }
}

//===----------------------------------------------------------------------===//
// Epoch (deterministic round replay) mode.
//===----------------------------------------------------------------------===//

// Deterministic per (round, slot) random seed.  Derived from nothing but the
// round and slot numbers so two replays make the same assignments and runs.
static unsigned EpochSeed(size_t Round, size_t Slot) {
  uint64_t V = 0x9e3779b97f4a7c15ULL;
  V ^= (uint64_t)Round + 0x9e3779b97f4a7c15ULL;
  V = (V ^ (V >> 30)) * 0xbf58476d1ce4e5b9ULL;
  V ^= (uint64_t)Slot + 0x94d049bb133111ebULL;
  V = (V ^ (V >> 27)) * 0x94d049bb133111ebULL;
  V ^= V >> 31;
  // Keep the lowest bit set so libFuzzer never treats 0 as "pick a seed".
  return (unsigned)(V & 0xffffffffu) | 1u;
}

// The classification key is the full text of the first log line carrying an
// "ERROR:" or "runtime error:" marker.  Addresses are never symbolized and no
// crash input is constructed.  Returns an empty string when neither marker is
// present; such a failure has no classification key.
static std::string EpochDiagnosticKey(const std::string &LogPath) {
  std::ifstream In(LogPath);
  std::string Line;
  while (std::getline(In, Line)) {
    if (!Line.empty() && Line.back() == '\r')
      Line.pop_back();
    if (Line.find("ERROR:") != std::string::npos ||
        Line.find("runtime error:") != std::string::npos)
      return Line;
  }
  return "";
}

// Same features/merge collection path as GlobalEnv::RunOneMergeJob, but
// returns the content SHA1 of every newly accepted input instead of touching
// the schedule.  The crash-resistant merge / minimum-coverage algorithm itself
// is reused unchanged.
static void EpochMergeJob(GlobalEnv *Env, FuzzJob *Job,
                          std::map<std::string, std::string> *ShaToPath,
                          std::vector<std::string> *AddedShas) {
  auto Stats = ParseFinalStatsFromLog(Job->LogPath);
  Env->NumRuns += Stats.number_of_executed_units;

  Vector<SizedFile> TempFiles, MergeCandidates;
  GetSizedFilesFromDir(Job->CorpusDir, &TempFiles);
  std::sort(TempFiles.begin(), TempFiles.end());
  for (auto &F : TempFiles) {
    auto FeatureFile = F.File;
    FeatureFile.replace(0, Job->CorpusDir.size(), Job->FeaturesDir);
    auto FeatureBytes = FileToVector(FeatureFile, 0, false);
    assert((FeatureBytes.size() % sizeof(uint32_t)) == 0);
    Vector<uint32_t> NewFeatures(FeatureBytes.size() / sizeof(uint32_t));
    memcpy(NewFeatures.data(), FeatureBytes.data(), FeatureBytes.size());
    for (auto Ft : NewFeatures) {
      if (!Env->Features.count(Ft)) {
        MergeCandidates.push_back(F);
        break;
      }
    }
  }
  Printf("#%zd: epoch cov: %zd ft: %zd corp: %zd exec/s %zd job: %zd\n",
         Env->NumRuns, Env->Cov.size(), Env->Features.size(),
         Env->Files.size(), Stats.average_exec_per_sec, Job->JobId);

  if (MergeCandidates.empty())
    return;

  Vector<std::string> FilesToAdd;
  Set<uint32_t> NewFeatures, NewCov;
  CrashResistantMerge(Env->Args, {}, MergeCandidates, &FilesToAdd,
                      Env->Features, &NewFeatures, Env->Cov, &NewCov,
                      Job->CFPath, false);
  for (auto &Path : FilesToAdd) {
    auto U = FileToVector(Path);
    auto Sha = Hash(U);
    if (ShaToPath->count(Sha))
      continue; // Identical bytes already discovered earlier this round.
    auto NewPath = DirPlusFile(Env->MainCorpusDir, Sha);
    WriteToFile(U, NewPath);
    (*ShaToPath)[Sha] = NewPath;
    Env->Files.push_back(NewPath);
    AddedShas->push_back(Sha);
  }
  Env->Features.insert(NewFeatures.begin(), NewFeatures.end());
  Env->Cov.insert(NewCov.begin(), NewCov.end());
}

static std::unique_ptr<FuzzJob>
CreateEpochJob(const Vector<std::string> &Args,
               const Vector<std::string> &CorpusDirs,
               const std::string &TempDir, const std::string &SeedPath,
               size_t RoundNo, size_t Slot, size_t RunsPerSlot) {
  std::unique_ptr<FuzzJob> Job(new FuzzJob);
  Command Cmd(Args);
  Cmd.removeFlag("fork");
  Cmd.removeFlag("runs");
  Cmd.removeFlag("max_total_time");
  Cmd.removeFlag("collect_data_flow");
  Cmd.removeFlag("schedule_rounds");
  Cmd.removeFlag("schedule_runs");
  Cmd.removeFlag("schedule_state");
  for (auto &C : CorpusDirs) // Remove all corpora from the args.
    Cmd.removeArgument(C);
  Cmd.addFlag("reload", "0"); // Isolated dir; nothing to reload.
  Cmd.addFlag("print_final_stats", "1");
  Cmd.addFlag("print_funcs", "0");
  // Fixed run budget per slot; wall-clock based exits are disabled.
  Cmd.addFlag("runs", std::to_string(RunsPerSlot));
  Cmd.addFlag("max_total_time", "0");
  Cmd.addFlag("seed", std::to_string(EpochSeed(RoundNo, Slot)));
  Job->SeedListPath = DirPlusFile(TempDir, "r" + std::to_string(RoundNo) +
                                               ".s" + std::to_string(Slot) +
                                               ".seeds");
  WriteToFile(SeedPath, Job->SeedListPath);
  Cmd.addFlag("seed_inputs", "@" + Job->SeedListPath);

  Job->LogPath = DirPlusFile(TempDir, "r" + std::to_string(RoundNo) + ".s" +
                                          std::to_string(Slot) + ".log");
  Job->CorpusDir = DirPlusFile(TempDir, "r" + std::to_string(RoundNo) + ".s" +
                                            std::to_string(Slot) + ".C");
  Job->FeaturesDir = DirPlusFile(TempDir, "r" + std::to_string(RoundNo) +
                                              ".s" + std::to_string(Slot) +
                                              ".F");
  Job->CFPath = DirPlusFile(TempDir, "r" + std::to_string(RoundNo) + ".s" +
                                         std::to_string(Slot) + ".merge");
  Job->JobId = Slot;

  Cmd.addArgument(Job->CorpusDir);
  Cmd.addFlag("features_dir", Job->FeaturesDir);
  // Keep any crash artifact inside the per-job scratch dir; the FuzzJob
  // destructor removes it together with the rest of the job.  Fault inputs are
  // never reconstructed by the orchestrator.
  Cmd.addFlag("artifact_prefix", DirPlusFile(Job->CorpusDir, "artifact-"));
  for (auto &D : {Job->CorpusDir, Job->FeaturesDir}) {
    RmDirRecursive(D);
    MkDir(D);
  }
  Cmd.setOutputFile(Job->LogPath);
  Cmd.combineOutAndErr();
  Job->Cmd = Cmd;
  return Job;
}

static void EpochFuzzWithFork(const FuzzingOptions &Options,
                              const Vector<std::string> &Args,
                              const Vector<std::string> &CorpusDirs,
                              int NumJobs) {
  if (NumJobs <= 0 || Options.ScheduleRounds <= 0 ||
      Options.ScheduleRuns <= 0 || Options.ScheduleState.empty()) {
    Printf("ERROR: epoch mode requires -fork=N -schedule_rounds=N "
           "-schedule_runs=K -schedule_state=PATH with positive N and K\n");
    exit(1);
  }
  if (CorpusDirs.empty()) {
    Printf("ERROR: epoch mode requires a local corpus directory\n");
    exit(1);
  }
  for (const auto &Dir : CorpusDirs) {
    if (GetEpoch(Dir) == 0) {
      Printf("ERROR: epoch mode only handles existing local directories: "
             "%s\n",
             Dir.c_str());
      exit(1);
    }
  }

  const size_t Slots = (size_t)NumJobs;
  EpochParams Params;
  Params.Slots = Slots;
  Params.RunsPerSlot = (size_t)Options.ScheduleRuns;
  Params.Rounds = (size_t)Options.ScheduleRounds;

  Printf("INFO: -fork=%zd epoch mode: %zd rounds, %zd runs per slot, state "
         "%s\n",
         Slots, Params.Rounds, Params.RunsPerSlot,
         Options.ScheduleState.c_str());

  GlobalEnv Env;
  Env.Args = Args;
  Env.CorpusDirs = CorpusDirs;
  Env.Verbosity = Options.Verbosity;
  Env.ProcessStartTime = std::chrono::system_clock::now();
  Env.MainCorpusDir = CorpusDirs[0];
  Env.TempDir = TempPath(".epoch");
  RmDirRecursive(Env.TempDir);
  MkDir(Env.TempDir);

  // Freeze the starting corpus through the same crash-resistant merge path.
  Vector<SizedFile> SeedFiles;
  for (auto &Dir : CorpusDirs)
    GetSizedFilesFromDir(Dir, &SeedFiles);
  std::sort(SeedFiles.begin(), SeedFiles.end());
  auto InitialCFPath = DirPlusFile(Env.TempDir, "merge.txt");
  CrashResistantMerge(Env.Args, {}, SeedFiles, &Env.Files, {}, &Env.Features,
                      {}, &Env.Cov, InitialCFPath, false);
  RemoveFile(InitialCFPath);

  std::vector<std::string> DiskShas;
  std::map<std::string, std::string> ShaToPath;
  for (const auto &Path : Env.Files) {
    auto Sha = Hash(FileToVector(Path));
    DiskShas.push_back(Sha);
    ShaToPath[Sha] = Path;
  }

  EpochStore Store(Options.ScheduleState, DiskShas, Params);
  EpochSchedule Schedule;
  if (Store.Exists()) {
    std::string Error;
    if (!Store.Load(&Schedule, &Error)) {
      Printf("ERROR: cannot load epoch state '%s': %s; leaving the old state "
             "untouched\n",
             Options.ScheduleState.c_str(), Error.c_str());
      exit(1);
    }
    Printf("INFO: epoch state resumed after round %zd\n", Schedule.Round());
  } else {
    for (const auto &Sha : DiskShas)
      if (!Schedule.AddRoot(Sha)) {
        Printf("ERROR: failed to register root corpus input %s\n",
               Sha.c_str());
        exit(1);
      }
    std::string Error;
    if (!Store.Save(Schedule, &Error)) {
      Printf("ERROR: cannot write epoch state: %s\n", Error.c_str());
      exit(1);
    }
  }

  JobQueue FuzzQ, MergeQ;
  Vector<std::thread> Threads;
  for (size_t T = 0; T < Slots; T++)
    Threads.push_back(std::thread(WorkerThread, &FuzzQ, &MergeQ));

  while (Schedule.Round() < Params.Rounds) {
    const size_t RoundNo = Schedule.Round();
    // Freeze the corpus available at the start of the round and assign every
    // slot before any subprocess is launched.
    std::vector<std::string> Assigned = Schedule.BeginRound(Slots);
    if (Assigned.size() != Slots) {
      Printf("ERROR: failed to begin round %zd\n", RoundNo);
      exit(1);
    }

    std::vector<std::unique_ptr<FuzzJob>> Jobs(Slots);
    for (size_t Slot = 0; Slot < Slots; Slot++) {
      auto It = ShaToPath.find(Assigned[Slot]);
      if (It == ShaToPath.end()) {
        Printf("ERROR: round %zd slot %zd: no corpus file for %s\n", RoundNo,
               Slot, Assigned[Slot].c_str());
        exit(1);
      }
      Jobs[Slot] = CreateEpochJob(Args, CorpusDirs, Env.TempDir, It->second,
                                  RoundNo, Slot, Params.RunsPerSlot);
    }
    // All seeds are assigned; only now do the workers start.
    for (auto &Job : Jobs)
      FuzzQ.Push(Job.get());

    // Subprocesses may finish in any order.  Cache them, but only merge in
    // slot order once the whole round is back.
    std::vector<FuzzJob *> Finished(Slots, nullptr);
    for (size_t Received = 0; Received < Slots; Received++) {
      FuzzJob *Done = MergeQ.Pop();
      if (!Done) {
        Printf("ERROR: epoch worker stopped unexpectedly\n");
        exit(1);
      }
      Finished[Done->JobId] = Done;
      Fuzzer::MaybeExitGracefully();
    }

    for (size_t Slot = 0; Slot < Slots; Slot++) {
      FuzzJob *Job = Finished[Slot];
      std::string Diag = EpochDiagnosticKey(Job->LogPath);
      if (Job->ExitCode != 0) {
        if (Diag.empty()) {
          // No classification key: the slot is still completed, but it must
          // not be treated as a success and contributes nothing.
          if (!Schedule.Fail(Slot)) {
            Printf("ERROR: failed to record unclassified failure at round "
                   "%zd slot %zd\n",
                   RoundNo, Slot);
            exit(1);
          }
          Printf("INFO: round %zd slot %zd failed (exit %d) without a "
                 "classification key; ignored\n",
                 RoundNo, Slot, Job->ExitCode);
        } else {
          if (!Schedule.Complete(Slot, {}, Diag)) {
            Printf("ERROR: failed to record diagnostic at round %zd slot "
                   "%zd\n",
                   RoundNo, Slot);
            exit(1);
          }
          Printf("INFO: round %zd slot %zd classified failure: %s\n", RoundNo,
                 Slot, Diag.c_str());
        }
        continue;
      }

      std::vector<std::string> Children;
      EpochMergeJob(&Env, Job, &ShaToPath, &Children);
      if (!Schedule.Complete(Slot, Children, "")) {
        Printf("ERROR: failed to record round %zd slot %zd\n", RoundNo, Slot);
        exit(1);
      }
    }

    if (!Schedule.CommitRound()) {
      Printf("ERROR: failed to commit round %zd\n", RoundNo);
      exit(1);
    }
    std::string Error;
    if (!Store.Save(Schedule, &Error)) {
      Printf("ERROR: cannot persist epoch state: %s\n", Error.c_str());
      exit(1);
    }
    Printf("INFO: committed epoch round %zd/%zd\n", Schedule.Round(),
           Params.Rounds);

    // Jobs (and their scratch directories) are released after the commit.
    for (auto &Job : Jobs)
      Job.reset();
  }

  for (size_t T = 0; T < Slots; T++)
    FuzzQ.Push(nullptr);
  MergeQ.Push(nullptr);
  for (auto &T : Threads)
    T.join();
  RmDirRecursive(Env.TempDir);

  Printf("INFO: epoch replay finished after %zd rounds\n", Schedule.Round());
  exit(0);
}

// This is just a skeleton of an experimental -fork=1 feature.
void FuzzWithFork(Random &Rand, const FuzzingOptions &Options,
                  const Vector<std::string> &Args,
                  const Vector<std::string> &CorpusDirs, int NumJobs) {
  if (Options.ScheduleRounds > 0 || Options.ScheduleRuns > 0 ||
      !Options.ScheduleState.empty())
    EpochFuzzWithFork(Options, Args, CorpusDirs, NumJobs);

  Printf("INFO: -fork=%d: fuzzing in separate process(s)\n", NumJobs);

  GlobalEnv Env;
  Env.Args = Args;
  Env.CorpusDirs = CorpusDirs;
  Env.Rand = &Rand;
  Env.Verbosity = Options.Verbosity;
  Env.ProcessStartTime = std::chrono::system_clock::now();
  Env.DataFlowBinary = Options.CollectDataFlow;

  Vector<SizedFile> SeedFiles;
  for (auto &Dir : CorpusDirs)
    GetSizedFilesFromDir(Dir, &SeedFiles);
  std::sort(SeedFiles.begin(), SeedFiles.end());
  Env.TempDir = TempPath(".dir");
  Env.DFTDir = DirPlusFile(Env.TempDir, "DFT");
  RmDirRecursive(Env.TempDir);  // in case there is a leftover from old runs.
  MkDir(Env.TempDir);
  MkDir(Env.DFTDir);


  if (CorpusDirs.empty())
    MkDir(Env.MainCorpusDir = DirPlusFile(Env.TempDir, "C"));
  else
    Env.MainCorpusDir = CorpusDirs[0];

  auto CFPath = DirPlusFile(Env.TempDir, "merge.txt");
  CrashResistantMerge(Env.Args, {}, SeedFiles, &Env.Files, {}, &Env.Features,
                      {}, &Env.Cov,
                      CFPath, false);
  RemoveFile(CFPath);
  Printf("INFO: -fork=%d: %zd seed inputs, starting to fuzz in %s\n", NumJobs,
         Env.Files.size(), Env.TempDir.c_str());

  int ExitCode = 0;

  JobQueue FuzzQ, MergeQ;

  auto StopJobs = [&]() {
    for (int i = 0; i < NumJobs; i++)
      FuzzQ.Push(nullptr);
    MergeQ.Push(nullptr);
    WriteToFile(Unit({1}), Env.StopFile());
  };

  size_t JobId = 1;
  Vector<std::thread> Threads;
  for (int t = 0; t < NumJobs; t++) {
    Threads.push_back(std::thread(WorkerThread, &FuzzQ, &MergeQ));
    FuzzQ.Push(Env.CreateNewJob(JobId++));
  }

  while (true) {
    std::unique_ptr<FuzzJob> Job(MergeQ.Pop());
    if (!Job)
      break;
    ExitCode = Job->ExitCode;
    if (ExitCode == Options.InterruptExitCode) {
      Printf("==%lu== libFuzzer: a child was interrupted; exiting\n", GetPid());
      StopJobs();
      break;
    }
    Fuzzer::MaybeExitGracefully();

    Env.RunOneMergeJob(Job.get());

    // Continue if our crash is one of the ignorred ones.
    if (Options.IgnoreTimeouts && ExitCode == Options.TimeoutExitCode)
      Env.NumTimeouts++;
    else if (Options.IgnoreOOMs && ExitCode == Options.OOMExitCode)
      Env.NumOOMs++;
    else if (ExitCode != 0) {
      Env.NumCrashes++;
      if (Options.IgnoreCrashes) {
        std::ifstream In(Job->LogPath);
        std::string Line;
        while (std::getline(In, Line, '\n'))
          if (Line.find("ERROR:") != Line.npos ||
              Line.find("runtime error:") != Line.npos)
            Printf("%s\n", Line.c_str());
      } else {
        // And exit if we don't ignore this crash.
        Printf("INFO: log from the inner process:\n%s",
               FileToString(Job->LogPath).c_str());
        StopJobs();
        break;
      }
    }

    // Stop if we are over the time budget.
    // This is not precise, since other threads are still running
    // and we will wait while joining them.
    // We also don't stop instantly: other jobs need to finish.
    if (Options.MaxTotalTimeSec > 0 &&
        Env.secondsSinceProcessStartUp() >= (size_t)Options.MaxTotalTimeSec) {
      Printf("INFO: fuzzed for %zd seconds, wrapping up soon\n",
             Env.secondsSinceProcessStartUp());
      StopJobs();
      break;
    }
    if (Env.NumRuns >= Options.MaxNumberOfRuns) {
      Printf("INFO: fuzzed for %zd iterations, wrapping up soon\n",
             Env.NumRuns);
      StopJobs();
      break;
    }

    FuzzQ.Push(Env.CreateNewJob(JobId++));
  }

  for (auto &T : Threads)
    T.join();

  // The workers have terminated. Don't try to remove the directory before they
  // terminate to avoid a race condition preventing cleanup on Windows.
  RmDirRecursive(Env.TempDir);

  // Use the exit code from the last child process.
  Printf("INFO: exiting: %d time: %zds\n", ExitCode,
         Env.secondsSinceProcessStartUp());
  exit(ExitCode);
}

} // namespace fuzzer
