package opengraph

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"reflect"
	"strings"
	"testing"

	"github.com/otiai10/opengraph/v2/task_support/taskcheck"
	"golang.org/x/net/html"
)

// ---------------------------------------------------------------- helpers

func mfObject(c *taskcheck.C, obj map[string]any, label string) map[string]any {
	c.Check(obj != nil, "%s: nil object", label)
	return obj
}

func mfProps(c *taskcheck.C, obj map[string]any, label string) map[string]any {
	raw, ok := obj["properties"]
	c.Check(ok, "%s: missing properties", label)
	if !ok {
		return map[string]any{}
	}
	props, ok := raw.(map[string]any)
	c.Check(ok, "%s: properties is not an object", label)
	return props
}

func mfList(c *taskcheck.C, props map[string]any, name, label string) []any {
	raw, ok := props[name]
	c.Check(ok, "%s: missing property %q", label, name)
	if !ok {
		return nil
	}
	list, ok := raw.([]any)
	c.Check(ok, "%s: property %q is not an array", label, name)
	return list
}

func mfChildren(c *taskcheck.C, obj map[string]any, label string) []any {
	raw, ok := obj["children"]
	if !ok {
		return nil
	}
	list, ok := raw.([]any)
	c.Check(ok, "%s: children is not an array", label)
	return list
}

func mfStrings(c *taskcheck.C, props map[string]any, name, label string) []string {
	list := mfList(c, props, name, label)
	out := make([]string, 0, len(list))
	for i, v := range list {
		s, ok := v.(string)
		c.Check(ok, "%s: %s[%d] is not a string: %T", label, name, i, v)
		out = append(out, s)
	}
	return out
}

func mfString(c *taskcheck.C, props map[string]any, name, label string) string {
	values := mfStrings(c, props, name, label)
	c.Check(len(values) == 1, "%s: %s has %d values, want 1", label, name, len(values))
	if len(values) == 0 {
		return ""
	}
	return values[0]
}

func mfType(c *taskcheck.C, obj map[string]any, label string) []string {
	raw, ok := obj["type"]
	c.Check(ok, "%s: missing type", label)
	list, ok := raw.([]any)
	c.Check(ok, "%s: type is not an array", label)
	out := make([]string, 0, len(list))
	for i, v := range list {
		s, ok := v.(string)
		c.Check(ok, "%s: type[%d] is not a string", label, i)
		out = append(out, s)
	}
	return out
}

func mfChild(c *taskcheck.C, list []any, i int, label string) map[string]any {
	c.Check(i < len(list), "%s: missing index %d (len %d)", label, i, len(list))
	if i >= len(list) {
		return map[string]any{}
	}
	obj, ok := list[i].(map[string]any)
	c.Check(ok, "%s[%d] is not an object: %T", label, i, list[i])
	return obj
}

func mfParse(c *taskcheck.C, page, rawurl string) *OpenGraph {
	og := New(rawurl)
	og.Intent.CollectMicroformats = true
	if err := og.Parse(strings.NewReader(page)); err != nil {
		c.Check(false, "Parse(%q): %v", rawurl, err)
	}
	return og
}

func mfJSON(v any) string {
	b, _ := json.Marshal(v)
	return string(b)
}

func mfFindElement(n *html.Node, id string) *html.Node {
	if n.Type == html.ElementNode {
		for _, attr := range n.Attr {
			if attr.Key == "id" && attr.Val == id {
				return n
			}
		}
	}
	for child := n.FirstChild; child != nil; child = child.NextSibling {
		if found := mfFindElement(child, id); found != nil {
			return found
		}
	}
	return nil
}

// mfBody is a response body that can exhaust normally or fail on read.
type mfBody struct {
	data string
	err  error
	pos  int
}

func (b *mfBody) Read(p []byte) (int, error) {
	if b.err != nil {
		return 0, b.err
	}
	if b.pos >= len(b.data) {
		return 0, io.EOF
	}
	n := copy(p, b.data[b.pos:])
	b.pos += n
	return n, nil
}

func (b *mfBody) Close() error { return nil }

type mfTransport struct {
	page     string
	finalURL string
	bodyErr  error
	requests []string
	closed   int
}

