#include "wels/codec_api.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdint>
static const int W=64,H=48;
static void fill(std::vector<unsigned char>&p,int frame){
 for(size_t i=0;i<p.size();++i){uint32_t x=(uint32_t)i*1664525u+1013904223u+(uint32_t)frame*977u;p[i]=(unsigned char)((x^(x>>13)^(x>>23))&255);}
}
static int run(int entropy){
 ISVCEncoder*enc=NULL;ISVCDecoder*dec=NULL;
 if(WelsCreateSVCEncoder(&enc)||!enc||WelsCreateDecoder(&dec)||!dec)return 2;
 int quiet=WELS_LOG_QUIET;enc->SetOption(ENCODER_OPTION_TRACE_LEVEL,&quiet);dec->SetOption(DECODER_OPTION_TRACE_LEVEL,&quiet);
 SEncParamExt p;std::memset(&p,0,sizeof(p));if(enc->GetDefaultParams(&p))return 3;
 p.iUsageType=CAMERA_VIDEO_REAL_TIME;p.iPicWidth=W;p.iPicHeight=H;p.fMaxFrameRate=25;p.iTargetBitrate=200000;
 p.iRCMode=RC_OFF_MODE;p.iTemporalLayerNum=1;p.iSpatialLayerNum=1;p.bEnableDenoise=false;p.bEnableFrameSkip=false;
 p.bEnableBackgroundDetection=false;p.bEnableAdaptiveQuant=false;p.bEnableSceneChangeDetect=false;
 p.iMultipleThreadIdc=1;p.iLoopFilterDisableIdc=1;p.iEntropyCodingModeFlag=entropy;
 p.sSpatialLayers[0].iVideoWidth=W;p.sSpatialLayers[0].iVideoHeight=H;p.sSpatialLayers[0].fFrameRate=25;
 p.sSpatialLayers[0].iSpatialBitrate=200000;p.sSpatialLayers[0].uiProfileIdc=entropy?PRO_MAIN:PRO_BASELINE;
 p.sSpatialLayers[0].iDLayerQp=32;p.sSpatialLayers[0].sSliceArgument.uiSliceMode=SM_SINGLE_SLICE;
 if(enc->InitializeExt(&p))return 4;
 SDecodingParam dp;std::memset(&dp,0,sizeof(dp));dp.sVideoProperty.eVideoBsType=VIDEO_BITSTREAM_AVC;
 dp.eEcActiveIdc=ERROR_CON_DISABLE;if(dec->Initialize(&dp))return 5;
 size_t bytes=0,mismatch=0;int decoded=0;unsigned long hash=2166136261u;
 std::vector<unsigned char>raw(W*H*3/2);
 for(int f=0;f<3;f++){
  fill(raw,f);SSourcePicture pic;std::memset(&pic,0,sizeof(pic));pic.iColorFormat=videoFormatI420;pic.iPicWidth=W;pic.iPicHeight=H;
  pic.iStride[0]=W;pic.iStride[1]=pic.iStride[2]=W/2;pic.pData[0]=raw.data();pic.pData[1]=raw.data()+W*H;pic.pData[2]=raw.data()+W*H*5/4;
  pic.uiTimeStamp=f*40;SFrameBSInfo out;std::memset(&out,0,sizeof(out));if(enc->EncodeFrame(&pic,&out))return 6;
  std::vector<unsigned char>bs;
  for(int l=0;l<out.iLayerNum;l++){SLayerBSInfo&layer=out.sLayerInfo[l];int len=0;for(int k=0;k<layer.iNalCount;k++)len+=layer.pNalLengthInByte[k];bs.insert(bs.end(),layer.pBsBuf,layer.pBsBuf+len);}
  if(bs.empty())return 7;bytes+=bs.size();for(size_t k=0;k<bs.size();k++)hash=(hash^bs[k])*16777619u;
  unsigned char*planes[3]={NULL,NULL,NULL};SBufferInfo info;std::memset(&info,0,sizeof(info));
  DECODING_STATE ds=dec->DecodeFrameNoDelay(bs.data(),(int)bs.size(),planes,&info);
  if(ds!=dsErrorFree)return 8;
  if(info.iBufferStatus){
   std::vector<unsigned char>expected(W*H*3/2);fill(expected,decoded);
   if(info.UsrData.sSystemBuffer.iWidth!=W||info.UsrData.sSystemBuffer.iHeight!=H)return 9;
   for(int pl=0;pl<3;pl++){int width=pl?W/2:W,height=pl?H/2:H;size_t off=pl?(pl==1?W*H:W*H*5/4):0;int stride=info.UsrData.sSystemBuffer.iStride[pl?1:0];
    for(int y=0;y<height;y++)for(int x=0;x<width;x++)if(planes[pl][y*stride+x]!=expected[off+y*width+x])mismatch++;
   }
   decoded++;
  }
 }
 int eos=1;if(dec->SetOption(DECODER_OPTION_END_OF_STREAM,&eos))return 11;
 for(int attempt=0;attempt<5&&decoded<3;attempt++){
  unsigned char*planes[3]={NULL,NULL,NULL};SBufferInfo info;std::memset(&info,0,sizeof(info));
  DECODING_STATE ds=attempt?dec->FlushFrame(planes,&info):dec->DecodeFrameNoDelay(NULL,0,planes,&info);
  if(ds!=dsErrorFree)return 12;
  if(info.iBufferStatus){
   std::vector<unsigned char>expected(W*H*3/2);fill(expected,decoded);
   if(info.UsrData.sSystemBuffer.iWidth!=W||info.UsrData.sSystemBuffer.iHeight!=H)return 13;
   for(int pl=0;pl<3;pl++){int width=pl?W/2:W,height=pl?H/2:H;size_t off=pl?(pl==1?W*H:W*H*5/4):0;int stride=info.UsrData.sSystemBuffer.iStride[pl?1:0];
    for(int y=0;y<height;y++)for(int x=0;x<width;x++)if(planes[pl][y*stride+x]!=expected[off+y*width+x])mismatch++;
   }
   decoded++;
  }
 }
 enc->Uninitialize();dec->Uninitialize();WelsDestroySVCEncoder(enc);WelsDestroyDecoder(dec);
 if(decoded!=3||!bytes||!mismatch)return 10;
 std::printf("ENCODE_DECODE_OK entropy=%d frames=%d bytes=%zu differing_pixels=%zu hash=%lx\n",entropy,decoded,bytes,mismatch,hash);return 0;
}
int main(){int rc=run(0);return rc?rc:run(1);}
