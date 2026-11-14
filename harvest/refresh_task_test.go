package harvest

import (
	"context"
	"errors"
	"fmt"
	"io"
	"iter"
	"net/http"
	"os"
	"os/exec"
	"slices"
	"strings"
	"testing"
	"time"

	"github.com/miku/metha/oai"
	"github.com/miku/metha/store"
	"github.com/miku/metha/task_support/taskcheck"
)

// refreshIdentity is the endpoint the refresh tests harvest into. It carries
// a set, so the request sequence assertions see the set parameter flow
// through a refresh the way the protocol sends it.
var refreshIdentity = store.Identity{
	BaseURL: "http://refresh.example.invalid/oai",
	Format:  "oai_dc",
	Set:     "main",
}

// refreshPage is one scripted answer an endpoint can give.
type refreshPage struct {
	ids    []string      // identifiers to answer with
	stamp  string        // datestamp for the records; defaults to the request's from
	token  string        // resumption token to answer with; empty ends the chain
	status int           // HTTP status to fail with instead, zero for 200
	oaiErr string        // OAI error code to answer with, e.g. "noRecordsMatch"
	gate   chan struct{} // closed when this page is requested
	wait   chan struct{} // the answer blocks until this is closed
}

// scriptDoer is an endpoint whose answers are a table, keyed by what a
// request asks for: the range for a first page, the token for every page
// after. It keeps the queries it was asked, in order, so a test can assert
// the exact request sequence a refresh made.
type scriptDoer struct {
	pages map[string]refreshPage
	asked []string
}

// pageKey is what identifies a request to the script: a resumption token is
// an exclusive argument in OAI-PMH, so a paged request is its token and
// nothing else.
func pageKey(q map[string][]string) string {
	if tok := first(q, "resumptionToken"); tok != "" {
		return "token:" + tok
	}
	return "range:" + first(q, "from") + ":" + first(q, "until")
}

func first(q map[string][]string, key string) string {
	if vs := q[key]; len(vs) > 0 {
		return vs[0]
	}
	return ""
}

func (d *scriptDoer) Do(req *http.Request) (*http.Response, error) {
	q := req.URL.Query()
	d.asked = append(d.asked, req.URL.RawQuery)
	p, ok := d.pages[pageKey(q)]
	if !ok {
		return nil, fmt.Errorf("unscripted request: %s", req.URL.RawQuery)
	}
	if p.gate != nil {
		close(p.gate)
	}
	if p.wait != nil {
		<-p.wait
	}
	if p.status != 0 {
		return &http.Response{
			StatusCode: p.status,
			Status:     fmt.Sprintf("%d %s", p.status, http.StatusText(p.status)),
			Body:       io.NopCloser(strings.NewReader("")),
			Header:     make(http.Header),
			Request:    req,
		}, nil
	}
	var body string
	if p.oaiErr != "" {
		body = `<OAI-PMH><error code="` + p.oaiErr + `">scripted</error></OAI-PMH>`
	} else {
		stamp := p.stamp
		if stamp == "" {
			stamp = first(q, "from")
		}
		var b strings.Builder
		b.WriteString(`<OAI-PMH><ListRecords>`)
		for _, id := range p.ids {
			fmt.Fprintf(&b, `<record><header><identifier>%s</identifier>`+
				`<datestamp>%s</datestamp></header><metadata><title>%s</title></metadata></record>`,
				id, stamp, id)
		}
		if p.token != "" {
			fmt.Fprintf(&b, `<resumptionToken>%s</resumptionToken>`, p.token)
		}
		b.WriteString(`</ListRecords></OAI-PMH>`)
		body = b.String()
	}
	return &http.Response{
		StatusCode: http.StatusOK,
		Body:       io.NopCloser(strings.NewReader(body)),
		Header:     make(http.Header),
		Request:    req,
	}, nil
}

