#include "harness.h"
unsigned uf_assertions=0;
void uf_metadata();
void uf_sustain();
void uf_storage();
int main(){
  uf_assertions=0; uf_metadata(); if (!uf_assertions) { std::fprintf(stderr,"EMPTY metadata\n"); return 3; } std::printf("GROUP metadata assertions=%u PASS\n",uf_assertions);
  uf_assertions=0; uf_sustain(); if (!uf_assertions) { std::fprintf(stderr,"EMPTY sustain\n"); return 3; } std::printf("GROUP sustain assertions=%u PASS\n",uf_assertions);
  uf_assertions=0; uf_storage(); if (!uf_assertions) { std::fprintf(stderr,"EMPTY storage\n"); return 3; } std::printf("GROUP storage assertions=%u PASS\n",uf_assertions);
  std::puts("TARGET_GROUPS_OK");
}
