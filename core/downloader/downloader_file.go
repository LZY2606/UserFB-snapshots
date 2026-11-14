package downloader

import (
    "crypto/sha256"
    "encoding/hex"
    "encoding/json"
    "fmt"
    "io"
    "net/http"
    "net/url"
    "os"
    "path/filepath"
    "regexp"
    "strings"
    "sync"

    "github.com/hu17889/go_spider/core/common/mlog"
    "github.com/hu17889/go_spider/core/common/page"
    "github.com/hu17889/go_spider/core/common/request"
)

// File capture keeps large responses on disk instead of loading them into a
// Page body. The protocol and the on-disk record format are fixed by
// task_support/file_contract.md:
//
//   staging  <path>.part        raw bytes, written at fixed block offsets
//   receipt  <path>.ranges.json durable, atomically replaced completion record
//
// The destination path itself is replaced atomically and only after every
// block has been downloaded, verified and persisted.
const (
    fileReceiptVersion = 1
    fileStagingSuffix  = ".part"
    fileReceiptSuffix  = ".ranges.json"
)

// fileReceipt is the persistent record of a resumable capture. Only blocks
// whose bytes have been fully written and verified appear in Chunks.
type fileReceipt struct {
    Version      int                `json:"version"`
    URL          string             `json:"url"`
    EffectiveURL string             `json:"effective_url"`
    ETag         string             `json:"etag"`
    Size         int64              `json:"size"`
    ChunkSize    int64              `json:"chunk_size"`
    Chunks       []fileReceiptChunk `json:"chunks"`
}

type fileReceiptChunk struct {
    Index  int    `json:"index"`
    Sha256 string `json:"sha256"`
}

// fileRepresentation binds one strong validator, the total length and the
// final address of a single source representation.
type fileRepresentation struct {
    etag         string
    size         int64
    effectiveURL string
}

// fileBlock is one fixed interval of the representation.
type fileBlock struct {
    index int64
    start int64
    end   int64 // inclusive
}

var contentRangeRegexp = regexp.MustCompile(`^bytes (\d+)-(\d+)/(\d+)$`)

// downloadFileTarget implements RespType "file". It never converts charsets,
// never decompresses and never places response bytes in Page.BodyStr.
func (this *HttpDownloader) downloadFileTarget(p *page.Page, req *request.Request) *page.Page {
    target, err := validateFileRequest(req)
    if err != nil {
        mlog.LogInst().LogError(err.Error())
        p.SetStatus(true, err.Error())
        return p
    }

    client, err := newFileClient(req)
    if err != nil {
        mlog.LogInst().LogError(err.Error())
        p.SetStatus(true, err.Error())
        return p
    }

    stagingPath := target + fileStagingSuffix
    receiptPath := target + fileReceiptSuffix

    // A corrupt or foreign receipt is rejected before any HTTP call and
    // before the staging files are touched.
    priorReceipt, receiptExists, err := loadFileReceipt(receiptPath, req.GetUrl())
    if err != nil {
        mlog.LogInst().LogError(err.Error())
        p.SetStatus(true, err.Error())
        return p
    }

    // Probe with a single byte Range. The caller's Header is cloned, so the
    // internal Range never leaks back into the request object.
    probeResp, err := doFileProbe(client, req)
    if err != nil {
        mlog.LogInst().LogError(err.Error())
        p.SetStatus(true, err.Error())
        return p
    }

    if probeResp.StatusCode == http.StatusOK {
        // A 200 probe is already the full sequential response; consume its
        // body directly into staging instead of issuing a second GET.
        if err = streamFullDownload(client, req, target, stagingPath, receiptPath, probeResp); err != nil {
            mlog.LogInst().LogError(err.Error())
            p.SetStatus(true, err.Error())
            return p
        }
        p.SetFilePath(target).SetStatus(false, "")
        return p
    }

    rep, err := parseResumableProbe(probeResp, req)
    probeResp.Body.Close()
    if err != nil {
        // A 206 without a strong validator (or any unusable 206 metadata)
        // falls back to one unconditional sequential GET; it never claims
        // resumability and leaves no receipt.
        if _, ok := err.(*fileFallbackError); ok {
            if ferr := streamFullDownload(client, req, target, stagingPath, receiptPath, nil); ferr != nil {
                mlog.LogInst().LogError(ferr.Error())
                p.SetStatus(true, ferr.Error())
                return p
            }
            p.SetFilePath(target).SetStatus(false, "")
            return p
        }
        mlog.LogInst().LogError(err.Error())
        p.SetStatus(true, err.Error())
        return p
    }

    completed, err := prepareResumableState(req, rep, priorReceipt, receiptExists, stagingPath, receiptPath)
    if err != nil {
        mlog.LogInst().LogError(err.Error())
        p.SetStatus(true, err.Error())
        return p
    }

    if err = fetchBlocks(client, req, rep, stagingPath, receiptPath, completed); err != nil {
        mlog.LogInst().LogError(err.Error())
        p.SetStatus(true, err.Error())
        return p
    }

    if err = publishFile(target, stagingPath, receiptPath, rep); err != nil {
        mlog.LogInst().LogError(err.Error())
        p.SetStatus(true, err.Error())
        return p
    }

    p.SetFilePath(target).SetStatus(false, "")
    return p
}

