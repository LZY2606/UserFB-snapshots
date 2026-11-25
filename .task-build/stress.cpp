#include "wels/codec_api.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdlib>
static int run(int W,int H,int ent,int sliceN,int allpcm){
 ISVCEncoder*e=0; WelsCreateSVCEncoder(&e);
 int q=WELS_LOG_DEBUG; e->SetOption(ENCODER_OPTION_TRACE_LEVEL,&q);
 SEncParamExt p; memset(&p,0,sizeof p); e->GetDefaultParams(&p);
 p.iUsageType=CAMERA_VIDEO_REAL_TIME;p.iPicWidth=W;p.iPicHeight=H;p.fMaxFrameRate=25;p.iTargetBitrate=400000;
 p.iRCMode=RC_OFF_MODE;p.iTemporalLayerNum=1;p.iSpatialLayerNum=1;p.bEnableDenoise=false;p.bEnableFrameSkip=false;
 p.bEnableBackgroundDetection=false;p.bEnableAdaptiveQuant=false;p.bEnableSceneChangeDetect=false;
 p.iMultipleThreadIdc=1;p.iLoopFilterDisableIdc=1;p.iEntropyCodingModeFlag=ent;
 p.sSpatialLayers[0].iVideoWidth=W;p.sSpatialLayers[0].iVideoHeight=H;p.sSpatialLayers[0].fFrameRate=25;
 p.sSpatialLayers[0].iSpatialBitrate=400000;p.sSpatialLayers[0].uiProfileIdc=ent?PRO_MAIN:PRO_BASELINE;p.sSpatialLayers[0].iDLayerQp=28;
 p.sSpatialLayers[0].sSliceArgument.uiSliceMode=sliceN>1?SM_FIXEDSLCNUM_SLICE:SM_SINGLE_SLICE;
 if(sliceN>1)p.sSpatialLayers[0].sSliceArgument.uiSliceNum=sliceN;
 if(e->InitializeExt(&p)){printf("  initfail\n");return 1;}
 int mbw=W/16,mbh=H/16; std::vector<unsigned char> m(mbw*mbh,0);
 for(int i=0;i<mbw*mbh;i++) if(allpcm||((i*7)%3==0)) m[i]=1;
 SPcmMbMap pm; memset(&pm,0,sizeof pm);pm.pMap=m.data();pm.iMbWidth=mbw;pm.iMbHeight=mbh;pm.iStride=mbw;
 if(e->SetOption(ENCODER_OPTION_PCM_MB_MAP,&pm)){printf("  setfail\n");return 1;}
 ISVCDecoder*d=0;WelsCreateDecoder(&d); d->SetOption(DECODER_OPTION_TRACE_LEVEL,&q);
 SDecodingParam dp;memset(&dp,0,sizeof dp);dp.sVideoProperty.eVideoBsType=VIDEO_BITSTREAM_AVC;dp.eEcActiveIdc=ERROR_CON_DISABLE; d->Initialize(&dp);
 std::vector<unsigned char> raw(W*H*3/2);
 std::vector<std::vector<unsigned char> > raws;
 int decoded=0;
 auto checkPic=[&](unsigned char**pl,SBufferInfo&bi,int expectIdx)->long long{
   int sY=bi.UsrData.sSystemBuffer.iStride[0],sC=bi.UsrData.sSystemBuffer.iStride[1]; long long bad=0;
   auto&src=raws[expectIdx];
   for(int my=0;my<mbh;my++)for(int mx=0;mx<mbw;mx++){ if(!m[my*mbw+mx])continue;
    for(int y=0;y<16;y++)for(int x=0;x<16;x++) if(pl[0][(my*16+y)*sY+mx*16+x]!=src[(my*16+y)*W+mx*16+x])bad++;
    for(int y=0;y<8;y++)for(int x=0;x<8;x++){ if(pl[1][(my*8+y)*sC+mx*8+x]!=src[W*H+(my*8+y)*(W/2)+mx*8+x])bad++; if(pl[2][(my*8+y)*sC+mx*8+x]!=src[W*H*5/4+(my*8+y)*(W/2)+mx*8+x])bad++; }}
   return bad;
 };
 for(int f=0;f<3;f++){
  for(size_t i=0;i<raw.size();i++){unsigned x=(unsigned)i*1664525u+1013904223u+(unsigned)f*977u;raw[i]=(x^(x>>13)^(x>>23))&255;}
  raws.push_back(raw);
  SSourcePicture pic;memset(&pic,0,sizeof pic);pic.iColorFormat=videoFormatI420;pic.iPicWidth=W;pic.iPicHeight=H;
  pic.iStride[0]=W;pic.iStride[1]=pic.iStride[2]=W/2;pic.pData[0]=raw.data();pic.pData[1]=raw.data()+W*H;pic.pData[2]=raw.data()+W*H*5/4;pic.uiTimeStamp=f*40;
  SFrameBSInfo out;memset(&out,0,sizeof out); int r=e->EncodeFrame(&pic,&out);
  int bytes=0;for(int l=0;l<out.iLayerNum;l++){SLayerBSInfo&L=out.sLayerInfo[l];for(int k=0;k<L.iNalCount;k++)bytes+=L.pNalLengthInByte[k];}
  if(r){printf("  ENCFAIL %dx%d ent=%d sl=%d all=%d f=%d r=%d\n",W,H,ent,sliceN,allpcm,f,r);return 2;}
  std::vector<unsigned char> bs; for(int l=0;l<out.iLayerNum;l++){SLayerBSInfo&L=out.sLayerInfo[l];int len=0;for(int k=0;k<L.iNalCount;k++)len+=L.pNalLengthInByte[k];bs.insert(bs.end(),L.pBsBuf,L.pBsBuf+len);}
  unsigned char*pl[3]={0,0,0};SBufferInfo bi;memset(&bi,0,sizeof bi); DECODING_STATE ds=d->DecodeFrameNoDelay(bs.data(),(int)bs.size(),pl,&bi);
  if(ds){printf("  DECFAIL ds=%d %dx%d ent=%d sl=%d all=%d f=%d bytes=%d\n",(int)ds,W,H,ent,sliceN,allpcm,f,bytes);return 3;}
  if(bi.iBufferStatus){ long long bad=checkPic(pl,bi,decoded); if(bad){printf("  PIXELBAD bad=%lld %dx%d ent=%d sl=%d all=%d outidx=%d\n",bad,W,H,ent,sliceN,allpcm,decoded);return 4;} ++decoded; }
 }
 int eos=1; d->SetOption(DECODER_OPTION_END_OF_STREAM,&eos);
 for(int a=0;a<6 && decoded<(int)raws.size();++a){
  unsigned char*pl[3]={0,0,0};SBufferInfo bi;memset(&bi,0,sizeof bi);
  DECODING_STATE ds=a?d->FlushFrame(pl,&bi):d->DecodeFrameNoDelay(0,0,pl,&bi);
  if(ds){printf("  DRAINFAIL ds=%d\n",(int)ds);return 5;}
  if(bi.iBufferStatus){ long long bad=checkPic(pl,bi,decoded); if(bad){printf("  PIXELBAD drain bad=%lld idx=%d\n",bad,decoded);return 4;} ++decoded; }
 }
 if(decoded!=(int)raws.size()){printf("  OUTCOUNT %d != %zu\n",decoded,raws.size());return 6;}
 e->Uninitialize();WelsDestroySVCEncoder(e); d->Uninitialize();WelsDestroyDecoder(d);
 printf("ok %dx%d ent=%d sl=%d all=%d\n",W,H,ent,sliceN,allpcm); return 0;
}
int main(){
 int sz[][2]={{16,16},{32,16},{16,32},{48,16},{64,48},{64,64},{176,144},{320,240},{640,480}};
 for(auto&s:sz)for(int ent=0;ent<2;ent++)for(int all=0;all<2;all++){
   if(run(s[0],s[1],ent,1,all))return 1;
   int nmb=(s[0]/16)*(s[1]/16);
   if(nmb>=4 && run(s[0],s[1],ent,4,0))return 1;
 }
 printf("STRESS_OK\n");return 0;
}
