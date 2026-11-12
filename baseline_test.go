//go:build baseline

package mqtt
import("bytes";"io";"testing")
type testDuplex struct{r *bytes.Reader;w bytes.Buffer}
func(d *testDuplex)Read(p []byte)(int,error){return d.r.Read(p)}
func(d *testDuplex)Write(p []byte)(int,error){return d.w.Write(p)}
func(d *testDuplex)Close()error{return nil}
func TestQoS2SessionBaseline(t *testing.T){
 a:=&testDuplex{r:bytes.NewReader([]byte{0x34,0x08,0,1,'a',0,17,'2','3','4'})}
 c:=&BaseClient{Transport:a};calls:=0;c.Handle(HandlerFunc(func(*Message){calls++}))
 if err:=c.serve();err!=io.EOF{t.Fatal(err)}
 if !bytes.Equal(a.w.Bytes(),[]byte{0x50,2,0,17}){t.Fatalf("PUBREC %x",a.w.Bytes())}
 b:=&testDuplex{r:bytes.NewReader([]byte{0x62,2,0,17})};c.Transport=b
 if err:=c.serve();err!=io.EOF{t.Fatal(err)}
 if calls!=0 || b.w.Len()!=0{t.Fatal("starting behavior changed")}
 t.Log("MQTT_SESSION_BASELINE_OK");t.Log("CURRENT_PENDING_LOST_AND_PUBCOMP_MISSING")
}
