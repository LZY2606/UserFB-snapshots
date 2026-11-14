package gowebdav

import (
	"bytes"
	"context"
	"encoding/xml"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"sort"
	"strconv"
	"strings"
)

// SyncResource is a single member collected by CollectSync.
type SyncResource struct {
	// ETag is the strong ETag of a file. Directories leave it empty.
	ETag string
	// IsDir reports whether the member is a collection.
	IsDir bool
	// Data holds the full file body. It is nil for directories.
	Data []byte
}

// SyncSnapshot is the result of a successful CollectSync run.
//
// Collection and the keys of Resources are absolute URLs.
type SyncSnapshot struct {
	// Collection is the request URL of the synced collection,
	// including its trailing slash.
	Collection string
	// Token is the server supplied sync-token of this snapshot.
	Token string
	// Resources maps member URLs to their current state.
	Resources map[string]SyncResource
}

// syncChange is a collected member state before bodies are downloaded.
type syncChange struct {
	etag    string
	isDir   bool
	deleted bool
}

type syncMultistatus struct {
	XMLName   xml.Name       `xml:"DAV: multistatus"`
	Token     string         `xml:"DAV: sync-token"`
	Responses []syncResponse `xml:"DAV: response"`
}

type syncResponse struct {
	Href     string           `xml:"DAV: href"`
	Status   string           `xml:"DAV: status"`
	Error    *syncResponseErr `xml:"DAV: error"`
	PropStat []syncPropStat   `xml:"DAV: propstat"`
}

type syncResponseErr struct {
	NumberOfMatches *struct{} `xml:"DAV: number-of-matches-within-limits,omitempty"`
}

type syncPropStat struct {
	Status string   `xml:"DAV: status"`
	Prop   syncProp `xml:"DAV: prop"`
}

type syncProp struct {
	ETag         string           `xml:"DAV: getetag"`
	ResourceType syncResourceType `xml:"DAV: resourcetype"`
}

type syncResourceType struct {
	Collection *struct{} `xml:"DAV: collection,omitempty"`
}

type syncErrorEnvelope struct {
	XMLName        xml.Name  `xml:"DAV: error"`
	ValidSyncToken *struct{} `xml:"DAV: valid-sync-token,omitempty"`
}

