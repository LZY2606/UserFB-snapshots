#include "tsf.h"
#include <vector>
#include <string>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
typedef std::vector<unsigned char> Bytes;
static void u16(Bytes&b,unsigned x){b.push_back(x);b.push_back(x>>8);}
static void u32(Bytes&b,unsigned x){u16(b,x);u16(b,x>>16);}
static void name(Bytes&b,const char*s){size_t n=strlen(s);for(int i=0;i<20;i++)b.push_back(i<(int)n?s[i]:0);}
static void tag(Bytes&b,const char*s){b.insert(b.end(),s,s+4);}
static void chunk(Bytes&b,const char*s,const Bytes&v){tag(b,s);u32(b,(unsigned)v.size());b.insert(b.end(),v.begin(),v.end());if(v.size()%2)b.push_back(0);}
static Bytes list(const char*s,const Bytes&v){Bytes b;tag(b,s);b.insert(b.end(),v.begin(),v.end());return b;}
static Bytes font(){
 Bytes pd,b,sd,all,sm;
 for(unsigned i=0;i<64;i++)u16(sm,(unsigned)(int16_t)((int)(i%16)*1000-7500));for(int i=0;i<46;i++)u16(sm,0);
 chunk(sd,"smpl",sm);
 name(b,"OriginalTone");u16(b,7);u16(b,2);u16(b,0);u32(b,0);u32(b,0);u32(b,0);
 name(b,"EOP");u16(b,0);u16(b,0);u16(b,1);u32(b,0);u32(b,0);u32(b,0);chunk(pd,"phdr",b);b.clear();
 u16(b,0);u16(b,0);u16(b,1);u16(b,0);chunk(pd,"pbag",b);b.assign(10,0);chunk(pd,"pmod",b);b.clear();
 u16(b,41);u16(b,0);u16(b,0);u16(b,0);chunk(pd,"pgen",b);b.clear();
 name(b,"Instrument");u16(b,0);name(b,"EOI");u16(b,1);chunk(pd,"inst",b);b.clear();
 u16(b,0);u16(b,0);u16(b,2);u16(b,0);chunk(pd,"ibag",b);b.assign(10,0);chunk(pd,"imod",b);b.clear();
 u16(b,54);u16(b,1);u16(b,53);u16(b,0);u16(b,0);u16(b,0);chunk(pd,"igen",b);b.clear();
 name(b,"Triangle");u32(b,0);u32(b,64);u32(b,8);u32(b,56);u32(b,22050);b.push_back(60);b.push_back(0);u16(b,0);u16(b,1);
 name(b,"EOS");for(int i=0;i<4;i++)u32(b,110);u32(b,0);b.push_back(0);b.push_back(0);u16(b,0);u16(b,0);chunk(pd,"shdr",b);
 chunk(all,"LIST",list("sdta",sd));chunk(all,"LIST",list("pdta",pd));Bytes body=list("sfbk",all),out;chunk(out,"RIFF",body);return out;
}
int main(int argc,char**){
 if(argc>1){std::ifstream in(".task-build/sample.wav",std::ios::binary);Bytes b((std::istreambuf_iterator<char>(in)),{});if(b.size()!=44+220||memcmp(b.data()+36,"data",4))return 6;for(int i=0;i<64;i++){int16_t x=(int)(i%16)*1000-7500;unsigned y=b[44+2*i]|b[45+2*i]<<8;if((int16_t)y!=x)return 7;}puts("SFOTOOL_PCM samples=110 source_bytes=matched");return 0;}
 Bytes f=font();std::ofstream out(".task-build/source.sf2",std::ios::binary);out.write((char*)f.data(),f.size());out.close();
 tsf*t=tsf_load_memory(f.data(),(int)f.size());if(!t||tsf_get_presetcount(t)!=1)return 1;
 int p=tsf_get_presetindex(t,2,7);if(p<0)return 2;tsf_set_output(t,TSF_MONO,22050,0);tsf_note_on(t,p,60,1);
 float pcm[384];tsf_render_float(t,pcm,384,0);double energy=0;for(float x:pcm){if(!std::isfinite(x))return 3;energy+=x*x;}tsf_note_off(t,p,60);tsf_render_float(t,pcm,384,0);tsf_close(t);if(energy<0.001)return 4;
 std::printf("TSF_PLAY preset=2:7 frames=384 energy=%.8f\n",energy);return 0;
}
