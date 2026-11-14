package task_support
import("testing";"fmt";r "github.com/pion/rtcp")
func TestBaselineWire(t *testing.T) {
 sr:=r.SenderReport{SSRC:77,NTPTime:0x1122334455667788,RTPTime:0xffffff00,PacketCount:3,OctetCount:480}
 data,err:=r.Marshal([]r.Packet{&sr});if err!=nil{t.Fatal(err)}
 packets,err:=r.Unmarshal(data);if err!=nil||len(packets)!=1{t.Fatalf("decode SR: %v",err)}
 got,ok:=packets[0].(*r.SenderReport);if !ok||got.NTPTime!=sr.NTPTime||got.RTPTime!=sr.RTPTime{t.Fatal("sender report fields")}
 block:=r.ReceptionReport{SSRC:77,FractionLost:64,TotalLost:0xffffff,LastSequenceNumber:65538,Jitter:273,LastSenderReport:uint32(got.NTPTime>>16),Delay:98304}
 rr:=r.ReceiverReport{SSRC:8,Reports:[]r.ReceptionReport{block}}
 data,err=r.Marshal([]r.Packet{&rr});if err!=nil{t.Fatal(err)}
 packets,err=r.Unmarshal(data);if err!=nil||len(packets)!=1{t.Fatalf("decode RR: %v",err)}
 decoded,ok:=packets[0].(*r.ReceiverReport);if !ok||len(decoded.Reports)!=1||decoded.Reports[0]!=block{t.Fatalf("report block changed: %+v",packets)}
 fmt.Printf("BASELINE_SR_RR blocks=1 totalLost=%d lastSeq=%d delay=%d\n",block.TotalLost,block.LastSequenceNumber,block.Delay)
}

func TestBaselineCompoundWire(t *testing.T) {
 blocks:=make([]r.ReceptionReport,31)
 for i:=range blocks {blocks[i]=r.ReceptionReport{SSRC:uint32(i+1),LastSequenceNumber:65538}}
 rr:=&r.ReceiverReport{SSRC:8,Reports:blocks}
 compound:=r.CompoundPacket{rr,r.NewCNAMESourceDescription(8,"peer")}
 data,err:=compound.Marshal();if err!=nil{t.Fatal(err)}
 if len(data)!=768 {t.Fatalf("compound bytes=%d",len(data))}
 var parsed r.CompoundPacket;if err=parsed.Unmarshal(data);err!=nil{t.Fatal(err)}
 cname,err:=parsed.CNAME();if err!=nil||cname!="peer" {t.Fatalf("cname=%q %v",cname,err)}
 rr.Reports=append(rr.Reports,r.ReceptionReport{SSRC:32});if _,err=compound.Marshal();err==nil{t.Fatal("32 blocks should not fit one RR")}
 raw,err:=r.Marshal([]r.Packet{&r.SenderReport{SSRC:77,NTPTime:0x1122334455667788},&r.Goodbye{Sources:[]uint32{77}}});if err!=nil{t.Fatal(err)}
 packets,err:=r.Unmarshal(raw);if err!=nil||len(packets)!=2{t.Fatalf("SR/BYE %v",err)}
 if _,ok:=packets[0].(*r.SenderReport);!ok{t.Fatal("SR type")};if _,ok:=packets[1].(*r.Goodbye);!ok{t.Fatal("BYE type")}
 fmt.Println("BASELINE_COMPOUND bytes=768 blocks=31 cname=peer srBye=2")
}
