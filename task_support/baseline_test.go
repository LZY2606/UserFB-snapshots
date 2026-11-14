package task_support
import("testing";"fmt";"os";"io";"time";"github.com/nareix/joy4/av";"github.com/nareix/joy4/codec/aacparser";"github.com/nareix/joy4/format/mp4";"github.com/nareix/joy4/format/mp4/mp4io")
func TestBaseline(t *testing.T){
 c,e:=aacparser.NewCodecDataFromMPEG4AudioConfigBytes([]byte{0x12,0x10});if e!=nil{t.Fatal(e)}
 f,e:=os.CreateTemp(t.TempDir(),"clock");if e!=nil{t.Fatal(e)};defer f.Close()
 m:=mp4.NewMuxer(f);if e=m.WriteHeader([]av.CodecData{c});e!=nil{t.Fatal(e)}
 for i:=0;i<101;i++{e=m.WritePacket(av.Packet{Time:time.Duration(int64(i)*1024*1000000000/44100),Data:[]byte{byte(i),0x55}});if e!=nil{t.Fatal(e)}}
 if e=m.WriteTrailer();e!=nil{t.Fatal(e)};f.Seek(0,0)
 atoms,e:=mp4io.ReadFileAtoms(f);if e!=nil{t.Fatal(e)}
 for _,a:=range atoms{if moov,ok:=a.(*mp4io.Movie);ok{tr:=moov.Tracks[0];n:=uint64(0);runs:=tr.Media.Info.Sample.TimeToSample.Entries;for _,r:=range runs{n+=uint64(r.Count)*uint64(r.Duration)};fmt.Printf("BASELINE_MP4 samples=101 ticks=%d timescale=%d tail=%d\n",n,tr.Media.Header.TimeScale,runs[len(runs)-1].Duration);if n==0{t.Fatal("no clock")}}}
 f.Seek(0,0);dm:=mp4.NewDemuxer(f);if _,e=dm.Streams();e!=nil{t.Fatal(e)};n:=0
 for{p,e:=dm.ReadPacket();if e==io.EOF{break};if e!=nil{t.Fatal(e)};if len(p.Data)!=2{t.Fatal("payload")};n++};if n!=101{t.Fatal(n)};fmt.Printf("BASELINE_READBACK packets=%d\n",n)
}
