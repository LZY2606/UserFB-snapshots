#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include "jpge.h"
#include "jpgd.h"
int main(){
 std::vector<unsigned char> input(35*19*3),jpeg(65536);
 for(int y=0;y<19;y++)for(int x=0;x<35;x++)for(int c=0;c<3;c++)input[(y*35+x)*3+c]=(x*31+y*17+c*67)&255;
 int size=(int)jpeg.size();jpge::params p;p.m_quality=83;
 assert(jpge::compress_image_to_jpeg_file_in_memory(jpeg.data(),size,35,19,3,input.data(),p));
 assert(size>600&&jpeg[0]==255&&jpeg[1]==216);int sof=0;
 for(int i=0;i<size-1;i++)if(jpeg[i]==255&&jpeg[i+1]==192)sof++;
 assert(sof==1);jpgd::jpeg_decoder_mem_stream stream(jpeg.data(),size);
 int w=0,h=0,n=0;unsigned char *rgb=jpgd::decompress_jpeg_image_from_stream(&stream,&w,&h,&n,3);
 assert(rgb&&w==35&&h==19&&n==3);uint32_t digest=2166136261u;
 for(int i=0;i<w*h*3;i++)digest=(digest^rgb[i])*16777619u;
 free(rgb);printf("JPEG_BASELINE_OK dimensions=35x19 sof0=1 bytes=%d rgb=%08x\n",size,digest);
}
