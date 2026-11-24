#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ixheaac_type_def.h"
#include "ixheaace_api.h"
static void *alloc_audio(UWORD32 n,UWORD32 a) {
  void *p=0; if(a<sizeof(void*))a=sizeof(void*);
  return posix_memalign(&p,a,n)?NULL:p;
}
int main(void) {
  ixheaace_input_config in={0}; ixheaace_output_config out={0};
  in.ui_pcm_wd_sz=16; in.i_bitrate=96000; in.aot=AOT_AAC_LC;
  in.i_channels=2; in.i_samp_freq=44100; in.i_use_es=1;
  in.frame_length=1024; in.frame_cmd_flag=1;
  in.aac_config.bitrate=96000; in.aac_config.inv_quant=2;
  in.aac_config.bitreservoir_size=768; in.aac_config.length=64*1024;
  out.malloc_xheaace=alloc_audio; out.free_xheaace=free;
  int err=ixheaace_create(&in,&out);
  if(err) {fprintf(stderr,"create=%08x\n",err);return 1;}
  unsigned rng=12345; int frames=0,min=100000,max=0; long bytes=0;
  for(int f=0;f<64;f++) {
    int16_t *p=out.mem_info_table[IA_MEMTYPE_INPUT].mem_ptr;
    for(int i=0;i<out.input_size/2;i++) {
      rng=rng*1664525u+1013904223u;
      p[i]=(f<16)?0:(int16_t)(rng>>18);
    }
    err=ixheaace_process(out.pv_ia_process_api_obj,&in,&out);
    if(err) {fprintf(stderr,"process[%d]=%08x\n",f,err);ixheaace_delete(&out);return 2;}
    if(out.i_out_bytes<=0)return 3;
    frames++; bytes+=out.i_out_bytes;
    if(out.i_out_bytes<min)min=out.i_out_bytes;
    if(out.i_out_bytes>max)max=out.i_out_bytes;
  }
  ixheaace_delete(&out);
  printf("BASELINE_OK frames=%d bytes=%ld min_au=%d max_au=%d\n",frames,bytes,min,max);
  puts("CURRENT_CAPABILITY fixed_reservoir_no_per_AU_runtime_limit");
  return frames==64&&max>min?0:4;
}
