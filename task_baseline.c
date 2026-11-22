#include "enc_if.h"
#include <voAMRWB.h>
#include <cmnMemory.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
int main(void) {
    VO_AUDIO_CODECAPI api; VO_HANDLE h=NULL;
    VO_MEM_OPERATOR mem={0}; VO_CODEC_INIT_USERDATA user={0};
    mem.Alloc=cmnMemAlloc;mem.Copy=cmnMemCopy;mem.Free=cmnMemFree;mem.Set=cmnMemSet;mem.Check=cmnMemCheck;
    user.memflag=VO_IMF_USERMEMOPERATOR;user.memData=&mem;
    if(voGetAMRWBEncAPI(&api)!=VO_ERR_NONE || api.Init(&h,VO_AUDIO_CodingAMRWB,&user)!=VO_ERR_NONE)return 1;
    int fmt=VOAMRWB_RFC3267,dtx=0;
    if(api.SetParam(h,VO_PID_AMRWB_FRAMETYPE,&fmt)!=VO_ERR_NONE || api.SetParam(h,VO_PID_AMRWB_DTX,&dtx)!=VO_ERR_NONE)return 2;
    void *whole=E_IF_init(); if(!whole)return 3;
    unsigned char a[1024],b[1024];short pcm[320];
    static const int sizes[]={18,24,33,37,41,47,51,59,61};
    uint32_t hash=2166136261u;unsigned total=0, calls=0;
    for(int f=0;f<12;f++) {
        int mode=f%9;
        for(int i=0;i<320;i++)pcm[i]=(short)(f==6?8:(((f*320+i)*113)%4001-2000));
        int n=E_IF_encode(whole,mode,pcm,a,0);
        if(n!=sizes[mode] || ((a[0]>>3)&15)!=mode)return 4;
        if(api.SetParam(h,VO_PID_AMRWB_MODE,&mode)!=VO_ERR_NONE)return 5;
        const unsigned chunks[]={1,3,635,1};unsigned offset=0,got=0;
        for(unsigned k=0;k<4;k++) {
            VO_CODECBUFFER in={0};in.Buffer=((unsigned char*)pcm)+offset;in.Length=chunks[k];
            if(api.SetInputData(h,&in)!=VO_ERR_NONE)return 6;
            for(;;) {
                VO_CODECBUFFER out={0};VO_AUDIO_OUTPUTINFO info={0};out.Buffer=b;out.Length=sizeof b;
                VO_U32 rc=api.GetOutputData(h,&out,&info);++calls;
                if(rc==VO_ERR_INPUT_BUFFER_SMALL)break;
                if(rc!=VO_ERR_NONE || k!=3 || ++got!=1 || out.Length!=(unsigned)n || memcmp(a,b,n))return 7;
            }
            offset+=chunks[k];
        }
        if(got!=1)return 8;
        for(int j=0;j<n;j++) hash=(hash^a[j])*16777619u;
        total+=(unsigned)n;
    }
    E_IF_exit(whole);api.Uninit(h);
    printf("BASELINE_AMRWB_OK frames=12 samples=3840 bytes=%u split_calls=%u homing_frame=6 hash=%08x\n",total,calls,hash);
    return 0;
}
