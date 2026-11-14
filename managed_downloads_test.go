package selenium_test

import (
	"archive/zip"
	"bytes"
	"encoding/base64"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"github.com/tebeka/selenium"
	"github.com/tebeka/selenium/task_support/taskcheck"
)

// zipEntry describes one entry handed to buildZip.
type zipEntry struct {
	name string
	dir  bool
	data []byte
	// mode, when non-zero, sets a non-regular mode (for example os.ModeSymlink).
	mode os.FileMode
	// store forces the Store method, so the payload appears verbatim in the
	// archive and can be corrupted for CRC tests.
	store bool
}

// buildZip builds a ZIP archive in memory using only the standard library.
func buildZip(t *testing.T, entries ...zipEntry) []byte {
	t.Helper()
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	for _, entry := range entries {
		fh := &zip.FileHeader{Name: entry.name}
		switch {
		case entry.dir:
			fh.SetMode(os.ModeDir | 0700)
		case entry.mode != 0:
			fh.SetMode(entry.mode)
		default:
			fh.SetMode(0600)
		}
		if entry.store {
			fh.Method = zip.Store
		} else {
			fh.Method = zip.Deflate
		}
		w, err := zw.CreateHeader(fh)
		if err != nil {
			t.Fatalf("creating zip header %q: %v", entry.name, err)
		}
		if !entry.dir {
			if _, err := w.Write(entry.data); err != nil {
				t.Fatalf("writing zip entry %q: %v", entry.name, err)
			}
		}
	}
	if err := zw.Close(); err != nil {
		t.Fatalf("closing zip writer: %v", err)
	}
	return buf.Bytes()
}

// corruptZip flips the first byte of the given verbatim payload inside a
// Store-method archive, so extraction must fail the CRC check.
func corruptZip(t *testing.T, archive, payload []byte) []byte {
	t.Helper()
	idx := bytes.Index(archive, payload)
	if idx < 0 {
		t.Fatal("payload not found in stored archive")
	}
	out := append([]byte(nil), archive...)
	out[idx] ^= 0xff
	return out
}

// wireServer is a controllable stand-in for the Grid se/files endpoint.
type wireServer struct {
	t         *testing.T
	sessionID string
	server    *httptest.Server

	mu sync.Mutex

	names    []string
	archives map[string][]byte
	// rawContents, when set for a name, is served verbatim instead of a
	// properly encoded archive (for malformed-base64 tests).
	rawContents map[string]string
	failPost    map[string]bool
	mismatch    bool

	getCount  int
	postCount int
	requests  []string
	postNames []string
}

func newWireServer(t *testing.T, sessionID string, names []string, archives map[string][]byte) *wireServer {
	t.Helper()
	ws := &wireServer{
		t:           t,
		sessionID:   sessionID,
		names:       names,
		archives:    archives,
		rawContents: map[string]string{},
		failPost:    map[string]bool{},
	}
	ws.server = httptest.NewServer(http.HandlerFunc(ws.handle))
	t.Cleanup(ws.server.Close)
	return ws
}

