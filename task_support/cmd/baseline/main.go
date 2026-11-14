package main
import("bytes";"fmt";"os";macho "github.com/blacktop/go-macho";"github.com/blacktop/go-macho/task_support/fixture")
func main(){
 plain,err:=macho.NewFile(bytes.NewReader(fixture.Image(false)));must(err)
 rs,err:=plain.GetRebaseInfo();must(err);if len(rs)!=1||rs[0].Start+rs[0].Offset!=fixture.Base+0x1100{panic("plain rebase mismatch")};fmt.Printf("BASELINE_CLASSIC rebases=%d slot=%#x\n",len(rs),rs[0].Start+rs[0].Offset)
 image:=fixture.Image(true);f,err:=macho.NewFile(bytes.NewReader(image));must(err)
 rs,err=f.GetRebaseInfo();must(err);bs,err:=f.GetBindInfo();must(err)
 fmt.Printf("OBSERVED_THREADED rebases=%d binds=%d\n",len(rs),len(bs));if len(bs)!=1{panic("unexpected baseline binds")};fmt.Printf("OBSERVED_BIND slot=%#x segment=%q file=%#x\n",bs[0].Start+bs[0].SegOffset,bs[0].Segment,bs[0].Offset())
 fmt.Printf("INPUT_UNCHANGED=%t\n",bytes.Equal(image,fixture.Image(true)))
 if len(rs)==0&&bs[0].Start==0{fmt.Println("KNOWN_GAP threaded_rebase_missing_and_bind_origin_lost")}else{fmt.Println("THREADED_BEHAVIOR_CHANGED")}
}
func must(err error){if err!=nil{fmt.Fprintln(os.Stderr,err);os.Exit(1)}}
