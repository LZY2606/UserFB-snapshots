package main

import (
	"fmt"
	"net/http"
	"net/http/httptest"
	"reflect"
	"sync/atomic"

	"github.com/hu17889/go_spider/core/common/page"
	"github.com/hu17889/go_spider/core/common/request"
	"github.com/hu17889/go_spider/core/downloader"
	"github.com/hu17889/go_spider/core/spider"
)

type processor struct{ seen, finished int32 }

func (p *processor) Process(result *page.Page) {
	if !result.IsSucc() || result.GetJson() == nil || result.GetJson().Get("record").MustString() != "catalog" {
		panic("production JSON result missing")
	}
	result.AddField("record", "catalog")
	atomic.AddInt32(&p.seen, 1)
}
func (p *processor) Finish() { atomic.AddInt32(&p.finished, 1) }

func main() {
	var hits int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&hits, 1)
		w.Header().Set("Content-Type", "application/json; charset=utf-8")
		fmt.Fprint(w, `{"record":"catalog","next":"detail/7"}`)
	}))
	defer srv.Close()
	r := request.NewRequest(srv.URL+"/catalog", "json", "primary", "GET", "", nil, nil, nil, "logical-meta")
	dl := downloader.NewHttpDownloader()
	result := dl.Download(r)
	if !result.IsSucc() || result.GetJson() == nil || result.GetJson().Get("record").MustString() != "catalog" || result.GetRequest() != r {
		panic("direct download failed")
	}
	fmt.Println("BASELINE_JSON_OK record=catalog identity=original")
	proc := &processor{}
	items := spider.NewSpider(proc, "mirror-baseline").SetThreadnum(2).CloseStrace().GetByRequest(r)
	if items == nil || atomic.LoadInt32(&proc.seen) != 1 || atomic.LoadInt32(&proc.finished) != 1 {
		panic("Spider did not process and finish exactly once")
	}
	if value, ok := items.GetItem("record"); !ok || value != "catalog" || atomic.LoadInt32(&hits) != 2 {
		panic("pipeline output or request count differs")
	}
	fmt.Println("BASELINE_SPIDER_OK processed=1 finished=1 total_http=2")
	_, mirrors := reflect.TypeOf(*r).FieldByName("MirrorURLs")
	_, capSetter := reflect.TypeOf(dl).MethodByName("SetMaxConcurrentRequests")
	fmt.Printf("MIRROR_CONFIG_PRESENT=%t PHYSICAL_CAP_SETTER_PRESENT=%t\n", mirrors, capSetter)
}
