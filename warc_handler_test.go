package fetchbot

import (
	"bufio"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/textproto"
	"net/url"
	"os"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/PuerkitoBio/fetchbot/task_support/taskcheck"
)

// warcRespSpec describes one scripted response of the controlled Doer.
type warcRespSpec struct {
	status       int
	header       http.Header
	body         []byte
	uncompressed bool
	noRequest    bool // leave res.Request nil so the Command URL must be used
	makeBody     func() io.ReadCloser
}

// warcServer is a controlled Doer scripting responses per URL. When several
// specs are queued for one URL, they are consumed in order.
type warcServer struct {
	mu     sync.Mutex
	specs  map[string][]warcRespSpec
	closed *int32
	kept   map[string]*http.Response
}

func (s *warcServer) Do(r *http.Request) (*http.Response, error) {
	s.mu.Lock()
	q := s.specs[r.URL.String()]
	if len(q) == 0 {
		s.mu.Unlock()
		return nil, fmt.Errorf("unexpected request %s %s", r.Method, r.URL)
	}
	spec := q[0]
	if len(q) > 1 {
		s.specs[r.URL.String()] = q[1:]
	}
	s.mu.Unlock()

	st := strings.TrimSpace(strconv.Itoa(spec.status) + " " + http.StatusText(spec.status))
	res := &http.Response{
		Status:       st,
		StatusCode:   spec.status,
		Proto:        "HTTP/1.1",
		ProtoMajor:   1,
		ProtoMinor:   1,
		Header:       spec.header.Clone(),
		Uncompressed: spec.uncompressed,
	}
	if spec.makeBody != nil {
		res.Body = spec.makeBody()
	} else {
		res.Body = &testBody{bytes.NewReader(spec.body), s.closed}
	}
	if !spec.noRequest {
		res.Request = r
	}
	if s.kept != nil {
		s.mu.Lock()
		s.kept[r.URL.String()] = res
		s.mu.Unlock()
	}
	return res, nil
}

// startWARCFetcher starts a real Fetcher with politeness and delays disabled.
func startWARCFetcher(h Handler, doer Doer) *Queue {
	f := New(h)
	f.DisablePoliteness = true
	f.CrawlDelay = 0
	f.HttpClient = doer
	return f.Start()
}

// warcRec is a WARC record parsed back from the data output.
type warcRec struct {
	version string
	header  textproto.MIMEHeader
	block   []byte
	offset  int
	length  int
}

// parseWARC is an independent small parser: it walks the data output using
// only the WARC Content-Length of each record.
func parseWARC(t *testing.T, data []byte) []warcRec {
	t.Helper()
	src := bytes.NewReader(data)
	b := bufio.NewReader(src)
	pos := func() int { return len(data) - src.Len() - b.Buffered() }
	var recs []warcRec
	for pos() < len(data) {
		start := pos()
		version, err := b.ReadString('\n')
		if err != nil {
			t.Fatalf("record %d: version line: %v", len(recs), err)
		}
		hdr, err := textproto.NewReader(b).ReadMIMEHeader()
		if err != nil {
			t.Fatalf("record %d: WARC header: %v", len(recs), err)
		}
		cl, err := strconv.Atoi(hdr.Get("Content-Length"))
		if err != nil {
			t.Fatalf("record %d: Content-Length: %v", len(recs), err)
		}
		block := make([]byte, cl)
		if _, err := io.ReadFull(b, block); err != nil {
			t.Fatalf("record %d: block: %v", len(recs), err)
		}
		sep := make([]byte, len(warcRecordTrailer))
		if _, err := io.ReadFull(b, sep); err != nil {
			t.Fatalf("record %d: separator: %v", len(recs), err)
		}
		if string(sep) != warcRecordTrailer {
			t.Fatalf("record %d: separator %q", len(recs), sep)
		}
		recs = append(recs, warcRec{strings.TrimSpace(version), hdr, block, start, pos() - start})
	}
	return recs
}

// parseBlock splits a record block into status line, HTTP headers and body.
func parseBlock(t *testing.T, block []byte) (string, textproto.MIMEHeader, []byte) {
	t.Helper()
	b := bufio.NewReader(bytes.NewReader(block))
	status, err := b.ReadString('\n')
	if err != nil {
		t.Fatalf("block status line: %v", err)
	}
	hdr, err := textproto.NewReader(b).ReadMIMEHeader()
	if err != nil {
		t.Fatalf("block headers: %v", err)
	}
	body, err := io.ReadAll(b)
	if err != nil {
		t.Fatalf("block body: %v", err)
	}
	return strings.TrimSpace(status), hdr, body
}

func sha256Digest(b []byte) string {
	s := sha256.Sum256(b)
	return "sha256:" + hex.EncodeToString(s[:])
}

// warcIdx is one parsed JSON index line.
type warcIdx struct {
	ID            string `json:"id"`
	URL           string `json:"url"`
	Date          string `json:"date"`
	Offset        int64  `json:"offset"`
	Length        int64  `json:"length"`
	PayloadDigest string `json:"payload_digest"`
	RevisitOf     string `json:"revisit_of"`
}

