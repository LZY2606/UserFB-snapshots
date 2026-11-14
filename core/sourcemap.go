package core

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net/url"
	"path"
	"regexp"
	"strings"
	"sync"

	"github.com/gocolly/colly/v2"
	jsoniter "github.com/json-iterator/go"
)

// Source map structures follow the Source Map Revision 3 proposal.
// Only version 3 maps (regular and indexed) are understood; the "mappings"
// field is never decoded because endpoint discovery only needs "sources" and
// "sourcesContent".

type sourceMap struct {
	Version        int          `json:"version"`
	File           string       `json:"file"`
	SourceRoot     string       `json:"sourceRoot"`
	Sources        []string     `json:"sources"`
	SourcesContent []*string    `json:"sourcesContent"`
	Sections       []mapSection `json:"sections"`
}

type mapSection struct {
	Offset mapOffset  `json:"offset"`
	URL    string     `json:"url"`
	Map    *sourceMap `json:"map"`
}

type mapOffset struct {
	Line   int `json:"line"`
	Column int `json:"column"`
}

// sourceMappingURLPattern matches a directive that occupies its own line.
// Both the //# and /*# comment styles are supported and the last directive
// in the file wins. The same text embedded in an ordinary string does not
// match because it is not anchored at the start of a line.
var sourceMappingURLPattern = regexp.MustCompile(`(?m)^[ \t]*(?://|/\*)[#@]\s*sourceMappingURL=([^\s*]+?)\s*(?:\*/)?[ \t]*\r?$`)

// extractSourceMappingURL returns the URL value of the last
// sourceMappingURL directive that occupies its own line.
func extractSourceMappingURL(script string) string {
	matches := sourceMappingURLPattern.FindAllStringSubmatch(script, -1)
	if len(matches) == 0 {
		return ""
	}
	return matches[len(matches)-1][1]
}

// isHTTPURL reports whether u can be requested over HTTP(S).
func isHTTPURL(u *url.URL) bool {
	return u != nil && (u.Scheme == "http" || u.Scheme == "https") && u.Host != ""
}

// resolveReference resolves reference against base the same way browsers do.
func resolveReference(base *url.URL, reference string) (*url.URL, error) {
	ref, err := url.Parse(reference)
	if err != nil {
		return nil, err
	}
	return base.ResolveReference(ref), nil
}

// mapBaseURL turns a map resource URL into the directory URL against which
// sourceRoot and sources entries are resolved.
func mapBaseURL(mapURL *url.URL) *url.URL {
	base := *mapURL
	base.Path = path.Dir(mapURL.Path)
	if base.Path != "/" {
		base.Path += "/"
	}
	base.RawQuery = ""
	base.Fragment = ""
	return &base
}

// sourceRootURL resolves a non-empty sourceRoot against the map base as a
// directory.
func sourceRootURL(mapURL *url.URL, sourceRoot string) (*url.URL, error) {
	root := sourceRoot
	if !strings.HasSuffix(root, "/") {
		root += "/"
	}
	return resolveReference(mapBaseURL(mapURL), root)
}

// sourceURL computes the address of the original source described by a map
// entry. With a sourceRoot it is resolved against the sourceRoot directory,
// otherwise directly against the map base.
func sourceURL(mapURL *url.URL, sourceRoot string, source string) (*url.URL, error) {
	if sourceRoot != "" {
		root, err := sourceRootURL(mapURL, sourceRoot)
		if err != nil {
			return nil, err
		}
		return resolveReference(root, source)
	}
	return resolveReference(mapBaseURL(mapURL), source)
}

// decodeInlineSourceMap decodes a data:application/json;base64,... payload.
// Non-base64 data URLs (including URL-encoded JSON) are not expected from
// sourceMappingURL directives and are rejected.
func decodeInlineSourceMap(ref string) ([]byte, bool) {
	if !strings.HasPrefix(ref, "data:") {
		return nil, false
	}
	comma := strings.Index(ref, ",")
	if comma < 0 {
		return nil, false
	}
	meta := ref[:comma]
	if !strings.Contains(meta, "base64") || !strings.Contains(meta, "json") {
		return nil, false
	}
	data, err := base64.StdEncoding.DecodeString(ref[comma+1:])
	if err != nil {
		return nil, false
	}
	return data, true
}

