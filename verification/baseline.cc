#include "libde265/de265.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
int main(int argc,char **argv) {
  assert(argc==3); std::ifstream in(argv[1],std::ios::binary);
  std::vector<char> data((std::istreambuf_iterator<char>(in)),{}); assert(!data.empty());
  auto *ctx=de265_new_decoder(); assert(ctx);
  assert(de265_isOK(de265_push_data(ctx,data.data(),data.size(),0,nullptr)));
  assert(de265_isOK(de265_flush_data(ctx)));
  FILE *out=fopen(argv[2],"wb"); assert(out); int frames=0,more=1;
  for(int tries=0;more && tries<1000;tries++) {
    assert(de265_isOK(de265_decode(ctx,&more)));
    while(const auto *img=de265_get_next_picture(ctx)) {
      assert(de265_get_image_width(img,0)==128 && de265_get_image_height(img,0)==96);
      for(int c=0;c<3;c++) {
        int stride,w=de265_get_image_width(img,c),h=de265_get_image_height(img,c);
        const auto *p=de265_get_image_plane(img,c,&stride);
        for(int y=0;y<h;y++) assert(fwrite(p+y*stride,1,w,out)==(size_t)w);
      }
      frames++;
    }
  }
  assert(!more && frames==3); fclose(out); de265_free_decoder(ctx);
  puts("public API decoded 3 HEVC frames");
}
