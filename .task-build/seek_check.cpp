#include <juce_audio_devices/juce_audio_devices.h>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace juce;
struct S:PositionableAudioSource{
 std::vector<float> p; int64 pos=0; CriticalSection lk;
 S(int n):p(n){for(int i=0;i<n;++i)p[i]=0.25f+1e-5f*(i%29);}
 void prepareToPlay(int,double)override{}void releaseResources()override{}
 void setNextReadPosition(int64 x)override{const ScopedLock g(lk);pos=x;}
 int64 getNextReadPosition()const override{return pos;}
 int64 getTotalLength()const override{return(int64)p.size();}bool isLooping()const override{return false;}
 void getNextAudioBlock(const AudioSourceChannelInfo&i)override{const ScopedLock g(lk);
  for(int k=0;k<i.numSamples;++k){int64 x=pos+k;i.buffer->setSample(0,i.startSample+k,x<(int64)p.size()?p[x]:0.f);}pos+=i.numSamples;}
 void edit(int e){const ScopedLock g(lk);for(int i=e;i<(int)p.size();++i)p[i]=-0.5f;}
};
static std::vector<float> run(bool pre,double r,int64 seekSrc,int Dprefix,int total){
 S s(32768); double dev=44100,sr=dev*r;
 int64 devSeek=(int64)std::llround(seekSrc/r);
 int E=(int)std::ceil((double)seekSrc+(double)Dprefix*r-1e-9)+2;
 if(pre)s.edit(E);
 TimeSliceThread th("s");th.startThread(); AudioTransportSource t;
 t.setSource(&s,4096,&th,sr,1); t.prepareToPlay(64,dev);
 t.setNextReadPosition(devSeek); t.start();
 std::vector<int> zs={64,64,37,37,64,13,128,41,64};
 std::vector<float> y; int del=0; bool ed=false; int ci=0;
 while(del<total){int n=zs[ci%zs.size()]; ++ci; if(del+n>total)n=total-del;
   if(!pre&&!ed&&del==Dprefix){s.edit(E);t.notifySourceChanged({E,E+4096});ed=true;}
   AudioBuffer<float>b(1,n);b.clear();t.getNextAudioBlock({&b,0,n});
   for(int k=0;k<n;++k)y.push_back(b.getSample(0,k)); del+=n;}
 t.setSource(nullptr);th.stopThread(4000);return y;
}
int main(){ScopedJuceInitialiser_GUI ini;int fail=0;
 for(double r:{1.5,2.0/3.0}) for(int64 seek:{0,900,137}) for(int rep=0;rep<3;++rep){
   auto a=run(true,r,seek,128,420); auto b=run(false,r,seek,128,420);
   double w=0;for(size_t i=0;i<a.size();++i)w=std::max(w,(double)std::abs(a[i]-b[i]));
   if(w>2e-5){fail++;printf("FAIL r=%.4f seek=%lld w=%.3e\n",r,(long long)seek,w);}
 }
 printf("seek+RS cases=18 fails=%d\n",fail);return fail?1:0;}
