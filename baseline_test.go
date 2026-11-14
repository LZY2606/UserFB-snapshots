package vnc
import (
 "encoding/binary"
 "fmt"
 "testing"
 "github.com/kward/go-vnc/buttons"
 "github.com/kward/go-vnc/encodings"
)
func TestBaseline(t *testing.T) {
 SetSettle(0)
 m:=&MockConn{}
 c:=NewClientConn(m,&ClientConfig{})
 if err:=c.SetEncodings(Encodings{&RawEncoding{},&DesktopSizePseudoEncoding{}});err!=nil {t.Fatal(err)}
 m.Reset()
 // FramebufferUpdate body: padding, one DesktopSize pseudo rectangle.
 for _,v:=range []interface{}{uint8(0),uint16(1),uint16(0),uint16(0),uint16(800),uint16(600),int32(-223)} {if err:=binary.Write(m,binary.BigEndian,v);err!=nil {t.Fatal(err)}}
 if _,err:=(&FramebufferUpdate{}).Read(c);err!=nil {t.Fatal(err)}
 if c.FramebufferWidth()!=800||c.FramebufferHeight()!=600 {t.Fatal("legacy resize failed")}
 m.Reset()
 if err:=c.PointerEvent(buttons.Button(1),250,125);err!=nil {t.Fatal(err)}
 if got:=fmt.Sprintf("%x",m.b.Bytes());got!="050100fa007d" {t.Fatalf("pointer=%s",got)}
 fmt.Println("legacy desktop=800x600 pointer=050100fa007d")
 _,ok:=c.Encodable(encodings.Encoding(-308))
 fmt.Printf("extended_desktop_supported=%v\n",ok)
}
