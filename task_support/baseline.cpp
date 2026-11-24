#include <bw64/bw64.hpp>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <vector>

int main() {
  const char* path=".task-build/baseline.wav";
  auto output=bw64::writeFile(path,1,48000,24);
  float samples[]={0.25f,-0.5f,0.125f};
  if(output->write(samples,1)!=1 || output->write(samples+1,2)!=2) return 1;
  output->setAxmlChunk(std::make_shared<bw64::AxmlChunk>("audio"));
  output->close();
  auto input=bw64::readFile(path);
  if(input->numberOfFrames()!=3 || input->channels()!=1 || input->bitDepth()!=24) return 2;
  if(!input->axmlChunk() || input->axmlChunk()->data()!="audio") return 3;
  float decoded[3]={};
  if(input->read(decoded,2)!=2 || input->tell()!=2) return 4;
  input->seek(-1,std::ios::end);
  if(input->read(decoded+2,1)!=1 || input->tell()!=3) return 5;
  for(int i=0;i<3;i++) if(std::fabs(decoded[i]-samples[i])>0.000001f) return 6;
  input->close();
  if(std::remove(path)!=0) return 7;
  std::cout<<"BASELINE_OK frames=3 bitDepth=24 tail_axml=audio seek_last=0.125\n";
}
