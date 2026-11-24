#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "duma.h"
int main(void) {
    void *slots[512];
    duma_init();
    unsigned char *p = malloc(37);
    if (!p) return 2;
    for (size_t i=0;i<37;i++) p[i]=(unsigned char)(i+1);
    p=realloc(p,79);
    if (!p) return 3;
    for (size_t i=0;i<37;i++) if(p[i]!=(unsigned char)(i+1)) return 4;
    unsigned char *z=calloc(12,7);
    if(!z) return 5;
    for(size_t i=0;i<84;i++) if(z[i]) return 6;
    for(size_t i=0;i<512;i++) { slots[i]=malloc(17+i%11); if(!slots[i])return 7; memset(slots[i],42,17+i%11); }
    duma_check(p);duma_checkAll();
    for(size_t i=0;i<512;i++) free(slots[i]);
    free(z);free(p);
    puts("DUMA_BASELINE_OK preserve=37 zero=84 growth=512");return 0;
}
