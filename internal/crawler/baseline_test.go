package crawler

import (
 "io"
 "net/http"
 "net/http/httptest"
 "strings"
 "sync"
 "testing"
 "github.com/s0rg/crawley/internal/links"
)

func TestBaselineCSSLexer(t *testing.T) {
 var got []string
 links.ExtractCSS(strings.NewReader(`@import "child.css"; a {background:url(pic.png)}`), func(s string) {got=append(got,s)})
 if len(got)!=1 || got[0]!="pic.png" {t.Fatalf("legacy extraction=%v",got)}
 t.Log("LEGACY_LEXER ordinary=pic.png string_import=missing")
}

func TestBaselineCSSCrawl(t *testing.T) {
 var mu sync.Mutex
 requests:=map[string]int{}
 ts:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){
  mu.Lock(); requests[r.Method+" "+r.URL.Path]++; mu.Unlock()
  switch r.URL.Path {
  case "/":
   w.Header().Set("Content-Type","text/html")
   io.WriteString(w,`<link rel="stylesheet" href="/main.css">`)
  case "/main.css":
   w.Header().Set("Content-Type","text/css")
   io.WriteString(w,`@import "child.css"; a {background:url(pic.png)}`)
  case "/child.css":
   w.Header().Set("Content-Type","text/css")
   io.WriteString(w,`b {background:url(deep.png)}`)
  default:
   w.Header().Set("Content-Type","image/png")
  }
 }))
 defer ts.Close()
 var got []string
 c:=New(WithMaxCrawlDepth(-1), WithWorkersCount(1), WithScanCSS(true))
 if err:=c.Run(ts.URL+"/",func(s string){got=append(got,s)});err!=nil {t.Fatal(err)}
 has:=func(s string)bool{for _,u:=range got{if u==ts.URL+s{return true}};return false}
 mu.Lock(); defer mu.Unlock()
 if requests["GET /main.css"]!=1 || !has("/pic.png") {t.Fatalf("ordinary CSS path failed requests=%v output=%v",requests,got)}
 if requests["GET /child.css"]!=0 || has("/deep.png") {t.Fatalf("unexpected imported stylesheet crawl: %v %v",requests,got)}
 if requests["HEAD /pic.png"]!=1 {t.Fatalf("expected legacy asset HEAD, got %v",requests)}
 t.Log("LEGACY_CRAWL main_get=1 image_output=1 image_head=1 import_get=0")
}
