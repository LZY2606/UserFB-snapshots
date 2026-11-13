package main
import (
 "bufio"
 "bytes"
 "fmt"
 "io"
 "net/http"
 "net/http/httptest"
 "strings"
 warc "github.com/nlnwa/gowarc/v3"
)
func main(){
 body:=strings.Repeat("capture-",512)
 server:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){w.Header().Set("Content-Type","text/plain");w.Header().Set("Content-Length","4096");io.WriteString(w,body)}));defer server.Close()
 c:=&http.Client{Transport:&http.Transport{Proxy:nil}};resp,err:=c.Get(server.URL+"/capture");if err!=nil{panic(err)};defer resp.Body.Close()
 builder:=warc.NewRecordBuilder(warc.Response);builder.AddWarcHeader(warc.WarcTargetURI,server.URL+"/capture");builder.AddWarcHeader(warc.WarcDate,"2026-01-01T00:00:00Z");builder.AddWarcHeader(warc.ContentType,"application/http;msgtype=response")
 if err=resp.Write(builder);err!=nil{panic(err)}
 record,_,err:=builder.Build();if err!=nil{panic(err)};defer record.Close()
 var out bytes.Buffer;next,n,err:=warc.NewMarshaler().Marshal(&out,record,512);if err!=nil{panic(err)}
 if n!=int64(out.Len())||n<=512||next!=nil{panic("unexpected baseline maxSize behavior")}
 captured,_,_,err:=warc.NewUnmarshaler().Unmarshal(bufio.NewReader(bytes.NewReader(out.Bytes())));if err!=nil{panic(err)};defer captured.Close()
 h,ok:=captured.Block().(warc.HttpResponseBlock);if !ok{panic("lost HTTP type")};r,err:=h.PayloadBytes();if err!=nil{panic(err)};got,err:=io.ReadAll(r);if err!=nil||string(got)!=body{panic(fmt.Sprintf("capture payload mismatch len=%d err=%v",len(got),err))}
 fmt.Printf("captured=%d cap=512 cap-ignored=%v continuation=%v serialized=%d\n",len(got),n>512,next!=nil,n)
}
