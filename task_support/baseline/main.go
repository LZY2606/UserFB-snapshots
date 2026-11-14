package main

import (
 "fmt"
 "io"
 "net/http"
 "net/http/httptest"
 "reflect"
 "strings"
 "sync"
 "sync/atomic"
 "time"

 gc "github.com/PuerkitoBio/gocrawl"
 "github.com/PuerkitoBio/goquery"
)

type noticeBody struct { io.ReadCloser; remaining int; reached chan struct{}; once sync.Once }
func (b *noticeBody) Read(p []byte) (int,error) {
 n,err:=b.ReadCloser.Read(p); b.remaining-=n
 if b.remaining<=0 { b.once.Do(func(){close(b.reached)}) }; return n,err
}
type observer struct { gc.DefaultExtender; prefixRead chan struct{}; prefixLen int; visits atomic.Int32 }
func (e *observer) Fetch(c *gc.URLContext, a string, head bool) (*http.Response,error) {
 r,err:=e.DefaultExtender.Fetch(c,a,head)
 if err==nil && c.URL().Path=="/directory" {r.Body=&noticeBody{ReadCloser:r.Body,remaining:e.prefixLen,reached:e.prefixRead}}
 return r,err
}
func (e *observer) Visit(c *gc.URLContext,r *http.Response,d *goquery.Document) (interface{},bool) {
 if d==nil { panic("missing DOM") }; e.visits.Add(1); return nil,true
}
func main() {
 var child atomic.Int32
 leaf:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){
  if r.URL.Path=="/robots.txt" {io.WriteString(w,"User-agent: *\nAllow: /\n"); return}
  if r.URL.Path!="/item" {panic("unexpected leaf path")}; child.Add(1); io.WriteString(w,"<html>article</html>")
 })); defer leaf.Close()
 prefix:="<html><head><base href=\""+leaf.URL+"/\"></head><body><a href=\"item\">article</a>"
 release:=make(chan struct{}); var releaseOnce sync.Once
 defer releaseOnce.Do(func(){close(release)})
 parent:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){
  if r.URL.Path=="/robots.txt" {io.WriteString(w,"User-agent: *\nAllow: /\n");return}
  if r.URL.Path!="/directory" {panic("unexpected parent path")}
  w.Header().Set("Content-Type","text/html"); io.WriteString(w,prefix); w.(http.Flusher).Flush()
  <-release; io.WriteString(w,"</body></html>")
 })); defer parent.Close()
 ext:=&observer{prefixRead:make(chan struct{}),prefixLen:len(prefix)}
 opts:=gc.NewOptions(ext); opts.CrawlDelay=0; opts.WorkerIdleTTL=0; opts.SameHostOnly=false;opts.URLNormalizationFlags=0;opts.LogFlags=0
 _,available:=reflect.TypeOf(*opts).FieldByName("StreamLinks")
 fmt.Printf("STREAM_OPTION_PRESENT=%v\n",available)
 done:=make(chan error,1);go func(){done<-gc.NewCrawlerWithOptions(opts).Run(parent.URL+"/directory")}()
 select {case <-ext.prefixRead: case <-time.After(10*time.Second): panic("prefix not read")}
 before:=child.Load(); fmt.Printf("BEFORE_EOF child_requests=%d\n",before)
 releaseOnce.Do(func(){close(release)})
 select {case err:=<-done:if err!=nil{panic(err)};case <-time.After(10*time.Second):panic("crawl stuck")}
 fmt.Printf("AFTER_EOF child_requests=%d visits=%d\n",child.Load(),ext.visits.Load())
 if before!=0 || child.Load()!=1 || ext.visits.Load()!=2 {panic("unexpected baseline")}
 if !strings.Contains(prefix,"base") {panic("invalid fixture")}
}
