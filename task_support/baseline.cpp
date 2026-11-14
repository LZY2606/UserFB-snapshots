#include "codec_api.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <fstream>
#include <iterator>
#include <cstring>
static void need(bool ok) { if(!ok) std::exit(1); }
static int decode(const char* file, bool normal) {
 std::ifstream f(file,std::ios::binary); std::vector<unsigned char>b((std::istreambuf_iterator<char>(f)),{}); need(!b.empty());
 std::vector<size_t> units;
 for(size_t i=0;i+4<b.size();i++) {
  size_t n=0; if(b[i]==0&&b[i+1]==0&&b[i+2]==1)n=3;
  else if(b[i]==0&&b[i+1]==0&&b[i+2]==0&&b[i+3]==1)n=4;
  if(n){if((b[i+n]&31)==9) units.push_back(i); i+=n-1;}
 }
 need(units.size()==8&&units[0]==0); units.push_back(b.size());
 ISVCDecoder* d=nullptr; need(WelsCreateDecoder(&d)==0&&d); int trace=0;d->SetOption(DECODER_OPTION_TRACE_LEVEL,&trace);
 SDecodingParam p{};p.eEcActiveIdc=ERROR_CON_DISABLE;p.sVideoProperty.eVideoBsType=VIDEO_BITSTREAM_AVC;need(d->Initialize(&p)==0);
 std::ifstream ref("task_support/fixtures/clip420.yuv",std::ios::binary);std::vector<unsigned char>expected((std::istreambuf_iterator<char>(ref)),{});
 int count=0;unsigned status=0;unsigned long sum=0;size_t k=0;
 auto output=[&](unsigned char** dst,SBufferInfo& info){if(!info.iBufferStatus)return;
  ++count;if(normal){need(info.UsrData.sSystemBuffer.iWidth==62&&info.UsrData.sSystemBuffer.iHeight==46);
   for(int pl=0;pl<3;pl++)for(int y=0;y<(pl?23:46);y++)for(int x=0;x<(pl?31:62);x++){
    unsigned char v=dst[pl][y*info.UsrData.sSystemBuffer.iStride[pl?1:0]+x];need(k<expected.size()&&expected[k++]==v);sum+=v;
   }
  }
 };
 for(size_t i=0;i+1<units.size();i++){unsigned char*dst[3]{};SBufferInfo info{};status|=d->DecodeFrameNoDelay(b.data()+units[i],int(units[i+1]-units[i]),dst,&info);output(dst,info);}
 int end=1;d->SetOption(DECODER_OPTION_END_OF_STREAM,&end);
 for(int i=0;i<16;i++){unsigned char*dst[3]{};SBufferInfo info{};status|=d->FlushFrame(dst,&info);if(!info.iBufferStatus)break;output(dst,info);}
 d->Uninitialize();WelsDestroyDecoder(d);
 if(normal){need(count==8&&status==0&&sum>0&&k==expected.size());std::printf("BASELINE_420 frames=%d sum=%lu exact=1\n",count,sum);}
 else {need(count==0&&status!=0);std::printf("BASELINE_422_UNSUPPORTED frames=%d status=%u\n",count,status);}
 return count;
}
int main(){decode("task_support/fixtures/clip420.264",true);decode("task_support/fixtures/clip422.264",false);std::puts("BASELINE_OK");}
