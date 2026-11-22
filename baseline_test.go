package mp3

import (
 "bytes"
 "encoding/binary"
 "fmt"
 "io"
 "os"
 "strings"
 "testing"
 "github.com/hajimehoshi/go-mp3/internal/frameheader"
)

// These probes document the fixed starting tree; they do not implement free-format decoding.
func TestBaseline(t *testing.T) {
 for _, name := range []string{"classic", "mpeg2"} {
  raw, err := os.ReadFile("example/"+name+".mp3"); if err != nil {t.Fatal(err)}
  var run []byte; var offsets []int; base, frameBytes, rate := 0, 0, 0
  for p:=0; p+4<=len(raw); {
   h:=frameheader.FrameHeader(binary.BigEndian.Uint32(raw[p:p+4]))
   if !h.IsValid() || h.Layer()!=1 || h.BitrateIndex()==0 {p++;continue}
   size, e:=h.FrameSize(); if e!=nil || size<4 || p+size>len(raw) {p++;continue}
   b:=size-int(h.PaddingBit())
   if len(offsets)>0 && b!=base {run=nil;offsets=nil}
   base=b; frameBytes=h.BytesPerFrame();rate,_=h.SamplingFrequencyValue()
   offsets=append(offsets,len(run));run=append(run,raw[p:p+size]...);p+=size
   if len(offsets)==32 {break}
  }
  if len(offsets)<3 {t.Fatalf("%s: no suitable continuous frames",name)}
  d,e:=NewDecoder(bytes.NewReader(run));if e!=nil {t.Fatal(e)}
  pcm,e:=io.ReadAll(d);if e!=nil {t.Fatal(e)}
  if len(pcm)!=len(offsets)*frameBytes || bytes.Equal(pcm,make([]byte,len(pcm))) {t.Fatalf("bad real PCM %d",len(pcm))}
  free:=append([]byte(nil),run...);for _,p:=range offsets {free[p+2]&=0x0f}
  fd,fe:=NewDecoder(bytes.NewReader(free))
  if fe==nil {got,e:=io.ReadAll(fd);if e!=nil || !bytes.Equal(got,pcm){t.Fatal("accepted free-format changed PCM",e)}} else if !strings.Contains(fe.Error(),"free bitrate format is not supported") {t.Fatal(fe)}
  fmt.Printf("MP3_PRODUCTION name=%s frames=%d rate=%d pcm_bytes=%d free_supported=%t\n",name,len(offsets),rate,len(pcm),fe==nil)
 }
}
