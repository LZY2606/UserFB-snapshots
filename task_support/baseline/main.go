package main

import (
 "bytes"
 "encoding/binary"
 "fmt"
 "os"
 flv "github.com/yapingcat/gomedia/go-flv"
 codec "github.com/yapingcat/gomedia/go-codec"
)
func check(ok bool, s string) {if !ok {fmt.Fprintln(os.Stderr,s);os.Exit(1)}}
func size24(b []byte)int {return int(b[0])<<16|int(b[1])<<8|int(b[2])}
func main(){
 // Minimal baseline-profile SPS: 16x16 frame, no VUI or frame crop.
 sps:=[]byte{0,0,0,1,0x67,0x42,0,0x1e,0xfb,0xc8}
 pps:=[]byte{0,0,0,1,0x68,0xce,0x3c,0x80}
 idr:=[]byte{0,0,0,1,0x65,0xb8,0x80}
 frame:=append(append(append([]byte{},sps...),pps...),idr...)
 var out bytes.Buffer;w:=flv.CreateFlvWriter(&out)
 check(w.WriteFlvHeader()==nil,"write header")
 check(w.WriteH264(frame,0x01000008,0x01000010)==nil,"write h264")
 raw:=out.Bytes();off:=13;video:=0;scripts:=0;var types []byte
 for off<len(raw) {
  check(off+15<=len(raw),"tag header")
  n:=size24(raw[off+1:off+4]);end:=off+11+n;check(end+4<=len(raw),"tag body")
  check(binary.BigEndian.Uint32(raw[end:end+4])==uint32(11+n),"previous size")
  ts:=uint32(raw[off+7])<<24|uint32(size24(raw[off+4:off+7]));check(ts==0x01000010,"timestamp")
  if raw[off]==9 {video++;types=append(types,raw[off+12]);if raw[off+12]==1 {check(size24(raw[off+13:off+16])==0xfffff8,"signed CTS bytes")}};if raw[off]==18 {scripts++}
  off=end+4
 }
 check(video==2&&scripts==0&&bytes.Equal(types,[]byte{0,1}),"tag types")
 fmt.Printf("DEFAULT_FLV_VIDEO_TAGS=%d packet_types=%v script_tags=%d\n",video,types,scripts)
 frames:=0;r:=flv.CreateFlvReader();r.OnFrame=func(cid codec.CodecID,b []byte,pts,dts uint32){
  check(cid==codec.CODECID_VIDEO_H264&&bytes.Contains(b,[]byte{0x65,0xb8,0x80}),"read payload")
  check(dts==0x01000010,"read DTS");frames++
 }
 // Input may convert AVCC payload in place, so use a distinct copy.
 check(r.Input(append([]byte{},raw...))==nil,"read flv");check(frames==1,"read count")
 fmt.Printf("ROUNDTRIP_FRAMES=%d writer_cts=-8 dts=%d\n",frames,uint32(0x01000010))
 fmt.Println("BASELINE_OK")
}
