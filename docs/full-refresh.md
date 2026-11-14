# Full refresh (library usage)

An incremental `metha` harvest only ever asks an endpoint for what is new:
the plan resumes where the store's coverage ends, and windows the store
already covers are skipped. That is the right default, but it cannot see
history being rewritten - a repository reindexed upstream, records removed,
a set reorganised. Deletions and changes behind the resume point never reach
the cache, and the usual remedy of deleting the directory and harvesting
again throws the old view away before the new one exists.

`harvest.Config.FullRefresh` rebuilds the local view of one (base URL,
format, set) group in place:

```go
w, err := store.OpenWriter(dir, store.Identity{
    BaseURL: "https://example.org/oai",
    Format:  "oai_dc",
    Set:     "main",
})
if err != nil {
    return err
}
defer w.Close()

h := &harvest.Harvest{
    Config: &harvest.Config{
        BaseURL:       "https://example.org/oai",
        Format:        "oai_dc",
        Set:           "main",
        From:          "2020-01-01", // optional; defaults to the endpoint's earliest datestamp
        DailyInterval: true,         // windowing works as on an incremental run
        FullRefresh:   true,
    },
    Client:   client,   // as usual
    Writer:   w,        // the caller keeps owning the writer
    Identify: identify, // or let NewHarvestWithClient fetch it
}
if err := h.Run(ctx); err != nil {
    // Nothing was published. The group still serves the previous view,
    // and a later run can refresh again.
    return err
}
```

What it does:

* The plan covers the whole span from the endpoint's `Identify` (or `From`)
  to `Until`, ignoring the stored resume point and any windows the store
  already covers. `DailyInterval`, `HourlyInterval`, `Set`, the metadata
  prefix and resumption tokens work exactly as on an incremental run.
* The result **replaces** everything the group holds - including ranges
  outside the refreshed span. The unit of replacement is the group, so other
  formats and sets of the same endpoint are untouched.
* The switch is **atomic**. While the refresh runs, `store.Open`, `Records`,
  `Stat` and any other process keep reading the old data and the old
  coverage; a read already in progress finishes the old view. When the last
  window commits, the new data, the record counts and the coverage become
  visible together.
* A refresh publishes only a **complete** pass. `noRecordsMatch` is a
  legitimate empty result, but HTTP or protocol errors, a refused or
  repeating resumption token, and a window cut short by `MaxRequests` or
  `MaxEmptyResponses` all fail the run with `harvest.ErrIncompleteRefresh`
  (or the underlying error) - even when `IgnoreHTTPErrors` or
  `IgnoreUnexpectedEOF` would let an incremental run skip past. Nothing is
  switched over: the old view survives, and closing and reopening the writer
  is enough to try again.
* An all-empty refresh is a real outcome, not a missing one: the group reads
  as harvested and empty, and the next incremental run resumes from the new
  (empty) coverage. After any completed refresh, the same writer can go on
  harvesting incrementally.

The superseded bytes stay in the segment files, unreachable through the new
index - the same thing a re-fetched window has always left behind, so no
extra cleanup is needed.
