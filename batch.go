package sling

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"mime"
	"mime/multipart"
	"net/http"
	"net/textproto"
	"net/url"
	"sort"
	"strings"
)

// BatchRequest is a single read-only query to send as one part of a
// multipart/mixed batch request. ID identifies the part and may only contain
// letters, digits, '.', '-' and '_'. Request must be a same-origin HTTP(S)
// GET or HEAD request without a body. Success and Failure behave like the
// Receive arguments: a 2XX sub-response is decoded into Success, any other
// status into Failure, and a nil value skips decoding.
type BatchRequest struct {
	ID      string
	Request *http.Request
	Success interface{}
	Failure interface{}
}

// BatchPart is the outcome of one BatchRequest. Response is the HTTP
// sub-response for the part, with its Status, Header, and Request available
// for inspection. Its Body has been consumed and closed by ReceiveBatch.
// Err holds a decode error for this part only; a non-2XX status is a
// business result decoded into Failure, not an error.
type BatchPart struct {
	ID       string
	Response *http.Response
	Err      error
}

// BatchResult is the result of a ReceiveBatch call. Response is the outer
// batch HTTP response (its Body already consumed and closed). Parts holds
// one BatchPart per BatchRequest, in input order.
type BatchResult struct {
	Response *http.Response
	Parts    []BatchPart
}

// ReceiveBatch sends the given read-only queries as a single multipart/mixed
// POST to the Sling's URL, using the Sling's query structs, headers, and
// Doer. Each part is an application/http message holding one sub-request.
//
// The response must be multipart/mixed with one application/http part per
// request ID (in any order). After the whole structure is validated, each
// sub-response is decoded with the Sling's ResponseDecoder into the part's
// Success (2XX) or Failure (non-2XX) value, in input order. Decoding is
// skipped for HEAD requests, 204 or empty bodies, and nil targets. A decode
// failure is recorded on that BatchPart's Err only; the remaining parts are
// still processed.
//
// Transport failures, a non-2XX outer status, and malformed multipart
// responses are protocol errors: ReceiveBatch returns a nil *BatchResult
// and a non-nil error, and no Success or Failure value is modified.
// ReceiveBatch does not modify the Sling, the sub-requests, their URLs, or
// their Headers, so a batch may be reused across calls.
func (s *Sling) ReceiveBatch(parts []BatchRequest) (*BatchResult, error) {
	req, err := s.newBatchRequest(parts)
	if err != nil {
		return nil, err
	}
	resp, err := s.httpClient.Do(req)
	if err != nil {
		return nil, err
	}
	// when err is nil, resp contains a non-nil resp.Body which must be closed
	defer resp.Body.Close()
	defer io.Copy(io.Discard, resp.Body)

	if resp.StatusCode < 200 || resp.StatusCode > 299 {
		return nil, fmt.Errorf("sling: batch request failed: %s", resp.Status)
	}
	mediaType, params, err := mime.ParseMediaType(resp.Header.Get("Content-Type"))
	if err != nil || mediaType != "multipart/mixed" || params["boundary"] == "" {
		return nil, fmt.Errorf("sling: batch response Content-Type %q is not multipart/mixed with a boundary", resp.Header.Get("Content-Type"))
	}
	boundary := params["boundary"]
	raw, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, fmt.Errorf("sling: reading batch response body: %w", err)
	}
	closing := []byte("--" + boundary + "--")
	if !bytes.Contains(raw, closing) {
		return nil, fmt.Errorf("sling: batch response is missing its closing MIME boundary")
	}
	responses, err := readBatchResponses(raw, boundary, parts)
	if err != nil {
		return nil, err
	}

	result := &BatchResult{Response: resp, Parts: make([]BatchPart, len(parts))}
	for i := range parts {
		sub := responses[i]
		part := BatchPart{ID: parts[i].ID, Response: sub}
		if shouldDecodeBatchPart(parts[i].Request.Method, sub) {
			if err := decodeResponse(sub, s.responseDecoder, parts[i].Success, parts[i].Failure); err != nil {
				part.Err = fmt.Errorf("sling: decoding batch part %q: %w", parts[i].ID, err)
			}
		}
		sub.Body.Close()
		result.Parts[i] = part
	}
	return result, nil
}

