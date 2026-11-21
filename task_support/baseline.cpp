#include "Nsf_Emu.h"
#include "task_support/fixture.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
static void good(const char* e){if(e){std::fprintf(stderr,"GME: %s\n",e);std::exit(1);}}
class CountedNsf:public Nsf_Emu{
public: unsigned calls=0;long clocks=0;
protected: blargg_err_t run_clocks(blip_time_t& n,int ms) override{
    ++calls;auto e=Nsf_Emu::run_clocks(n,ms);clocks+=n;return e;
}};
static void begin(Nsf_Emu& e,const std::vector<unsigned char>& b,long rate){
    good(e.set_sample_rate(rate));e.ignore_silence(true);
    Mem_File_Reader r(b.data(),static_cast<long>(b.size()));good(e.load(r));good(e.start_track(0));
}
int main(){
    for(bool pal:{false,true})for(long rate:{44100L,48000L}){
        auto b=task_fixture::nsf(pal);CountedNsf e;Nsf_Emu ref;begin(e,b,rate);begin(ref,b,rate);
        const long frames=rate/3;std::vector<short>a(frames*2),q(a.size()),replay(a.size());
        good(e.play(static_cast<long>(a.size()),a.data()));
        long pos=0;unsigned k=0;const long chunks[]={1,7,257};
        while(pos<frames){long n=std::min(chunks[k++%3],frames-pos);good(ref.play(n*2,&q[pos*2]));pos+=n;}
        unsigned nonzero=0;for(short s:a)nonzero+=s!=0;
        if(!nonzero||a!=q||e.tell_samples()!=frames*2)return 2;
        good(e.start_track(0));good(e.play(static_cast<long>(replay.size()),replay.data()));if(a!=replay)return 3;
        auto changed=task_fixture::nsf(pal,1);Nsf_Emu alt;begin(alt,changed,rate);
        std::vector<short>altpcm(a.size());good(alt.play(static_cast<long>(altpcm.size()),altpcm.data()));
        if(a==altpcm)return 4;
        unsigned long long hash=1469598103934665603ULL;for(short v:a){hash^=static_cast<unsigned short>(v)&255;hash*=1099511628211ULL;hash^=static_cast<unsigned short>(v)>>8;hash*=1099511628211ULL;}
        std::printf("NSF_PCM_HASH rate=%ld pal=%d fnv64=%016llx\n",rate,pal,hash);
        std::printf("NSF_BASELINE rate=%ld pal=%d nonzero=%u frames=%ld frame_calls=%u clocks=%ld bank_dmc_changes_pcm=1\n",rate,pal,nonzero,frames,e.calls,e.clocks);
    }
    auto tinybytes=task_fixture::nsf();CountedNsf tiny;begin(tiny,tinybytes,48000);short tinyout[2];
    good(tiny.play(2,tinyout));
    std::printf("NSF_PREFETCH requested_frames=1 internal_frames=%u emulated_clocks=%ld\n",tiny.calls,tiny.clocks);
    std::puts("NSF_BASELINE_OK restart_pcm_equal=1 chunk_pcm_equal=1");return 0;
}
