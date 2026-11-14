// Copyright 2026. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package downloader_test

import (
    "bytes"
    "crypto/sha256"
    "encoding/json"
    "fmt"
    "io"
    "net/http"
    "net/http/httptest"
    "os"
    "os/exec"
    "path/filepath"
    "strings"
    "sync"
    "testing"
    "time"

    "github.com/hu17889/go_spider/core/common/page"
    "github.com/hu17889/go_spider/core/common/request"
    "github.com/hu17889/go_spider/core/downloader"
    "github.com/hu17889/go_spider/core/pipeline"
    "github.com/hu17889/go_spider/core/spider"
    "github.com/hu17889/go_spider/task_support/taskcheck"
)

// rangeServer is a loopback HTTP server whose Range behaviour the tests can
// steer: gated blocks, truncated bodies, full 200 conflicts, weak validators
// and switched representations. It records every interval served so tests can
// prove exactly which bytes were re-requested across processes.
type rangeServer struct {
    srv *httptest.Server

    mu         sync.Mutex
    payload    []byte
    etag       string
    weakETag   bool
    ignoreRng  bool // ignore Range, always answer 200 with the full body
    badMeta    bool // answer 206 with a Content-Range that lies
    gzipEncode bool // advertise a non-identity encoding
    hits       map[int64]int
    ifRanges   []string
    acceptEnc  []string
    headers    http.Header
    cookies    []string
    fullGets   int
    probes     int

    failBlock int64 // block index that must fail, -1 disables
    fail200   bool  // failure mode: answer a full 200 instead of 206

    gated   map[int64]bool
    started chan int64
    release chan struct{}

    // failHold, when non-nil, blocks the failing response until the test
    // closes it. It makes cross-process ordering deterministic: the parent
    // waits for durable good blocks before allowing the failure to land.
    failHold chan struct{}

    handlerWG sync.WaitGroup
}

func newRangeServer(t *testing.T, payload []byte, etag string) *rangeServer {
    t.Helper()
    rs := &rangeServer{
        payload:   append([]byte(nil), payload...),
        etag:      etag,
        hits:      map[int64]int{},
        failBlock: -1,
        gated:     map[int64]bool{},
        started:   make(chan int64, 16),
        release:   make(chan struct{}),
        headers:   make(http.Header),
    }
    rs.srv = httptest.NewServer(http.HandlerFunc(rs.handle))
    t.Cleanup(rs.srv.Close)
    return rs
}

func (rs *rangeServer) URL() string { return rs.srv.URL + "/export.bin" }

func (rs *rangeServer) snapshot() (map[int64]int, int) {
    rs.mu.Lock()
    defer rs.mu.Unlock()
    out := make(map[int64]int, len(rs.hits))
    for k, v := range rs.hits {
        out[k] = v
    }
    return out, rs.fullGets
}

func (rs *rangeServer) setPayload(payload []byte, etag string) {
    rs.mu.Lock()
    rs.payload = append([]byte(nil), payload...)
    rs.etag = etag
    rs.mu.Unlock()
}

func (rs *rangeServer) setFail(index int64, as200 bool) {
    rs.mu.Lock()
    rs.failBlock = index
    rs.fail200 = as200
    rs.mu.Unlock()
}

func (rs *rangeServer) clearFail() { rs.setFail(-1, false) }

func (rs *rangeServer) setFailHold(ch chan struct{}) {
    rs.mu.Lock()
    rs.failHold = ch
    rs.mu.Unlock()
}

// waitForOtherHits blocks until every block other than excluded has been
// served at least once. It is used to order the deterministic failure.
func waitForOtherHits(rs *rangeServer, excluded int64, payloadLen int) {
    total := (int64(payloadLen) + blockSpan - 1) / blockSpan
    deadline := time.Now().Add(5 * time.Second)
    for time.Now().Before(deadline) {
        rs.mu.Lock()
        served := 0
        for i := int64(0); i < total; i++ {
            if i != excluded && rs.hits[i] > 0 {
                served++
            }
        }
        rs.mu.Unlock()
        if int64(served) == total-1 {
            return
        }
        time.Sleep(time.Millisecond)
    }
}

func (rs *rangeServer) setGated(blocks ...int64) {
    rs.mu.Lock()
    defer rs.mu.Unlock()
    rs.gated = map[int64]bool{}
    for _, b := range blocks {
        rs.gated[b] = true
    }
}

func (rs *rangeServer) waitQuiet() { rs.handlerWG.Wait() }