// validateFileRequest enforces the pre-I/O rules of the file contract.
func validateFileRequest(req *request.Request) (string, error) {
    if req.GetMethod() != "GET" {
        return "", fmt.Errorf("file capture requires a bodyless GET, got method %q", req.GetMethod())
    }
    if req.GetPostdata() != "" {
        return "", fmt.Errorf("file capture requires a bodyless GET, got POST data")
    }
    if h := req.GetHeader(); h != nil {
        if r := h.Get("Range"); r != "" {
            return "", fmt.Errorf("caller-supplied Range header is rejected in file mode: %q", r)
        }
        if r := h.Get("If-Range"); r != "" {
            return "", fmt.Errorf("caller-supplied If-Range header is rejected in file mode: %q", r)
        }
    }
    target := req.GetFilePath()
    if target == "" {
        return "", fmt.Errorf("file capture requires SetFileTarget before Download")
    }
    if req.GetChunkSize() <= 0 {
        return "", fmt.Errorf("file capture requires a positive chunk size, got %d", req.GetChunkSize())
    }
    if req.GetWorkers() <= 0 {
        return "", fmt.Errorf("file capture requires a positive worker count, got %d", req.GetWorkers())
    }
    parent := filepath.Dir(target)
    fi, err := os.Stat(parent)
    if err != nil {
        return "", fmt.Errorf("file target parent directory unavailable: %v", err)
    }
    if !fi.IsDir() {
        return "", fmt.Errorf("file target parent is not a directory: %s", parent)
    }
    return target, nil
}

// newFileClient preserves the request proxy and redirect callback.
func newFileClient(req *request.Request) (*http.Client, error) {
    client := &http.Client{CheckRedirect: req.GetRedirectFunc()}
    if proxyHost := req.GetProxyHost(); proxyHost != "" {
        proxy, err := url.Parse(proxyHost)
        if err != nil {
            return nil, fmt.Errorf("invalid proxy host %q: %v", proxyHost, err)
        }
        client.Transport = &http.Transport{Proxy: http.ProxyURL(proxy)}
    }
    return client, nil
}

// newFileRequest builds one internal bodyless GET. The caller header map is
// cloned so internal Range/If-Range/Accept-Encoding values never mutate the
// caller's objects.
func newFileRequest(req *request.Request) (*http.Request, error) {
    hr, err := http.NewRequest("GET", req.GetUrl(), nil)
    if err != nil {
        return nil, err
    }
    if h := req.GetHeader(); h != nil {
        hr.Header = h.Clone()
    } else {
        hr.Header = make(http.Header)
    }
    // identity disables transparent gzip handling: bytes must stay raw.
    hr.Header.Set("Accept-Encoding", "identity")
    for _, c := range req.GetCookies() {
        hr.AddCookie(c)
    }
    return hr, nil
}

