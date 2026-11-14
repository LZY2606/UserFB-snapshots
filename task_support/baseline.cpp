#include "gme.h"
#include "fixture.h"
#include <cstdio>
#include <cstdlib>
#include <string>
struct Result{std::vector<short>pcm;std::string warning;};
static void need(bool ok){if(!ok)std::exit(1);}
static Result render(bool nsf2,bool split){
 auto b=nsf_fixture(nsf2);Music_Emu*e=nullptr;need(!gme_open_data(b.data(),long(b.size()),&e,44100));
 const char*w=gme_warning(e);Result r;if(w)r.warning=w;gme_ignore_silence(e,1);need(!gme_start_track(e,0));
 r.pcm.resize(44100);size_t pos=0;int i=0;const int pattern[]={2,14,514};
 while(pos<r.pcm.size()){size_t n=split?size_t(pattern[i++%3]):r.pcm.size();if(n>r.pcm.size()-pos)n=r.pcm.size()-pos;need(!gme_play(e,int(n),r.pcm.data()+pos));pos+=n;}
 gme_delete(e);return r;
}
int main(){auto a=render(false,false),b=render(true,true);need(a.pcm==b.pcm);long nonzero=0;unsigned long long hash=1469598103934665603ULL;
 for(short x:a.pcm){if(x)++nonzero;hash^=static_cast<unsigned short>(x);hash*=1099511628211ULL;}need(nonzero>0);
 need(b.warning.find("Unknown file version")!=std::string::npos);
 std::printf("BASELINE_NSF1 frames=22050 nonzero=%ld hash=%llu\n",nonzero,hash);
 std::printf("BASELINE_NSF2_IGNORED equal_legacy=1 warning=%s\n",b.warning.c_str());std::puts("BASELINE_OK");}