func (rs *rangeServer) handle(w http.ResponseWriter, r *http.Request) {
    rs.handlerWG.Add(1)
    defer rs.handlerWG.Done()

    rs.mu.Lock()
    payload := append([]byte(nil), rs.payload...)
    etag := rs.etag
    weak := rs.weakETag
    ignore := rs.ignoreRng
    badMeta := rs.badMeta
    gzip := rs.gzipEncode
    rs.mu.Unlock()

    if weak {
        w.Header().Set("ETag", "W/"+etag)
    } else {
        w.Header().Set("ETag", etag)
    }
    w.Header().Set("Accept-Ranges", "bytes")
    w.Header().Set("Content-Type", "application/octet-stream")
    if gzip {
        w.Header().Set("Content-Encoding", "gzip")
    }
    if c, err := r.Cookie("session"); err == nil {
        rs.mu.Lock()
        rs.cookies = append(rs.cookies, c.Value)
        rs.mu.Unlock()
    }
    rs.mu.Lock()
    rs.acceptEnc = append(rs.acceptEnc, r.Header.Get("Accept-Encoding"))
    rs.headers = r.Header.Clone()
    rs.mu.Unlock()

    rng := r.Header.Get("Range")
    if rng == "" || ignore {
        rs.mu.Lock()
        rs.fullGets++
        rs.mu.Unlock()
        w.WriteHeader(http.StatusOK)
        _, _ = w.Write(payload)
        return
    }

    start, end, ok := parseReqRange(rng)
    if !ok {
        http.Error(w, "bad range", http.StatusRequestedRangeNotSatisfiable)
        return
    }
    rs.mu.Lock()
    rs.probes++
    isBlock := r.Header.Get("If-Range") != ""
    if isBlock {
        rs.ifRanges = append(rs.ifRanges, r.Header.Get("If-Range"))
    }
    rs.mu.Unlock()

    if end >= int64(len(payload)) {
        end = int64(len(payload)) - 1
    }
    block := start / blockSpan
    if rs.isGated(block) {
        rs.started <- block
        <-rs.release
    }

    rs.mu.Lock()
    failNow := rs.failBlock == block
    as200 := rs.fail200
    hold := rs.failHold
    if isBlock {
        // Only If-Range guarded block GETs count as block hits; the one-byte
        // probe must not distort the per-block request accounting.
        rs.hits[block]++
    }
    rs.mu.Unlock()

    if failNow && hold != nil {
        // Wait until every other block has been fully served so the child is
        // guaranteed to have durable good blocks before this failure lands.
        waitForOtherHits(rs, block, len(payload))
        <-hold
    }

    if failNow && as200 {
        // Representation conflict: a block request answered with the whole 200.
        w.WriteHeader(http.StatusOK)
        _, _ = w.Write(payload)
        return
    }

    w.WriteHeader(http.StatusPartialContent)
    if badMeta && isBlock {
        // Lie only on block responses so the probe binds the real geometry.
        w.Header().Set("Content-Range", fmt.Sprintf("bytes %d-%d/%d", start, end, int64(len(payload))+99))
    } else {
        w.Header().Set("Content-Range", fmt.Sprintf("bytes %d-%d/%d", start, end, len(payload)))
    }

    if failNow {
        // Truncated block: correct interval header, short body, clean EOF.
        cut := end - 2
        if cut <= start {
            cut = start
        }
        _, _ = w.Write(payload[start:cut])
        return
    }
    _, _ = w.Write(payload[start : end+1])
}

func (rs *rangeServer) isGated(block int64) bool {
    rs.mu.Lock()
    defer rs.mu.Unlock()
    return rs.gated[block]
}

const blockSpan int64 = 7

// fileReceiptOnDisk mirrors the documented persistence schema so tests can
// assert the exact field names written by the downloader.
type fileReceiptOnDisk struct {
    Version      int    `json:"version"`
    URL          string `json:"url"`
    EffectiveURL string `json:"effective_url"`
    ETag         string `json:"etag"`
    Size         int64  `json:"size"`
    ChunkSize    int64  `json:"chunk_size"`
    Chunks       []struct {
        Index  int    `json:"index"`
        Sha256 string `json:"sha256"`
    } `json:"chunks"`
}

func parseReqRange(v string) (start, end int64, ok bool) {
    if !strings.HasPrefix(v, "bytes=") {
        return 0, 0, false
    }
    pair := strings.TrimPrefix(v, "bytes=")
    parts := strings.Split(pair, "-")
    if len(parts) != 2 {
        return 0, 0, false
    }
    if _, err := fmt.Sscanf(parts[0], "%d", &start); err != nil {
        return 0, 0, false
    }
    if _, err := fmt.Sscanf(parts[1], "%d", &end); err != nil {
        return 0, 0, false
    }
    return start, end, end >= start
}

