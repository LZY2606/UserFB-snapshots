package browser

import (
	"bytes"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/PuerkitoBio/goquery"
	"github.com/headzoo/surf/jar"
	"github.com/headzoo/surf/task_support/taskcheck"
)

// recordedRequest keeps the raw wire bytes the test server received so the
// tests can assert on the actual submission encoding.
type recordedRequest struct {
	method       string
	rawQuery     string
	rawBody      string
	formValues   map[string]string
	multiValues  map[string]string
	multiFiles   map[string]string
	multiContent map[string][]byte
}

// charsetTestServer serves hand-written raw byte fixtures and records every
// submission made against it.
type charsetTestServer struct {
	mu       sync.Mutex
	requests map[string]*recordedRequest
	server   *httptest.Server
}

func newCharsetTestServer() *charsetTestServer {
	s := &charsetTestServer{requests: make(map[string]*recordedRequest)}
	s.server = httptest.NewServer(http.HandlerFunc(s.handle))
	return s
}

func (s *charsetTestServer) close() { s.server.Close() }

func (s *charsetTestServer) record(path string, rec *recordedRequest) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.requests[path] = rec
}

func (s *charsetTestServer) get(path string) *recordedRequest {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.requests[path]
}

// okPage is the plain UTF-8 answer returned by the submission endpoints.
var okPage = []byte(`<!doctype html><html><head><title>ok</title></head><body>done</body></html>`)

func (s *charsetTestServer) handle(w http.ResponseWriter, r *http.Request) {
	switch r.URL.Path {
	case "/cp1252":
		w.Header().Set("Content-Type", "text/html; charset=windows-1252")
		w.Write(cp1252Page)
		return
	case "/sjis":
		w.Header().Set("Content-Type", "text/html")
		w.Write(shiftJISPage)
		return
	case "/bom":
		// The header deliberately conflicts with the UTF-8 BOM: the BOM wins.
		w.Header().Set("Content-Type", "text/html; charset=windows-1252")
		w.Write(bomPage)
		return
	case "/conflict":
		// The HTTP alias (iso-8859-1) must beat the meta shift_jis claim.
		w.Header().Set("Content-Type", "text/html; charset=iso-8859-1")
		w.Write(conflictPage)
		return
	case "/header-bogus-meta":
		// The unknown HTTP charset is skipped, the valid meta declaration wins.
		w.Header().Set("Content-Type", "text/html; charset=totally-made-up")
		w.Write(headerBogusMetaPage)
		return
	case "/header-case":
		w.Header().Set("Content-Type", "text/html; charset=WINDOWS-1252")
		w.Write(cp1252TitlePage)
		return
	case "/utf8-alias":
		w.Header().Set("Content-Type", "text/html; charset=UTF8")
		w.Write(utf8TitlePage)
		return
	case "/latin1-alias":
		w.Header().Set("Content-Type", "text/html; charset=latin1")
		w.Write(cp1252TitlePage)
		return
	case "/meta-late":
		w.Header().Set("Content-Type", "text/html")
		w.Write(metaLatePage)
		return
	case "/page2":
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Write([]byte(`<!doctype html><title>page2</title>`))
		return
	}

	rec := &recordedRequest{method: r.Method, rawQuery: r.URL.RawQuery}
	switch {
	case strings.HasPrefix(r.Header.Get("Content-Type"), "multipart/form-data"):
		if err := r.ParseMultipartForm(10 << 20); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		rec.multiValues = make(map[string]string)
		rec.multiFiles = make(map[string]string)
		rec.multiContent = make(map[string][]byte)
		for name, vals := range r.MultipartForm.Value {
			rec.multiValues[name] = vals[0]
		}
		for name, headers := range r.MultipartForm.File {
			fh := headers[0]
			rec.multiFiles[name] = fh.Filename
			f, err := fh.Open()
			if err != nil {
				http.Error(w, err.Error(), http.StatusInternalServerError)
				return
			}
			data, err := io.ReadAll(f)
			f.Close()
			if err != nil {
				http.Error(w, err.Error(), http.StatusInternalServerError)
				return
			}
			rec.multiContent[name] = data
		}
	default:
		body, err := io.ReadAll(r.Body)
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		rec.rawBody = string(body)
		if err := r.ParseForm(); err == nil {
			rec.formValues = make(map[string]string)
			for name, vals := range r.Form {
				rec.formValues[name] = vals[0]
			}
		}
	}
	s.record(r.URL.Path, rec)
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Write(okPage)
}

