package rtp
import("bytes";"fmt";"testing";"github.com/pion/rtp/codecs")
func TestBaseline(t *testing.T){
 // Low overhead sequence header, temporal delimiter, frame OBU.
 seq:=[]byte{0x0a,0x02,0x11,0x22};frame:=append([]byte{0x32,0x14},bytes.Repeat([]byte{0x55},20)...)
 input:=append(append(append([]byte{},seq...),0x12,0),frame...)
 want:=append(append([]byte{},seq...),frame...)
 var p codecs.AV1Payloader;packets:=p.Payload(8,input)
 if len(packets)<3{t.Fatalf("expected fragmentation: %x",packets)}
 var d codecs.AV1Depacketizer;var got []byte
 for _,packet:=range packets{if len(packet)>8{t.Fatal("MTU")};b,err:=d.Unmarshal(packet);if err!=nil{t.Fatal(err)};got=append(got,b...)}
 if !bytes.Equal(got,want){t.Fatalf("got=%x want=%x",got,want)}
 fmt.Println("BASELINE_OK fragmented-av1-roundtrip")
}

// Current wire-level defect witness; not a target implementation.
func TestBaselineLayerMix(t *testing.T) {
 input:=[]byte{0x2e,0x00,1,0x11,0x2e,0x20,1,0x22,0x2e,0x40,1,0x33}
 var p codecs.AV1Payloader
 packets:=p.Payload(100,input)
 want:=[]byte{0x20,3,0x2c,0x20,0x22,0x2c,0x40,0x33}
 if len(packets)!=2 || !bytes.Equal(packets[1],want){t.Fatalf("current layer witness changed: %x",packets)}
 fmt.Printf("CURRENT_LAYER_MIX packet=%x temporal_ids=1,2\n",packets[1])
 var d codecs.AV1Depacketizer
 out,err:=d.Unmarshal([]byte{0,0})
 if err!=nil || len(out)!=0 {t.Fatalf("zero W=0 element contract: %x %v",out,err)}
}