func fileRequest(url, target string) *request.Request {
    return request.NewRequest(url, "file", "", "GET", "", nil, nil, nil, nil).
        SetFileTarget(target, blockSpan, 3)
}

func readReceipt(t *testing.T, target string) *fileReceiptOnDisk {
    t.Helper()
    data, err := os.ReadFile(target + ".ranges.json")
    if err != nil {
        t.Fatalf("receipt unreadable: %v", err)
    }
    var rc fileReceiptOnDisk
    if err := json.Unmarshal(data, &rc); err != nil {
        t.Fatalf("receipt is not valid JSON: %v", err)
    }
    return &rc
}

func waitSignal(t *testing.T, ch chan int64, want int) []int64 {
    t.Helper()
    got := make([]int64, 0, want)
    deadline := time.After(5 * time.Second)
    for len(got) < want {
        select {
        case v := <-ch:
            got = append(got, v)
        case <-deadline:
            t.Fatalf("timed out waiting for %d gated blocks, got %v", want, got)
        }
    }
    return got
}

func samplePayload(n int) []byte {
    b := make([]byte, n)
    for i := range b {
        // Binary content, including 0x00 and 0xff: bytes must survive raw.
        b[i] = byte((i*37 + 11) % 256)
    }
    return b
}

// ---------------------------------------------------------------------------
// TestFileParallel: protocol correctness, real overlap, failure semantics.
// ---------------------------------------------------------------------------

