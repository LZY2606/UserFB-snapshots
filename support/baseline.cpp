#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"
#include <vector>
#include <cassert>
#include <cstring>
#include <cstdio>
static int wr(int64_t off,const void *p,size_t n,void *v){auto &b=*(std::vector<unsigned char>*)v;if((size_t)off+n>b.size())b.resize((size_t)off+n);memcpy(b.data()+off,p,n);return 0;}
static int rd(int64_t off,void *p,size_t n,void *v){auto &b=*(std::vector<unsigned char>*)v;if(off<0||(size_t)off>b.size()||n>b.size()-(size_t)off)return 1;memcpy(p,b.data()+off,n);return 0;}
int main(){for(int frag=0;frag<=1;++frag){
 std::vector<unsigned char>b;auto *w=MP4E_open(0,frag,&b,wr);assert(w);
 MP4E_track_t t={};t.object_type_indication=0x40;t.track_media_kind=e_audio;t.time_scale=48000;t.default_duration=1024;t.u.a.channelcount=2;memcpy(t.language,"und",4);
 int id=MP4E_add_track(w,&t);assert(id>=0);unsigned char dsi[]={0x11,0x90};assert(!MP4E_set_dsi(w,id,dsi,2));
 for(int i=0;i<3;++i){unsigned char p[]={(unsigned char)(41+i),42,43};assert(!MP4E_put_sample(w,id,p,3,1024,MP4E_SAMPLE_DEFAULT));}assert(!MP4E_close(w));
 MP4D_demux_t d={};assert(MP4D_open(&d,rd,&b,b.size()));assert(d.track_count==1);
 if(!frag){assert(d.track[0].sample_count==3);for(unsigned i=0;i<3;++i){unsigned n,ts,dur;auto off=MP4D_frame_offset(&d,0,i,&n,&ts,&dur);assert(n==3&&ts==i*1024&&dur==1024&&b[off]==41+i);}puts("CLASSIC_SAMPLES_OK 3");}
 else printf("FRAGMENT_STARTPOINT samples=%u\n",d.track[0].sample_count);
 MP4D_close(&d);
} }