// sourceMapEngine keeps the per-crawler state for map discovery. It owns no
// goroutines and no HTTP client: every map request is issued through the
// crawler's Link Finder collector, so transport, concurrency limits and
// filters are reused and all work is covered by that collector's WaitGroup.
// All processing runs synchronously inside collector response callbacks.
type sourceMapEngine struct {
	crawler *Crawler

	// mapMu guards the visited set of external map URLs and the set of URLs
	// that must be parsed as maps rather than ordinary scripts. Inline maps
	// are never keyed here: identical inline payloads under different
	// scripts keep their own base URL.
	mapMu       sync.Mutex
	visitedMap  map[string]bool
	mapRequests map[string]bool

	// recordMu guards emitted (original source URL, endpoint URL) pairs.
	// It uses a key space separate from the map reference set.
	recordMu       sync.Mutex
	emittedRecords map[string]bool
}

func newSourceMapEngine(crawler *Crawler) *sourceMapEngine {
	return &sourceMapEngine{
		crawler:        crawler,
		visitedMap:     make(map[string]bool),
		mapRequests:    make(map[string]bool),
		emittedRecords: make(map[string]bool),
	}
}

// mapDedupKey strips the fragment but keeps the query string.
func mapDedupKey(u *url.URL) string {
	key := *u
	key.Fragment = ""
	return key.String()
}

// claimExternalMap marks an external map URL as in progress and reports
// whether this caller owns the single expansion of it.
func (e *sourceMapEngine) claimExternalMap(u *url.URL) bool {
	key := mapDedupKey(u)
	e.mapMu.Lock()
	defer e.mapMu.Unlock()
	if e.visitedMap[key] {
		return false
	}
	e.visitedMap[key] = true
	return true
}

// isMapRequest reports whether a fetched URL was requested as a map.
func (e *sourceMapEngine) isMapRequest(u string) bool {
	e.mapMu.Lock()
	defer e.mapMu.Unlock()
	return e.mapRequests[u]
}

// requestMap fetches an external map through the Link Finder collector. The
// URL is remembered so its response is parsed as a map instead of script.
// Transport, limits and filters are all inherited from that collector.
func (e *sourceMapEngine) requestMap(u *url.URL) {
	if !e.claimExternalMap(u) {
		return
	}
	address := u.String()
	e.mapMu.Lock()
	e.mapRequests[address] = true
	e.mapMu.Unlock()
	e.crawler.enqueueJS(address)
}

// handleScript inspects a fetched script for a sourceMappingURL directive
// and expands the referenced map synchronously in the callback goroutine.
func (e *sourceMapEngine) handleScript(scriptURL *url.URL, body string) {
	ref := extractSourceMappingURL(body)
	if ref == "" {
		return
	}

	if data, ok := decodeInlineSourceMap(ref); ok {
		// Inline maps inherit the final response address of their script.
		e.processMapBytes(scriptURL, data)
		return
	}

	mapURL, err := resolveReference(scriptURL, ref)
	if err != nil {
		e.crawler.warnSourceMap("parse sourceMappingURL of %s: %s", scriptURL.String(), err)
		return
	}
	if !isHTTPURL(mapURL) {
		return
	}
	e.requestMap(mapURL)
}

// handleMapResponse parses one externally fetched map document.
func (e *sourceMapEngine) handleMapResponse(mapURL *url.URL, data []byte) {
	e.processMapBytes(mapURL, data)
}

// processMapBytes parses one map document and expands it. Bad JSON, an
// unsupported version or a malformed section only terminate this branch;
// they never stop sibling sections or other scripts.
func (e *sourceMapEngine) processMapBytes(base *url.URL, data []byte) {
	var sm sourceMap
	if err := json.Unmarshal(data, &sm); err != nil {
		e.crawler.warnSourceMap("parse map %s failed: %s", base.String(), err)
		return
	}
	if sm.Version != 3 {
		e.crawler.warnSourceMap("unsupported source map version %d in %s", sm.Version, base.String())
		return
	}

	if len(sm.Sections) > 0 {
		e.processIndexedMap(base, &sm)
		return
	}
	e.processRegularMap(base, &sm)
}

// processRegularMap feeds every embedded original source to LinkFinder.
// Missing or null sourcesContent entries are skipped, and the original
// sources are never downloaded. Map structure fields are never passed to
// LinkFinder as script text.
func (e *sourceMapEngine) processRegularMap(mapURL *url.URL, sm *sourceMap) {
	for i, source := range sm.Sources {
		if i >= len(sm.SourcesContent) || sm.SourcesContent[i] == nil {
			continue
		}
		content := *sm.SourcesContent[i]
		if content == "" {
			continue
		}

		srcURL, err := sourceURL(mapURL, sm.SourceRoot, source)
		if err != nil {
			e.crawler.warnSourceMap("resolve source %q of map %s failed: %s", source, mapURL.String(), err)
			continue
		}
		if !isHTTPURL(srcURL) {
			continue
		}

		e.processOriginalSource(srcURL, content)
	}
}

