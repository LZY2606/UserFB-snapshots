* Build status: <a href="https://scan.coverity.com/projects/ireader-media-server"> <img alt="Coverity Scan Build Status" src="https://scan.coverity.com/projects/14645/badge.svg"/> </a>
* Build Dependence: https://github.com/ireader/sdk
 
# libflv
1. FLV video codec: H.264/H.265/H.266/AV1/VP8/VP9/VP10
2. FLV audio codec: AAC/MP3/G.711/Opus
3. FLV file read/write
4. H.264/H.265/H.266/AV1 bitstream filter: annex-b <-> mp4 stream
5. AAC bitstream filter: ADTS <-> ASC

# librtmp
1. rtmp-client: RTMP publish/play
2. rtmp-server: RTMP Server live/vod streaming

# libmpeg
1. ITU-T H.222.0 PS/TS read/write
2. ps/ts codec: H.264/H.265/H.266/AAC/MP3/G.711/Opus

# librtp
1. RFC3550 RTP/RTCP
2. RTP with H.264/H.265/H.266/MPEG-2/MPEG-4/VP8/VP9/AV1
3. RTP with G.711/G.726/G.729/MP3/AAC/Opus
4. RTP with MPEG-2 PS/TS
5. RTP Header Extension
6. RTCP PSFB/RTPFB/XR
7. H.264 interleaved mode (RFC6184 packetization-mode=2) de-interleaving

### H.264 interleaved mode
The default H.264 decoder (`rtp_h264_decode()`, `rtp_payload_decode_create()`)
keeps handing NAL units downstream in transmission order. Receivers that get
STAP-B / MTAP / FU-B streams (RFC6184 6.4 Interleaved Mode) can opt in to
decoding-order delivery with `rtp-h264-interleaved.h`:

```c
struct rtp_h264_interleaved_t* d = rtp_h264_interleaved_create(depth, &handler, param);
rtp_h264_interleaved_input(d, packet, bytes); // one complete RTP packet
rtp_h264_interleaved_flush(d);                // deliver what remains
rtp_h264_interleaved_destroy(d);              // free only, no callback
```

* `depth` is the maximum number of complete NAL units kept pending, range
  `[0, 32767]`. After a packet's NAL units are collected, while more than
  `depth` are pending the one with the earliest decoding order number (DON) is
  delivered, until only `depth` remain.
* `flush` delivers the remaining complete NAL units in the same order. An
  unfinished FU (no final fragment) is not a complete NAL unit and is never
  flushed.
* Input must already be in network sequence order; network reordering is not
  handled. Sequence 65535 followed by 0 counts as contiguous. Only
  interleaved-mode payloads are accepted (STAP-B, MTAP16/24, FU-B start with
  FU-A continuation); single NAL unit packets and STAP-A are discarded.
* Callback bytes are valid only for the duration of that callback.

# librtsp
1. RFC2326 RTSP
2. RFC4566 SDP
3. SDP fmtp: H.264/H.265/H.266/AAC/Opus/G.711 

# libhls
1. HLS M3U8: generate m3u8 file
2. HLS Media: TS segmenter
3. HLS fmp4 segmenter
4. HLS Master/Playlist m3u8 parser

# libdash
1. ISO/IEC 23009-1 MPEG-DASH static(vod)
2. ISO/IEC 23009-1 MPEG-DASH dynamic(live)
3. DASH MPD v3/v4 parser

# libmov
1. ISO/IEC 14496-12 MP4 File reader/writer
2. MP4 faststart(moov box before mdat)
3. fMP4(Fragment MP4) writer
4. MP4 with H.264/H.265/H.266/AV1/VP8/VP9/JPEG/PNG
5. MP4 with AAC/Opus/MP3/G.711

# libmkv
1. MKV/WebM file read/write
2. MKV/WebM live streaming

# libsip
1. sip user-agent (UAC/UAS)
2. sip with ICE

# libhttp(https://github.com/ireader/sdk)
1. HTTP Server(base AIO)
2. HTTP Client
3. HTTP Cookie

### Make
1. make clean && make
2. make RELEASE=1 (make release library, default debug)
3. make PLATFORM=arm-hisiv100nptl-linux (cross compile)

#### [编译说明](compile.cn.md)

# 友情链接

|  |  |
| ---: | :---- |
| <a href="https://github.com/ZLMediaKit/ZLMediaKit">ZLMediaKit</a> | 基于C++11的高性能运营级流媒体服务框架 |
| <a href="https://mediainsight360.com/">MediaInsight360</a> | 流媒体格式在线解析 |
|  |  |