package main
import(
 "fmt"
 "go/ast"
 "go/parser"
 "go/token"
 "io"
 "net/http"
 "net/http/httptest"
 "path/filepath"
 "sync/atomic"
 "time"
 ant "github.com/antchfx/antch"
)
func main(){
 var total,second atomic.Int32
 srv:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){total.Add(1);if r.URL.Path=="/dir/page2"{second.Add(1);w.Header().Set("X-Crawl-Delta","/changes");fmt.Fprint(w,"two");return};w.Header().Set("X-Crawl-Next","page2");fmt.Fprint(w,"one")}));defer srv.Close()
 crawler:=ant.NewCrawler();exit:=make(chan struct{});crawler.Exit=exit;defer close(exit);crawler.DownloadDelay=time.Nanosecond
 entered,release,finished:=make(chan struct{}),make(chan struct{}),make(chan string,1)
 crawler.Handle("*",ant.HandlerFunc(func(out chan<- ant.Item,r *http.Response){b,e:=io.ReadAll(r.Body);if e!=nil{panic(e)};out<-string(b)}))
 crawler.UsePipeline(func(next ant.PipelineHandler)ant.PipelineHandler{return ant.PipelineHandlerFunc(func(v ant.Item){close(entered);<-release;next.ServePipeline(v);finished<-v.(string)})})
 req,e:=http.NewRequest("GET",srv.URL+"/dir/first",nil);if e!=nil{panic(e)};if e=crawler.Crawl(req);e!=nil{panic(e)}
 <-entered
 fmt.Println("LEGACY_ENQUEUE_RETURNED_WITH_PIPELINE_BLOCKED=true")
 close(release);value:=<-finished;if value!="one"{panic("wrong production pipeline value")};fmt.Println("PIPELINE_VALUE="+value)
 if total.Load()!=1||second.Load()!=0{panic("unexpected automatic continuation")}
 fmt.Printf("LEGACY_HTTP_REQUESTS=%d NEXT_PAGE_REQUESTS=%d\n",total.Load(),second.Load())
 found:=false;paths,e:=filepath.Glob("*.go");if e!=nil{panic(e)};for _,p:=range paths{f,e:=parser.ParseFile(token.NewFileSet(),p,nil,0);if e!=nil{panic(e)};for _,d:=range f.Decls{if fn,ok:=d.(*ast.FuncDecl);ok&&fn.Name.Name=="Collect"{found=true}}}
 if found{panic("baseline Collect unexpectedly exists")};fmt.Println("COLLECT_DECLARED=false")
}
