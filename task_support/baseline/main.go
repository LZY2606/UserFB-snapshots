package main

import (
 "context"
 "fmt"
 "net/http"
 "net/http/httptest"
 "os"
 "strings"
 "sync/atomic"

 rss "github.com/SlyMarbo/rss/v2"
 "github.com/SlyMarbo/rss/v2/atom1"
)

func main() {
 current, err := os.ReadFile("task_support/fixtures/current.atom"); must(err)
 archive, err := os.ReadFile("task_support/fixtures/archive.atom"); must(err)
 var archiveRequests atomic.Int32
 server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
  w.Header().Set("Content-Type", "application/atom+xml")
  switch r.URL.Path {
  case "/current": w.Write(current)
  case "/archive": archiveRequests.Add(1); w.Write(archive)
  default: http.NotFound(w,r)
  }
 }))
 defer server.Close()
 reader, err := rss.NewReader(rss.WithHTTPClient(server.Client())); must(err)
 feed, err := reader.Fetch(context.Background(),server.URL+"/current"); must(err)
 if len(feed.Items)!=2 || feed.Items[0].ID!="urn:recent" || feed.Unread!=2 {panic("ordinary production fetch changed")}
 found:=false
 for _, link:=range feed.Links {if link.Rel=="prev-archive" && link.Href=="/archive" {found=true}}
 if !found || archiveRequests.Load()!=0 {panic("unexpected default fetch traversal")}
 raw,err:=atom1.Parse(strings.NewReader(string(current)));must(err)
 if raw.Updated!="2026-11-14T00:00:00Z" {panic("raw Atom updated missing")}
 fmt.Printf("BASELINE Fetch items=%d unread=%d archive_requests=%d prev-archive=present\n",len(feed.Items),feed.Unread,archiveRequests.Load())
 fmt.Printf("RAW Atom feed.updated=%s; default Fetch follows no archive links\n",raw.Updated)
}
func must(err error) {if err!=nil {panic(err)}}
