#include "gme/Nes_Apu.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>
struct Event { int time, addr, value; };
struct Reader {
    std::vector<unsigned> addresses;
    static int read(void* p, nes_addr_t a) {
        Reader& r = *static_cast<Reader*>(p);
        r.addresses.push_back(a);
        return ((a * 73u) ^ (a >> 3) ^ 0x96u) & 255;
    }
};
struct Result { std::vector<short> pcm; std::vector<unsigned> reads; };
static void need(bool b, const char* what) {
    if (!b) { std::cerr << what << '\n'; std::exit(1); }
}
static Result render(bool split) {
    const Event events[] = {
        {0,0x4017,0x80},{0,0x4015,0x0f},
        {0,0x4000,0xbf},{0,0x4001,8},{0,0x4002,53},{0,0x4003,0xf8},
        {0,0x4004,0x78},{0,0x4005,8},{0,0x4006,71},{0,0x4007,0xf8},
        {0,0x4008,0xff},{0,0x400a,89},{0,0x400b,0xf8},
        {0,0x400c,0x35},{0,0x400e,7},{0,0x400f,0xf8},
        {0,0x4010,0x4f},{0,0x4011,32},{0,0x4012,0},{0,0x4013,2},
        {0,0x4015,0x1f},{711,0x4011,79},{6000,0x4000,0x76},
        {12003,0x400e,0x8a},{39999,0x4004,0xba},{70003,0x4011,16}
    };
    Blip_Buffer b;
    need(!b.set_sample_rate(44100,1000),"Blip allocation");
    b.clock_rate(1789773);
    Nes_Apu a;
    Reader r;
    a.dmc_reader(Reader::read,&r);
    a.output(&b);
    a.volume(0.25);
    a.reset();
    Result out;
    const int total=120000, chunks[]={1,7,257,997,4093};
    unsigned ei=0, ci=0;
    for (int base=0; base<total;) {
        int n=split ? std::min(chunks[ci++%5],total-base) : total-base;
        while (ei<sizeof(events)/sizeof(events[0]) && events[ei].time<base+n) {
            const Event& e=events[ei++];
            a.write_register(e.time-base,e.addr,e.value);
        }
        a.end_frame(n);
        b.end_frame(n);
        short buf[4096];
        while (long k=b.read_samples(buf,4096)) out.pcm.insert(out.pcm.end(),buf,buf+k);
        base+=n;
    }
    out.reads=r.addresses;
    return out;
}
int main() {
    Result whole=render(false), split=render(true);
    need(whole.pcm==split.pcm,"legacy PCM changed with frame partition");
    need(whole.reads==split.reads,"DMC read order changed with frame partition");
    need(whole.pcm.size()>2000 && whole.reads.size()>20,"legacy path not reached");
    need(std::any_of(whole.pcm.begin(),whole.pcm.end(),[](short s){return s!=0;}),"silent legacy signal");
    uint64_t h=1469598103934665603ULL;
    for (short s:whole.pcm) { h^=uint16_t(s); h*=1099511628211ULL; }
    std::cout << "LEGACY_APU frames=" << whole.pcm.size() << " dmc_reads=" << whole.reads.size() << " fnv=" << h << '\n';
    std::cout << "BASELINE_NES_LEGACY_PARTITIONS_OK\n";
}
