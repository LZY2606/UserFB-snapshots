package fetchbot
import (
 "io"
 "net/http"
 "net/http/httptest"
 "net/url"
 "sync"
 "testing"
)
type testAgentCmd struct {*Cmd; h http.Header}
func(c *testAgentCmd) Header() http.Header {return c.h}
func TestBaseline(t *testing.T) {
 var mu sync.Mutex; hits:=0
 srv:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){
  if r.URL.Path=="/robots.txt" {io.WriteString(w,"User-agent: BlockedBot\nDisallow: /\n\nUser-agent: AllowedBot\nDisallow:\n");return}
  mu.Lock();hits++;mu.Unlock();io.WriteString(w,"collected")
 }));defer srv.Close()
 u,_:=url.Parse(srv.URL+"/data")
 var gotErr error
 f:=New(HandlerFunc(func(c *Context,r *http.Response,e error){gotErr=e;if r!=nil {io.Copy(io.Discard,r.Body)}}))
 f.CrawlDelay=0;f.UserAgent="BlockedBot"
 q:=f.Start();if err:=q.Send(&testAgentCmd{&Cmd{u,"GET"},http.Header{"User-Agent":[]string{"AllowedBot"}}});err!=nil {t.Fatal(err)};q.Close()
 if gotErr!=ErrDisallowed || hits!=0 {t.Fatalf("old policy: error=%v hits=%d",gotErr,hits)}
 t.Log("observed mismatch: command_agent=AllowedBot robots_agent=BlockedBot page_hits=0 ErrDisallowed")
 f2:=New(HandlerFunc(func(c *Context,r *http.Response,e error){if e!=nil {t.Error(e);return};b,_:=io.ReadAll(r.Body);if string(b)!="collected" {t.Error("body")}}));f2.CrawlDelay=0;f2.UserAgent="AllowedBot"
 q2:=f2.Start();q2.SendStringGet(srv.URL+"/data");q2.Close()
 if hits!=1 {t.Fatalf("normal capture hits=%d",hits)}
 t.Log("normal matching agent HTTP fetch: page_hits=1 body=collected")
}
