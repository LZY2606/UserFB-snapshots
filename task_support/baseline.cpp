#include <unordered_map>
#include "mpegts_demuxer.h"
#include "mpegts_muxer.h"
#include <cstdio>
#include <fstream>
#include <iterator>
using namespace mpegts;
int main() {
    std::ifstream file("task_support/two_pictures.h264",std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
    if(bytes.empty())return 1;
    MpegTsDemuxer d;
    MpegTsMuxer m({{0x1b,256}},100,256,MpegTsMuxer::MuxType::segmentType);
    SimpleBuffer in;m.createPat(in,100,0);m.createPmt(in,{{0x1b,256}},100,0);d.decode(in);
    int count=0;bool same=false;
    d.esOutCallback=[&](const EsFrame& f) {
        ++count;
        same=f.mCompleted&&!f.mBroken&&f.mPts==9000&&f.mData->size()==bytes.size()
            &&std::equal(bytes.begin(),bytes.end(),f.mData->data());
    };
    EsFrame frame(0x1b,256);frame.mStreamId=0xe0;frame.mPts=frame.mDts=9000;
    frame.mData->append(bytes.data(),bytes.size());
    SimpleBuffer transport;m.createPes(frame,transport);d.decode(transport);
    std::printf("PES_CALLBACKS=%d H264_BYTES=%zu PAYLOAD_EXACT=%d\n",count,bytes.size(),same);
    if(count!=1||!same)return 2;
    std::puts("BASELINE_OK");return 0;
}
