// Package spider: persistent crawl frontier used by Spider.RunCheckpoint.
//
// A checkpoint directory is owned by a single crawl process. It holds one
// JSON snapshot that records every request the crawl has already admitted:
// the FIFO queue waiting to be downloaded, the requests currently in flight
// and the set of completed urls. Identity is the raw url string, so a url is
// admitted exactly once per crawl and keeps the parameters of its first
// admission.
package spider

import (
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"sync"

	"github.com/hu17889/go_spider/core/common/request"
)

const (
	// checkpointFileName is the single snapshot file inside a checkpoint dir.
	checkpointFileName = "checkpoint.json"

	// snapshotVersion is bumped whenever the on-disk format changes.
	snapshotVersion = 1
)

// snapshotRequest is the persisted form of request.Request.
// Function fields (checkRedirect) and the opaque Meta interface are never
// persisted: requests carrying them are rejected before admission.
type snapshotRequest struct {
	Url       string         `json:"url"`
	RespType  string         `json:"resp_type"`
	Method    string         `json:"method"`
	Postdata  string         `json:"postdata"`
	Urltag    string         `json:"urltag"`
	Header    http.Header    `json:"header,omitempty"`
	Cookies   []*http.Cookie `json:"cookies,omitempty"`
	ProxyHost string         `json:"proxy_host,omitempty"`
}

// snapshot is the full durable state of one crawl.
type snapshot struct {
	Version int `json:"version"`

	// Finished is set once the frontier drained and the Finish callback was
	// due. A resumed snapshot with Finished=true is terminal: Finish is not
	// called a second time.
	Finished bool              `json:"finished"`
	Queued   []snapshotRequest `json:"queued"`
	Inflight []snapshotRequest `json:"inflight"`
	// Done holds the completed url strings.
	Done []string `json:"done"`
}

func toSnapshotRequest(req *request.Request) snapshotRequest {
	return snapshotRequest{
		Url:       req.Url,
		RespType:  req.RespType,
		Method:    req.Method,
		Postdata:  req.Postdata,
		Urltag:    req.Urltag,
		Header:    req.Header,
		Cookies:   req.Cookies,
		ProxyHost: req.ProxyHost,
	}
}

func (sr snapshotRequest) toRequest() (*request.Request, error) {
	if sr.Url == "" {
		return nil, errors.New("checkpoint: snapshot contains a request without url")
	}
	return request.NewRequestWithProxy(
		sr.Url, sr.RespType, sr.Urltag, sr.Method, sr.Postdata,
		sr.Header, sr.Cookies, sr.ProxyHost, nil, nil,
	), nil
}

// validatePersistable rejects requests whose state cannot be represented on
// disk. The error is returned before the request enters any persistent state
// and before its download starts, so no field is dropped silently.
func validatePersistable(req *request.Request) error {
	if req == nil {
		return errors.New("checkpoint: cannot persist a nil request")
	}
	if req.GetUrl() == "" {
		return errors.New("checkpoint: cannot persist a request with empty url")
	}
	if req.GetRedirectFunc() != nil {
		return fmt.Errorf("checkpoint: request %q carries a custom redirect callback, which is not supported by RunCheckpoint", req.GetUrl())
	}
	if req.GetMeta() != nil {
		return fmt.Errorf("checkpoint: request %q carries non-nil Meta, which is not supported by RunCheckpoint", req.GetUrl())
	}
	return nil
}

// checkpointStore is the durable crawl frontier. All mutations are written to
// the snapshot file before they are visible as completed in memory.
type checkpointStore struct {
	mu sync.Mutex

	dir  string
	path string

	// queued keeps FIFO order: the first element is dispatched next.
	queued []*request.Request

	// inflight keeps claim order: the first element was claimed first.
	inflight []*request.Request

	// done is the set of durably completed urls.
	done map[string]struct{}

	// seen is the identity set: queued + inflight + done, keyed by raw url.
	seen map[string]struct{}

	// finished is true once the frontier drained in an earlier process.
	finished bool
}

