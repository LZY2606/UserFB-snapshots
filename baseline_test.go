//go:build baselinecheck

package gos7

import (
 "bytes"
 "encoding/binary"
 "io"
 "net"
 "testing"
 "time"
)

func TestMultiBaseline(t *testing.T) {
 for _, short := range []bool{false,true} {
  a,b:=net.Pipe()
  h:=NewTCPClientHandler("127.0.0.1:102",0,1); h.conn=a; h.PDULength=480; h.IdleTimeout=0; h.Timeout=time.Second
  done:=make(chan error,1)
  payload:=[]byte{255,4,0,8,0x11,0,255,4,0,16,0x22,0x33}
  if short {payload=payload[:len(payload)-1]}
  reply:=append([]byte{3,0,0,0,2,0xf0,0x80,0x32,3,0,0,0,1,0,2,0,0,0,0,4,2},payload...)
  binary.BigEndian.PutUint16(reply[2:4],uint16(len(reply)));binary.BigEndian.PutUint16(reply[15:17],uint16(len(payload)))
  go func(){defer b.Close(); head:=make([]byte,4);_,e:=io.ReadFull(b,head);if e==nil {body:=make([]byte,int(binary.BigEndian.Uint16(head[2:]))-4);_,e=io.ReadFull(b,body)};if e==nil {_,e=b.Write(reply)};done<-e}()
  items:=[]S7DataItem{{Area:s7areadb,WordLen:s7wlbyte,DBNumber:1,Amount:1,Data:[]byte{0xee}},{Area:s7areadb,WordLen:s7wlword,DBNumber:1,Start:2,Amount:1,Data:[]byte{0xee,0xee}}}
  e:=NewClient(h).AGReadMulti(items,2);h.Close();if e!=nil {t.Fatal(e)};if e=<-done;e!=nil {t.Fatal(e)}
  want:=[]byte{0x22,0x33};if short {want=[]byte{0x22,0}}
  if !bytes.Equal(items[0].Data,[]byte{0x11}) || !bytes.Equal(items[1].Data,want) {t.Fatalf("items=%x/%x",items[0].Data,items[1].Data)}
  if short {t.Log("BASELINE_TRUNCATED_MULTI accepted=2200 error=nil")} else {t.Log("BASELINE_NORMAL_MULTI values=11,2233")}
 }
}
