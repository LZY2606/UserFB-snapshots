#include "harness.h"
unsigned uf_assertions=0;
void uf_layers(void);
void uf_admission(void);
void uf_channel_audio(void);
int main(void){
  uf_assertions=0; uf_layers(); if (!uf_assertions) { fprintf(stderr,"EMPTY layers\n"); return 3; } printf("GROUP layers assertions=%u PASS\n",uf_assertions);
  uf_assertions=0; uf_admission(); if (!uf_assertions) { fprintf(stderr,"EMPTY admission\n"); return 3; } printf("GROUP admission assertions=%u PASS\n",uf_assertions);
  uf_assertions=0; uf_channel_audio(); if (!uf_assertions) { fprintf(stderr,"EMPTY channel_audio\n"); return 3; } printf("GROUP channel_audio assertions=%u PASS\n",uf_assertions);
  puts("TARGET_GROUPS_OK");
}
