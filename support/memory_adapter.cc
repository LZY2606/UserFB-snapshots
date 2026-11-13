// Offline code-generation adapter: buffers are data, never executed.
#include "PlatformUnifiedInterface/platform.h"
#include <cstdlib>
#include <cstring>
int OSMemory::PageSize() { return 65536; }
void *OSMemory::Allocate(size_t n, MemoryPermission) { return std::calloc(1,n); }
void *OSMemory::Allocate(size_t n, MemoryPermission p, void *) { return Allocate(n,p); }
bool OSMemory::SetPermission(void *, size_t, MemoryPermission) { return true; }
extern "C" int DobbyCodePatch(void *p, uint8_t *b, uint32_t n) { std::memcpy(p,b,n); return 0; }
