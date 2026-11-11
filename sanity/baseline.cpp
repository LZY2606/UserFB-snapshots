#include "Voicer.h"
#include "Tone.h"
#include <cassert>
#include <cmath>
#include <iostream>
int main() {
    stk::Stk::setSampleRate(48000);
    Tone a,b;
    stk::Voicer v(0);
    v.addInstrument(&a,0);v.addInstrument(&b,1);
    long ta=v.noteOn(69,64,0),tb=v.noteOn(60,64,1);
    assert(ta!=tb && ta>=0 && tb>=0);
    assert(std::abs(a.frequency-440)<1e-9);
    double before=b.frequency;
    v.pitchBend(12288.0,0);
    assert(std::abs(a.frequency-440*std::sqrt(2.0))<1e-9);
    assert(b.frequency==before);
    stk::StkFrames block(64,1);v.tick(block);
    double energy=0;for(unsigned i=0;i<block.size();++i) energy+=block[i]*block[i];
    assert(energy>0);
    v.noteOff(ta,0);v.noteOff(tb,0);
    assert(v.tick()==0);
    std::cout<<"BASELINE_OK groups=2 oldBend=6semitones pcm=nonzero noteOff=quiet\n";
}
