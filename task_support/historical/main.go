package main
import (
 "fmt"
 "net/http"
 "net/http/httptest"
 "strings"
 "github.com/mmcdole/gofeed"
 "github.com/mmcdole/gofeed/rss"
)
const document = `<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#" xmlns="http://purl.org/rss/1.0/"><channel rdf:about="urn:feed"><title>Discovery</title><link>https://source.invalid/</link><description>Local</description><items><rdf:Seq><rdf:li rdf:resource="urn:b"/><rdf:li rdf:resource="urn:a"/></rdf:Seq></items></channel><item rdf:about="urn:a"><title>A</title><link>https://source.invalid/a</link></item><item rdf:about="urn:unused"><title>Unused</title></item><item rdf:about="urn:b"><title>B</title><link>https://source.invalid/b</link></item></rdf:RDF>`
func main() {
 raw, err := (&rss.Parser{}).Parse(strings.NewReader(document)); if err != nil {panic(err)}
 if len(raw.Items)!=3 || raw.Items[0].Title!="A" || raw.Items[1].Title!="Unused" || raw.Items[2].Title!="B" {panic("physical RSS order changed")}
 fmt.Println("BASE_RSS_PHYSICAL_ORDER_OK")
 server:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){w.Header().Set("Content-Type","application/rss+xml");fmt.Fprint(w,document)}));defer server.Close()
 parser:=gofeed.NewParser();parser.KeepOriginalFeed=true
 feed,err:=parser.ParseURL(server.URL);if err!=nil {panic(err)}
 if len(feed.Items)!=3 || feed.Items[0].GUID!="urn:a" || feed.Items[2].GUID!="urn:b" {panic("URL collection changed")}
 if _,ok:=feed.OriginalFeed().(*rss.Feed);!ok {panic("missing native feed")}
 fmt.Println("BASE_RSS_URL_SEQUENCE_NOT_APPLIED")
}