func TestFileParallel(t *testing.T) {
    taskcheck.Run(t, func(c *taskcheck.C) {
        payload := samplePayload(30) // 5 blocks of 7 bytes
        rs := newRangeServer(t, payload, `"v1"`)
        dir := t.TempDir()
        target := filepath.Join(dir, "export.bin")

        // 1. Happy path: exact ranges, raw binary bytes, clean publication.
        p := downloader.NewHttpDownloader().Download(fileRequest(rs.URL(), target))
        c.Check(p.IsSucc(), "first download failed: %s", p.Errormsg())
        got, err := os.ReadFile(target)
        c.Check(err == nil && bytes.Equal(got, payload), "published bytes differ from source (err=%v)", err)
        c.Check(p.GetFilePath() == target, "GetFilePath returned %q want %q", p.GetFilePath(), target)
        c.Check(p.GetBodyStr() == "", "file mode must leave BodyStr empty")
        c.Check(!fileExists(target+".part"), "staging file removed after publish")
        c.Check(!fileExists(target+".ranges.json"), "receipt removed after publish")

        hits, full := rs.snapshot()
        c.Check(len(hits) == 5 && full == 0, "expected five 206 blocks and no full GET, got hits=%v full=%d", hits, full)
        rs.mu.Lock()
        encOK := len(rs.acceptEnc) > 0
        for _, e := range rs.acceptEnc {
            encOK = encOK && e == "identity"
        }
        ifRangeOK := len(rs.ifRanges) == 5
        for _, v := range rs.ifRanges {
            ifRangeOK = ifRangeOK && v == `"v1"`
        }
        rs.mu.Unlock()
        c.Check(encOK, "every request must force Accept-Encoding: identity")
        c.Check(ifRangeOK, "every block request must carry If-Range with the probed strong ETag")

        // 2. Redirect: the final response URL is what gets bound.
        redir := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
            http.Redirect(w, r, rs.URL(), http.StatusFound)
        }))
        defer redir.Close()
        target2 := filepath.Join(dir, "redirected.bin")
        p2 := downloader.NewHttpDownloader().Download(fileRequest(redir.URL+"/go", target2))
        c.Check(p2.IsSucc(), "redirect download failed: %s", p2.Errormsg())
        got2, _ := os.ReadFile(target2)
        c.Check(bytes.Equal(got2, payload), "redirected bytes mismatch")

        // 3. Overlapping workers plus one truncated block: fail, keep old
        // target absent, join every worker, then resume to completion.
        rs.setGated(0, 1)
        rs.setFail(2, false)
        target3 := filepath.Join(dir, "parallel-fail.bin")
        done := make(chan *page.Page, 1)
        go func() { done <- downloader.NewHttpDownloader().Download(fileRequest(rs.URL(), target3)) }()
        active := waitSignal(t, rs.started, 2)
        c.Check(active[0] != active[1], "two distinct blocks must run concurrently, got %v", active)
        close(rs.release)
        var failPage *page.Page
        select {
        case failPage = <-done:
        case <-time.After(10 * time.Second):
            t.Fatal("Download did not return after block failure")
        }
        c.Check(!failPage.IsSucc() && failPage.Errormsg() != "", "truncated block must fail the Download")
        c.Check(failPage.GetFilePath() == "", "failed Download must not expose a file path")
        c.Check(!fileExists(target3), "destination must not be created on failure")
        c.Check(fileExists(target3+".ranges.json"), "completed blocks must remain recorded")
        rs.waitQuiet() // returns only once every in-flight handler has exited
        rc := readReceipt(t, target3)
        c.Check(rc.Version == 1 && rc.ETag == `"v1"` && rc.Size == 30 && rc.ChunkSize == blockSpan,
            "receipt metadata wrong: %+v", rc)
        c.Check(len(rc.Chunks) >= 1 && len(rc.Chunks) < 5, "only completed blocks may be recorded, got %d", len(rc.Chunks))

        // Second attempt reuses the validated blocks and finishes.
        rs.setGated()
        rs.clearFail()
        p3 := downloader.NewHttpDownloader().Download(fileRequest(rs.URL(), target3))
        c.Check(p3.IsSucc(), "resume after block failure failed: %s", p3.Errormsg())
        got3, _ := os.ReadFile(target3)
        c.Check(bytes.Equal(got3, payload), "resumed bytes mismatch")
        hits3, _ := rs.snapshot()
        for _, ch := range rc.Chunks {
            c.Check(hits3[int64(ch.Index)] == 1, "good block %d must not be re-downloaded, hit counts=%v", ch.Index, hits3)
        }
        c.Check(hits3[2] >= 2, "previously failed block 2 must be fetched again, hit counts=%v", hits3)

        // 4. A full 200 answering a block request is a conflict, never a block.
        target4 := filepath.Join(dir, "conflict.bin")
        rs.setFail(1, true)
        p4 := downloader.NewHttpDownloader().Download(fileRequest(rs.URL(), target4))
        c.Check(!p4.IsSucc(), "200 on a block fetch must fail the Download")
        c.Check(!fileExists(target4), "conflict must not publish a target")
        rs.clearFail()

        // 5. Caller objects are not mutated by internal Range headers.
        hdr := http.Header{}
        hdr.Set("X-Task", "export")
        req5 := request.NewRequest(rs.URL(), "file", "", "GET", "", hdr,
            []*http.Cookie{{Name: "session", Value: "abc"}}, nil, nil).
            SetFileTarget(filepath.Join(dir, "hdr.bin"), blockSpan, 2)
        p5 := downloader.NewHttpDownloader().Download(req5)
        c.Check(p5.IsSucc(), "headered download failed: %s", p5.Errormsg())
        c.Check(req5.GetHeader().Get("Range") == "" && req5.GetHeader().Get("If-Range") == "",
            "internal Range/If-Range leaked into caller Header: %v", req5.GetHeader())
        c.Check(req5.GetHeader().Get("X-Task") == "export", "caller header lost")
        rs.mu.Lock()
        c.Check(rs.headers.Get("X-Task") == "export", "caller header did not reach the server")
        c.Check(len(rs.cookies) > 0 && rs.cookies[len(rs.cookies)-1] == "abc", "caller cookie did not reach the server")
        rs.mu.Unlock()

        // 6. Caller-supplied Range/If-Range is rejected before any I/O.
        before, fullBefore := rs.snapshot()
        bad := http.Header{}
        bad.Set("Range", "bytes=0-3")
        req6 := request.NewRequest(rs.URL(), "file", "", "GET", "", bad, nil, nil, nil).
            SetFileTarget(filepath.Join(dir, "bad.bin"), blockSpan, 2)
        p6 := downloader.NewHttpDownloader().Download(req6)
        c.Check(!p6.IsSucc(), "caller Range must be rejected")
        after, fullAfter := rs.snapshot()
        c.Check(fmt.Sprint(after) == fmt.Sprint(before) && fullAfter == fullBefore,
            "rejected request must perform no I/O")

        // 7. Non-identity Content-Encoding always fails, raw bytes stay raw.
        rs.gzipEncode = true
        p7 := downloader.NewHttpDownloader().Download(fileRequest(rs.URL(), filepath.Join(dir, "gz.bin")))
        c.Check(!p7.IsSucc(), "gzip Content-Encoding must fail file capture")
        rs.gzipEncode = false

        // 8. Proxy, custom header and redirect callback stay effective.
        proxy := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
            if !r.URL.IsAbs() {
                http.Error(w, "proxy expects absolute URL", http.StatusBadRequest)
                return
            }
            upstream, err := http.NewRequest(r.Method, r.URL.String(), nil)
            if err != nil {
                http.Error(w, err.Error(), http.StatusBadGateway)
                return
            }
            upstream.Header = r.Header.Clone()
            resp, err := http.DefaultClient.Do(upstream)
            if err != nil {
                http.Error(w, err.Error(), http.StatusBadGateway)
                return
            }
            defer resp.Body.Close()
            for k, vs := range resp.Header {
                for _, v := range vs {
                    w.Header().Add(k, v)
                }
            }
            w.WriteHeader(resp.StatusCode)
            _, _ = io.Copy(w, resp.Body)
        }))
        defer proxy.Close()
        req8 := request.NewRequestWithProxy(rs.URL(), "file", "", "GET", "", nil, nil, proxy.URL, nil, nil).
            SetFileTarget(filepath.Join(dir, "via-proxy.bin"), blockSpan, 2)
        p8 := downloader.NewHttpDownloader().Download(req8)
        c.Check(p8.IsSucc(), "proxied download failed: %s", p8.Errormsg())
        got8, _ := os.ReadFile(filepath.Join(dir, "via-proxy.bin"))
        c.Check(bytes.Equal(got8, payload), "proxied bytes mismatch")
    })
}

