// Copyright 2014 Martin Angers and Contributors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package fetchbot

import (
	"bytes"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	// warcContentType is the WARC Content-Type of every captured record.
	warcContentType = "application/http; msgtype=response"
	// warcRevisitProfile is the WARC profile URI written on revisit records.
	warcRevisitProfile = "http://netpreserve.org/warc/1.1/revisit/identical-payload-digest"
	// warcRecordTrailer separates a record from the next one.
	warcRecordTrailer = "\r\n\r\n"
)

// WARCOptions configures a WARCHandler. The zero value is valid.
type WARCOptions struct {
	// TempDir is the directory used to spool response bodies while their
	// digest is computed. If empty, the system temporary directory is used.
	TempDir string
	// Now returns the time used to date the WARC records. If nil, time.Now
	// is used.
	Now func() time.Time
	// NewID returns a unique URI, without angle brackets, used as WARC record
	// ID. If nil, a new UUID URN is generated for each record.
	NewID func() string
}

// WARCHandler is a Handler that archives every error-free GET response as an
// uncompressed WARC/1.1 record, deduplicating identical payloads as revisit
// records. It is safe for concurrent use by the Fetcher's host workers.
//
// The handler never closes the response Body (the Fetcher closes it once) and
// never closes the data and index writers. After the Queue is closed, Err
// reports the first error encountered, if any.
type WARCHandler struct {
	data  io.Writer
	index io.Writer

	tempDir string
	now     func() time.Time
	newID   func() string

	mu      sync.Mutex
	err     error
	offset  int64
	digests map[string]warcIdentity
}

// warcIdentity is the deduplication identity of the first complete response
// record published for a payload digest.
type warcIdentity struct {
	id     string
	target string
	date   string
}

// warcIndexEntry is a single line of the JSON index.
type warcIndexEntry struct {
	ID            string `json:"id"`
	URL           string `json:"url"`
	Date          string `json:"date"`
	Offset        int64  `json:"offset"`
	Length        int64  `json:"length"`
	PayloadDigest string `json:"payload_digest"`
	RevisitOf     string `json:"revisit_of"`
}

// NewWARCHandler creates a WARCHandler writing WARC records to data and JSON
// index lines to index. Both writers must be non-nil and are expected to be
// empty output streams; they are not closed by the handler.
func NewWARCHandler(data, index io.Writer, opts WARCOptions) (*WARCHandler, error) {
	if data == nil || index == nil {
		return nil, errors.New("fetchbot: WARCHandler requires non-nil data and index writers")
	}
	if opts.TempDir == "" {
		opts.TempDir = os.TempDir()
	}
	if opts.Now == nil {
		opts.Now = time.Now
	}
	if opts.NewID == nil {
		opts.NewID = newWARCID
	}
	return &WARCHandler{
		data:    data,
		index:   index,
		tempDir: opts.TempDir,
		now:     opts.Now,
		newID:   opts.NewID,
		digests: make(map[string]warcIdentity),
	}, nil
}

// newWARCID returns a new random UUID URN.
func newWARCID() string {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		panic(err)
	}
	b[6] = (b[6] & 0x0f) | 0x40 // version 4
	b[8] = (b[8] & 0x3f) | 0x80 // variant 10
	return fmt.Sprintf("urn:uuid:%x-%x-%x-%x-%x", b[0:4], b[4:6], b[6:8], b[8:10], b[10:])
}

// Err returns the first error encountered by the handler, if any. Once an
// error is recorded, the handler stops archiving and stops consuming response
// bodies.
func (h *WARCHandler) Err() error {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.err
}

// setErr records err if it is the first error.
func (h *WARCHandler) setErr(err error) {
	h.mu.Lock()
	if h.err == nil {
		h.err = err
	}
	h.mu.Unlock()
}

// fail records err if it is the first error, and reports whether err is
// non-nil. h.mu must be held.
func (h *WARCHandler) fail(err error) bool {
	if err == nil {
		return false
	}
	if h.err == nil {
		h.err = err
	}
	return true
}

