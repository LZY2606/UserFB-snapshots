#include <cstddef>
#include <cstdint>
#include <cstdio>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *p, size_t n) {
  if (n && p[0] == 'A') std::puts("BENIGN_A");
  else std::puts("BENIGN_OTHER");
  return 0;
}
