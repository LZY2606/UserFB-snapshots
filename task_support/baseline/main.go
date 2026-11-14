package main

import (
 "fmt"
 "io"
 "net/http"
 "net/http/httptest"
 "reflect"
 "sync/atomic"

 "github.com/hu17889/go_spider/core/common/page"
 "github.com/hu17889/go_spider/core/common/request"
 "github.com/hu17889/go_spider/core/downloader"
 "github.com/hu17889/go_spider/core/spider"
)

type processor struct { finished int }
func (p *processor) Process(pg *page.Page) { pg.AddField("body",pg.GetBodyStr()) }
func (p *processor) Finish() { p.finished++ }
func main() {
 const payload="EXPORT\nBLOCK_A\nBLOCK_B\n"
 var requests atomic.Int32
 srv:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){
  requests.Add(1); w.Header().Set("ETag",`"export-v1"`);w.Header().Set("Accept-Ranges","bytes")
  w.Header().Set("Content-Type","text/plain; charset=utf-8");io.WriteString(w,payload)
 }));defer srv.Close()
 pr:=&processor{};sp:=spider.NewSpider(pr,"local-export").SetThreadnum(1).CloseStrace()
 req:=request.NewRequest(srv.URL+"/export","text","","GET","",nil,nil,nil,nil)
 items:=sp.GetByRequest(req);if items==nil{panic("no pipeline output")}
 body,ok:=items.GetItem("body");if !ok||body!=payload||pr.finished!=1||requests.Load()!=1{panic("text production path failed")}
 fmt.Printf("TEXT_PIPELINE_OK bytes=%d requests=%d finish=%d\n",len(body),requests.Load(),pr.finished)
 _,cfg:=reflect.TypeOf(req).MethodByName("SetFileTarget")
 _,result:=reflect.TypeOf(&page.Page{}).MethodByName("GetFilePath")
 before:=requests.Load()
 fileReq:=request.NewRequest(srv.URL+"/export","file","","GET","",nil,nil,nil,nil)
 filePage:=downloader.NewHttpDownloader().Download(fileReq)
 fmt.Printf("FILE_MODE configuration=%v result=%v network=%d body_bytes=%d\n",cfg,result,requests.Load()-before,len(filePage.GetBodyStr()))
 fmt.Printf("UNCONFIGURED_FILE_LEGACY_SUCCESS=%v\n",filePage.IsSucc())
}
