package main
import (
 "fmt"
 "net/http"
 "net/http/httptest"
 "os"
 "strings"
 "github.com/headzoo/surf"
 _ "golang.org/x/net/html/charset"
)
func main(){
 posted:=""
 page:=`<title>caf`+string([]byte{0xe9})+`</title><form action="/submit" method="post"><input name="q" value="caf`+string([]byte{0xe9})+`"><input name="_charset_" value="ignored"></form>`
 ts:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){
 if r.URL.Path=="/ascii"{fmt.Fprint(w,`<title>orders</title><form method="post" action="/submit"><input name="q" value="normal"></form>`);return}
 if r.URL.Path=="/submit"{_ = r.ParseForm();posted=r.PostForm.Get("q");fmt.Fprint(w,"<title>done</title>");return}
 w.Header().Set("Content-Type","text/html; charset=windows-1252");fmt.Fprint(w,page)
 }));defer ts.Close()
 b:=surf.NewBrowser();must(b.Open(ts.URL+"/ascii"));if b.Title()!="orders"{panic("normal title")};f,e:=b.Form("form");must(e);must(f.Submit());if posted!="normal"{panic("normal submit")};fmt.Println("BASELINE_ASCII_FORM_OK")
 must(b.Open(ts.URL));fmt.Printf("LEGACY_TITLE %q\n",b.Title());f,e=b.Form("form");must(e);must(f.Input("q","café"));must(f.Submit());fmt.Printf("LEGACY_POST_HEX %x\n",[]byte(posted));if strings.Contains(posted,"\x00"){panic("unexpected nul")}
}
func must(e error){if e!=nil{fmt.Fprintln(os.Stderr,e);os.Exit(1)}}
