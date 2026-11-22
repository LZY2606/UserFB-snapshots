package main

import (
 "bytes"
 "fmt"
 "time"
 "github.com/pion/interceptor"
 internalcc "github.com/pion/interceptor/internal/cc"
 "github.com/pion/interceptor/pkg/gcc"
 "github.com/pion/interceptor/pkg/twcc"
 "github.com/pion/rtcp"
 "github.com/pion/rtp"
)
func main() {
 must:=func(ok bool){if !ok {panic("production baseline assertion failed")}}
 adapter:=internalcc.NewFeedbackAdapter()
 bwe,err:=gcc.NewSendSideBWE(gcc.SendSideBWEPacer(gcc.NewNoOpPacer()));must(err==nil);defer bwe.Close()
 info:=&interceptor.StreamInfo{SSRC:7,PayloadType:96,ClockRate:90000,RTPHeaderExtensions:[]interceptor.RTPHeaderExtension{{URI:"http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01",ID:1}}}
 observed:=0
 sink:=interceptor.RTPWriterFunc(func(h *rtp.Header,p []byte,a interceptor.Attributes)(int,error){
  wire,e:=(&rtp.Packet{Header:*h,Payload:p}).Marshal();if e!=nil{return 0,e}
  var packet rtp.Packet;must(packet.Unmarshal(wire)==nil)
  must(packet.SequenceNumber==uint16(65535+observed)&&packet.Timestamp==uint32(uint64(0xfffffff0)+uint64(observed*900)))
  must(bytes.Equal(packet.Payload,[]byte{0x61,byte(observed)}))
  var transport rtp.TransportCCExtension;must(transport.Unmarshal(packet.GetExtension(1))==nil);must(transport.TransportSequence==uint16(observed))
  attrs:=interceptor.Attributes{};attrs.Set(internalcc.TwccExtensionAttributesKey,uint8(1))
  must(adapter.OnSent(time.Unix(1000,0).Add(time.Duration(observed)*time.Millisecond),&packet.Header,len(packet.Payload),attrs)==nil)
  observed++;return len(wire),nil
 })
 extensions:=&twcc.HeaderExtensionInterceptor{}
 writer:=extensions.BindLocalStream(info,bwe.AddStream(info,sink))
 for i:=0;i<2;i++ {n,e:=writer.Write(&rtp.Header{Version:2,PayloadType:96,SSRC:7,SequenceNumber:uint16(65535+i),Timestamp:uint32(uint64(0xfffffff0)+uint64(i*900))},[]byte{0x61,byte(i)},nil);must(e==nil&&n==22)}
 feedback:=&rtcp.TransportLayerCC{Header:rtcp.Header{Type:rtcp.TypeTransportSpecificFeedback,Count:rtcp.FormatTCC},SenderSSRC:99,MediaSSRC:7,BaseSequenceNumber:0,PacketStatusCount:2,ReferenceTime:10,PacketChunks:[]rtcp.PacketStatusChunk{&rtcp.RunLengthChunk{Type:rtcp.TypeTCCRunLengthChunk,PacketStatusSymbol:rtcp.TypeTCCPacketReceivedSmallDelta,RunLength:2}},RecvDeltas:[]*rtcp.RecvDelta{{Type:rtcp.TypeTCCPacketReceivedSmallDelta,Delta:1000},{Type:rtcp.TypeTCCPacketReceivedSmallDelta,Delta:1000}}}
 feedback.Header.Length=uint16(feedback.MarshalSize()/4-1)
 wire,err:=feedback.Marshal();must(err==nil)
 pkts,err:=rtcp.Unmarshal(wire);must(err==nil&&len(pkts)==1)
 acks,err:=adapter.OnTransportCCFeedback(time.Unix(1001,0),pkts[0].(*rtcp.TransportLayerCC));must(err==nil&&len(acks)==2)
 must(acks[0].SequenceNumber==0&&acks[1].SequenceNumber==1&&acks[0].Size==22&&!acks[0].Arrival.IsZero())
 must(bwe.WriteRTCP(pkts,nil)==nil&&observed==2)
 fmt.Printf("PRODUCTION_GCC packets=%d bytes_each=22 rtp_seq=65535,0 twcc=0,1 feedback_acks=%d arrival_gap_us=%d\n",observed,len(acks),acks[1].Arrival.Sub(acks[0].Arrival).Microseconds())
}
