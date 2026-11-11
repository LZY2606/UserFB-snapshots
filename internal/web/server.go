// Package web exposes the offline review UI and its JSON API over HTTP. The
// server never dials anything: all data comes from imports against the local
// SQLite database.
package web

import (
	"embed"
	"encoding/json"
	"errors"
	"io"
	"io/fs"
	"net/http"
	"strconv"
	"strings"

	"rtpfreeze/internal/store"
)

//go:embed static
var staticFS embed.FS

// Server holds the HTTP handlers and their database handle.
type Server struct {
	store *store.Store
	mux   *http.ServeMux
}

// NewServer wires the API routes and the embedded static assets.
func NewServer(st *store.Store) *Server {
	s := &Server{store: st, mux: http.NewServeMux()}
	s.routes()
	return s
}

func (s *Server) routes() {
	s.mux.HandleFunc("GET /api/captures", s.listCaptures)
	s.mux.HandleFunc("POST /api/import", s.importCapture)
	s.mux.HandleFunc("GET /api/captures/{id}/overview", s.overview)
	s.mux.HandleFunc("GET /api/captures/{id}/rtp", s.rtpPackets)
	s.mux.HandleFunc("GET /api/captures/{id}/feedback", s.feedback)
	s.mux.HandleFunc("GET /api/captures/{id}/freezes", s.freezes)
	s.mux.HandleFunc("GET /api/captures/{id}/sender-reports", s.senderReports)
	s.mux.HandleFunc("GET /api/captures/{id}/packets/{idx}", s.rawPacket)

	// The embed root contains the "static" directory itself; fs.Sub makes its
	// contents (index.html, ...) answer at "/".
	sub, err := fs.Sub(staticFS, "static")
	if err != nil {
		panic(err)
	}
	s.mux.Handle("GET /", http.FileServer(http.FS(sub)))
}

// ServeHTTP makes Server an http.Handler.
func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	s.mux.ServeHTTP(w, r)
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	if err := json.NewEncoder(w).Encode(v); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
	}
}

func writeErr(w http.ResponseWriter, code int, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": msg})
}

func captureID(r *http.Request) (int64, bool) {
	id, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if err != nil {
		return 0, false
	}
	return id, true
}

func (s *Server) listCaptures(w http.ResponseWriter, r *http.Request) {
	caps, err := s.store.ListCaptures(r.Context())
	if err != nil {
		writeErr(w, 500, err.Error())
		return
	}
	writeJSON(w, caps)
}

// importCapture accepts either an uploaded JSONL body (field "file") or a
// raw application/x-ndjson request body.
func (s *Server) importCapture(w http.ResponseWriter, r *http.Request) {
	var reader io.Reader
	source := "upload"
	ct := r.Header.Get("Content-Type")
	if strings.HasPrefix(ct, "multipart/form-data") {
		if err := r.ParseMultipartForm(32 << 20); err != nil {
			writeErr(w, 400, "bad multipart form: "+err.Error())
			return
		}
		file, header, err := r.FormFile("file")
		if err != nil {
			writeErr(w, 400, "missing form file: "+err.Error())
			return
		}
		defer file.Close()
		reader = file
		source = header.Filename
	} else {
		reader = r.Body
		if name := r.Header.Get("X-Capture-Name"); name != "" {
			source = name
		}
	}
	res, err := s.store.Import(r.Context(), source, reader)
	if err != nil {
		writeErr(w, 400, err.Error())
		return
	}
	writeJSON(w, res)
}

func (s *Server) overview(w http.ResponseWriter, r *http.Request) {
	id, ok := captureID(r)
	if !ok {
		writeErr(w, 400, "bad capture id")
		return
	}
	o, err := s.store.Overview(r.Context(), id)
	if errors.Is(err, store.ErrNotFound) {
		writeErr(w, 404, "capture not found")
		return
	}
	if err != nil {
		writeErr(w, 500, err.Error())
		return
	}
	writeJSON(w, o)
}

func (s *Server) rtpPackets(w http.ResponseWriter, r *http.Request) {
	id, ok := captureID(r)
	if !ok {
		writeErr(w, 400, "bad capture id")
		return
	}
	f := store.RTPFilter{GenID: -1, TrackID: -1, Limit: 2000}
	if v := r.URL.Query().Get("gen_id"); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			f.GenID = n
		}
	}
	if v := r.URL.Query().Get("track_id"); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			f.TrackID = n
		}
	}
	if v := r.URL.Query().Get("limit"); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			f.Limit = n
		}
	}
	rows, err := s.store.RTPPackets(r.Context(), id, f)
	if err != nil {
		writeErr(w, 500, err.Error())
		return
	}
	writeJSON(w, rows)
}

func (s *Server) feedback(w http.ResponseWriter, r *http.Request) {
	id, ok := captureID(r)
	if !ok {
		writeErr(w, 400, "bad capture id")
		return
	}
	f := store.FeedbackFilter{GenID: -1, Limit: 1000}
	q := r.URL.Query()
	f.Kind = q.Get("kind")
	f.Status = q.Get("status")
	if v := q.Get("gen_id"); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			f.GenID = n
		}
	}
	if v := q.Get("limit"); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			f.Limit = n
		}
	}
	rows, err := s.store.Feedback(r.Context(), id, f)
	if err != nil {
		writeErr(w, 500, err.Error())
		return
	}
	writeJSON(w, rows)
}

func (s *Server) freezes(w http.ResponseWriter, r *http.Request) {
	id, ok := captureID(r)
	if !ok {
		writeErr(w, 400, "bad capture id")
		return
	}
	rows, err := s.store.Freezes(r.Context(), id)
	if err != nil {
		writeErr(w, 500, err.Error())
		return
	}
	writeJSON(w, rows)
}

func (s *Server) senderReports(w http.ResponseWriter, r *http.Request) {
	id, ok := captureID(r)
	if !ok {
		writeErr(w, 400, "bad capture id")
		return
	}
	rows, err := s.store.SenderReports(r.Context(), id)
	if err != nil {
		writeErr(w, 500, err.Error())
		return
	}
	writeJSON(w, rows)
}

func (s *Server) rawPacket(w http.ResponseWriter, r *http.Request) {
	id, ok := captureID(r)
	if !ok {
		writeErr(w, 400, "bad capture id")
		return
	}
	idx, err := strconv.Atoi(r.PathValue("idx"))
	if err != nil {
		writeErr(w, 400, "bad packet index")
		return
	}
	raw, err := s.store.RawPacket(r.Context(), id, idx)
	if errors.Is(err, store.ErrNotFound) {
		writeErr(w, 404, "packet not found")
		return
	}
	if err != nil {
		writeErr(w, 500, err.Error())
		return
	}
	// Re-encode through json.RawMessage so the response stays valid JSON.
	w.Header().Set("Content-Type", "application/json")
	_, _ = io.WriteString(w, raw)
}
