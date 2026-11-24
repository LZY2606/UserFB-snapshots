#include "sequence/Sequence.h"
#include "sequence/SequenceEntry.h"
#include "generator/Sine.h"
#include "IReader.h"
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

static double energy(std::shared_ptr<aud::IReader> reader, int at) {
  reader->seek(at);
  int length=100; bool eos=false; std::vector<float> data(200);
  reader->read(length,eos,data.data());
  if(length!=100 || eos) throw std::runtime_error("short sequence read");
  double sum=0; for(int i=0;i<length*2;i++) sum+=data[i]*data[i];
  return sum;
}
int main() {
  aud::Specs specs{8000,aud::CHANNELS_STEREO};
  auto sequence=std::make_shared<aud::Sequence>(specs,25,false);
  auto source=std::make_shared<aud::Sine>(200,8000);
  auto entry=sequence->add(source,0,2,0);
  auto reader=sequence->createReader();
  if(energy(reader,800)<1) return 1;
  entry->move(1,2,0);
  if(energy(reader,800)!=0) return 2;
  if(energy(reader,8800)<1) return 3;
  sequence->remove(entry);
  if(energy(reader,8800)!=0) return 4;
  std::cout<<"BASELINE_OK audible=1 moved_gap=0 removed=0\n";
}
