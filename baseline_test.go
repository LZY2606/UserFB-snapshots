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

// Regression witness: three metadata OBUs with temporal_id 0, 1 and 2 must
// never be aggregated into a packet that mixes different layers.
func TestBaselineLayerMix(t *testing.T) {
 input:=[]byte{0x2e,0x00,1,0x11,0x2e,0x20,1,0x22,0x2e,0x40,1,0x33}
 var p codecs.AV1Payloader
 packets:=p.Payload(100,input)
 want:=[][]byte{
  {0x10,0x2c,0x00,0x11},
  {0x10,0x2c,0x20,0x22},
  {0x10,0x2c,0x40,0x33},
 }
 if len(packets)!=len(want){t.Fatalf("layer separation changed: %x",packets)}
 for i:=range want{if !bytes.Equal(packets[i],want[i]){t.Fatalf("layer separation changed: packet[%d]=%x want=%x",i,packets[i],want[i])}}
 // Each packet carries exactly one element (W=1), so the extension byte
 // at offset 2 is that packet's only layer. No packet mixes layers.
 for i,packet:=range packets{
  if w:=(packet[0]&0b00110000)>>4;w!=1{t.Fatalf("packet[%d] W=%d, expected a single OBU element",i,w)}
  if got:=packet[2]>>5;got!=byte(i){t.Fatalf("packet[%d] temporal_id=%d, expected %d",i,got,i)}
 }
 fmt.Printf("LAYER_SEPARATION packet0=%x packet1=%x packet2=%x temporal_ids=0,1,2\n",packets[0],packets[1],packets[2])
 var d codecs.AV1Depacketizer
 out,err:=d.Unmarshal([]byte{0,0})
 if err!=nil || len(out)!=0 {t.Fatalf("zero W=0 element contract: %x %v",out,err)}
}