// refreshConfig is a daily, day-granularity harvest over a fixed past span,
// which makes every window boundary deterministic.
func refreshConfig(from, until string) *Config {
	return &Config{
		BaseURL:           refreshIdentity.BaseURL,
		Format:            refreshIdentity.Format,
		Set:               refreshIdentity.Set,
		From:              from,
		Until:             until,
		DailyInterval:     true,
		MaxRetries:        0,
		RetryDelay:        time.Millisecond,
		RetryBackoff:      1.0,
		MaxEmptyResponses: 10,
	}
}

// refreshHarvest wires a harvest over a scripted endpoint, with the identify
// response a day-granularity endpoint would have given.
func refreshHarvest(w *store.Writer, d *scriptDoer, cfg *Config) *Harvest {
	return &Harvest{
		Config: cfg,
		Client: &oai.Client{Doer: d},
		Writer: w,
		Identify: &oai.Identify{
			Granularity:       "YYYY-MM-DD",
			EarliestDatestamp: "2020-01-01",
		},
	}
}

// openWriter is store.OpenWriter with the test's identity.
func openWriter(c *taskcheck.C, base string) *store.Writer {
	w, err := store.OpenWriter(base, refreshIdentity)
	c.Check(err == nil, "OpenWriter: %v", err)
	return w
}

// closeWriter closes a writer and records whether that worked.
func closeWriter(c *taskcheck.C, w *store.Writer) {
	err := w.Close()
	c.Check(err == nil, "close: %v", err)
}

// collectIDs reads every identifier a store holds, in read order.
func collectIDs(s store.Store) ([]string, error) {
	var ids []string
	for rec, err := range s.Records(store.ReadOptions{}) {
		if err != nil {
			return ids, err
		}
		ids = append(ids, rec.Header.Identifier)
	}
	return ids, nil
}

// seedOld harvests one old record per day over the span, which is the view a
// refresh is then asked to replace.
func seedOld(c *taskcheck.C, base, from, until string, ids ...string) {
	pages := make(map[string]refreshPage)
	day, _ := time.ParseInLocation("2006-01-02", from, time.Local)
	for i, id := range ids {
		d := day.AddDate(0, 0, i).Format("2006-01-02")
		pages["range:"+d+":"+d] = refreshPage{ids: []string{id}}
	}
	d := &scriptDoer{pages: pages}
	w := openWriter(c, base)
	h := refreshHarvest(w, d, refreshConfig(from, until))
	err := h.Run(context.Background())
	c.Check(err == nil, "seed harvest: %v", err)
	err = w.Close()
	c.Check(err == nil, "seed close: %v", err)
}