func doFileProbe(client *http.Client, req *request.Request) (*http.Response, error) {
    hr, err := newFileRequest(req)
    if err != nil {
        return nil, err
    }
    hr.Header.Set("Range", "bytes=0-0")
    resp, err := client.Do(hr)
    if err != nil {
        return nil, fmt.Errorf("file probe failed: %v", err)
    }
    if ce := resp.Header.Get("Content-Encoding"); ce != "" && !strings.EqualFold(ce, "identity") {
        resp.Body.Close()
        return nil, fmt.Errorf("non-identity Content-Encoding %q is rejected", ce)
    }
    return resp, nil
}

// fileFallbackError marks a 206 that cannot support resumability and must be
// retried as one unconditional full GET.
type fileFallbackError struct{ msg string }

func (e *fileFallbackError) Error() string { return e.msg }

func strongETag(v string) bool {
    return v != "" && !strings.HasPrefix(v, "W/")
}

// parseResumableProbe validates a 206 probe. A weak/missing strong validator
// triggers the sequential fallback; every other malformed detail fails.
func parseResumableProbe(resp *http.Response, req *request.Request) (*fileRepresentation, error) {
    if resp.StatusCode != http.StatusPartialContent {
        return nil, fmt.Errorf("file probe returned unexpected status %d", resp.StatusCode)
    }
    etag := resp.Header.Get("ETag")
    if !strongETag(etag) {
        // Drain and discard: the fallback issues its own fresh GET.
        _, _ = io.Copy(io.Discard, resp.Body)
        return nil, &fileFallbackError{msg: fmt.Sprintf("probe 206 without strong ETag (%q), falling back to full GET", etag)}
    }
    start, end, total, err := parseContentRange(resp.Header.Get("Content-Range"))
    if err != nil || start != 0 || end != 0 || total < 1 {
        return nil, fmt.Errorf("malformed probe Content-Range %q: %v", resp.Header.Get("Content-Range"), err)
    }
    if resp.ContentLength >= 0 && resp.ContentLength != 1 {
        return nil, fmt.Errorf("probe Content-Length %d does not match one byte", resp.ContentLength)
    }
    body, err := io.ReadAll(resp.Body)
    if err != nil {
        return nil, fmt.Errorf("reading probe body failed: %v", err)
    }
    if len(body) != 1 {
        return nil, fmt.Errorf("probe body length %d does not match one byte", len(body))
    }
    effective := req.GetUrl()
    if resp.Request != nil && resp.Request.URL != nil {
        effective = resp.Request.URL.String()
    }
    return &fileRepresentation{etag: etag, size: total, effectiveURL: effective}, nil
}

func parseContentRange(v string) (start, end, total int64, err error) {
    m := contentRangeRegexp.FindStringSubmatch(strings.TrimSpace(v))
    if m == nil {
        return 0, 0, 0, fmt.Errorf("unparseable Content-Range %q", v)
    }
    if _, err = fmt.Sscanf(m[1], "%d", &start); err != nil {
        return
    }
    if _, err = fmt.Sscanf(m[2], "%d", &end); err != nil {
        return
    }
    if _, err = fmt.Sscanf(m[3], "%d", &total); err != nil {
        return
    }
    if end < start || total < 0 {
        err = fmt.Errorf("inconsistent Content-Range %q", v)
    }
    return
}

// streamFullDownload implements both sequential fallbacks: a probe that
// already answered 200 (resp supplied) or a fresh unconditional GET.
func streamFullDownload(client *http.Client, req *request.Request, target, stagingPath, receiptPath string, existing *http.Response) error {
    // Sequential downloads never combine with older resumable state.
    _ = os.Remove(stagingPath)
    _ = os.Remove(receiptPath)

    resp := existing
    if resp == nil {
        hr, err := newFileRequest(req)
        if err != nil {
            return err
        }
        resp, err = client.Do(hr)
        if err != nil {
            return fmt.Errorf("full GET failed: %v", err)
        }
        if ce := resp.Header.Get("Content-Encoding"); ce != "" && !strings.EqualFold(ce, "identity") {
            resp.Body.Close()
            return fmt.Errorf("non-identity Content-Encoding %q is rejected", ce)
        }
        if resp.StatusCode != http.StatusOK {
            resp.Body.Close()
            return fmt.Errorf("full GET returned unexpected status %d", resp.StatusCode)
        }
    }
    defer resp.Body.Close()

    f, err := os.OpenFile(stagingPath, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0644)
    if err != nil {
        return fmt.Errorf("opening staging file failed: %v", err)
    }
    n, copyErr := io.Copy(f, resp.Body)
    syncErr := f.Sync()
    closeErr := f.Close()
    if copyErr != nil {
        return fmt.Errorf("reading full response failed: %v", copyErr)
    }
    if syncErr != nil {
        return fmt.Errorf("syncing staging file failed: %v", syncErr)
    }
    if closeErr != nil {
        return fmt.Errorf("closing staging file failed: %v", closeErr)
    }
    if resp.ContentLength >= 0 && n != resp.ContentLength {
        return fmt.Errorf("full response truncated: got %d of %d bytes", n, resp.ContentLength)
    }
    return replaceTarget(target, stagingPath, receiptPath)
}

