#include "harness.h"
unsigned uf_assertions=0;
void uf_integral();
void uf_modulation();
void uf_compatibility();
int main(){
  uf_assertions=0; uf_integral(); if (!uf_assertions) { std::fprintf(stderr,"EMPTY integral\n"); return 3; } std::printf("GROUP integral assertions=%u PASS\n",uf_assertions);
  uf_assertions=0; uf_modulation(); if (!uf_assertions) { std::fprintf(stderr,"EMPTY modulation\n"); return 3; } std::printf("GROUP modulation assertions=%u PASS\n",uf_assertions);
  uf_assertions=0; uf_compatibility(); if (!uf_assertions) { std::fprintf(stderr,"EMPTY compatibility\n"); return 3; } std::printf("GROUP compatibility assertions=%u PASS\n",uf_assertions);
  std::puts("TARGET_GROUPS_OK");
}