// TestRefreshPlan: a full refresh plans the whole configured span from the
// endpoint and From/Until, ignoring what the store already covers - the old
// resume point and the old windows would have planned nothing at all here.
// Windows and resumption pages follow the protocol as they do on an
// incremental run, and once the refresh has switched, an incremental run on
// the same writer resumes from the new coverage only.
func TestRefreshPlan(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		base := t.TempDir()

		// Four settled days of old data; a resume point past them.
		seedOld(c, base, "2020-01-01", "2020-01-04", "old-01", "old-02", "old-03", "old-04")
		stats, err := store.Stat(base, refreshIdentity)
		c.Check(err == nil, "seed stat: %v", err)
		c.Check(stats.Records == 4, "seeded records: got %d, want 4", stats.Records)
		c.Check(stats.Last == "2020-01-04", "seeded coverage: got %q, want 2020-01-04", stats.Last)

		// Another set of the same endpoint lives in the same shard, and a
		// refresh of this group is not allowed to touch it.
		sibling := store.Identity{
			BaseURL: refreshIdentity.BaseURL,
			Format:  refreshIdentity.Format,
			Set:     "other",
		}
		sw, err := store.OpenWriter(base, sibling)
		c.Check(err == nil, "sibling OpenWriter: %v", err)
		siblingDoer := &scriptDoer{pages: map[string]refreshPage{
			"range:2020-01-01:2020-01-01": {ids: []string{"sibling-01"}},
		}}
		siblingCfg := refreshConfig("2020-01-01", "2020-01-01")
		siblingCfg.Set = sibling.Set
		sh := refreshHarvest(sw, siblingDoer, siblingCfg)
		err = sh.Run(t.Context())
		c.Check(err == nil, "sibling harvest: %v", err)
		err = sw.Close()
		c.Check(err == nil, "sibling close: %v", err)

		// Refresh a narrower span than the store covers, the first window in
		// two pages. Old coverage ends at 01-04, so anything short of a full
		// plan would either ask for nothing or start at 01-05.
		d := &scriptDoer{pages: map[string]refreshPage{
			"range:2020-01-01:2020-01-01": {ids: []string{"new-01a"}, token: "w1p2"},
			"token:w1p2":                  {ids: []string{"new-01b"}, stamp: "2020-01-01"},
			"range:2020-01-02:2020-01-02": {ids: []string{"new-02"}},
			"range:2020-01-03:2020-01-03": {ids: []string{"new-03"}},
		}}
		w := openWriter(c, base)
		cfg := refreshConfig("2020-01-01", "2020-01-03")
		cfg.FullRefresh = true
		err = refreshHarvest(w, d, cfg).Run(t.Context())
		c.Check(err == nil, "refresh: %v", err)

		// The request sequence is the whole span from its start, set and
		// metadataPrefix on every first page, the resumption token alone on
		// the second - and nothing skipped as already harvested.
		want := []string{
			"from=2020-01-01&metadataPrefix=oai_dc&set=main&until=2020-01-01&verb=ListRecords",
			"resumptionToken=w1p2&verb=ListRecords",
			"from=2020-01-02&metadataPrefix=oai_dc&set=main&until=2020-01-02&verb=ListRecords",
			"from=2020-01-03&metadataPrefix=oai_dc&set=main&until=2020-01-03&verb=ListRecords",
		}
		c.Check(slices.Equal(d.asked, want), "request sequence:\n got %q\nwant %q", d.asked, want)

		// The same writer goes on incrementally, with no refresh state left
		// behind: only the tail past the new coverage is planned.
		c.Check(!w.Refreshing(), "writer still refreshing after a committed refresh")
		inc := &scriptDoer{pages: map[string]refreshPage{
			"range:2020-01-04:2020-01-04": {ids: []string{"new-04"}},
		}}
		err = refreshHarvest(w, inc, refreshConfig("", "2020-01-04")).Run(t.Context())
		c.Check(err == nil, "incremental after refresh: %v", err)
		wantInc := []string{
			"from=2020-01-04&metadataPrefix=oai_dc&set=main&until=2020-01-04&verb=ListRecords",
		}
		c.Check(slices.Equal(inc.asked, wantInc), "incremental sequence:\n got %q\nwant %q", inc.asked, wantInc)
		closeWriter(c, w)

		// The new view is the refreshed span plus the incremental tail. The
		// old data is gone - old-04 lay outside the refreshed span and is not
		// kept either.
		s, err := store.Open(base, refreshIdentity)
		c.Check(err == nil, "open: %v", err)
		ids, err := collectIDs(s)
		c.Check(err == nil, "records: %v", err)
		wantIDs := []string{"new-01a", "new-01b", "new-02", "new-03", "new-04"}
		c.Check(slices.Equal(ids, wantIDs), "view after refresh:\n got %v\nwant %v", ids, wantIDs)

		stats, err = store.Stat(base, refreshIdentity)
		c.Check(err == nil, "stat: %v", err)
		c.Check(stats.Records == 5, "records after refresh: got %d, want 5", stats.Records)
		c.Check(stats.First == "2020-01-01" && stats.Last == "2020-01-04",
			"coverage after refresh: got %q to %q", stats.First, stats.Last)
		c.Check(stats.Failed == 0, "failed windows after refresh: got %d", stats.Failed)

		// The sibling group of the same endpoint is untouched: the refresh
		// replaced this group's view, not the shard's.
		ss, err := store.Open(base, sibling)
		c.Check(err == nil, "sibling open: %v", err)
		siblingIDs, err := collectIDs(ss)
		c.Check(err == nil, "sibling records: %v", err)
		c.Check(slices.Equal(siblingIDs, []string{"sibling-01"}),
			"sibling view after refresh: got %v", siblingIDs)
		siblingStats, err := store.Stat(base, sibling)
		c.Check(err == nil, "sibling stat: %v", err)
		c.Check(siblingStats.Records == 1 && siblingStats.Last == "2020-01-01",
			"sibling coverage after refresh: got %d records until %q",
			siblingStats.Records, siblingStats.Last)

		// Coverage is now judged from the new set alone: the refreshed range
		// is covered, so a repeat run over it is already synced.
		w2 := openWriter(c, base)
		err = refreshHarvest(w2, &scriptDoer{}, refreshConfig("", "2020-01-04")).Run(t.Context())
		c.Check(errors.Is(err, ErrAlreadySynced), "re-run after refresh: %v", err)
		closeWriter(c, w2)
	})
}

