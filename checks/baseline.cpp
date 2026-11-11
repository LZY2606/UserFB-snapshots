#include "MidiFile.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <sstream>
int main() {
    smf::MidiFile f;
    f.setTPQ(480); f.addTrack();
    f.addTimeSignature(0,0,4,4);
    f.addTimeSignature(1,1200,3,8);
    f.addTempo(0,0,120); f.addTempo(1,960,60);
    f.addNoteOn(1,0,0,60,100); f.addNoteOff(1,1680,0,60);
    for(int t=0;t<f.size();++t) for(int i=0;i<f[t].size();++i) f[t][i].track=t;
    f.sortTracks(); f.linkNotePairs(); f.doTimeAnalysis();
    assert(std::abs(f.getTimeInSeconds(1680)-2.5)<1e-9);
    f.makeDeltaTicks();
    std::stringstream io(std::ios::in|std::ios::out|std::ios::binary);
    assert(f.write(io)); io.seekg(0); smf::MidiFile g;
    assert(g.read(io)); g.makeAbsoluteTicks();
    int meters=0;
    for(int t=0;t<g.size();++t) for(int i=0;i<g[t].size();++i)
        if(g[t][i].isTimeSignature()) ++meters;
    assert(meters==2 && g.size()==2 && g.getTPQ()==480);
    std::cout << "BASELINE_OK tracks=2 meters=2 tick1680_seconds=2.5\n";
}
