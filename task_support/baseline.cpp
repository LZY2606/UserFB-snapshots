#include "Gb_Apu.h"
#include "Multi_Buffer.h"
#include "gme.h"
#include "gbs_fixture.h"
#include <vector>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdint>
static void tone(Gb_Apu& a,int vol){
 a.write_register(0,0xff26,0x80);a.write_register(0,0xff24,vol);a.write_register(0,0xff25,0x11);
 a.write_register(0,0xff11,0x80);a.write_register(0,0xff12,0xf0);a.write_register(0,0xff13,0);a.write_register(0,0xff14,0x87);
}
static std::vector<short> apu(int vol){
 Stereo_Buffer b; if(b.set_sample_rate(48000,250))return {};b.clock_rate(4194304);b.bass_freq(0);
 Gb_Apu a;a.output(b.center(),b.left(),b.right());a.reset();tone(a,vol);
 const int clocks=419430;a.end_frame(clocks);b.end_frame(clocks);
 std::vector<short> out(b.samples_avail());b.read_samples(out.data(),out.size());return out;
}
static double ratio(const std::vector<short>& out){
 double l=0,r=0;for(size_t i=256;i+1<out.size();i+=2){l+=double(out[i])*out[i];r+=double(out[i+1])*out[i+1];}return r?std::sqrt(l/r):0;
}
int main(int argc,char** argv){
 const bool target=argc>1&&!std::strcmp(argv[1],"target");
 auto symmetric=apu(0x77),asymmetric=apu(0x70);if(symmetric.size()<8000||asymmetric.size()!=symmetric.size())return 1;
 double sym=ratio(symmetric),asym=ratio(asymmetric);if(sym<0.99||sym>1.01||asym<0.9)return 2;
 bool equal=symmetric==asymmetric;
 std::printf("BASELINE_APU samples=%zu symmetric_ratio=%.6f nr50_70_ratio=%.6f same_pcm=%d\n",asymmetric.size(),sym,asym,equal);
 if(!target && (!equal||std::fabs(asym-1)>0.01))return 3; // asserted upstream defect, not a target expectation
 auto data=task_gbs(0x70);Music_Emu* e=0;gme_err_t err=gme_open_data(data.data(),data.size(),&e,48000);if(err){std::fprintf(stderr,"open: %s\n",err);return 4;}
 gme_ignore_silence(e,1);err=gme_start_track(e,0);if(err)return 5;
 std::vector<short> out(9600);if(gme_play(e,out.size(),out.data()))return 6;
 bool nonzero=false;uint64_t h=1469598103934665603ull;for(short v:out){nonzero|=v!=0;h^=static_cast<uint16_t>(v);h*=1099511628211ull;}
 std::printf("BASELINE_GBS voices=%d samples=%zu ratio=%.6f nonzero=%d pcm_hash=%016llx\n",gme_voice_count(e),out.size(),ratio(out),nonzero,(unsigned long long)h);
 gme_delete(e);if(!nonzero)return 7;
 std::puts("BASELINE_OK");return 0;
}
