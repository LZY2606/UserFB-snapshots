#include "task_support/audio_host.h"
#include "src/core/chuck_beat.h"
#include <iostream>
#include <cmath>
int main(){
 try {
  auto a=task_render("BeatClock c; c.bpm(120); Step s=>dac; 0=>s.next; c.at(1)=>now; 0.25=>s.next; 1::samp=>now; 0=>s.next; 100::samp=>now;",25000,113);
  int n=0;for(int i=0;i<25000;++i)if(std::abs(a[i])>1e-7){++n;if(i!=24000||std::abs(a[i]-0.25)>1e-6){std::cerr<<"BEAT_API wrong PCM at "<<i<<"\n";return 3;}}
  if(n!=1){std::cerr<<"BEAT_API missing impulse\n";return 4;}
  std::cout<<"BEAT_API_OK frame=24000\n";return 0;
 }catch(const std::exception &e){std::cerr<<"BEAT_API "<<e.what()<<"\n";return 2;}
}
