#include "task_support/audio_host.h"
#include <iostream>
#include <cmath>
int main(){
 try {
  const std::string code="Step s => dac; 0 => s.next; 100::samp => now; 0.25 => s.next; 1::samp => now; 0 => s.next; 200::samp => now;";
  auto a=task_render(code,512,113), b=task_render(code,512,7);
  if(a.size()!=512||a!=b)return 2;
  int count=0,pos=-1;
  for(int i=0;i<512;++i){if(!std::isfinite(a[i]))return 3;if(std::abs(a[i])>1e-7){++count;pos=i;if(std::abs(a[i]-0.25)>1e-6)return 4;}}
  if(count!=1||pos!=100){std::cerr<<"observed count="<<count<<" pos="<<pos<<"\n";return 5;}
  std::cout<<"BASELINE_STEP frame=100 value=0.25\nBASELINE_BLOCKS 7=113 frames=512\n";
  return 0;
 } catch(const std::exception &e){std::cerr<<e.what()<<"\n";return 10;}
}