// prepareResumableState decides which recorded blocks survive and creates the
// fresh staging geometry when the source representation changed.
func prepareResumableState(req *request.Request, rep *fileRepresentation, prior *fileReceipt, priorExists bool, stagingPath, receiptPath string) (map[int64]string, error) {
    blockCount := blockCountOf(rep.size, req.GetChunkSize())
    fresh := func() (map[int64]string, error) {
        if err := os.Remove(stagingPath); err != nil && !os.IsNotExist(err) {
            return nil, fmt.Errorf("resetting staging file failed: %v", err)
        }
        if err := os.Remove(receiptPath); err != nil && !os.IsNotExist(err) {
            return nil, fmt.Errorf("resetting receipt failed: %v", err)
        }
        f, err := os.OpenFile(stagingPath, os.O_CREATE|os.O_TRUNC|os.O_RDWR, 0644)
        if err != nil {
            return nil, fmt.Errorf("creating staging file failed: %v", err)
        }
        if err = f.Close(); err != nil {
            return nil, err
        }
        rc := &fileReceipt{
            Version:      fileReceiptVersion,
            URL:          req.GetUrl(),
            EffectiveURL: rep.effectiveURL,
            ETag:         rep.etag,
            Size:         rep.size,
            ChunkSize:    req.GetChunkSize(),
            Chunks:       []fileReceiptChunk{},
        }
        if err = saveFileReceipt(receiptPath, rc); err != nil {
            return nil, err
        }
        return map[int64]string{}, nil
    }

    if !priorExists {
        return fresh()
    }
    same := prior.Version == fileReceiptVersion &&
        prior.URL == req.GetUrl() &&
        prior.ETag == rep.etag &&
        prior.EffectiveURL == rep.effectiveURL &&
        prior.Size == rep.size &&
        prior.ChunkSize == req.GetChunkSize()
    if !same {
        // The server changed versions (or geometry changed): old bytes belong
        // to another representation and must never be mixed in.
        return fresh()
    }

    completed := make(map[int64]string, len(prior.Chunks))
    f, err := os.OpenFile(stagingPath, os.O_RDONLY, 0)
    if err != nil {
        if os.IsNotExist(err) {
            // Receipt without staging proves nothing: start over.
            return fresh()
        }
        return nil, fmt.Errorf("opening staging file failed: %v", err)
    }
    defer f.Close()

    var dropped bool
    for _, ch := range prior.Chunks {
        b := blockOf(int64(ch.Index), rep.size, req.GetChunkSize())
        span := b.end - b.start + 1
        h := sha256.New()
        if _, err := io.CopyN(h, io.NewSectionReader(f, b.start, span), span); err != nil {
            dropped = true
            continue
        }
        if hex.EncodeToString(h.Sum(nil)) != strings.ToLower(ch.Sha256) {
            dropped = true
            continue
        }
        completed[b.index] = strings.ToLower(ch.Sha256)
    }
    if int64(len(completed)) > blockCount {
        return nil, fmt.Errorf("receipt holds more blocks than the representation geometry")
    }
    if dropped {
        // Persist the surviving subset before fetching anything new.
        rc := receiptFromState(req, rep, completed)
        if err = saveFileReceipt(receiptPath, rc); err != nil {
            return nil, err
        }
    }
    return completed, nil
}