func (ws *wireServer) handle(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")

	if r.Method == "POST" && r.URL.Path == "/session" {
		io.Copy(io.Discard, r.Body)
		w.Write([]byte(`{"value":{"sessionId":"` + ws.sessionID + `","capabilities":{"browserName":"chrome"}}}`))
		return
	}

	filesPath := "/session/" + ws.sessionID + "/se/files"
	if r.URL.Path != filesPath {
		w.WriteHeader(http.StatusNotFound)
		w.Write([]byte(`{"value":{"error":"unknown command","message":"unexpected path"}}`))
		return
	}

	ws.mu.Lock()
	defer ws.mu.Unlock()
	ws.requests = append(ws.requests, r.Method+" "+r.URL.Path)

	switch r.Method {
	case "GET":
		ws.getCount++
		json.NewEncoder(w).Encode(map[string]interface{}{
			"value": map[string]interface{}{"names": ws.names},
		})
	case "POST":
		var body struct {
			Name string `json:"name"`
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			w.WriteHeader(http.StatusBadRequest)
			w.Write([]byte(`{"value":{"error":"invalid argument","message":"bad body"}}`))
			return
		}
		ws.postCount++
		ws.postNames = append(ws.postNames, body.Name)

		if ws.failPost[body.Name] {
			w.WriteHeader(http.StatusInternalServerError)
			w.Write([]byte(`{"value":{"error":"unknown error","message":"injected server failure"}}`))
			return
		}
		filename := body.Name
		if ws.mismatch {
			filename = "not-the-requested-name"
		}
		contents, ok := ws.rawContents[body.Name]
		if !ok {
			archive, known := ws.archives[body.Name]
			if !known {
				w.WriteHeader(http.StatusNotFound)
				w.Write([]byte(`{"value":{"error":"unknown error","message":"unknown download"}}`))
				return
			}
			contents = base64.StdEncoding.EncodeToString(archive)
		}
		json.NewEncoder(w).Encode(map[string]interface{}{
			"value": map[string]string{
				"filename": filename,
				"contents": contents,
			},
		})
	default:
		w.WriteHeader(http.StatusMethodNotAllowed)
	}
}

func (ws *wireServer) snapshot() (get, post int, requests, postNames []string) {
	ws.mu.Lock()
	defer ws.mu.Unlock()
	return ws.getCount, ws.postCount,
		append([]string(nil), ws.requests...),
		append([]string(nil), ws.postNames...)
}

func newRemote(t *testing.T, url string) selenium.WebDriver {
	t.Helper()
	wd, err := selenium.NewRemote(selenium.Capabilities{
		"browserName":         "chrome",
		"se:downloadsEnabled": true,
	}, url)
	if err != nil {
		t.Fatalf("NewRemote: %v", err)
	}
	t.Cleanup(func() { wd.Quit() })
	return wd
}

func generousLimits() selenium.DownloadLimits {
	return selenium.DownloadLimits{
		MaxArchiveBytes:  1 << 20,
		MaxExpandedBytes: 1 << 20,
		MaxFiles:         100,
	}
}

func pathExists(t *testing.T, p string) bool {
	t.Helper()
	_, err := os.Lstat(p)
	if err == nil {
		return true
	}
	if os.IsNotExist(err) {
		return false
	}
	t.Fatalf("stating %q: %v", p, err)
	return false
}

func noStagingLeft(t *testing.T, dir string) bool {
	t.Helper()
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatalf("reading %q: %v", dir, err)
	}
	for _, entry := range entries {
		if strings.HasPrefix(entry.Name(), ".download-snapshot-") {
			return false
		}
	}
	return true
}