// Raw byte fixtures. Every non-ASCII byte is written out explicitly so the
// served entity is encoded in the declared legacy charset rather than UTF-8.
var (
	// "caf\xe9" is café encoded in Windows-1252.
	cp1252Cafe = []byte{0x63, 0x61, 0x66, 0xe9}
	// "Go caf\xe9": the clicked button carries a legacy byte too.
	cp1252Button = []byte("Go caf\xe9")

	cp1252Page = []byte(`<!doctype html><html><head><title>` + string(cp1252Cafe) + `</title></head><body>` +
		// GET form with a hidden _charset_ control and a submit button.
		`<form method="get" action="/get">` +
		`<input type="text" name="q" value="` + string(cp1252Cafe) + `">` +
		`<input type="hidden" name="_charset_" value="ignored">` +
		`<input type="submit" name="go" value="` + string(cp1252Button) + `">` +
		`</form>` +
		// urlencoded POST form with a text field and a textarea.
		`<form method="post" action="/post">` +
		`<input type="text" name="q" value="` + string(cp1252Cafe) + `">` +
		`<textarea name="lines">line1
line2</textarea>` +
		`<input type="hidden" name="_charset_" value="ignored">` +
		`</form>` +
		// POST form whose accept-charset lists an unknown label first and a
		// UTF8 alias second: the first supported label must win.
		`<form method="post" action="/post-ac" accept-charset="bogus UTF8">` +
		`<input type="text" name="q" value="` + string(cp1252Cafe) + `">` +
		`<input type="hidden" name="_charset_" value="ignored">` +
		`</form>` +
		// POST form selecting Shift_JIS through accept-charset.
		`<form method="post" action="/post-ac" accept-charset="nope shift_jis windows-1252">` +
		`<input type="text" name="q" value="` + string(cp1252Cafe) + `">` +
		`<input type="hidden" name="_charset_" value="ignored">` +
		`</form>` +
		// multipart form with a text field, a file and _charset_.
		`<form method="post" action="/multi" enctype="multipart/form-data">` +
		`<input type="text" name="comment" value="">` +
		`<input type="file" name="upload">` +
		`<input type="hidden" name="_charset_" value="ignored">` +
		`</form>` +
		// _charset_ mixed with a non-hidden control: treated as an ordinary field.
		`<form method="post" action="/mixed">` +
		`<input type="text" name="_charset_" value="keep-me">` +
		`<input type="text" name="q" value="` + string(cp1252Cafe) + `">` +
		`</form>` +
		`</body></html>`)

	// Shift_JIS bytes: 日本 = 93 fa 96 7b.
	sjisNihongo = []byte{0x93, 0xfa, 0x96, 0x7b}

	shiftJISPage = []byte(`<!doctype html><head>` +
		// Conflicting declarations: the unknown one is skipped, the SJIS
		// alias (case-insensitive) is selected.
		`<meta charset="totally-made-up">` +
		`<meta charset="SJIS">` +
		`</head><body>` +
		`<form method="post" action="/post-sjis">` +
		`<input type="text" name="q" value="` + string(sjisNihongo) + `">` +
		`<input type="hidden" name="_charset_" value="ignored">` +
		`</form>` +
		`</body></html>`)

	bomPage = append([]byte{0xef, 0xbb, 0xbf},
		[]byte(`<!doctype html><title>caf`+string([]byte{0xc3, 0xa9})+`-utf8</title>`)...)

	conflictPage = []byte(`<!doctype html><head><meta charset="shift_jis"></head>` +
		`<title>` + string(cp1252Cafe) + `</title>`)

	headerBogusMetaPage = []byte(`<!doctype html><head><meta charset="windows-1252"></head>` +
		`<title>` + string(cp1252Cafe) + `</title>`)

	cp1252TitlePage = []byte(`<!doctype html><title>` + string(cp1252Cafe) + `</title>`)
	utf8TitlePage   = []byte(`<!doctype html><title>caf` + string([]byte{0xc3, 0xa9}) + `-utf8</title>`)

	// The meta declaration sits beyond the first 1024 entity bytes and must
	// therefore be ignored, leaving UTF-8 as the fallback.
	metaLatePage = append(
		append([]byte(`<!--`), bytes.Repeat([]byte{' '}, 1030)...),
		[]byte(`--><meta charset="windows-1252"><title>late</title>`)...,
	)
)

