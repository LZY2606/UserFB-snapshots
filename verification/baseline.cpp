#include "codec/api/wels/codec_api.h"
#include <cassert>
#include <cstdio>
#include <vector>
int main() {
 ISVCEncoder* e=nullptr; assert(!WelsCreateSVCEncoder(&e));
 SEncParamBase p={}; p.iUsageType=CAMERA_VIDEO_REAL_TIME;p.fMaxFrameRate=25;p.iPicWidth=64;p.iPicHeight=48;p.iTargetBitrate=150000;p.iRCMode=RC_OFF_MODE;
 assert(!e->Initialize(&p)); ISVCDecoder* d=nullptr; assert(!WelsCreateDecoder(&d));
 SDecodingParam dp={};dp.sVideoProperty.eVideoBsType=VIDEO_BITSTREAM_AVC;dp.eEcActiveIdc=ERROR_CON_DISABLE;assert(!d->Initialize(&dp));
 std::vector<unsigned char> image(64*48*3/2,128); int decoded=0;
 for(int n=0;n<4;n++) {
  for(int y=0;y<48;y++)for(int x=0;x<64;x++) image[y*64+x]=(x*3+y*2+n*19)%220+16;
  SSourcePicture pic={};pic.iColorFormat=videoFormatI420;pic.iPicWidth=64;pic.iPicHeight=48;pic.iStride[0]=64;pic.iStride[1]=pic.iStride[2]=32;pic.pData[0]=image.data();pic.pData[1]=image.data()+64*48;pic.pData[2]=pic.pData[1]+32*24;pic.uiTimeStamp=n*40;
  SFrameBSInfo bs={};assert(!e->EncodeFrame(&pic,&bs));std::vector<unsigned char> au;
  for(int l=0;l<bs.iLayerNum;l++){auto& t=bs.sLayerInfo[l];int z=0;for(int k=0;k<t.iNalCount;k++)z+=t.pNalLengthInByte[k];au.insert(au.end(),t.pBsBuf,t.pBsBuf+z);}
  unsigned char* dst[3]={};SBufferInfo info={};info.uiInBsTimeStamp=n*40;
  auto err=d->DecodeFrameNoDelay(au.data(),(int)au.size(),dst,&info);assert(err==dsErrorFree);
  if(info.iBufferStatus){assert(info.UsrData.sSystemBuffer.iWidth==64&&info.UsrData.sSystemBuffer.iHeight==48);assert(dst[0]&&dst[1]&&dst[2]);unsigned sum=0;for(int y=0;y<48;y++)for(int x=0;x<64;x++)sum+=dst[0][y*info.UsrData.sSystemBuffer.iStride[0]+x];assert(sum>1000);decoded++;}
 }
 assert(decoded==4);d->Uninitialize();WelsDestroyDecoder(d);e->Uninitialize();WelsDestroySVCEncoder(e);
 puts("OPENH264_FOUR_FRAMES_PASS");
}