// TestRefreshVisibility: until the last window of a refresh commits, every
// reader - this process, an iterator already walking, another process - sees
// exactly the old view. The switch publishes the new data, the counts and the
// coverage together. An all-empty refresh is a real outcome: it replaces the
// old data with nothing, reads as harvested-and-empty rather than never
// harvested, and the next incremental run resumes from the new empty set.
func TestRefreshVisibility(t *testing.T) {
	// The subprocess half of the test: same binary, pointed at the cache by
	// the environment, printing what a fresh reader sees there.
	if dir := os.Getenv("METHA_REFRESH_READ_DIR"); dir != "" {
		runRefreshReader(dir)
		return
	}
	taskcheck.Run(t, func(c *taskcheck.C) {
		base := t.TempDir()
		seedOld(c, base, "2020-01-01", "2020-01-03", "old-01", "old-02", "old-03")

		// A refresh of two windows, held up inside the second: the first has
		// committed to the staging index by then, and none of it may show.
		gate := make(chan struct{})
		release := make(chan struct{})
		released := false
		defer func() {
			if !released {
				close(release)
			}
		}()
		d := &scriptDoer{pages: map[string]refreshPage{
			"range:2020-01-01:2020-01-01": {ids: []string{"new-01"}},
			"range:2020-01-02:2020-01-02": {ids: []string{"new-02"}, gate: gate, wait: release},
		}}
		w := openWriter(c, base)
		cfg := refreshConfig("2020-01-01", "2020-01-02")
		cfg.FullRefresh = true
		errch := make(chan error, 1)
		go func() { errch <- refreshHarvest(w, d, cfg).Run(t.Context()) }()
		select {
		case <-gate:
		case <-time.After(30 * time.Second):
			t.Fatal("refresh did not reach its second window")
		}

		// Mid-refresh, this process reads the old data and the old coverage.
		s, err := store.Open(base, refreshIdentity)
		c.Check(err == nil, "open: %v", err)
		ids, err := collectIDs(s)
		c.Check(err == nil, "mid-refresh records: %v", err)
		c.Check(slices.Equal(ids, []string{"old-01", "old-02", "old-03"}),
			"mid-refresh view: got %v", ids)
		stats, err := store.Stat(base, refreshIdentity)
		c.Check(err == nil, "mid-refresh stat: %v", err)
		c.Check(stats.Records == 3 && stats.Last == "2020-01-03",
			"mid-refresh coverage: got %d records until %q", stats.Records, stats.Last)

		// So does another process, reading the same cache directory.
		cmd := exec.Command(os.Args[0], "-test.run=^TestRefreshVisibility$")
		cmd.Env = append(os.Environ(), "METHA_REFRESH_READ_DIR="+base)
		out, err := cmd.Output()
		c.Check(err == nil, "reader subprocess: %v", err)
		c.Check(strings.Contains(string(out), "ids=old-01,old-02,old-03"),
			"subprocess view: %s", out)
		c.Check(strings.Contains(string(out), "records=3 first=2020-01-01 last=2020-01-03"),
			"subprocess coverage: %s", out)

		// A read that starts now walks the old view to its end, even across
		// the switch: the old bytes are not touched by it.
		next, stop := iter.Pull2(s.Records(store.ReadOptions{}))
		defer stop()
		rec, _, ok := next()
		c.Check(ok && rec.Header.Identifier == "old-01", "in-flight read starts at old-01: %v", rec.Header.Identifier)

		close(release)
		released = true
		select {
		case err = <-errch:
			c.Check(err == nil, "refresh: %v", err)
		case <-time.After(30 * time.Second):
			t.Fatal("refresh did not finish")
		}

		var rest []string
		for {
			rec, err, ok := next()
			if !ok {
				break
			}
			c.Check(err == nil, "in-flight read: %v", err)
			rest = append(rest, rec.Header.Identifier)
		}
		c.Check(slices.Equal(rest, []string{"old-02", "old-03"}),
			"in-flight read across the switch: got %v", rest)

		// After the switch, data, counts and coverage are the new round's.
		ids, err = collectIDs(s)
		c.Check(err == nil, "records after switch: %v", err)
		c.Check(slices.Equal(ids, []string{"new-01", "new-02"}), "view after switch: got %v", ids)
		stats, err = store.Stat(base, refreshIdentity)
		c.Check(err == nil, "stat after switch: %v", err)
		c.Check(stats.Records == 2 && stats.First == "2020-01-01" && stats.Last == "2020-01-02",
			"coverage after switch: got %d records, %q to %q", stats.Records, stats.First, stats.Last)
		closeWriter(c, w)

		// An all-empty refresh: noRecordsMatch is a legitimate empty answer,
		// and the empty result replaces the old data like any other.
		empty := &scriptDoer{pages: map[string]refreshPage{
			"range:2020-01-01:2020-01-01": {oaiErr: "noRecordsMatch"},
			"range:2020-01-02:2020-01-02": {oaiErr: "noRecordsMatch"},
		}}
		w2 := openWriter(c, base)
		cfg2 := refreshConfig("2020-01-01", "2020-01-02")
		cfg2.FullRefresh = true
		err = refreshHarvest(w2, empty, cfg2).Run(t.Context())
		c.Check(err == nil, "empty refresh: %v", err)
		closeWriter(c, w2)

		// Harvested and empty, not never harvested: no error, no records, and
		// the coverage of the empty set.
		ids, err = collectIDs(s)
		c.Check(err == nil, "records after empty refresh: %v", err)
		c.Check(len(ids) == 0, "view after empty refresh: got %v", ids)
		stats, err = store.Stat(base, refreshIdentity)
		c.Check(err == nil, "stat after empty refresh: %v", err)
		c.Check(stats.Records == 0 && stats.Empty == 1,
			"empty refresh counts: got %d records, %d empty windows", stats.Records, stats.Empty)
		c.Check(stats.Last == "2020-01-02", "empty refresh coverage: got %q", stats.Last)

		// The next incremental run resumes from the new empty set - one
		// window, starting the day after it ends. Were the empty refresh
		// treated as never harvested, the plan would start over from the
		// endpoint's earliest datestamp instead.
		inc := &scriptDoer{pages: map[string]refreshPage{
			"range:2020-01-03:2020-01-03": {ids: []string{"after-03"}},
		}}
		w3 := openWriter(c, base)
		err = refreshHarvest(w3, inc, refreshConfig("", "2020-01-03")).Run(t.Context())
		c.Check(err == nil, "incremental after empty refresh: %v", err)
		wantInc := []string{
			"from=2020-01-03&metadataPrefix=oai_dc&set=main&until=2020-01-03&verb=ListRecords",
		}
		c.Check(slices.Equal(inc.asked, wantInc),
			"incremental after empty refresh:\n got %q\nwant %q", inc.asked, wantInc)
		closeWriter(c, w3)
	})
}

