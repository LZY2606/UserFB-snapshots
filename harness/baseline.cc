#include <asmjit/core.h>
#include <cassert>
#include <cstring>
#include <cstdio>
using namespace asmjit;
int main() {
 JitAllocator a; JitAllocator::Span s,q;
 size_t g=a.granularity();
 assert(a.alloc(Out(s),g*3)==Error::kOk);
 unsigned char bytes[4]={11,22,33,44};
 assert(a.write(s,0,bytes,sizeof(bytes))==Error::kOk);
 assert(std::memcmp(s.rx(),bytes,sizeof(bytes))==0);
 void *inner=static_cast<unsigned char*>(s.rx())+g;
 assert(a.query(Out(q),inner)==Error::kOk);
 assert(q.rx()==inner && q.size()==s.size()-g);
 assert(a.release(s.rx())==Error::kOk);
 std::puts("ASMJIT_BASELINE_OK interior-query-is-suffix write release");
}
