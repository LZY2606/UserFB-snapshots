package main
import (
 "fmt"
 "io"
 "net/http"
 "net/http/httptest"
 "net/url"
 "sync/atomic"
 fb "github.com/PuerkitoBio/fetchbot"
)
type keyed struct{*fb.Cmd}
func(c *keyed)CoalesceKey()string{return "page-A"}
func main(){
 var requests,callbacks int32
 server:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){atomic.AddInt32(&requests,1);w.Header().Set("X-Source","normal");fmt.Fprint(w,"captured-data")}));defer server.Close()
 f:=fb.New(fb.HandlerFunc(func(c *fb.Context,r *http.Response,e error){if e!=nil {panic(e)};b,e:=io.ReadAll(r.Body);if e!=nil||string(b)!="captured-data"||r.Header.Get("X-Source")!="normal"{panic("bad production response")};atomic.AddInt32(&callbacks,1)}))
 f.DisablePoliteness=true;f.CrawlDelay=0;q:=f.Start();u,_:=url.Parse(server.URL+"/page")
 for i:=0;i<3;i++{if e:=q.Send(&keyed{&fb.Cmd{U:u,M:"GET"}});e!=nil{panic(e)}}
 if e:=q.Close();e!=nil{panic(e)}
 fmt.Printf("BASELINE keyed_commands=3 network=%d callbacks=%d body=ok\n",requests,callbacks)
 if requests!=3||callbacks!=3 {panic("unexpected upstream behavior")}
}
