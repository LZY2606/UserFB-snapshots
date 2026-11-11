#pragma once
#include "Instrmnt.h"
#include "SineWave.h"
class Tone : public stk::Instrmnt {
    stk::SineWave oscillator;
    stk::StkFloat gain = 0;
public:
    stk::StkFloat frequency = 0;
    void setFrequency(stk::StkFloat f) override {frequency=f; oscillator.setFrequency(f);}
    void noteOn(stk::StkFloat f,stk::StkFloat a) override {setFrequency(f);gain=a;}
    void noteOff(stk::StkFloat) override {gain=0;}
    stk::StkFloat tick(unsigned int=0) override {return lastFrame_[0]=gain*oscillator.tick();}
    stk::StkFrames& tick(stk::StkFrames& f,unsigned int c=0) override {
        for(unsigned i=0;i<f.frames();++i) f(i,c)=tick();
        return f;
    }
};
