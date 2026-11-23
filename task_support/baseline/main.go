package main
import (
 "fmt"
 "net/http"
 "net/http/httptest"
 "net/url"
 "github.com/frobware/grawler"
)
func main() {
 s:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){
  switch r.URL.Path {
  case "/root": fmt.Fprint(w,`<a href="/leaf">leaf</a>`)
  case "/leaf": fmt.Fprint(w,`<p>leaf</p>`)
  case "/jump": http.Redirect(w,r,"/dir/page",http.StatusFound)
  case "/dir/page": fmt.Fprint(w,`<a href="child">child</a>`)
  default: http.NotFound(w,r)
  }
 }));defer s.Close()
 p:=grawler.Crawl(s.URL+"/root",2,grawler.NewHTTPFetcher(),func(u string)bool{return u==s.URL+"/leaf"})
 if len(p)!=2 {panic(fmt.Sprintf("normal crawl pages=%d",len(p)))}
 for _,v:=range p {if v.FetcherError!=nil {panic(v.FetcherError)}}
 fmt.Println("BASELINE_OK pages=2 linked_leaf=true")
 p=grawler.Crawl(s.URL+"/jump",2,grawler.NewHTTPFetcher(),func(string)bool{return false})
 if len(p)!=1||len(p[0].Links)!=1||p[0].FetcherError!=nil {panic("redirect production path failed")}
 u,err:=url.Parse(p[0].Links[0]);if err!=nil {panic(err)}
 fmt.Printf("CURRENT_REDIRECT relative=%s logical=/jump\n",u.Path)
}
