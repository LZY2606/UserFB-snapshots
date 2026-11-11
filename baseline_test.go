package fetchbot

import (
    "io"
    "net/http"
    "strings"
    "testing"
)
type stubDoer func(*http.Request) (*http.Response,error)
func (d stubDoer) Do(r *http.Request) (*http.Response,error) {return d(r)}
func TestCloseBaseline(t *testing.T) {
    entered, release, firstDone := make(chan struct{}), make(chan struct{}), make(chan struct{})
    f := New(HandlerFunc(func(*Context,*http.Response,error){close(entered); <-release}))
    f.DisablePoliteness, f.CrawlDelay = true, 0
    f.HttpClient = stubDoer(func(r *http.Request)(*http.Response,error){
        return &http.Response{StatusCode:200, Header:make(http.Header), Body:io.NopCloser(strings.NewReader("ok")), Request:r},nil
    })
    q := f.Start()
    if _,err:=q.SendStringGet("https://source.invalid/a"); err!=nil {t.Fatal(err)}
    <-entered
    go func(){q.Close();close(firstDone)}()
    <-q.closed
    if err:=q.Close();err!=nil {t.Fatal(err)}
    select {case <-q.Done():t.Fatal("done closed before handler returned");default:}
    select {case <-firstDone:t.Fatal("first Close returned before handler");default:}
    close(release)
    <-firstDone
    <-q.Done()
    t.Log("BASELINE: second Close returns while first Close and handler are still blocked")
}
