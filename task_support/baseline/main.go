package main

import (
    "context"
    "errors"
    "fmt"
    "io"
    "net/http"
    "os"
    "strings"

    "github.com/miku/metha/harvest"
    "github.com/miku/metha/oai"
    "github.com/miku/metha/store"
)

type responder struct { calls int }

func (d *responder) Do(r *http.Request) (*http.Response, error) {
    d.calls++
    day := r.URL.Query().Get("from")
    if r.URL.Query().Get("verb") != "ListRecords" || len(day) != 10 {
        return nil, fmt.Errorf("unexpected request %s", r.URL)
    }
    body := fmt.Sprintf(`<OAI-PMH xmlns="http://www.openarchives.org/OAI/2.0/"><ListRecords><record><header><identifier>old-%s</identifier><datestamp>%s</datestamp></header><metadata><title>original</title></metadata></record></ListRecords></OAI-PMH>`, day[8:], day)
    return &http.Response{StatusCode: 200, Header: make(http.Header), Body: io.NopCloser(strings.NewReader(body)), Request: r}, nil
}

func main() {
    if err := run(); err != nil { fmt.Fprintln(os.Stderr, err); os.Exit(1) }
}

func run() error {
    dir, err := os.MkdirTemp("", "metha-baseline-")
    if err != nil { return err }
    defer os.RemoveAll(dir)
    id := store.Identity{BaseURL: "https://archive.example.invalid/oai", Format: "oai_dc"}
    first := &responder{}
    once := func(d *responder) error {
        w, err := store.OpenWriter(dir, id)
        if err != nil { return err }
        h := &harvest.Harvest{
            Config: &harvest.Config{BaseURL: id.BaseURL, Format: id.Format, From: "2020-01-01", Until: "2020-01-02", DailyInterval: true, MaxEmptyResponses: 10},
            Client: &oai.Client{Doer: d}, Writer: w,
            Identify: &oai.Identify{Granularity: "YYYY-MM-DD", EarliestDatestamp: "2020-01-01"},
        }
        err = h.Run(context.Background())
        return errors.Join(err, w.Close())
    }
    if err := once(first); err != nil { return err }
    second := &responder{}
    if err := once(second); !errors.Is(err, harvest.ErrAlreadySynced) { return fmt.Errorf("second run: %v", err) }
    s, err := store.Open(dir, id)
    if err != nil { return err }
    var ids []string
    for rec, err := range s.Records(store.ReadOptions{}) {
        if err != nil { return err }
        ids = append(ids, rec.Header.Identifier)
    }
    if first.calls != 2 || second.calls != 0 || strings.Join(ids, ",") != "old-01,old-02" {
        return fmt.Errorf("unexpected baseline first=%d second=%d ids=%v", first.calls, second.calls, ids)
    }
    fmt.Printf("METHA_BASELINE_OK first_requests=%d second_requests=%d records=%s\n", first.calls, second.calls, strings.Join(ids, ","))
    return nil
}