// ---------------------------------------------------------------------------
// TestFileResume: cross-process recovery and local block verification.
// ---------------------------------------------------------------------------

const (
    childEnv   = "GO_SPIDER_FILE_CHILD"
    childURL   = "GO_SPIDER_FILE_URL"
    childDir   = "GO_SPIDER_FILE_DIR"
    childPhase = "GO_SPIDER_FILE_PHASE"
)

func TestFileResume(t *testing.T) {
    if phase := os.Getenv(childEnv); phase != "" {
        runResumeChild(phase)
        return
    }

    taskcheck.Run(t, func(c *taskcheck.C) {
        payload := samplePayload(30)
        rs := newRangeServer(t, payload, `"v1"`)
        rs.setFail(2, false) // block 2 keeps failing throughout the first process
        // Hold the failing response until the other four blocks have been
        // served, guaranteeing the exiting child leaves durable good blocks.
        hold := make(chan struct{})
        rs.setFailHold(hold)
        go func() {
            waitForOtherHits(rs, 2, len(payload))
            close(hold)
        }()
        dir := t.TempDir()
        target := filepath.Join(dir, "resume.bin")

        // Process one: fails, exits, leaves a receipt and partial staging.
        out1, err := runResumeProcess(t, "fail", rs.URL(), dir)
        c.Check(err == nil, "first child process should exit successfully after a failed Download, got %v: %s", err, out1)
        c.Check(!fileExists(target), "first process must not publish the target")
        c.Check(fileExists(target+".part") && fileExists(target+".ranges.json"), "first process must leave staging and receipt")
        rc1 := readReceipt(t, target)
        c.Check(len(rc1.Chunks) == 4, "first process must record the four blocks served before the failure, got %d", len(rc1.Chunks))
        hits1, _ := rs.snapshot()

        // Corrupt one locally completed block before the second process.
        goodIndex := int64(rc1.Chunks[0].Index)
        f, err := os.OpenFile(target+".part", os.O_RDWR, 0644)
        c.Check(err == nil, "reopen staging failed: %v", err)
        _, werr := f.WriteAt([]byte{0xde, 0xad, 0xbe, 0xef}, goodIndex*blockSpan)
        c.Check(werr == nil, "corrupting block %d failed: %v", goodIndex, werr)
        c.Check(f.Close() == nil, "closing staging failed")

        // Process two: independent binary, same target directory, fills the
        // gaps and re-fetches the corrupted block only.
        rs.setFailHold(nil)
        rs.clearFail()
        out2, err := runResumeProcess(t, "complete", rs.URL(), dir)
        c.Check(err == nil, "second child process should complete the file, got %v: %s", err, out2)
        got, err := os.ReadFile(target)
        c.Check(err == nil && bytes.Equal(got, payload), "reassembled file mismatch (err=%v)", err)
        c.Check(!fileExists(target+".part") && !fileExists(target+".ranges.json"), "staging and receipt cleaned after publish")

        hits2, _ := rs.snapshot()
        c.Check(hits2[goodIndex] == hits1[goodIndex]+1, "corrupted block %d must be re-fetched once: %v -> %v", goodIndex, hits1[goodIndex], hits2[goodIndex])
        kept := 0
        for idx := range hits1 {
            if idx == 2 || idx == goodIndex {
                continue
            }
            if hits2[idx] == hits1[idx] {
                kept++
            }
        }
        c.Check(kept >= 1, "at least one good block must be kept without re-download: before=%v after=%v", hits1, hits2)

        // A full-length .part with an empty receipt proves nothing: every
        // block must be fetched again.
        dir2 := t.TempDir()
        target2 := filepath.Join(dir2, "holes.bin")
        c.Check(os.WriteFile(target2+".part", make([]byte, len(payload)), 0644) == nil, "seed staging failed")
        empty := fileReceiptOnDisk{Version: 1, URL: rs.URL(), EffectiveURL: rs.URL(), ETag: `"v1"`, Size: int64(len(payload)), ChunkSize: blockSpan, Chunks: []struct {
            Index  int    `json:"index"`
            Sha256 string `json:"sha256"`
        }{}}
        data, _ := json.Marshal(empty)
        c.Check(os.WriteFile(target2+".ranges.json", data, 0644) == nil, "seed receipt failed")
        before, _ := rs.snapshot()
        p := downloader.NewHttpDownloader().Download(fileRequest(rs.URL(), target2))
        c.Check(p.IsSucc(), "empty-receipt download failed: %s", p.Errormsg())
        after, _ := rs.snapshot()
        refetchedAll := true
        for i := int64(0); i < 5; i++ {
            if after[i] != before[i]+1 {
                refetchedAll = false
            }
        }
        c.Check(refetchedAll, "all blocks must be fetched with an empty receipt: before=%v after=%v", before, after)

        // Corrupt schema, unknown version and foreign URL fail before HTTP.
        cases := []struct {
            name string
            body string
        }{
            {"schema", "{not json"},
            {"version", `{"version":99,"url":"` + rs.URL() + `"}`},
            {"foreign", `{"version":1,"url":"http://127.0.0.1:1/other","chunks":[]}`},
        }
        for _, tc := range cases {
            dir3 := t.TempDir()
            t3 := filepath.Join(dir3, "x.bin")
            c.Check(os.MkdirAll(filepath.Dir(t3), 0755) == nil, "mkdir failed")
            c.Check(os.WriteFile(t3+".ranges.json", []byte(tc.body), 0644) == nil, "write receipt failed")
            h0, f0 := rs.snapshot()
            pBad := downloader.NewHttpDownloader().Download(fileRequest(rs.URL(), t3))
            h1, f1 := rs.snapshot()
            c.Check(!pBad.IsSucc(), "%s receipt must fail the Download", tc.name)
            c.Check(fmt.Sprint(h0) == fmt.Sprint(h1) && f0 == f1, "%s receipt must be rejected before HTTP", tc.name)
        }
    })
}

