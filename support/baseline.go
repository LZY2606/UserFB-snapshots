package main
import("fmt";"bytes";"io";"github.com/golang/snappy")
func main(){
 raw:=bytes.Repeat([]byte("a"),10);encoded:=snappy.Encode(nil,raw)
 got,err:=snappy.Decode(nil,encoded);if err!=nil||!bytes.Equal(got,raw){panic("block")}
 alternative:=[]byte{10,0,'a',21,1};got,err=snappy.Decode(nil,alternative)
 if err!=nil||!bytes.Equal(got,raw)||len(encoded)<=len(alternative){panic("short copy witness")}
 var b bytes.Buffer;w:=snappy.NewBufferedWriter(&b)
 for _,p:=range [][]byte{raw[:3],raw[3:]}{if _,err=w.Write(p);err!=nil{panic(err)}}
 if err=w.Close();err!=nil{panic(err)};got,err=io.ReadAll(snappy.NewReader(&b))
 if err!=nil||!bytes.Equal(got,raw){panic("stream")}
 fmt.Printf("greedy=%d valid-shorter=%d\n",len(encoded),len(alternative))
 fmt.Println("BASELINE PASS")
}
