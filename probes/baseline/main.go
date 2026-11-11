package main
import (
 "fmt"
 "io"
 "net/http"
 "strings"
 cache "github.com/gregjones/httpcache"
)
type wire struct { n int; condition string }
func (w *wire) RoundTrip(r *http.Request) (*http.Response, error) {
 w.n++
 h := http.Header{"Date": {"Thu, 01 Jan 2099 00:00:00 GMT"}, "Cache-Control": {"no-cache"}, "Etag": {`"old"`}}
 status, body := 200, "OLD"
 if w.n == 2 { w.condition = r.Header.Get("If-None-Match"); status, body = 304, ""; h.Set("Etag", `"caller"`) }
 return &http.Response{StatusCode:status, Status:fmt.Sprint(status), Proto:"HTTP/1.1", ProtoMajor:1, ProtoMinor:1, Header:h, Body:io.NopCloser(strings.NewReader(body)), ContentLength:int64(len(body)), Request:r}, nil
}
func main() {
 w := &wire{}
 t := cache.NewMemoryCacheTransport(); t.Transport = w
 c := t.Client()
 first, err := c.Get("http://collector.invalid/items"); if err != nil { panic(err) }
 b, err := io.ReadAll(first.Body); first.Body.Close(); if err != nil || string(b)!="OLD" { panic("seed GET") }
 req, _ := http.NewRequest("GET", "http://collector.invalid/items", nil)
 req.Header.Set("If-None-Match", `"caller"`); req.Header.Set("Cache-Control", "no-cache")
 resp, err := c.Do(req); if err != nil { panic(err) }
 b, err = io.ReadAll(resp.Body); resp.Body.Close()
 if err != nil || resp.StatusCode!=200 || string(b)!="OLD" || w.condition!=`"caller"` { panic("unexpected starting behavior") }
 fmt.Printf("baseline: wire condition=%s; origin=304; client=%d body=%s\n",w.condition,resp.StatusCode,b)
}
