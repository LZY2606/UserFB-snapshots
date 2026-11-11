#include "libs/maxiPolyBLEP.h"
#include <cassert>
#include <cmath>
#include <cstdio>
int main() {
 maxiSettings::setup(48000,2,64);
 maxiPolyBLEP a,b;a.setWaveform(PolyBLEP::SAWTOOTH);b.setWaveform(PolyBLEP::SAWTOOTH);
 double energy=0;
 for(int i=0;i<2048;i++){double x=a.play(3000),y=b.play(3000);assert(x==y&&std::isfinite(x));energy+=x*x;}
 a.sync(0.2);double first=a.play(0);a.sync(0.7);double second=a.play(0);
 assert(std::fabs(first-second)>0.1 && energy>1);
 std::printf("baseline: wrapper deterministic; sync jumps %.6f -> %.6f; energy=%.6f\n",first,second,energy);
}
