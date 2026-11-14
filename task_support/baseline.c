#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "common/fd.h"
#include "common/fuzz.h"
static unsigned char whole[3073], split[3073], orig[3073], mask[3073];
int main(void) {
    _zz_fd_init();zzuf_set_seed(1812);zzuf_set_ratio(.013,.013);
    _zz_register(4);_zz_register(5);_zz_register(6);
    for(size_t i=0;i<sizeof orig;i++)orig[i]=(unsigned char)(i*19+3);
    memcpy(whole,orig,sizeof orig);memcpy(split,orig,sizeof orig);
    _zz_fuzzing("xor");_zz_fuzz(4,whole,sizeof whole);
    _zz_setpos(5,0);_zz_fuzz(5,split,1009);
    _zz_setpos(5,1009);_zz_fuzz(5,split+1009,sizeof split-1009);
    if(memcmp(whole,split,sizeof whole))return 2;
    _zz_fuzz(6,mask,sizeof mask);
    size_t changed=0;
    for(size_t i=0;i<sizeof orig;i++) { if(whole[i]!=(unsigned char)(orig[i]^mask[i]))return 3; changed+=whole[i]!=orig[i]; }
    if(!changed)return 4;
    _zz_unregister(4);_zz_unregister(5);_zz_unregister(6);
    FILE *f=fopen(".task-build/baseline-input.bin","wb");if(!f)return 5;
    if(fwrite(orig,1,sizeof orig,f)!=sizeof orig)return 6;fclose(f);
    f=fopen(".task-build/baseline-expect.bin","wb");if(!f)return 7;
    if(fwrite(whole,1,sizeof whole,f)!=sizeof whole)return 8;fclose(f);
    printf("ZZUF_BASELINE_OK bytes=%zu split=1009 changed=%zu\n",sizeof orig,changed);return 0;
}
