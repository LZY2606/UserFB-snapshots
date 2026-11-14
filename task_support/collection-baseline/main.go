package main
import (
 "fmt"
 "io"
 "net/http"
 "strings"
 "github.com/mmcdole/gofeed"
)
type transport func(*http.Request)(*http.Response,error)
func(f transport)RoundTrip(r *http.Request)(*http.Response,error){return f(r)}
func main(){
 documents:=map[string]string{
 "rss":`<rss version="2.0"><channel><title>Local</title><item><guid isPermaLink="false">same</guid><title>RSS</title><link>post/a#x</link></item></channel></rss>`,
 "atom":`<feed xmlns="http://www.w3.org/2005/Atom"><id>urn:feed</id><title>Local</title><entry><id>same</id><title>Atom</title><link href="post/a#x"/><link href="post/b"/></entry></feed>`,
 "json":`{"version":"https://jsonfeed.org/version/1.1","title":"Local","items":[{"id":"same","title":"JSON","url":"post/a#x","external_url":"post/c"}]}`,
 }
 p:=gofeed.NewParser();hits:=0
 p.Client=&http.Client{Transport:transport(func(r *http.Request)(*http.Response,error){hits++;return &http.Response{StatusCode:200,Header:make(http.Header),Request:r,Body:io.NopCloser(strings.NewReader(documents[strings.TrimPrefix(r.URL.Path,"/")]))},nil})}
 for _,kind:=range []string{"rss","atom","json"}{
  f,e:=p.ParseURL("https://feeds.invalid/"+kind)
  if e!=nil || f.FeedType!=kind || len(f.Items)!=1 || f.Items[0].GUID!="same" || f.Items[0].Link!="post/a#x"{panic(fmt.Sprintf("%s feed=%+v error=%v",kind,f,e))}
  if kind=="atom" && len(f.Items[0].Links)!=2 {panic(fmt.Sprintf("atom aliases=%v",f.Items[0].Links))}
 }
 if hits!=3 {panic("unexpected requests")}
 fmt.Println("BASE_COLLECTION_FORMATS rss=1 atom=1 json=1 guid=same link=relative requests=3")
}
