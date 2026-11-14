package twitterscraper

import (
 "encoding/json"
 "fmt"
 "io"
 "net/http"
 "strings"
 "testing"
)

type testTransport func(*http.Request) (*http.Response,error)
func (f testTransport) RoundTrip(r *http.Request)(*http.Response,error){return f(r)}
type testBody struct {io.Reader; closed *int}
func (b *testBody) Close() error {*b.closed++;return nil}
func testTweet(id, text string) map[string]interface{} {
 return map[string]interface{}{"__typename":"Tweet","legacy":map[string]interface{}{"id_str":id,"full_text":text,"user_id_str":"u"},"core":map[string]interface{}{"user_results":map[string]interface{}{"result":map[string]interface{}{"legacy":map[string]interface{}{"screen_name":"local","name":"Local"}}}},"views":map[string]interface{}{"count":"17"}}
}
func testEntry(id string) map[string]interface{} {
 return map[string]interface{}{"entryId":"tweet-"+id,"content":map[string]interface{}{"itemContent":map[string]interface{}{"tweetDisplayType":"Tweet","tweet_results":map[string]interface{}{"result":testTweet(id,"offline text")}}}}
}
func testInstructions() []interface{} {return []interface{}{map[string]interface{}{"type":"TimelineAddEntries","entries":[]interface{}{testEntry("101"),map[string]interface{}{"entryId":"cursor-bottom","content":map[string]interface{}{"cursorType":"Bottom","value":"next-page"}}}}}}
func testJSON(kind string) string {
 instructions:=testInstructions();var v interface{}
 switch kind {
 case "search":v=map[string]interface{}{"data":map[string]interface{}{"search_by_raw_query":map[string]interface{}{"search_timeline":map[string]interface{}{"timeline":map[string]interface{}{"instructions":instructions}}}}}
 case "thread":v=map[string]interface{}{"data":map[string]interface{}{"threaded_conversation_with_injections_v2":map[string]interface{}{"instructions":instructions}}}
 default:v=map[string]interface{}{"data":map[string]interface{}{"user":map[string]interface{}{"result":map[string]interface{}{"timeline_v2":map[string]interface{}{"timeline":map[string]interface{}{"instructions":instructions}}}}}}
 }
 b,_:=json.Marshal(v);return string(b)
}
func TestCollectionBaseline(t *testing.T) {
 s:=New();s.isLogged=true;closed:=0;hits:=0
 s.client.Transport=testTransport(func(r *http.Request)(*http.Response,error){
  hits++; kind:="user";if strings.HasSuffix(r.URL.Path,"/SearchTimeline"){kind="search"};if strings.HasSuffix(r.URL.Path,"/TweetDetail"){kind="thread"}
  if r.Method!="GET" || r.URL.Query().Get("variables")=="" {t.Fatal("request lost method or variables")}
  return &http.Response{StatusCode:200,Header:make(http.Header),Request:r,Body:&testBody{strings.NewReader(testJSON(kind)),&closed}},nil
 })
 user,cursor,err:=s.FetchTweetsByUserID("u",10,"")
 if err!=nil || len(user)!=1 || user[0].ID!="101" || user[0].Text!="offline text" || user[0].Views!=17 || cursor!="next-page" {t.Fatalf("user result=%+v cursor=%q err=%v",user,cursor,err)}
 search,cursor,err:=s.FetchSearchTweets("local",10,"")
 if err!=nil || len(search)!=1 || search[0].Text!="offline text" || search[0].Views!=17 || cursor!="next-page" {t.Fatalf("search result=%+v cursor=%q err=%v",search,cursor,err)}
 thread,err:=s.GetTweet("101");if err!=nil || thread.ID!="101" || thread.Text!="offline text" {t.Fatalf("thread=%+v err=%v",thread,err)}
 if hits!=3 || closed!=3 {t.Fatalf("requests=%d closed=%d",hits,closed)}
 fmt.Println("BASE_TWITTER_USER_SEARCH_THREAD requests=3 closed=3 text=offline views=17 cursor=next-page")
}
