package main

import (
 "fmt"
 "time"
 "github.com/gwuhaolin/livego/av"
 "github.com/gwuhaolin/livego/configure"
 "github.com/gwuhaolin/livego/container/flv"
 "github.com/gwuhaolin/livego/protocol/rtmp/cache"
)
type sink struct { av.RWBaser; packets []av.Packet }
func (s *sink) Info() av.Info { return av.Info{} }
func (s *sink) Close(error) {}
func (s *sink) Write(p *av.Packet) error { q:=*p; q.Data=append([]byte(nil),p.Data...);s.packets=append(s.packets,q);return nil }
func video(ts uint32, seq bool, label byte) av.Packet {
 kind:=byte(1); if seq { kind=0 }
 p:=av.Packet{IsVideo:true, TimeStamp:ts, StreamID:1,Data:[]byte{0x17,kind,0,0,0,label}}
 if err:=flv.NewDemuxer().DemuxH(&p);err!=nil {panic(err)};return p
}
func main() {
 configure.Config.Set("gop_num", 2)
 c:=cache.NewCache()
 c.Write(video(0,true,'A'));c.Write(video(0,false,'a'))
 c.Write(video(1000,true,'B'));c.Write(video(1000,false,'b'))
 s:=&sink{RWBaser:av.NewRWBaser(time.Second)}
 if err:=c.Send(s);err!=nil{panic(err)}
 fmt.Print("CACHE_REPLAY")
 for _,p:=range s.packets{fmt.Printf(" %c@%d",p.Data[len(p.Data)-1],p.TimeStamp)}
 fmt.Println()
 if len(s.packets)<2 {panic("cache production path delivered no media")}
 fmt.Println("BASELINE_OK current cache replay; no play-start selector in upstream")
}
