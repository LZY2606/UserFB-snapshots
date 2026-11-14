package main
import (
 "context"
 "encoding/json"
 "fmt"
 "net/http"
 "net/http/httptest"
 "os"
 "reflect"
 graphql "github.com/machinebox/graphql"
)
func main(){
 calls:=0
 server:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){
  if r.Method!="POST" {panic("not POST")};var body struct{Query string;Variables map[string]any};if e:=json.NewDecoder(r.Body).Decode(&body);e!=nil {panic(e)}
  if body.Query!="query($key:ID!){item(id:$key){name}}" || body.Variables["key"]!="7" {panic("wrong request")};calls++;w.Header().Set("Content-Type","application/json");fmt.Fprint(w,`{"data":{"item":{"name":"COLLECTED"}}}`)
 }));defer server.Close()
 client:=graphql.NewClient(server.URL);req:=graphql.NewRequest("query($key:ID!){item(id:$key){name}}");req.Var("key","7")
 var result struct{Item struct{Name string}}
 if err:=client.Run(context.Background(),req,&result);err!=nil {panic(err)}
 if calls!=1 || result.Item.Name!="COLLECTED" {panic("wrong production result")};fmt.Println("BASELINE_QUERY_OK calls=1 name=COLLECTED")
 if os.Getenv("TASK_BASELINE_CURRENT")=="1" { _,exists:=reflect.TypeOf(client).MethodByName("Subscribe");if exists {panic("baseline missing-capability assertion no longer current")};fmt.Println("BASELINE_SUBSCRIPTIONS_MISSING") }
}
