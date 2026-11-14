package task_support
import("bytes";"testing";"fmt";"io";e "github.com/at-wat/ebml-go";m "github.com/at-wat/ebml-go/mkvcore")
func TestBaseline(t *testing.T){
 type tr struct{TrackNumber uint64;TrackUID uint64;TrackType uint64;CodecID string;DefaultDuration uint64;CodecDelay uint64;Audio struct{SamplingFrequency float64;Channels uint64;BitDepth uint64}}
 type bg struct{Block e.Block;BlockDuration uint64;DiscardPadding int64}
 type cl struct{Timecode uint64;BlockGroup []bg}
 var doc struct{Segment struct{Info struct{TimecodeScale uint64};Tracks struct{TrackEntry []tr};Cluster []cl}}
 doc.Segment.Info.TimecodeScale=100000;track:=tr{TrackNumber:1,TrackUID:1,TrackType:2,CodecID:"A_PCM/INT/LIT",DefaultDuration:200000,CodecDelay:50000};track.Audio.SamplingFrequency=5000;track.Audio.Channels=1;track.Audio.BitDepth=8;doc.Segment.Tracks.TrackEntry=[]tr{track}
 doc.Segment.Cluster=[]cl{{10,[]bg{{e.Block{TrackNumber:1,Timecode:-2,Lacing:e.LacingFixed,Data:[][]byte{{0x11},{0x22},{0x33}}},6,250000}}}}
 var b bytes.Buffer;if er:=e.Marshal(&doc,&b);er!=nil{t.Fatal(er)}
 rs,er:=m.NewSimpleBlockReader(bytes.NewReader(b.Bytes()));if er!=nil{t.Fatal(er)};if len(rs)!=1{t.Fatal(len(rs))}
 n:=0;for{p,k,ts,er:=rs[0].Read();if er==io.EOF{break};if er!=nil{t.Fatal(er)};if len(p)!=1||p[0]!=byte(0x11*(n+1)){t.Fatal(p)};fmt.Printf("BASELINE_LACE frame=%d raw_tick=%d key=%v\n",n,ts,k);n++}
 rs[0].Close();if n!=3{t.Fatal(n)};fmt.Println("BASELINE_MKV frames=3 lacing=fixed")
}
