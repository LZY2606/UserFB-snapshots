#include "libde265/de265.h"
#include <fstream>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <atomic>
struct Buffers { std::atomic<unsigned> requests{0}, releases{0}; };
static int get_buffer(de265_decoder_context* ctx, de265_image_spec* spec, de265_image* image, void* data) {
 auto& b=*static_cast<Buffers*>(data); ++b.requests;
 return de265_get_default_image_allocation_functions()->get_buffer(ctx,spec,image,nullptr);
}
static void release_buffer(de265_decoder_context* ctx, de265_image* image, void* data) {
 ++static_cast<Buffers*>(data)->releases;
 de265_get_default_image_allocation_functions()->release_buffer(ctx,image,nullptr);
}
static uint64_t decode(const std::vector<char>& data,int workers,unsigned& frames,bool no_sao=false,bool no_deblock=false) {
 Buffers b; de265_image_allocation a{get_buffer,release_buffer};
 auto c=de265_new_decoder(); if(!c)std::exit(1);
 de265_set_image_allocation_functions(c,&a,&b);
 if(no_sao)de265_set_parameter_bool(c,DE265_DECODER_PARAM_DISABLE_SAO,1);
 if(no_deblock)de265_set_parameter_bool(c,DE265_DECODER_PARAM_DISABLE_DEBLOCKING,1);
 if(workers && de265_start_worker_threads(c,workers)!=DE265_OK)std::exit(2);
 if(de265_push_data(c,data.data(),data.size(),0,nullptr)!=DE265_OK || de265_flush_data(c)!=DE265_OK)std::exit(3);
 int more=1; uint64_t h=1469598103934665603ULL; unsigned steps=0; frames=0;
 while(more && ++steps<10000) {
  auto err=de265_decode(c,&more); if(!de265_isOK(err))std::exit(4);
  while(auto img=de265_get_next_picture(c)) {
   ++frames;
   for(int p=0;p<3;++p) {
    int stride; auto plane=de265_get_image_plane(img,p,&stride);
    int w=de265_get_image_width(img,p)*(de265_get_bits_per_pixel(img,p)>8?2:1),ht=de265_get_image_height(img,p);
    for(int y=0;y<ht;++y)for(int x=0;x<w;++x) {h^=plane[y*stride+x];h*=1099511628211ULL;}
   }
  }
 }
 de265_free_decoder(c); if(!frames||steps>=10000)std::exit(5);
 if(b.requests!=b.releases)std::exit(9);
 std::printf("BASELINE_BUFFER_REQUESTS workers=%d sao=%d deblock=%d frames=%u requests=%u releases=%u hash=%016llx\n",workers,!no_sao,!no_deblock,frames,b.requests.load(),b.releases.load(),(unsigned long long)h);
 return h;
}
int main() {
 std::ifstream f("support/fixtures/girlshy.h265",std::ios::binary);
 std::vector<char>data((std::istreambuf_iterator<char>(f)),{}); if(data.empty())return 6;
 unsigned frames; auto expected=decode(data,0,frames);
 for(int workers: {1,2,4}) {
  unsigned n; auto h=decode(data,workers,n); if(n!=frames || h!=expected)return 7;
 }
 unsigned n; auto off=decode(data,0,n,true); if(n!=frames || off==expected)return 8;
 unsigned m; auto off_parallel=decode(data,2,m,true); if(m!=n || off_parallel!=off)return 10;
 auto nd=decode(data,0,n,false,true); auto nd_parallel=decode(data,2,m,false,true);if(m!=n||nd!=nd_parallel)return 11;
 std::printf("BASELINE_HEVC_FILTER_COMPAT frames=%u hash=%016llx configs=8\n",frames,(unsigned long long)expected);
}