// Handle implements the Handler interface.
func (h *WARCHandler) Handle(ctx *Context, res *http.Response, err error) {
	if err != nil {
		// A fetch error is recorded as-is.
		h.setErr(err)
		return
	}
	if h.Err() != nil {
		// A previous failure stops all archiving; do not consume the body.
		return
	}
	if ctx == nil || ctx.Cmd == nil || ctx.Cmd.Method() != http.MethodGet {
		// Only GET responses are archived.
		return
	}
	if res == nil || res.Body == nil {
		h.setErr(errors.New("fetchbot: GET command delivered no valid response"))
		return
	}
	var target string
	if res.Request != nil && res.Request.URL != nil {
		target = res.Request.URL.String()
	}
	if target == "" {
		if u := ctx.Cmd.URL(); u != nil {
			target = u.String()
		}
	}
	if target == "" {
		h.setErr(errors.New("fetchbot: cannot determine the target URI of the response"))
		return
	}
	h.capture(res, target)
}

// capture archives a single response. The body is first spooled to a
// temporary file so that memory use does not grow with the body size; the
// spool file is removed whether the capture succeeds or fails.
func (h *WARCHandler) capture(res *http.Response, target string) {
	path, bodyLen, payloadDigest, err := h.spool(res.Body)
	if err != nil {
		h.setErr(err)
		return
	}
	defer os.Remove(path)

	blockHead := httpBlockHead(res, bodyLen)
	// The block digest depends on the record type, which is only known once
	// the write lock is held; both variants are cheap to compute upfront.
	fullDigest, err := blockDigest(path, blockHead, true)
	if err != nil {
		h.setErr(err)
		return
	}
	headDigest, err := blockDigest(path, blockHead, false)
	if err != nil {
		h.setErr(err)
		return
	}

	// Record appends and digest identity publishing are serialized.
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.err != nil {
		// Another in-flight capture failed while this one was prepared.
		return
	}

	id := h.newID()
	date := h.now().UTC().Format(time.RFC3339Nano)
	first, isRevisit := h.digests[payloadDigest]

	var recType, blockDigestSum string
	var blockLen int64
	if isRevisit {
		recType = "revisit"
		blockDigestSum = headDigest
		blockLen = int64(len(blockHead))
	} else {
		recType = "response"
		blockDigestSum = fullDigest
		blockLen = int64(len(blockHead)) + bodyLen
	}

	var wh bytes.Buffer
	wh.WriteString("WARC/1.1\r\n")
	wh.WriteString("WARC-Type: " + recType + "\r\n")
	wh.WriteString("WARC-Record-ID: <" + id + ">\r\n")
	wh.WriteString("WARC-Date: " + date + "\r\n")
	wh.WriteString("WARC-Target-URI: " + target + "\r\n")
	if isRevisit {
		wh.WriteString("WARC-Profile: " + warcRevisitProfile + "\r\n")
		wh.WriteString("WARC-Refers-To: <" + first.id + ">\r\n")
		wh.WriteString("WARC-Refers-To-Target-URI: " + first.target + "\r\n")
		wh.WriteString("WARC-Refers-To-Date: " + first.date + "\r\n")
		wh.WriteString("WARC-Truncated: length\r\n")
	}
	wh.WriteString("WARC-Payload-Digest: " + payloadDigest + "\r\n")
	wh.WriteString("WARC-Block-Digest: " + blockDigestSum + "\r\n")
	wh.WriteString("Content-Type: " + warcContentType + "\r\n")
	wh.WriteString("Content-Length: " + strconv.FormatInt(blockLen, 10) + "\r\n")
	wh.WriteString("\r\n")

	offset := h.offset
	recLen := int64(wh.Len()) + blockLen + int64(len(warcRecordTrailer))

	if h.fail(writeFull(h.data, wh.Bytes())) {
		return
	}
	if h.fail(writeFull(h.data, blockHead)) {
		return
	}
	if !isRevisit && bodyLen > 0 {
		if h.fail(h.copyBody(path, bodyLen)) {
			return
		}
	}
	if h.fail(writeFull(h.data, []byte(warcRecordTrailer))) {
		return
	}
	h.offset += recLen

	// The index line is written only once the full record made it to the
	// data output.
	revisitOf := ""
	if isRevisit {
		revisitOf = first.id
	}
	line, err := json.Marshal(warcIndexEntry{
		ID:            id,
		URL:           target,
		Date:          date,
		Offset:        offset,
		Length:        recLen,
		PayloadDigest: payloadDigest,
		RevisitOf:     revisitOf,
	})
	if h.fail(err) {
		return
	}
	if h.fail(writeFull(h.index, append(line, '\n'))) {
		return
	}

	// Both outputs succeeded: publish the deduplication identity.
	if !isRevisit {
		h.digests[payloadDigest] = warcIdentity{id: id, target: target, date: date}
	}
}

