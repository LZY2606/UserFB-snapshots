#include "codec_api.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <cstring>
int main() {
 ISVCEncoder *enc=nullptr; ISVCDecoder *dec=nullptr;
 assert(!WelsCreateSVCEncoder(&enc)); assert(!WelsCreateDecoder(&dec));
 SEncParamBase ep={}; ep.iUsageType=CAMERA_VIDEO_REAL_TIME; ep.fMaxFrameRate=25;
 ep.iPicWidth=32; ep.iPicHeight=32; ep.iTargetBitrate=100000; ep.iRCMode=RC_OFF_MODE;
 assert(!enc->Initialize(&ep)); SDecodingParam dp={}; dp.uiTargetDqLayer=255;
 dp.eEcActiveIdc=ERROR_CON_DISABLE; dp.sVideoProperty.eVideoBsType=VIDEO_BITSTREAM_AVC;
 assert(!dec->Initialize(&dp)); std::vector<unsigned char> pixels(32*32*3/2,128); int seen=0;
 for(int f=0;f<4;f++) {
  for(int i=0;i<32*32;i++) pixels[i]=(i+f*9)%180+32;
  SSourcePicture pic={}; pic.iColorFormat=videoFormatI420; pic.iPicWidth=pic.iPicHeight=32;
  pic.iStride[0]=32; pic.iStride[1]=pic.iStride[2]=16;
  pic.pData[0]=pixels.data(); pic.pData[1]=pixels.data()+1024; pic.pData[2]=pixels.data()+1280;
  pic.uiTimeStamp=f*40; SFrameBSInfo bs={}; assert(!enc->EncodeFrame(&pic,&bs));
  std::vector<unsigned char> au;
  for(int l=0;l<bs.iLayerNum;l++) { auto &s=bs.sLayerInfo[l]; int n=0;
   for(int k=0;k<s.iNalCount;k++) n+=s.pNalLengthInByte[k]; au.insert(au.end(),s.pBsBuf,s.pBsBuf+n); }
  assert(!au.empty()); unsigned char *out[3]={}; SBufferInfo bi={}; bi.uiInBsTimeStamp=100+f;
  assert(dec->DecodeFrameNoDelay(au.data(),(int)au.size(),out,&bi)==dsErrorFree);
  assert(bi.iBufferStatus==1 && out[0]); assert(bi.UsrData.sSystemBuffer.iWidth==32);
  assert(bi.uiOutYuvTimeStamp==(unsigned)(100+f)); ++seen;
 }
 assert(seen==4); enc->Uninitialize(); dec->Uninitialize(); WelsDestroySVCEncoder(enc); WelsDestroyDecoder(dec);
 std::printf("openh264 encode/decode frames=%d: ok\n",seen);
}