// TestDownloadSnapshotWire covers the se/files wire protocol: the single list
// fetch, ordered per-name fetches with the exact body and routing, empty
// batches, protocol-level failures, parameter validation and retry.
func TestDownloadSnapshotWire(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const sessionID = "wire-session"
		binaryA := bytes.Repeat([]byte{0, 1, 2, 255, 128, 7}, 40)
		binaryB := bytes.Repeat([]byte{255, 254, 1, 0, 99}, 33)

		// Remote names deliberately contain spaces and slashes: they are
		// opaque identifiers and must never be interpreted as local paths.
		names := []string{"jobs/42/alpha report.bin", "jobs/42/beta report.bin"}
		archives := map[string][]byte{
			names[0]: buildZip(t,
				zipEntry{name: "reports/", dir: true},
				zipEntry{name: "reports/data/", dir: true},
				// Deliberately out of lexical order, so central-directory
				// ordering can be distinguished from sorted ordering.
				zipEntry{name: "reports/data/z-first.bin", data: binaryA},
				zipEntry{name: "reports/notes.txt", data: []byte("hello notes")},
			),
			names[1]: buildZip(t,
				zipEntry{name: "reports/data/b-second.bin", data: binaryB},
				zipEntry{name: "reports/top.bin", data: []byte("top level")},
			),
		}

		ws := newWireServer(t, sessionID, names, archives)
		wd := newRemote(t, ws.server.URL)

		tmp := t.TempDir()
		destination := filepath.Join(tmp, "snapshot")

		files, err := wd.DownloadSnapshot(destination, generousLimits())
		c.Check(err == nil, "successful snapshot: %v", err)

		// Result ordering: remote name order first, then central directory
		// order within each archive. Directories are not returned.
		expected := []DownloadedFileExpectation{
			{names[0], "reports/data/z-first.bin", int64(len(binaryA)), binaryA},
			{names[0], "reports/notes.txt", int64(len("hello notes")), []byte("hello notes")},
			{names[1], "reports/data/b-second.bin", int64(len(binaryB)), binaryB},
			{names[1], "reports/top.bin", int64(len("top level")), []byte("top level")},
		}
		c.Check(len(files) == len(expected), "got %d files, want %d", len(files), len(expected))
		for i, want := range expected {
			if i >= len(files) {
				break
			}
			got := files[i]
			wantPath := filepath.Join(destination, filepath.FromSlash(want.relPath))
			c.Check(got.RemoteName == want.remoteName, "file %d remote name = %q, want %q", i, got.RemoteName, want.remoteName)
			c.Check(got.Path == wantPath, "file %d path = %q, want %q", i, got.Path, wantPath)
			c.Check(filepath.IsAbs(got.Path), "file %d path %q is not absolute", i, got.Path)
			c.Check(got.Size == want.size, "file %d size = %d, want %d", i, got.Size, want.size)
			payload, readErr := os.ReadFile(got.Path)
			c.Check(readErr == nil, "reading published file %q: %v", got.Path, readErr)
			c.Check(bytes.Equal(payload, want.content), "file %d content mismatch", i)
			info, statErr := os.Stat(got.Path)
			c.Check(statErr == nil, "stating %q: %v", got.Path, statErr)
			c.Check(statErr == nil && info.Mode().Perm() == 0600, "file %q perms = %o, want 0600", got.Path, info.Mode().Perm())
		}
		for _, dir := range []string{"reports", "reports/data"} {
			info, statErr := os.Stat(filepath.Join(destination, dir))
			c.Check(statErr == nil && info.IsDir(), "directory %q missing: %v", dir, statErr)
			c.Check(statErr == nil && info.Mode().Perm() == 0700, "directory %q perms = %o, want 0700", dir, info.Mode().Perm())
		}

		get, post, requests, postNames := ws.snapshot()
		c.Check(get == 1, "GET count = %d, want 1", get)
		c.Check(post == 2, "POST count = %d, want 2", post)
		c.Check(len(requests) == 3, "request count = %d, want 3", len(requests))
		wantRoute := "GET /session/" + sessionID + "/se/files"
		c.Check(len(requests) > 0 && requests[0] == wantRoute, "first request = %q, want %q", firstOf(requests), wantRoute)
		for i, request := range requests {
			c.Check(request == wantRoute || request == "POST /session/"+sessionID+"/se/files",
				"request %d routed to %q", i, request)
		}
		c.Check(equalStrings(postNames, names), "POST names = %v, want %v", postNames, names)

		// An empty list returns an empty result without creating anything or
		// posting.
		emptyWS := newWireServer(t, "empty-session", nil, nil)
		emptyWD := newRemote(t, emptyWS.server.URL)
		emptyDest := filepath.Join(tmp, "empty")
		emptyFiles, emptyErr := emptyWD.DownloadSnapshot(emptyDest, generousLimits())
		c.Check(emptyErr == nil, "empty snapshot: %v", emptyErr)
		c.Check(len(emptyFiles) == 0, "empty snapshot returned %d files", len(emptyFiles))
		c.Check(!pathExists(t, emptyDest), "empty snapshot created %q", emptyDest)
		emptyGet, emptyPost, _, _ := emptyWS.snapshot()
		c.Check(emptyGet == 1 && emptyPost == 0, "empty batch requests: GET=%d POST=%d", emptyGet, emptyPost)

		// A reply whose filename does not match the request is a protocol
		// error and publishes nothing.
		mismatchArchives := map[string][]byte{
			"only.bin": buildZip(t, zipEntry{name: "f.txt", data: []byte("x")}),
		}
		mismatchWS := newWireServer(t, "mismatch-session", []string{"only.bin"}, mismatchArchives)
		mismatchWS.mismatch = true
		mismatchWD := newRemote(t, mismatchWS.server.URL)
		mismatchDest := filepath.Join(tmp, "mismatch")
		_, mismatchErr := mismatchWD.DownloadSnapshot(mismatchDest, generousLimits())
		c.Check(mismatchErr != nil, "filename mismatch did not error")
		c.Check(!pathExists(t, mismatchDest), "mismatch published %q", mismatchDest)

		// Malformed base64 is rejected.
		badB64WS := newWireServer(t, "badb64-session", []string{"only.bin"}, mismatchArchives)
		badB64WS.rawContents["only.bin"] = "not base64!"
		badB64WD := newRemote(t, badB64WS.server.URL)
		badB64Dest := filepath.Join(tmp, "badb64")
		_, badB64Err := badB64WD.DownloadSnapshot(badB64Dest, generousLimits())
		c.Check(badB64Err != nil, "malformed base64 did not error")
		c.Check(!pathExists(t, badB64Dest), "malformed base64 published %q", badB64Dest)

		// Empty and duplicate remote names are protocol errors: nothing is
		// posted and no destination directory is created.
		badNameCases := [][]string{
			{""},
			{"only.bin", "only.bin"},
		}
		for i, badNames := range badNameCases {
			badNameWS := newWireServer(t, "badnames-session", badNames, mismatchArchives)
			badNameWD := newRemote(t, badNameWS.server.URL)
			badNameDest := filepath.Join(tmp, "badnames-"+string(rune('0'+i)))
			_, badNameErr := badNameWD.DownloadSnapshot(badNameDest, generousLimits())
			c.Check(badNameErr != nil, "bad remote names %v did not error", badNames)
			c.Check(!pathExists(t, badNameDest), "bad remote names published %q", badNameDest)
			badNameGet, badNamePost, _, _ := badNameWS.snapshot()
			c.Check(badNameGet == 1 && badNamePost == 0,
				"bad remote names sent POSTs: GET=%d POST=%d", badNameGet, badNamePost)
		}

		// Invalid parameters must fail before a single request is sent.
		invalidWS := newWireServer(t, "invalid-session", []string{"only.bin"}, mismatchArchives)
		invalidWD := newRemote(t, invalidWS.server.URL)
		existingDir := filepath.Join(tmp, "existing")
		if err := os.Mkdir(existingDir, 0700); err != nil {
			t.Fatal(err)
		}
		symlinkPath := filepath.Join(tmp, "symlink-dest")
		if err := os.Symlink(existingDir, symlinkPath); err != nil {
			t.Fatal(err)
		}
		invalidCases := []struct {
			name        string
			destination string
			limits      selenium.DownloadLimits
		}{
			{"zero archive limit", filepath.Join(tmp, "d1"), selenium.DownloadLimits{0, 10, 10}},
			{"zero expanded limit", filepath.Join(tmp, "d2"), selenium.DownloadLimits{10, 0, 10}},
			{"zero file limit", filepath.Join(tmp, "d3"), selenium.DownloadLimits{10, 10, 0}},
			{"negative archive limit", filepath.Join(tmp, "d4"), selenium.DownloadLimits{-1, 10, 10}},
			{"relative destination", filepath.Join("relative", "d5"), generousLimits()},
			{"missing parent", filepath.Join(tmp, "no-such-parent", "d6"), generousLimits()},
			{"existing destination", existingDir, generousLimits()},
			{"symlink destination", symlinkPath, generousLimits()},
		}
		for _, tc := range invalidCases {
			_, invalidErr := invalidWD.DownloadSnapshot(tc.destination, tc.limits)
			c.Check(invalidErr != nil, "%s did not error", tc.name)
		}
		invalidGet, invalidPost, _, _ := invalidWS.snapshot()
		c.Check(invalidGet == 0 && invalidPost == 0,
			"invalid parameters sent requests: GET=%d POST=%d", invalidGet, invalidPost)

		// A server failure on the second package publishes nothing; after the
		// server recovers, a retry against the same destination succeeds.
		failWS := newWireServer(t, "fail-session", names, archives)
		failWS.failPost[names[1]] = true
		failWD := newRemote(t, failWS.server.URL)
		failDest := filepath.Join(tmp, "retry")
		_, failErr := failWD.DownloadSnapshot(failDest, generousLimits())
		c.Check(failErr != nil, "injected server failure did not error")
		c.Check(!pathExists(t, failDest), "failed attempt published %q", failDest)
		c.Check(noStagingLeft(t, tmp), "failed attempt left a staging directory")

		failWS.failPost = map[string]bool{}
		retryFiles, retryErr := failWD.DownloadSnapshot(failDest, generousLimits())
		c.Check(retryErr == nil, "retry failed: %v", retryErr)
		c.Check(len(retryFiles) == 4, "retry returned %d files, want 4", len(retryFiles))
		c.Check(pathExists(t, failDest), "retry did not publish %q", failDest)
		retryGet, retryPost, _, retryPostNames := failWS.snapshot()
		c.Check(retryGet == 2, "GET count across attempts = %d, want 2", retryGet)
		c.Check(retryPost == 4, "POST count across attempts = %d, want 4", retryPost)
		c.Check(equalStrings(retryPostNames, []string{names[0], names[1], names[0], names[1]}),
			"POST order across attempts = %v", retryPostNames)
	})
}

