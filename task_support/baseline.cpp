#include "ALACEncoder.h"
#include "ALACDecoder.h"
#include "ALACBitUtilities.h"
#include <vector>
#include <cstdio>
#include <cstring>
int main(){AudioFormatDescription in={},out={};in.mSampleRate=out.mSampleRate=44100;in.mFormatID=kALACFormatLinearPCM;out.mFormatID=kALACFormatAppleLossless;in.mChannelsPerFrame=out.mChannelsPerFrame=2;in.mBytesPerPacket=in.mBytesPerFrame=4;in.mFramesPerPacket=1;in.mBitsPerChannel=16;in.mFormatFlags=kALACFormatFlagIsSignedInteger|kALACFormatFlagIsPacked;out.mFormatFlags=1;ALACEncoder e;e.SetFrameSize(512);if(e.InitializeEncoder(out))return 1;std::vector<unsigned char> pcm(2048),bits(16384),decoded(2048),cookie(e.GetMagicCookieSize(2));unsigned x=13;for(size_t i=0;i<pcm.size();i++){x=x*1664525u+1013904223u;pcm[i]=x>>24;}unsigned size=cookie.size();e.GetMagicCookie(cookie.data(),&size);int n=pcm.size();if(e.Encode(in,out,pcm.data(),bits.data(),&n))return 2;ALACDecoder d;if(d.Init(cookie.data(),size))return 3;BitBuffer b;BitBufferInit(&b,bits.data(),n);unsigned count=0;if(d.Decode(&b,decoded.data(),512,2,&count))return 4;if(count!=512||memcmp(pcm.data(),decoded.data(),2048))return 5;printf("ALAC_BASE frames=%u bytes=%d escape=%u exact=1\n",count,n,(bits[2]>>1)&1);return 0;}
