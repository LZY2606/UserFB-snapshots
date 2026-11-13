package main
import (
 "fmt"
 "reflect"
 "github.com/hu17889/go_spider/core/common/page"
 "github.com/hu17889/go_spider/core/common/request"
 "github.com/hu17889/go_spider/core/spider"
)
type processor struct { n int; finished bool }
func(p *processor) Process(v *page.Page){p.n++; if v.GetRequest().GetUrl()=="https://fixture.invalid/root" {v.AddTargetRequest("https://fixture.invalid/child","text")}}
func(p *processor) Finish(){p.finished=true}
type download struct{}
func(download) Download(r *request.Request)*page.Page{return page.NewPage(r).SetBodyStr("captured:"+r.GetUrl())}
func main(){p:=new(processor);s:=spider.NewSpider(p,"baseline").CloseStrace().SetDownloader(download{}).SetThreadnum(1);s.AddUrl("https://fixture.invalid/root","text").Run();if p.n!=2||!p.finished{panic("production crawl failed")};_,found:=reflect.TypeOf(s).MethodByName("RunCheckpoint");if found{panic("baseline expected missing feature")};fmt.Printf("production pages=%d finish=%v RunCheckpoint=%v\n",p.n,p.finished,found)}