// CollectSync performs an RFC 6578 sync-collection REPORT against the
// collection named name (relative to the client root, interpreted the same
// way as in ReadDir) and returns a snapshot of its direct members.
//
// A nil previous snapshot triggers a full initial collection. Otherwise the
// previous successful snapshot is merged with the reported changes. The
// caller must keep previous read-only for the duration of the call. On
// failure the returned snapshot is nil and the input snapshot is untouched.
//
// pageSize limits the number of results per REPORT request and must be
// positive. Only the collection itself is inspected; sub collections are not
// descended into.
func (c *Client) CollectSync(ctx context.Context, name string, previous *SyncSnapshot, pageSize int) (*SyncSnapshot, error) {
	const op = "CollectSync"

	if pageSize <= 0 {
		return nil, fmt.Errorf("%s: pageSize must be positive, got %d", op, pageSize)
	}

	collectionURL, err := url.Parse(PathEscape(Join(c.root, FixSlashes(name))))
	if err != nil {
		return nil, NewPathErrorErr(op, name, err)
	}
	collection := collectionURL.String()

	if previous != nil {
		if previous.Collection != collection {
			return nil, fmt.Errorf("%s: snapshot collection %q does not match target collection %q", op, previous.Collection, collection)
		}
		if previous.Token == "" {
			return nil, fmt.Errorf("%s: snapshot token is empty", op)
		}
	}

	changes := make(map[string]syncChange)
	seenTokens := make(map[string]bool)
	token := ""
	relisted := false
	if previous != nil {
		token = previous.Token
	}

	for {
		if err := ctx.Err(); err != nil {
			return nil, NewPathErrorErr(op, collection, err)
		}
		if seenTokens[token] {
			return nil, fmt.Errorf("%s: sync-token loop detected at %q", op, token)
		}
		seenTokens[token] = true

		body := buildSyncReport(token, pageSize)
		rs, err := c.reqCtx(ctx, "REPORT", collection, bytes.NewReader(body), func(r *http.Request) {
			r.Header.Set("Depth", "0")
			r.Header.Set("Content-Type", "application/xml; charset=utf-8")
			r.Header.Set("Accept", "application/xml,text/xml")
		})
		if err != nil {
			return nil, NewPathErrorErr(op, collection, err)
		}

		if rs.StatusCode != http.StatusMultiStatus {
			// A 403 carrying DAV:valid-sync-token means the old token is
			// gone; one full re-collection from the empty token is allowed.
			shouldRelist := rs.StatusCode == http.StatusForbidden && previous != nil && !relisted &&
				hasValidSyncTokenError(rs.Body)
			closeAndDiscard(rs.Body)
			if !shouldRelist {
				return nil, NewPathError(op, collection, rs.StatusCode)
			}
			relisted = true
			changes = make(map[string]syncChange)
			seenTokens = make(map[string]bool)
			token = ""
			continue
		}

		page, nextToken, truncated, err := parseSyncPage(rs.Body, collectionURL)
		closeErr := rs.Body.Close()
		if err != nil {
			return nil, NewPathErrorErr(op, collection, err)
		}
		if closeErr != nil {
			return nil, NewPathErrorErr(op, collection, closeErr)
		}

		if err := applySyncPage(changes, page, collectionURL); err != nil {
			return nil, NewPathErrorErr(op, collection, err)
		}

		token = nextToken
		if !truncated {
			break
		}
	}

	resources := make(map[string]SyncResource)
	if previous != nil && !relisted {
		resources = cloneSyncResources(previous.Resources)
	}
	for key, change := range changes {
		if change.deleted {
			delete(resources, key)
			continue
		}
		resources[key] = SyncResource{ETag: change.etag, IsDir: change.isDir}
	}

	if err := c.collectSyncBodies(ctx, resources, previous); err != nil {
		return nil, err
	}

	return &SyncSnapshot{
		Collection: collection,
		Token:      token,
		Resources:  resources,
	}, nil
}

// reqCtx issues a request through the client's transport, authentication,
// configured headers and interceptor, bound to ctx. Redirects are never
// followed so a sync-token cannot be carried into another collection.
func (c *Client) reqCtx(ctx context.Context, method, uri string, body io.Reader, intercept func(*http.Request)) (*http.Response, error) {
	auth, body := c.auth.NewAuthenticator(body)
	defer auth.Close()

	var r *http.Request
	var rs *http.Response
	var err error
	for {
		if r, err = http.NewRequestWithContext(ctx, method, uri, body); err != nil {
			return nil, err
		}

		for k, vals := range c.headers {
			for _, v := range vals {
				r.Header.Add(k, v)
			}
		}

		if err = auth.Authorize(c.c, r, r.URL.RequestURI()); err != nil {
			return nil, err
		}

		r.Header.Set(XInhibitRedirect, "1")

		if intercept != nil {
			intercept(r)
		}
		if c.interceptor != nil {
			c.interceptor(method, r)
		}

		if rs, err = c.c.Do(r); err != nil {
			return nil, err
		}

		redo, verr := auth.Verify(c.c, rs, r.URL.RequestURI())
		if verr != nil {
			rs.Body.Close()
			return nil, verr
		}
		if redo && rs.StatusCode >= 300 && rs.StatusCode < 400 {
			// CollectSync never follows redirects, not even through an
			// authenticator's negotiation loop.
			closeAndDiscard(rs.Body)
			return nil, fmt.Errorf("%s %s: redirect %d must not be followed", method, uri, rs.StatusCode)
		}
		if !redo {
			return rs, nil
		}

		rs.Body.Close()
		if body, err = r.GetBody(); err != nil {
			return nil, err
		}
	}
}

