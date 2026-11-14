package rtmp

import (
 "bytes"
 "encoding/binary"
 "fmt"
 "io"
 "net"
 "testing"
 "time"
 "github.com/nareix/joy4/codec/aacparser"
 "github.com/nareix/joy4/format/flv/flvio"
)

type authorConn struct { in *bytes.Reader; out bytes.Buffer }
func (c *authorConn) Read(p []byte) (int,error) { return c.in.Read(p) }
func (c *authorConn) Write(p []byte) (int,error) { return c.out.Write(p) }
func (c *authorConn) Close() error { return nil }
func (c *authorConn) LocalAddr() net.Addr { return nil }
func (c *authorConn) RemoteAddr() net.Addr { return nil }
func (c *authorConn) SetDeadline(time.Time) error { return nil }
func (c *authorConn) SetReadDeadline(time.Time) error { return nil }
func (c *authorConn) SetWriteDeadline(time.Time) error { return nil }

// Independent short fmt0 wire builder; these baseline messages fit one chunk.
func authorWire(csid byte, sid uint32, typ byte, ms uint32, body []byte) []byte {
 if len(body)>128 { panic("baseline wire too large") }
 b:=make([]byte,12+len(body)); b[0]=csid
 b[1]=byte(ms>>16);b[2]=byte(ms>>8);b[3]=byte(ms)
 b[4]=byte(len(body)>>16);b[5]=byte(len(body)>>8);b[6]=byte(len(body));b[7]=typ
 binary.LittleEndian.PutUint32(b[8:12],sid);copy(b[12:],body);return b
}
func authorAMF(vals ...interface{}) []byte {
 n:=0; for _,v:=range vals {n+=flvio.LenAMF0Val(v)}
 b:=make([]byte,n); off:=0;for _,v:=range vals {off+=flvio.FillAMF0Val(b[off:],v)};return b
}
func TestAuthorBaseline(t *testing.T) {
 wire:=authorWire(3,0,20,0,authorAMF("connect",float64(1),flvio.AMFMap{"app":"live","tcUrl":"rtmp://local/live"}))
 wire=append(wire,authorWire(3,0,20,0,authorAMF("createStream",float64(2),nil))...)
 wire=append(wire,authorWire(4,0,20,0,authorAMF("createStream",float64(3),nil))...)
 wire=append(wire,authorWire(5,2,20,0,authorAMF("publish",float64(0),nil,"camera-b","live"))...)
 transport:=&authorConn{in:bytes.NewReader(wire)}
 server:=NewConn(transport);server.isserver=true
 if err:=server.readConnect();err!=nil {t.Fatal(err)}
 reply:=NewConn(&authorConn{in:bytes.NewReader(transport.out.Bytes())})
 ids:=[]float64{}
 for {err:=reply.pollMsg();if err==io.EOF {break};if err!=nil {t.Fatal(err)}
  if reply.gotcommand&&reply.commandname=="_result"&&(reply.commandtransid==2||reply.commandtransid==3) {
   id,ok:=reply.commandparams[0].(float64);if !ok||id<1 {t.Fatal("bad createStream reply")};ids=append(ids,id)
  }
 }
 if len(ids)!=2 {t.Fatalf("missing replies: %v",ids)}
 fmt.Printf("CREATE_STREAM_IDS=%.0f,%.0f\n",ids[0],ids[1])
 codec,err:=aacparser.NewCodecDataFromMPEG4AudioConfigBytes([]byte{0x12,0x10});if err!=nil {t.Fatal(err)}
 media:=NewConn(&authorConn{in:bytes.NewReader(authorWire(6,1,8,40,[]byte{0xaf,1,0x11,0x22}))})
 media.stage=stageCodecDataDone;media.reading=true;media.prober.GotAudio=true
 media.prober.Streams=append(media.prober.Streams,codec)
 pkt,err:=media.ReadPacket();if err!=nil {t.Fatal(err)}
 if !bytes.Equal(pkt.Data,[]byte{0x11,0x22})||pkt.Time!=40*time.Millisecond||pkt.Idx!=0 {t.Fatalf("bad packet: %+v",pkt)}
 fmt.Printf("SINGLE_STREAM_OK time_ms=%d payload=%x\n",pkt.Time/time.Millisecond,pkt.Data)
}
