package main
import (
 "bytes"
 "errors"
 "fmt"
 "net"
 rtp "github.com/wernerd/GoRTP/src/net/rtp/v2"
)
type memoryTransport struct { fail bool; wire []byte; calls int }
func (m *memoryTransport) WriteDataTo(p *rtp.DataPacket,a *rtp.Address)(int,error){
 m.calls++;if m.fail{return 0,errors.New("local simulated unavailable destination")}
 m.wire=append([]byte(nil),p.Buffer()[:p.InUse()]...);return p.InUse(),nil
}
func(m *memoryTransport) WriteCtrlTo(p *rtp.CtrlPacket,a *rtp.Address)(int,error){return p.InUse(),nil}
func(m *memoryTransport) SetToLower(rtp.TransportWrite){}
func(m *memoryTransport) CloseWrite(){}
func(m *memoryTransport) ListenOnTransports()error{return nil}
func(m *memoryTransport) OnRecvData(*rtp.DataPacket)bool{return true}
func(m *memoryTransport) OnRecvCtrl(*rtp.CtrlPacket)bool{return true}
func(m *memoryTransport) SetCallUpper(rtp.TransportRecv){}
func(m *memoryTransport) CloseRecv(){}
func(m *memoryTransport) SetEndChannel(rtp.TransportEnd){}
func main(){
 for _,fail:=range []bool{false,true}{
  m:=&memoryTransport{fail:fail};s:=rtp.NewSession(m,m)
  a:=&rtp.Address{IPAddr:net.IPv4(127,0,0,1),DataPort:8000,CtrlPort:8001}
  idx,streamErr:=s.NewSsrcStreamOut(a,7,65535);if streamErr!=""{panic(streamErr)}
  s.SsrcStreamOutForIndex(idx).SetPayloadType(0)
  if _,err:=s.AddRemote(a);err!=nil{panic(err)}
  p:=s.NewDataPacketForStream(idx,160);p.SetPayload([]byte{1,2,3})
  _,err:=s.WriteData(p);str:=s.SsrcStreamOutForIndex(idx)
  if m.calls!=1||str.SenderPacketCnt!=1||str.SenderOctectCnt!=3{panic("baseline accounting changed")}
  if fail {
   if err==nil||len(m.wire)!=0{panic("expected actual local write failure")}
   fmt.Printf("OBSERVED_FAILED_WRITE_PRECOUNTS packet=%d octets=%d\n",str.SenderPacketCnt,str.SenderOctectCnt)
  }else{
   if err!=nil||!bytes.Equal(m.wire,p.Buffer()[:p.InUse()]){panic("direct RTP output mismatch")}
   fmt.Println("BASELINE_DIRECT_RTP_OK sequence=65535 payload=010203")
  }
  p.FreePacket()
 }
}
