#include "TableLookupOsc.h"
#include "SineWave.h"
#include <cassert>
#include <cmath>
#include <iostream>
int main() {
    Tonic::SampleTable table(5,1);
    const float values[]={0,1,0,-1,0};
    for(unsigned i=0;i<5;++i) table.dataPointer()[i]=values[i];
    Tonic::TableLookupOsc osc;
    osc.setLookupTable(table).freq(11025);
    Tonic::TonicFrames frames(Tonic::kSynthesisBlockSize,1);
    Tonic::Tonic_::SynthesisContext_ c;
    osc.tick(frames,c);
    for(unsigned i=0;i<frames.size();++i) assert(std::abs(frames[i]-values[i%4])<1e-6);
    c.tick();osc.tick(frames,c);
    for(unsigned i=0;i<frames.size();++i) assert(std::abs(frames[i]-values[i%4])<1e-6);
    Tonic::SineWave sine;sine.freq(440);
    c.tick();sine.tick(frames,c);
    double energy=0;for(unsigned i=0;i<frames.size();++i) energy+=frames[i]*frames[i];
    assert(energy>1 && std::isfinite(energy));
    std::cout<<"BASELINE_OK tableBlocks=2 sequence=0,1,0,-1 sine=nonzero\n";
}
