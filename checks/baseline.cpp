#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>
extern "C" {
#include "dumb.h"
#include "internal/it.h"
}
using Bytes=std::vector<unsigned char>;
static void word(Bytes& b,size_t p,uint32_t v,int n){for(int i=0;i<n;++i)b.at(p+i)=static_cast<unsigned char>(v>>(i*8));}
static void need(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
// Small witness encoder: fixed maximum width only, no width changes.
static Bytes constant_block(int count,int bits,int first){
  Bytes raw((size_t(count)*(bits+1)+7)/8,0); unsigned value=unsigned(first)&((1u<<bits)-1);
  for(int j=0;j<bits+1;++j)if(value&(1u<<j))raw[size_t(j)/8]|=1u<<(j%8);
  Bytes out(2); word(out,0,uint32_t(raw.size()),2);out.insert(out.end(),raw.begin(),raw.end());return out;
}
static Bytes module(int bits,bool stereo,bool loop,bool packed,bool twice,int frames){
  const size_t sample=0xd0, payload=sample+80;
  Bytes b(payload,0); std::memcpy(b.data(),"IMPM",4);word(b,32,1,2);word(b,36,1,2);word(b,38,1,2);
  word(b,40,0x215,2);word(b,42,0x214,2);word(b,44,1,2);
  b[48]=128;b[49]=48;b[50]=6;b[51]=125;b[52]=128;
  for(int i=0;i<64;++i){b[64+i]=32;b[128+i]=64;}
  b[192]=0;word(b,193,uint32_t(sample),4);word(b,197,0,4);
  std::memcpy(b.data()+sample,"IMPS",4);
  b[sample+17]=64;b[sample+18]=1|(bits==16?2:0)|(stereo?4:0)|(packed?8:0)|(loop?16:0);
  b[sample+19]=64;b[sample+46]=1|(twice?4:0);word(b,sample+48,frames,4);
  word(b,sample+56,loop?8:0,4);word(b,sample+60,8363,4);word(b,sample+72,uint32_t(payload),4);
  for(int ch=0;ch<(stereo?2:1);++ch){
    if(packed){int left=frames;int block=0;while(left){int n=left<(bits==8?32768:16384)?left:(bits==8?32768:16384);
      Bytes x=constant_block(n,bits,ch?23:(block?7:1));b.insert(b.end(),x.begin(),x.end());left-=n;++block;}}
    else for(int i=0;i<frames;++i){int v=(ch?23:1);b.push_back(v&255);if(bits==16)b.push_back((v>>8)&255);}
  }
  return b;
}
struct Loaded{
  DUH* duh=nullptr;DUMBFILE* f=nullptr;
  explicit Loaded(const Bytes&b){f=dumbfile_open_memory(reinterpret_cast<const char*>(b.data()),b.size());need(f,"open");duh=dumb_read_it_quick(f);}
  ~Loaded(){if(duh)unload_duh(duh);if(f)dumbfile_close(f);}
  IT_SAMPLE& sample(){need(duh,"load rejected");auto*p=duh_get_it_sigdata(duh);need(p&&p->n_samples==1,"sample count");return p->sample[0];}
};
static int pcm(IT_SAMPLE&s,int i){return s.flags&IT_SAMPLE_16BIT?static_cast<short*>(s.data)[i]:static_cast<signed char*>(s.data)[i];}
static void legacy(){
 int cases=0;
 for(int bits:{8,16})for(bool stereo:{false,true})for(bool packed:{false,true})for(bool twice:{false,true}){
   if(twice&&!packed)continue;
   Bytes b=module(bits,stereo,false,packed,twice,32);Loaded x(b);auto&s=x.sample();need(s.length==32,"legacy length");
   for(int i=0;i<32;++i)for(int ch=0;ch<(stereo?2:1);++ch){int want=(ch?23:1)*(twice?i+1:1);if(bits==8)want=int(static_cast<signed char>(want));need(pcm(s,i*(stereo?2:1)+ch)==want,"legacy PCM");}
   ++cases;
 }
 std::printf("DUMB_LEGACY cases=%d pcm=OK\n",cases);
}
int main(int argc,char**argv){try{
 legacy();if(argc>1&&std::strcmp(argv[1],"--legacy")==0)return 0;
 Bytes b=module(8,true,true,true,false,32769);Loaded x(b);auto&s=x.sample();int l=pcm(s,0),r=pcm(s,1);
 need(s.length==8&&l==1&&r==7,"loop witness changed");
 std::printf("IT_LOOP_WITNESS retained=%ld left=%d right=%d expected_right=23\n",s.length,l,r);
 Bytes shortb=module(8,false,false,true,false,8);
 // Physically present block lacks the bits needed for eight 9-bit deltas.
 shortb.resize(0xd0+80+3);word(shortb,0xd0+80,1,2);shortb.back()=0;
 Loaded y(shortb);need(y.duh!=nullptr,"exhaustion witness changed");
 std::printf("IT_BITS_WITNESS incomplete_block=accepted frames=%ld\n",y.sample().length);
 return 0;
 }catch(const std::exception&e){std::fprintf(stderr,"BASELINE_FAIL %s\n",e.what());return 1;}}