func (tr *mfTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	tr.requests = append(tr.requests, req.URL.String())
	res := &http.Response{
		StatusCode: 200,
		Header:     http.Header{"Content-Type": {"text/html; charset=utf-8"}},
		Body:       &mfBody{data: tr.page, err: tr.bodyErr},
		Request:    req,
	}
	if tr.finalURL != "" {
		finalReq := req.Clone(req.Context())
		u := *req.URL
		u.Path = tr.finalURL
		u.RawQuery = ""
		finalReq.URL = &u
		res.Request = finalReq
	}
	return res, nil
}

// ------------------------------------------------------------ ownership

func TestMicroformatsOwnership(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		page := `<!doctype html><html><head>
<base href="https://example.invalid/base/">
</head><body>
<article id="entry" class="h-entry">
  <div class="p-author u-author h-card" id="alice">
    <span class="p-name">Alice &amp; Co</span>
    <img class="u-photo" src="alice.png" alt="Alice">
    <a class="u-url" href="people/alice">self</a>
  </div>
  <span class="p-name">Entry title</span>
  <div class="h-cite"><span class="p-name">Quoted</span></div>
  <div class="p-author h-card"><span class="p-name">Bob</span></div>
  <div class="e-content h-cite" id="embedded"><span class="p-name">Embedded</span></div>
</article>
</body></html>`

		og := mfParse(c, page, "https://example.invalid/page")
		c.Check(len(og.Microformats) == 1, "top-level roots: got %d, want 1", len(og.Microformats))
		if len(og.Microformats) != 1 {
			return
		}
		entry := mfObject(c, og.Microformats[0], "entry")
		c.Check(reflect.DeepEqual(mfType(c, entry, "entry"), []string{"h-entry"}), "entry type: %v", mfType(c, entry, "entry"))
		entryProps := mfProps(c, entry, "entry")

		authors := mfList(c, entryProps, "author", "entry")
		c.Check(len(authors) == 3, "author count: got %d, want 3 (DOM order: alice p, alice u, bob p)", len(authors))
		if len(authors) != 3 {
			return
		}

		// The p-author projection of Alice carries the child root's name.
		aliceP := mfChild(c, authors, 0, "author")
		c.Check(reflect.DeepEqual(mfType(c, aliceP, "aliceP"), []string{"h-card"}), "aliceP type: %v", mfType(c, aliceP, "aliceP"))
		c.Check(aliceP["value"] == "Alice & Co", "aliceP value: got %q, want %q", aliceP["value"], "Alice & Co")
		alicePProps := mfProps(c, aliceP, "aliceP")
		c.Check(reflect.DeepEqual(mfStrings(c, alicePProps, "name", "aliceP"), []string{"Alice & Co"}), "aliceP name: %v", mfStrings(c, alicePProps, "name", "aliceP"))
		c.Check(reflect.DeepEqual(mfStrings(c, alicePProps, "url", "aliceP"), []string{"https://example.invalid/base/people/alice"}), "aliceP url: %v", mfStrings(c, alicePProps, "url", "aliceP"))
		photoList := mfList(c, alicePProps, "photo", "aliceP")
		c.Check(len(photoList) == 1, "aliceP photo count: %d", len(photoList))
		alicePhoto := mfChild(c, photoList, 0, "aliceP.photo")
		c.Check(alicePhoto["value"] == "https://example.invalid/base/alice.png", "aliceP photo value: %v", alicePhoto["value"])
		c.Check(alicePhoto["alt"] == "Alice", "aliceP photo alt: %v", alicePhoto["alt"])

		// The u-author projection on the same element is a separate object
		// whose value comes from the child's first url.
		aliceU := mfChild(c, authors, 1, "author")
		c.Check(aliceU["value"] == "https://example.invalid/base/people/alice", "aliceU value: %v", aliceU["value"])
		aliceUProps := mfProps(c, aliceU, "aliceU")
		c.Check(mfJSON(aliceUProps) == mfJSON(alicePProps), "aliceU properties differ: %s vs %s", mfJSON(aliceUProps), mfJSON(alicePProps))
		nameP := mfList(c, alicePProps, "name", "aliceP")
		nameU := mfList(c, aliceUProps, "name", "aliceU")
		c.Check(reflect.ValueOf(nameP).Pointer() != reflect.ValueOf(nameU).Pointer(), "projections share a properties slice")

		bob := mfChild(c, authors, 2, "author")
		c.Check(bob["value"] == "Bob", "bob value: %v", bob["value"])
		c.Check(reflect.DeepEqual(mfStrings(c, mfProps(c, bob, "bob"), "name", "bob"), []string{"Bob"}), "bob name")

		// The root's own property classes only feed its parent.
		_, ownAuthor := alicePProps["author"]
		c.Check(!ownAuthor, "nested h-card counted its own p-author/u-author class")

		// The parent's later p-name still belongs to the parent.
		c.Check(reflect.DeepEqual(mfStrings(c, entryProps, "name", "entry"), []string{"Entry title"}), "entry name: %v", mfStrings(c, entryProps, "name", "entry"))

		// A child root without property classes is a child, and its own
		// properties never flow into the parent.
		children := mfChildren(c, entry, "entry")
		c.Check(len(children) == 1, "children count: got %d, want 1", len(children))
		cite := mfChild(c, children, 0, "children")
		c.Check(reflect.DeepEqual(mfType(c, cite, "cite"), []string{"h-cite"}), "cite type: %v", mfType(c, cite, "cite"))
		c.Check(reflect.DeepEqual(mfStrings(c, mfProps(c, cite, "cite"), "name", "cite"), []string{"Quoted"}), "cite name")
		c.Check(!reflect.DeepEqual(mfStrings(c, entryProps, "name", "entry"), []string{"Quoted"}), "child root text leaked into parent")

		// Empty children are omitted.
		_, bobChildren := bob["children"]
		c.Check(!bobChildren, "bob should not have children")

		// An e-* nested root is projected with its html and value.
		embeddedList := mfList(c, entryProps, "content", "entry")
		embedded := mfChild(c, embeddedList, 0, "entry.content")
		c.Check(reflect.DeepEqual(mfType(c, embedded, "embedded"), []string{"h-cite"}), "embedded type: %v", mfType(c, embedded, "embedded"))
		c.Check(reflect.DeepEqual(mfStrings(c, mfProps(c, embedded, "embedded"), "name", "embedded"), []string{"Embedded"}), "embedded name")
		c.Check(embedded["html"] == `<span class="p-name">Embedded</span>`, "embedded html: %q", embedded["html"])
		c.Check(embedded["value"] == "Embedded", "embedded value: %v", embedded["value"])
	})
}

