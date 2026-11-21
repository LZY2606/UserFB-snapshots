# gomedia
 mpeg-ts,mpeg-ps,flv,mp4,rtmp muxer/demuxer
 
## Installation
```
go get github.com/yapingcat/gomedia
```


## H264/H265/AAC/VP8/OPUS/MP3
 [USAGE](https://github.com/yapingcat/gomedia/blob/main/go-codec/README.md)
  - decode sps/pps/vps/slice header
  - decode HEVCDecoderConfigurationRecord/AVCDecoderConfigurationRecord/AAC-ADTS/AudioSpecificConfiguration
  - encode HEVCDecoderConfigurationRecord/AVCDecoderConfigurationRecord/AAC-ADTS/AudioSpecificConfiguration
  - decode OPUS Extradata(ID Head "OpusHead") /OPUS Packet(TOC...)
  - encode OPUS Extradata
  - decode VP8 Frame Tag/Key Frame Head
  - decode MP3 Frame head

## mpeg-ts
  - mux
    - H264
    - H265
    - AAC
    - MP3
  - demux
    - H264
    - H265
    - AAC
    - MP3

## mpeg-ps
  - mux 
    - H264
    - H265
    - AAC
    - G711A
    - G711U
  - demux 
    - H264
    - H265
    - AAC
    - G711A
    - G711U
   
## flv
  - mux
    - H264
    - H265
    - AAC
    - G711A
    - G711U
    - MP3
  - demux
    - H264
    - H265
    - AAC
    - G711A
    - G711U
    - MP3

### indexed flv recording
  `CreateIndexedFlvWriter` records an H.264 (+ optional AAC) FLV whose
  onMetaData script tag, including the keyframes index (times and
  filepositions), is reserved at fixed size in front of the media and
  backfilled by `Finalize`. Already written media bytes are never moved, so
  the resulting file supports keyframe seeking directly after recording.

```go
fd, _ := os.OpenFile("record.flv", os.O_CREATE|os.O_RDWR|os.O_TRUNC, 0666)
defer fd.Close()

// Keep at most 4096 keyframes in the onMetaData keyframes index.
writer, err := flv.CreateIndexedFlvWriter(fd, 4096)
if err != nil {
    panic(err)
}
if err := writer.WriteFlvHeader(); err != nil {
    panic(err)
}

// Annex-B H.264 frames and ADTS AAC frames, DTS must be non-decreasing.
// writer.WriteH264(h264Frame, pts, dts)
// writer.WriteAAC(aacFrame, pts, dts)

// Backfill duration, filesize, hasVideo, hasAudio and keyframes in place.
if err := writer.Finalize(); err != nil {
    panic(err)
}
```

  A runnable version lives in `example/example_mux_indexed_flv.go`.

  
## mp4
  - demux 
    - H264
    - H265
    - AAC
    - G711A
    - G711U
    - MP3
  - mux 
    - H264
    - H265
    - AAC
    - G711A
    - G711U
    - MP3
    - OPUS


## fmp4
  - demux 
    - H264
    - H265
    - AAC
    - G711A
    - G711U
  - mux 
    - H264
    - H265
    - AAC
    - G711A
    - G711U

## ogg
  - demux 
    - OPUS
    - VP8
  
## rtmp
  
  [USAGE](https://github.com/yapingcat/gomedia/blob/main/go-rtmp/README.md)
  
  - support client/server
  - support play/publish
  - support h264/h265/aac/g711a/g711u/mp3
  
  
## rtsp

  - support client/server(rfc2326)
  - support basic/digest
  - support rtp(rfc3550)
  - support g711/aac/h264/h265
 





  
