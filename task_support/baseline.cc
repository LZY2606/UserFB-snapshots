#include "libde265/de265.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <cstdint>
#include <stdexcept>
struct Result { int frames=0; uint64_t hash=14695981039346656037ull; };
static void require(bool yes) { if(!yes) throw std::runtime_error("baseline contract failed"); }
static Result replay(const std::vector<unsigned char>& bytes, size_t chunk, int workers) {
  auto* ctx=de265_new_decoder(); require(ctx!=nullptr);
  if(workers) require(de265_start_worker_threads(ctx,workers)==DE265_OK);
  Result r;
  auto drain=[&](){ while(const auto* img=de265_peek_next_picture(ctx)) {
    ++r.frames;
    for(int c=0;c<3;++c) { int stride=0; auto* p=de265_get_image_plane(img,c,&stride);
      int w=de265_get_image_width(img,c),h=de265_get_image_height(img,c); require(p&&w>0&&h>0);
      for(int y=0;y<h;++y) for(int x=0;x<w;++x) {r.hash^=p[y*stride+x];r.hash*=1099511628211ull;}
    } de265_release_next_picture(ctx);
  }};
  auto pump=[&](){ for(int n=0;n<10000;++n) {
    int more=0; auto err=de265_decode(ctx,&more); drain();
    if(err==DE265_ERROR_WAITING_FOR_INPUT_DATA) return;
    if(err==DE265_ERROR_IMAGE_BUFFER_FULL) continue;
    require(err==DE265_OK); if(!more) return;
  } throw std::runtime_error("decode did not settle"); };
  for(size_t pos=0;pos<bytes.size();) { size_t n=std::min(chunk,bytes.size()-pos);
    require(de265_push_data(ctx,bytes.data()+pos,int(n),17,nullptr)==DE265_OK); pos+=n;pump();
  }
  require(de265_flush_data(ctx)==DE265_OK);pump();drain();
  require(de265_free_decoder(ctx)==DE265_OK);require(r.frames==75);
  std::printf("DECODE chunk=%zu workers=%d frames=%d hash=%016llx\n",chunk,workers,r.frames,(unsigned long long)r.hash);
  return r;
}
int main(){try {
  std::ifstream f("testdata/girlshy.h265",std::ios::binary); require(bool(f));
  std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)),{});require(!b.empty());
  auto a=replay(b,4096,0),c=replay(b,1,0),d=replay(b,4096,2);
  require(a.hash==c.hash&&a.hash==d.hash);std::puts("BASELINE_COMPLETE frames=75 chunk_worker_equal=1");
  return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
