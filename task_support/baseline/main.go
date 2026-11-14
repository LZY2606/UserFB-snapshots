package main

import (
 "context"
 "fmt"
 "io"
 "net/http"
 "os"
 "strings"
 m "github.com/mattn/go-mastodon"
)

type transport func(*http.Request) (*http.Response,error)
func (f transport) RoundTrip(r *http.Request)(*http.Response,error){return f(r)}
type body struct {io.Reader; closed *int}
func (b *body) Close()error{*b.closed++;return nil}
func check(ok bool, why string){if !ok{fmt.Fprintln(os.Stderr,"BASELINE FAILED:",why);os.Exit(1)}}
func main(){
 requests,closed:=0,0
 c:=m.NewClient(&m.Config{Server:"https://source.invalid",AccessToken:"local-fixture"})
 c.UserAgent="source-baseline"
 c.Transport=transport(func(req *http.Request)(*http.Response,error){
  requests++
  check(req.Method=="GET"&&req.URL.Path=="/api/v1/timelines/public","real public endpoint")
  check(req.Header.Get("Authorization")=="Bearer local-fixture"&&req.UserAgent()==c.UserAgent,"client header behavior")
  check(req.URL.Query().Get("local")=="t"&&req.URL.Query().Get("since_id")=="100","local and since reach transport")
  h:=make(http.Header)
  payload:=`[{"id":"103","content":"newer"},{"id":"102","content":"older"}]`
  if requests==1{h.Set("Link",`<https://foreign.invalid/unused?max_id=102>; rel="next"`)}else{
   check(req.URL.Query().Get("max_id")=="102","next cursor reaches public endpoint")
   payload=`[{"id":"101","content":"oldest"}]`
  }
  return &http.Response{StatusCode:200,Header:h,Request:req,Body:&body{strings.NewReader(payload),&closed}},nil
 })
 pg:=&m.Pagination{SinceID:"100"}
 first,err:=c.GetTimelinePublic(context.Background(),true,pg)
 check(err==nil&&len(first)==2&&first[0].ID=="103"&&first[1].Content=="older","real first page JSON decode")
 check(pg.MaxID=="102"&&pg.SinceID=="","upstream Link overwrites pagination, including absent since")
 fmt.Println("BASELINE public-page-link=ok")
 pg.SinceID="100"
 second,err:=c.GetTimelinePublic(context.Background(),true,pg)
 check(err==nil&&len(second)==1&&second[0].ID=="101"&&*pg==(m.Pagination{}),"second page and link exhaustion")
 fmt.Println("BASELINE public-page-client-options=ok")
 check(requests==2&&closed==2,"response close count")
 fmt.Printf("BASELINE response-bodies=%d/%d\n",closed,requests)
}
