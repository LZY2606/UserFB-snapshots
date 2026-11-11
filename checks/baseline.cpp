#include "Tunings.h"
#include <cassert>
#include <cmath>
#include <iostream>
int main() {
    const std::string text="Four tone tritave\n4\n5/4\n3/2\n2/1\n3/1\n! @ABL REFERENCE_PITCH 4 0 300.0\n! @ABL NOTE_NAMES A B C D\n";
    auto a=Tunings::parseASCLData(text);
    Tunings::Tuning t(a);
    int anchor=a.keyboardMapping.tuningConstantNote;
    assert(a.scale.count==4 && a.notationMapping.count==4);
    assert(std::abs(t.frequencyForMidiNote(anchor)-300.0)<1e-9);
    assert(std::abs(t.frequencyForMidiNote(anchor+4)/t.frequencyForMidiNote(anchor)-3)<1e-10);
    assert(t.noteNameForScalePosition(0)=="A");
    assert(t.isMidiNoteMapped(anchor));
    std::cout<<"BASELINE_OK tones=4 period=3 reference=300 names=4\n";
}
