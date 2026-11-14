// Selected unmodified upstream tests; Apache-2.0 WITH LLVM-exception.
// Source: lib/fuzzer/tests/FuzzerUnittest.cpp, Corpus.Distribution and Merge.*.
#include "FuzzerCorpus.h"
#include "FuzzerMerge.h"
#include <memory>
#include <set>
#include <sstream>
#include <cstdio>
#include <cstdlib>
using namespace fuzzer;
#define TEST(s,n) static void s##_##n()
#define EXPECT_EQ(a,b) do { if(!((a)==(b))) {fprintf(stderr,"baseline eq at %d\n",__LINE__);exit(1);} } while(0)
#define EXPECT_GT(a,b) do { if(!((a)>(b))) {fprintf(stderr,"baseline gt at %d\n",__LINE__);exit(1);} } while(0)
#define EXPECT_TRUE(a) EXPECT_EQ(!!(a),true)
#define EXPECT_FALSE(a) EXPECT_EQ(!!(a),false)
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *, size_t) { return 0; }
TEST(Corpus, Distribution) {
  DataFlowTrace DFT;
  Random Rand(0);
  std::unique_ptr<InputCorpus> C(new InputCorpus(""));
  size_t N = 10;
  size_t TriesPerUnit = 1<<16;
  for (size_t i = 0; i < N; i++)
    C->AddToCorpus(Unit{static_cast<uint8_t>(i)}, 1, false, false, {}, DFT,
                   nullptr);

  Vector<size_t> Hist(N);
  for (size_t i = 0; i < N * TriesPerUnit; i++) {
    Hist[C->ChooseUnitIdxToMutate(Rand)]++;
  }
  for (size_t i = 0; i < N; i++) {
    // A weak sanity check that every unit gets invoked.
    EXPECT_GT(Hist[i], TriesPerUnit / N / 3);
  }
}

TEST(Merge, Bad) {
  const char *kInvalidInputs[] = {
    "",
    "x",
    "3\nx",
    "2\n3",
    "2\n2",
    "2\n2\nA\n",
    "2\n2\nA\nB\nC\n",
    "0\n0\n",
    "1\n1\nA\nFT 0",
    "1\n1\nA\nSTARTED 1",
  };
  Merger M;
  for (auto S : kInvalidInputs) {
    // fprintf(stderr, "TESTING:\n%s\n", S);
    EXPECT_FALSE(M.Parse(S, false));
  }
}

void EQ(const Vector<uint32_t> &A, const Vector<uint32_t> &B) {
  EXPECT_EQ(A, B);
}

void EQ(const Vector<std::string> &A, const Vector<std::string> &B) {
  Set<std::string> a(A.begin(), A.end());
  Set<std::string> b(B.begin(), B.end());
  EXPECT_EQ(a, b);
}

static void Merge(const std::string &Input,
                  const Vector<std::string> Result,
                  size_t NumNewFeatures) {
  Merger M;
  Vector<std::string> NewFiles;
  Set<uint32_t> NewFeatures, NewCov;
  EXPECT_TRUE(M.Parse(Input, true));
  EXPECT_EQ(NumNewFeatures, M.Merge({}, &NewFeatures, {}, &NewCov, &NewFiles));
  EQ(NewFiles, Result);
}

