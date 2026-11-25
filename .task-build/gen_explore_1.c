#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "neaacdec.h"
#define HAVE_STDINT_H 1
#include "common.h"
#include "structs.h"

typedef struct { unsigned char b[1024]; size_t bits; } BW;
static void bw(BW*w,uint32_t v,int n){ for(int i=n-1;i>=0;i--){ size_t p=w->bits++; if((v>>i)&1) w->b[p/8]|=1u<<(7-p%8);} }
static size_t bytes(BW*w){ return (w->bits+7)/8; }
static size_t mono_au(BW*w,int seq,int shape,int tns){
 memset(w,0,sizeof *w);
 int shorty=seq==2;
 bw(w,0,3); bw(w,0,4);
 bw(w,160,8);
 bw(w,0,1); bw(w,seq,2); bw(w,shape,1);
 if(shorty){ bw(w,1,4); bw(w,127,7); } else { bw(w,1,6); }
 if(!shorty) bw(w,0,1);
 bw(w,13,4); bw(w,1,shorty?3:5);
 bw(w,256,9);
 bw(w,0,1);
 bw(w,tns?1:0,1);
 if(tns && !shorty){
   bw(w,1,2); bw(w,0,1); bw(w,49,6); bw(w,1,5);
   bw(w,0,1); bw(w,0,1); bw(w,2,3);
 }
 if(tns && shorty){ for(int i=0;i<8;i++) bw(w,0,1); }
 bw(w,0,1);
 bw(w,7,3);
 while(w->bits%8) bw(w,0,1);
 return bytes(w);
}
int main(void){
 NeAACDecHandle h=NeAACDecOpen(); NeAACDecConfigurationPtr c=NeAACDecGetCurrentConfiguration(h); c->outputFormat=FAAD_FMT_FLOAT; c->dontUpSampleImplicitSBR=1; NeAACDecSetConfiguration(h,c);
 unsigned char asc[2]={0x11,0x88}; unsigned long rate; unsigned char ch; char ir=NeAACDecInit2(h,asc,2,&rate,&ch); printf("init %d rate %lu ch %u\n",ir,rate,ch);
 NeAACDecStruct*x=(void*)h;
 int seqs[4]={0,1,2,3};
 for(int f=0;f<4;f++){ BW w; size_t n=mono_au(&w,seqs[f],f%2,f==0); NeAACDecFrameInfo info; float*p=NeAACDecDecode(h,&info,w.b,n); double sum=0; int nz=0; for(int i=0;i<1024;i++){sum+=fabs(x->conceal_spectrum[0][i]); if(x->conceal_spectrum[0][i]!=0)nz++;}
  printf("f%d n%zu err%u samp%lu p%p seq%u cap%u valid%u specsum %.3f nz%d r %x/%x",f,n,info.error,info.samples,(void*)p,x->conceal_history_sequence[0],x->conceal_capture_mask,x->conceal_history_valid,sum,nz,x->__r1,x->__r2);
  if(info.error){ printf(" msg %s",NeAACDecGetErrorMessage(info.error)); } printf("\n");
 }
 NeAACDecFrameInfo ci; float*q=NeAACDecConceal(h,&ci); printf("conceal err%u samp%lu q%p first %f\n",ci.error,ci.samples,(void*)q,q?q[100]:0);
 NeAACDecClose(h); return 0;
}
