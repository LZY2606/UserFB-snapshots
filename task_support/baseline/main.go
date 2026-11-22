package main

import (
 "bytes"
 "context"
 "fmt"
 "reflect"
 "github.com/asticode/go-astits"
)
func must(err error) { if err != nil { panic(err) } }
func main() {
 var out bytes.Buffer
 m:=astits.NewMuxer(context.Background(), &out)
 must(m.AddElementaryStream(astits.PMTElementaryStream{ElementaryPID:256, StreamType:astits.StreamTypeH264Video}))
 m.SetPCRPID(256)
 payload:=bytes.Repeat([]byte{0x65,0x23,0x47,0x81},100)
 n,err:=m.WriteData(&astits.MuxerData{PID:256,AdaptationField:&astits.PacketAdaptationField{RandomAccessIndicator:true},PES:&astits.PESData{Data:payload,Header:&astits.PESHeader{StreamID:0xe0,OptionalHeader:&astits.PESOptionalHeader{PTSDTSIndicator:astits.PTSDTSIndicatorOnlyPTS,PTS:&astits.ClockReference{Base:90000}}}}})
 must(err)
 if n!=out.Len() || n%188!=0 { panic("TS length") }
 d:=astits.NewDemuxer(context.Background(),bytes.NewReader(out.Bytes()),astits.DemuxerOptPacketSize(188))
 pat,pmt,media:=0,0,0
 for { x,e:=d.NextData(); if e==astits.ErrNoMorePackets { break }; must(e); if x.PAT!=nil {pat++};if x.PMT!=nil {pmt++}; if x.PES!=nil {media++; if !bytes.Equal(x.PES.Data,payload)||x.PES.Header.OptionalHeader.PTS.Base!=90000 {panic("PES content")}} }
 if pat!=1||pmt!=1||media!=1 {panic(fmt.Sprint(pat,pmt,media))}
 _,has:=reflect.TypeOf(d).MethodByName("BuildSeekIndex")
 if has {panic("upstream capability unexpectedly present")}
 fmt.Printf("BASELINE TS_packets=%d PAT=%d PMT=%d PES=%d payload=400 PTS90k=90000\n",n/188,pat,pmt,media)
 fmt.Println("CAPABILITY BuildSeekIndex=absent; current sequential demux verified")
}
