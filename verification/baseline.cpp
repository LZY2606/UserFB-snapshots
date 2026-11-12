#include "lodepng.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
int main(){
 std::vector<unsigned char> raw;
 for(unsigned r=0;r<640;r++){for(unsigned i=0;i<240;i++)raw.push_back(65);for(unsigned j=0;j<18;j++)raw.push_back((r*37+j*53+(r>>(j%7)))%251+1);}
 LodePNGCompressSettings s;lodepng_compress_settings_init(&s);s.windowsize=32768;s.nicematch=258;
 unsigned char *z=0,*decoded=0;size_t nz=0,nd=0;
 assert(!lodepng_zlib_compress(&z,&nz,raw.data(),raw.size(),&s));assert(!lodepng_zlib_decompress(&decoded,&nd,z,nz,&lodepng_default_decompress_settings));assert(nd==raw.size()&&!memcmp(raw.data(),decoded,nd));free(z);free(decoded);puts("LODEPNG_LZ77_ROUNDTRIP_PASS");
 std::vector<unsigned char> image(31*17*4);for(size_t i=0;i<image.size();i++)image[i]=(i*17+i/31)%256;
 unsigned w=0,h=0;assert(!lodepng_encode32(&z,&nz,image.data(),31,17));assert(!lodepng_decode32(&decoded,&w,&h,z,nz));assert(w==31&&h==17&&!memcmp(decoded,image.data(),image.size()));free(z);free(decoded);puts("LODEPNG_RGBA_PASS");
}
