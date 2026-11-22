package main
import (
 "bytes"
 "fmt"
 "os"
 "github.com/pion/rtp"
 "github.com/pion/rtp/codecs"
)
func main() {
 dep := &codecs.H264Packet{}
 for i,payload := range [][]byte{{0x7c,0x85,0xaa},{0x7c,0x45,0xcc}} {
  seq:=uint16(10); if i==1 {seq=12}
  p:=rtp.Packet{Header:rtp.Header{Version:2,PayloadType:96,SSRC:7,SequenceNumber:seq,Timestamp:9000,Marker:i==1},Payload:payload}
  raw,err:=p.Marshal(); if err!=nil {panic(err)}
  var back rtp.Packet; if err=back.Unmarshal(raw); err!=nil {panic(err)}
  out,err:=dep.Unmarshal(back.Payload); if err!=nil {panic(err)}
  if i==0 && len(out)!=0 {os.Exit(2)}
  if i==1 && !bytes.Equal(out,[]byte{0,0,0,1,0x65,0xaa,0xcc}) {os.Exit(3)}
 }
 fmt.Println("BASELINE_RTP_WIRE_OK H264_FU_OK")
 fmt.Println("OBSERVED_PAYLOAD_ONLY_FU_HAS_NO_SEQUENCE_OR_DEADLINE_INPUT")
}
