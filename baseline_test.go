package gots_test
import (
 "fmt"
 "testing"
 "github.com/Comcast/gots/v3"
 "github.com/Comcast/gots/v3/packet"
 "github.com/Comcast/gots/v3/pes"
 "github.com/Comcast/gots/v3/scte35"
)
func TestClockBaseline(t *testing.T) {
 p:=packet.New();p.SetPID(256)
 if err:=p.SetAdaptationFieldControl(packet.PayloadAndAdaptationFieldFlag);err!=nil{t.Fatal(err)}
 af,_:=p.AdaptationField();if err:=af.SetHasPCR(true);err!=nil{t.Fatal(err)}
 if err:=af.SetPCR(90000*300+17);err!=nil{t.Fatal(err)}
 h:=[]byte{0,0,1,0xe0,0,0,0x80,0x80,5,0,0,0,0,0,0xaa}
 gots.InsertPTS(h[9:14],90000)
 if _,err:=p.SetPayload(h);err!=nil{t.Fatal(err)}
 payload,err:=p.Payload();if err!=nil{t.Fatal(err)}
 hdr,err:=pes.NewPESHeader(payload);if err!=nil{t.Fatal(err)}
 clock,err:=af.PCR();if err!=nil||clock!=27000017||hdr.PTS()!=90000{t.Fatal(clock,hdr,err)}
 s:=scte35.CreateSCTE35();raw:=s.UpdateData()
 decoded,err:=scte35.NewSCTE35(append([]byte{0},raw...));if err!=nil||decoded.Command()!=scte35.SpliceNull{t.Fatal(err)}
 fmt.Println("TS_CLOCK_BASELINE_OK pts=90000 pcr=27000017 splice_null=parsed")
}