// TestFormDocumentCharset covers charset detection, DOM decoding and the raw
// byte contract of Download.
func TestFormDocumentCharset(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ts := newCharsetTestServer()
		defer ts.close()
		bow := newDefaultTestBrowser()

		// Before any page is opened the documented default is utf-8.
		c.Check(bow.DocumentCharset() == "utf-8", "fresh browser charset = %q, want utf-8", bow.DocumentCharset())

		mustOpen(t, bow, ts.server.URL+"/cp1252")
		c.Check(bow.DocumentCharset() == "windows-1252", "cp1252 page charset = %q", bow.DocumentCharset())
		c.Check(bow.Title() == "café", "cp1252 decoded title = %q", bow.Title())
		form, err := bow.Form("form[action='/post']")
		c.Check(err == nil, "cp1252 form lookup error: %v", err)
		value, err := form.Value("q")
		c.Check(err == nil, "cp1252 field lookup error: %v", err)
		c.Check(value == "café", "cp1252 decoded field value = %q", value)

		// Download keeps serving the decompressed original bytes.
		var download bytes.Buffer
		n, err := bow.Download(&download)
		c.Check(err == nil, "Download error: %v", err)
		c.Check(n == int64(len(cp1252Page)), "Download length = %d, want %d", n, len(cp1252Page))
		c.Check(bytes.Equal(download.Bytes(), cp1252Page), "Download must return raw windows-1252 bytes")

		mustOpen(t, bow, ts.server.URL+"/bom")
		c.Check(bow.DocumentCharset() == "utf-8", "BOM page charset = %q, want utf-8", bow.DocumentCharset())
		c.Check(bow.Title() == "café-utf8", "BOM page decoded title = %q", bow.Title())
		c.Check(!strings.ContainsRune(bow.Title(), '\uFEFF'), "BOM must not enter the DOM, title = %q", bow.Title())
		download.Reset()
		_, err = bow.Download(&download)
		c.Check(err == nil, "BOM Download error: %v", err)
		c.Check(bytes.HasPrefix(download.Bytes(), []byte{0xef, 0xbb, 0xbf}), "Download must keep the BOM bytes")

		mustOpen(t, bow, ts.server.URL+"/conflict")
		c.Check(bow.DocumentCharset() == "windows-1252", "HTTP charset must beat meta, got %q", bow.DocumentCharset())
		c.Check(bow.Title() == "café", "conflict page title = %q", bow.Title())

		mustOpen(t, bow, ts.server.URL+"/header-bogus-meta")
		c.Check(bow.DocumentCharset() == "windows-1252", "unknown HTTP charset should fall through to meta, got %q", bow.DocumentCharset())
		c.Check(bow.Title() == "café", "header-bogus/meta page title = %q", bow.Title())

		mustOpen(t, bow, ts.server.URL+"/header-case")
		c.Check(bow.DocumentCharset() == "windows-1252", "case-insensitive header charset got %q", bow.DocumentCharset())

		mustOpen(t, bow, ts.server.URL+"/latin1-alias")
		c.Check(bow.DocumentCharset() == "windows-1252", "latin1 alias got %q", bow.DocumentCharset())

		mustOpen(t, bow, ts.server.URL+"/utf8-alias")
		c.Check(bow.DocumentCharset() == "utf-8", "UTF8 alias got %q", bow.DocumentCharset())
		c.Check(bow.Title() == "café-utf8", "UTF8 alias title = %q", bow.Title())

		mustOpen(t, bow, ts.server.URL+"/sjis")
		c.Check(bow.DocumentCharset() == "shift_jis", "meta shift_jis page got %q", bow.DocumentCharset())
		sjisForm, err := bow.Form("form[action='/post-sjis']")
		c.Check(err == nil, "sjis form lookup error: %v", err)
		value, err = sjisForm.Value("q")
		c.Check(err == nil, "sjis field lookup error: %v", err)
		c.Check(value == "日本", "shift_jis decoded field = %q", value)

		mustOpen(t, bow, ts.server.URL+"/meta-late")
		c.Check(bow.DocumentCharset() == "utf-8", "late meta must be ignored, got %q", bow.DocumentCharset())
	})
}

