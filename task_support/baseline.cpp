#include "SoundTouch.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace soundtouch;
int main() {
  for(int ch : {1,2,6}) for(float rate : {0.73f,1.31f}) {
    SoundTouch s; s.setSampleRate(48000); s.setChannels(ch); s.setRate(rate);
    s.setTempo(0.83f); s.setSetting(SETTING_USE_QUICKSEEK,0);
    s.setSetting(SETTING_USE_AA_FILTER,1);
    const unsigned total=24000; unsigned sent=0, received=0; uint32_t rng=17;
    std::vector<float> out(97*ch); double energy=0, error=0;
    auto drain=[&] { unsigned n; while((n=s.receiveSamples(out.data(),97))) {
      received+=n; for(unsigned i=0;i<n;i++) {
        float x=out[i*ch]; if(!std::isfinite(x)) throw std::runtime_error("nonfinite");
        energy+=x*x; for(int c=1;c<ch;c++) error=std::fmax(error,std::fabs(out[i*ch+c]-x*((c%2)?-1.f:1.f)));
      }
    }};
    unsigned blocks=0; while(sent<total) {
      unsigned n=(blocks++%3==0?1:blocks%3==1?7:113); if(n>total-sent)n=total-sent;
      std::vector<float> in(n*ch);
      for(unsigned i=0;i<n;i++) { rng=rng*1664525u+1013904223u;
        float x=0.15f*std::sin((sent+i)*0.031)+0.10f*(float(int(rng>>16)-32768)/32768.f);
        for(int c=0;c<ch;c++) in[i*ch+c]=x*((c%2)?-1.f:1.f);
      }
      s.putSamples(in.data(),n); sent+=n;
      if(blocks%4)drain();
    }
    unsigned pending=s.numSamples(), unprocessed=s.numUnprocessedSamples();
    s.flush();drain();
    if(received<10000 || energy<1 || error>1e-6) return 2;
    std::printf("PIPELINE channels=%d rate=%.2f frames=%u pending=%u unprocessed=%u energy=%.6f relation=%.3g\n",ch,rate,received,pending,unprocessed,energy,error);
  }
  std::puts("BASELINE_OK runs=6");
}
