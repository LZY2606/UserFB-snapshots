#include "harness.h"
unsigned uf_assertions=0;
void uf_numeric();
void uf_voices();
void uf_native();
int main(){
  uf_assertions=0; uf_numeric(); if (!uf_assertions) { std::fprintf(stderr,"EMPTY numeric\n"); return 3; } std::printf("GROUP numeric assertions=%u PASS\n",uf_assertions);
  uf_assertions=0; uf_voices(); if (!uf_assertions) { std::fprintf(stderr,"EMPTY voices\n"); return 3; } std::printf("GROUP voices assertions=%u PASS\n",uf_assertions);
  uf_assertions=0; uf_native(); if (!uf_assertions) { std::fprintf(stderr,"EMPTY native\n"); return 3; } std::printf("GROUP native assertions=%u PASS\n",uf_assertions);
  std::puts("TARGET_GROUPS_OK");
}
