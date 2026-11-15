package main
import("context";"fmt";"net/http";"net/http/httptest";"reflect";"sync/atomic";"github.com/yields/ant")
type scraper struct{ eng *ant.Engine; count atomic.Int32 }
func(s *scraper) Scrape(ctx context.Context,p *ant.Page)(ant.URLs,error){
 s.count.Add(1)
 if p.URL.Path=="/" { if err:=s.eng.Enqueue(ctx,p.URL.String());err!=nil{return nil,err};return p.Next("a") }
 return nil,nil
}
func main(){
 var hits atomic.Int32
 srv:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){hits.Add(1);w.Header().Set("ETag",`"v1"`);fmt.Fprint(w,`<a href="/child">next</a>`)}));defer srv.Close()
 s:=new(scraper);e,err:=ant.NewEngine(ant.EngineConfig{Scraper:s,Impolite:true,Workers:1,Concurrency:1});if err!=nil{panic(err)};s.eng=e
 if err=e.Run(context.Background(),srv.URL);err!=nil{panic(err)}
 if hits.Load()!=2||s.count.Load()!=2{panic("normal discovery or dedupe failed")}
 _,present:=reflect.TypeOf(e).MethodByName("Refresh")
 fmt.Printf("BASELINE_OK pages=%d callbacks=%d duplicate_suppressed=true refresh_api=%v\n",hits.Load(),s.count.Load(),present)
}
