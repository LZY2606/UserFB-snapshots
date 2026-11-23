#include "lib/fuzzer/FuzzerEpoch.h"
#include <cassert>
#include <string>
#include <vector>
int main() {
  fuzzer::EpochSchedule s;
  const std::string a(40,'a'), b(40,'b');
  assert(s.Round()==0);
  assert(s.AddRoot(b)); assert(s.AddRoot(a)); assert(!s.AddRoot(a));
  const auto r=s.BeginRound(2);
  assert((r==std::vector<std::string>{a,b}));
  assert(s.Complete(1,{},"")); assert(!s.CommitRound());
  assert(s.Complete(0,{},"")); assert(!s.Complete(0,{},""));
  assert(s.CommitRound()); assert(s.Round()==1);
}
