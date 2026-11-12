package main
import (
 "encoding/binary"
 "fmt"
 "net"
 "github.com/gopacket/gopacket"
 "github.com/gopacket/gopacket/ip6defrag"
 "github.com/gopacket/gopacket/layers"
)
func wire(offset uint16,more bool,payload string) []byte {
 b:=make([]byte,64+len(payload));b[0]=0x60;b[6]=0;b[7]=64
 copy(b[8:24],net.ParseIP("2001:db8::1").To16());copy(b[24:40],net.ParseIP("2001:db8::2").To16())
 binary.BigEndian.PutUint16(b[4:6],uint16(len(b)-40))
 b[40]=60;b[48]=44;b[56]=17;v:=offset<<3;if more {v|=1};binary.BigEndian.PutUint16(b[58:60],v);binary.BigEndian.PutUint32(b[60:64],71);copy(b[64:],payload);return b
}
func main() {
 d:=ip6defrag.NewIPv6Defragmenter(); var out *layers.IPv6
 for _,b:=range [][]byte{wire(1,false,"tail"),wire(0,true,"12345678")} {
  p:=gopacket.NewPacket(b,layers.LayerTypeIPv6,gopacket.Default)
  ip,ok:=p.Layer(layers.LayerTypeIPv6).(*layers.IPv6);if !ok {panic("no IPv6")}
  fg,ok:=p.Layer(layers.LayerTypeIPv6Fragment).(*layers.IPv6Fragment);if !ok {panic("no fragment")}
  if x:=d.DefragIPv6(ip,fg);x!=nil {out=x}
 }
 if out==nil || string(out.Payload)!="12345678tail" {panic("wrong baseline bytes")}
 fmt.Printf("BASELINE_IPV6_REASSEMBLED=%s\nLEGACY_EXTENSION_STATE=next:%d,hop:%t,length:%d\n",out.Payload,out.NextHeader,out.HopByHop!=nil,out.Length)
}