// DownloadedFileExpectation is a test-only description of an expected result.
type DownloadedFileExpectation struct {
	remoteName string
	relPath    string
	size       int64
	content    []byte
}

func firstOf(values []string) string {
	if len(values) == 0 {
		return ""
	}
	return values[0]
}

func equalStrings(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// singleArchiveServer builds a server serving one archive under one name.
func singleArchiveServer(t *testing.T, sessionID, name string, archive []byte) *wireServer {
	t.Helper()
	return newWireServer(t, sessionID, []string{name}, map[string][]byte{name: archive})
}

// TestDownloadSnapshotArchive covers archive validation: budget boundaries,
// CRC verification, cross-package namespace conflicts and entry-name rejection
// rules.
func TestDownloadSnapshotArchive(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		tmp := t.TempDir()
		const name = "download.bin"
		payload := bytes.Repeat([]byte("0123456789abcdef"), 8) // 128 bytes

		// Boundary values are accepted; one byte or one file beyond is not.
		goodArchive := buildZip(t, zipEntry{name: "f.bin", data: payload})
		archiveSize := int64(len(goodArchive))
		expandedSize := int64(len(payload))

		okWS := singleArchiveServer(t, "bound-ok", name, goodArchive)
		okWD := newRemote(t, okWS.server.URL)
		okFiles, okErr := okWD.DownloadSnapshot(filepath.Join(tmp, "bound-ok"), selenium.DownloadLimits{
			MaxArchiveBytes:  archiveSize,
			MaxExpandedBytes: expandedSize,
			MaxFiles:         1,
		})
		c.Check(okErr == nil, "boundary equality rejected: %v", okErr)
		c.Check(len(okFiles) == 1 && okFiles[0].Size == expandedSize, "boundary result = %+v", okFiles)

		tooSmallArchive := singleArchiveServer(t, "bound-archive", name, goodArchive)
		_, archiveErr := newRemote(t, tooSmallArchive.server.URL).DownloadSnapshot(
			filepath.Join(tmp, "bound-archive"),
			selenium.DownloadLimits{archiveSize - 1, 1 << 20, 100})
		c.Check(archiveErr != nil, "MaxArchiveBytes-1 did not error")

		tooSmallExpanded := singleArchiveServer(t, "bound-expanded", name, goodArchive)
		_, expandedErr := newRemote(t, tooSmallExpanded.server.URL).DownloadSnapshot(
			filepath.Join(tmp, "bound-expanded"),
			selenium.DownloadLimits{archiveSize, expandedSize - 1, 100})
		c.Check(expandedErr != nil, "MaxExpandedBytes-1 did not error")

		twoFileArchive := buildZip(t,
			zipEntry{name: "a", data: []byte("aa")},
			zipEntry{name: "b", data: []byte("bb")},
		)
		tooFewFiles := singleArchiveServer(t, "bound-files", name, twoFileArchive)
		_, filesErr := newRemote(t, tooFewFiles.server.URL).DownloadSnapshot(
			filepath.Join(tmp, "bound-files"),
			selenium.DownloadLimits{1 << 20, 1 << 20, 1})
		c.Check(filesErr != nil, "MaxFiles below actual count did not error")
		exactFiles := singleArchiveServer(t, "bound-files-ok", name, twoFileArchive)
		exactFileResults, exactFilesErr := newRemote(t, exactFiles.server.URL).DownloadSnapshot(
			filepath.Join(tmp, "bound-files-ok"),
			selenium.DownloadLimits{1 << 20, 1 << 20, 2})
		c.Check(exactFilesErr == nil, "MaxFiles equality rejected: %v", exactFilesErr)
		c.Check(len(exactFileResults) == 2, "MaxFiles equality returned %d files", len(exactFileResults))

		// Directory entries do not count against MaxFiles.
		dirsOnlyPlusFile := buildZip(t,
			zipEntry{name: "d1/", dir: true},
			zipEntry{name: "d2/nested/", dir: true},
			zipEntry{name: "d2/f", data: []byte("x")},
		)
		dirWS := singleArchiveServer(t, "dirs", name, dirsOnlyPlusFile)
		dirResults, dirErr := newRemote(t, dirWS.server.URL).DownloadSnapshot(
			filepath.Join(tmp, "dirs"),
			selenium.DownloadLimits{1 << 20, 1 << 20, 1})
		c.Check(dirErr == nil, "directory-only entries broke MaxFiles: %v", dirErr)
		c.Check(len(dirResults) == 1, "directory entries were returned as files: %+v", dirResults)

		// A corrupted payload must fail the end-of-entry CRC check.
		storedArchive := buildZip(t, zipEntry{name: "f.bin", data: payload, store: true})
		corrupted := corruptZip(t, storedArchive, payload)
		crcWS := singleArchiveServer(t, "crc", name, corrupted)
		_, crcErr := newRemote(t, crcWS.server.URL).DownloadSnapshot(
			filepath.Join(tmp, "crc"), generousLimits())
		c.Check(crcErr != nil, "corrupted archive did not error")
		c.Check(!pathExists(t, filepath.Join(tmp, "crc")), "corrupted archive was published")

		// Cross-package namespace conflicts.
		conflictCases := []struct {
			scenario string
			first    []zipEntry
			second   []zipEntry
		}{
			{
				"duplicate file",
				[]zipEntry{{name: "dup.txt", data: []byte("one")}},
				[]zipEntry{{name: "dup.txt", data: []byte("two")}},
			},
			{
				"file then directory",
				[]zipEntry{{name: "x", data: []byte("one")}},
				[]zipEntry{{name: "x/", dir: true}},
			},
			{
				"directory then file",
				[]zipEntry{{name: "d/", dir: true}},
				[]zipEntry{{name: "d", data: []byte("one")}},
			},
			{
				"file ancestor of another file",
				[]zipEntry{{name: "a", data: []byte("one")}},
				[]zipEntry{{name: "a/b", data: []byte("two")}},
			},
		}
		for _, tc := range conflictCases {
			conflictNames := []string{"first.zip", "second.zip"}
			conflictArchives := map[string][]byte{
				"first.zip":  buildZip(t, tc.first...),
				"second.zip": buildZip(t, tc.second...),
			}
			conflictWS := newWireServer(t, "conflict", conflictNames, conflictArchives)
			conflictDest := filepath.Join(tmp, "conflict-"+tc.scenario)
			_, conflictErr := newRemote(t, conflictWS.server.URL).DownloadSnapshot(conflictDest, generousLimits())
			c.Check(conflictErr != nil, "conflict %q did not error", tc.scenario)
			c.Check(!pathExists(t, conflictDest), "conflict %q published %q", tc.scenario, conflictDest)
		}

		// An explicit directory entry may coincide with a directory that was
		// created implicitly by an earlier package.
		coexistNames := []string{"first.zip", "second.zip"}
		coexistArchives := map[string][]byte{
			"first.zip": buildZip(t, zipEntry{name: "k/sub/f1", data: []byte("one")}),
			"second.zip": buildZip(t,
				zipEntry{name: "k/sub/", dir: true},
				zipEntry{name: "k/sub/f2", data: []byte("two")},
			),
		}
		coexistWS := newWireServer(t, "coexist", coexistNames, coexistArchives)
		coexistResults, coexistErr := newRemote(t, coexistWS.server.URL).DownloadSnapshot(
			filepath.Join(tmp, "coexist"), generousLimits())
		c.Check(coexistErr == nil, "explicit/implicit directory coexistence failed: %v", coexistErr)
		c.Check(len(coexistResults) == 2, "coexistence returned %d files", len(coexistResults))

		// Entry-name rejection rules, each archive hand-built to contain one
		// invalid entry alongside a harmless one.
		rejectCases := []struct {
			scenario string
			entries  []zipEntry
		}{
			{"absolute path", []zipEntry{{name: "good.txt", data: []byte("g")}, {name: "/etc/evil", data: []byte("x")}}},
			{"backslash", []zipEntry{{name: "a\\b", data: []byte("x")}}},
			{"NUL byte", []zipEntry{{name: "a\x00b", data: []byte("x")}}},
			{"volume prefix", []zipEntry{{name: "C:/evil", data: []byte("x")}}},
			{"dot segment", []zipEntry{{name: "a/./b", data: []byte("x")}}},
			{"parent segment", []zipEntry{{name: "a/../b", data: []byte("x")}}},
			{"empty segment", []zipEntry{{name: "a//b", data: []byte("x")}}},
			{"dot only", []zipEntry{{name: ".", data: []byte("x")}}},
			{"parent only", []zipEntry{{name: "..", data: []byte("x")}}},
			{"symlink type", []zipEntry{{name: "link", data: []byte("target"), mode: os.ModeSymlink}}},
		}
		for _, tc := range rejectCases {
			rejectWS := singleArchiveServer(t, "reject", name, buildZip(t, tc.entries...))
			rejectDest := filepath.Join(tmp, "reject-"+tc.scenario)
			_, rejectErr := newRemote(t, rejectWS.server.URL).DownloadSnapshot(rejectDest, generousLimits())
			c.Check(rejectErr != nil, "rejection %q did not error", tc.scenario)
			c.Check(!pathExists(t, rejectDest), "rejection %q published %q", tc.scenario, rejectDest)
		}
	})
}