func runResumeProcess(t *testing.T, phase, srvURL, dir string) ([]byte, error) {
    t.Helper()
    cmd := exec.Command(os.Args[0], "-test.run", "^TestFileResume$", "-test.v")
    env := append(os.Environ(),
        childEnv+"="+phase,
        childURL+"="+srvURL,
        childDir+"="+dir,
        childPhase+"="+phase,
    )
    cmd.Env = env
    return cmd.CombinedOutput()
}

func runResumeChild(phase string) {
    target := filepath.Join(os.Getenv(childDir), "resume.bin")
    req := request.NewRequest(os.Getenv(childURL), "file", "", "GET", "", nil, nil, nil, nil).
        SetFileTarget(target, blockSpan, 3)
    p := downloader.NewHttpDownloader().Download(req)
    switch phase {
    case "fail":
        if p.IsSucc() {
            fmt.Println("CHILD_UNEXPECTED_SUCCESS")
            os.Exit(2)
        }
        os.Exit(0)
    case "complete":
        want := samplePayload(30)
        got, err := os.ReadFile(target)
        if !p.IsSucc() || err != nil || !bytes.Equal(got, want) {
            fmt.Printf("CHILD_COMPLETE_FAILED succ=%v err=%v\n", p.IsSucc(), err)
            os.Exit(1)
        }
        os.Exit(0)
    default:
        os.Exit(3)
    }
}

// ---------------------------------------------------------------------------
// TestFilePublish: representation switches, fallbacks, publication safety and
// the real Spider PageProcesser/Pipeline path.
// ---------------------------------------------------------------------------

type fileResultProcessor struct {
    finished int
    path     string
    sha      string
}

func (pr *fileResultProcessor) Process(pg *page.Page) {
    pr.path = pg.GetFilePath()
    b, err := os.ReadFile(pr.path)
    if err != nil {
        pg.SetStatus(true, err.Error())
        return
    }
    sum := sha256.Sum256(b)
    pr.sha = fmt.Sprintf("%x", sum)
    pg.AddField("path", pr.path)
    pg.AddField("sha256", pr.sha)
}

