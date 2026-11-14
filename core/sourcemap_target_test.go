package core

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io/ioutil"
	"net/http"
	"net/url"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"github.com/jaeles-project/gospider/task_support/taskcheck"
	"github.com/sirupsen/logrus"
	"github.com/spf13/cobra"
)

// scriptTransport is a local RoundTripper that serves scripted routes and
// records every real request. It stands in for the network, so nothing in
// these tests opens a socket.
type scriptTransport struct {
	mu        sync.Mutex
	hits      map[string]int
	routes    map[string]scriptRoute
	gatePath  string
	inflight  chan struct{}
	release   chan struct{}
	userAgent string
}

type scriptRoute struct {
	body        string
	contentType string
}

func newScriptTransport() *scriptTransport {
	return &scriptTransport{
		hits:   make(map[string]int),
		routes: make(map[string]scriptRoute),
	}
}

func (t *scriptTransport) add(path, body, contentType string) {
	t.routes[path] = scriptRoute{body: body, contentType: contentType}
}

func (t *scriptTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	key := req.URL.Path
	if req.URL.RawQuery != "" {
		key += "?" + req.URL.RawQuery
	}
	t.mu.Lock()
	t.hits[key]++
	t.userAgent = req.Header.Get("User-Agent")
	t.mu.Unlock()

	if t.gatePath != "" && req.URL.Path == t.gatePath {
		// Hand the in-flight state to the test and wait for its channel.
		t.inflight <- struct{}{}
		<-t.release
	}

	route, ok := t.routes[key]
	if !ok {
		route, ok = t.routes[req.URL.Path]
	}
	status := http.StatusOK
	if !ok {
		status = http.StatusNotFound
		route = scriptRoute{body: "not found", contentType: "text/plain"}
	}
	header := http.Header{}
	header.Set("Content-Type", route.contentType)
	return &http.Response{
		StatusCode: status,
		Header:     header,
		Body:       ioutil.NopCloser(strings.NewReader(route.body)),
		Request:    req,
	}, nil
}

func (t *scriptTransport) count(path string) int {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.hits[path]
}

// memLogHook collects log diagnostics without swapping the global logger
// output, which would race with background request goroutines.
type memLogHook struct {
	mu      sync.Mutex
	entries []string
}

func (h *memLogHook) Levels() []logrus.Level {
	return logrus.AllLevels
}

func (h *memLogHook) Fire(entry *logrus.Entry) error {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.entries = append(h.entries, entry.Message)
	return nil
}

func (h *memLogHook) text() string {
	h.mu.Lock()
	defer h.mu.Unlock()
	return strings.Join(h.entries, "\n")
}

// testFlags builds a cobra command carrying every flag NewCrawler reads.
func testFlags(sourceMaps bool) *cobra.Command {
	cmd := &cobra.Command{}
	boolFlags := map[string]bool{
		"quiet": false, "json": true, "length": false, "raw": false,
		"subs": false, "no-redirect": false, "source-maps": sourceMaps,
	}
	for name, value := range boolFlags {
		cmd.Flags().Bool(name, value, "")
	}
	intFlags := map[string]int{
		"depth": 0, "concurrent": 4, "delay": 0, "random-delay": 0, "timeout": 10,
	}
	for name, value := range intFlags {
		cmd.Flags().Int(name, value, "")
	}
	cmd.Flags().String("proxy", "", "")
	cmd.Flags().String("burp", "", "")
	cmd.Flags().String("cookie", "", "")
	cmd.Flags().String("output", "", "")
	cmd.Flags().String("user-agent", "map-test-agent", "")
	cmd.Flags().String("filter-length", "", "")
	cmd.Flags().String("blacklist", "", "")
	cmd.Flags().String("whitelist", "", "")
	cmd.Flags().String("whitelist-domain", "", "")
	cmd.Flags().StringArray("header", nil, "")
	return cmd
}

// newTestCrawler builds a crawler whose collectors all use transport.
func newTestCrawler(t *testing.T, transport http.RoundTripper, sourceMaps bool) *Crawler {
	t.Helper()
	site, err := url.Parse("http://map.example/")
	if err != nil {
		t.Fatal(err)
	}
	crawler := NewCrawler(site, testFlags(sourceMaps))
	crawler.C.WithTransport(transport)
	crawler.LinkFinderCollector.WithTransport(transport)
	return crawler
}

