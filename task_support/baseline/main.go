package main
import (
 "fmt"
 "io"
 "net/http"
 "strings"
 "sync/atomic"
 "time"
 "github.com/gocolly/colly/v2"
 "github.com/gocolly/colly/v2/queue"
)
type transport struct{ gate chan struct{}; entered chan struct{}; active,peak atomic.Int32 }
func(t *transport)RoundTrip(r *http.Request)(*http.Response,error){
 n:=t.active.Add(1);for {p:=t.peak.Load();if n<=p || t.peak.CompareAndSwap(p,n){break}}
 t.entered<-struct{}{};<-t.gate;t.active.Add(-1)
 return &http.Response{StatusCode:200,Status:"200 OK",Header:http.Header{"Content-Type":[]string{"text/plain"}},Body:io.NopCloser(strings.NewReader("record=17")),Request:r,ContentLength:9},nil
}
func main(){
 tr:=&transport{gate:make(chan struct{}),entered:make(chan struct{},3)}
 c:=colly.NewCollector(colly.Async(true),colly.IgnoreRobotsTxt());c.SetClient(&http.Client{Transport:tr})
 var callbacks atomic.Int32
 c.OnResponse(func(r *colly.Response){if string(r.Body)!="record=17"{panic("wrong production body")};callbacks.Add(1)})
 q,e:=queue.New(1,nil);if e!=nil{panic(e)}
 for _,p:=range []string{"a","b","c"}{if e=q.AddURL("http://catalog.test/"+p);e!=nil{panic(e)}}
 result:=make(chan error,1);go func(){result<-q.Run(c)}()
 select{case e=<-result:if e!=nil{panic(e)};case <-time.After(3*time.Second):close(tr.gate);panic("old premature return no longer reproduced")}
 for i:=0;i<3;i++ {select{case <-tr.entered:case <-time.After(3*time.Second):close(tr.gate);panic("expected all 3 source requests")}}
 if tr.peak.Load()!=3 || callbacks.Load()!=0{close(tr.gate);panic("old async completion defect not reproduced")}
 fmt.Printf("Run_returned_while_blocked=true threads=1 actual_peak=%d callbacks_before_release=%d\n",tr.peak.Load(),callbacks.Load())
 close(tr.gate);c.Wait();if callbacks.Load()!=3{panic("callbacks missing after release")};fmt.Printf("after_release_callbacks=%d production_body=record17\n",callbacks.Load())
}
