package httpcache

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"io/ioutil"
	"net/http"
	"net/http/httputil"
	"strings"
	"sync"
)

// varyCatalogMagic prefixes every multi-representation cache entry. Entries
// without it are single-response entries written by the default mode and keep
// being read by the legacy path.
var varyCatalogMagic = []byte("HTTPCCAT1\n")

// varyCondition records the exact request header values a representation was
// selected with. A field missing from the request has no entry at all, while a
// field present with an empty value maps to []string{""}, so the two can never
// be confused. Values keep their order and case; only surrounding spaces and
// tabs are trimmed from each value.
type varyCondition map[string][]string

// varyVariant is one stored representation of one URL.
type varyVariant struct {
	condition varyCondition
	blob      []byte // full httputil.DumpResponse bytes, body included
	gen       uint64 // request generation when the representation was published
}

// varyCatalog is everything stored under one cache key in VaryVariants mode.
// A URL keeps a single Vary field set at a time; wildcard means the entry must
// never be reused by a later request.
type varyCatalog struct {
	fields   []string // canonicalized, deduplicated field names
	wildcard bool
	gen      uint64 // generation of the newest published request
	variants []*varyVariant
}

// varyBarrier marks a request that went downstream holding a matched
// representation. When a newer request publishes a different Vary set, the
// barrier is invalidated so that a slow, older response cannot republish the
// old set afterwards.
type varyBarrier struct {
	gen uint64
}

// varyInflight coordinates requests in flight for one cache key. The mutexes
// only protect in-memory state; network calls and body reads happen without
// either lock held.
type varyInflight struct {
	mu       sync.Mutex
	nextGen  uint64
	barriers map[uint64]*varyBarrier
	pubMu    sync.Mutex // serializes load-modify-set publications on this key
}

func (t *Transport) varyState(key string) *varyInflight {
	t.varyMu.Lock()
	if t.varyInflight == nil {
		t.varyInflight = map[string]*varyInflight{}
	}
	in := t.varyInflight[key]
	if in == nil {
		in = &varyInflight{barriers: map[uint64]*varyBarrier{}}
		t.varyInflight[key] = in
	}
	t.varyMu.Unlock()
	return in
}

// varyBegin assigns the downstream ordering generation for a request before it
// touches the network.
func (t *Transport) varyBegin(key string) (*varyInflight, uint64) {
	in := t.varyState(key)
	in.mu.Lock()
	gen := in.nextGen
	in.nextGen++
	in.mu.Unlock()
	return in, gen
}

func (t *Transport) varyRegisterBarrier(key string, gen uint64) *varyBarrier {
	in := t.varyState(key)
	b := &varyBarrier{gen: gen}
	in.mu.Lock()
	in.barriers[gen] = b
	in.mu.Unlock()
	return b
}

// varyExpireBarriers invalidates every registered barrier when a new Vary set
// is published.
func (t *Transport) varyExpireBarriers(key string, b *varyBarrier) {
	in := t.varyState(key)
	in.mu.Lock()
	defer in.mu.Unlock()
	for gen := range in.barriers {
		delete(in.barriers, gen)
	}
}

// varyReleaseBarrier drops a barrier owned by a request that finished without
// triggering a set switch.
func (t *Transport) varyReleaseBarrier(key string, b *varyBarrier) {
	if b == nil {
		return
	}
	in := t.varyState(key)
	in.mu.Lock()
	if cur, ok := in.barriers[b.gen]; ok && cur == b {
		delete(in.barriers, b.gen)
	}
	in.mu.Unlock()
}

func (t *Transport) varyBarrierAlive(key string, b *varyBarrier) bool {
	if b == nil {
		return true
	}
	in := t.varyState(key)
	in.mu.Lock()
	defer in.mu.Unlock()
	cur, ok := in.barriers[b.gen]
	return ok && cur == b
}

// parseVaryFields interprets every Vary header line together: tokens are
// trimmed, empty ones ignored, field names are canonicalized and deduplicated
// case-insensitively. A lone "*" wins over everything else.
func parseVaryFields(h http.Header) (fields []string, wildcard bool) {
	seen := map[string]bool{}
	for _, token := range headerAllCommaSepValues(h, "Vary") {
		token = strings.TrimSpace(token)
		if token == "" {
			continue
		}
		if token == "*" {
			return nil, true
		}
		canonical := http.CanonicalHeaderKey(token)
		if seen[canonical] {
			continue
		}
		seen[canonical] = true
		fields = append(fields, canonical)
	}
	return fields, false
}

