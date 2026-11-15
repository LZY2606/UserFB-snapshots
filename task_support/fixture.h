#ifndef TASK_IT_FIXTURE_H
#define TASK_IT_FIXTURE_H
/* A normal signed, uncompressed mono IT sample and one ordinary pattern.
   Author material only: no proposed loop processing or reference algorithm. */
#include <stddef.h>
#include <string.h>
#include <stdint.h>
static void fixture_u16(unsigned char *p, unsigned n){p[0]=(unsigned char)n;p[1]=(unsigned char)(n>>8);}
static void fixture_u32(unsigned char *p, unsigned n){fixture_u16(p,n);fixture_u16(p+2,n>>16);}
static size_t fixture_it(unsigned char *b, size_t cap, int release_row) {
    enum {HEADER=192,SAMPLE=208,PATTERN=288,DATA=512,LENGTH=256};
    unsigned row, pos=PATTERN+8, packed;
    if(cap<DATA+LENGTH || release_row>31)return 0;
    memset(b,0,DATA+LENGTH);
    memcpy(b,"IMPM",4);memcpy(b+4,"ordinary sustain",16);
    fixture_u16(b+32,2); /* orders: pattern 0 then end */
    fixture_u16(b+36,1);fixture_u16(b+38,1);
    fixture_u16(b+40,0x214);fixture_u16(b+42,0x214);
    fixture_u16(b+44,1); /* stereo song, sample mode */
    b[48]=128;b[49]=48;b[50]=1;b[51]=125;b[52]=128;
    memset(b+64,32,64);memset(b+128,64,64);
    b[192]=0;b[193]=255;
    fixture_u32(b+194,SAMPLE);fixture_u32(b+198,PATTERN);
    memcpy(b+SAMPLE,"IMPS",4);b[SAMPLE+17]=64;
    b[SAMPLE+18]=1|32; /* exists, forward sustain; no regular loop */
    b[SAMPLE+19]=64;b[SAMPLE+46]=1;b[SAMPLE+47]=32|128;
    fixture_u32(b+SAMPLE+48,LENGTH);fixture_u32(b+SAMPLE+60,6000);
    fixture_u32(b+SAMPLE+64,32);fixture_u32(b+SAMPLE+68,160);
    fixture_u32(b+SAMPLE+72,DATA);
    for(row=0;row<32;row++) {
        if(row==0){b[pos++]=0x81;b[pos++]=3;b[pos++]=60;b[pos++]=1;}
        else if((int)row==release_row){b[pos++]=0x81;b[pos++]=1;b[pos++]=255;}
        b[pos++]=0;
    }
    packed=pos-(PATTERN+8);fixture_u16(b+PATTERN,packed);fixture_u16(b+PATTERN+2,32);
    for(row=0;row<LENGTH;row++) {
        int n=(int)((row*37u+row*row*3u)%151u)-75;
        b[DATA+row]=(unsigned char)(signed char)n;
    }
    return DATA+LENGTH;
}
#endif
