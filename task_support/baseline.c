#include "srla_encoder.h"
#include "srla_decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    enum {N=1024, CAP=65536};
    int32_t input_data[N], output_data[N];
    const int32_t *input[]={input_data};int32_t *output[]={output_data};
    uint8_t encoded[CAP];uint32_t bytes=0,estimate=0,consumed=0,decoded=0,i;
    struct SRLAEncoderConfig cfg={1,256,N,N,32};
    struct SRLAEncodeParameter par={1,16,44100,256,N,N,1,0,0};
    struct SRLADecoderConfig dc={1,32,1};
    struct SRLAHeader h={SRLA_FORMAT_VERSION,SRLA_CODEC_VERSION,1,N,44100,16,0,N,0};
    struct SRLAEncoder *e=SRLAEncoder_Create(&cfg,NULL,0);
    struct SRLADecoder *d=SRLADecoder_Create(&dc,NULL,0);
    if(!e||!d)return 1;
    for(i=0;i<N;i++)input_data[i]=((int)(i%31)-15)*400+((i/31)%3)*17;
    if(SRLAEncoder_SetEncodeParameter(e,&par)||SRLADecoder_SetHeader(d,&h))return 2;
    if(SRLAEncoder_ComputeBlockSize(e,input,N,&estimate))return 3;
    if(SRLAEncoder_EncodeBlock(e,input,N,encoded,sizeof encoded,&bytes))return 4;
    if(SRLADecoder_DecodeBlock(d,encoded,bytes,output,1,N,&consumed,&decoded))return 5;
    if(decoded!=N||consumed!=bytes||estimate!=bytes||memcmp(input_data,output_data,sizeof input_data))return 6;
    FILE *f=fopen(".task-build/legacy.block","wb");
    if(!f||fwrite(encoded,1,bytes,f)!=bytes||fclose(f))return 7;
    printf("LEGACY samples=%u bytes=%u estimate=%u PCM_EXACT=1\n",decoded,bytes,estimate);
    puts("BASELINE_OK");SRLADecoder_Destroy(d);SRLAEncoder_Destroy(e);return 0;
}