// processIndexedMap expands every section. An embedded child map inherits
// the parent map base; an external child uses its own final response URL.
// Offsets do not affect endpoint discovery.
func (e *sourceMapEngine) processIndexedMap(parentBase *url.URL, sm *sourceMap) {
	for i, section := range sm.Sections {
		switch {
		case section.Map != nil:
			data, err := json.Marshal(section.Map)
			if err != nil {
				e.crawler.warnSourceMap("bad section %d of indexed map %s: %s", i, parentBase.String(), err)
				continue
			}
			// Inline child maps inherit the parent's base address.
			e.processMapBytes(parentBase, data)
		case section.URL != "":
			childURL, err := resolveReference(parentBase, section.URL)
			if err != nil {
				e.crawler.warnSourceMap("bad section %d of indexed map %s: %s", i, parentBase.String(), err)
				continue
			}
			if !isHTTPURL(childURL) {
				continue
			}
			// A shared child map is requested only once (fragment stripped,
			// query kept), but its single fetch still contributes content.
			e.requestMap(childURL)
		default:
			e.crawler.warnSourceMap("bad section %d of indexed map %s: empty map and url", i, parentBase.String())
		}
	}
}

// processOriginalSource runs LinkFinder over one embedded source and emits
// one provenance record per distinct (source, endpoint) pair. Relative
// endpoints are resolved only against this source's own address.
func (e *sourceMapEngine) processOriginalSource(srcURL *url.URL, content string) {
	paths, err := LinkFinder(content)
	if err != nil {
		e.crawler.warnSourceMap("linkfinder on source %s failed: %s", srcURL.String(), err)
		return
	}

	crawler := e.crawler
	for _, relPath := range paths {
		endpoint, err := resolveReference(srcURL, relPath)
		if err != nil {
			continue
		}
		if !isHTTPURL(endpoint) {
			continue
		}
		endpointString := endpoint.String()

		// The request still goes through normal filter/dedup even when the
		// provenance record itself was already emitted.
		if e.claimRecord(srcURL.String(), endpointString) {
			crawler.emitSourceMapRecord(srcURL.String(), endpointString)
		}
		crawler.requestSourceMapEndpoint(srcURL.String(), endpointString)
	}
}

// claimRecord reports whether the (source, endpoint) pair is new. Map
// references and source records therefore never share a dedup key.
func (e *sourceMapEngine) claimRecord(source, endpoint string) bool {
	key := source + "\x00" + endpoint
	e.recordMu.Lock()
	defer e.recordMu.Unlock()
	if e.emittedRecords[key] {
		return false
	}
	e.emittedRecords[key] = true
	return true
}

func (crawler *Crawler) emitSourceMapRecord(source, endpoint string) {
	outputFormat := fmt.Sprintf("[sourcemap] - [from: %s] - %s", source, endpoint)
	if crawler.JsonOutput {
		sout := SpiderOutput{
			Input:      crawler.Input,
			Source:     source,
			OutputType: "sourcemap",
			Output:     endpoint,
		}
		if data, err := jsoniter.MarshalToString(sout); err == nil {
			outputFormat = data
		}
	}
	fmt.Println(outputFormat)
	if crawler.Output != nil {
		crawler.Output.WriteToFile(outputFormat)
	}
}

// requestSourceMapEndpoint hands an endpoint discovered in an original
// source to the existing collectors. Whether it is actually requested stays
// controlled by the crawler's URL filters and per-collector dedup. The
// "sourcemap" output type is reserved for provenance records, so the script
// feed keeps the ordinary "linkfinder" type with the real source address.
func (crawler *Crawler) requestSourceMapEndpoint(sourceAddr, endpoint string) {
	fileExt := GetExtType(endpoint)
	if fileExt == ".js" || fileExt == ".xml" || fileExt == ".json" || fileExt == ".map" {
		crawler.feedLinkfinder(endpoint, "linkfinder", sourceAddr)
		return
	}
	if crawler.urlSet.Duplicate(endpoint) {
		return
	}
	crawler.enqueuePage(endpoint)
}

func (crawler *Crawler) warnSourceMap(format string, args ...interface{}) {
	Logger.Warnf("[sourcemap] "+format, args...)
}

// mapRequestError records a diagnostic for a failed external map fetch.
func (e *sourceMapEngine) handleMapError(response *colly.Response, err error) {
	resource := ""
	if response != nil && response.Request != nil {
		resource = response.Request.URL.String()
	}
	e.crawler.warnSourceMap("request map %s failed: %s", resource, err)
}
