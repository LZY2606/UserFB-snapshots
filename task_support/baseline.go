package main
import("bytes";"fmt";"time";"github.com/notedit/rtmp/av";"github.com/notedit/rtmp/format/flv";"github.com/notedit/rtmp/format/flv/flvio";"github.com/notedit/rtmp/format/rtmp")
type wire struct{bytes.Buffer};func(w *wire)Flush()error{return nil}
func must(e error){if e!=nil{panic(e)}}
func main(){
 data:=bytes.Repeat([]byte{0,0,0,1,0x65},90)
 p:=av.Packet{Type:av.H264,Time:250*time.Millisecond,CTime:-2*time.Millisecond,IsKeyFrame:true,Data:data}
 var f bytes.Buffer;m:=flv.NewMuxer(&f);m.HasVideo=true;must(m.WritePacket(p));d:=flv.NewDemuxer(&f);a,e:=d.ReadPacket();must(e);if a.Type!=p.Type||!bytes.Equal(a.Data,data)||a.Time!=p.Time||a.CTime!=p.CTime{panic("flv mismatch")};fmt.Println("BASELINE FLV AVC payload=450 dts_ms=250 cts_ms=-2")
 w:=new(wire);tx:=rtmp.NewConn(w);tx.Stage=rtmp.StageDataStart;must(tx.WritePacket(p));rx:=rtmp.NewConn(w);rx.Stage=rtmp.StageDataStart;a,e=rx.ReadPacket();must(e);if !bytes.Equal(a.Data,data)||a.Time!=p.Time||a.CTime!=p.CTime{panic("rtmp mismatch")};fmt.Println("BASELINE RTMP chunked AVC payload=450 dts_ms=250 cts_ms=-2")
 t:=flvio.Tag{Type:flvio.TAG_VIDEO};n,e:=t.ParseHeader([]byte{0x91,'h','v','c','1',0,0,1});must(e);if n!=1||t.VideoFormat!=1{panic("upstream changed")};fmt.Println("MISSING enhanced hvc1 parsed_as_legacy_codec=1 header_bytes=1")
}
