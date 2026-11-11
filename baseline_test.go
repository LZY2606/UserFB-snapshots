package astits
import("bytes";"context";"fmt";"testing")
func TestBaseline(t *testing.T){
 var wire bytes.Buffer;m:=NewMuxer(context.Background(),&wire)
 if err:=m.AddElementaryStream(PMTElementaryStream{ElementaryPID:0x120,StreamType:StreamTypeH264Video});err!=nil{t.Fatal(err)}
 if err:=m.AddElementaryStream(PMTElementaryStream{ElementaryPID:0x121,StreamType:StreamTypeSCTE35});err!=nil{t.Fatal(err)}
 m.SetPCRPID(0x120);if _,err:=m.WriteTables();err!=nil{t.Fatal(err)}
 d:=NewDemuxer(context.Background(),bytes.NewReader(wire.Bytes()),DemuxerOptPacketSize(188))
 pat,err:=d.NextData();if err!=nil||pat.PAT==nil{t.Fatalf("PAT %v",err)}
 pmt,err:=d.NextData();if err!=nil||pmt.PMT==nil{t.Fatalf("PMT %v",err)}
 streams:=pmt.PMT.ElementaryStreams
 if len(streams)!=2||streams[1].ElementaryPID!=0x121||streams[1].StreamType!=StreamTypeSCTE35{t.Fatalf("streams=%+v",streams)}
 fmt.Println("BASELINE_OK pat-pmt-scte-stream-declaration")
}