// ---------------------------------------------------------------- values

func TestMicroformatsValues(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		page := `<!doctype html><html><head></head><body>
<div class="h-event" id="v">
  <span class="p-name">ignored <span class="value">A</span><span class="wrap"><span class="value-title" title="B"></span></span><span class="value">C</span><span class="value"></span></span>
</div>
<div class="h-entry" id="pvals">
  <abbr class="p-abbr-empty" title="">body</abbr>
  <abbr class="p-abbr" title="Title &amp; More">body</abbr>
  <data class="p-data-empty" value=""></data>
  <input class="p-input" value="Input V">
  <img class="p-img-empty" alt="">
  <img class="p-img" alt="Alt V">
  <area class="p-area" alt="Area V">
  <span class="p-text">  a<img src="rel.png">b<img src="rel2.png" alt="Y">c<script>bad</script><style>bad</style><template>bad</template>  </span>
</div>
<div class="h-card" id="uvals">
  <a class="u-href" href="rel/a">A</a>
  <img class="u-img-alt" src="rel/i.png" alt="I">
  <img class="u-img" src="rel/j.png">
  <video class="u-video-poster" poster="rel/poster.png"></video>
  <video class="u-video-src" src="rel/v.mp4" poster="rel/poster2.png"></video>
  <audio class="u-audio" src="rel/a.mp3"></audio>
  <source class="u-source" src="rel/s.mp4">
  <iframe class="u-iframe" src="rel/frame"></iframe>
  <object class="u-object" data="rel/o.swf"></object>
  <abbr class="u-abbr" title="rel/t">T</abbr>
  <data class="u-data" value="rel/d"></data>
  <span class="u-text">rel/text</span>
</div>
<div class="h-entry" id="evals">
  <div class="e-content">Hi <a href="rel/link">there</a><script>s</script><img src="rel/p.png" alt="P"></div>
</div>
<div class="h-entry h-entry h-event" id="classes">
  <span class="H-card">not root</span>
  <span class="p-Name">bad suffix</span>
  <span class="p-name2">bad digit</span>
  <span class="p--bad">bad dash</span>
  <span class="p-name p-name">dup</span>
  <span class="p-x p-x">same</span>
  <span class="p-x">same</span>
</div>
</body></html>`

		og := mfParse(c, page, "https://example.invalid/dir/page")
		c.Check(len(og.Microformats) == 5, "top-level roots: got %d, want 5", len(og.Microformats))
		if len(og.Microformats) != 5 {
			return
		}

		v := mfProps(c, og.Microformats[0], "v")
		c.Check(mfString(c, v, "name", "v") == "ABC", "value-class-pattern: got %q", mfString(c, v, "name", "v"))

		p := mfProps(c, og.Microformats[1], "pvals")
		c.Check(mfString(c, p, "abbr-empty", "pvals") == "", "empty abbr title must win over text")
		c.Check(mfString(c, p, "abbr", "pvals") == "Title & More", "abbr title")
		c.Check(mfString(c, p, "data-empty", "pvals") == "", "empty data value")
		c.Check(mfString(c, p, "input", "pvals") == "Input V", "input value")
		c.Check(mfString(c, p, "img-empty", "pvals") == "", "empty img alt")
		c.Check(mfString(c, p, "img", "pvals") == "Alt V", "img alt")
		c.Check(mfString(c, p, "area", "pvals") == "Area V", "area alt")
		c.Check(mfString(c, p, "text", "pvals") == "a https://example.invalid/dir/rel.png b Y c", "p text with image spacing: got %q", mfString(c, p, "text", "pvals"))

		u := mfProps(c, og.Microformats[2], "uvals")
		uExpect := map[string]string{
			"href":         "https://example.invalid/dir/rel/a",
			"img":          "https://example.invalid/dir/rel/j.png",
			"video-poster": "https://example.invalid/dir/rel/poster.png",
			"video-src":    "https://example.invalid/dir/rel/v.mp4",
			"audio":        "https://example.invalid/dir/rel/a.mp3",
			"source":       "https://example.invalid/dir/rel/s.mp4",
			"iframe":       "https://example.invalid/dir/rel/frame",
			"object":       "https://example.invalid/dir/rel/o.swf",
			"abbr":         "https://example.invalid/dir/rel/t",
			"data":         "https://example.invalid/dir/rel/d",
			"text":         "https://example.invalid/dir/rel/text",
		}
		for name, want := range uExpect {
			got := mfString(c, u, name, "uvals")
			c.Check(got == want, "u-%s: got %q, want %q", name, got, want)
		}
		imgAlt := mfChild(c, mfList(c, u, "img-alt", "uvals"), 0, "u-img-alt")
		c.Check(imgAlt["value"] == "https://example.invalid/dir/rel/i.png", "u-img-alt value: %v", imgAlt["value"])
		c.Check(imgAlt["alt"] == "I", "u-img-alt alt: %v", imgAlt["alt"])

		e := mfProps(c, og.Microformats[3], "evals")
		content := mfChild(c, mfList(c, e, "content", "evals"), 0, "e-content")
		wantHTML := `Hi <a href="rel/link">there</a><script>s</script><img src="rel/p.png" alt="P"/>`
		c.Check(content["html"] == wantHTML, "e html: got %q, want %q", content["html"], wantHTML)
		c.Check(content["value"] == "Hi there P", "e value: got %q", content["value"])

		classes := mfObject(c, og.Microformats[4], "classes")
		c.Check(reflect.DeepEqual(mfType(c, classes, "classes"), []string{"h-entry", "h-event"}), "root type dedupe/sort: %v", mfType(c, classes, "classes"))
		classProps := mfProps(c, classes, "classes")
		c.Check(reflect.DeepEqual(mfStrings(c, classProps, "name", "classes"), []string{"dup"}), "duplicate class token counted once: %v", mfStrings(c, classProps, "name", "classes"))
		c.Check(reflect.DeepEqual(mfStrings(c, classProps, "x", "classes"), []string{"same", "same"}), "same value from different elements must be kept: %v", mfStrings(c, classProps, "x", "classes"))
		for _, bad := range []string{"Name", "name2", "-bad"} {
			_, exists := classProps[bad]
			c.Check(!exists, "invalid suffix %q must not create a property", bad)
		}
	})
}

