package main
import (
 "fmt"
 "io"
 "net/http"
 "reflect"
 "strings"
 "time"
 fetchbot "github.com/PuerkitoBio/fetchbot"
)
type source struct { robots,pages int }
func (s *source) Do(r *http.Request)(*http.Response,error){
 text:="catalog item=17"
 if r.URL.Path=="/robots.txt" {s.robots++;text="User-agent: *\nDisallow: /private\n"} else {s.pages++}
 return &http.Response{StatusCode:200,Header:make(http.Header),Body:io.NopCloser(strings.NewReader(text)),Request:r},nil
}
func main(){
 s:=&source{};allowed,denied:=0,0;var bad error
 f:=fetchbot.New(fetchbot.HandlerFunc(func(c *fetchbot.Context,r *http.Response,e error){
  if e==fetchbot.ErrDisallowed {denied++;return};if e!=nil {bad=e;return}
  b,err:=io.ReadAll(r.Body);if err!=nil || string(b)!="catalog item=17" {bad=fmt.Errorf("unexpected body %q err=%v",b,err);return};allowed++
 }))
 f.HttpClient=s;f.CrawlDelay=0;f.WorkerIdleTTL=time.Hour
 q:=f.Start();if _,e:=q.SendStringGet("http://catalog.test/ok","http://catalog.test/private","http://catalog.test/ok?again");e!=nil {panic(e)};q.Close()
 if bad!=nil || allowed!=2 || denied!=1 || s.robots!=1 || s.pages!=2 {panic(fmt.Sprintf("bad=%v allowed=%d denied=%d robots=%d pages=%d",bad,allowed,denied,s.robots,s.pages))}
 _,present:=reflect.TypeOf(*f).FieldByName("RobotsCacheTTL")
 fmt.Printf("robots_hits=%d page_hits=%d allowed=%d denied=%d body=item17\n",s.robots,s.pages,allowed,denied)
 fmt.Printf("RobotsCacheTTL_present=%t\n",present)
}
