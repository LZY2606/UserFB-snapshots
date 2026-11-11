//go:build baselinecheck

package gomavlib

import (
 "context"
 "net"
 "testing"
 "time"
 "github.com/bluenviron/gomavlib/v4/pkg/dialect"
 "github.com/bluenviron/gomavlib/v4/pkg/message"
)

type MessageAuthorSample struct { Value uint16; Counter uint8; Ext uint16 `mavext:"true"` }
func (*MessageAuthorSample) GetID() uint32 {return 200}
func testCRC(b []byte) uint16 {c:=uint16(0xffff);for _,v:=range b {x:=uint16(v)^c&255;x^=x<<4&255;c=c>>8^x<<8^x<<3^x>>4};return c}
func testWire(id byte,body []byte,extra byte) []byte {b:=append([]byte{0xfd,byte(len(body)),0,0,9,42,3,id,0,0},body...);c:=testCRC(append(append([]byte{},b[1:]...),extra));return append(b,byte(c),byte(c>>8))}
func TestTelemetryBaseline(t *testing.T) {
 remote,e:=net.ListenUDP("udp4",&net.UDPAddr{IP:net.IPv4(127,0,0,1)});if e!=nil {t.Fatal(e)};defer remote.Close()
 local,e:=net.DialUDP("udp4",nil,remote.LocalAddr().(*net.UDPAddr));if e!=nil {t.Fatal(e)}
 n:=&Node{OutVersion:V2,OutSystemID:1,HeartbeatDisable:true,Dialect:&dialect.Dialect{Messages:[]message.Message{&MessageAuthorSample{}}},Endpoints:[]Endpoint{&EndpointCustomClient{IsDatagram:true,Connect:func(context.Context)(net.Conn,error){return local,nil}}}}
 if e=n.Initialize();e!=nil {t.Fatal(e)};defer n.Close()
 next:=func() Event {select {case e:=<-n.Events():return e;case <-time.After(time.Second):t.Fatal("event timeout");return nil}}
 if _,ok:=next().(*EventChannelOpen);!ok {t.Fatal("open")}
 c:=testCRC([]byte("AUTHOR_SAMPLE uint16_t value uint8_t counter "));extra:=byte(c)^byte(c>>8)
 good:=testWire(200,[]byte{0x34,0x12,7},extra)
 if _,e=remote.WriteToUDP(good,local.LocalAddr().(*net.UDPAddr));e!=nil {t.Fatal(e)}
 ev,ok:=next().(*EventFrame);if !ok {t.Fatal("frame")};v,ok:=ev.Message().(*MessageAuthorSample)
 if !ok || v.Value!=0x1234 || v.Counter!=7 || v.Ext!=0 || ev.SystemID()!=42 || ev.ComponentID()!=3 {t.Fatalf("decoded %v",ev)}
 t.Log("BASELINE_TELEMETRY value=4660 extension=0 source=42/3")
 bad:=append([]byte{},good...);bad[10]^=1;remote.WriteToUDP(bad,local.LocalAddr().(*net.UDPAddr))
 if _,ok=next().(*EventParseError);!ok {t.Fatal("missing CRC error")}
 raw:=testWire(201,[]byte{9},0);remote.WriteToUDP(raw,local.LocalAddr().(*net.UDPAddr))
 ev,ok=next().(*EventFrame);if !ok {t.Fatal("raw frame")};r,ok:=ev.Message().(*message.MessageRaw);if !ok || r.ID!=201 || len(r.Payload)!=1 || r.Payload[0]!=9 {t.Fatal("raw")}
 t.Log("BASELINE_BAD_CRC rejected unknown_id=201 preserved")
 remote.WriteToUDP(append(append([]byte{},good...),good...),local.LocalAddr().(*net.UDPAddr));if _,ok=next().(*EventFrame);!ok {t.Fatal("first frame")}
 remote.WriteToUDP(good,local.LocalAddr().(*net.UDPAddr));skipped,ok:=next().(*EventParseError);if !ok || skipped.Error.Error()!="skipped 15 bytes" {t.Fatalf("skip event=%v",skipped)}
 if _,ok=next().(*EventFrame);!ok {t.Fatal("next datagram")};t.Log("BASELINE_DATAGRAM one_frame_per_packet skipped=15")
}
