package support_test
import(. "github.com/korandiz/mpa";"bytes";"encoding/binary";"testing";"fmt")
func TestCRCBaseline(t *testing.T){
 h:=uint32(0xffe00000|1<<20|1<<19|3<<17|4<<12|3<<6)
 f:=make([]byte,136);binary.BigEndian.PutUint32(f,h)
 for _,crc:=range []uint16{0,65535}{binary.BigEndian.PutUint16(f[4:],crc); data:=bytes.Repeat(f,3);d:=Decoder{Input:bytes.NewReader(data)};for k:=0;k<3;k++{if e:=d.DecodeFrame();e!=nil{t.Fatal(e)};if d.Layer()!=1||d.NSamples()!=384||d.NChannels()!=1{t.Fatal("header")};samples:=make([]float32,384);d.ReadSamples(0,samples);for _,v:=range samples{if v!=0{t.Fatal("nonzero silence")}}};fmt.Printf("LayerI crc=%04x frames=3 pcm=1152 zeros accepted\n",crc)}
}