func buildSyncReport(token string, pageSize int) []byte {
	var buf bytes.Buffer
	buf.WriteString(`<?xml version="1.0" encoding="utf-8"?>`)
	buf.WriteString(`<D:sync-collection xmlns:D="DAV:">`)
	if token == "" {
		buf.WriteString(`<D:sync-token/>`)
	} else {
		buf.WriteString(`<D:sync-token>`)
		_ = xml.EscapeText(&buf, []byte(token))
		buf.WriteString(`</D:sync-token>`)
	}
	buf.WriteString(`<D:sync-level>1</D:sync-level>`)
	buf.WriteString(`<D:prop><D:getetag/><D:resourcetype/></D:prop>`)
	fmt.Fprintf(&buf, `<D:limit><D:nresults>%d</D:nresults></D:limit>`, pageSize)
	buf.WriteString(`</D:sync-collection>`)
	return buf.Bytes()
}

// parseSyncPage reads a 207 multistatus body completely and returns the
// member responses, the unique sync-token and the truncation marker carried
// by the collection itself.
func parseSyncPage(r io.Reader, collection *url.URL) ([]syncResponse, string, bool, error) {
	data, err := io.ReadAll(r)
	if err != nil {
		return nil, "", false, err
	}

	var multi syncMultistatus
	if err := xml.Unmarshal(data, &multi); err != nil {
		return nil, "", false, err
	}
	if multi.XMLName.Space != "DAV:" || multi.XMLName.Local != "multistatus" {
		return nil, "", false, fmt.Errorf("expected DAV:multistatus root element")
	}

	tokenCount, err := countSyncTokens(data)
	if err != nil {
		return nil, "", false, err
	}
	if tokenCount != 1 || strings.TrimSpace(multi.Token) == "" {
		return nil, "", false, fmt.Errorf("expected exactly one non-empty DAV:sync-token, got %d", tokenCount)
	}

	truncated := false
	for _, resp := range multi.Responses {
		self, err := isCollectionHref(resp.Href, collection)
		if err != nil {
			return nil, "", false, err
		}
		if !self {
			continue
		}
		code, err := responseStatus(resp)
		if err != nil {
			return nil, "", false, err
		}
		switch {
		case code == http.StatusOK:
			// The collection itself may be reported on the final page.
			continue
		case code == http.StatusInsufficientStorage &&
			resp.Error != nil && resp.Error.NumberOfMatches != nil:
			truncated = true
		default:
			return nil, "", false, fmt.Errorf("unexpected status %d on collection itself", code)
		}
	}

	return multi.Responses, strings.TrimSpace(multi.Token), truncated, nil
}

// responseStatus reads the response-level status, falling back to its
// propstat statuses when the server omitted a top-level status line.
func responseStatus(resp syncResponse) (int, error) {
	if strings.TrimSpace(resp.Status) != "" {
		return parseHTTPStatus(resp.Status)
	}
	code := 0
	for _, ps := range resp.PropStat {
		c, err := parseHTTPStatus(ps.Status)
		if err != nil {
			return 0, err
		}
		if c != http.StatusOK {
			return c, nil
		}
		code = c
	}
	if code == 0 {
		return 0, fmt.Errorf("response for %q has no status", resp.Href)
	}
	return code, nil
}

func countSyncTokens(data []byte) (int, error) {
	dec := xml.NewDecoder(bytes.NewReader(data))
	depth := 0
	count := 0
	for {
		t, err := dec.Token()
		if err == io.EOF {
			return count, nil
		}
		if err != nil {
			return 0, err
		}
		switch se := t.(type) {
		case xml.StartElement:
			if depth == 1 && se.Name.Space == "DAV:" && se.Name.Local == "sync-token" {
				count++
			}
			depth++
		case xml.EndElement:
			depth--
		}
	}
}

