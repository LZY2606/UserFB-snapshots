#include "adlmidi.h"
#include <cstdio>
#include <vector>
int main() {
 ADL_MIDIPlayer *p=adl_init(48000); if(!p) return 2;
 if(adl_switchEmulator(p,ADLMIDI_EMU_DOSBOX)!=0 || adl_setBank(p,0)!=0) return 3;
 adl_rt_patchChange(p,0,80);
 if(adl_rt_noteOn(p,0,69,100)==0) return 4;
 std::vector<short> pcm(9600); int n=adl_generate(p,(int)pcm.size(),pcm.data());
 long long energy=0; for(int i=0;i<n;i++) energy+=(long long)pcm[i]*pcm[i];
 const unsigned char volume[]={0xf0,0x7f,0x7f,0x04,0x01,0x7f,0x7f,0xf7};
 const unsigned char tuning[]={0xf0,0x7f,0x7f,0x08,0x02,0x00,0x01,69,70,0,0,0xf7};
 int v=adl_rt_systemExclusive(p,volume,sizeof(volume));
 int t=adl_rt_systemExclusive(p,tuning,sizeof(tuning));
 std::printf("PCM_SAMPLES=%d NONZERO=%d MASTER_VOLUME=%d MTS_SINGLE=%d\n",n,energy>0,v,t);
 adl_rt_noteOff(p,0,69); adl_close(p);
 if(n!=9600 || energy==0 || v!=1) return 5;
 std::puts("BASELINE_ADLMIDI_RENDER_OK"); return 0;
}
