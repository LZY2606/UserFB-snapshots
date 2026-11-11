package mp4_test
import (
 "bytes"
 "encoding/binary"
 "fmt"
 "testing"
 mp4 "github.com/abema/go-mp4"
)
func TestSeedFragments(t *testing.T) {
 word:=func(n uint32)[]byte {b:=make([]byte,4);binary.BigEndian.PutUint32(b,n);return b}
 box:=func(typ string, payload ...[]byte)[]byte {b:=append([]byte(typ),bytes.Join(payload,nil)...);return append(word(uint32(4+len(b))),b...)}
 tfhd:=box("tfhd",word(0x020018),word(7),word(10),word(3))
 tfdt:=box("tfdt",word(0),word(100))
 trun1:=box("trun",word(1),word(1),word(0))
 trun2:=box("trun",word(0),word(1))
 moof:=box("moof",box("traf",tfhd,tfdt,trun1,trun2))
 binary.BigEndian.PutUint32(trun1[len(trun1)-4:],uint32(len(moof)+8))
 moof=box("moof",box("traf",tfhd,tfdt,trun1,trun2))
 input:=append(moof,box("mdat",[]byte("abcDEF"))...)
 runs,err:=mp4.ExtractBoxWithPayload(bytes.NewReader(input),nil,mp4.BoxPath{mp4.BoxTypeMoof(),mp4.BoxTypeTraf(),mp4.BoxTypeTrun()})
 if err!=nil || len(runs)!=2 {t.Fatalf("runs: %v %d",err,len(runs))}
 p,err:=mp4.Probe(bytes.NewReader(input));if err!=nil {t.Fatal(err)}
 if len(p.Segments)!=1 || p.Segments[0].TrackID!=7 || p.Segments[0].SampleCount!=1 || p.Segments[0].BaseMediaDecodeTime!=100 {t.Fatalf("current probe: %#v",p.Segments)}
 if !bytes.Equal(input[len(moof)+8:],[]byte("abcDEF")) {t.Fatal("mdat marker")}
 fmt.Println("SEED_FRAGMENTS_OK raw_runs=2 legacy_count=1 payload=abcDEF")
}
