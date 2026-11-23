package pubsub

import (
    "fmt"
    "io"
    "testing"
    "time"
    "github.com/notedit/rtmp/av"
)

type stubReader struct { packets []av.Packet }
func (r *stubReader) ReadPacket() (av.Packet,error) {
    if len(r.packets)==0 { return av.Packet{},io.EOF }
    p:=r.packets[0];r.packets=r.packets[1:];return p,nil
}
type stubWriter struct { packets []av.Packet }
func (w *stubWriter) WritePacket(p av.Packet) error {
    w.packets=append(w.packets,p)
    if p.Type==av.AAC { return io.EOF };return nil
}
func TestBaseline(t *testing.T) {
    s:=&PubSub{}
    s.SetPub(&stubReader{packets:[]av.Packet{
        {Type:av.Metadata,Data:[]byte("meta")},
        {Type:av.H264DecoderConfig,Data:[]byte{1,2}},
        {Type:av.AACDecoderConfig,Data:[]byte{3,4}},
        {Type:av.H264,IsKeyFrame:true,Time:100*time.Millisecond,CTime:-5*time.Millisecond,Data:[]byte{5}},
        {Type:av.AAC,Time:120*time.Millisecond,Data:[]byte{6}},
    }})
    w:=&stubWriter{}
    s.AddSub(make(chan bool),w)
    want:=[]int{av.Metadata,av.H264DecoderConfig,av.H264,av.AACDecoderConfig,av.AAC}
    if len(w.packets)!=len(want) {t.Fatalf("wire packets=%d",len(w.packets))}
    for i,v:=range want {if w.packets[i].Type!=v {t.Fatalf("packet %d type=%d",i,w.packets[i].Type)}}
    if w.packets[2].Time!=100*time.Millisecond || w.packets[4].Time!=120*time.Millisecond || w.packets[2].CTime!=-5*time.Millisecond {t.Fatal("media times changed")}
    if len(w.packets[1].Data)!=2 || w.packets[2].Data[0]!=5 || w.packets[3].Data[0]!=3 || w.packets[4].Data[0]!=6 {t.Fatal("payload/config changed")}
    fmt.Println("BASELINE cached=2 wire=5 keyDTS=100ms audioDTS=120ms cts=-5ms")
}
