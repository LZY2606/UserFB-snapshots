#include <cassert>
#include <cstdio>
extern "C" int uv_repro(unsigned*,unsigned*);
int main(){unsigned varied=0,flat=0;assert(uv_repro(&varied,&flat)==0);assert(flat!=0);assert((flat&255)==2);std::printf("TINYGL_BASELINE_OK varied=%06x flat=%06x equal=%d\n",varied,flat,varied==flat);}
