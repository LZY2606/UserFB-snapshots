#include "harness.h"
unsigned uf_assertions=0;
void uf_noise_clock(void);
void uf_percussion(void);
void uf_render(void);
int main(void){
  uf_assertions=0; uf_noise_clock(); if (!uf_assertions) { fprintf(stderr,"EMPTY noise_clock\n"); return 3; } printf("GROUP noise_clock assertions=%u PASS\n",uf_assertions);
  uf_assertions=0; uf_percussion(); if (!uf_assertions) { fprintf(stderr,"EMPTY percussion\n"); return 3; } printf("GROUP percussion assertions=%u PASS\n",uf_assertions);
  uf_assertions=0; uf_render(); if (!uf_assertions) { fprintf(stderr,"EMPTY render\n"); return 3; } printf("GROUP render assertions=%u PASS\n",uf_assertions);
  puts("TARGET_GROUPS_OK");
}