// loadCheckpoint opens the checkpoint directory. A missing snapshot file is
// treated as a brand-new crawl; truncated files, unknown format versions and
// incomplete requests are hard errors.
func loadCheckpoint(dir string) (*checkpointStore, bool, error) {
	store := &checkpointStore{
		dir:  dir,
		path: filepath.Join(dir, checkpointFileName),
		done: make(map[string]struct{}),
		seen: make(map[string]struct{}),
	}

	data, err := os.ReadFile(store.path)
	if os.IsNotExist(err) {
		return store, false, nil
	}
	if err != nil {
		return nil, false, fmt.Errorf("checkpoint: cannot read snapshot %s: %w", store.path, err)
	}

	var snap snapshot
	if err := json.Unmarshal(data, &snap); err != nil {
		return nil, false, fmt.Errorf("checkpoint: snapshot %s is truncated or invalid: %w", store.path, err)
	}
	if snap.Version != snapshotVersion {
		return nil, false, fmt.Errorf("checkpoint: snapshot %s has unsupported version %d, want %d", store.path, snap.Version, snapshotVersion)
	}

	for _, sr := range snap.Queued {
		req, err := sr.toRequest()
		if err != nil {
			return nil, false, fmt.Errorf("checkpoint: queued entry invalid: %w", err)
		}
		store.queued = append(store.queued, req)
		store.seen[req.Url] = struct{}{}
	}
	for _, sr := range snap.Inflight {
		req, err := sr.toRequest()
		if err != nil {
			return nil, false, fmt.Errorf("checkpoint: inflight entry invalid: %w", err)
		}
		store.inflight = append(store.inflight, req)
		store.seen[req.Url] = struct{}{}
	}
	for _, u := range snap.Done {
		if u == "" {
			return nil, false, errors.New("checkpoint: snapshot contains a completed entry without url")
		}
		store.done[u] = struct{}{}
		store.seen[u] = struct{}{}
	}
	store.finished = snap.Finished

	return store, true, nil
}

// bootstrap re-queues requests that were in flight when the previous process
// stopped (in claim order, ahead of everything else) and then admits the
// seeds added through AddRequest/AddUrl. It always creates the snapshot file,
// even when the crawl starts with an empty frontier.
func (s *checkpointStore) bootstrap(seeds []*request.Request) error {
	s.mu.Lock()
	defer s.mu.Unlock()

	for _, req := range seeds {
		if err := validatePersistable(req); err != nil {
			return err
		}
	}

	nextQueued := make([]*request.Request, 0, len(s.inflight)+len(s.queued)+len(seeds))
	// In-flight items go back to the front, preserving their original claim
	// order, so recovery dispatches them before anything that was still queued.
	nextQueued = append(nextQueued, s.inflight...)
	nextQueued = append(nextQueued, s.queued...)

	nextSeen := make(map[string]struct{}, len(s.seen)+len(seeds))
	for k := range s.seen {
		nextSeen[k] = struct{}{}
	}
	for _, req := range seeds {
		if _, ok := nextSeen[req.Url]; ok {
			// Identity already known: the first admission's parameters win.
			continue
		}
		nextSeen[req.Url] = struct{}{}
		nextQueued = append(nextQueued, req)
	}

	if err := s.persist(nextQueued, nil, s.done, nextSeen, s.finished); err != nil {
		return err
	}
	s.queued = nextQueued
	s.inflight = nil
	s.seen = nextSeen
	return nil
}

// claim moves the head of the queue into the in-flight list and durably
// records that transition before returning. The downloader is only called
// after this write succeeds.
func (s *checkpointStore) claim() (*request.Request, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	if len(s.queued) == 0 {
		return nil, nil
	}

	req := s.queued[0]
	nextQueued := append([]*request.Request{}, s.queued[1:]...)
	nextInflight := append(append([]*request.Request{}, s.inflight...), req)

	if err := s.persist(nextQueued, nextInflight, s.done, s.seen, s.finished); err != nil {
		return nil, err
	}
	s.queued = nextQueued
	s.inflight = nextInflight
	return req, nil
}

// commit marks the parent page completed and admits its children as one
// durable transaction: there is no persisted state in which the parent is
// done while its discovered children are missing. Duplicate children are
// dropped by url identity. On write failure neither the disk snapshot nor the
// in-memory frontier is advanced.
func (s *checkpointStore) commit(parent *request.Request, children []*request.Request) error {
	s.mu.Lock()
	defer s.mu.Unlock()

	for _, child := range children {
		if err := validatePersistable(child); err != nil {
			return err
		}
	}

	idx := -1
	for i, req := range s.inflight {
		if req.Url == parent.Url {
			idx = i
			break
		}
	}
	if idx < 0 {
		return fmt.Errorf("checkpoint: internal error, %q is not recorded as in flight", parent.Url)
	}

	nextInflight := append([]*request.Request{}, s.inflight[:idx]...)
	nextInflight = append(nextInflight, s.inflight[idx+1:]...)

	nextQueued := append([]*request.Request{}, s.queued...)
	nextDone := make(map[string]struct{}, len(s.done)+1)
	for k := range s.done {
		nextDone[k] = struct{}{}
	}
	nextDone[parent.Url] = struct{}{}

	nextSeen := make(map[string]struct{}, len(s.seen)+len(children))
	for k := range s.seen {
		nextSeen[k] = struct{}{}
	}
	for _, child := range children {
		if _, ok := nextSeen[child.Url]; ok {
			continue
		}
		nextSeen[child.Url] = struct{}{}
		nextQueued = append(nextQueued, child)
	}

	if err := s.persist(nextQueued, nextInflight, nextDone, nextSeen, s.finished); err != nil {
		// The old snapshot remains on disk and readable.
		return err
	}
	s.queued = nextQueued
	s.inflight = nextInflight
	s.done = nextDone
	s.seen = nextSeen
	return nil
}

