package task_support
import("bytes";"context";"fmt";"testing";a "github.com/asticode/go-astits")
func TestBaseline(t *testing.T){
 var b bytes.Buffer;m:=a.NewMuxer(context.Background(),&b);if e:=m.AddElementaryStream(a.PMTElementaryStream{ElementaryPID:256,StreamType:a.StreamTypeH264Video});e!=nil{t.Fatal(e)};m.SetPCRPID(256)
 _,e:=m.WriteData(&a.MuxerData{PID:256,AdaptationField:&a.PacketAdaptationField{HasPCR:true,PCR:&a.ClockReference{Base:(1<<33)-5,Extension:299}},PES:&a.PESData{Data:[]byte{0x55,0x66,0x77},Header:&a.PESHeader{StreamID:0xe0,OptionalHeader:&a.PESOptionalHeader{MarkerBits:2,PTSDTSIndicator:a.PTSDTSIndicatorOnlyPTS,PTS:&a.ClockReference{Base:7}}}}});if e!=nil{t.Fatal(e)}
 d:=a.NewDemuxer(context.Background(),bytes.NewReader(b.Bytes()),a.DemuxerOptPacketSize(188));tables,pes:=0,0
 for{v,e:=d.NextData();if e==a.ErrNoMorePackets{break};if e!=nil{t.Fatal(e)};if v.PAT!=nil||v.PMT!=nil{tables++};if v.PES!=nil{pes++;if !bytes.Equal(v.PES.Data,[]byte{0x55,0x66,0x77}){t.Fatalf("payload %x",v.PES.Data)};fmt.Printf("BASELINE_RAW_CLOCK pts=%d pcr_base=%d pcr_ext=%d\n",v.PES.Header.OptionalHeader.PTS.Base,v.FirstPacket.AdaptationField.PCR.Base,v.FirstPacket.AdaptationField.PCR.Extension)}}
 if tables!=2||pes!=1{t.Fatal(tables,pes)};fmt.Printf("BASELINE_TS tables=%d pes=%d bytes=%d\n",tables,pes,b.Len())
}