func parseIndex(t *testing.T, data []byte) []warcIdx {
	t.Helper()
	var out []warcIdx
	for _, line := range strings.Split(strings.TrimSpace(string(data)), "\n") {
		if line == "" {
			continue
		}
		var e warcIdx
		if err := json.Unmarshal([]byte(line), &e); err != nil {
			t.Fatalf("index line %q: %v", line, err)
		}
		out = append(out, e)
	}
	return out
}

// checkIndexCrossRef verifies that index lines match the parsed records one
// to one, in order, including the exact byte ranges in the data output.
func checkIndexCrossRef(c *taskcheck.C, recs []warcRec, idx []warcIdx, data []byte) {
	c.Check(len(idx) == len(recs), "index lines %d != records %d", len(idx), len(recs))
	n := len(recs)
	if len(idx) < n {
		n = len(idx)
	}
	for i := 0; i < n; i++ {
		c.Check(idx[i].Offset == int64(recs[i].offset) && idx[i].Length == int64(recs[i].length),
			"index[%d] range %d/%d != record range %d/%d", i, idx[i].Offset, idx[i].Length, recs[i].offset, recs[i].length)
		if idx[i].Offset >= 0 && idx[i].Offset+idx[i].Length <= int64(len(data)) {
			seg := data[idx[i].Offset : idx[i].Offset+idx[i].Length]
			c.Check(bytes.HasPrefix(seg, []byte("WARC/1.1\r\n")) && bytes.HasSuffix(seg, []byte(warcRecordTrailer)),
				"index[%d] byte range does not frame a whole record", i)
		}
		c.Check(idx[i].ID == strings.TrimSuffix(strings.TrimPrefix(recs[i].header.Get("WARC-Record-ID"), "<"), ">"),
			"index[%d] id %q != record id %q", i, idx[i].ID, recs[i].header.Get("WARC-Record-ID"))
		c.Check(idx[i].URL == recs[i].header.Get("WARC-Target-URI"),
			"index[%d] url %q != target %q", i, idx[i].URL, recs[i].header.Get("WARC-Target-URI"))
		c.Check(idx[i].Date == recs[i].header.Get("WARC-Date"),
			"index[%d] date %q != WARC-Date %q", i, idx[i].Date, recs[i].header.Get("WARC-Date"))
		c.Check(idx[i].PayloadDigest == recs[i].header.Get("WARC-Payload-Digest"),
			"index[%d] payload digest mismatch", i)
	}
}

func dirEntries(t *testing.T, dir string) []os.DirEntry {
	t.Helper()
	ents, err := os.ReadDir(dir)
	if err != nil {
		t.Fatalf("read temp dir: %v", err)
	}
	return ents
}

