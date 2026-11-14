package main

import (
 "fmt"
 "io"
 "net/http"
 "net/http/httptest"
 "net/url"
 "os"
 "slices"
 "strings"
 "sync"
 "github.com/s0rg/crawley/internal/crawler"
 "github.com/s0rg/crawley/internal/links"
 "golang.org/x/net/html/atom"
)

func must(ok bool, text string) { if !ok { fmt.Fprintln(os.Stderr, text); os.Exit(1) } }
func main() {
 base,_:=url.Parse("http://example.test/a")
 var tags []string
 links.ExtractHTML(strings.NewReader(`<html><head><link rel="canonical" href="/article"></head><body><a href="/detail">D</a></body></html>`),base,links.HTMLParams{
  Filter:links.AllowALL,HandleHTML:func(_ atom.Atom,s string){ tags=append(tags,s) },HandleStatic:func(string){},
 })
 must(slices.Contains(tags,"http://example.test/article")&&slices.Contains(tags,"http://example.test/detail"),"unexpected extractor")
 fmt.Println("HTML_BASELINE canonical_is_ordinary_link=true")
 var mu sync.Mutex
 gets:=map[string]int{}
 ts:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){
  w.Header().Set("Content-Type","text/html")
  if r.Method==http.MethodHead{return}
  mu.Lock();gets[r.URL.Path]++;mu.Unlock()
  switch r.URL.Path {
  case "/":io.WriteString(w,`<html><body><a href="/a">A</a><a href="/b">B</a></body></html>`)
  case "/a","/b":io.WriteString(w,`<html><head><link rel=canonical href="/article"></head><body><a href="/detail">D</a></body></html>`)
  default:io.WriteString(w,`<html><body>empty</body></html>`)
  }
 }))
 defer ts.Close()
 c:=crawler.New(crawler.WithWorkersCount(2),crawler.WithRobotsPolicy(crawler.RobotsIgnore),crawler.WithMaxCrawlDepth(-1),crawler.WithoutHeads(true))
 var output []string
 err:=c.Run(ts.URL+"/",func(s string){output=append(output,strings.TrimPrefix(s,ts.URL))})
 must(err==nil,"crawler returned error");slices.Sort(output)
 must(slices.Equal(output,[]string{"/a","/article","/b","/detail"}),fmt.Sprint("unexpected output ",output))
 mu.Lock();a,b,detail:=gets["/a"],gets["/b"],gets["/detail"];mu.Unlock()
 must(a==1&&b==1&&detail==1,"unexpected production fetch counts")
 fmt.Println("CRAWL_BASELINE output=/a,/article,/b,/detail alias_GETs=2 detail_GETs=1")
}