// ------------------------------------------------------------ collection

func TestMicroformatsCollection(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Implied properties and their suppression rules.
		implied := `<!doctype html><html><head></head><body>
<div class="h-card" id="plain"><img src="pic.png" alt="Pic"><span>Some text</span><a href="me">link</a></div>
<div class="h-card" id="suppressed"><span class="p-name">Explicit</span><a class="u-url" href="u"></a><img src="pic.png" alt="Pic"></div>
<div class="h-card" id="emptyname"><span class="p-name"></span><img src="pic.png" alt="Pic"></div>
<div class="h-card" id="ambiguous"><img src="a.png"><img src="b.png"><a href="one">x</a><a href="two">y</a></div>
<div class="h-card" id="wrapped"><span><img src="w.png" alt="W"><a href="wlink">w</a></span></div>
<div class="h-card" id="deep"><span><span><img src="d.png"></span></span></div>
<div class="h-card" id="nested"><span class="h-card"><span class="p-name">Inside</span><img src="in.png"></span></div>
</body></html>`

		og := mfParse(c, implied, "https://example.invalid/dir/page")
		c.Check(len(og.Microformats) == 7, "implied roots: got %d, want 7", len(og.Microformats))
		if len(og.Microformats) != 7 {
			return
		}
		plain := mfProps(c, og.Microformats[0], "plain")
		c.Check(mfString(c, plain, "name", "plain") == "PicSome textlink", "implied name: got %q", mfString(c, plain, "name", "plain"))
		plainPhoto := mfChild(c, mfList(c, plain, "photo", "plain"), 0, "plain.photo")
		c.Check(plainPhoto["value"] == "https://example.invalid/dir/pic.png", "implied photo: %v", plainPhoto["value"])
		c.Check(plainPhoto["alt"] == "Pic", "implied photo alt: %v", plainPhoto["alt"])
		c.Check(reflect.DeepEqual(mfStrings(c, plain, "url", "plain"), []string{"https://example.invalid/dir/me"}), "implied url: %v", mfStrings(c, plain, "url", "plain"))

		suppressed := mfProps(c, og.Microformats[1], "suppressed")
		c.Check(reflect.DeepEqual(mfStrings(c, suppressed, "name", "suppressed"), []string{"Explicit"}), "explicit name kept")
		c.Check(reflect.DeepEqual(mfStrings(c, suppressed, "url", "suppressed"), []string{"https://example.invalid/dir/u"}), "explicit u-url value")
		_, hasPhoto := suppressed["photo"]
		c.Check(!hasPhoto, "explicit u-url must suppress implied photo")

		emptyName := mfProps(c, og.Microformats[2], "emptyname")
		c.Check(reflect.DeepEqual(mfStrings(c, emptyName, "name", "emptyname"), []string{""}), "explicit empty name must suppress implied name")

		ambiguous := mfProps(c, og.Microformats[3], "ambiguous")
		_, ambPhoto := ambiguous["photo"]
		c.Check(!ambPhoto, "two img children must not imply a photo")
		_, ambURL := ambiguous["url"]
		c.Check(!ambURL, "two a children must not imply a url")

		wrapped := mfProps(c, og.Microformats[4], "wrapped")
		c.Check(mfChild(c, mfList(c, wrapped, "photo", "wrapped"), 0, "wrapped.photo")["value"] == "https://example.invalid/dir/w.png", "wrapped implied photo")
		c.Check(reflect.DeepEqual(mfStrings(c, wrapped, "url", "wrapped"), []string{"https://example.invalid/dir/wlink"}), "wrapped implied url")

		deep := mfProps(c, og.Microformats[5], "deep")
		_, deepPhoto := deep["photo"]
		c.Check(!deepPhoto, "implied photo must not search arbitrary depth")

		nested := mfProps(c, og.Microformats[6], "nested")
		_, nestedName := nested["name"]
		c.Check(!nestedName, "nested root must suppress implied name")
		_, nestedPhoto := nested["photo"]
		c.Check(!nestedPhoto, "nested root must suppress implied photo/url")

		// Redirect: Fetch resolves against the final response URL.
		redirectPage := `<!doctype html><html><head></head><body>
<div class="h-card" id="r"><a class="p-name u-url" href="rel/me">R</a></div>
</body></html>`
		tr := &mfTransport{page: redirectPage, finalURL: "/final/dir/page"}
		fetchOG := New("https://origin.invalid/start")
		fetchOG.Intent.CollectMicroformats = true
		fetchOG.Intent.HTTPClient = &http.Client{Transport: tr}
		err := fetchOG.Fetch()
		c.Check(err == nil, "redirect Fetch error: %v", err)
		c.Check(fetchOG.Intent.URL == "https://origin.invalid/start", "Intent.URL must not be overwritten: %q", fetchOG.Intent.URL)
		if err == nil && len(fetchOG.Microformats) == 1 {
			r := mfProps(c, fetchOG.Microformats[0], "redirect")
			c.Check(reflect.DeepEqual(mfStrings(c, r, "url", "redirect"), []string{"https://origin.invalid/final/dir/rel/me"}), "redirect url: %v", mfStrings(c, r, "url", "redirect"))
		}

		// Same page via Parse and Walk with Intent.URL.
		parseOG := mfParse(c, redirectPage, "https://parse.invalid/a/b")
		doc, docErr := html.Parse(strings.NewReader(redirectPage))
		c.Check(docErr == nil, "html.Parse: %v", docErr)
		var walkedOG *OpenGraph
		if docErr == nil {
			walkedOG = New("https://walk.invalid/a/b")
			walkedOG.Intent.CollectMicroformats = true
			c.Check(walkedOG.Walk(doc) == nil, "Walk error")
		}
		if len(parseOG.Microformats) == 1 && walkedOG != nil && len(walkedOG.Microformats) == 1 {
			c.Check(strings.Contains(mfJSON(walkedOG.Microformats), "https://walk.invalid/a/rel/me"), "Walk base: %s", mfJSON(walkedOG.Microformats))
			c.Check(strings.Contains(mfJSON(parseOG.Microformats), "https://parse.invalid/a/rel/me"), "Parse base: %s", mfJSON(parseOG.Microformats))
			c.Check(mfJSON(parseOG.Microformats) != mfJSON(walkedOG.Microformats), "Parse and Walk with different Intent URLs should differ")
		}

		// A later <base> applies, and og:url is never the base.
		basePage := `<!doctype html><html><head>
<meta property="og:url" content="https://wrong.invalid/elsewhere/">
</head><body>
<div class="h-card"><a class="p-name u-url" href="rel/x">X</a></div>
<base href="https://based.invalid/root/">
</body></html>`
		baseOG := mfParse(c, basePage, "https://caller.invalid/page")
		c.Check(baseOG.URL == "https://wrong.invalid/elsewhere/", "og:url still parses normally: %q", baseOG.URL)
		if len(baseOG.Microformats) == 1 {
			bp := mfProps(c, baseOG.Microformats[0], "base")
			c.Check(reflect.DeepEqual(mfStrings(c, bp, "url", "base"), []string{"https://based.invalid/root/rel/x"}), "base href handling: %v", mfStrings(c, bp, "url", "base"))
		}

		// Walk only inspects the given subtree.
		scopePage := `<!doctype html><html><head></head><body>
<div id="outside"><div class="h-card"><span class="p-name">Outer</span></div></div>
<div id="inside"><div class="h-card"><span class="p-name">Inner</span></div></div>
</body></html>`
		scopeDoc, scopeErr := html.Parse(strings.NewReader(scopePage))
		c.Check(scopeErr == nil, "scope parse: %v", scopeErr)
		if scopeErr == nil {
			inside := mfFindElement(scopeDoc, "inside")
			c.Check(inside != nil, "inside node not found")
			if inside != nil {
				scopeOG := New("https://scope.invalid/")
				scopeOG.Intent.CollectMicroformats = true
				c.Check(scopeOG.Walk(inside) == nil, "scope Walk error")
				c.Check(len(scopeOG.Microformats) == 1, "Walk scope roots: got %d", len(scopeOG.Microformats))
				if len(scopeOG.Microformats) == 1 {
					c.Check(reflect.DeepEqual(mfStrings(c, mfProps(c, scopeOG.Microformats[0], "scope"), "name", "scope"), []string{"Inner"}), "Walk scope name")
				}
			}
		}

		// Re-collection replaces and never shares slices.
		repeatPage := `<!doctype html><html><head></head><body>
<div class="h-card"><span class="p-name">One</span><span class="p-nickname">N</span></div>
</body></html>`
		repeat := New("https://repeat.invalid/")
		repeat.Intent.CollectMicroformats = true
		c.Check(repeat.Parse(strings.NewReader(repeatPage)) == nil, "first repeat parse")
		first := repeat.Microformats
		c.Check(repeat.Parse(strings.NewReader(repeatPage)) == nil, "second repeat parse")
		second := repeat.Microformats
		c.Check(mfJSON(first) == mfJSON(second), "repeated collection differs")
		if len(first) == 1 && len(second) == 1 {
			fn := mfList(c, mfProps(c, first[0], "first"), "name", "first")
			sn := mfList(c, mfProps(c, second[0], "second"), "name", "second")
			c.Check(reflect.ValueOf(fn).Pointer() != reflect.ValueOf(sn).Pointer(), "repeated collection shares property slices")
			c.Check(fmt.Sprintf("%p", first[0]) != fmt.Sprintf("%p", second[0]), "repeated collection shares root objects")
		}

		// Failure keeps the previous Microformats and the existing OG fields.
		sentinel := []map[string]any{{"type": []any{"h-card"}, "properties": map[string]any{"name": []any{"old"}}}}
		keep := New("not-a-valid-url")
		keep.Intent.CollectMicroformats = true
		keep.Microformats = sentinel
		keep.Title = "Old title"
		c.Check(keep.Parse(strings.NewReader(repeatPage)) != nil, "non-absolute Intent.URL must fail with CollectMicroformats")
		c.Check(mfJSON(keep.Microformats) == mfJSON(sentinel), "failed collection must preserve Microformats")
		c.Check(keep.Title == "Old title", "failed collection must preserve existing fields")

		// Body read failure keeps the previous Microformats.
		trFail := &mfTransport{page: "ignored", bodyErr: errors.New("boom")}
		failOG := New("https://fail.invalid/page")
		failOG.Intent.CollectMicroformats = true
		failOG.Intent.HTTPClient = &http.Client{Transport: trFail}
		failOG.Microformats = sentinel
		c.Check(failOG.Fetch() != nil, "body read failure must return an error")
		c.Check(mfJSON(failOG.Microformats) == mfJSON(sentinel), "body read failure must preserve Microformats")

		// No roots: non-nil empty slice; switch off leaves the field alone.
		emptyOG := mfParse(c, `<!doctype html><html><head></head><body><p>nothing</p></body></html>`, "https://empty.invalid/")
		c.Check(emptyOG.Microformats != nil, "empty collection must be non-nil")
		c.Check(mfJSON(emptyOG.Microformats) == "[]", "empty collection must be an empty slice: %s", mfJSON(emptyOG.Microformats))

		off := New("https://off.invalid/")
		off.Microformats = sentinel
		c.Check(off.Parse(strings.NewReader(repeatPage)) == nil, "disabled parse error")
		c.Check(mfJSON(off.Microformats) == mfJSON(sentinel), "disabled collection must not touch Microformats")

		// Strict/TrustedTags behaviour is unchanged by the new switch.
		strictPage := `<html><head><meta property="og:title" content="T"><title>Doc</title></head><body><div class="h-card"><span class="p-name">S</span></div></body></html>`
		strict := New("https://strict.invalid/")
		strict.Intent.Strict = true
		strict.Intent.CollectMicroformats = true
		c.Check(strict.Parse(strings.NewReader(strictPage)) == nil, "strict parse")
		c.Check(strict.Title == "T", "strict title: %q", strict.Title)
		c.Check(len(strict.Microformats) == 1, "strict must still collect microformats")
	})
}
