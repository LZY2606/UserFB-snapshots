#include <juce_audio_devices/juce_audio_devices.h>
#include <iostream>
#include <cmath>
#include <vector>
struct EditableSource final : juce::PositionableAudioSource {
 std::vector<float> pcm=std::vector<float>(8192,0.25f);
 juce::int64 pos=0;
 juce::CriticalSection lock;
 void prepareToPlay(int,double) override {}
 void releaseResources() override {}
 void setNextReadPosition(juce::int64 p) override { const juce::ScopedLock g(lock); pos=p; }
 juce::int64 getNextReadPosition() const override {return pos;}
 juce::int64 getTotalLength() const override {return (juce::int64)pcm.size();}
 bool isLooping() const override {return false;}
 void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override {
  const juce::ScopedLock g(lock);
  for(int c=0;c<info.buffer->getNumChannels();++c)
   for(int i=0;i<info.numSamples;++i)
    info.buffer->setSample(c,info.startSample+i,pos+i<(juce::int64)pcm.size()?pcm[(size_t)(pos+i)]:0);
  pos+=info.numSamples;
 }
 void replace() { const juce::ScopedLock g(lock); for(int i=128;i<192;++i)pcm[i]=-0.5f; }
};
int main() {
 juce::ScopedJuceInitialiser_GUI juce_init;
 EditableSource source;
 juce::TimeSliceThread thread("task-prefetch"); thread.startThread();
 juce::AudioTransportSource transport;
 transport.setSource(&source,4096,&thread,0,1);
 transport.prepareToPlay(64,44100);
 transport.start();
 juce::AudioBuffer<float> first(1,64); transport.getNextAudioBlock({&first,0,64});
 if(std::abs(first.getSample(0,0)-0.25f)>1e-6f) return 2;
 source.replace();
 juce::AudioBuffer<float> next(1,192); transport.getNextAudioBlock({&next,0,192});
 float stale=next.getSample(0,64);
 std::cout<<"BASELINE_AUDIO normal="<<first.getSample(0,0)<<" edited_source=-0.5 cached_output="<<stale<<" output_frames=256\n";
 transport.setSource(nullptr);
 thread.stopThread(2000);
 return std::abs(stale-0.25f)>1e-6f?3:0;
}