// applySyncPage merges one page into the change set. Later pages override
// earlier ones; duplicate keys within one page must carry identical state.
func applySyncPage(changes map[string]syncChange, responses []syncResponse, collection *url.URL) error {
	pageEntries := make(map[string]string)
	for _, resp := range responses {
		if strings.TrimSpace(resp.Href) == "" {
			return fmt.Errorf("response without DAV:href")
		}

		member, err := collection.Parse(resp.Href)
		if err != nil {
			return fmt.Errorf("cannot parse href %q: %w", resp.Href, err)
		}
		self, err := isCollectionHref(resp.Href, collection)
		if err != nil {
			return err
		}
		if self {
			// The collection itself only carries the truncation marker.
			continue
		}
		if err := validateMemberURL(member, collection); err != nil {
			return err
		}

		key := member.String()
		code := 0
		if strings.TrimSpace(resp.Status) != "" {
			code, err = parseHTTPStatus(resp.Status)
			if err != nil {
				return err
			}
		}

		var change syncChange
		signature := ""
		switch {
		case code == http.StatusNotFound:
			change = syncChange{deleted: true}
			signature = "404"
		case code != 0 && code != http.StatusOK:
			return fmt.Errorf("member %q returned unexpected status %d", key, code)
		default:
			etag, dir, err := successfulProps(resp.PropStat)
			if err != nil {
				return fmt.Errorf("member %q: %w", key, err)
			}
			if dir {
				change = syncChange{isDir: true}
				signature = "200|dir"
			} else {
				if etag == "" {
					return fmt.Errorf("file %q is missing getetag", key)
				}
				if strings.HasPrefix(etag, "W/") || !strings.HasPrefix(etag, `"`) {
					return fmt.Errorf("file %q has no strong ETag: %q", key, etag)
				}
				change = syncChange{etag: etag}
				signature = "200|file|" + etag
			}
		}

		if previous, ok := pageEntries[key]; ok {
			if previous != signature {
				return fmt.Errorf("member %q appears twice in one page with conflicting state", key)
			}
			continue
		}
		pageEntries[key] = signature
		changes[key] = change
	}

	return nil
}

// successfulProps extracts the properties of the successful propstat
// elements. Conflicting successful propstat elements are rejected.
func successfulProps(propstats []syncPropStat) (etag string, isDir bool, err error) {
	found := false
	for _, ps := range propstats {
		code, perr := parseHTTPStatus(ps.Status)
		if perr != nil {
			return "", false, perr
		}
		if code != http.StatusOK {
			continue
		}
		dir := ps.Prop.ResourceType.Collection != nil
		if found && (dir != isDir || ps.Prop.ETag != etag) {
			return "", false, fmt.Errorf("conflicting successful propstat elements")
		}
		etag = ps.Prop.ETag
		isDir = dir
		found = true
	}
	if !found {
		return "", false, fmt.Errorf("no successful propstat")
	}
	return etag, isDir, nil
}

// isCollectionHref reports whether href resolves to the collection itself,
// distinguished by the collection's trailing slash.
func isCollectionHref(href string, collection *url.URL) (bool, error) {
	member, err := collection.Parse(href)
	if err != nil {
		return false, fmt.Errorf("cannot parse href %q: %w", href, err)
	}
	if member.User != nil || member.RawQuery != "" || member.Fragment != "" {
		return false, nil
	}
	return member.Scheme == collection.Scheme &&
		member.Host == collection.Host &&
		member.EscapedPath() == collection.EscapedPath(), nil
}

// validateMemberURL enforces same-origin direct members without userinfo,
// query or fragment. Paths are compared by escaped segments so an encoded
// %2F is never treated as a directory separator.
func validateMemberURL(member, collection *url.URL) error {
	if member.Scheme != collection.Scheme || member.Host != collection.Host {
		return fmt.Errorf("href %q is not same-origin with %q", member.String(), collection.String())
	}
	if member.User != nil {
		return fmt.Errorf("href %q must not contain userinfo", member.String())
	}
	if member.RawQuery != "" {
		return fmt.Errorf("href %q must not contain a query", member.String())
	}
	if member.Fragment != "" {
		return fmt.Errorf("href %q must not contain a fragment", member.String())
	}

	escaped := member.EscapedPath()
	prefix := collection.EscapedPath()
	if !strings.HasPrefix(escaped, prefix) {
		return fmt.Errorf("href %q is outside of collection %q", member.String(), collection.String())
	}
	remainder := escaped[len(prefix):]
	if strings.HasSuffix(remainder, "/") {
		remainder = remainder[:len(remainder)-1]
	}
	if remainder == "" || strings.Contains(remainder, "/") {
		return fmt.Errorf("href %q is not a direct member of %q", member.String(), collection.String())
	}
	if decoded, err := url.PathUnescape(remainder); err != nil || decoded == "." || decoded == ".." {
		return fmt.Errorf("href %q is not a direct member of %q", member.String(), collection.String())
	}
	return nil
}

