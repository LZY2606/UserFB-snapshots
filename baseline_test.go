//go:build baselinecheck

package modbus

import (
 "bytes"
 "encoding/binary"
 "io"
 "net"
 "testing"
 "time"
)

func TestFileBaseline(t *testing.T) {
 l, e := net.Listen("tcp", "127.0.0.1:0"); if e != nil { t.Fatal(e) }; defer l.Close()
 done := make(chan error, 1)
 go func() {
  c, err := l.Accept(); if err != nil { done <- err; return }; defer c.Close()
  for _, pdu := range [][]byte{{3,2,0,42},{20,6,5,6,0,10,0,11}} {
   h := make([]byte,7); if _, err = io.ReadFull(c,h); err != nil { done <- err; return }
   body := make([]byte,int(binary.BigEndian.Uint16(h[4:6]))-1)
   if _, err = io.ReadFull(c,body); err != nil { done <- err; return }
   binary.BigEndian.PutUint16(h[4:6],uint16(len(pdu)+1)); if _, err = c.Write(append(h,pdu...)); err != nil { done <- err; return }
  }; done <- nil
 }()
 h := NewTCPClientHandler(l.Addr().String()); h.SlaveId=1; h.Timeout=time.Second; defer h.Close()
 got,e := NewClient(h).ReadHoldingRegisters(0,1); if e != nil || !bytes.Equal(got,[]byte{0,42}) { t.Fatalf("regular read %x %v",got,e) }
 req:=&ProtocolDataUnit{FunctionCode:20,Data:[]byte{7,6,0,1,0,2,0,2}}
 wire,e:=h.Encode(req); if e!=nil {t.Fatal(e)}; raw,e:=h.Send(wire); if e!=nil {t.Fatal(e)}
 if e=h.Verify(wire,raw);e!=nil {t.Fatal(e)}; p,e:=h.Decode(raw);if e!=nil || !bytes.Equal(p.Data,[]byte{6,5,6,0,10,0,11}) {t.Fatalf("raw file %v %v",p,e)}
 rtu:=&rtuPackager{SlaveId:1}; frame,_:=rtu.Encode(req)
 if n:=calculateResponseLength(frame);n!=4 {t.Fatalf("current RTU file estimate=%d",n)}
 if e=<-done;e!=nil {t.Fatal(e)}
 t.Log("BASELINE_DEVICE_REGISTER value=42")
 t.Log("BASELINE_FILE_RAW_TCP records=2 RTU_estimate=4 expected=11")
}