func (pr *fileResultProcessor) Finish() { pr.finished++ }

func TestFilePublish(t *testing.T) {
    taskcheck.Run(t, func(c *taskcheck.C) {
        payload := samplePayload(30)
        rs := newRangeServer(t, payload, `"v1"`)
        dir := t.TempDir()

        // 1. Server switches version: recorded blocks belong to the old
        // representation and must be discarded, never stitched together.
        target := filepath.Join(dir, "versioned.bin")
        rs.setFail(2, false)
        p1 := downloader.NewHttpDownloader().Download(fileRequest(rs.URL(), target))
        c.Check(!p1.IsSucc(), "seed download should fail midway")
        rcOld := readReceipt(t, target)
        c.Check(rcOld.ETag == `"v1"`, "seed receipt etag wrong")
        payload2 := samplePayload(30)
        for i := range payload2 {
            payload2[i] ^= 0x5a
        }
        rs.setPayload(payload2, `"v2"`)
        rs.clearFail()
        p2 := downloader.NewHttpDownloader().Download(fileRequest(rs.URL(), target))
        c.Check(p2.IsSucc(), "version switch download failed: %s", p2.Errormsg())
        got, _ := os.ReadFile(target)
        c.Check(bytes.Equal(got, payload2), "published file must be exactly the new representation")
        rcNew := readReceiptOrNil(target)
        c.Check(rcNew == nil, "receipt must be gone after publication")

        // 2. Lying range metadata fails and preserves an existing target.
        marker := []byte("DO-NOT-TOUCH")
        keep := filepath.Join(dir, "keep.bin")
        c.Check(os.WriteFile(keep, marker, 0644) == nil, "seed existing target failed")
        rs.badMeta = true
        p3 := downloader.NewHttpDownloader().Download(fileRequest(rs.URL(), keep))
        c.Check(!p3.IsSucc(), "malformed Content-Range must fail")
        kept, err := os.ReadFile(keep)
        c.Check(err == nil && bytes.Equal(kept, marker), "existing target must stay byte-identical")
        rs.badMeta = false

        // 3. Sequential fallback when Range is unsupported: 200 probe body is
        // consumed directly, no receipt is written.
        plain := newRangeServer(t, payload, `"v1"`)
        plain.ignoreRng = true
        seqTarget := filepath.Join(dir, "sequential.bin")
        p4 := downloader.NewHttpDownloader().Download(fileRequest(plain.URL(), seqTarget))
        c.Check(p4.IsSucc(), "no-range fallback failed: %s", p4.Errormsg())
        got4, _ := os.ReadFile(seqTarget)
        c.Check(bytes.Equal(got4, payload), "sequential bytes mismatch")
        c.Check(!fileExists(seqTarget+".ranges.json") && !fileExists(seqTarget+".part"), "fallback leaves no resumable state")
        _, full4 := plain.snapshot()
        c.Check(full4 == 1, "exactly one full GET expected, got %d", full4)

        // 4. 206 with only a weak ETag triggers one fresh unconditional GET.
        weak := newRangeServer(t, payload, `"v1"`)
        weak.weakETag = true
        weakTarget := filepath.Join(dir, "weak.bin")
        p5 := downloader.NewHttpDownloader().Download(fileRequest(weak.URL(), weakTarget))
        c.Check(p5.IsSucc(), "weak-etag fallback failed: %s", p5.Errormsg())
        got5, _ := os.ReadFile(weakTarget)
        c.Check(bytes.Equal(got5, payload), "weak-etag bytes mismatch")
        c.Check(!fileExists(weakTarget+".ranges.json"), "weak-etag fallback must not claim resumability")
        _, full5 := weak.snapshot()
        c.Check(full5 == 1, "weak-etag fallback must issue exactly one full GET, got %d", full5)

        // 5. Empty 200 is a valid empty file.
        emptySrv := newRangeServer(t, []byte{}, `"empty"`)
        emptySrv.ignoreRng = true
        emptyTarget := filepath.Join(dir, "empty.bin")
        p6 := downloader.NewHttpDownloader().Download(fileRequest(emptySrv.URL(), emptyTarget))
        c.Check(p6.IsSucc(), "empty download failed: %s", p6.Errormsg())
        fi, err := os.Stat(emptyTarget)
        c.Check(err == nil && fi.Size() == 0, "empty file expected, err=%v", err)

        // 6. Any earlier failure leaves the existing destination untouched.
        rs.setFail(0, false)
        p7 := downloader.NewHttpDownloader().Download(fileRequest(rs.URL(), keep))
        c.Check(!p7.IsSucc(), "truncated block should fail again")
        kept7, _ := os.ReadFile(keep)
        c.Check(bytes.Equal(kept7, marker), "target changed despite failed Download")
        rs.clearFail()

        // 7. The real Spider path: PageProcesser and Pipeline read the
        // published file through the normal request scheduling flow.
        pr := &fileResultProcessor{}
        sp := spider.NewSpider(pr, "file-export").SetThreadnum(1).CloseStrace()
        spiderTarget := filepath.Join(dir, "spider.bin")
        req := request.NewRequest(rs.URL(), "file", "export", "GET", "", nil, nil, nil, nil).
            SetFileTarget(spiderTarget, blockSpan, 2)
        items := sp.GetByRequest(req)
        c.Check(items != nil, "pipeline produced no items")
        wantSum := fmt.Sprintf("%x", sha256.Sum256(payload2))
        pathVal, _ := items.GetItem("path")
        shaVal, _ := items.GetItem("sha256")
        c.Check(pathVal == spiderTarget && shaVal == wantSum, "pipeline saw wrong file result: path=%q sha=%q", pathVal, shaVal)
        c.Check(pr.finished == 1 && pr.path == spiderTarget, "processor must observe the published file once")
        spiderBytes, _ := os.ReadFile(spiderTarget)
        c.Check(bytes.Equal(spiderBytes, payload2), "spider-published bytes mismatch")

        // 8. Text collection still works alongside file capture.
        textSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
            w.Header().Set("Content-Type", "text/plain; charset=utf-8")
            _, _ = io.WriteString(w, "PLAIN-TEXT-OK")
        }))
        defer textSrv.Close()
        textReq := request.NewRequest(textSrv.URL, "text", "", "GET", "", nil, nil, nil, nil)
        p8 := downloader.NewHttpDownloader().Download(textReq)
        c.Check(p8.IsSucc() && p8.GetBodyStr() == "PLAIN-TEXT-OK", "text response type regressed: %s", p8.Errormsg())

        // 9. Invalid file configurations fail before I/O.
        h0, f0 := rs.snapshot()
        badCases := []*request.Request{
            request.NewRequest(rs.URL(), "file", "", "GET", "", nil, nil, nil, nil), // no SetFileTarget
            request.NewRequest(rs.URL(), "file", "", "POST", "x", nil, nil, nil, nil).
                SetFileTarget(filepath.Join(dir, "x1.bin"), blockSpan, 2),
            request.NewRequest(rs.URL(), "file", "", "GET", "", nil, nil, nil, nil).
                SetFileTarget(filepath.Join(dir, "x2.bin"), 0, 2),
            request.NewRequest(rs.URL(), "file", "", "GET", "", nil, nil, nil, nil).
                SetFileTarget(filepath.Join(dir, "x3.bin"), blockSpan, 0),
            request.NewRequest(rs.URL(), "file", "", "GET", "", nil, nil, nil, nil).
                SetFileTarget(filepath.Join(dir, "no-such-dir", "x4.bin"), blockSpan, 2),
        }
        for i, br := range badCases {
            pb := downloader.NewHttpDownloader().Download(br)
            c.Check(!pb.IsSucc(), "invalid configuration %d must fail", i)
        }
        h1, f1 := rs.snapshot()
        c.Check(fmt.Sprint(h0) == fmt.Sprint(h1) && f0 == f1, "invalid configurations must perform no I/O")

        // 10. A console pipeline still runs on the file page's PageItems.
        pr2 := &fileResultProcessor{}
        sp2 := spider.NewSpider(pr2, "file-console").SetThreadnum(1).CloseStrace().
            AddPipeline(pipeline.NewPipelineConsole())
        consoleTarget := filepath.Join(dir, "console.bin")
        sp2.AddRequest(request.NewRequest(rs.URL(), "file", "", "GET", "", nil, nil, nil, nil).
            SetFileTarget(consoleTarget, blockSpan, 2))
        sp2.Run()
        consoleBytes, err := os.ReadFile(consoleTarget)
        c.Check(err == nil && bytes.Equal(consoleBytes, payload2), "console-pipeline file mismatch: %v", err)
    })
}

func readReceiptOrNil(target string) *fileReceiptOnDisk {
    data, err := os.ReadFile(target + ".ranges.json")
    if err != nil {
        return nil
    }
    var rc fileReceiptOnDisk
    if err := json.Unmarshal(data, &rc); err != nil {
        return nil
    }
    return &rc
}

func fileExists(path string) bool {
    _, err := os.Stat(path)
    return err == nil
}
