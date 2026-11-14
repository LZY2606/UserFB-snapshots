package main

import (
 "context"
 "fmt"
 "io"
 "net/http"
 "os"
 "path/filepath"
 "strings"
 "sync"
 grab "github.com/cavaliergopher/grab/v3"
)
type transport struct{ closed int }
type chunks struct{ text *strings.Reader; owner *transport }
func (b *chunks) Read(p []byte)(int,error){if len(p)>5{p=p[:5]};return b.text.Read(p)}
func (b *chunks) Close()error{b.owner.closed++;return nil}
func (t *transport) Do(r *http.Request)(*http.Response,error){
 return &http.Response{StatusCode:200,ContentLength:10,Header:make(http.Header),Body:&chunks{strings.NewReader("row1\nrow2\n"),t},Request:r},nil
}
type gate struct{ once sync.Once; prefix,release chan struct{} }
func(g *gate) WaitN(ctx context.Context,n int)error{
 g.once.Do(func(){close(g.prefix);select{case <-g.release:case <-ctx.Done():}})
 return ctx.Err()
}
func run(dir string,memory bool)error{
 tr:=&transport{};cl:=grab.NewClient();cl.HTTPClient=tr
 req,err:=grab.NewRequest(filepath.Join(dir,fmt.Sprintf("capture-%t",memory)),"http://fixture.invalid/export");if err!=nil{return err}
 hold:=&gate{prefix:make(chan struct{}),release:make(chan struct{})}
 req.RateLimiter=hold;req.NoStore=memory
 resp:=cl.Do(req)
 <-hold.prefix
 n,done:=resp.BytesComplete(),resp.IsComplete()
 fmt.Printf("NoStore=%t stored_prefix=%d Done=%t\n",memory,n,done)
 close(hold.release)
 if n!=5||done{return fmt.Errorf("unexpected partial state")}
 rd,err:=resp.Open();if err!=nil{return err};defer rd.Close()
 data,err:=io.ReadAll(rd);if err!=nil{return err}
 if string(data)!="row1\nrow2\n"||tr.closed!=1{return fmt.Errorf("unexpected completion: %q closed=%d",data,tr.closed)}
 fmt.Printf("NoStore=%t legacy_open=%q bytes=%d body_closed=%d\n",memory,data,resp.BytesComplete(),tr.closed)
 return nil
}
func main(){
 dir,err:=os.MkdirTemp("","check-baseline-");if err!=nil{panic(err)};defer os.RemoveAll(dir)
 for _,memory:=range []bool{false,true}{if err:=run(dir,memory);err!=nil{fmt.Fprintln(os.Stderr,err);os.Exit(1)}}
 fmt.Println("BASELINE_OK HTTP copies publish progress before Done; legacy Open is completion-only")
}
