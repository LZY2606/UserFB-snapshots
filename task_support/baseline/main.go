package main
import("fmt";"net/http";"net/http/httptest";"sync/atomic";"time";sitemap "github.com/yterajima/go-sitemap")
func main(){
 var requests atomic.Int32
 var base string
 srv:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){
 requests.Add(1);w.Header().Set("Content-Type","application/xml")
 switch r.URL.Path {
 case "/root":fmt.Fprintf(w,`<sitemapindex><sitemap><loc>%s/a</loc><lastmod>2024-01-01</lastmod></sitemap><sitemap><loc>%s/b</loc><lastmod>2024-01-01</lastmod></sitemap></sitemapindex>`,base,base)
 case "/nested":fmt.Fprintf(w,`<sitemapindex><sitemap><loc>%s/root</loc></sitemap></sitemapindex>`,base)
 default:fmt.Fprintf(w,`<urlset><url><loc>https://pages.invalid%s</loc></url></urlset>`,r.URL.Path)
 }
 }));defer srv.Close();base=srv.URL
 sitemap.SetInterval(time.Nanosecond)
 for i:=0;i<2;i++ { result,err:=sitemap.Get(base+"/root",nil);if err!=nil||len(result.URL)!=2{panic(fmt.Sprintf("normal=%v %v",result,err))} }
 repeated:=requests.Load()
 _,nestedErr:=sitemap.Get(base+"/nested",nil)
 fmt.Printf("BASELINE_OK normal_pages=2 repeated_get_requests=%d\n",repeated)
 fmt.Printf("nested_index_error=%v\n",nestedErr!=nil)
 if nestedErr!=nil{fmt.Printf("nested_detail=%v\n",nestedErr)}
}
