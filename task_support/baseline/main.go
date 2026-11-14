package main
import("encoding/binary";"fmt";"github.com/q191201771/lal/pkg/base";"github.com/q191201771/lal/pkg/rtsp";"github.com/q191201771/lal/pkg/rtprtcp";"github.com/q191201771/lal/pkg/sdp")
type observer struct{ packets []base.AvPacket; raw int }
func(o *observer)OnSdp(sdp.LogicContext){}
func(o *observer)OnRtpPacket(rtprtcp.RtpPacket){o.raw++}
func(o *observer)OnAvPacket(p base.AvPacket){o.packets=append(o.packets,p)}
type writer struct{n int}
func(w *writer)WriteInterleavedPacket(b []byte,ch int)error{w.n++;return nil}
func rtp(pt byte,seq uint16,ts,ssrc uint32,p []byte)[]byte{b:=make([]byte,12+len(p));b[0]=0x80;b[1]=pt;binary.BigEndian.PutUint16(b[2:],seq);binary.BigEndian.PutUint32(b[4:],ts);binary.BigEndian.PutUint32(b[8:],ssrc);copy(b[12:],p);return b}
func sr(ssrc,rtp,sec,frac uint32)[]byte{b:=make([]byte,28);b[0]=0x80;b[1]=200;binary.BigEndian.PutUint16(b[2:],6);for i,v:=range []uint32{ssrc,sec,frac,rtp,1,1}{binary.BigEndian.PutUint32(b[4+4*i:],v)};return b}
func main(){
 ctx,err:=sdp.ParseSdp2LogicContext([]byte("v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=clock\r\nt=0 0\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=control:v\r\nm=audio 0 RTP/AVP 8\r\na=rtpmap:8 PCMA/8000\r\na=control:a\r\n"));if err!=nil{panic(err)}
 o:=&observer{};w:=&writer{};s:=rtsp.NewBaseInSessionWithObserver(base.SessionTypeRtspPub,w,o);s.InitWithSdp(ctx);if s.SetupWithChannel("v",0,1)!=nil||s.SetupWithChannel("a",2,3)!=nil{panic("setup")}
 s.HandleInterleavedPacket(rtp(96,1,90000,111,[]byte{0x65,1,2}),0)
 s.HandleInterleavedPacket(rtp(8,1,8000,222,[]byte{1,2,3}),2)
 // SR says that the audio first frame is 250ms after the video first frame.
 s.HandleInterleavedPacket(sr(111,90000,3900000000,0),1)
 s.HandleInterleavedPacket(sr(222,8000,3900000000,1<<30),3)
 s.HandleInterleavedPacket(rtp(96,2,93600,111,[]byte{0x41,3,4}),0)
 s.HandleInterleavedPacket(rtp(8,2,8320,222,[]byte{4,5,6}),2)
 var at,vt int64=-1,-1;for _,p:=range o.packets{if p.IsAudio()&&at<0{at=p.Timestamp};if p.IsVideo()&&vt<0{vt=p.Timestamp}}
 if o.raw!=4||at!=0||vt!=0||w.n<2{panic(fmt.Sprintf("unexpected baseline raw=%d audio=%d video=%d rr=%d",o.raw,at,vt,w.n))}
 fmt.Printf("BASELINE_OK raw=%d audio_first_ms=%d video_first_ms=%d sr_offset_ms=250 legacy_offset_ms=%d rr=%d\n",o.raw,at,vt,at-vt,w.n)
}
