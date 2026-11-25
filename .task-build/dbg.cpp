#include "wels/codec_api.h"
#include <cstdio>
#include <cstring>
#include <vector>
static const int W=64,H=64;
static int g_pcm=0;
static void cb(void*,int,const char*s){ if(s){ if(strstr(s,"I_PCM"))++g_pcm; fprintf(stderr,"LOG: %s\n",s);} }
static void fill(std::vector<unsigned char>&p,int frame){
 for(size_t i=0;i<p.size();++i){unsigned x=(unsigned)i*1664525u+1013904223u+(unsigned)frame*977u;p[i]=(unsigned char)((x^(x>>13)^(x>>23))&255);}
}
int main(int argc,char**argv){
 int entropy = argc>1?atoi(argv[1]):1;
 int slices = argc>2?atoi(argv[2]):1;
 ISVCEncoder*enc=0; WelsCreateSVCEncoder(&enc);
 int lvl=WELS_LOG_QUIET; enc->SetOption(ENCODER_OPTION_TRACE_LEVEL,&lvl);
 SEncParamExt p; memset(&p,0,sizeof p); enc->GetDefaultParams(&p);
 p.iUsageType=CAMERA_VIDEO_REAL_TIME;p.iPicWidth=W;p.iPicHeight=H;p.fMaxFrameRate=25;p.iTargetBitrate=200000;
 p.iRCMode=RC_OFF_MODE;p.iTemporalLayerNum=1;p.iSpatialLayerNum=1;p.bEnableDenoise=false;p.bEnableFrameSkip=false;
 p.bEnableBackgroundDetection=false;p.bEnableAdaptiveQuant=false;p.bEnableSceneChangeDetect=false;
 p.iMultipleThreadIdc=1;p.iLoopFilterDisableIdc=1;p.iEntropyCodingModeFlag=entropy;
 p.sSpatialLayers[0].iVideoWidth=W;p.sSpatialLayers[0].iVideoHeight=H;p.sSpatialLayers[0].fFrameRate=25;
 p.sSpatialLayers[0].iSpatialBitrate=200000;p.sSpatialLayers[0].uiProfileIdc=entropy?PRO_MAIN:PRO_BASELINE;p.sSpatialLayers[0].iDLayerQp=28;
 p.sSpatialLayers[0].sSliceArgument.uiSliceMode=slices>1?SM_FIXEDSLCNUM_SLICE:SM_SINGLE_SLICE;
 if(slices>1)p.sSpatialLayers[0].sSliceArgument.uiSliceNum=slices;
 enc->InitializeExt(&p);
 int mbw=W/16,mbh=H/16;
 std::vector<unsigned char> map(mbw*mbh,0);
 const char* which = argc>3?argv[3]:"mix";
 if(!strcmp(which,"first")){ map[0]=1; }
 else if(!strcmp(which,"last")){ map[mbw*mbh-1]=1; }
 else { map[0]=1; map[mbw*mbh-1]=1; map[5]=1; }
 SPcmMbMap pm; memset(&pm,0,sizeof pm); pm.pMap=map.data();pm.iMbWidth=mbw;pm.iMbHeight=mbh;pm.iStride=mbw;
 enc->SetOption(ENCODER_OPTION_PCM_MB_MAP,&pm);

 ISVCDecoder*dec=0; WelsCreateDecoder(&dec);
 int dl=WELS_LOG_DEBUG; dec->SetOption(DECODER_OPTION_TRACE_LEVEL,&dl);
 WelsTraceCallback f=cb; dec->SetOption(DECODER_OPTION_TRACE_CALLBACK,&f);
 SDecodingParam dp; memset(&dp,0,sizeof dp); dp.sVideoProperty.eVideoBsType=VIDEO_BITSTREAM_AVC; dp.eEcActiveIdc=ERROR_CON_DISABLE;
 dec->Initialize(&dp);

 std::vector<unsigned char> raw(W*H*3/2);
 for(int fr=0;fr<3;fr++){
  fill(raw,fr);
  SSourcePicture pic; memset(&pic,0,sizeof pic); pic.iColorFormat=videoFormatI420;pic.iPicWidth=W;pic.iPicHeight=H;
  pic.iStride[0]=W;pic.iStride[1]=pic.iStride[2]=W/2;
  pic.pData[0]=raw.data();pic.pData[1]=raw.data()+W*H;pic.pData[2]=raw.data()+W*H*5/4;pic.uiTimeStamp=fr*40;
  SFrameBSInfo out; memset(&out,0,sizeof out);
  int r=enc->EncodeFrame(&pic,&out);
  std::vector<unsigned char> bs;
  for(int l=0;l<out.iLayerNum;l++){SLayerBSInfo&L=out.sLayerInfo[l];int len=0;for(int k=0;k<L.iNalCount;k++)len+=L.pNalLengthInByte[k];bs.insert(bs.end(),L.pBsBuf,L.pBsBuf+len);}
  unsigned char*planes[3]={0,0,0}; SBufferInfo bi; memset(&bi,0,sizeof bi);
  DECODING_STATE ds=dec->DecodeFrameNoDelay(bs.data(),(int)bs.size(),planes,&bi);
  printf("fr=%d enc=%d bs=%zu ds=%d bufstat=%d pcm=%d\n",fr,r,bs.size(),(int)ds,bi.iBufferStatus,g_pcm);
  if(ds!=dsErrorFree){
    // try one more decode to see repeated logs only
  }
 }
 enc->Uninitialize();WelsDestroySVCEncoder(enc);
 dec->Uninitialize();WelsDestroyDecoder(dec);
 return 0;
}