// requeue puts an in-flight request back at the front of the queue (for
// example after exhausting download retries or a processor panic) and records
// that state durably. The request is not marked done.
func (s *checkpointStore) requeue(req *request.Request) error {
	s.mu.Lock()
	defer s.mu.Unlock()

	idx := -1
	for i, in := range s.inflight {
		if in.Url == req.Url {
			idx = i
			break
		}
	}
	if idx < 0 {
		return fmt.Errorf("checkpoint: internal error, %q is not recorded as in flight", req.Url)
	}

	nextInflight := append([]*request.Request{}, s.inflight[:idx]...)
	nextInflight = append(nextInflight, s.inflight[idx+1:]...)
	nextQueued := append([]*request.Request{req}, s.queued...)

	if err := s.persist(nextQueued, nextInflight, s.done, s.seen, s.finished); err != nil {
		return err
	}
	s.queued = nextQueued
	s.inflight = nextInflight
	return nil
}

func (s *checkpointStore) empty() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.queued) == 0 && len(s.inflight) == 0
}

// markFinished durably records that the crawl drained completely. Reopening
// the directory afterwards must not invoke Finish again.
func (s *checkpointStore) markFinished() error {
	s.mu.Lock()
	defer s.mu.Unlock()

	if err := s.persist(s.queued, s.inflight, s.done, s.seen, true); err != nil {
		return err
	}
	s.finished = true
	return nil
}

// persist serializes the given frontier state and swaps it into place
// atomically. It deliberately takes the state as arguments so callers can
// stage a full new state without mutating the live one first: a failed write
// leaves both the on-disk snapshot and the in-memory state untouched.
func (s *checkpointStore) persist(queued, inflight []*request.Request, done, seen map[string]struct{}, finished bool) error {
	snap := snapshot{Version: snapshotVersion, Finished: finished}

	snap.Queued = make([]snapshotRequest, 0, len(queued))
	for _, req := range queued {
		snap.Queued = append(snap.Queued, toSnapshotRequest(req))
	}
	snap.Inflight = make([]snapshotRequest, 0, len(inflight))
	for _, req := range inflight {
		snap.Inflight = append(snap.Inflight, toSnapshotRequest(req))
	}
	snap.Done = make([]string, 0, len(done))
	for u := range done {
		snap.Done = append(snap.Done, u)
	}
	sort.Strings(snap.Done)

	data, err := json.MarshalIndent(&snap, "", "  ")
	if err != nil {
		return fmt.Errorf("checkpoint: cannot encode snapshot: %w", err)
	}
	if err := writeSnapshotFile(s.path, data); err != nil {
		return err
	}
	return nil
}

// writeSnapshotFile writes to a temp file in the same directory, flushes it
// and renames it over the old snapshot. Until the rename succeeds the old
// file stays readable.
func writeSnapshotFile(path string, data []byte) (retErr error) {
	dir := filepath.Dir(path)
	tmp, err := os.CreateTemp(dir, ".checkpoint-*.tmp")
	if err != nil {
		return fmt.Errorf("checkpoint: cannot create temporary snapshot: %w", err)
	}
	tmpName := tmp.Name()
	defer func() {
		if retErr != nil {
			os.Remove(tmpName)
		}
	}()

	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		return fmt.Errorf("checkpoint: cannot write snapshot: %w", err)
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		return fmt.Errorf("checkpoint: cannot flush snapshot: %w", err)
	}
	if err := tmp.Close(); err != nil {
		return fmt.Errorf("checkpoint: cannot close snapshot: %w", err)
	}
	if err := os.Rename(tmpName, path); err != nil {
		return fmt.Errorf("checkpoint: cannot replace snapshot: %w", err)
	}
	if d, err := os.Open(dir); err == nil {
		_ = d.Sync()
		_ = d.Close()
	}
	return nil
}
