#include "sbc.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
int main(void) {
    sbc_t enc, dec;
    struct sbc_frame f = {.msbc = true}, info = {0};
    sbc_reset(&enc); sbc_reset(&dec);
    uint8_t packets[6][SBC_MSBC_SIZE];
    int16_t pcm[120], out[120];
    long long good_energy=0, lost_energy=0;
    for (int j=0;j<6;j++) {
        for (int i=0;i<120;i++) pcm[i]=(int16_t)((((i+j*120)*397)%20001)-10000);
        if (sbc_encode(&enc,pcm,1,NULL,0,&f,packets[j],sizeof packets[j])) return 1;
        if (sbc_decode(&dec,packets[j],sizeof packets[j],&info,out,1,NULL,0)) return 2;
        if (!info.msbc || info.nblocks!=15 || info.nsubbands!=8) return 3;
        for(int i=0;i<120;i++) good_energy+=(long long)out[i]*out[i];
    }
    for(int j=0;j<5;j++) {
        if(sbc_decode(&dec,NULL,0,&info,out,1,NULL,0))return 4;
        long long energy=0;
        for(int i=0;i<120;i++)energy+=(long long)out[i]*out[i];
        printf("NULL_FRAME %d energy=%lld\n",j+1,energy);
        lost_energy+=energy;
        if(j>=1 && energy!=0)return 5;
    }
    sbc_t before=dec;
    packets[5][3]^=1;
    if(sbc_decode(&dec,packets[5],57,&info,out,1,NULL,0)!=-1)return 6;
    if(memcmp(&before,&dec,sizeof dec))return 7;
    if(good_energy<=0 || lost_energy<=0)return 8;
    printf("BASELINE_MSBC_OK frames=6 samples=720 good_energy=%lld lost_energy=%lld crc_rejected=1\n",good_energy,lost_energy);
    return 0;
}