// TestFormWireCharset covers the bytes produced for GET, urlencoded POST,
// multipart POST, buttons, _charset_, attachments and direct UTF-8 calls.
func TestFormWireCharset(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ts := newCharsetTestServer()
		defer ts.close()
		bow := newDefaultTestBrowser()
		mustOpen(t, bow, ts.server.URL+"/cp1252")

		// GET: clicked button value is transcoded too, spaces become '+'.
		getForm, err := bow.Form("form[action='/get']")
		c.Check(err == nil, "get form lookup: %v", err)
		c.Check(getForm.(*Form).documentCharset == "windows-1252", "form captured charset = %q", getForm.(*Form).documentCharset)
		c.Check(getForm.Click("go") == nil, "get form click error")
		rec := ts.get("/get")
		c.Check(rec != nil && rec.method == "GET", "GET request was not recorded")
		if rec != nil {
			c.Check(rec.rawQuery == "_charset_=windows-1252&go=Go+caf%E9&q=caf%E9",
				"GET raw query = %q", rec.rawQuery)
		}
		// The _charset_ substitution never writes back into Form.Get.
		c.Check(strings.Join(getForm.(*Form).Get("_charset_"), ",") == "ignored",
			"Form.Get(_charset_) mutated to %q", getForm.(*Form).Get("_charset_"))

		// urlencoded POST: unrepresentable rune becomes a decimal character
		// reference, LF line endings become CRLF on the wire.
		mustOpen(t, bow, ts.server.URL+"/cp1252")
		postForm, err := bow.Form("form[action='/post']")
		c.Check(err == nil, "post form lookup: %v", err)
		c.Check(postForm.Input("q", "a😀b") == nil, "post form input error")
		c.Check(postForm.Input("lines", "line1\nline2") == nil, "textarea input error")
		c.Check(postForm.Submit() == nil, "post form submit error")
		rec = ts.get("/post")
		c.Check(rec != nil, "POST request was not recorded")
		if rec != nil {
			c.Check(rec.rawBody == "_charset_=windows-1252&lines=line1%0D%0Aline2&q=a%26%23128512%3Bb",
				"urlencoded POST body = %q", rec.rawBody)
			c.Check(rec.formValues["q"] == "a&#128512;b", "server parsed q = %q", rec.formValues["q"])
		}
		c.Check(strings.Join(postForm.(*Form).Get("q"), ",") == "a😀b", "Form.Get(q) mutated to %q", postForm.(*Form).Get("q"))

		// accept-charset: unknown label skipped, UTF8 alias selected.
		mustOpen(t, bow, ts.server.URL+"/cp1252")
		acForm, err := bow.Form("form[action='/post-ac'][accept-charset='bogus UTF8']")
		c.Check(err == nil, "accept-charset utf8 form lookup: %v", err)
		c.Check(acForm.Submit() == nil, "accept-charset utf8 submit error")
		rec = ts.get("/post-ac")
		c.Check(rec != nil, "accept-charset POST not recorded")
		if rec != nil {
			// é in UTF-8 is C3 A9; _charset_ reports the canonical utf-8 name.
			c.Check(rec.rawBody == "_charset_=utf-8&q=caf%C3%A9", "utf-8 accept-charset body = %q", rec.rawBody)
		}

		// accept-charset: first supported label shift_jis wins over windows-1252.
		mustOpen(t, bow, ts.server.URL+"/cp1252")
		sjisACForm, err := bow.Form("form[accept-charset='nope shift_jis windows-1252']")
		c.Check(err == nil, "accept-charset sjis form lookup: %v", err)
		c.Check(sjisACForm.Input("q", "日本") == nil, "sjis accept-charset input error")
		c.Check(sjisACForm.Submit() == nil, "sjis accept-charset submit error")
		rec = ts.get("/post-ac")
		c.Check(rec != nil, "sjis accept-charset POST not recorded")
		if rec != nil {
			c.Check(rec.rawBody == "_charset_=shift_jis&q=%93%FA%96%7B", "shift_jis accept-charset body = %q", rec.rawBody)
		}

		// A _charset_ control that is not entirely hidden behaves as a normal field.
		mustOpen(t, bow, ts.server.URL+"/cp1252")
		mixedForm, err := bow.Form("form[action='/mixed']")
		c.Check(err == nil, "mixed _charset_ form lookup: %v", err)
		c.Check(mixedForm.Submit() == nil, "mixed form submit error")
		rec = ts.get("/mixed")
		c.Check(rec != nil, "mixed POST not recorded")
		if rec != nil {
			c.Check(rec.rawBody == "_charset_=keep-me&q=caf%E9", "mixed _charset_ body = %q", rec.rawBody)
		}

		// Multipart: text fields follow the text rules, files keep raw bytes.
		mustOpen(t, bow, ts.server.URL+"/cp1252")
		multiForm, err := bow.Form("form[action='/multi']")
		c.Check(err == nil, "multipart form lookup: %v", err)
		c.Check(multiForm.Input("comment", "a😀b\nsecond") == nil, "multipart comment input error")
		fileBytes := []byte{0x00, 0x01, 0x7e, 0x80, 0xff, 0x0a}
		c.Check(multiForm.File("upload", "upload.bin", bytes.NewReader(fileBytes)) == nil, "file set error")
		c.Check(multiForm.Submit() == nil, "multipart submit error")
		rec = ts.get("/multi")
		c.Check(rec != nil, "multipart POST not recorded")
		if rec != nil {
			c.Check(rec.multiValues["comment"] == "a&#128512;b\r\nsecond",
				"multipart comment wire text = %q", rec.multiValues["comment"])
			c.Check(rec.multiValues["_charset_"] == "windows-1252",
				"multipart _charset_ = %q", rec.multiValues["_charset_"])
			c.Check(rec.multiFiles["upload"] == "upload.bin", "multipart filename = %q", rec.multiFiles["upload"])
			c.Check(bytes.Equal(rec.multiContent["upload"], fileBytes), "multipart file bytes changed")
		}

		// Shift_JIS document: representable runes use Shift_JIS bytes, the
		// emoji falls back to a decimal numeric character reference.
		mustOpen(t, bow, ts.server.URL+"/sjis")
		sjisForm, err := bow.Form("form[action='/post-sjis']")
		c.Check(err == nil, "sjis wire form lookup: %v", err)
		c.Check(sjisForm.Input("q", "カフェ😀") == nil, "sjis input error")
		c.Check(sjisForm.Submit() == nil, "sjis submit error")
		rec = ts.get("/post-sjis")
		c.Check(rec != nil, "shift_jis POST not recorded")
		if rec != nil {
			c.Check(rec.rawBody == "_charset_=shift_jis&q=%83J%83t%83F%26%23128512%3B",
				"shift_jis POST body = %q", rec.rawBody)
		}

		// Direct browser calls keep their historical UTF-8 convention.
		c.Check(bow.PostForm(ts.server.URL+"/direct", url.Values{"q": {"café"}}) == nil, "direct PostForm error")
		rec = ts.get("/direct")
		c.Check(rec != nil, "direct POST not recorded")
		if rec != nil {
			c.Check(rec.rawBody == "q=caf%C3%A9", "direct PostForm body = %q", rec.rawBody)
		}

		// A custom Browsable implementation without encoding information
		// never opts into the legacy charset submission path: its forms
		// default to utf-8 and use the original UTF-8 entry points.
		fake := newFakeBrowsable()
		fakeForm := NewForm(fake, fake.doc.Find("form").First())
		c.Check(fakeForm.documentCharset == "utf-8", "custom browser form charset = %q", fakeForm.documentCharset)
		c.Check(fakeForm.Input("q", "café") == nil, "custom browser form input error")
		c.Check(fakeForm.Submit() == nil, "custom browser form submit error")
		c.Check(fake.postBody == "q=caf%C3%A9", "custom browser submission body = %q", fake.postBody)
	})
}

