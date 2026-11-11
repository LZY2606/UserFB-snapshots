package xmlquery

import (
	"encoding/xml"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"reflect"
	"strings"
	"testing"
)

// byteReader serves its input one byte per Read call.
type byteReader struct {
	data string
	pos  int
}

func (r *byteReader) Read(p []byte) (int, error) {
	if r.pos >= len(r.data) {
		return 0, io.EOF
	}
	p[0] = r.data[r.pos]
	r.pos++
	return 1, nil
}

// cappedReader serves at most cap bytes and then fails with err, proving
// the stream did not need to read further.
type cappedReader struct {
	data string
	pos  int
	cap  int
	err  error
}

func (r *cappedReader) Read(p []byte) (int, error) {
	if r.pos >= r.cap {
		return 0, r.err
	}
	n := len(p)
	if remain := r.cap - r.pos; n > remain {
		n = remain
	}
	if remain := len(r.data) - r.pos; n > remain {
		n = remain
	}
	copy(p, r.data[r.pos:r.pos+n])
	r.pos += n
	if n == 0 {
		return 0, io.EOF
	}
	return n, nil
}

// noReadReader fails the test if anything is ever read from it.
type noReadReader struct{ t *testing.T }

func (r noReadReader) Read([]byte) (int, error) {
	r.t.Fatal("reader must not be touched")
	return 0, errors.New("must not be read")
}

func liveNodeCount(n *Node) int {
	count := 0
	for child := n.FirstChild; child != nil; child = child.NextSibling {
		count += 1 + liveNodeCount(child)
	}
	return count
}

func readAllRecords(t *testing.T, rs *RecordStream) []*RecordMatch {
	t.Helper()
	var out []*RecordMatch
	for {
		m, err := rs.Read()
		if err == io.EOF {
			return out
		}
		if err != nil {
			t.Fatal(err)
		}
		out = append(out, m)
	}
}

func TestRecordStreamContract(t *testing.T) {
	t.Run("Overlap", testRecordStreamOverlap)
	t.Run("Context", testRecordStreamContext)
	t.Run("Prune", testRecordStreamPrune)
}

func testRecordStreamOverlap(t *testing.T) {
	const body = `<catalog xmlns:p="urn:records">` +
		`<p:item kind="part" id="outer"><p:item kind="part" id="inner">IN</p:item><v>TAIL</v></p:item>` +
		`<p:item kind="part" id="last">LAST</p:item>` +
		`</catalog>`
	rules := []RecordRule{
		{Name: "item", Path: "/catalog//p:item"},
		{Name: "part", Path: "//p:item[@kind='part']"},
	}
	namespaces := map[string]string{"p": "urn:records"}
	rs, err := NewRecordStream(strings.NewReader(body), rules, namespaces)
	if err != nil {
		t.Fatal(err)
	}
	// Mutating the caller's slices and maps must not affect the stream.
	rules[0].Path = "/nope"
	rules[1].Name = "nope"
	namespaces["p"] = "urn:elsewhere"

	var got []struct {
		rule, id string
	}
	var locs [][]int
	var matches []*RecordMatch
	for {
		m, err := rs.Read()
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		matches = append(matches, m)
		got = append(got, struct {
			rule, id string
		}{m.Rule, m.Node.SelectAttr("id")})
		locs = append(locs, m.Location)
	}
	want := []struct {
		rule, id string
	}{
		{"item", "inner"}, {"part", "inner"},
		{"item", "outer"}, {"part", "outer"},
		{"item", "last"}, {"part", "last"},
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("delivery order = %v, want %v", got, want)
	}
	wantLocs := [][]int{{1, 1, 1}, {1, 1, 1}, {1, 1}, {1, 1}, {1, 2}, {1, 2}}
	for i, loc := range locs {
		if !reflect.DeepEqual(loc, wantLocs[i]) {
			t.Errorf("record %d location = %v, want %v", i, loc, wantLocs[i])
		}
	}
	for i, m := range matches {
		if m.Node.Parent != nil || m.Node.PrevSibling != nil || m.Node.NextSibling != nil {
			t.Errorf("record %d root is not detached", i)
		}
	}

	innerA, innerB := matches[0], matches[1]
	// Records for the same node share no mutable state.
	innerA.Node.SetAttr("id", "MUT")
	RemoveFromTree(innerA.Node.FirstChild)
	if id := innerB.Node.SelectAttr("id"); id != "inner" {
		t.Errorf("sibling record id = %q after mutation", id)
	}
	if text := innerB.Node.InnerText(); text != "IN" {
		t.Errorf("sibling record text = %q after mutation", text)
	}
	AddAttr(innerB.Node, "extra", "1")
	if len(innerA.Node.Attr) == len(innerB.Node.Attr) {
		t.Errorf("records share attribute storage")
	}

	// Mutating the already delivered inner records must not change the
	// outer record delivered later.
	outer := matches[2].Node
	var inner, tail *Node
	for _, child := range outer.ChildNodes() {
		switch child.Data {
		case "item":
			inner = child
		case "v":
			tail = child
		}
	}
	if inner == nil || inner.SelectAttr("id") != "inner" || inner.InnerText() != "IN" {
		t.Errorf("outer record lost or altered inner record: %v", outer.OutputXML(true))
	}
	if tail == nil || tail.InnerText() != "TAIL" {
		t.Errorf("outer record lost TAIL: %v", outer.OutputXML(true))
	}

	if _, err := rs.Read(); err != io.EOF {
		t.Fatalf("want EOF, got %v", err)
	}
	if _, err := rs.Read(); err != io.EOF {
		t.Fatalf("EOF not stable, got %v", err)
	}
	t.Log("RECORD_OVERLAP_OK")
}

