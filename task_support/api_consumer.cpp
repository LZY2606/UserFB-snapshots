// Fixed build/link consumer. This supplies no Nsf_Pump implementation.
#include "gme/Nsf_Pump.h"
#include "Nsf_Emu.h"
#include "task_support/fixture.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <vector>
static_assert(!std::is_copy_constructible<Nsf_Pump>::value,"Nsf_Pump owns execution state");
static void need(bool b,const char* where){if(!b){std::fprintf(stderr,"NSF_PUMP_CONSUMER_FAIL %s\n",where);std::exit(1);}}
static void good(const char* e,const char* where){if(e){std::fprintf(stderr,"NSF_PUMP_CONSUMER_FAIL %s: %s\n",where,e);std::exit(1);}}
static bool same(Nsf_Pump::Progress a,Nsf_Pump::Progress b){return a.instructions==b.instructions&&a.delivered_frames==b.delivered_frames&&a.emulated_clocks==b.emulated_clocks;}
static void zero(Nsf_Pump::Progress p){need(!p.instructions&&!p.delivered_frames&&!p.emulated_clocks,"initial progress");}
static unsigned long long hash(const std::vector<short>& a){
    unsigned long long h=1469598103934665603ULL;
    for(short v:a){h^=static_cast<unsigned short>(v)&255;h*=1099511628211ULL;h^=static_cast<unsigned short>(v)>>8;h*=1099511628211ULL;}return h;
}
static void start(Nsf_Pump& p,bool pal,long rate){
    auto b=task_fixture::nsf(pal);good(p.begin(b.data(),static_cast<long>(b.size()),rate,0),"begin normal");zero(p.progress());
    std::fill(b.begin(),b.end(),0); // The producer must own the input after begin.
}
struct Collect{
    Nsf_Pump p;std::vector<short> pcm;long rate;bool pal;unsigned round=0;
    Collect(bool ispal,long r):rate(r),pal(ispal){start(p,pal,rate);}
    bool step(){
        const long goal=rate/3,done=static_cast<long>(pcm.size()/2);
        if(done==goal)return false;
        need(round<2000000,"finite instruction-budget progress");
        const long caps[]={1,7,257};const unsigned quotas[]={1,2,37,511};
        long cap=std::min(caps[round%3],goal-done);unsigned budget=quotas[round%4];++round;
        std::vector<short> out(cap*2+4,12345);auto before=p.progress();Nsf_Pump::Result z{};
        good(p.pump(out.data(),cap,0,&z),"zero budget");need(!z.work&&!z.instructions&&!z.frames&&same(before,p.progress()),"zero budget state");
        good(p.pump(nullptr,0,budget,&z),"zero capacity");need(!z.work&&!z.instructions&&!z.frames&&same(before,p.progress()),"zero capacity state");
        need(p.pump(nullptr,cap,budget,&z)!=nullptr,"null output rejected");
        need(p.pump(out.data(),-1,budget,&z)!=nullptr,"negative capacity rejected");
        need(p.pump(out.data(),cap,budget,nullptr)!=nullptr,"null result rejected");
        need(same(before,p.progress()),"invalid pump preserved state");
        good(p.pump(out.data(),cap,budget,&z),"pump normal");auto now=p.progress();
        need(z.frames>=0&&z.frames<=cap,"capacity");
        need(z.work>0&&z.work<=budget&&z.work==z.instructions+static_cast<unsigned>(z.frames),"work accounting");
        need(now.instructions==before.instructions+z.instructions&&now.delivered_frames==before.delivered_frames+z.frames,"progress counters");
        need(now.emulated_clocks>=before.emulated_clocks,"monotonic CPU clock");
        for(long k=z.frames*2;k<cap*2+4;++k)need(out[k]==12345,"output overrun or padding");
        pcm.insert(pcm.end(),out.begin(),out.begin()+z.frames*2);return true;
    }
    void finish(){
        auto b=task_fixture::nsf(pal);Nsf_Emu ref;good(ref.set_sample_rate(rate),"reference rate");ref.ignore_silence(true);
        Mem_File_Reader reader(b.data(),static_cast<long>(b.size()));good(ref.load(reader),"reference load");good(ref.start_track(0),"reference start");
        std::vector<short> expected(pcm.size());good(ref.play(static_cast<long>(expected.size()),expected.data()),"reference play");
        need(pcm==expected,"PCM equals independent sequential playback");
        const unsigned long long frozen[2][2]={{0xc7fbbcea7c5a4e83ULL,0xfb37fc8d964db32fULL},{0xaaf5dfbc73434eebULL,0x562b13514b255f03ULL}};
        need(hash(pcm)==frozen[pal?1:0][rate==48000],"PCM equals frozen upstream source observation");
        need(p.progress().instructions>1000&&p.progress().emulated_clocks>0,"real playback progress");
        auto before=p.progress();auto invalid=b;invalid[5]=2;
        need(p.begin(invalid.data(),static_cast<long>(invalid.size()),rate,0)!=nullptr,"NSF2 rejected");
        need(p.begin(b.data(),static_cast<long>(b.size()),rate,1)!=nullptr,"invalid track rejected");
        need(p.begin(nullptr,0,rate,0)!=nullptr,"empty input rejected");
        need(p.begin(b.data(),static_cast<long>(b.size()),32000,0)!=nullptr,"invalid rate rejected");
        need(same(before,p.progress()),"failed begin preserved state");
        p.reset();p.reset();zero(p.progress());short output[2];Nsf_Pump::Result r{};
        need(p.pump(output,1,1,&r)!=nullptr,"unopened pump rejected");
        start(p,pal,rate);pcm.clear();round=0;
        while(pcm.size()<514){step();} // Restart need not land exactly on a former return boundary.
        need(std::equal(pcm.begin(),pcm.end(),expected.begin()),"restart PCM prefix");
    }
};
int main(){
    for(long rate:{44100L,48000L}){
        Collect a(false,rate),b(true,rate);bool morea=true,moreb=true;
        while(morea||moreb){if(morea)morea=a.step();if(moreb)moreb=b.step();}
        a.finish();b.finish();
    }
    std::puts("NSF_PUMP_API_OK pcm_equal=1 bounded_work=1 lifecycle=1");return 0;
}