// newBatchRequest validates the batch parts and builds the outer
// multipart/mixed POST request. Nothing is sent and neither the Sling nor
// the sub-requests are modified.
func (s *Sling) newBatchRequest(parts []BatchRequest) (*http.Request, error) {
	if s.bodyProvider != nil {
		return nil, fmt.Errorf("sling: ReceiveBatch cannot be combined with a Sling body provider")
	}
	if len(parts) == 0 {
		return nil, fmt.Errorf("sling: ReceiveBatch requires at least one part")
	}
	baseURL, err := url.Parse(s.rawURL)
	if err != nil {
		return nil, fmt.Errorf("sling: parsing batch URL: %w", err)
	}
	seen := make(map[string]bool, len(parts))
	for i := range parts {
		part := &parts[i]
		if !validBatchID(part.ID) {
			return nil, fmt.Errorf("sling: invalid batch ID %q: only letters, digits, '.', '-' and '_' are allowed", part.ID)
		}
		if seen[part.ID] {
			return nil, fmt.Errorf("sling: duplicate batch ID %q", part.ID)
		}
		seen[part.ID] = true
		r := part.Request
		if r == nil || r.URL == nil {
			return nil, fmt.Errorf("sling: batch part %q has no request URL", part.ID)
		}
		if r.Method != http.MethodGet && r.Method != http.MethodHead {
			return nil, fmt.Errorf("sling: batch part %q uses method %q, only GET and HEAD are allowed", part.ID, r.Method)
		}
		if r.Body != nil && r.Body != http.NoBody {
			return nil, fmt.Errorf("sling: batch part %q must not have a request body", part.ID)
		}
		if err := checkBatchOrigin(baseURL, r.URL); err != nil {
			return nil, fmt.Errorf("sling: batch part %q: %w", part.ID, err)
		}
	}

	var buf bytes.Buffer
	writer := multipart.NewWriter(&buf)
	for i := range parts {
		header := textproto.MIMEHeader{}
		header.Set("Content-Type", "application/http")
		header.Set("Content-ID", "<"+parts[i].ID+">")
		partWriter, err := writer.CreatePart(header)
		if err != nil {
			return nil, err
		}
		writeBatchSubRequest(partWriter, parts[i].Request)
	}
	if err := writer.Close(); err != nil {
		return nil, err
	}

	reqURL := *baseURL
	if err := addQueryStructs(&reqURL, s.queryStructs); err != nil {
		return nil, err
	}
	req, err := http.NewRequest(http.MethodPost, reqURL.String(), bytes.NewReader(buf.Bytes()))
	if err != nil {
		return nil, err
	}
	addHeaders(req, s.header)
	req.Header.Set("Content-Type", "multipart/mixed; boundary="+writer.Boundary())
	return req, nil
}

// validBatchID reports whether id only contains letters, digits, '.', '-'
// and '_'.
func validBatchID(id string) bool {
	if id == "" {
		return false
	}
	for _, c := range id {
		switch {
		case 'a' <= c && c <= 'z', 'A' <= c && c <= 'Z', '0' <= c && c <= '9', c == '.', c == '-', c == '_':
		default:
			return false
		}
	}
	return true
}

// checkBatchOrigin ensures the sub-request URL is HTTP(S) and same-origin
// with the batch endpoint URL.
func checkBatchOrigin(base, sub *url.URL) error {
	scheme := strings.ToLower(sub.Scheme)
	if scheme != "http" && scheme != "https" {
		return fmt.Errorf("URL %q is not an HTTP(S) URL", sub)
	}
	if !strings.EqualFold(base.Scheme, sub.Scheme) || batchHostPort(base) != batchHostPort(sub) {
		return fmt.Errorf("URL %q is not same-origin with the batch endpoint", sub)
	}
	return nil
}

// batchHostPort returns the lowercase host with an explicit port, filling in
// the default port for the scheme.
func batchHostPort(u *url.URL) string {
	port := u.Port()
	if port == "" {
		if strings.EqualFold(u.Scheme, "https") {
			port = "443"
		} else {
			port = "80"
		}
	}
	return strings.ToLower(u.Hostname()) + ":" + port
}

