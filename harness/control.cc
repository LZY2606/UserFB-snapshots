#include <asmjit/core.h>
#include <cassert>
#include <cstdio>
using namespace asmjit;
int main() {
 JitAllocator a; JitAllocator::Span s,q;
 assert(a.alloc(Out(s),256)==Error::kOk);
 assert(a.query(Out(q),s.rx())==Error::kOk && q.rx()==s.rx() && q.size()==s.size());
 unsigned char bytes[4]={1,2,3,4};
 assert(a.write(s,0,bytes,sizeof(bytes))==Error::kOk);
 assert(a.shrink(s,64)==Error::kOk && s.size()==64);
 assert(a.release(s.rx())==Error::kOk);
 assert(a.statistics().allocation_count()==0);
 std::puts("ASMJIT_CONTROL_OK allocation write shrink statistics");
}
