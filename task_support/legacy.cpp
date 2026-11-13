#include "signalsmith-stretch.h"
#include "cmd/util/wav.h"
#include <cstdio>
#include <stdexcept>
#include <string>
static void check(bool b) { if (!b) throw std::runtime_error("legacy check failed"); }
static void run(bool split, int inBlock, int outBlock, double pitch, double formant) {
 signalsmith::stretch::SignalsmithStretch<double> s(123);
 s.configure(2,512,128,split);s.setTransposeFactor(pitch);s.setFormantFactor(formant,true);s.setFormantBase(0.01);
 std::vector<double> a(inBlock),b(inBlock),x(outBlock),y(outBlock);
 double energy=0, relation=0; long count=0;
 for (int block=0;block<80;++block) {
  for (int i=0;i<inBlock;++i) { int n=block*inBlock+i;a[i]=.2*std::sin(.071*n)+.1*std::cos(.137*n);b[i]=-2*a[i]; }
  const double *in[2]={a.data(),b.data()};double *out[2]={x.data(),y.data()};
  s.process(in,inBlock,out,outBlock);
  for (int i=0;i<outBlock;++i) {check(std::isfinite(x[i])&&std::isfinite(y[i]));energy+=x[i]*x[i];relation=std::max(relation,std::abs(y[i]+2*x[i]));++count;}
 }
 check(energy>1&&relation<.005);
 double *out[2]={x.data(),y.data()};s.flush(out,outBlock);
 for(int i=0;i<outBlock;++i)check(std::isfinite(x[i])&&std::isfinite(y[i]));
 s.reset();
 std::printf("LEGACY_STRETCH split=%d input=%d output=%d frames=%ld energy=%.9f relation=%.3g\n",split,inBlock,outBlock,count,energy,relation);
}
int main(){
 try{
  for(bool split:{false,true}){run(split,128,128,1,1);run(split,96,128,1.25,.9);}
  Wav w;w.channels=2;w.sampleRate=48000;w.samples={0,.125,-.25,.375,.5,-.5};
  check(bool(w.write(".task-build/legacy.wav")));Wav back;check(bool(back.read(".task-build/legacy.wav")));
  check(back.channels==2&&back.samples.size()==w.samples.size());
  for(size_t i=0;i<w.samples.size();++i)check(std::abs(back.samples[i]-w.samples[i])<1e-4);
  std::puts("LEGACY_WAV_ROUNDTRIP_OK frames=3");std::puts("LEGACY_BASELINE_COMPLETE cases=5");
 }catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
