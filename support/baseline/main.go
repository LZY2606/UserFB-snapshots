package main

import (
 "bytes"
 "encoding/binary"
 "fmt"
 "io"
 "os"

 ov "github.com/jfreymuth/oggvorbis"
)

type stream struct{ io.Reader }

// Give a second copy a distinct logical-stream identity, preserving every packet.
func relabel(src []byte) []byte {
 b := append([]byte(nil), src...)
 for off := 0; off < len(b); {
  if off+27 > len(b) || string(b[off:off+4]) != "OggS" { panic("fixture framing") }
  n := int(b[off+26]); end := off+27+n
  for _, v := range b[off+27:end] { end += int(v) }
  binary.LittleEndian.PutUint32(b[off+14:], binary.LittleEndian.Uint32(b[off+14:])^0x7fffffff)
  binary.LittleEndian.PutUint32(b[off+22:],0)
  var crc uint32
  for _, v := range b[off:end] {
   crc ^= uint32(v)<<24
   for bit:=0;bit<8;bit++ { if crc&0x80000000!=0 { crc=crc<<1 ^ 0x04c11db7 } else { crc<<=1 } }
  }
  binary.LittleEndian.PutUint32(b[off+22:],crc)
  off=end
 }
 return b
}
func must(err error) { if err!=nil { panic(err) } }
func main() {
 b,err:=os.ReadFile("testdata/test.ogg");must(err)
 pcm,f,err:=ov.ReadAll(bytes.NewReader(b));must(err)
 if len(pcm)!=44100 || f.SampleRate!=44100 || f.Channels!=1 { panic("single stream mismatch") }
 fmt.Printf("SINGLE frames=%d rate=%d channels=%d\n",len(pcm),f.SampleRate,f.Channels)
 r,err:=ov.NewReader(bytes.NewReader(b));must(err);must(r.SetPosition(1000))
 out:=make([]float32,128);n,err:=r.Read(out);must(err)
 if n!=128 { panic("seek count") };for i,v:=range out { d:=v-pcm[1000+i];if d>0.00001 || d < -0.00001 { panic("seek samples") } }
 fmt.Println("SEEK frame=1000 values=128 matched")
 chain:=append(append([]byte(nil),b...),relabel(b)...)
 got,_,err:=ov.ReadAll(stream{bytes.NewReader(chain)});must(err)
 fmt.Printf("CHAIN observed_frames=%d independent_sum=%d\n",len(got),2*len(pcm))
 alt,err:=os.ReadFile("support/media/alternate.ogg");must(err)
 a,af,err:=ov.ReadAll(bytes.NewReader(alt));must(err)
 if len(a)==0 || af.Channels!=2 || af.SampleRate!=44100 {panic("alternate fixture")}
 fmt.Printf("ALTERNATE frames=%d rate=%d channels=%d\n",len(a)/af.Channels,af.SampleRate,af.Channels)
}