// parseHTTPStatus parses a status line like "HTTP/1.1 200 OK".
func parseHTTPStatus(status string) (int, error) {
	fields := strings.Fields(status)
	if len(fields) < 2 {
		return 0, fmt.Errorf("invalid HTTP status %q", status)
	}
	code, err := strconv.Atoi(fields[1])
	if err != nil {
		return 0, fmt.Errorf("invalid HTTP status %q: %w", status, err)
	}
	return code, nil
}

// hasValidSyncTokenError reports whether a response body is a DAV:error
// directly containing DAV:valid-sync-token. The body is always consumed.
func hasValidSyncTokenError(r io.Reader) bool {
	data, err := io.ReadAll(r)
	if err != nil {
		return false
	}
	var envelope syncErrorEnvelope
	if err := xml.Unmarshal(data, &envelope); err != nil {
		return false
	}
	return envelope.XMLName.Space == "DAV:" && envelope.XMLName.Local == "error" && envelope.ValidSyncToken != nil
}

func closeAndDiscard(body io.ReadCloser) {
	_, _ = io.Copy(io.Discard, body)
	_ = body.Close()
}

func cloneSyncResources(in map[string]SyncResource) map[string]SyncResource {
	out := make(map[string]SyncResource, len(in))
	for key, resource := range in {
		if resource.Data != nil {
			copied := make([]byte, len(resource.Data))
			copy(copied, resource.Data)
			resource.Data = copied
		}
		out[key] = resource
	}
	return out
}

// collectSyncBodies downloads the files still present after every change has
// been merged. Unchanged bodies are reused from previous; every other GET is
// protected with If-Match and its response ETag must match exactly.
func (c *Client) collectSyncBodies(ctx context.Context, resources map[string]SyncResource, previous *SyncSnapshot) error {
	const op = "CollectSync"

	keys := make([]string, 0, len(resources))
	for key, resource := range resources {
		if !resource.IsDir {
			keys = append(keys, key)
		}
	}
	sort.Strings(keys)

	for _, key := range keys {
		if err := ctx.Err(); err != nil {
			return NewPathErrorErr(op, key, err)
		}
		resource := resources[key]

		if previous != nil {
			if old, ok := previous.Resources[key]; ok && !old.IsDir && old.ETag == resource.ETag && old.Data != nil {
				resource.Data = append([]byte(nil), old.Data...)
				resources[key] = resource
				continue
			}
		}

		rs, err := c.reqCtx(ctx, http.MethodGet, key, nil, func(r *http.Request) {
			r.Header.Set("If-Match", resource.ETag)
		})
		if err != nil {
			return NewPathErrorErr(op, key, err)
		}
		if rs.StatusCode != http.StatusOK {
			closeAndDiscard(rs.Body)
			return NewPathError(op, key, rs.StatusCode)
		}
		if rs.Header.Get("ETag") != resource.ETag {
			closeAndDiscard(rs.Body)
			return NewPathErrorErr(op, key, fmt.Errorf("response ETag %q does not match requested ETag %q", rs.Header.Get("ETag"), resource.ETag))
		}

		data, readErr := io.ReadAll(rs.Body)
		closeErr := rs.Body.Close()
		if readErr != nil {
			return NewPathErrorErr(op, key, readErr)
		}
		if closeErr != nil {
			return NewPathErrorErr(op, key, closeErr)
		}
		resource.Data = data
		resources[key] = resource
	}
	return nil
}
