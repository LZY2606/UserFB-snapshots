#include "ALACEncoder.h"
#include "ALACDecoder.h"
#include "ALACBitUtilities.h"
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdint>
static int roundtrip(unsigned depth) {
    const unsigned frames=512,channels=2,width=depth/8;
    std::vector<uint8_t> input(frames*channels*width),encoded(input.size()*4+1024,0),decoded(input.size()+64,0);
    for(unsigned i=0;i<frames;i++) {
        int32_t t=int32_t((i*19)%401)-200;
        int32_t pair[2]={t*61,int32_t(t*43+(int(i%13)-6)*3)};
        for(unsigned c=0;c<channels;c++) {
            int32_t v=depth==24?pair[c]*256+int32_t((i*(17+c*12)+c*19)&255):pair[c];
            if(width==2) { int16_t s=int16_t(v);std::memcpy(&input[(i*channels+c)*width],&s,2); }
            else for(unsigned j=0;j<3;j++) input[(i*channels+c)*width+j]=uint8_t(uint32_t(v)>>(8*j));
        }
    }
    AudioFormatDescription pcm={},out={};
    pcm.mSampleRate=out.mSampleRate=44100;pcm.mFormatID=kALACFormatLinearPCM;
    pcm.mFormatFlags=kALACFormatFlagIsSignedInteger|kALACFormatFlagIsPacked;
    pcm.mChannelsPerFrame=out.mChannelsPerFrame=channels;pcm.mBitsPerChannel=depth;
    pcm.mBytesPerPacket=pcm.mBytesPerFrame=channels*width;pcm.mFramesPerPacket=1;
    out.mFormatID=kALACFormatAppleLossless;out.mFormatFlags=depth==16?1:3;out.mFramesPerPacket=frames;
    ALACEncoder encoder;encoder.SetFrameSize(frames);
    if(encoder.InitializeEncoder(out)) return 10;
    uint32_t cookie_size=encoder.GetMagicCookieSize(channels);
    std::vector<uint8_t> cookie(cookie_size+16,0);encoder.GetMagicCookie(cookie.data(),&cookie_size);
    ALACDecoder decoder;if(decoder.Init(cookie.data(),cookie_size)) return 11;
    int32_t size=int32_t(input.size());if(encoder.Encode(pcm,pcm,input.data(),encoded.data(),&size)||size<=0) return 12;
    BitBuffer info;BitBufferInit(&info,encoded.data(),size);
    unsigned tag=BitBufferReadSmall(&info,3);BitBufferReadSmall(&info,4);BitBufferRead(&info,12);
    unsigned header=BitBufferReadSmall(&info,4);if(header&8){BitBufferRead(&info,16);BitBufferRead(&info,16);}
    if(tag!=ID_CPE || (header&1)) return 13;
    unsigned mixbits=BitBufferRead(&info,8),mixres=BitBufferRead(&info,8);
    BitBuffer bits;BitBufferInit(&bits,encoded.data(),size);uint32_t got=0;
    if(decoder.Decode(&bits,decoded.data(),frames,channels,&got)||got!=frames) return 14;
    if(std::memcmp(input.data(),decoded.data(),input.size())) { std::puts("CONTENT_MISMATCH baseline_pcm");return 42; }
    std::printf("ALAC_COMPRESSED_ROUNDTRIP_OK depth=%u frames=%u bytes=%d mixbits=%u mixres=%u\n",depth,frames,size,mixbits,mixres);
    return 0;
}
int main(){int r=roundtrip(16);if(r)return r;r=roundtrip(24);if(r)return r;std::puts("BASELINE_OK");return 0;}
