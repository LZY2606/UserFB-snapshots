/* SPDX-License-Identifier: MIT; normal public encoder/decoder smoke only. */
#include "xvidcore/src/xvid.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define W 128
#define H 96
#define N 8
#define CHECK(x) do { if(!(x)){fprintf(stderr,"baseline:%d %s\n",__LINE__,#x);return 1;} }while(0)
static unsigned char input[W*H*3/2],output[W*H*3/2],bits[W*H*8+64];
static int clamp(int x,int max){return x<0?0:x>=max?max-1:x;}
static void fill(int f){
  for(int c=0;c<3;c++){
    int w=c?W/2:W,h=c?H/2:H,off=c?W*H+(c-1)*W*H/4:0;
    for(int y=0;y<h;y++)for(int x=0;x<w;x++){
      int a=clamp(w/2+(x-w/2)*100/(100+f*3),w);
      int b=clamp(h/2+(y-h/2)*100/(100+f*3),h);
      input[off+y*w+x]=(unsigned char)((a*7+b*11+((a/5)^(b/7))*31+c*53)&255);
    }
  }
}
int main(void){
  xvid_gbl_init_t g={0};g.version=XVID_VERSION;g.cpu_flags=XVID_CPU_FORCE;
  CHECK(xvid_global(NULL,XVID_GBL_INIT,&g,NULL)==0);
  xvid_enc_create_t ec={0};ec.version=XVID_VERSION;ec.width=W;ec.height=H;
  ec.profile=XVID_PROFILE_AS_L4;ec.fincr=1;ec.fbase=25;ec.max_key_interval=100;
  ec.max_bframes=0;ec.num_threads=1;
  CHECK(xvid_encore(NULL,XVID_ENC_CREATE,&ec,NULL)==0);
  xvid_dec_create_t dc={0};dc.version=XVID_VERSION;dc.width=W;dc.height=H;
  CHECK(xvid_decore(NULL,XVID_DEC_CREATE,&dc,NULL)==0);
  int encoded=0,decoded=0,sframes=0;uint64_t hash=14695981039346656037ULL;
  FILE *file=fopen(".task-build/baseline.m4v","wb");CHECK(file);
  for(int i=0;i<N;i++){
    fill(i);xvid_enc_frame_t fr={0};xvid_enc_stats_t st={0};
    fr.version=st.version=XVID_VERSION;fr.vol_flags=XVID_VOL_GMC;
    fr.vop_flags=XVID_VOP_HALFPEL|XVID_VOP_INTER4V|XVID_VOP_HQACPRED;
    fr.motion=XVID_ME_ADVANCEDDIAMOND16|XVID_ME_HALFPELREFINE16|XVID_ME_GME_REFINE;
    fr.input.csp=XVID_CSP_I420;fr.input.plane[0]=input;fr.input.stride[0]=W;
    fr.type=i?XVID_TYPE_PVOP:XVID_TYPE_IVOP;fr.quant=4;fr.bitstream=bits;fr.length=sizeof(bits)-64;
    int n=xvid_encore(ec.handle,XVID_ENC_ENCODE,&fr,&st);CHECK(n>0);
    CHECK(fwrite(bits,1,n,file)==(size_t)n);memset(bits+n,0,64);encoded++;
    printf("ENC frame=%d type=%d bytes=%d\n",i,st.type,n);
    int pos=0,loops=0;
    while(pos<n){
      CHECK(++loops<8);xvid_dec_frame_t df={0};xvid_dec_stats_t ds={0};
      df.version=ds.version=XVID_VERSION;df.general=XVID_LOWDELAY;
      df.bitstream=bits+pos;df.length=n-pos;df.output.csp=XVID_CSP_I420;
      df.output.plane[0]=output;df.output.stride[0]=W;
      int used=xvid_decore(dc.handle,XVID_DEC_DECODE,&df,&ds);CHECK(used>0 && used<=n-pos);pos+=used;
      if(ds.type>0){decoded++;sframes+=ds.type==XVID_TYPE_SVOP;
        for(size_t k=0;k<sizeof(output);k++){hash^=output[k];hash*=1099511628211ULL;}
      }
    }
  }
  CHECK(fclose(file)==0);CHECK(encoded==N && decoded==N && sframes>0);
  CHECK(xvid_encore(ec.handle,XVID_ENC_DESTROY,NULL,NULL)==0);
  CHECK(xvid_decore(dc.handle,XVID_DEC_DESTROY,NULL,NULL)==0);
  printf("BASELINE_COMPLETE encoded=%d decoded=%d S=%d pixels=%d hash=%016llx\n",encoded,decoded,sframes,N*W*H*3/2,(unsigned long long)hash);
  return 0;
}
