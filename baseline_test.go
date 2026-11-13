package httpcache
import (
 "fmt"
 "io"
 "net/http"
 "net/http/httptest"
 "testing"
)
func TestBaseline(t *testing.T) {
 hits:=0
 srv:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){
  hits++
  w.Header().Set("Cache-Control","max-age=3600")
  w.Header().Set("ETag",`"v1"`)
  if r.Header.Get("Range")!="" {w.Header().Set("Content-Range","bytes 1-2/6");w.WriteHeader(206);fmt.Fprint(w,"BC");return}
  fmt.Fprint(w,"ABCDEF")
 }))
 defer srv.Close()
 tp:=NewMemoryCacheTransport();client:=tp.Client()
 fetch:=func(rng string)string {
  req,_:=http.NewRequest("GET",srv.URL,nil);if rng!="" {req.Header.Set("Range",rng)}
  resp,err:=client.Do(req);if err!=nil {t.Fatal(err)}
  b,err:=io.ReadAll(resp.Body);resp.Body.Close();if err!=nil {t.Fatal(err)};return string(b)
 }
 if fetch("")!="ABCDEF" || fetch("")!="ABCDEF" || hits!=1 {t.Fatal("normal full cache path failed")}
 t.Log("normal full GET cache: body=ABCDEF origin_hits=1")
 if fetch("bytes=1-2")!="BC" || hits!=2 {t.Fatal("old range behavior changed")}
 if fetch("")!="ABCDEF" || hits!=3 {t.Fatal("old range invalidation changed")}
 t.Log("observed range bypass and eviction: origin_hits=3")
}
