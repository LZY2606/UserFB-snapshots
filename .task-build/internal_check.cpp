#include <juce_audio_devices/juce_audio_devices.h>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace juce;
struct S : PositionableAudioSource {
 std::vector<float> L,R; int64 pos=0; CriticalSection lk;
 S(int n):L(n),R(n){ for(int i=0;i<n;++i){L[i]=0.25f+1e-5f*(i%31);R[i]=-0.2f-2e-5f*(i%17);} }
 void prepareToPlay(int,double)override{} void releaseResources()override{}
 void setNextReadPosition(int64 p)override{const ScopedLock g(lk);pos=p;}
 int64 getNextReadPosition()const override{return pos;}
 int64 getTotalLength()const override{return (int64)L.size();} bool isLooping()const override{return false;}
 void getNextAudioBlock(const AudioSourceChannelInfo& i)override{const ScopedLock g(lk);
  for(int k=0;k<i.numSamples;++k){int64 x=pos+k;
   i.buffer->setSample(0,i.startSample+k,x<(int64)L.size()?L[x]:0.f);
   if(i.buffer->getNumChannels()>1)i.buffer->setSample(1,i.startSample+k,x<(int64)R.size()?R[x]:0.f);}
  pos+=i.numSamples;}
 void edit(int e){const ScopedLock g(lk);for(int i=e;i<(int)L.size();++i){L[i]=-0.5f;R[i]=0.9f;}}
};
static std::vector<float> run(bool pre,double ratio,const std::vector<int>& chunks,int pc,bool notify){
 S s(16384); int ch=2;
 int D=0; for(int i=0;i<pc;++i)D+=chunks[i];
 int E=(int)std::ceil(D*ratio-1e-9)+2;
 if(pre)s.edit(E);
 auto th=std::make_unique<TimeSliceThread>("i"); th->startThread();
 AudioTransportSource t; double sr=44100*ratio;
 t.setSource(&s,4096,th.get(),sr,ch); t.prepareToPlay(64,44100); t.start();
 std::vector<float> y; int del=0; bool ed=false;
 for(int ci=0;ci<(int)chunks.size();++ci){ int N=chunks[ci];
   if(!pre&&notify&&!ed&&ci==pc){s.edit(E);t.notifySourceChanged({E,E+4096});ed=true;}
   AudioBuffer<float> b(ch,N);b.clear();t.getNextAudioBlock({&b,0,N});
   for(int f=0;f<N;++f)for(int c=0;c<ch;++c)y.push_back(b.getSample(c,f));
   del+=N;}
 t.setSource(nullptr); th->stopThread(4000); return y;
}
int main(){
 juce::ScopedJuceInitialiser_GUI ini;
 int fails=0; double worstAll=0;
 struct C{double r;std::vector<int> z;int pc;};
 std::vector<C> cs={
  {1.5,{64,64,128,64,64},2},
  {1.5,{1,2,3,7,64,192,32,16},4},
  {2.0/3,{64,64,128,64},2},
  {2.0/3,{3,1,64,7,192,2,128},3},
  {1.0,{64,192,64},1},
 };
 for(int rep=0;rep<5;++rep) for(auto& c:cs){
   auto ref=run(true,c.r,c.z,c.pc,true);
   auto got=run(false,c.r,c.z,c.pc,true);
   double w=0; for(size_t i=0;i<ref.size();++i)w=std::max(w,(double)std::abs(ref[i]-got[i]));
   worstAll=std::max(worstAll,w); if(w>2e-5){fails++;printf("FAIL r=%.4f rep%d w=%.3e\n",c.r,rep,w);}
 }
 printf("internal+RS stereo reps=25 fails=%d worst=%.3e\n",fails,worstAll);
 return fails?1:0;
}
