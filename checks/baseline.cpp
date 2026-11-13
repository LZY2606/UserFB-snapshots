#include "sid.h"
#include <cstdio>
#include <cstdlib>
static void setup(reSID::SID& s){s.reset();s.set_chip_model(reSID::MOS6581);s.write(0,0x45);s.write(1,0x23);s.write(5,0);s.write(6,0xf0);s.write(4,0x81);s.write(24,15);}
int main(){reSID::SID a,b;setup(a);setup(b);for(int i=0;i<257;i++)a.clock();b.clock(257);auto x=a.read_state(),y=b.read_state();if(x.accumulator[0]!=y.accumulator[0])return 1;printf("RESID_DIGITAL_OBSERVATION envelope=%u/%u rate=%u/%u pipeline=%d/%d\n",x.envelope_counter[0],y.envelope_counter[0],x.rate_counter[0],y.rate_counter[0],x.shift_pipeline[0],y.shift_pipeline[0]);reSID::SID snd;setup(snd);if(!snd.set_sampling_parameters(985248,reSID::SAMPLE_INTERPOLATE,44100))return 2;short pcm[512];reSID::cycle_count cycles=10000;int n=snd.clock(cycles,pcm,512);long energy=0;for(int i=0;i<n;i++)energy+=std::abs(int(pcm[i]));if(n<1||energy==0)return 3;printf("RESID_BASELINE_OK frames=%d energy=%ld\n",n,energy);}
