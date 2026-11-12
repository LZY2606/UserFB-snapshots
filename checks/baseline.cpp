#include "Source/Effects/overdrive.h"
#include "Source/Effects/wavefolder.h"
#include <cmath>
#include <cstdio>
#include <cassert>
int main(){
 daisysp::Overdrive d;d.Init();d.SetDrive(.6f);
 daisysp::Wavefolder f;f.Init();f.SetGain(2);f.SetOffset(.25f);
 double energy=0;
 for(int i=0;i<4096;++i){float x=.8f*std::sin(i*.17f);float a=d.Process(x),b=f.Process(x);assert(std::isfinite(a)&&std::isfinite(b));assert(std::abs(b)<=1.00001f);energy+=a*a+b*b;}
 assert(energy>100);f.Init();assert(std::abs(f.Process(1.5f)-.5f)<1e-6f);assert(std::abs(f.Process(-1.5f)+.5f)<1e-6f);
 std::printf("DAISY_LEGACY_OK energy=%.6f\n",energy);
}