// attachFileOutput points the crawler at a fresh output folder and returns
// it. Provenance records are read back from this file instead of stdout.
func attachFileOutput(t *testing.T, crawler *Crawler) string {
	t.Helper()
	dir := t.TempDir()
	crawler.Output = NewOutput(dir, "map_output")
	return dir
}

func sourceMapRecordsFromDir(dir string) []SpiderOutput {
	files, err := filepath.Glob(filepath.Join(dir, "*"))
	if err != nil || len(files) == 0 {
		return nil
	}
	data, err := ioutil.ReadFile(files[0])
	if err != nil {
		return nil
	}
	var records []SpiderOutput
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if !strings.HasPrefix(line, "{") {
			continue
		}
		var record SpiderOutput
		if err := json.Unmarshal([]byte(line), &record); err != nil {
			continue
		}
		if record.OutputType == "sourcemap" {
			records = append(records, record)
		}
	}
	return records
}

func b64JSON(raw string) string {
	return base64.StdEncoding.EncodeToString([]byte(raw))
}

const htmlPage = `<html><body>
<script src="/a.js"></script>
<script src="/b.js"></script>
<script src="/c.js"></script>
</body></html>`

// TestSourceMapDiscovery covers directive discovery: line-only directives,
// both comment styles, last-directive-wins, string decoys, external and
// inline maps, and the default-off switch.
func TestSourceMapDiscovery(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Unit-level directive parsing first.
		c.Check(extractSourceMappingURL("var x = 1;\n//# sourceMappingURL=real.map\n") == "real.map",
			"line //# directive parsed")
		c.Check(extractSourceMappingURL("/*# sourceMappingURL=block.map */") == "block.map",
			"block /*# directive parsed")
		c.Check(extractSourceMappingURL("const s = \"//# sourceMappingURL=decoy.map\";\n") == "",
			"same text inside a string is not a directive")
		multi := "//# sourceMappingURL=first.map\n//# sourceMappingURL=last.map\n"
		c.Check(extractSourceMappingURL(multi) == "last.map", "last directive wins")
		crlf := "//# sourceMappingURL=crlf.map\r\n"
		c.Check(extractSourceMappingURL(crlf) == "crlf.map", "CRLF line ending supported")

		transport := newScriptTransport()
		transport.add("/", htmlPage, "text/html")
		transport.add("/a.js",
			"const normal = \"/api/normal\";\n//# sourceMappingURL=a.js.map\n",
			"application/javascript")
		transport.add("/a.js.map",
			`{"version":3,"sources":["x.ts"],"sourcesContent":["const m = \"/api/frommap\";"],"mappings":""}`,
			"application/json")
		// b.js mentions a directive inside a string (decoy) and ends with a
		// real block-style directive.
		transport.add("/b.js",
			"const decoy = \"//# sourceMappingURL=b.js.map\";\n/*# sourceMappingURL=b.real.map */\n",
			"application/javascript")
		transport.add("/b.real.map",
			`{"version":3,"sources":["b.ts"],"sourcesContent":["const b = \"/api/fromb\";"],"mappings":""}`,
			"application/json")
		// c.js embeds the map payload inline; its base is /c.js.
		inlineMap := `{"version":3,"sources":["deep/y.ts"],"sourcesContent":["const i = \"/api/inline\";"],"mappings":""}`
		transport.add("/c.js",
			fmt.Sprintf("const c=1;\n//# sourceMappingURL=data:application/json;base64,%s\n", b64JSON(inlineMap)),
			"application/javascript")

		crawler := newTestCrawler(t, transport, true)
		crawler.Start(true)
		crawler.Wait()

		c.Check(transport.count("/a.js.map") == 1, "external map requested once, got %d", transport.count("/a.js.map"))
		c.Check(transport.count("/b.js.map") == 0, "string decoy map never requested")
		c.Check(transport.count("/b.real.map") == 1, "real block-style directive followed")
		c.Check(transport.count("/api/normal") == 1, "ordinary script LinkFinder still works")
		c.Check(transport.count("/api/frommap") == 1, "endpoint from external map discovered")
		c.Check(transport.count("/api/fromb") == 1, "endpoint from block-style map discovered")
		c.Check(transport.count("/api/inline") == 1, "endpoint from inline map discovered")
		c.Check(transport.count("/deep/y.ts") == 0, "embedded sources are never downloaded")

		// The map request must ride the configured client, including UA.
		transport.mu.Lock()
		ua := transport.userAgent
		transport.mu.Unlock()
		c.Check(ua == "map-test-agent", "map request used configured transport client, ua=%q", ua)

		// Default-off: a second crawler without the switch never reads maps.
		offTransport := newScriptTransport()
		offTransport.add("/", htmlPage, "text/html")
		offTransport.add("/a.js", transport.routes["/a.js"].body, "application/javascript")
		offTransport.add("/a.js.map", transport.routes["/a.js.map"].body, "application/json")
		offTransport.add("/b.js", transport.routes["/b.js"].body, "application/javascript")
		offTransport.add("/c.js", transport.routes["/c.js"].body, "application/javascript")
		offCrawler := newTestCrawler(t, offTransport, false)
		c.Check(offCrawler.smu == nil, "source map engine absent when switch is off")
		offCrawler.Start(true)
		offCrawler.Wait()
		c.Check(offTransport.count("/a.js.map") == 0, "map not requested when --source-maps is off")

		// --js=false (Link Finder pipeline disabled): even with the engine
		// present, the script response hook is never installed, so no map is
		// read. main.go additionally forces the flag off in this case.
		jsOffTransport := newScriptTransport()
		jsOffTransport.add("/", htmlPage, "text/html")
		jsOffTransport.add("/a.js", transport.routes["/a.js"].body, "application/javascript")
		jsOffTransport.add("/a.js.map", transport.routes["/a.js.map"].body, "application/json")
		jsOffCrawler := newTestCrawler(t, jsOffTransport, true)
		c.Check(jsOffCrawler.smu != nil, "engine built when the switch is on")
		jsOffCrawler.Start(false) // mirrors --js=false / --base
		jsOffCrawler.Wait()
		c.Check(jsOffTransport.count("/a.js.map") == 0, "no map read when the JavaScript pipeline is off")
	})
}