// writeBatchSubRequest writes an HTTP request message in origin-form: the
// request line with path and query, the Host, and the request's own headers.
func writeBatchSubRequest(w io.Writer, r *http.Request) {
	fmt.Fprintf(w, "%s %s HTTP/1.1\r\n", r.Method, r.URL.RequestURI())
	host := r.Host
	if host == "" {
		host = r.URL.Host
	}
	fmt.Fprintf(w, "Host: %s\r\n", host)
	keys := make([]string, 0, len(r.Header))
	for key := range r.Header {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	for _, key := range keys {
		for _, value := range r.Header[key] {
			fmt.Fprintf(w, "%s: %s\r\n", key, value)
		}
	}
	io.WriteString(w, "\r\n")
}

// batchContentID trims whitespace and one pair of angle brackets from a
// Content-ID header value.
func batchContentID(raw string) string {
	id := strings.TrimSpace(raw)
	if len(id) >= 2 && id[0] == '<' && id[len(id)-1] == '>' {
		id = id[1 : len(id)-1]
	}
	return id
}

// readBatchResponses parses and validates every part of a multipart/mixed
// batch response body. It returns the sub-responses aligned to the input
// parts order, with each Body buffered in memory. Any structural problem
// (wrong part type, unknown, duplicate, or missing Content-ID, malformed or
// truncated HTTP message) is an error.
func readBatchResponses(raw []byte, boundary string, parts []BatchRequest) ([]*http.Response, error) {
	indexByID := make(map[string]int, len(parts))
	for i := range parts {
		indexByID[parts[i].ID] = i
	}
	responses := make([]*http.Response, len(parts))
	seen := make(map[string]bool, len(parts))
	reader := multipart.NewReader(bytes.NewReader(raw), boundary)
	for {
		part, err := reader.NextPart()
		if err == io.EOF {
			break
		}
		if err != nil {
			return nil, fmt.Errorf("sling: malformed batch response MIME: %w", err)
		}
		mediaType, _, err := mime.ParseMediaType(part.Header.Get("Content-Type"))
		if err != nil || mediaType != "application/http" {
			return nil, fmt.Errorf("sling: batch response part Content-Type %q is not application/http", part.Header.Get("Content-Type"))
		}
		id := batchContentID(part.Header.Get("Content-ID"))
		index, ok := indexByID[id]
		if !ok {
			return nil, fmt.Errorf("sling: batch response part has unknown Content-ID %q", id)
		}
		if seen[id] {
			return nil, fmt.Errorf("sling: batch response has a duplicate part for Content-ID %q", id)
		}
		seen[id] = true
		// the sub-request method (HEAD) and status (204, 304) decide whether
		// a body follows, per HTTP semantics
		sub, err := http.ReadResponse(bufio.NewReader(part), parts[index].Request)
		if err != nil {
			return nil, fmt.Errorf("sling: parsing batch response part %q: %w", id, err)
		}
		body, err := io.ReadAll(sub.Body)
		sub.Body.Close()
		if err != nil {
			return nil, fmt.Errorf("sling: batch response part %q body is truncated: %w", id, err)
		}
		sub.Body = batchPartBody{bytes.NewReader(body)}
		responses[index] = sub
	}
	for i := range parts {
		if !seen[parts[i].ID] {
			return nil, fmt.Errorf("sling: batch response is missing a part for Content-ID %q", parts[i].ID)
		}
	}
	return responses, nil
}

// batchPartBody is a buffered sub-response body, consumed and closed by
// ReceiveBatch so callers never need to read or close it.
type batchPartBody struct {
	*bytes.Reader
}

// Close is a no-op; the buffered body needs no resources released.
func (batchPartBody) Close() error { return nil }

// shouldDecodeBatchPart reports whether a sub-response carries a body worth
// decoding: HEAD responses, 204s, and bodies known to be empty are skipped.
func shouldDecodeBatchPart(method string, resp *http.Response) bool {
	if method == http.MethodHead || resp.StatusCode == http.StatusNoContent || resp.ContentLength == 0 {
		return false
	}
	if body, ok := resp.Body.(batchPartBody); ok && body.Len() == 0 {
		return false
	}
	return true
}