// fetchBlocks downloads every missing fixed block with bounded concurrency.
// On failure it stops dispatching new blocks and joins all started workers
// before returning; validated blocks finished in the meantime stay recorded.
func fetchBlocks(client *http.Client, req *request.Request, rep *fileRepresentation, stagingPath, receiptPath string, completed map[int64]string) error {
    chunkSize := req.GetChunkSize()
    totalBlocks := blockCountOf(rep.size, chunkSize)

    f, err := os.OpenFile(stagingPath, os.O_RDWR, 0644)
    if err != nil {
        return fmt.Errorf("opening staging file failed: %v", err)
    }
    defer f.Close()

    var (
        wg       sync.WaitGroup
        mu       sync.Mutex
        firstErr error
        abortCh  = make(chan struct{})
        sem      = make(chan struct{}, req.GetWorkers())
    )

    fail := func(e error) {
        mu.Lock()
        if firstErr == nil {
            firstErr = e
            close(abortCh)
        }
        mu.Unlock()
    }

dispatch:
    for i := int64(0); i < totalBlocks; i++ {
        mu.Lock()
        _, done := completed[i]
        mu.Unlock()
        if done {
            continue
        }
        // Give abort priority: once a worker failed, no new block may be
        // dispatched even if a semaphore token is available at the same time.
        select {
        case <-abortCh:
            break dispatch
        default:
        }
        select {
        case <-abortCh:
            break dispatch
        case sem <- struct{}{}:
        }
        wg.Add(1)
        go func(idx int64) {
            defer wg.Done()
            defer func() { <-sem }()

            b := blockOf(idx, rep.size, chunkSize)
            data, err := fetchOneBlock(client, req, rep, b)
            if err != nil {
                fail(err)
                return
            }
            sum := sha256.Sum256(data)
            sumHex := hex.EncodeToString(sum[:])

            // Durable bytes first, completion record second.
            if _, err = f.WriteAt(data, b.start); err != nil {
                fail(fmt.Errorf("writing block %d failed: %v", idx, err))
                return
            }
            if err = f.Sync(); err != nil {
                fail(fmt.Errorf("syncing block %d failed: %v", idx, err))
                return
            }
            mu.Lock()
            // The block was fully validated and fsynced before the lock, so
            // record it even if another worker has already aborted: it is
            // honest durable work the next process can reuse.
            if _, ok := completed[idx]; !ok {
                completed[idx] = sumHex
                if recErr := saveFileReceipt(receiptPath, receiptFromState(req, rep, completed)); recErr != nil && firstErr == nil {
                    firstErr = recErr
                    close(abortCh)
                }
            }
            mu.Unlock()
        }(i)
    }
    wg.Wait()
    if firstErr != nil {
        return firstErr
    }
    if int64(len(completed)) != totalBlocks {
        return fmt.Errorf("only %d of %d blocks completed", len(completed), totalBlocks)
    }
    return nil
}