// requestVaryCondition captures the request values participating in matching
// for the given Vary fields.
func requestVaryCondition(req *http.Request, fields []string) varyCondition {
	cond := varyCondition{}
	for _, field := range fields {
		values, present := req.Header[field]
		if !present {
			continue
		}
		copied := make([]string, len(values))
		for i, v := range values {
			copied[i] = strings.Trim(v, " \t")
		}
		cond[field] = copied
	}
	return cond
}

func conditionsEqual(a, b varyCondition) bool {
	if len(a) != len(b) {
		return false
	}
	for field, av := range a {
		bv, ok := b[field]
		if !ok || len(av) != len(bv) {
			return false
		}
		for i := range av {
			if av[i] != bv[i] {
				return false
			}
		}
	}
	return true
}

func (c *varyCatalog) match(cond varyCondition) *varyVariant {
	for _, v := range c.variants {
		if conditionsEqual(v.condition, cond) {
			return v
		}
	}
	return nil
}

func sameVaryFields(a, b []string) bool {
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

// loadVaryCatalog returns the catalog stored under key, or nil for a miss, a
// legacy single-response entry or an unreadable/wildcard-free marker.
func loadVaryCatalog(cache Cache, key string) *varyCatalog {
	raw, ok := cache.Get(key)
	if !ok || !bytes.HasPrefix(raw, varyCatalogMagic) {
		return nil
	}
	catalog, err := decodeVaryCatalog(raw)
	if err != nil {
		return nil
	}
	return catalog
}

func encodeVaryCatalog(c *varyCatalog) []byte {
	var buf bytes.Buffer
	buf.Write(varyCatalogMagic)
	var flags uint64
	if c.wildcard {
		flags = 1
	}
	writeUvarint(&buf, flags)
	writeUvarint(&buf, c.gen)
	writeUvarint(&buf, uint64(len(c.fields)))
	for _, f := range c.fields {
		writeBytes(&buf, []byte(f))
	}
	writeUvarint(&buf, uint64(len(c.variants)))
	for _, v := range c.variants {
		writeUvarint(&buf, v.gen)
		writeUvarint(&buf, uint64(len(v.condition)))
		// deterministic field order for the condition map
		for _, field := range c.fields {
			values, ok := v.condition[field]
			if !ok {
				continue
			}
			writeBytes(&buf, []byte(field))
			writeUvarint(&buf, uint64(len(values)))
			for _, value := range values {
				writeBytes(&buf, []byte(value))
			}
		}
		writeUvarint(&buf, uint64(len(v.blob)))
		buf.Write(v.blob)
	}
	return buf.Bytes()
}

func decodeVaryCatalog(raw []byte) (*varyCatalog, error) {
	if !bytes.HasPrefix(raw, varyCatalogMagic) {
		return nil, errInvalidVaryCatalog
	}
	r := bytes.NewReader(raw[len(varyCatalogMagic):])
	flags, err := binary.ReadUvarint(r)
	if err != nil {
		return nil, err
	}
	gen, err := binary.ReadUvarint(r)
	if err != nil {
		return nil, err
	}
	c := &varyCatalog{wildcard: flags&1 != 0, gen: gen}
	fieldCount, err := binary.ReadUvarint(r)
	if err != nil {
		return nil, err
	}
	c.fields = make([]string, fieldCount)
	for i := range c.fields {
		name, err := readBytes(r)
		if err != nil {
			return nil, err
		}
		c.fields[i] = string(name)
	}
	variantCount, err := binary.ReadUvarint(r)
	if err != nil {
		return nil, err
	}
	c.variants = make([]*varyVariant, 0, variantCount)
	for i := uint64(0); i < variantCount; i++ {
		vGen, err := binary.ReadUvarint(r)
		if err != nil {
			return nil, err
		}
		condCount, err := binary.ReadUvarint(r)
		if err != nil {
			return nil, err
		}
		cond := varyCondition{}
		for j := uint64(0); j < condCount; j++ {
			name, err := readBytes(r)
			if err != nil {
				return nil, err
			}
			valueCount, err := binary.ReadUvarint(r)
			if err != nil {
				return nil, err
			}
			values := make([]string, valueCount)
			for k := range values {
				value, err := readBytes(r)
				if err != nil {
					return nil, err
				}
				values[k] = string(value)
			}
			cond[string(name)] = values
		}
		blobLen, err := binary.ReadUvarint(r)
		if err != nil {
			return nil, err
		}
		blob := make([]byte, blobLen)
		if _, err := io.ReadFull(r, blob); err != nil {
			return nil, err
		}
		c.variants = append(c.variants, &varyVariant{condition: cond, blob: blob, gen: vGen})
	}
	return c, nil
}

var errInvalidVaryCatalog = errors.New("invalid httpcache vary catalog entry")

func writeUvarint(buf *bytes.Buffer, v uint64) {
	var tmp [binary.MaxVarintLen64]byte
	n := binary.PutUvarint(tmp[:], v)
	buf.Write(tmp[:n])
}

func writeBytes(buf *bytes.Buffer, b []byte) {
	writeUvarint(buf, uint64(len(b)))
	buf.Write(b)
}

func readBytes(r *bytes.Reader) ([]byte, error) {
	n, err := binary.ReadUvarint(r)
	if err != nil {
		return nil, err
	}
	b := make([]byte, n)
	if _, err := io.ReadFull(r, b); err != nil {
		return nil, err
	}
	return b, nil
}

// cachedRepresentation is a matched catalog variant or validated legacy entry.
type cachedRepresentation struct {
	resp *http.Response
	blob []byte
}

// selectCached finds the representation usable for req under the entry stored
// at key. Legacy single-response entries are validated against their Vary
// header conservatively: anything the old format cannot distinguish (notably a
// missing field versus an explicit empty value, or multi-valued request
// headers) counts as a miss.
func (t *Transport) selectCached(key string, req *http.Request) *cachedRepresentation {
	raw, ok := t.Cache.Get(key)
	if !ok {
		return nil
	}
	if bytes.HasPrefix(raw, varyCatalogMagic) {
		catalog, err := decodeVaryCatalog(raw)
		if err != nil || catalog.wildcard {
			return nil
		}
		variant := catalog.match(requestVaryCondition(req, catalog.fields))
		if variant == nil {
			return nil
		}
		resp, err := http.ReadResponse(bufio.NewReader(bytes.NewReader(variant.blob)), req)
		if err != nil {
			return nil
		}
		return &cachedRepresentation{resp: resp, blob: variant.blob}
	}

	// Legacy single-response entry.
	resp, err := http.ReadResponse(bufio.NewReader(bytes.NewReader(raw)), req)
	if err != nil {
		return nil
	}
	fields, wildcard := parseVaryFields(resp.Header)
	if wildcard {
		return nil
	}
	for _, field := range fields {
		fake := "X-Varied-" + field
		recorded := resp.Header.Get(fake)
		values, present := req.Header[field]
		if !present {
			// The old writer only recorded non-empty values, so an absent
			// marker is consistent only with a missing request field.
			if recorded != "" {
				return nil
			}
			continue
		}
		// The old format stores just the first, untrimmed value and cannot
		// describe multi-valued or empty request headers.
		if len(values) != 1 || values[0] == "" {
			return nil
		}
		if recorded != strings.Trim(values[0], " \t") {
			return nil
		}
	}
	return &cachedRepresentation{resp: resp, blob: raw}
}

func (t *Transport) markCachedResponse(resp *http.Response) {
	if t.MarkCachedResponses {
		resp.Header.Set(XFromCache, "1")
	}
}

// roundTripVaryCatalog serves eligible GETs from the multi-representation
// catalog. All locking stays around short in-memory bookkeeping; neither
// downstream RoundTripper calls nor caller body reads happen under a lock.
func (t *Transport) roundTripVaryCatalog(req *http.Request) (*http.Response, error) {
	key := cacheKey(req)
	transport := t.Transport
	if transport == nil {
		transport = http.DefaultTransport
	}

	in, gen := t.varyBegin(key)

	cached := t.selectCached(key, req)
	var barrier *varyBarrier
	downstreamReq := req

	if cached != nil {
		freshness := getFreshness(cached.resp.Header, req.Header)
		if freshness == fresh {
			t.markCachedResponse(cached.resp)
			return cached.resp, nil
		}
		if freshness == stale {
			cloned := cloneRequest(req)
			etag := cached.resp.Header.Get("etag")
			if etag != "" && req.Header.Get("etag") == "" {
				cloned.Header.Set("if-none-match", etag)
			}
			lastModified := cached.resp.Header.Get("last-modified")
			if lastModified != "" && req.Header.Get("last-modified") == "" {
				cloned.Header.Set("if-modified-since", lastModified)
			}
			downstreamReq = cloned
			barrier = t.varyRegisterBarrier(key, gen)
		}
	} else if _, ok := parseCacheControl(req.Header)["only-if-cached"]; ok {
		return newGatewayTimeoutResponse(req), nil
	}

	resp, err := transport.RoundTrip(downstreamReq)
	if err != nil {
		t.varyReleaseBarrier(key, barrier)
		if cached != nil && canStaleOnError(cached.resp.Header, req.Header) {
			t.markCachedResponse(cached.resp)
			return cached.resp, nil
		}
		return nil, err
	}

	if resp.StatusCode == http.StatusNotModified {
		// A 304 only makes sense with the one representation we validated;
		// without a match the server sent it unsolicited and it carries no
		// body, so hand it back untouched and leave the cache alone.
		if cached == nil {
			return resp, nil
		}
		resp.Body.Close()
		merged, body, err := mergeNotModified(cached, resp)
		if err != nil {
			t.varyReleaseBarrier(key, barrier)
			return nil, err
		}
		merged.Request = req
		t.publishVary(in, key, gen, barrier, req, merged, body)
		t.markCachedResponse(merged)
		return merged, nil
	}

	if resp.StatusCode >= 500 && cached != nil && canStaleOnError(cached.resp.Header, req.Header) {
		resp.Body.Close()
		t.varyReleaseBarrier(key, barrier)
		t.markCachedResponse(cached.resp)
		return cached.resp, nil
	}

	if !canStore(parseCacheControl(req.Header), parseCacheControl(resp.Header)) {
		t.varyReleaseBarrier(key, barrier)
		t.Cache.Delete(key)
		return resp, nil
	}

	// Snapshot everything publication needs before handing the body to the
	// caller: later header or request mutations must not change what gets
	// stored, and a half-read body must never publish anything.
	storedHeader := cloneHeaderWithoutMark(resp.Header)
	reqHeader := cloneHeaderWithoutMark(req.Header)
	statusCode, status := resp.StatusCode, resp.Status
	resp.Body = &varyCachingReadCloser{
		R: resp.Body,
		OnEOF: func(body []byte) {
			storedResp := &http.Response{
				Status:        status,
				StatusCode:    statusCode,
				Proto:         "HTTP/1.1",
				ProtoMajor:    1,
				ProtoMinor:    1,
				Header:        storedHeader,
				ContentLength: int64(len(body)),
			}
			snapshotReq := cloneRequest(req)
			snapshotReq.Header = reqHeader
			t.publishVary(in, key, gen, barrier, snapshotReq, storedResp, body)
		},
	}
	return resp, nil
}

// mergeNotModified combines the 304's updated end-to-end headers with the
// cached body. The cached response bytes are re-parsed so the caller receives a
// fresh, unread body.
func mergeNotModified(cached *cachedRepresentation, notModified *http.Response) (*http.Response, []byte, error) {
	body, err := extractBody(cached.blob)
	if err != nil {
		return nil, nil, err
	}
	resp, err := http.ReadResponse(bufio.NewReader(bytes.NewReader(cached.blob)), nil)
	if err != nil {
		return nil, nil, err
	}
	for _, header := range getEndToEndHeaders(notModified.Header) {
		resp.Header[header] = notModified.Header[header]
	}
	resp.Header.Del(XFromCache)
	resp.Body = ioutil.NopCloser(bytes.NewReader(body))
	resp.ContentLength = int64(len(body))
	return resp, body, nil
}

func extractBody(blob []byte) ([]byte, error) {
	resp, err := http.ReadResponse(bufio.NewReader(bytes.NewReader(blob)), nil)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	return ioutil.ReadAll(resp.Body)
}

func cloneHeaderWithoutMark(h http.Header) http.Header {
	clone := make(http.Header, len(h))
	for k, vs := range h {
		copied := make([]string, len(vs))
		copy(copied, vs)
		clone[k] = copied
	}
	clone.Del(XFromCache)
	return clone
}

// publishVary stores one fully read representation. Same-set publications only
// replace the variant with the same request condition; a different set replaces
// the whole catalog, and only when a newer request did not already do so.
func (t *Transport) publishVary(in *varyInflight, key string, gen uint64, barrier *varyBarrier,
	req *http.Request, storedResp *http.Response, body []byte) {

	fields, wildcard := parseVaryFields(storedResp.Header)

	in.pubMu.Lock()
	defer in.pubMu.Unlock()

	if !t.varyBarrierAlive(key, barrier) {
		// A newer request already published a different Vary set; this older
		// response must not bring the old set back.
		return
	}

	current := loadVaryCatalog(t.Cache, key)
	catalog := &varyCatalog{fields: fields, wildcard: wildcard, gen: gen}

	if current != nil && !wildcard && !current.wildcard && sameVaryFields(current.fields, fields) {
		// Same field set: keep every other representation, replace only the
		// one with the same condition. A newer same-condition publication, if
		// one already landed, must not be downgraded by an older response
		// whose body finished reading later.
		cond := requestVaryCondition(req, fields)
		superseded := false
		for _, v := range current.variants {
			switch {
			case conditionsEqual(v.condition, cond) && v.gen > gen:
				superseded = true
			case conditionsEqual(v.condition, cond):
				// Drop the variant this publication replaces.
			default:
				catalog.variants = append(catalog.variants, v)
			}
		}
		if superseded {
			t.varyReleaseBarrier(key, barrier)
			return
		}
		if current.gen > gen {
			catalog.gen = current.gen
		}
	} else {
		// The Vary set changes (including a switch to "*"). A generation
		// check makes set switches follow downstream request ordering, even
		// when bodies finish reading out of order.
		if current != nil && gen <= current.gen {
			return
		}
	}

	if !wildcard {
		blob, err := dumpStoredResponse(storedResp, body)
		if err != nil {
			return
		}
		cond := requestVaryCondition(req, fields)
		catalog.variants = append(catalog.variants, &varyVariant{
			condition: cond,
			blob:      blob,
			gen:       gen,
		})
	}

	// Expire in-flight older revalidations before installing the new entry.
	if current == nil || wildcard || current.wildcard || !sameVaryFields(current.fields, fields) {
		t.varyExpireBarriers(key, barrier)
	} else {
		t.varyReleaseBarrier(key, barrier)
	}
	t.Cache.Set(key, encodeVaryCatalog(catalog))
}

func dumpStoredResponse(storedResp *http.Response, body []byte) ([]byte, error) {
	resp := *storedResp
	resp.Header = cloneHeaderWithoutMark(storedResp.Header)
	resp.Body = ioutil.NopCloser(bytes.NewReader(body))
	return httputil.DumpResponse(&resp, true)
}

// varyCachingReadCloser buffers a downstream body and only invokes OnEOF once
// the complete body has been read. An early Close or a read error publishes
// nothing, so no partial representation ever reaches the cache.
type varyCachingReadCloser struct {
	R     io.ReadCloser
	OnEOF func(body []byte)

	buf       bytes.Buffer
	published bool
}

func (r *varyCachingReadCloser) Read(p []byte) (int, error) {
	n, err := r.R.Read(p)
	if n > 0 {
		r.buf.Write(p[:n])
	}
	if err != nil {
		if err == io.EOF && !r.published {
			r.published = true
			r.OnEOF(r.buf.Bytes())
		} else if err != io.EOF {
			// A read failure forbids any later publication of a partial body.
			r.published = true
		}
	}
	return n, err
}

func (r *varyCachingReadCloser) Close() error {
	return r.R.Close()
}
