// A finite, benign coverage fixture. Every input succeeds; no input parser.
#include <cstddef>
#include <cstdint>
static volatile uint32_t observed;
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if(size && data[0]=='A') observed+=1;
  if(size>1 && data[1]=='B') observed+=2;
  if(size>2 && data[2]=='C') observed+=3;
  return 0;
}