// TestDownloadSnapshotPublication covers the staging-and-rename publication
// contract: failures never touch the parent or its neighbors, and a successful
// snapshot publishes the whole tree at once.
func TestDownloadSnapshotPublication(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		tmp := t.TempDir()
		names := []string{"first.zip", "second.zip"}
		archives := map[string][]byte{
			"first.zip": buildZip(t,
				zipEntry{name: "reports/", dir: true},
				zipEntry{name: "reports/a.bin", data: bytes.Repeat([]byte{1, 2, 3}, 20)},
			),
			"second.zip": buildZip(t,
				zipEntry{name: "reports/nested/b.bin", data: bytes.Repeat([]byte{9, 8, 7}, 20)},
			),
		}

		neighbor := filepath.Join(tmp, "neighbor.bin")
		neighborBytes := []byte("precious neighbor contents\n")
		if err := os.WriteFile(neighbor, neighborBytes, 0600); err != nil {
			t.Fatal(err)
		}
		neighborDir := filepath.Join(tmp, "neighbor-dir")
		if err := os.Mkdir(neighborDir, 0700); err != nil {
			t.Fatal(err)
		}
		parentBefore, err := os.ReadDir(tmp)
		if err != nil {
			t.Fatal(err)
		}

		destination := filepath.Join(tmp, "published")

		// A server-side failure of the second package must leave the parent
		// directory and its neighbors byte-for-byte unchanged.
		failWS := newWireServer(t, "pub-fail", names, archives)
		failWS.failPost["second.zip"] = true
		failWD := newRemote(t, failWS.server.URL)
		_, failErr := failWD.DownloadSnapshot(destination, generousLimits())
		c.Check(failErr != nil, "injected failure did not error")
		c.Check(!pathExists(t, destination), "failed snapshot published %q", destination)
		c.Check(noStagingLeft(t, tmp), "staging directory left behind")
		gotNeighbor, readErr := os.ReadFile(neighbor)
		c.Check(readErr == nil && bytes.Equal(gotNeighbor, neighborBytes),
			"neighbor changed after failure: %v", readErr)
		parentAfter, readDirErr := os.ReadDir(tmp)
		c.Check(readDirErr == nil && len(parentAfter) == len(parentBefore),
			"parent entry count changed after failure: %d vs %d", len(parentAfter), len(parentBefore))

		// An archive-level failure must be equally clean.
		corruptName := "broken.zip"
		payload := bytes.Repeat([]byte("stored-payload"), 4)
		corruptArchive := corruptZip(t,
			buildZip(t, zipEntry{name: "broken.bin", data: payload, store: true}), payload)
		corruptWS := newWireServer(t, "pub-crc", []string{corruptName},
			map[string][]byte{corruptName: corruptArchive})
		corruptDest := filepath.Join(tmp, "corrupt-published")
		_, corruptErr := newRemote(t, corruptWS.server.URL).DownloadSnapshot(corruptDest, generousLimits())
		c.Check(corruptErr != nil, "corrupt archive did not error")
		c.Check(!pathExists(t, corruptDest), "corrupt snapshot published %q", corruptDest)
		c.Check(noStagingLeft(t, tmp), "staging directory left after corrupt archive")
		gotNeighbor, readErr = os.ReadFile(neighbor)
		c.Check(readErr == nil && bytes.Equal(gotNeighbor, neighborBytes),
			"neighbor changed after corrupt archive: %v", readErr)

		// After recovery, the retry publishes the complete tree in one rename.
		failWS.failPost = map[string]bool{}
		files, retryErr := failWD.DownloadSnapshot(destination, generousLimits())
		c.Check(retryErr == nil, "retry failed: %v", retryErr)
		c.Check(len(files) == 2, "retry published %d files, want 2", len(files))
		c.Check(pathExists(t, destination), "destination missing after retry")
		c.Check(noStagingLeft(t, tmp), "staging directory left after success")

		// Walk the published tree: files 0600, directories 0700.
		walkErr := filepath.Walk(destination, func(p string, info os.FileInfo, walkErr error) error {
			if walkErr != nil {
				return walkErr
			}
			if p == destination {
				return nil
			}
			if info.IsDir() {
				c.Check(info.Mode().Perm() == 0700, "directory %q perms = %o", p, info.Mode().Perm())
			} else {
				c.Check(info.Mode().IsRegular(), "non-regular file %q published", p)
				c.Check(info.Mode().Perm() == 0600, "file %q perms = %o", p, info.Mode().Perm())
			}
			return nil
		})
		c.Check(walkErr == nil, "walking published tree: %v", walkErr)

		// The neighbor and its bytes are still intact, and only the new
		// destination joined the parent directory.
		gotNeighbor, readErr = os.ReadFile(neighbor)
		c.Check(readErr == nil && bytes.Equal(gotNeighbor, neighborBytes),
			"neighbor changed after success: %v", readErr)
		c.Check(pathExists(t, neighborDir), "neighbor directory disappeared")
		parentFinal, _ := os.ReadDir(tmp)
		c.Check(len(parentFinal) == len(parentBefore)+1,
			"parent entry count after success = %d, want %d", len(parentFinal), len(parentBefore)+1)

		// A second publication onto an existing destination fails without
		// altering either directory.
		_, againErr := failWD.DownloadSnapshot(destination, generousLimits())
		c.Check(againErr != nil, "publishing onto an existing destination did not error")
		gotNeighbor, readErr = os.ReadFile(neighbor)
		c.Check(readErr == nil && bytes.Equal(gotNeighbor, neighborBytes),
			"neighbor changed after rejected re-publication: %v", readErr)
	})
}
