#include <zita-resampler/vresampler.h>
#include <zita-resampler/cresampler.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL line=%d %s\n",__LINE__,#x);std::exit(1);}}while(0)
static int setup(VResampler &r,double ratio,unsigned c){return r.setup(ratio,c,32);}
static int setup(CResampler &r,double ratio,unsigned c){return r.setup(ratio,c);}
template<class R>static std::vector<float> run(unsigned channels,bool split,bool identical=false){
 R r;CHECK(setup(r,48000.0/44100,channels)==0);r.set_phase(0.25);
 unsigned prefix=r.inpsize()/2-1,total=8192+prefix+r.inpsize();
 std::vector<float> input(total*channels,0),output;float buffer[113*6];
 for(unsigned i=0;i<8192;i++)for(unsigned c=0;c<channels;c++){
  unsigned k=identical?0:c;
  input[(prefix+i)*channels+c]=float(0.21*std::sin(0.013*i*(k+1))+0.08*std::cos(0.007*i*(k+2))+(i==100+17*k?0.1:0));
 }
 const unsigned chunks[]={1,7,113};unsigned pos=0,iteration=0;
 for(;iteration<100000;iteration++){
  unsigned offer=std::min(total-pos,split?chunks[iteration%3]:total),capacity=split?chunks[(iteration+1)%3]:113;
  r.inp_count=offer;r.inp_data=input.data()+pos*channels;r.out_count=capacity;r.out_data=buffer;
  CHECK(r.process()==0);unsigned used=offer-r.inp_count,made=capacity-r.out_count;pos+=used;
  for(unsigned i=0;i<made*channels;i++)CHECK(std::isfinite(buffer[i]));
  output.insert(output.end(),buffer,buffer+made*channels);
  if(!used&&!made){CHECK(pos==total);break;}
 }
 CHECK(iteration<100000&&pos==total&&!output.empty());
 if(identical)for(size_t i=0;i<output.size()/channels;i++)for(unsigned c=1;c<channels;c++)CHECK(output[i*channels+c]==output[i*channels]);
 return output;
}
template<class R>static void verify(const char *name){
 const unsigned cs[]={1,2,6};double energy=0,maxdiff=0;size_t frames=0;
 for(unsigned ch:cs){auto a=run<R>(ch,false);auto b=run<R>(ch,true);CHECK(a.size()==b.size());
  frames+=a.size()/ch;for(size_t i=0;i<a.size();i++){energy+=a[i]*a[i];maxdiff=std::max(maxdiff,double(std::fabs(a[i]-b[i])));}
 }
 CHECK(energy>1&&maxdiff<=2e-6);auto shared=run<R>(6,true,true);CHECK(!shared.empty());
 std::printf("%s_BASELINE channels=1,2,6 total_frames=%zu maxdiff=%.9g energy=%.9g shared_phase=equal\n",name,frames,maxdiff,energy);
}
int main(){verify<VResampler>("VRESAMPLER");verify<CResampler>("CRESAMPLER");std::puts("PRODUCTION_BASELINE_OK");}
