package main
import (
 "context"
 "fmt"
 "net/http"
 "net/http/httptest"
 "net/url"
 "sync/atomic"
 "time"
 ant "github.com/yields/ant"
)
type scraperFunc func(context.Context,*ant.Page)(ant.URLs,error)
func(f scraperFunc) Scrape(c context.Context,p *ant.Page)(ant.URLs,error){return f(c,p)}
type observedQueue struct{ ant.Queue; takes atomic.Int32; done atomic.Int32; prefetched chan struct{}; acked chan struct{} }
func(q *observedQueue) Dequeue(ctx context.Context)(*ant.URL,error){u,e:=q.Queue.Dequeue(ctx);if e==nil&&q.takes.Add(1)==2{close(q.prefetched)};return u,e}
func(q *observedQueue) Done(ctx context.Context,u *ant.URL)error{e:=q.Queue.Done(ctx,u);if q.done.Add(1)==1{close(q.acked)};return e}
func must(e error){if e!=nil{panic(e)}}
func main(){
 srv:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){fmt.Fprint(w,"<html><body>fixture</body></html>")}));defer srv.Close()
 var visits atomic.Int32
 eng,e:=ant.NewEngine(ant.EngineConfig{Impolite:true,Concurrency:2,Scraper:scraperFunc(func(ctx context.Context,p *ant.Page)(ant.URLs,error){visits.Add(1);if p.URL.Path=="/"{u,_:=url.Parse(srv.URL+"/child");return ant.URLs{u},nil};return nil,nil})});must(e);must(eng.Run(context.Background(),srv.URL+"/"));if visits.Load()!=2{panic("normal crawl lost discovery")};fmt.Printf("NORMAL_DISCOVERY_VISITS=%d\n",visits.Load())
 q:=&observedQueue{Queue:ant.MemoryQueue(2),prefetched:make(chan struct{}),acked:make(chan struct{})}
 entered:=make(chan struct{});ctx,cancel:=context.WithCancel(context.Background());defer cancel()
 eng,e=ant.NewEngine(ant.EngineConfig{Impolite:true,Workers:1,Concurrency:1,Queue:q,Scraper:scraperFunc(func(c context.Context,p *ant.Page)(ant.URLs,error){close(entered);<-c.Done();return nil,c.Err()})});must(e)
 finished:=make(chan error,1);go func(){finished<-eng.Run(ctx,srv.URL+"/one",srv.URL+"/two")}()
 <-entered;<-q.prefetched;cancel();<-q.acked
 select{case e:=<-finished:panic(fmt.Sprintf("old hang unexpectedly absent: %v",e));case <-time.After(500*time.Millisecond):}
 fmt.Printf("CANCELED_QUEUE_DEQUEUED=%d ACKNOWLEDGED=%d\n",q.takes.Load(),q.done.Load())
 fmt.Println("OBSERVED_RUN_STUCK_AFTER_CANCEL=true")
}