// TestFormCharsetHistory covers Back() charset restoration, forms keeping
// their captured charset across navigation, and browser independence.
func TestFormCharsetHistory(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ts := newCharsetTestServer()
		defer ts.close()
		bow := newDefaultTestBrowser()

		mustOpen(t, bow, ts.server.URL+"/cp1252")
		legacyForm, err := bow.Form("form[action='/post']")
		c.Check(err == nil, "legacy form lookup: %v", err)

		// Navigate elsewhere: the browser now speaks UTF-8, but the form
		// captured earlier must still submit as windows-1252.
		mustOpen(t, bow, ts.server.URL+"/page2")
		c.Check(bow.DocumentCharset() == "utf-8", "page2 charset = %q", bow.DocumentCharset())
		c.Check(legacyForm.(*Form).documentCharset == "windows-1252",
			"old form charset changed to %q", legacyForm.(*Form).documentCharset)
		c.Check(legacyForm.Submit() == nil, "legacy form submit after navigation error")
		rec := ts.get("/post")
		c.Check(rec != nil, "legacy cross-page POST not recorded")
		if rec != nil {
			c.Check(strings.Contains(rec.rawBody, "_charset_=windows-1252"),
				"old form submitted as %q", rec.rawBody)
		}

		// The submission pushed another state: walk back through page2 to
		// the original windows-1252 page. Back restores DOM and charset together.
		c.Check(bow.Back() == true, "first Back should return to page2")
		c.Check(bow.DocumentCharset() == "utf-8", "page2 restored charset = %q", bow.DocumentCharset())
		c.Check(bow.Back() == true, "second Back should return to the legacy page")
		c.Check(bow.DocumentCharset() == "windows-1252", "Back restored charset = %q", bow.DocumentCharset())
		c.Check(bow.Title() == "café", "Back restored DOM title = %q", bow.Title())
		// Only the initial empty state remains: Back must report false.
		c.Check(bow.Back() == false, "Back should be false at the bottom of the history")

		// A form created from the restored page still carries its charset.
		restoredForm, err := bow.Form("form[action='/post']")
		c.Check(err == nil, "restored form lookup: %v", err)
		c.Check(restoredForm.(*Form).documentCharset == "windows-1252",
			"restored form charset = %q", restoredForm.(*Form).documentCharset)
		c.Check(restoredForm.Submit() == nil, "restored form submit error")
		rec = ts.get("/post")
		c.Check(rec != nil, "restored POST not recorded")
		if rec != nil {
			c.Check(strings.Contains(rec.rawBody, "_charset_=windows-1252"),
				"restored form submitted as %q", rec.rawBody)
		}

		// Two browsers do not influence each other's document charset.
		bow1 := newDefaultTestBrowser()
		bow2 := newDefaultTestBrowser()
		mustOpen(t, bow1, ts.server.URL+"/cp1252")
		mustOpen(t, bow2, ts.server.URL+"/sjis")
		c.Check(bow1.DocumentCharset() == "windows-1252", "browser1 charset = %q", bow1.DocumentCharset())
		c.Check(bow2.DocumentCharset() == "shift_jis", "browser2 charset = %q", bow2.DocumentCharset())
		form1, err := bow1.Form("form[action='/post']")
		c.Check(err == nil, "browser1 form lookup: %v", err)
		form2, err := bow2.Form("form[action='/post-sjis']")
		c.Check(err == nil, "browser2 form lookup: %v", err)
		// Navigating browser2 onto the cp1252 page leaves form2 pinned to shift_jis.
		mustOpen(t, bow2, ts.server.URL+"/cp1252")
		c.Check(bow2.DocumentCharset() == "windows-1252", "browser2 new page charset = %q", bow2.DocumentCharset())
		c.Check(form2.(*Form).documentCharset == "shift_jis", "form2 charset changed to %q", form2.(*Form).documentCharset)
		c.Check(form1.(*Form).documentCharset == "windows-1252", "form1 charset changed to %q", form1.(*Form).documentCharset)
		c.Check(form2.Submit() == nil, "pinned shift_jis form submit error")
		rec = ts.get("/post-sjis")
		c.Check(rec != nil, "pinned shift_jis POST not recorded")
		if rec != nil {
			c.Check(strings.Contains(rec.rawBody, "_charset_=shift_jis"), "pinned form body = %q", rec.rawBody)
		}
	})
}