func TestWARCRecords(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var (
			data, idx bytes.Buffer
			closed    int32
			tick      int64
			idSeq     int64
		)
		tmp := t.TempDir()
		base := time.Date(2026, 3, 4, 5, 6, 7, 123456789, time.FixedZone("Z", 5*3600+1800))
		h, err := NewWARCHandler(&data, &idx, WARCOptions{
			TempDir: tmp,
			Now: func() time.Time {
				return base.Add(time.Duration(atomic.AddInt64(&tick, 1)) * time.Second)
			},
			NewID: func() string {
				return fmt.Sprintf("urn:test:%d", atomic.AddInt64(&idSeq, 1))
			},
		})
		c.Check(err == nil && h != nil, "NewWARCHandler: %v", err)
		if err != nil {
			return
		}

		binBody := []byte("a\x00b\xff\xfe\x00binary\x01\x02")
		plainBody := []byte("hello one")
		alphaBody := []byte("alpha two")
		noreqBody := []byte("no request pointer")
		headBody := []byte("head-body-ignored")

		kept := map[string]*http.Response{}
		srv := &warcServer{
			closed: &closed,
			kept:   kept,
			specs: map[string][]warcRespSpec{
				"https://one.invalid/bin": {{
					status: 200,
					header: http.Header{
						"Content-Type":      {"application/octet-stream"},
						"X-Multi":           {"1", "2"},
						"Transfer-Encoding": {"chunked"},
						"Content-Length":    {"999"},
						"Content-Encoding":  {"gzip"},
					},
					body:         binBody,
					uncompressed: true,
				}},
				"https://one.invalid/plain": {{
					status: 404,
					header: http.Header{"Content-Type": {"text/plain"}},
					body:   plainBody,
				}},
				"https://two.invalid/alpha": {{
					status: 500,
					header: http.Header{"Content-Type": {"text/plain"}},
					body:   alphaBody,
				}},
				"https://two.invalid/noreq": {{
					status:    200,
					header:    http.Header{"Content-Type": {"text/plain"}},
					body:      noreqBody,
					noRequest: true,
				}},
				"https://two.invalid/head": {{
					status: 200,
					header: http.Header{"Content-Type": {"text/plain"}},
					body:   headBody,
				}},
			},
		}

		q := startWARCFetcher(h, srv)
		if _, err := q.SendStringGet(
			"https://one.invalid/bin",
			"https://one.invalid/plain",
			"https://two.invalid/alpha",
			"https://two.invalid/noreq",
		); err != nil {
			c.Check(false, "send GETs: %v", err)
			return
		}
		if _, err := q.SendStringHead("https://two.invalid/head"); err != nil {
			c.Check(false, "send HEAD: %v", err)
			return
		}
		if err := q.Close(); err != nil {
			c.Check(false, "queue close: %v", err)
			return
		}

		c.Check(h.Err() == nil, "handler error: %v", h.Err())
		// The Fetcher closes every delivered body exactly once; the handler
		// must not close any itself.
		c.Check(atomic.LoadInt32(&closed) == 5, "bodies closed %d times, want 5", atomic.LoadInt32(&closed))

		recs := parseWARC(t, data.Bytes())
		c.Check(len(recs) == 4, "got %d records, want 4 (HEAD must be ignored)", len(recs))
		byURL := map[string]warcRec{}
		ids := map[string]bool{}
		for _, r := range recs {
			c.Check(r.version == "WARC/1.1", "version line %q", r.version)
			c.Check(r.header.Get("Content-Type") == "application/http; msgtype=response",
				"WARC Content-Type %q", r.header.Get("Content-Type"))
			rawID := r.header.Get("WARC-Record-ID")
			c.Check(strings.HasPrefix(rawID, "<urn:test:") && strings.HasSuffix(rawID, ">"),
				"record ID %q is not a bracketed test URN", rawID)
			ids[rawID] = true
			d := r.header.Get("WARC-Date")
			parsed, perr := time.Parse(time.RFC3339Nano, d)
			c.Check(perr == nil && strings.HasSuffix(d, "Z"), "WARC-Date %q not UTC RFC3339Nano: %v", d, perr)
			if perr == nil {
				c.Check(parsed.Location() == time.UTC || strings.HasSuffix(d, "Z"), "WARC-Date %q not UTC", d)
			}
			byURL[r.header.Get("WARC-Target-URI")] = r
		}
		c.Check(len(ids) == len(recs), "record IDs not unique: %v", ids)

		// Binary body record with rewritten headers.
		r, ok := byURL["https://one.invalid/bin"]
		c.Check(ok, "missing record for one.invalid/bin")
		if ok {
			c.Check(r.header.Get("WARC-Type") == "response", "bin record type %q", r.header.Get("WARC-Type"))
			c.Check(r.header.Get("WARC-Payload-Digest") == sha256Digest(binBody),
				"bin payload digest %q", r.header.Get("WARC-Payload-Digest"))
			c.Check(r.header.Get("WARC-Block-Digest") == sha256Digest(r.block),
				"bin block digest does not match actual block bytes")
			status, hdr, body := parseBlock(t, r.block)
			c.Check(status == "HTTP/1.1 200 OK", "bin status line %q", status)
			c.Check(bytes.Equal(body, binBody), "bin body corrupted: %x", body)
			mv := hdr.Values("X-Multi")
			c.Check(len(mv) == 2 && mv[0] == "1" && mv[1] == "2", "multi-value header flattened: %v", mv)
			c.Check(hdr.Get("Transfer-Encoding") == "", "transfer chunking not removed: %q", hdr.Get("Transfer-Encoding"))
			c.Check(hdr.Get("Content-Length") == strconv.Itoa(len(binBody)),
				"Content-Length %q not rewritten to actual body length", hdr.Get("Content-Length"))
			c.Check(hdr.Get("Content-Encoding") == "", "Content-Encoding kept despite Uncompressed: %q", hdr.Get("Content-Encoding"))
			c.Check(hdr.Get("Content-Type") == "application/octet-stream", "Content-Type lost: %q", hdr.Get("Content-Type"))
		}
		// Non-200 statuses are archived too.
		r, ok = byURL["https://one.invalid/plain"]
		c.Check(ok, "missing record for one.invalid/plain")
		if ok {
			status, _, body := parseBlock(t, r.block)
			c.Check(status == "HTTP/1.1 404 Not Found", "plain status line %q", status)
			c.Check(bytes.Equal(body, plainBody), "plain body %q", body)
		}
		r, ok = byURL["https://two.invalid/alpha"]
		c.Check(ok, "missing record for two.invalid/alpha")
		if ok {
			status, _, _ := parseBlock(t, r.block)
			c.Check(status == "HTTP/1.1 500 Internal Server Error", "alpha status line %q", status)
		}
		// Missing Request falls back to the Command URL.
		r, ok = byURL["https://two.invalid/noreq"]
		c.Check(ok, "missing record for two.invalid/noreq")
		if ok {
			_, _, body := parseBlock(t, r.block)
			c.Check(bytes.Equal(body, noreqBody), "noreq body %q", body)
		}
		_, headArchived := byURL["https://two.invalid/head"]
		c.Check(!headArchived, "HEAD response must not be archived")

		// The original responses must not be modified.
		res := kept["https://one.invalid/bin"]
		c.Check(res != nil, "bin response not kept")
		if res != nil {
			c.Check(res.Header.Get("Content-Encoding") == "gzip", "original Content-Encoding modified: %q", res.Header.Get("Content-Encoding"))
			c.Check(res.Header.Get("Transfer-Encoding") == "chunked", "original Transfer-Encoding modified: %q", res.Header.Get("Transfer-Encoding"))
			c.Check(res.Header.Get("Content-Length") == "999", "original Content-Length modified: %q", res.Header.Get("Content-Length"))
			c.Check(len(res.Header.Values("X-Multi")) == 2, "original X-Multi modified: %v", res.Header.Values("X-Multi"))
		}

		// Index cross-checks, including exact byte ranges.
		idxLines := parseIndex(t, idx.Bytes())
		checkIndexCrossRef(c, recs, idxLines, data.Bytes())
		for _, e := range idxLines {
			c.Check(e.RevisitOf == "", "first-capture index line has revisit_of %q", e.RevisitOf)
			c.Check(!strings.Contains(e.ID, "<") && !strings.Contains(e.ID, ">"), "index id has brackets: %q", e.ID)
		}

		// Spool files are cleaned up.
		c.Check(len(dirEntries(t, tmp)) == 0, "temp dir not cleaned: %v", dirEntries(t, tmp))

		// Default options: UUID URN IDs, system temp dir, wall clock.
		var d2, i2 bytes.Buffer
		var closed2 int32
		h2, err := NewWARCHandler(&d2, &i2, WARCOptions{})
		c.Check(err == nil, "NewWARCHandler with defaults: %v", err)
		if err != nil {
			return
		}
		srv2 := &warcServer{
			closed: &closed2,
			specs: map[string][]warcRespSpec{
				"https://def.invalid/x": {{status: 200, header: http.Header{"X-A": {"b"}}, body: []byte("x")}},
			},
		}
		q2 := startWARCFetcher(h2, srv2)
		if _, err := q2.SendStringGet("https://def.invalid/x"); err != nil {
			c.Check(false, "send default GET: %v", err)
			return
		}
		if err := q2.Close(); err != nil {
			c.Check(false, "default queue close: %v", err)
			return
		}
		c.Check(h2.Err() == nil, "default handler error: %v", h2.Err())
		recs2 := parseWARC(t, d2.Bytes())
		c.Check(len(recs2) == 1, "default run produced %d records", len(recs2))
		if len(recs2) == 1 {
			rawID := recs2[0].header.Get("WARC-Record-ID")
			c.Check(strings.HasPrefix(rawID, "<urn:uuid:") && strings.HasSuffix(rawID, ">") && len(rawID) == len("<urn:uuid:00000000-0000-0000-0000-000000000000>"),
				"default record ID %q is not a UUID URN", rawID)
			_, perr := time.Parse(time.RFC3339Nano, recs2[0].header.Get("WARC-Date"))
			c.Check(perr == nil, "default WARC-Date: %v", perr)
		}
	})
}