// spool copies r to a temporary file, computing the SHA-256 payload digest on
// the fly. It returns the file path, the number of bytes copied and the
// digest. On error the temporary file is removed.
func (h *WARCHandler) spool(r io.Reader) (path string, n int64, digest string, err error) {
	f, err := os.CreateTemp(h.tempDir, "fetchbot-warc-*")
	if err != nil {
		return "", 0, "", err
	}
	hw := sha256.New()
	n, err = io.Copy(io.MultiWriter(f, hw), r)
	if err != nil {
		f.Close()
		os.Remove(f.Name())
		return "", 0, "", err
	}
	if err = f.Close(); err != nil {
		os.Remove(f.Name())
		return "", 0, "", err
	}
	return f.Name(), n, "sha256:" + hex.EncodeToString(hw.Sum(nil)), nil
}

// copyBody streams n bytes of the spooled body file to the data writer.
func (h *WARCHandler) copyBody(path string, n int64) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	_, err = io.CopyN(h.data, f, n)
	return err
}

// blockDigest computes the SHA-256 digest of a record block: the HTTP block
// header, optionally followed by the spooled body.
func blockDigest(path string, head []byte, withBody bool) (string, error) {
	hw := sha256.New()
	hw.Write(head)
	if withBody {
		f, err := os.Open(path)
		if err != nil {
			return "", err
		}
		_, err = io.Copy(hw, f)
		f.Close()
		if err != nil {
			return "", err
		}
	}
	return "sha256:" + hex.EncodeToString(hw.Sum(nil)), nil
}

// httpBlockHead builds the HTTP part of a record block: the status line
// followed by the response headers and a blank line. Multi-value headers are
// preserved, transfer chunking information is dropped, Content-Length is
// rewritten to the actual body length, and Content-Encoding is dropped when
// the response was transparently decompressed. The response's own Header map
// is left untouched.
func httpBlockHead(res *http.Response, bodyLen int64) []byte {
	var b bytes.Buffer
	proto := res.Proto
	if proto == "" {
		proto = "HTTP/1.1"
	}
	status := strings.TrimSpace(res.Status)
	if status == "" {
		status = strings.TrimSpace(strconv.Itoa(res.StatusCode) + " " + http.StatusText(res.StatusCode))
	}
	b.WriteString(proto + " " + status + "\r\n")

	keys := make([]string, 0, len(res.Header))
	for k := range res.Header {
		switch http.CanonicalHeaderKey(k) {
		case "Transfer-Encoding", "Content-Length":
			// Dropped: chunking is removed and Content-Length is rewritten.
		case "Content-Encoding":
			if !res.Uncompressed {
				keys = append(keys, k)
			}
		default:
			keys = append(keys, k)
		}
	}
	sort.Strings(keys)
	for _, k := range keys {
		for _, v := range res.Header[k] {
			b.WriteString(k + ": " + v + "\r\n")
		}
	}
	b.WriteString("Content-Length: " + strconv.FormatInt(bodyLen, 10) + "\r\n")
	b.WriteString("\r\n")
	return b.Bytes()
}

// writeFull writes p to w, treating a short write without an error as
// io.ErrShortWrite.
func writeFull(w io.Writer, p []byte) error {
	n, err := w.Write(p)
	if err != nil {
		return err
	}
	if n != len(p) {
		return io.ErrShortWrite
	}
	return nil
}

var _ Handler = (*WARCHandler)(nil)
