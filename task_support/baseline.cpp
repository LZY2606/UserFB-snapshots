#include "fixture.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <fstream>
#include <iterator>
int main(int argc,char**argv) {
 if(argc<3)return 2;
 const char*name=argv[2]; auto s=session_fixture::settings(name);auto mod=session_fixture::module();
 ModPlug_SetSettings(&s); ModPlugFile*f=ModPlug_Load(mod.data(),static_cast<int>(mod.size()));
 if(!f || ModPlug_NumChannels(f)!=4 || ModPlug_NumSamples(f)<1)return 3;
 unsigned frame=s.mChannels*s.mBits/8, total=0;std::vector<unsigned char> pcm;
 for(unsigned i=0;total<32768;++i) {
  unsigned n=session_fixture::block(i);if(n>32768-total)n=32768-total;
  std::vector<unsigned char>b(n*frame+8,0xa5);
  int got=ModPlug_Read(f,b.data(),n*frame);
  if(got!=static_cast<int>(n*frame))return 4;
  for(unsigned j=n*frame;j<b.size();++j)if(b[j]!=0xa5)return 5;
  pcm.insert(pcm.end(),b.begin(),b.begin()+got);total+=n;
 }
 int order=ModPlug_GetCurrentOrder(f),row=ModPlug_GetCurrentRow(f);ModPlug_Unload(f);
 unsigned audible=0;for(auto v:pcm)if(v!=(s.mBits==8?0x80:0))++audible;
 if(audible<100)return 6;
 std::string path=std::string("task_support/golden/")+name+".pcm";
 if(std::string(argv[1])=="dump") {std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(pcm.data()),pcm.size());if(!out)return 7;}
 else {std::ifstream in(path,std::ios::binary);std::vector<unsigned char> expected((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());if(expected!=pcm){std::fprintf(stderr,"PCM mismatch %s bytes=%zu expected=%zu\n",name,pcm.size(),expected.size());return 8;}}
 std::printf("MODPLUG_BASELINE_OK %s frames=%u bytes=%zu audible=%u order=%d row=%d\n",name,total,pcm.size(),audible,order,row);
 return 0;
}
