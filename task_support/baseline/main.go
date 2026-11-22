package main

import (
 "bytes"
 "fmt"
 "github.com/bluenviron/mediacommon/v2/pkg/codecs/mpeg4audio"
 "github.com/bluenviron/mediacommon/v2/pkg/formats/fmp4"
 "github.com/bluenviron/mediacommon/v2/pkg/formats/fmp4/seekablebuffer"
 "github.com/bluenviron/mediacommon/v2/pkg/formats/mp4/codecs"
)
type tracked struct {seekablebuffer.Buffer; pos int64; backward bool}
func (w *tracked) Write(p []byte)(int,error){n,e:=w.Buffer.Write(p);w.pos+=int64(n);return n,e}
func (w *tracked) Seek(off int64,whence int)(int64,error){p,e:=w.Buffer.Seek(off,whence);if e==nil {if p<w.pos{w.backward=true};w.pos=p};return p,e}
func must(e error){if e!=nil{panic(e)}}
func main(){
 init:=fmp4.Init{Tracks:[]*fmp4.InitTrack{
  {ID:1,TimeScale:90000,Codec:&codecs.H264{SPS:[]byte{0x67,0x42,0xc0,0x28,0xd9,0,0x78,2,0x27,0xe5,0x84,0,0,3,0,4,0,0,3,0,0xf0,0x3c,0x60,0xc9,0x20},PPS:[]byte{0x08}}},
  {ID:2,TimeScale:48000,Codec:&codecs.MPEG4Audio{Config:mpeg4audio.AudioSpecificConfig{Type:2,SampleRate:48000,ChannelConfig:2}}},
 }}
 var ib seekablebuffer.Buffer;must(init.Marshal(&ib));var decodedInit fmp4.Init;must(decodedInit.Unmarshal(bytes.NewReader(ib.Bytes())))
 if len(decodedInit.Tracks)!=2||decodedInit.Tracks[0].TimeScale!=90000||decodedInit.Tracks[1].TimeScale!=48000{panic("init tracks")}
 a,b:=[]byte{0,0,0,2,0x65,0xb1},[]byte{0xa1,0xa2}
 part:=fmp4.Part{SequenceNumber:7,Tracks:[]*fmp4.PartTrack{{ID:1,BaseTime:9000,Samples:[]*fmp4.Sample{{Duration:3000,PTSOffset:-300,Payload:a}}},{ID:2,BaseTime:4800,Samples:[]*fmp4.Sample{{Duration:1024,Payload:b}}}}}
 var out tracked;must(part.Marshal(&out));var ps fmp4.Parts;must(ps.Unmarshal(out.Bytes()))
 if len(ps)!=1||ps[0].SequenceNumber!=7||len(ps[0].Tracks)!=2{panic("part header")}
 for i,expected:=range []struct{base uint64;duration uint32;cts int32;payload []byte}{{9000,3000,-300,a},{4800,1024,0,b}}{t:=ps[0].Tracks[i];if t.ID!=i+1||t.BaseTime!=expected.base||len(t.Samples)!=1{panic("track")};s:=t.Samples[0];if s.Duration!=expected.duration||s.PTSOffset!=expected.cts||!bytes.Equal(s.Payload,expected.payload){panic("sample")}}
 if !out.backward {panic("production writer behavior changed")}
 fmt.Println("BASELINE init_tracks=2 seq=7 tracks=2 DTS=9000/4800 duration=3000/1024 CTS=-300/0 payloads=6/2")
 fmt.Printf("PRODUCTION Part.Marshal backward_seek=%t\n",out.backward)
}
