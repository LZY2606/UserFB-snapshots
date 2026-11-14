#ifndef SESSION_FIXTURE_H
#define SESSION_FIXTURE_H
#include <vector>
#include <cstring>
#include "modplug.h"
namespace session_fixture {
// Authored, valid four-channel MOD with a looping waveform and ordinary notes.
inline std::vector<unsigned char> module(unsigned variant=0) {
 std::vector<unsigned char> b(1084+1024+512,0);
 std::memcpy(b.data(),variant ? "session-B" : "session-A",9);
 b[20+22]=1; b[20+23]=0; b[20+25]=48;
 b[20+28]=1; b[20+29]=0;
 b[950]=1; b[951]=0x7f; std::memcpy(b.data()+1080,"M.K.",4);
 auto event=[&](unsigned row,unsigned channel,unsigned period,unsigned effect,unsigned value) {
  unsigned p=1084+(row*4+channel)*4;
  b[p]=(period>>8)&15; b[p+1]=period&255;
  b[p+2]=(period?0x10:0)|effect; b[p+3]=value;
 };
 event(0,0,variant?381:428,0,0); event(0,1,variant?285:339,0,0);
 event(1,0,0,0x0a,0x01); event(3,0,0,0x0c,0);event(3,1,0,0x0c,0);
 event(8,0,variant?254:320,0,0);event(8,1,variant?320:254,0,0);
 for(unsigned i=0;i<512;++i) {
  int phase=(i*(variant?3:1))%64;
  int sample=phase<32 ? phase*6-96 : 288-phase*6;
  b[1084+1024+i]=static_cast<unsigned char>(static_cast<signed char>(sample));
 }
 return b;
}
inline ModPlug_Settings settings(const char* name) {
 ModPlug_Settings s={}; s.mChannels=2;s.mBits=16;s.mFrequency=44100;
 s.mFlags=MODPLUG_ENABLE_OVERSAMPLING;s.mResamplingMode=MODPLUG_RESAMPLE_LINEAR;
 s.mStereoSeparation=128;s.mMaxMixChannels=64;s.mLoopCount=0;
 s.mReverbDepth=70;s.mReverbDelay=50;s.mBassAmount=60;s.mBassRange=40;
 s.mSurroundDepth=50;s.mSurroundDelay=15;
 if(!std::strcmp(name,"wet16")) s.mFlags|=MODPLUG_ENABLE_NOISE_REDUCTION|MODPLUG_ENABLE_REVERB|MODPLUG_ENABLE_MEGABASS|MODPLUG_ENABLE_SURROUND;
 if(!std::strcmp(name,"mono8")) {s.mChannels=1;s.mBits=8;s.mFrequency=11025;s.mResamplingMode=MODPLUG_RESAMPLE_NEAREST;}
 if(!std::strcmp(name,"stereo32")) {s.mBits=32;s.mFrequency=22050;s.mResamplingMode=MODPLUG_RESAMPLE_SPLINE;}
 return s;
}
inline unsigned block(unsigned i) {const unsigned sizes[]={257,23,1,511,37,1024};return sizes[i%6];}
}
#endif