func mustOpen(t *testing.T, bow *Browser, u string) {
	t.Helper()
	if err := bow.Open(u); err != nil {
		t.Fatalf("Open(%s) failed: %v", u, err)
	}
}

// fakeBrowsable is a hand-written Browsable implementation that carries no
// charset information. It records how a form submits so the UTF-8 fallback
// path can be asserted without a network server.
type fakeBrowsable struct {
	doc       *goquery.Document
	pageURL   *url.URL
	postBody  string
	getQuery  string
	multipart bool
}

func newFakeBrowsable() *fakeBrowsable {
	page := `<html><body><form method="post" action="/submit">` +
		`<input type="text" name="q" value=""></form></body></html>`
	doc, err := goquery.NewDocumentFromReader(strings.NewReader(page))
	if err != nil {
		panic(err)
	}
	u, _ := url.Parse("http://custom.example/page")
	return &fakeBrowsable{doc: doc, pageURL: u}
}

func (f *fakeBrowsable) SetUserAgent(string)                       {}
func (f *fakeBrowsable) SetAttribute(Attribute, bool)              {}
func (f *fakeBrowsable) SetAttributes(AttributeMap)                {}
func (f *fakeBrowsable) SetState(*jar.State)                       {}
func (f *fakeBrowsable) State() *jar.State                         { return nil }
func (f *fakeBrowsable) SetBookmarksJar(jar.BookmarksJar)          {}
func (f *fakeBrowsable) BookmarksJar() jar.BookmarksJar            { return nil }
func (f *fakeBrowsable) SetCookieJar(http.CookieJar)               {}
func (f *fakeBrowsable) CookieJar() http.CookieJar                 { return nil }
func (f *fakeBrowsable) SetHistoryJar(jar.History)                 {}
func (f *fakeBrowsable) HistoryJar() jar.History                   { return nil }
func (f *fakeBrowsable) SetHeadersJar(http.Header)                 {}
func (f *fakeBrowsable) SetTimeout(time.Duration)                  {}
func (f *fakeBrowsable) SetTransport(http.RoundTripper)            {}
func (f *fakeBrowsable) AddRequestHeader(string, string)           {}
func (f *fakeBrowsable) Open(string) error                         { return nil }
func (f *fakeBrowsable) Head(string) error                         { return nil }
func (f *fakeBrowsable) OpenBookmark(string) error                 { return nil }
func (f *fakeBrowsable) Post(string, string, io.Reader) error      { return nil }
func (f *fakeBrowsable) Back() bool                                { return false }
func (f *fakeBrowsable) Reload() error                             { return nil }
func (f *fakeBrowsable) Bookmark(string) error                     { return nil }
func (f *fakeBrowsable) Click(string) error                        { return nil }
func (f *fakeBrowsable) Form(string) (Submittable, error)          { return nil, nil }
func (f *fakeBrowsable) Forms() []Submittable                      { return nil }
func (f *fakeBrowsable) Links() []*Link                            { return nil }
func (f *fakeBrowsable) Images() []*Image                          { return nil }
func (f *fakeBrowsable) Stylesheets() []*Stylesheet                { return nil }
func (f *fakeBrowsable) Scripts() []*Script                        { return nil }
func (f *fakeBrowsable) SiteCookies() []*http.Cookie               { return nil }
func (f *fakeBrowsable) ResolveStringUrl(u string) (string, error) { return u, nil }
func (f *fakeBrowsable) Download(io.Writer) (int64, error)         { return 0, nil }
func (f *fakeBrowsable) StatusCode() int                           { return 200 }
func (f *fakeBrowsable) Title() string                             { return "" }
func (f *fakeBrowsable) ResponseHeaders() http.Header              { return http.Header{} }
func (f *fakeBrowsable) Body() string                              { return "" }
func (f *fakeBrowsable) Dom() *goquery.Selection                   { return f.doc.Selection }
func (f *fakeBrowsable) Find(expr string) *goquery.Selection       { return f.doc.Find(expr) }
func (f *fakeBrowsable) NewTab() *Browser                          { return nil }
func (f *fakeBrowsable) Url() *url.URL                             { return f.pageURL }
func (f *fakeBrowsable) ResolveUrl(u *url.URL) *url.URL            { return f.pageURL.ResolveReference(u) }

func (f *fakeBrowsable) OpenForm(u string, data url.Values) error {
	f.getQuery = data.Encode()
	return nil
}

func (f *fakeBrowsable) PostForm(u string, data url.Values) error {
	f.postBody = data.Encode()
	return nil
}

func (f *fakeBrowsable) PostMultipart(u string, fields url.Values, files FileSet) error {
	f.multipart = true
	return nil
}
