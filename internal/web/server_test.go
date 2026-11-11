package web

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"testing"

	"rtpfreeze/internal/capture"
	"rtpfreeze/internal/store"
)

func newTestServer(t *testing.T) (*Server, *store.Store) {
	t.Helper()
	st, err := store.Open(filepath.Join(t.TempDir(), "web.db"))
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() { st.Close() })
	return NewServer(st), st
}

func do(srv *Server, method, target string, body []byte) *httptest.ResponseRecorder {
	var r *http.Request
	if body != nil {
		r = httptest.NewRequest(method, target, bytes.NewReader(body))
		r.Header.Set("Content-Type", "application/x-ndjson")
	} else {
		r = httptest.NewRequest(method, target, nil)
	}
	w := httptest.NewRecorder()
	srv.ServeHTTP(w, r)
	return w
}

func TestImportAndAPIRoundTrip(t *testing.T) {
	srv, _ := newTestServer(t)

	// Empty capture list first.
	w := do(srv, http.MethodGet, "/api/captures", nil)
	if w.Code != http.StatusOK {
		t.Fatalf("list status = %d", w.Code)
	}

	// Import the bundled sample through the HTTP boundary.
	jsonl, err := capture.JSONL()
	if err != nil {
		t.Fatal(err)
	}
	w = do(srv, http.MethodPost, "/api/import", jsonl)
	if w.Code != http.StatusOK {
		t.Fatalf("import status = %d body=%s", w.Code, w.Body.String())
	}
	var imported struct {
		CaptureID   int64 `json:"capture_id"`
		Freezes     int   `json:"freezes"`
		Generations int   `json:"generations"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &imported); err != nil {
		t.Fatal(err)
	}
	if imported.CaptureID == 0 || imported.Freezes == 0 {
		t.Fatalf("import response incomplete: %+v", imported)
	}

	base := "/api/captures/" + itoa64(imported.CaptureID)

	for _, path := range []string{
		base + "/overview",
		base + "/rtp?limit=10",
		base + "/feedback?limit=10",
		base + "/freezes",
		base + "/sender-reports",
		base + "/packets/0",
	} {
		if w := do(srv, http.MethodGet, path, nil); w.Code != http.StatusOK {
			t.Fatalf("GET %s -> %d: %s", path, w.Code, w.Body.String())
		}
	}

	// Static UI is served and contains the required section heading.
	w = do(srv, http.MethodGet, "/", nil)
	if w.Code != http.StatusOK || !bytes.Contains(w.Body.Bytes(), []byte("Feedback causality")) {
		t.Fatalf("index page missing Feedback causality section: code=%d", w.Code)
	}
}

func TestUnknownCaptureAndPacket404(t *testing.T) {
	srv, _ := newTestServer(t)
	w := do(srv, http.MethodGet, "/api/captures/999/overview", nil)
	if w.Code != http.StatusNotFound {
		t.Fatalf("want 404 for unknown capture, got %d", w.Code)
	}
}

func TestImportRejectsGarbage(t *testing.T) {
	srv, _ := newTestServer(t)
	w := do(srv, http.MethodPost, "/api/import", []byte("{not json\n"))
	if w.Code != http.StatusBadRequest {
		t.Fatalf("want 400 for malformed JSONL, got %d", w.Code)
	}
}

func itoa64(v int64) string {
	if v == 0 {
		return "0"
	}
	var b [24]byte
	i := len(b)
	for v > 0 {
		i--
		b[i] = byte('0' + v%10)
		v /= 10
	}
	return string(b[i:])
}
