#include "jdksmidi/world.h"
#include "jdksmidi/multitrack.h"
#include "jdksmidi/sequencer.h"
#include "jdksmidi/filewritemultitrack.h"
#include "jdksmidi/filereadmultitrack.h"
#include <cassert>
#include <cstdio>
using namespace jdksmidi;
int main(int argc,char**argv) {
  assert(argc==2); MIDIMultiTrack s(3); s.SetClksPerBeat(480);
  MIDITimedBigMessage e;
  e.SetTempo(500000); e.SetTime(0); s.GetTrack(0)->PutEvent(e);
  e.SetNoteOn(0,60,90); e.SetTime(20); s.GetTrack(1)->PutEvent(e);
  e.SetControlChange(0,64,127); e.SetTime(30); s.GetTrack(2)->PutEvent(e);
  e.SetNoteOff(0,60,0); e.SetTime(40); s.GetTrack(1)->PutEvent(e);
  e.SetControlChange(0,64,0); e.SetTime(80); s.GetTrack(2)->PutEvent(e);
  MIDIMultiTrackIterator it(&s); it.GoToTime(0);
  const MIDITimedBigMessage *m; int tr=0,count=0; MIDIClockTime last=0;
  while(it.GetCurEvent(&tr,&m)) {assert(m->GetTime()>=last);last=m->GetTime();++count;if(!it.GoToNextEvent())break;}
  assert(count==5); puts("MIDI_MERGE_BASELINE_OK events=5");
  {MIDIFileWriteStreamFileName stream(argv[1]); MIDIFileWriteMultiTrack w(&s,&stream); assert(w.Write());}
  MIDIMultiTrack back(3); MIDIFileReadMultiTrack events(&back);
  MIDIFileReadStreamFile stream(argv[1]); MIDIFileRead reader(&stream,&events); assert(reader.Parse());
  assert(back.GetClksPerBeat()==480); assert(back.GetNumEvents()>=5);
  puts("MIDI_FILE_ROUNDTRIP_OK ppqn=480");
}
