package main
import("fmt";r "github.com/pion/rtcp")
func main(){
 p:=r.TransportLayerCC{Header:r.Header{Padding:true,Count:r.FormatTCC,Type:r.TypeTransportSpecificFeedback,Length:6},SenderSSRC:1,MediaSSRC:2,BaseSequenceNumber:65535,PacketStatusCount:3,ReferenceTime:1,FbPktCount:255,PacketChunks:[]r.PacketStatusChunk{&r.StatusVectorChunk{Type:r.TypeTCCStatusVectorChunk,SymbolSize:r.TypeTCCSymbolSizeTwoBit,SymbolList:[]uint16{1,0,2}}},RecvDeltas:[]*r.RecvDelta{{Type:r.TypeTCCPacketReceivedSmallDelta,Delta:1000},{Type:r.TypeTCCPacketReceivedLargeDelta,Delta:-250}}}
 b,e:=p.Marshal();if e!=nil{panic(e)};var out r.TransportLayerCC;if e=out.Unmarshal(b);e!=nil{panic(e)}
 if len(b)!=28||out.PacketStatusCount!=3||len(out.RecvDeltas)!=2||out.RecvDeltas[1].Delta!=-250{panic("roundtrip")}
 fmt.Printf("bytes=%d statuses=%d deltas=%d negative_delta=%d\n",len(b),out.PacketStatusCount,len(out.RecvDeltas),out.RecvDeltas[1].Delta)
 fmt.Println("TWCC_BASELINE_OK")
}