// TestSourceMapProvenance covers original-source base resolution, the JSON
// record shape, sourceRoot handling, null/missing sourcesContent, distinct
// provenance records and file output.
func TestSourceMapProvenance(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// The exact example from the spec: /maps/app.map, sourceRoot ../src/,
		// source pkg/a.ts, endpoint ../api/items -> /src/api/items.
		mapURL, _ := url.Parse("http://map.example/maps/app.map")
		src, err := sourceURL(mapURL, "../src/", "pkg/a.ts")
		c.Check(err == nil, "sourceURL parses")
		c.Check(src.String() == "http://map.example/src/pkg/a.ts", "source address got %s", src.String())
		endpoint, err := resolveReference(src, "../api/items")
		c.Check(err == nil, "endpoint resolves")
		c.Check(endpoint.String() == "http://map.example/src/api/items", "endpoint got %s", endpoint.String())

		transport := newScriptTransport()
		appMap := `{"version":3,"file":"ignored.js","sourceRoot":"../src/","sources":["pkg/a.ts","pkg/b.ts","missing.ts","file:///local.ts"],` +
			`"sourcesContent":["const a = \"/api/shared\";","const b = \"/api/shared\";",null,"const z = \"/api/local\";"],` +
			`"mappings":""}`
		transport.add("/",
			`<script src="/maps/app.js"></script><script src="/maps/noroot.js"></script>`, "text/html")
		transport.add("/maps/app.js", "//# sourceMappingURL=app.map\n", "application/javascript")
		transport.add("/maps/app.map", appMap, "application/json")
		transport.add("/maps/noroot.js", "//# sourceMappingURL=noroot.map\n", "application/javascript")
		transport.add("/maps/noroot.map",
			`{"version":3,"sources":["z.ts"],"sourcesContent":["const n = \"/api/noroot\";"],"mappings":""}`,
			"application/json")

		crawler := newTestCrawler(t, transport, true)
		dir := attachFileOutput(t, crawler)
		crawler.Start(true)
		crawler.Wait()
		crawler.Output.Close()

		records := sourceMapRecordsFromDir(dir)
		c.Check(len(records) >= 3, "expected records for a,b,z sources, got %d: %v", len(records), records)

		recordBySource := map[string]SpiderOutput{}
		for _, record := range records {
			recordBySource[record.Source] = record
			c.Check(record.OutputType == "sourcemap", "JSON type is sourcemap")
		}
		recA, okA := recordBySource["http://map.example/src/pkg/a.ts"]
		c.Check(okA, "record for pkg/a.ts present")
		c.Check(recA.Output == "http://map.example/api/shared", "a endpoint got %s", recA.Output)
		recB, okB := recordBySource["http://map.example/src/pkg/b.ts"]
		c.Check(okB, "second source keeps its own provenance record")
		c.Check(recB.Output == recA.Output, "same endpoint from two sources keeps both source records")
		recZ, okZ := recordBySource["http://map.example/maps/z.ts"]
		c.Check(okZ, "no-sourceRoot source resolved against map base")
		c.Check(recZ.Output == "http://map.example/api/noroot", "no-root endpoint got %s", recZ.Output)

		// One endpoint request despite two provenance records: existing
		// dedup still controls actual requests.
		c.Check(transport.count("/api/shared") == 1, "shared endpoint requested once, got %d", transport.count("/api/shared"))
		c.Check(transport.count("/api/noroot") == 1, "noroot endpoint requested")
		// Map structure fields and source paths are never fetched.
		c.Check(transport.count("/maps/ignored.js") == 0, "map file field not treated as a script")
		c.Check(transport.count("/src/pkg/a.ts") == 0, "source path not downloaded")
		c.Check(transport.count("/src/missing.ts") == 0, "null sourcesContent skipped without download")
		c.Check(transport.count("/local.ts") == 0, "non-HTTP source skipped")

		// File output retains the sourcemap records.
		files, _ := filepath.Glob(filepath.Join(dir, "*"))
		data, _ := ioutil.ReadFile(files[0])
		fileText := string(data)
		c.Check(strings.Contains(fileText, `"type":"sourcemap"`), "file output keeps sourcemap records")
		c.Check(strings.Contains(fileText, "http://map.example/api/shared"), "file output keeps absolute endpoint")
	})
}