// fetchOneBlock performs one If-Range guarded single-block GET and enforces
// every protocol invariant. A 200 here is always a representation conflict.
func fetchOneBlock(client *http.Client, req *request.Request, rep *fileRepresentation, b fileBlock) ([]byte, error) {
    hr, err := newFileRequest(req)
    if err != nil {
        return nil, err
    }
    hr.Header.Set("Range", fmt.Sprintf("bytes=%d-%d", b.start, b.end))
    hr.Header.Set("If-Range", rep.etag)

    resp, err := client.Do(hr)
    if err != nil {
        return nil, fmt.Errorf("block %d request failed: %v", b.index, err)
    }
    defer resp.Body.Close()

    if ce := resp.Header.Get("Content-Encoding"); ce != "" && !strings.EqualFold(ce, "identity") {
        return nil, fmt.Errorf("block %d rejected non-identity Content-Encoding %q", b.index, ce)
    }
    if resp.StatusCode == http.StatusOK {
        return nil, fmt.Errorf("block %d got full 200 response instead of its interval", b.index)
    }
    if resp.StatusCode != http.StatusPartialContent {
        return nil, fmt.Errorf("block %d got unexpected status %d", b.index, resp.StatusCode)
    }
    if etag := resp.Header.Get("ETag"); etag != rep.etag {
        return nil, fmt.Errorf("block %d ETag changed: got %q want %q", b.index, etag, rep.etag)
    }
    effective := req.GetUrl()
    if resp.Request != nil && resp.Request.URL != nil {
        effective = resp.Request.URL.String()
    }
    if effective != rep.effectiveURL {
        return nil, fmt.Errorf("block %d effective URL changed: got %q want %q", b.index, effective, rep.effectiveURL)
    }
    start, end, total, err := parseContentRange(resp.Header.Get("Content-Range"))
    if err != nil {
        return nil, fmt.Errorf("block %d: %v", b.index, err)
    }
    if start != b.start || end != b.end || total != rep.size {
        return nil, fmt.Errorf("block %d Content-Range bytes %d-%d/%d does not match assigned %d-%d/%d",
            b.index, start, end, total, b.start, b.end, rep.size)
    }
    span := b.end - b.start + 1
    if resp.ContentLength >= 0 && resp.ContentLength != span {
        return nil, fmt.Errorf("block %d Content-Length %d does not match span %d", b.index, resp.ContentLength, span)
    }
    data, err := io.ReadAll(resp.Body)
    if err != nil {
        return nil, fmt.Errorf("block %d body read failed: %v", b.index, err)
    }
    if int64(len(data)) != span {
        return nil, fmt.Errorf("block %d truncated: got %d of %d bytes", b.index, int64(len(data)), span)
    }
    return data, nil
}

// publishFile performs the final whole-file verification and atomically
// moves staging into the destination. The existing target stays untouched on
// every earlier failure because this is the only place it is replaced.
func publishFile(target, stagingPath, receiptPath string, rep *fileRepresentation) error {
    fi, err := os.Stat(stagingPath)
    if err != nil {
        return fmt.Errorf("staging file unavailable: %v", err)
    }
    if fi.Size() != rep.size {
        return fmt.Errorf("staging size %d does not match representation size %d", fi.Size(), rep.size)
    }
    if err = verifyAllBlocks(stagingPath, receiptPath); err != nil {
        return err
    }
    return replaceTarget(target, stagingPath, receiptPath)
}

// verifyAllBlocks rehashes every recorded interval straight from staging so a
// hole or a stray byte sequence can never reach the destination.
func verifyAllBlocks(stagingPath, receiptPath string) error {
    rc, exists, err := loadFileReceipt(receiptPath, "")
    if err != nil {
        return err
    }
    if !exists {
        return fmt.Errorf("receipt %s missing before publication", receiptPath)
    }
    f, err := os.Open(stagingPath)
    if err != nil {
        return err
    }
    defer f.Close()
    for _, ch := range rc.Chunks {
        b := blockOf(int64(ch.Index), rc.Size, rc.ChunkSize)
        span := b.end - b.start + 1
        h := sha256.New()
        if _, err := io.CopyN(h, io.NewSectionReader(f, b.start, span), span); err != nil {
            return fmt.Errorf("final verification of block %d failed: %v", ch.Index, err)
        }
        if hex.EncodeToString(h.Sum(nil)) != strings.ToLower(ch.Sha256) {
            return fmt.Errorf("final verification of block %d hash mismatch", ch.Index)
        }
    }
    if int64(len(rc.Chunks)) != blockCountOf(rc.Size, rc.ChunkSize) {
        return fmt.Errorf("receipt lists %d blocks, geometry requires %d", len(rc.Chunks), blockCountOf(rc.Size, rc.ChunkSize))
    }
    return nil
}

// replaceTarget renames staging over the destination and then removes the
// receipt. Both live in the same directory, so the rename is atomic.
func replaceTarget(target, stagingPath, receiptPath string) error {
    if err := os.Rename(stagingPath, target); err != nil {
        return fmt.Errorf("publishing target failed: %v", err)
    }
    if err := os.Remove(receiptPath); err != nil && !os.IsNotExist(err) {
        return fmt.Errorf("removing receipt after publication failed: %v", err)
    }
    return nil
}

func blockCountOf(size, chunkSize int64) int64 {
    if size <= 0 {
        return 0
    }
    return (size + chunkSize - 1) / chunkSize
}

