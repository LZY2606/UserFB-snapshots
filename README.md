# go-sitemap

[![Github Actions CI](https://github.com/yterajima/go-sitemap/actions/workflows/ci.yml/badge.svg)](https://github.com/yterajima/go-sitemap/actions/workflows/ci.yml)
[![GoDoc](https://img.shields.io/badge/pkg.go.dev-doc-blue)](http://pkg.go.dev/github.com/yterajima/go-sitemap)


go-sitemap get sitemap.xml (or sitemapindex.xml) and generate Sitemap object.

## Installation

```
go install github.com/yterajima/go-sitemap
```

## Get and ForceGet

`Get` downloads the entry document, recognizes whether it is a `urlset` or a
`sitemapindex` and expands nested sitemap indexes recursively. A document
referenced from several indexes is downloaded only once per call. `ForceGet`
keeps the same walk but ignores failures of individual child documents; both
functions pass `options` through to the fetch closure configured with
`SetFetch`.

```go
smap, err := sitemap.Get("https://example.com/sitemapindex.xml", nil)
if err != nil {
    log.Fatal(err)
}

for _, u := range smap.URL {
    fmt.Println(u.Loc)
}

// Keep going when one child sitemap cannot be retrieved or parsed.
forced, err := sitemap.ForceGet("https://example.com/sitemapindex.xml", nil)
```

A custom fetch closure (for example to send headers) receives every request,
including the recursive ones:

```go
sitemap.SetFetch(func(docURL string, options interface{}) ([]byte, error) {
    req, err := http.NewRequest(http.MethodGet, docURL, nil)
    if err != nil {
        return nil, err
    }
    req.Header.Set("User-Agent", "MyBot")

    res, err := http.DefaultClient.Do(req)
    if err != nil {
        return nil, err
    }
    defer res.Body.Close()
    return io.ReadAll(res.Body)
})
```

## Session

A `Session` crawls one sitemap tree repeatedly and keeps state between rounds.
The root document is read on every `Sync`. In `full` mode every reachable
document is re-downloaded; in `incremental` mode a document is reused from the
previous round only when every current parent index declares it with a
lastmod that parses to the same instant as the evidence saved with the old
document.

```go
session, err := sitemap.NewSession("https://example.com/sitemapindex.xml")
if err != nil {
    log.Fatal(err)
}

for {
    // "full" re-reads everything; "incremental" reuses unchanged documents.
    snap, err := session.Sync("incremental", nil)
    if err != nil {
        // The snapshot is empty and the previous round's state is untouched.
        log.Printf("sync failed, will retry: %v", err)
        continue
    }

    fmt.Printf("reachable pages: %d\n", len(snap.URLs))
    for _, loc := range snap.Added {
        fmt.Printf("added:   %s\n", loc)
    }
    for _, loc := range snap.Removed {
        fmt.Printf("removed: %s\n", loc)
    }
}
```

`Snapshot.URLs` holds the trimmed, deduplicated and sorted page locs of every
reachable leaf sitemap; `Added` and `Removed` are the sorted differences
against the previous successful round. On any download or parse failure
`Sync` returns an empty `Snapshot` and an error without changing the saved
state, so a failed round can never poison later reuse decisions.