// TestSourceMapGraph covers indexed maps, inline and external sections,
// shared/cyclic external maps, fragment stripping, query preservation,
// failed/unsupported branches and identical inline maps under different
// bases.
func TestSourceMapGraph(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		hook := &memLogHook{}
		oldHooks := Logger.Hooks
		oldLevel := Logger.Level
		// The package logger is built from a struct literal, so Hooks may be
		// a nil map and must be initialised before adding a hook.
		Logger.Hooks = make(logrus.LevelHooks)
		Logger.AddHook(hook)
		Logger.SetLevel(logrus.WarnLevel)
		defer func() {
			Logger.SetLevel(oldLevel)
			Logger.Hooks = oldHooks
		}()

		transport := newScriptTransport()
		transport.add("/", `<html>
<script src="/idx.js"></script>
<script src="/sh1.js"></script>
<script src="/sh2.js"></script>
<script src="/cyc.js"></script>
<script src="/bad.js"></script>
<script src="/v1.js"></script>
<script src="/missing.js"></script>
<script src="/q1.js"></script>
<script src="/q2.js"></script>
<script src="/f1.js"></script>
<script src="/f2.js"></script>
<script src="/d1/s.js"></script>
<script src="/d2/s.js"></script>
</html>`, "text/html")

		// Indexed map: inline section (parent base), external section, and a
		// malformed empty section.
		transport.add("/idx.js", "//# sourceMappingURL=idx.map\n", "application/javascript")
		idxMap := `{"version":3,"sections":[` +
			`{"offset":{"line":0,"column":0},"map":{"version":3,"sources":["s1.ts"],"sourcesContent":["const s1 = \"/api/s1\";"],"mappings":""}},` +
			`{"offset":{"line":5,"column":0},"url":"child.map"},` +
			`{"offset":{"line":9,"column":0}}` +
			`]}`
		transport.add("/idx.map", idxMap, "application/json")
		transport.add("/child.map",
			`{"version":3,"sources":["s2.ts"],"sourcesContent":["const s2 = \"/api/s2\";"],"mappings":""}`,
			"application/json")

		// Two parents referencing the same external child map.
		transport.add("/sh1.js", "//# sourceMappingURL=sh1.map\n", "application/javascript")
		transport.add("/sh2.js", "//# sourceMappingURL=sh2.map\n", "application/javascript")
		sharedSection := `{"version":3,"sections":[{"offset":{"line":0,"column":0},"url":"shared.map"}]}`
		transport.add("/sh1.map", sharedSection, "application/json")
		transport.add("/sh2.map", sharedSection, "application/json")
		transport.add("/shared.map",
			`{"version":3,"sources":["sh.ts"],"sourcesContent":["const sh = \"/api/shared-child\";"],"mappings":""}`,
			"application/json")

		// A cycle: cyc.map -> cyc2.map -> cyc.map.
		transport.add("/cyc.js", "//# sourceMappingURL=cyc.map\n", "application/javascript")
		transport.add("/cyc.map",
			`{"version":3,"sections":[{"offset":{"line":0,"column":0},"url":"cyc2.map"}]}`,
			"application/json")
		transport.add("/cyc2.map",
			`{"version":3,"sections":[{"offset":{"line":0,"column":0},"url":"cyc.map"}]}`,
			"application/json")

		// Bad JSON, unsupported version and a failing request.
		transport.add("/bad.js", "//# sourceMappingURL=bad.map\n", "application/javascript")
		transport.add("/bad.map", `{broken json`, "application/json")
		transport.add("/v1.js", "//# sourceMappingURL=v1.map\n", "application/javascript")
		transport.add("/v1.map", `{"version":1,"sources":[]}`, "application/json")
		transport.add("/missing.js", "//# sourceMappingURL=gone.map\n", "application/javascript")

		// Query strings distinguish maps; fragments do not.
		transport.add("/q1.js", "//# sourceMappingURL=q.map?x=1\n", "application/javascript")
		transport.add("/q2.js", "//# sourceMappingURL=q.map?x=2\n", "application/javascript")
		transport.add("/q.map?x=1",
			`{"version":3,"sources":["q1.ts"],"sourcesContent":["const q1 = \"/api/q1\";"],"mappings":""}`,
			"application/json")
		transport.add("/q.map?x=2",
			`{"version":3,"sources":["q2.ts"],"sourcesContent":["const q2 = \"/api/q2\";"],"mappings":""}`,
			"application/json")
		transport.add("/f1.js", "//# sourceMappingURL=f.map#a\n", "application/javascript")
		transport.add("/f2.js", "//# sourceMappingURL=f.map#b\n", "application/javascript")
		transport.add("/f.map",
			`{"version":3,"sources":["f.ts"],"sourcesContent":["const f = \"/api/f\";"],"mappings":""}`,
			"application/json")

		// Byte-identical inline maps under scripts with different bases.
		identicalInline := `{"version":3,"sources":["u.ts"],"sourcesContent":["const u = \"rel/api\";"],"mappings":""}`
		encoded := b64JSON(identicalInline)
		transport.add("/d1/s.js",
			fmt.Sprintf("//# sourceMappingURL=data:application/json;base64,%s\n", encoded),
			"application/javascript")
		transport.add("/d2/s.js",
			fmt.Sprintf("//# sourceMappingURL=data:application/json;base64,%s\n", encoded),
			"application/javascript")

		crawler := newTestCrawler(t, transport, true)
		dir := attachFileOutput(t, crawler)
		crawler.Start(true)
		crawler.Wait()
		crawler.Output.Close()

		// Inline section inherited the parent base; external section used
		// its own response URL.
		c.Check(transport.count("/api/s1") == 1, "inline indexed section discovered endpoint")
		c.Check(transport.count("/child.map") == 1, "external section requested")
		c.Check(transport.count("/api/s2") == 1, "external section content processed")

		// Shared child requested once yet still contributes its content.
		c.Check(transport.count("/shared.map") == 1, "shared child map requested once, got %d", transport.count("/shared.map"))
		c.Check(transport.count("/api/shared-child") == 1, "shared child still contributes content")

		// The cycle terminates without re-expanding either map.
		c.Check(transport.count("/cyc.map") == 1, "cycle map A expanded once, got %d", transport.count("/cyc.map"))
		c.Check(transport.count("/cyc2.map") == 1, "cycle map B expanded once, got %d", transport.count("/cyc2.map"))

		// Broken branches end alone and carry the resource address in the
		// diagnostic, while siblings complete.
		logText := hook.text()
		c.Check(strings.Contains(logText, "/bad.map"), "bad JSON diagnostic names resource: %s", logText)
		c.Check(strings.Contains(logText, "version"), "unsupported version diagnostic emitted")
		c.Check(strings.Contains(logText, "/gone.map"), "failed request diagnostic names resource")
		c.Check(transport.count("/api/s1") == 1 && transport.count("/api/s2") == 1, "sibling sections still complete")

		// Query kept, fragment stripped.
		c.Check(transport.count("/q.map?x=1") == 1, "query variant 1 requested")
		c.Check(transport.count("/q.map?x=2") == 1, "query variant 2 requested")
		c.Check(transport.count("/f.map") == 1, "fragment variants collapse to one request, got %d", transport.count("/f.map"))
		c.Check(transport.count("/api/f") == 1, "fragment-collapsed map contributes content")

		// Identical inline payloads keep per-script bases.
		records := sourceMapRecordsFromDir(dir)
		sources := map[string]bool{}
		for _, record := range records {
			sources[record.Source] = true
		}
		c.Check(sources["http://map.example/d1/u.ts"], "inline map uses /d1 base")
		c.Check(sources["http://map.example/d2/u.ts"], "inline map uses /d2 base")
		c.Check(transport.count("/d1/rel/api") == 1, "endpoint from /d1 base requested")
		c.Check(transport.count("/d2/rel/api") == 1, "endpoint from /d2 base requested")
	})
}

