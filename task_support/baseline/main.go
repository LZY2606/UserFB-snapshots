package main
import (
 "fmt"
 "net/http"
 "net/http/httptest"
 "strings"
 "time"
 readability "github.com/go-shiori/go-readability"
)
func main(){
 first,second:=0,0
 h:=http.NewServeMux()
 h.HandleFunc("/entry",func(w http.ResponseWriter,r *http.Request){http.Redirect(w,r,"/one/start",http.StatusFound)})
 h.HandleFunc("/one/start",func(w http.ResponseWriter,r *http.Request){first++;w.Header().Set("Content-Type","text/html; charset=utf-8");fmt.Fprint(w,`<!doctype html><html><head><title>Collected article</title><base href="/assets/"><link rel="next" href="part2"></head><body><article><h1>Collected article</h1><p id="intro">`+strings.Repeat("A long field report, with details from the observed landscape and collection process. ",14)+`<a href="pic.jpg">SOURCE_LINK</a></p></article></body></html>`)})
 h.HandleFunc("/assets/part2",func(w http.ResponseWriter,r *http.Request){second++;fmt.Fprint(w,"UNREQUESTED_SECOND_PAGE")})
 s:=httptest.NewServer(h);defer s.Close()
 a,e:=readability.FromURL(s.URL+"/entry",5*time.Second)
 if e!=nil || !strings.Contains(a.TextContent,"SOURCE_LINK") || a.Node==nil{panic(fmt.Sprintf("production extraction failed: %v",e))}
 fmt.Printf("BASELINE_OK pages=%d second=%d text=%t\n",first,second,strings.Contains(a.TextContent,"field report"))
 if strings.Contains(a.Content,s.URL+"/pic.jpg"){fmt.Println("BASELINE_LINK /pic.jpg")}else if strings.Contains(a.Content,s.URL+"/assets/pic.jpg"){fmt.Println("BASELINE_LINK /assets/pic.jpg")}else{panic("missing resolved link")}
}