TEST(Merge, Good) {
  Merger M;

  EXPECT_TRUE(M.Parse("1\n0\nAA\n", false));
  EXPECT_EQ(M.Files.size(), 1U);
  EXPECT_EQ(M.NumFilesInFirstCorpus, 0U);
  EXPECT_EQ(M.Files[0].Name, "AA");
  EXPECT_TRUE(M.LastFailure.empty());
  EXPECT_EQ(M.FirstNotProcessedFile, 0U);

  EXPECT_TRUE(M.Parse("2\n1\nAA\nBB\nSTARTED 0 42\n", false));
  EXPECT_EQ(M.Files.size(), 2U);
  EXPECT_EQ(M.NumFilesInFirstCorpus, 1U);
  EXPECT_EQ(M.Files[0].Name, "AA");
  EXPECT_EQ(M.Files[1].Name, "BB");
  EXPECT_EQ(M.LastFailure, "AA");
  EXPECT_EQ(M.FirstNotProcessedFile, 1U);

  EXPECT_TRUE(M.Parse("3\n1\nAA\nBB\nC\n"
                        "STARTED 0 1000\n"
                        "FT 0 1 2 3\n"
                        "STARTED 1 1001\n"
                        "FT 1 4 5 6 \n"
                        "STARTED 2 1002\n"
                        "", true));
  EXPECT_EQ(M.Files.size(), 3U);
  EXPECT_EQ(M.NumFilesInFirstCorpus, 1U);
  EXPECT_EQ(M.Files[0].Name, "AA");
  EXPECT_EQ(M.Files[0].Size, 1000U);
  EXPECT_EQ(M.Files[1].Name, "BB");
  EXPECT_EQ(M.Files[1].Size, 1001U);
  EXPECT_EQ(M.Files[2].Name, "C");
  EXPECT_EQ(M.Files[2].Size, 1002U);
  EXPECT_EQ(M.LastFailure, "C");
  EXPECT_EQ(M.FirstNotProcessedFile, 3U);
  EQ(M.Files[0].Features, {1, 2, 3});
  EQ(M.Files[1].Features, {4, 5, 6});


  Vector<std::string> NewFiles;
  Set<uint32_t> NewFeatures, NewCov;

  EXPECT_TRUE(M.Parse("3\n2\nAA\nBB\nC\n"
                        "STARTED 0 1000\nFT 0 1 2 3\n"
                        "STARTED 1 1001\nFT 1 4 5 6 \n"
                        "STARTED 2 1002\nFT 2 6 1 3 \n"
                        "", true));
  EXPECT_EQ(M.Files.size(), 3U);
  EXPECT_EQ(M.NumFilesInFirstCorpus, 2U);
  EXPECT_TRUE(M.LastFailure.empty());
  EXPECT_EQ(M.FirstNotProcessedFile, 3U);
  EQ(M.Files[0].Features, {1, 2, 3});
  EQ(M.Files[1].Features, {4, 5, 6});
  EQ(M.Files[2].Features, {1, 3, 6});
  EXPECT_EQ(0U, M.Merge({}, &NewFeatures, {}, &NewCov, &NewFiles));
  EQ(NewFiles, {});

  EXPECT_TRUE(M.Parse("3\n1\nA\nB\nC\n"
                        "STARTED 0 1000\nFT 0 1 2 3\n"
                        "STARTED 1 1001\nFT 1 4 5 6 \n"
                        "STARTED 2 1002\nFT 2 6 1 3\n"
                        "", true));
  EQ(M.Files[0].Features, {1, 2, 3});
  EQ(M.Files[1].Features, {4, 5, 6});
  EQ(M.Files[2].Features, {1, 3, 6});
  EXPECT_EQ(3U, M.Merge({}, &NewFeatures, {}, &NewCov, &NewFiles));
  EQ(NewFiles, {"B"});

  // Same as the above, but with InitialFeatures.
  EXPECT_TRUE(M.Parse("2\n0\nB\nC\n"
                        "STARTED 0 1001\nFT 0 4 5 6 \n"
                        "STARTED 1 1002\nFT 1 6 1 3\n"
                        "", true));
  EQ(M.Files[0].Features, {4, 5, 6});
  EQ(M.Files[1].Features, {1, 3, 6});
  Set<uint32_t> InitialFeatures;
  InitialFeatures.insert(1);
  InitialFeatures.insert(2);
  InitialFeatures.insert(3);
  EXPECT_EQ(3U, M.Merge(InitialFeatures, &NewFeatures, {}, &NewCov, &NewFiles));
  EQ(NewFiles, {"B"});
}

TEST(Merge, Merge) {

  Merge("3\n1\nA\nB\nC\n"
        "STARTED 0 1000\nFT 0 1 2 3\n"
        "STARTED 1 1001\nFT 1 4 5 6 \n"
        "STARTED 2 1002\nFT 2 6 1 3 \n",
        {"B"}, 3);

  Merge("3\n0\nA\nB\nC\n"
        "STARTED 0 2000\nFT 0 1 2 3\n"
        "STARTED 1 1001\nFT 1 4 5 6 \n"
        "STARTED 2 1002\nFT 2 6 1 3 \n",
        {"A", "B", "C"}, 6);

  Merge("4\n0\nA\nB\nC\nD\n"
        "STARTED 0 2000\nFT 0 1 2 3\n"
        "STARTED 1 1101\nFT 1 4 5 6 \n"
        "STARTED 2 1102\nFT 2 6 1 3 100 \n"
        "STARTED 3 1000\nFT 3 1  \n",
        {"A", "B", "C", "D"}, 7);

  Merge("4\n1\nA\nB\nC\nD\n"
        "STARTED 0 2000\nFT 0 4 5 6 7 8\n"
        "STARTED 1 1100\nFT 1 1 2 3 \n"
        "STARTED 2 1100\nFT 2 2 3 \n"
        "STARTED 3 1000\nFT 3 1  \n",
        {"B", "D"}, 3);
}


int main(){Corpus_Distribution();Merge_Bad();Merge_Good();Merge_Merge();puts("UPSTREAM_CORPUS_MERGE_COMPLETE tests=4");}
