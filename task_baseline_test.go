package rtp

import (
 "encoding/binary"
 "fmt"
 "net"
 "testing"
 "time"
)
func baselineSession() *Session {
 return &Session{streamsIn:make(streamInMap),streamsOut:make(streamOutMap),remotes:make(remoteMap),conflicts:make(conflictMap),MaxNumberInStreams:100,rtcpServiceActive:true,ctrlEventChan:make(CtrlEventChan,2)}
}
func TestBaseline(t *testing.T) {
 addr:=Address{IPAddr:net.IPv4(127,0,0,1),DataPort:4000,CtrlPort:4001}
 s:=baselineSession()
 out:=newSsrcStreamOut(&addr,0x11111111,1);out.streamStatus=active;s.streamsOut[0]=out
 remote:=newSsrcStreamOut(&addr,0x22222222,100)
 if !remote.SetPayloadType(0){t.Fatal("payload type rejected")}
 in:=newSsrcStreamIn(&addr,remote.Ssrc());in.streamStatus=active;s.streamsIn[0]=in
 for i:=0;i<4;i++{
  p:=remote.newDataPacket(uint32(i*160));p.SetPayload([]byte{1,2,3,4})
  in.recordReceptionData(p,s,time.Unix(100,0).Add(time.Duration(i)*20*time.Millisecond).UnixNano());p.FreePacket()
 }
 if in.statistics.packetCount==0 {t.Fatal("no RTP accepted")}
 rc:=s.buildRtcpPkt(out,1)
 if rc.Type(0)!=RtcpRR || rc.Count(0)!=1 {t.Fatalf("bad RR: %d count %d",rc.Type(0),rc.Count(0))}
 rr:=rc.toRecvReport(8)
 if rr.ssrc()!=remote.Ssrc(){t.Fatal("RR has wrong media source")}
 // A normal compound with an independently constructed receiver-time XR.
 xr:=[]byte{0x80,207,0,4,0x11,0x11,0x11,0x11,4,0,0,2,0xe0,0,0,0,0x40,0,0,0}
 copy(rc.buffer[rc.inUse:],xr);rc.inUse+=len(xr)
 rc.fromAddr=addr
 peer:=baselineSession()
 peerOut:=newSsrcStreamOut(&addr,remote.Ssrc(),1);peerOut.streamStatus=active;peer.streamsOut[0]=peerOut
 raw:=append([]byte(nil),rc.Buffer()[:rc.InUse()]...)
 if !peer.OnRecvCtrl(rc) {t.Fatal("production OnRecvCtrl rejected normal RR+SDES+XR")}
 events:=<-peer.ctrlEventChan
 xrEvents:=0
 for _,e:=range events {if e.EventType==RtcpXr {xrEvents++}}
 if peerOut.HighestSeqNo!=in.statistics.seqNumAccum+uint32(in.statistics.maxSeqNum){t.Fatal("RR did not reach output statistics")}
 fmt.Printf("BASELINE_OK RTP_count=%d RR_count=1 RR_ssrc=%08x RR_highest=%d XR_events=%d wire_bytes=%d\n",in.statistics.packetCount,binary.BigEndian.Uint32(raw[8:12]),peerOut.HighestSeqNo,xrEvents,len(raw))
}
