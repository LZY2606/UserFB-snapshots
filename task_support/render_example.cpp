#include "task_support/audio_host.h"
#include <fstream>
#include <iterator>
#include <iostream>
int main(int argc,char **argv){
 if(argc!=3)return 1;
 try{
  std::ifstream in(argv[1]);if(!in) return 2;
  std::string code((std::istreambuf_iterator<char>(in)),{});
  if(code.empty())return 3;
  auto pcm=task_render(code,100000,113);
  std::ofstream out(argv[2],std::ios::binary);out.write(reinterpret_cast<const char*>(pcm.data()),pcm.size()*sizeof(float));
  if(!out)return 4;
  int count=0;for(int i=0;i<100000;++i){float expected=i==66542?0.25f:i==90542?-0.5f:0.f;if(!std::isfinite(pcm[i])||std::abs(pcm[i]-expected)>1e-6){std::cerr<<"EXAMPLE_PCM frame="<<i<<" actual="<<pcm[i]<<" expected="<<expected<<"\n";return 5;}if(expected)++count;}
  std::cout<<"BEAT_EXAMPLE_OK impulses="<<count<<" frames="<<pcm.size()<<"\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 10;}
}