func testRecordStreamContext(t *testing.T) {
	const body = `<root xmlns:a="urn:x" xml:space="preserve">` +
		`<skip/>` +
		`<wrap xmlns:b="urn:x"><b:entry>  spaced  </b:entry></wrap>` +
		`<skip/>` +
		`<a:entry xmlns:c="urn:y"><c:sub c:k="v"/><![CDATA[raw <>&]]><!--note--></a:entry>` +
		`</root>`
	rs, err := NewRecordStream(strings.NewReader(body),
		[]RecordRule{{Name: "entry", Path: "//p:entry"}},
		map[string]string{"p": "urn:x"})
	if err != nil {
		t.Fatal(err)
	}
	matches := readAllRecords(t, rs)
	if len(matches) != 2 {
		t.Fatalf("got %d records, want 2", len(matches))
	}

	// The document picked aliases a and b; the rule picked p. Both win.
	first, second := matches[0], matches[1]
	if !reflect.DeepEqual(first.Location, []int{1, 2, 1}) {
		t.Errorf("first location = %v", first.Location)
	}
	if !reflect.DeepEqual(second.Location, []int{1, 4}) {
		t.Errorf("second location = %v", second.Location)
	}
	if first.Node.Prefix != "b" || first.Node.NamespaceURI != "urn:x" {
		t.Errorf("first record = %s:%s (%s)", first.Node.Prefix, first.Node.Data, first.Node.NamespaceURI)
	}
	// Inherited xml:space must survive detachment.
	if space := first.Node.SelectAttr("xml:space"); space != "preserve" {
		t.Errorf("inherited xml:space = %q", space)
	}
	if text := first.Node.InnerText(); text != "  spaced  " {
		t.Errorf("preserved text = %q", text)
	}

	// The detached records must reparse to the same expanded names.
	type start struct {
		space, local string
		attrs        map[string]string
	}
	collect := func(text string) []start {
		var out []start
		decoder := xml.NewDecoder(strings.NewReader(text))
		for {
			tok, err := decoder.Token()
			if err == io.EOF {
				return out
			}
			if err != nil {
				t.Fatalf("record does not reparse: %v\n%s", err, text)
			}
			if se, ok := tok.(xml.StartElement); ok {
				s := start{space: se.Name.Space, local: se.Name.Local, attrs: map[string]string{}}
				for _, a := range se.Attr {
					s.attrs[a.Name.Space+"|"+a.Name.Local] = a.Value
				}
				out = append(out, s)
			}
		}
	}
	gotFirst := collect(first.Node.OutputXML(true))
	if len(gotFirst) != 1 || gotFirst[0].space != "urn:x" || gotFirst[0].local != "entry" {
		t.Errorf("first record reparsed as %+v", gotFirst)
	}
	gotSecond := collect(second.Node.OutputXML(true))
	if len(gotSecond) != 2 || gotSecond[0].space != "urn:x" || gotSecond[1].space != "urn:y" {
		t.Errorf("second record reparsed as %+v", gotSecond)
	}
	if gotSecond[1].attrs["urn:y|k"] != "v" {
		t.Errorf("rebound attribute lost: %+v", gotSecond[1].attrs)
	}

	// Text, CDATA and comments keep the usual Node behavior inside records.
	var cdata, comment *Node
	for _, child := range second.Node.ChildNodes() {
		switch child.Type {
		case CharDataNode:
			cdata = child
		case CommentNode:
			comment = child
		}
	}
	if cdata == nil || cdata.Data != "raw <>&" {
		t.Errorf("CDATA not preserved: %v", cdata)
	}
	if comment == nil || comment.Data != "note" {
		t.Errorf("comment not preserved: %v", comment)
	}

	// Unprefixed names in rules only match elements without a namespace.
	rs, err = NewRecordStream(strings.NewReader(`<root xmlns="urn:d"><plain/></root>`),
		[]RecordRule{{Name: "plain", Path: "//plain"}}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := rs.Read(); err != io.EOF {
		t.Errorf("unprefixed rule matched a namespaced element: %v", err)
	}
	rs, err = NewRecordStream(strings.NewReader(`<root><plain/></root>`),
		[]RecordRule{{Name: "plain", Path: "//plain"}}, nil)
	if err != nil {
		t.Fatal(err)
	}
	m, err := rs.Read()
	if err != nil || m.Node.Data != "plain" {
		t.Errorf("unprefixed rule did not match namespace-free element: %v", err)
	}

	// Configuration errors are reported before the reader is touched.
	reader := noReadReader{t: t}
	bad := [][]RecordRule{
		{{Name: "", Path: "/a"}},
		{{Name: "x", Path: "/a"}, {Name: "x", Path: "/b"}},
		{{Name: "x", Path: ""}},
		{{Name: "x", Path: "a/b"}},
		{{Name: "x", Path: "/a/"}},
		{{Name: "x", Path: "/q:a"}},
		{{Name: "x", Path: "//a[text()='v']"}},
		{{Name: "x", Path: "//a[1]"}},
		{{Name: "x", Path: "//a[contains(@k, 'v')]"}},
		{{Name: "x", Path: "//a | //b"}},
		{{Name: "x", Path: "//child::a"}},
		{{Name: "x", Path: "//@a"}},
		{{Name: "x", Path: "//a[@k]"}},
		{{Name: "x", Path: "//a[@q:k='v']"}},
	}
	for i, rules := range bad {
		if _, err := NewRecordStream(reader, rules, nil); err == nil {
			t.Errorf("bad config %d (%v) accepted", i, rules)
		}
	}
	t.Log("RECORD_CONTEXT_OK")
}

func testRecordStreamPrune(t *testing.T) {
	// The first record must be returned without reading the rest of the
	// input: the reader fails everything past the first </item>.
	const head = `<root><item>A</item>`
	const tail = `<item>B</item><item>C</item></root>`
	blocked := errors.New("read blocked")
	capped := &cappedReader{data: head + tail, cap: len(head), err: blocked}
	rs, err := NewRecordStream(capped, []RecordRule{{Name: "item", Path: "/root/item"}}, nil)
	if err != nil {
		t.Fatal(err)
	}
	m, err := rs.Read()
	if err != nil {
		t.Fatalf("first record was not delivered before the input ran out: %v", err)
	}
	if m.Node.InnerText() != "A" || !reflect.DeepEqual(m.Location, []int{1, 1}) {
		t.Errorf("first record = %q %v", m.Node.InnerText(), m.Location)
	}
	if _, err := rs.Read(); err != blocked {
		t.Fatalf("want the reader error, got %v", err)
	}
	if _, err := rs.Read(); err != blocked {
		t.Fatalf("terminal error not stable, got %v", err)
	}

	// Byte-by-byte input drives the same state machine.
	rs, err = NewRecordStream(&byteReader{data: `<root><item>A</item><item>B</item></root>`},
		[]RecordRule{{Name: "item", Path: "/root/item"}}, nil)
	if err != nil {
		t.Fatal(err)
	}
	var texts []string
	for _, m := range readAllRecords(t, rs) {
		texts = append(texts, m.Node.InnerText())
	}
	if !reflect.DeepEqual(texts, []string{"A", "B"}) {
		t.Errorf("byte reader records = %v", texts)
	}

	// A corrupt tail: complete records stay delivered, the unclosed one
	// is dropped, and the terminal error is stable.
	rs, err = NewRecordStream(strings.NewReader(`<root><item>A</item><item>B`),
		[]RecordRule{{Name: "item", Path: "/root/item"}}, nil)
	if err != nil {
		t.Fatal(err)
	}
	m, err = rs.Read()
	if err != nil || m.Node.InnerText() != "A" {
		t.Fatalf("record before corruption = %v, %v", m, err)
	}
	_, err1 := rs.Read()
	if err1 == nil || err1 == io.EOF {
		t.Fatalf("want a syntax error for the corrupt tail, got %v", err1)
	}
	_, err2 := rs.Read()
	if err2 == nil || err2.Error() != err1.Error() {
		t.Fatalf("terminal error not stable: %v then %v", err1, err2)
	}

	// A failing reader surfaces its own error, stably.
	boom := errors.New("boom")
	rs, err = NewRecordStream(&cappedReader{data: `<root><item>A</item><item>B</item></root>`, cap: 10, err: boom},
		[]RecordRule{{Name: "item", Path: "/root/item"}}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := rs.Read(); err != boom {
		t.Fatalf("want boom, got %v", err)
	}
	if _, err := rs.Read(); err != boom {
		t.Fatalf("reader failure not stable, got %v", err)
	}

	// Closed subtrees that did not match must not pile up on the live
	// tree; only open ancestors and the subtrees being captured remain.
	var b strings.Builder
	b.WriteString(`<root>`)
	for i := 0; i < 500; i++ {
		fmt.Fprintf(&b, `<junk a="%d"><deep><deeper>x</deeper></deep></junk>`, i)
		if i%50 == 0 {
			b.WriteString(`<item/>`)
		}
	}
	b.WriteString(`</root>`)
	rs, err = NewRecordStream(strings.NewReader(b.String()),
		[]RecordRule{{Name: "item", Path: "/root/item"}}, nil)
	if err != nil {
		t.Fatal(err)
	}
	items := 0
	for {
		m, err := rs.Read()
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		items++
		if live := liveNodeCount(rs.doc); live > 8 {
			t.Fatalf("live tree holds %d nodes after %d records", live, items)
		}
		if m.Node.Data != "item" {
			t.Fatalf("record = %q", m.Node.Data)
		}
	}
	if items != 10 {
		t.Fatalf("got %d items, want 10", items)
	}
	if rs.doc.FirstChild != nil || len(rs.stack) != 0 {
		t.Fatalf("stream still holds the document: %d nodes, %d open", liveNodeCount(rs.doc), len(rs.stack))
	}
	t.Log("RECORD_PRUNE_OK")
}

func ExampleNewRecordStream() {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/xml")
		io.WriteString(w, `<catalog xmlns:p="urn:records">`+
			`<p:item kind="part"><name>bolt</name></p:item>`+
			`<p:item kind="raw"><name>ore</name></p:item>`+
			`</catalog>`)
	}))
	defer srv.Close()

	resp, err := http.Get(srv.URL)
	if err != nil {
		panic(err)
	}
	defer resp.Body.Close()

	rs, err := NewRecordStream(resp.Body,
		[]RecordRule{{Name: "part", Path: "/catalog//p:item[@kind='part']"}},
		map[string]string{"p": "urn:records"})
	if err != nil {
		panic(err)
	}
	for {
		m, err := rs.Read()
		if err == io.EOF {
			break
		}
		if err != nil {
			panic(err)
		}
		fmt.Println(m.Rule, m.Location, m.Node.SelectElement("name").InnerText())
	}
	// Output: part [1 1] bolt
}