// runRefreshReader is the subprocess half of TestRefreshVisibility: a fresh
// process prints what it reads from the cache, so the parent can compare it
// against the view it sees itself.
func runRefreshReader(dir string) {
	s, err := store.Open(dir, refreshIdentity)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	ids, err := collectIDs(s)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	stats, err := store.Stat(dir, refreshIdentity)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Printf("ids=%s records=%d first=%s last=%s\n",
		strings.Join(ids, ","), stats.Records, stats.First, stats.Last)
	os.Exit(0)
}

// TestRefreshRecovery: a refresh that cannot complete publishes nothing -
// not an HTTP failure, not one the configuration would tolerate on an
// incremental run, not a broken or circling token chain, not a window a
// limit cut short. The old view survives every one of them, a closed and
// reopened writer can refresh again, and a writer that failed a refresh can
// both refresh and harvest incrementally afterwards.
func TestRefreshRecovery(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		base := t.TempDir()
		seedOld(c, base, "2020-01-01", "2020-01-02", "old-01", "old-02")

		s, err := store.Open(base, refreshIdentity)
		c.Check(err == nil, "open: %v", err)
		oldView := func(round string) {
			ids, err := collectIDs(s)
			c.Check(err == nil, "%s: records: %v", round, err)
			c.Check(slices.Equal(ids, []string{"old-01", "old-02"}),
				"%s: view: got %v", round, ids)
			stats, err := store.Stat(base, refreshIdentity)
			c.Check(err == nil, "%s: stat: %v", round, err)
			c.Check(stats.Records == 2 && stats.Failed == 0,
				"%s: counts: got %d records, %d failed", round, stats.Records, stats.Failed)
		}

		// The second window fails with HTTP 500.
		failing := &scriptDoer{pages: map[string]refreshPage{
			"range:2020-01-01:2020-01-01": {ids: []string{"new-01"}},
			"range:2020-01-02:2020-01-02": {status: 500},
		}}
		w := openWriter(c, base)
		cfg := refreshConfig("2020-01-01", "2020-01-02")
		cfg.FullRefresh = true
		err = refreshHarvest(w, failing, cfg).Run(t.Context())
		c.Check(err != nil, "refresh with a failing window returned nil")
		closeWriter(c, w)

		// Closed and reopened, the writer sees the old coverage and no
		// leftover refresh state; the store still serves the old view, and no
		// failure row leaked into the live index.
		w2 := openWriter(c, base)
		c.Check(!w2.Refreshing(), "reopened writer is refreshing")
		noon := time.Date(2020, 1, 1, 12, 0, 0, 0, time.Local)
		c.Check(w2.HasWindow(noon, noon), "old coverage no longer visible after failed refresh")
		c.Check(w2.Resume().Format("2006-01-02") == "2020-01-03",
			"resume after failed refresh: got %v", w2.Resume())
		closeWriter(c, w2)
		oldView("after failed refresh")

		// The same failure under IgnoreHTTPErrors: tolerated on an
		// incremental run, still not a complete refresh.
		w3 := openWriter(c, base)
		cfgTolerant := refreshConfig("2020-01-01", "2020-01-02")
		cfgTolerant.FullRefresh = true
		cfgTolerant.IgnoreHTTPErrors = true
		err = refreshHarvest(w3, failing, cfgTolerant).Run(t.Context())
		c.Check(errors.Is(err, ErrIncompleteRefresh),
			"tolerated failure: got %v, want ErrIncompleteRefresh", err)
		closeWriter(c, w3)
		oldView("after tolerated failure")

		// A healthy retry on a reopened writer succeeds and switches.
		good := &scriptDoer{pages: map[string]refreshPage{
			"range:2020-01-01:2020-01-01": {ids: []string{"new-01"}},
			"range:2020-01-02:2020-01-02": {ids: []string{"new-02"}},
		}}
		w4 := openWriter(c, base)
		cfgGood := refreshConfig("2020-01-01", "2020-01-02")
		cfgGood.FullRefresh = true
		err = refreshHarvest(w4, good, cfgGood).Run(t.Context())
		c.Check(err == nil, "retry refresh: %v", err)
		ids, err := collectIDs(s)
		c.Check(err == nil, "records after retry: %v", err)
		c.Check(slices.Equal(ids, []string{"new-01", "new-02"}), "view after retry: got %v", ids)

		// From here the same writer stays in use: a failed refresh must not
		// leave it in a state the next run trips over.

		// A window a request limit cuts short is not a complete pass.
		limited := &scriptDoer{pages: map[string]refreshPage{
			"range:2020-01-01:2020-01-01": {ids: []string{"lim-01a"}, token: "lim"},
			"token:lim":                   {ids: []string{"lim-01b"}, stamp: "2020-01-01"},
			"range:2020-01-02:2020-01-02": {ids: []string{"lim-02"}},
		}}
		cfgLimited := refreshConfig("2020-01-01", "2020-01-02")
		cfgLimited.FullRefresh = true
		cfgLimited.MaxRequests = 1
		err = refreshHarvest(w4, limited, cfgLimited).Run(t.Context())
		c.Check(errors.Is(err, ErrIncompleteRefresh),
			"max requests: got %v, want ErrIncompleteRefresh", err)

		// A token chain that goes in circles.
		dup := &scriptDoer{pages: map[string]refreshPage{
			"range:2020-01-01:2020-01-01": {ids: []string{"dup-01"}, token: "dup"},
			"token:dup":                   {ids: []string{"dup-01b"}, stamp: "2020-01-01", token: "dup"},
			"range:2020-01-02:2020-01-02": {ids: []string{"dup-02"}},
		}}
		err = refreshHarvest(w4, dup, cfgGood).Run(t.Context())
		c.Check(errors.Is(err, ErrIncompleteRefresh),
			"duplicate token: got %v, want ErrIncompleteRefresh", err)

		// A token the endpoint then refuses.
		bad := &scriptDoer{pages: map[string]refreshPage{
			"range:2020-01-01:2020-01-01": {ids: []string{"bad-01"}, token: "bt"},
			"token:bt":                    {oaiErr: "badResumptionToken"},
			"range:2020-01-02:2020-01-02": {ids: []string{"bad-02"}},
		}}
		err = refreshHarvest(w4, bad, cfgGood).Run(t.Context())
		c.Check(errors.Is(err, ErrIncompleteRefresh),
			"bad resumption token: got %v, want ErrIncompleteRefresh", err)

		// A chain of empty pages that runs into the empty-response limit.
		empties := &scriptDoer{pages: map[string]refreshPage{
			"range:2020-01-01:2020-01-01": {token: "e1"},
			"token:e1":                    {token: "e2"},
			"token:e2":                    {},
			"range:2020-01-02:2020-01-02": {ids: []string{"e-02"}},
		}}
		cfgEmpty := refreshConfig("2020-01-01", "2020-01-02")
		cfgEmpty.FullRefresh = true
		cfgEmpty.MaxEmptyResponses = 2
		err = refreshHarvest(w4, empties, cfgEmpty).Run(t.Context())
		c.Check(errors.Is(err, ErrIncompleteRefresh),
			"empty responses: got %v, want ErrIncompleteRefresh", err)

		// After every one of those, the view is still the last completed
		// refresh, and the same writer both refreshes and harvests
		// incrementally without a reopen.
		ids, err = collectIDs(s)
		c.Check(err == nil, "records after failed refreshes: %v", err)
		c.Check(slices.Equal(ids, []string{"new-01", "new-02"}),
			"view after failed refreshes: got %v", ids)
		stats, err := store.Stat(base, refreshIdentity)
		c.Check(err == nil, "stat after failed refreshes: %v", err)
		c.Check(stats.Records == 2 && stats.Failed == 0,
			"counts after failed refreshes: got %d records, %d failed", stats.Records, stats.Failed)

		good2 := &scriptDoer{pages: map[string]refreshPage{
			"range:2020-01-01:2020-01-01": {ids: []string{"final-01"}},
			"range:2020-01-02:2020-01-02": {ids: []string{"final-02"}},
		}}
		err = refreshHarvest(w4, good2, cfgGood).Run(t.Context())
		c.Check(err == nil, "refresh after failures on the same writer: %v", err)
		inc := &scriptDoer{pages: map[string]refreshPage{
			"range:2020-01-03:2020-01-03": {ids: []string{"final-03"}},
		}}
		err = refreshHarvest(w4, inc, refreshConfig("", "2020-01-03")).Run(t.Context())
		c.Check(err == nil, "incremental after failures on the same writer: %v", err)
		closeWriter(c, w4)

		ids, err = collectIDs(s)
		c.Check(err == nil, "final records: %v", err)
		c.Check(slices.Equal(ids, []string{"final-01", "final-02", "final-03"}),
			"final view: got %v", ids)
	})
}

