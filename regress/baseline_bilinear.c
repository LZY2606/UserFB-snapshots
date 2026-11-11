#include <stdio.h>
#define OLIVEC_IMPLEMENTATION
#include "olive.c"
int main(void) {
    uint32_t src[6]={0xff000000,0xff000064,0xdeadbeef,0xff0000c8,0xff0000ff,0xdeadbeef};
    uint32_t pos[24],neg[24];
    for(int i=0;i<24;i++)pos[i]=neg[i]=0x12345678;
    Olivec_Canvas s=olivec_canvas(src,2,2,3);
    olivec_sprite_copy_bilinear(olivec_canvas(pos,4,4,6),0,0,4,4,s);
    olivec_sprite_copy_bilinear(olivec_canvas(neg,4,4,6),3,3,-4,-4,s);
    printf("positive red rows:");
    for(int y=0;y<4;y++){printf(" [");for(int x=0;x<4;x++)printf("%u,",OLIVEC_RED(pos[y*6+x]));printf("]");}
    puts("");
    // Negative w/h must render the pixel-for-pixel mirror of the positive result.
    for(int y=0;y<4;y++){
        for(int x=0;x<4;x++){
            if(neg[y*6+x]!=pos[(3-y)*6+(3-x)])return 1;
        }
    }
    for(int i=0;i<24;i++){
        int in_stride_pad=(i%6)>=4;
        if(in_stride_pad && (pos[i]!=0x12345678 || neg[i]!=0x12345678))return 2;
    }
    if(src[2]!=0xdeadbeef || src[5]!=0xdeadbeef)return 3;
    puts("BASELINE: positive stride rendering works; negative dimensions mirror it");
    return 0;
}