func blockOf(index, size, chunkSize int64) fileBlock {
    start := index * chunkSize
    end := start + chunkSize - 1
    if end >= size {
        end = size - 1
    }
    return fileBlock{index: index, start: start, end: end}
}

func receiptFromState(req *request.Request, rep *fileRepresentation, completed map[int64]string) *fileReceipt {
    chunks := make([]fileReceiptChunk, 0, len(completed))
    for i := int64(0); i < blockCountOf(rep.size, req.GetChunkSize()); i++ {
        if sum, ok := completed[i]; ok {
            chunks = append(chunks, fileReceiptChunk{Index: int(i), Sha256: sum})
        }
    }
    return &fileReceipt{
        Version:      fileReceiptVersion,
        URL:          req.GetUrl(),
        EffectiveURL: rep.effectiveURL,
        ETag:         rep.etag,
        Size:         rep.size,
        ChunkSize:    req.GetChunkSize(),
        Chunks:       chunks,
    }
}

// loadFileReceipt reads and structurally validates the receipt. When wantURL
// is nonempty, a receipt naming another original URL (like an unknown version
// or corrupt schema) is a hard error before HTTP and before file changes.
func loadFileReceipt(receiptPath, wantURL string) (*fileReceipt, bool, error) {
    data, err := os.ReadFile(receiptPath)
    if err != nil {
        if os.IsNotExist(err) {
            return nil, false, nil
        }
        return nil, false, fmt.Errorf("reading receipt failed: %v", err)
    }
    var rc fileReceipt
    if err = json.Unmarshal(data, &rc); err != nil {
        return nil, true, fmt.Errorf("corrupt receipt %s: %v", receiptPath, err)
    }
    if rc.Version != fileReceiptVersion {
        return nil, true, fmt.Errorf("unknown receipt version %d in %s", rc.Version, receiptPath)
    }
    if wantURL != "" && rc.URL != wantURL {
        return nil, true, fmt.Errorf("receipt %s belongs to another URL %q", receiptPath, rc.URL)
    }
    if rc.ChunkSize <= 0 || rc.Size < 0 {
        return nil, true, fmt.Errorf("receipt %s has invalid geometry", receiptPath)
    }
    seen := map[int64]bool{}
    geometry := blockCountOf(rc.Size, rc.ChunkSize)
    for _, ch := range rc.Chunks {
        idx := int64(ch.Index)
        if idx < 0 || idx >= geometry || seen[idx] {
            return nil, true, fmt.Errorf("receipt %s has invalid or duplicated block %d", receiptPath, ch.Index)
        }
        seen[idx] = true
        if len(ch.Sha256) != sha256.Size*2 {
            return nil, true, fmt.Errorf("receipt %s block %d has malformed sha256", receiptPath, ch.Index)
        }
    }
    return &rc, true, nil
}

// saveFileReceipt persists the receipt atomically: temp file in the
// same directory, sync, rename. A crashed process therefore sees either the
// previous complete JSON or the new complete JSON. Callers updating the
// shared completion map must hold its mutex.
func saveFileReceipt(receiptPath string, rc *fileReceipt) error {
    data, err := json.MarshalIndent(rc, "", "  ")
    if err != nil {
        return err
    }
    dir := filepath.Dir(receiptPath)
    tmp, err := os.CreateTemp(dir, filepath.Base(receiptPath)+".*.tmp")
    if err != nil {
        return fmt.Errorf("creating receipt temp file failed: %v", err)
    }
    tmpName := tmp.Name()
    if _, err = tmp.Write(data); err != nil {
        tmp.Close()
        os.Remove(tmpName)
        return fmt.Errorf("writing receipt failed: %v", err)
    }
    if err = tmp.Sync(); err != nil {
        tmp.Close()
        os.Remove(tmpName)
        return err
    }
    if err = tmp.Close(); err != nil {
        os.Remove(tmpName)
        return err
    }
    if err = os.Rename(tmpName, receiptPath); err != nil {
        os.Remove(tmpName)
        return fmt.Errorf("replacing receipt failed: %v", err)
    }
    return nil
}