// ExampleConfig_FullRefresh rebuilds a group's local view of an endpoint:
// the whole span is planned and fetched again, and the result replaces
// everything the group held - atomically, so readers see the old view until
// the new one is complete.
func ExampleConfig_FullRefresh() {
	dir, err := os.MkdirTemp("", "metha-refresh-")
	if err != nil {
		panic(err)
	}
	defer os.RemoveAll(dir)
	id := store.Identity{BaseURL: "http://refresh.example.invalid/oai", Format: "oai_dc", Set: "main"}

	// What the endpoint said when the group was first harvested: one record.
	first := &scriptDoer{pages: map[string]refreshPage{
		"range:2020-01-01:2020-01-01": {ids: []string{"old-record"}},
	}}
	w, err := store.OpenWriter(dir, id)
	if err != nil {
		panic(err)
	}
	cfg := &Config{
		BaseURL: id.BaseURL, Format: id.Format, Set: id.Set,
		From: "2020-01-01", Until: "2020-01-01", DailyInterval: true,
	}
	h := &Harvest{
		Config:   cfg,
		Client:   &oai.Client{Doer: first},
		Writer:   w,
		Identify: &oai.Identify{Granularity: "YYYY-MM-DD", EarliestDatestamp: "2020-01-01"},
	}
	if err := h.Run(context.Background()); err != nil {
		panic(err)
	}

	// The endpoint's history changed since - a record was removed upstream,
	// which an incremental run never hears about. A full refresh replaces the
	// group's whole view with what the endpoint says now.
	second := &scriptDoer{pages: map[string]refreshPage{
		"range:2020-01-01:2020-01-01": {ids: []string{"new-record"}},
	}}
	h.Config = &Config{
		BaseURL: id.BaseURL, Format: id.Format, Set: id.Set,
		From: "2020-01-01", Until: "2020-01-01", DailyInterval: true,
		FullRefresh: true,
	}
	h.Client = &oai.Client{Doer: second}
	if err := h.Run(context.Background()); err != nil {
		panic(err)
	}
	if err := w.Close(); err != nil {
		panic(err)
	}

	s, err := store.Open(dir, id)
	if err != nil {
		panic(err)
	}
	for rec, err := range s.Records(store.ReadOptions{}) {
		if err != nil {
			panic(err)
		}
		fmt.Println(rec.Header.Identifier)
	}
	// Output:
	// new-record
}