// gateBody blocks its first Read until release is closed, so that two host
// workers are both inside the handler before either finishes its body.
type gateBody struct {
	r       io.Reader
	once    sync.Once
	arrive  chan<- struct{}
	release <-chan struct{}
	closed  *int32
}

func (g *gateBody) Read(p []byte) (int, error) {
	g.once.Do(func() {
		g.arrive <- struct{}{}
		<-g.release
	})
	return g.r.Read(p)
}

func (g *gateBody) Close() error { atomic.AddInt32(g.closed, 1); return nil }

func TestWARCDedup(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		shared := []byte("shared payload body")
		changed := []byte("changed body")

		// Phase 1: sequential same-host captures, so the append order is
		// deterministic.
		var (
			data, idx bytes.Buffer
			closed    int32
			tick      int64
			idSeq     int64
		)
		tmp := t.TempDir()
		base := time.Date(2026, 5, 6, 7, 8, 9, 0, time.UTC)
		h, err := NewWARCHandler(&data, &idx, WARCOptions{
			TempDir: tmp,
			Now: func() time.Time {
				return base.Add(time.Duration(atomic.AddInt64(&tick, 1)) * time.Second)
			},
			NewID: func() string {
				return fmt.Sprintf("urn:dedup:%d", atomic.AddInt64(&idSeq, 1))
			},
		})
		c.Check(err == nil && h != nil, "NewWARCHandler: %v", err)
		if err != nil {
			return
		}

		srv := &warcServer{
			closed: &closed,
			specs: map[string][]warcRespSpec{
				// Same host, sequential: first capture of the digest.
				"https://dedup.invalid/a": {
					{status: 200, header: http.Header{"X-H": {"old"}}, body: shared},
					// Same URL, new body: must be a fresh response.
					{status: 200, header: http.Header{"X-H": {"old"}}, body: changed},
					// Same URL, original body again: revisit of the FIRST record.
					{status: 200, header: http.Header{"X-H": {"old"}}, body: shared},
				},
				// Same body, different URL and different headers: revisit
				// carrying the new headers.
				"https://dedup.invalid/b": {
					{status: 200, header: http.Header{"X-H": {"new"}, "X-Other": {"o1", "o2"}}, body: shared},
				},
			},
		}

		q := startWARCFetcher(h, srv)
		if _, err := q.SendStringGet("https://dedup.invalid/a", "https://dedup.invalid/b", "https://dedup.invalid/a", "https://dedup.invalid/a"); err != nil {
			c.Check(false, "send dedup GETs: %v", err)
			return
		}
		if err := q.Close(); err != nil {
			c.Check(false, "queue close: %v", err)
			return
		}
		c.Check(h.Err() == nil, "handler error: %v", h.Err())
		c.Check(atomic.LoadInt32(&closed) == 4, "bodies closed %d times, want 4", atomic.LoadInt32(&closed))

		recs := parseWARC(t, data.Bytes())
		c.Check(len(recs) == 4, "got %d records, want 4", len(recs))
		idxLines := parseIndex(t, idx.Bytes())
		checkIndexCrossRef(c, recs, idxLines, data.Bytes())
		if len(recs) < 4 {
			return
		}
		recA, recB, recA2, recA3 := recs[0], recs[1], recs[2], recs[3]
		idOf := func(r warcRec) string {
			return strings.TrimSuffix(strings.TrimPrefix(r.header.Get("WARC-Record-ID"), "<"), ">")
		}

		c.Check(recA.header.Get("WARC-Type") == "response", "first /a type %q", recA.header.Get("WARC-Type"))
		c.Check(recA.header.Get("WARC-Payload-Digest") == sha256Digest(shared), "first /a payload digest")

		// /b: same body, different URL and headers -> revisit with new headers.
		c.Check(recB.header.Get("WARC-Type") == "revisit", "/b type %q", recB.header.Get("WARC-Type"))
		c.Check(recB.header.Get("WARC-Profile") == "http://netpreserve.org/warc/1.1/revisit/identical-payload-digest",
			"/b profile %q", recB.header.Get("WARC-Profile"))
		c.Check(recB.header.Get("WARC-Truncated") == "length", "/b truncated %q", recB.header.Get("WARC-Truncated"))
		c.Check(recB.header.Get("WARC-Refers-To") == "<"+idOf(recA)+">",
			"/b refers-to %q, want first response %q", recB.header.Get("WARC-Refers-To"), idOf(recA))
		c.Check(recB.header.Get("WARC-Refers-To-Target-URI") == "https://dedup.invalid/a",
			"/b refers-to-target %q", recB.header.Get("WARC-Refers-To-Target-URI"))
		c.Check(recB.header.Get("WARC-Refers-To-Date") == recA.header.Get("WARC-Date"),
			"/b refers-to-date %q != %q", recB.header.Get("WARC-Refers-To-Date"), recA.header.Get("WARC-Date"))
		c.Check(recB.header.Get("WARC-Payload-Digest") == sha256Digest(shared), "/b payload digest")
		c.Check(recB.header.Get("WARC-Block-Digest") == sha256Digest(recB.block), "/b block digest independent")
		_, hdrB, bodyB := parseBlock(t, recB.block)
		c.Check(len(bodyB) == 0, "revisit /b must omit the body, got %d bytes", len(bodyB))
		c.Check(hdrB.Get("X-H") == "new", "revisit /b must keep this capture's headers, X-H=%q", hdrB.Get("X-H"))
		ov := hdrB.Values("X-Other")
		c.Check(len(ov) == 2 && ov[0] == "o1" && ov[1] == "o2", "revisit /b multi-value header: %v", ov)
		c.Check(hdrB.Get("Content-Length") == strconv.Itoa(len(shared)),
			"revisit /b Content-Length %q", hdrB.Get("Content-Length"))

		// /a again with a changed body: new response, new digest.
		c.Check(recA2.header.Get("WARC-Type") == "response", "changed /a type %q", recA2.header.Get("WARC-Type"))
		c.Check(recA2.header.Get("WARC-Payload-Digest") == sha256Digest(changed), "changed /a payload digest")
		c.Check(recA2.header.Get("WARC-Payload-Digest") != recA.header.Get("WARC-Payload-Digest"),
			"changed body must produce a new response digest")

		// /a again with the original body: revisit of the FIRST response, not
		// of another revisit and not of the changed response.
		c.Check(recA3.header.Get("WARC-Type") == "revisit", "third /a type %q", recA3.header.Get("WARC-Type"))
		c.Check(recA3.header.Get("WARC-Refers-To") == "<"+idOf(recA)+">",
			"third /a refers-to %q, want first response %q", recA3.header.Get("WARC-Refers-To"), idOf(recA))
		c.Check(recA3.header.Get("WARC-Refers-To") != "<"+idOf(recB)+">", "revisit must not point to another revisit")

		// Index revisit_of mirrors the record chain.
		if len(idxLines) >= 4 {
			c.Check(idxLines[0].RevisitOf == "", "index[0] revisit_of %q", idxLines[0].RevisitOf)
			c.Check(idxLines[1].RevisitOf == idOf(recA), "index[1] revisit_of %q", idxLines[1].RevisitOf)
			c.Check(idxLines[2].RevisitOf == "", "index[2] revisit_of %q", idxLines[2].RevisitOf)
			c.Check(idxLines[3].RevisitOf == idOf(recA), "index[3] revisit_of %q", idxLines[3].RevisitOf)
		}
		c.Check(len(dirEntries(t, tmp)) == 0, "temp dir not cleaned: %v", dirEntries(t, tmp))

		// Phase 2: concurrent cross-site duplicates. Exactly one response and
		// one revisit must come out, the revisit pointing at the response,
		// whichever site appended first.
		var (
			data2, idx2 bytes.Buffer
			closed2     int32
			idSeq2      int64
		)
		concBody := []byte("concurrent shared body")
		tmp2 := t.TempDir()
		h2, err := NewWARCHandler(&data2, &idx2, WARCOptions{
			TempDir: tmp2,
			NewID: func() string {
				return fmt.Sprintf("urn:conc:%d", atomic.AddInt64(&idSeq2, 1))
			},
		})
		c.Check(err == nil && h2 != nil, "NewWARCHandler: %v", err)
		if err != nil {
			return
		}
		arrive := make(chan struct{}, 2)
		release := make(chan struct{})
		gate := func() io.ReadCloser {
			return &gateBody{r: bytes.NewReader(concBody), arrive: arrive, release: release, closed: &closed2}
		}
		srv2 := &warcServer{
			closed: &closed2,
			specs: map[string][]warcRespSpec{
				"https://c1.invalid/x": {{status: 200, header: http.Header{"X-Site": {"c1"}}, makeBody: gate}},
				"https://c2.invalid/y": {{status: 200, header: http.Header{"X-Site": {"c2"}}, makeBody: gate}},
			},
		}
		q2 := startWARCFetcher(h2, srv2)
		if _, err := q2.SendStringGet("https://c1.invalid/x", "https://c2.invalid/y"); err != nil {
			c.Check(false, "send concurrent GETs: %v", err)
			return
		}
		// Wait until both site workers are preparing their bodies inside the
		// handler, then let both race to append.
		for i := 0; i < 2; i++ {
			select {
			case <-arrive:
			case <-time.After(10 * time.Second):
				c.Check(false, "timed out waiting for concurrent body preparation")
				return
			}
		}
		close(release)
		if err := q2.Close(); err != nil {
			c.Check(false, "concurrent queue close: %v", err)
			return
		}
		c.Check(h2.Err() == nil, "concurrent handler error: %v", h2.Err())
		c.Check(atomic.LoadInt32(&closed2) == 2, "concurrent bodies closed %d times, want 2", atomic.LoadInt32(&closed2))

		conc := parseWARC(t, data2.Bytes())
		c.Check(len(conc) == 2, "concurrent run produced %d records, want 2", len(conc))
		idx2Lines := parseIndex(t, idx2.Bytes())
		checkIndexCrossRef(c, conc, idx2Lines, data2.Bytes())
		concDigest := sha256Digest(concBody)
		var respRec, revRec *warcRec
		for i := range conc {
			c.Check(conc[i].header.Get("WARC-Payload-Digest") == concDigest,
				"concurrent record payload digest %q", conc[i].header.Get("WARC-Payload-Digest"))
			switch conc[i].header.Get("WARC-Type") {
			case "response":
				respRec = &conc[i]
			case "revisit":
				revRec = &conc[i]
			}
		}
		c.Check(respRec != nil && revRec != nil, "concurrent duplicates must yield one response and one revisit")
		if respRec != nil && revRec != nil {
			c.Check(respRec.header.Get("WARC-Target-URI") != revRec.header.Get("WARC-Target-URI"),
				"concurrent records must belong to different sites")
			c.Check(revRec.header.Get("WARC-Refers-To") == respRec.header.Get("WARC-Record-ID"),
				"concurrent revisit refers-to %q != response id %q",
				revRec.header.Get("WARC-Refers-To"), respRec.header.Get("WARC-Record-ID"))
			c.Check(revRec.header.Get("WARC-Refers-To-Target-URI") == respRec.header.Get("WARC-Target-URI"),
				"concurrent revisit target mismatch")
			c.Check(revRec.header.Get("WARC-Refers-To-Date") == respRec.header.Get("WARC-Date"),
				"concurrent revisit date mismatch")
			_, _, rbody := parseBlock(t, revRec.block)
			c.Check(len(rbody) == 0, "concurrent revisit must omit the body")
			_, _, sbody := parseBlock(t, respRec.block)
			c.Check(bytes.Equal(sbody, concBody), "concurrent response body %q", sbody)
			// Index attribution follows the actual append order.
			for i, r := range conc {
				want := ""
				if r.header.Get("WARC-Type") == "revisit" {
					want = strings.TrimSuffix(strings.TrimPrefix(r.header.Get("WARC-Refers-To"), "<"), ">")
				}
				if i < len(idx2Lines) {
					c.Check(idx2Lines[i].RevisitOf == want, "concurrent index[%d] revisit_of %q, want %q", i, idx2Lines[i].RevisitOf, want)
				}
			}
		}
		c.Check(len(dirEntries(t, tmp2)) == 0, "concurrent temp dir not cleaned: %v", dirEntries(t, tmp2))
	})
}