// TestSourceMapCompletion covers asynchronous derived work: maps discovered
// late, cross-collector handoff controlled by a channel, and two crawlers
// running concurrently without shared state.
func TestSourceMapCompletion(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		transport := newScriptTransport()
		transport.inflight = make(chan struct{}, 1)
		transport.release = make(chan struct{})
		transport.gatePath = "/chain/step1.map"

		transport.add("/", `<script src="/p.js"></script>`, "text/html")
		transport.add("/p.js", "//# sourceMappingURL=p.map\n", "application/javascript")
		transport.add("/p.map",
			`{"version":3,"sources":["p.ts"],"sourcesContent":["const next = \"/chain/step1.js\";"],"mappings":""}`,
			"application/json")
		transport.add("/chain/step1.js",
			"//# sourceMappingURL=step1.map\n", "application/javascript")
		transport.add("/chain/step1.map",
			`{"version":3,"sources":["step.ts"],"sourcesContent":["const last = \"/chain/step2\";"],"mappings":""}`,
			"application/json")
		transport.add("/chain/step2", "done", "application/json")

		crawler := newTestCrawler(t, transport, true)

		// Wait must cover the map collector even while its request is parked
		// on a channel; no sleep or fixed round count is involved.
		crawler.Start(true)
		done := make(chan struct{})
		go func() {
			crawler.Wait()
			close(done)
		}()
		<-transport.inflight
		close(transport.release)
		<-done

		c.Check(transport.count("/p.map") == 1, "first map fetched")
		c.Check(transport.count("/chain/step1.js") == 1, "script derived from embedded source fetched")
		c.Check(transport.count("/chain/step1.map") == 1, "nested map fetched through map collector")
		c.Check(transport.count("/chain/step2") == 1, "endpoint derived from nested map fetched after Wait chain")

		// Two crawlers at once: disjoint hosts and routes, independent state.
		transportA := newScriptTransport()
		transportA.add("/", `<script src="/a.js"></script>`, "text/html")
		transportA.add("/a.js", "//# sourceMappingURL=a.map\n", "application/javascript")
		transportA.add("/a.map",
			`{"version":3,"sources":["a.ts"],"sourcesContent":["const a = \"/api/a\";"],"mappings":""}`,
			"application/json")

		transportB := newScriptTransport()
		transportB.add("/", `<script src="/b.js"></script>`, "text/html")
		transportB.add("/b.js", "//# sourceMappingURL=b.map\n", "application/javascript")
		transportB.add("/b.map",
			`{"version":3,"sources":["b.ts"],"sourcesContent":["const b = \"/api/b\";"],"mappings":""}`,
			"application/json")

		crawlerA := newTestCrawler(t, transportA, true)
		crawlerB := newTestCrawler(t, transportB, true)
		var parallel sync.WaitGroup
		parallel.Add(2)
		go func() {
			defer parallel.Done()
			crawlerA.Start(true)
			crawlerA.Wait()
		}()
		go func() {
			defer parallel.Done()
			crawlerB.Start(true)
			crawlerB.Wait()
		}()
		parallel.Wait()

		c.Check(transportA.count("/a.map") == 1, "crawler A read its own map")
		c.Check(transportA.count("/api/a") == 1, "crawler A found its own endpoint")
		c.Check(transportA.count("/b.map") == 0, "crawler A never touched crawler B state")
		c.Check(transportB.count("/b.map") == 1, "crawler B read its own map")
		c.Check(transportB.count("/api/b") == 1, "crawler B found its own endpoint")
		c.Check(transportB.count("/a.map") == 0, "crawler B never touched crawler A state")
	})
}
