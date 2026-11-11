package zeroconf
import("context";"fmt";"net";"testing";"time";"github.com/miekg/dns";"golang.org/x/net/ipv4")
func testPacket(instance string) *dns.Msg {
 name:=instance+"._meter._tcp.local."
 return &dns.Msg{MsgHdr:dns.MsgHdr{Response:true},Answer:[]dns.RR{
 &dns.PTR{Hdr:dns.RR_Header{Name:"_meter._tcp.local.",Rrtype:dns.TypePTR,Class:1,Ttl:120},Ptr:name},
 &dns.SRV{Hdr:dns.RR_Header{Name:name,Rrtype:dns.TypeSRV,Class:1,Ttl:120},Port:8123,Target:"meter.local."},
 &dns.TXT{Hdr:dns.RR_Header{Name:name,Rrtype:dns.TypeTXT,Class:1,Ttl:120},Txt:[]string{"unit=kW"}},
 &dns.A{Hdr:dns.RR_Header{Name:"meter.local.",Rrtype:dns.TypeA,Class:1,Ttl:120},A:net.IPv4(192,0,2,8)},}}
}
func testCollect(t *testing.T,msgs ...*dns.Msg) []*ServiceEntry{
 t.Helper();recv,err:=net.ListenUDP("udp4",&net.UDPAddr{IP:net.IPv4(127,0,0,1)});if err!=nil{t.Fatal(err)}
 c:=&client{ipv4conn:ipv4.NewPacketConn(recv)};entries:=make(chan *ServiceEntry,16)
 ctx,cancel:=context.WithTimeout(context.Background(),2*time.Second);defer cancel()
 p:=newLookupParams("","_meter._tcp","local",true,entries);go c.mainloop(ctx,p)
 sender,err:=net.DialUDP("udp4",nil,recv.LocalAddr().(*net.UDPAddr));if err!=nil{t.Fatal(err)};defer sender.Close()
 for _,m:=range append(msgs,testPacket("sentinel")){b,err:=m.Pack();if err!=nil{t.Fatal(err)};if _,err=sender.Write(b);err!=nil{t.Fatal(err)}}
 var got []*ServiceEntry
 for e:=range entries{if e.Instance=="sentinel"{cancel()}else{got=append(got,e)}}
 if ctx.Err()==context.DeadlineExceeded{t.Fatal("sentinel not received")};return got
}
func TestBaseline(t *testing.T){
 got:=testCollect(t,testPacket("sensor"));if len(got)!=1||got[0].Port!=8123||len(got[0].AddrIPv4)!=1||got[0].Text[0]!="unit=kW"{t.Fatalf("got=%+v",got)}
 fmt.Println("BASELINE_OK wire-to-service-entry")
}