// failReader returns its data and then fails with err.
type failReader struct {
	data []byte
	err  error
}

func (f *failReader) Read(p []byte) (int, error) {
	if len(f.data) > 0 {
		n := copy(p, f.data)
		f.data = f.data[n:]
		return n, nil
	}
	return 0, f.err
}

// countReader counts Read calls.
type countReader struct {
	r io.Reader
	n *int32
}

func (cr *countReader) Read(p []byte) (int, error) {
	atomic.AddInt32(cr.n, 1)
	return cr.r.Read(p)
}

// errWriter always fails.
type errWriter struct {
	err   error
	calls *int32
}

func (w errWriter) Write(p []byte) (int, error) {
	atomic.AddInt32(w.calls, 1)
	return 0, w.err
}

// shortWriter reports a short write without an error.
type shortWriter struct {
	calls *int32
}

func (w shortWriter) Write(p []byte) (int, error) {
	atomic.AddInt32(w.calls, 1)
	if len(p) == 0 {
		return 0, nil
	}
	return len(p) - 1, nil
}

func TestWARCFailure(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Nil writers are rejected.
		var junk bytes.Buffer
		_, errD := NewWARCHandler(nil, &junk, WARCOptions{})
		c.Check(errD != nil, "nil data writer must be rejected")
		_, errI := NewWARCHandler(&junk, nil, WARCOptions{})
		c.Check(errI != nil, "nil index writer must be rejected")

		// 1. Body read failure: first error kept, nothing appended, temp
		// cleaned, later bodies not consumed.
		var closed1, reads1 int32
		errBoom := errors.New("boom read")
		tmp1 := t.TempDir()
		var d1, i1 bytes.Buffer
		h1, err := NewWARCHandler(&d1, &i1, WARCOptions{TempDir: tmp1})
		c.Check(err == nil, "NewWARCHandler: %v", err)
		if err != nil {
			return
		}
		srv1 := &warcServer{
			closed: &closed1,
			specs: map[string][]warcRespSpec{
				"https://f1.invalid/fail": {{
					status: 200,
					header: http.Header{"X-A": {"b"}},
					makeBody: func() io.ReadCloser {
						return &testBody{&failReader{data: []byte("partial"), err: errBoom}, &closed1}
					},
				}},
				"https://f1.invalid/after": {{
					status: 200,
					header: http.Header{"X-A": {"b"}},
					makeBody: func() io.ReadCloser {
						return &testBody{&countReader{strings.NewReader("after body"), &reads1}, &closed1}
					},
				}},
			},
		}
		q1 := startWARCFetcher(h1, srv1)
		if _, err := q1.SendStringGet("https://f1.invalid/fail", "https://f1.invalid/after"); err != nil {
			c.Check(false, "send: %v", err)
			return
		}
		if err := q1.Close(); err != nil {
			c.Check(false, "close: %v", err)
			return
		}
		c.Check(errors.Is(h1.Err(), errBoom), "Err() = %v, want read error", h1.Err())
		c.Check(d1.Len() == 0, "data output not empty after read failure: %d bytes", d1.Len())
		c.Check(i1.Len() == 0, "index output not empty after read failure: %d bytes", i1.Len())
		c.Check(atomic.LoadInt32(&reads1) == 0, "body consumed after failure: %d reads", atomic.LoadInt32(&reads1))
		c.Check(atomic.LoadInt32(&closed1) == 2, "bodies closed %d times, want 2", atomic.LoadInt32(&closed1))
		c.Check(len(dirEntries(t, tmp1)) == 0, "temp dir not cleaned after read failure")

		// 2. Data writer failure: first error kept, no index line, no further
		// writes or body consumption.
		var closed2, reads2, calls2 int32
		errData := errors.New("data write failed")
		tmp2 := t.TempDir()
		var i2 bytes.Buffer
		w2 := errWriter{err: errData, calls: &calls2}
		h2, err := NewWARCHandler(w2, &i2, WARCOptions{TempDir: tmp2})
		c.Check(err == nil, "NewWARCHandler: %v", err)
		if err != nil {
			return
		}
		srv2 := &warcServer{
			closed: &closed2,
			specs: map[string][]warcRespSpec{
				"https://f2.invalid/one": {{status: 200, header: http.Header{"X-A": {"b"}}, body: []byte("one")}},
				"https://f2.invalid/two": {{
					status: 200,
					header: http.Header{"X-A": {"b"}},
					makeBody: func() io.ReadCloser {
						return &testBody{&countReader{strings.NewReader("two"), &reads2}, &closed2}
					},
				}},
			},
		}
		q2 := startWARCFetcher(h2, srv2)
		if _, err := q2.SendStringGet("https://f2.invalid/one", "https://f2.invalid/two"); err != nil {
			c.Check(false, "send: %v", err)
			return
		}
		if err := q2.Close(); err != nil {
			c.Check(false, "close: %v", err)
			return
		}
		c.Check(errors.Is(h2.Err(), errData), "Err() = %v, want data writer error", h2.Err())
		c.Check(atomic.LoadInt32(&calls2) == 1, "data writer called %d times after failure, want 1", atomic.LoadInt32(&calls2))
		c.Check(i2.Len() == 0, "index line written for failed record")
		c.Check(atomic.LoadInt32(&reads2) == 0, "body consumed after data failure: %d reads", atomic.LoadInt32(&reads2))
		c.Check(len(dirEntries(t, tmp2)) == 0, "temp dir not cleaned after data failure")

		// 3. Index writer failure: the full record stays in the data output
		// but gets no success index line.
		var closed3, calls3 int32
		errIndex := errors.New("index write failed")
		tmp3 := t.TempDir()
		var d3 bytes.Buffer
		w3 := errWriter{err: errIndex, calls: &calls3}
		h3, err := NewWARCHandler(&d3, w3, WARCOptions{TempDir: tmp3})
		c.Check(err == nil, "NewWARCHandler: %v", err)
		if err != nil {
			return
		}
		srv3 := &warcServer{
			closed: &closed3,
			specs: map[string][]warcRespSpec{
				"https://f3.invalid/one": {{status: 200, header: http.Header{"X-A": {"b"}}, body: []byte("three")}},
			},
		}
		q3 := startWARCFetcher(h3, srv3)
		if _, err := q3.SendStringGet("https://f3.invalid/one"); err != nil {
			c.Check(false, "send: %v", err)
			return
		}
		if err := q3.Close(); err != nil {
			c.Check(false, "close: %v", err)
			return
		}
		c.Check(errors.Is(h3.Err(), errIndex), "Err() = %v, want index writer error", h3.Err())
		recs3 := parseWARC(t, d3.Bytes())
		c.Check(len(recs3) == 1, "data output holds %d records, want 1 complete record", len(recs3))
		c.Check(atomic.LoadInt32(&calls3) == 1, "index writer called %d times, want 1", atomic.LoadInt32(&calls3))
		c.Check(len(dirEntries(t, tmp3)) == 0, "temp dir not cleaned after index failure")

		// 4. Short write without error is reported as io.ErrShortWrite.
		var closed4, calls4 int32
		tmp4 := t.TempDir()
		var i4 bytes.Buffer
		w4 := shortWriter{calls: &calls4}
		h4, err := NewWARCHandler(w4, &i4, WARCOptions{TempDir: tmp4})
		c.Check(err == nil, "NewWARCHandler: %v", err)
		if err != nil {
			return
		}
		srv4 := &warcServer{
			closed: &closed4,
			specs: map[string][]warcRespSpec{
				"https://f4.invalid/one": {{status: 200, header: http.Header{"X-A": {"b"}}, body: []byte("four")}},
			},
		}
		q4 := startWARCFetcher(h4, srv4)
		if _, err := q4.SendStringGet("https://f4.invalid/one"); err != nil {
			c.Check(false, "send: %v", err)
			return
		}
		if err := q4.Close(); err != nil {
			c.Check(false, "close: %v", err)
			return
		}
		c.Check(errors.Is(h4.Err(), io.ErrShortWrite), "Err() = %v, want io.ErrShortWrite", h4.Err())
		c.Check(i4.Len() == 0, "index line written for short-written record")

		// 5. Fetch errors are recorded; later bodies are not consumed.
		var closed5, reads5 int32
		errFetch := errors.New("fetch exploded")
		tmp5 := t.TempDir()
		var d5, i5 bytes.Buffer
		h5, err := NewWARCHandler(&d5, &i5, WARCOptions{TempDir: tmp5})
		c.Check(err == nil, "NewWARCHandler: %v", err)
		if err != nil {
			return
		}
		srv5 := &warcServer{
			closed: &closed5,
			specs: map[string][]warcRespSpec{
				"https://f5.invalid/after": {{
					status: 200,
					header: http.Header{"X-A": {"b"}},
					makeBody: func() io.ReadCloser {
						return &testBody{&countReader{strings.NewReader("after"), &reads5}, &closed5}
					},
				}},
			},
		}
		f5 := New(h5)
		f5.DisablePoliteness = true
		f5.CrawlDelay = 0
		f5.HttpClient = testDoer(func(r *http.Request) (*http.Response, error) {
			if r.URL.Host == "f5.invalid" && r.URL.Path == "/err" {
				return nil, errFetch
			}
			return srv5.Do(r)
		})
		q5 := f5.Start()
		u5, _ := url.Parse("https://f5.invalid/err")
		if err := q5.Send(&Cmd{U: u5, M: "GET"}); err != nil {
			c.Check(false, "send err cmd: %v", err)
			return
		}
		if _, err := q5.SendStringGet("https://f5.invalid/after"); err != nil {
			c.Check(false, "send after: %v", err)
			return
		}
		if err := q5.Close(); err != nil {
			c.Check(false, "close: %v", err)
			return
		}
		c.Check(errors.Is(h5.Err(), errFetch), "Err() = %v, want fetch error", h5.Err())
		c.Check(d5.Len() == 0 && i5.Len() == 0, "outputs written after fetch error: %d/%d bytes", d5.Len(), i5.Len())
		c.Check(atomic.LoadInt32(&reads5) == 0, "body consumed after fetch error: %d reads", atomic.LoadInt32(&reads5))

		// 6. GET without a valid response body is an error.
		var closed6 int32
		tmp6 := t.TempDir()
		var d6, i6 bytes.Buffer
		h6, err := NewWARCHandler(&d6, &i6, WARCOptions{TempDir: tmp6})
		c.Check(err == nil, "NewWARCHandler: %v", err)
		if err != nil {
			return
		}
		f6 := New(h6)
		f6.DisablePoliteness = true
		f6.CrawlDelay = 0
		f6.HttpClient = testDoer(func(r *http.Request) (*http.Response, error) {
			return &http.Response{
				Status: "200 OK", StatusCode: 200, Proto: "HTTP/1.1",
				ProtoMajor: 1, ProtoMinor: 1,
				Header: http.Header{"X-A": {"b"}}, Request: r,
			}, nil
		})
		q6 := f6.Start()
		if _, err := q6.SendStringGet("https://f6.invalid/nobody"); err != nil {
			c.Check(false, "send: %v", err)
			return
		}
		if err := q6.Close(); err != nil {
			c.Check(false, "close: %v", err)
			return
		}
		c.Check(h6.Err() != nil, "GET without valid response must record an error")
		c.Check(d6.Len() == 0 && i6.Len() == 0, "outputs written for invalid response")
		_ = closed6
	})
}
