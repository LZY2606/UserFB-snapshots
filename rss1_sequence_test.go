package gofeed_test

import (
	"context"
	"errors"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"

	"github.com/mmcdole/gofeed"
	"github.com/mmcdole/gofeed/rss"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

const (
	publicRDFNS = "http://www.w3.org/1999/02/22-rdf-syntax-ns#"
	publicRSSNS = "http://purl.org/rss/1.0/"
)

// publicSequenceDoc is the baseline layout again: declared order B, A with
// the items physically stored A, Unused, B.
const publicSequenceDoc = `<rdf:RDF xmlns:rdf="` + publicRDFNS + `" xmlns="` + publicRSSNS + `">
<channel rdf:about="urn:feed">
<title>Discovery</title>
<link>https://source.invalid/</link>
<description>Local</description>
<items><rdf:Seq><rdf:li rdf:resource="urn:b"/><rdf:li rdf:resource="urn:a"/></rdf:Seq></items>
</channel>
<item rdf:about="urn:a"><title>A</title><link>https://source.invalid/a</link></item>
<item rdf:about="urn:unused"><title>Unused</title></item>
<item rdf:about="urn:b"><title>B</title><link>https://source.invalid/b</link></item>
</rdf:RDF>`

const publicSequenceDocPrefixed = `<rr:RDF xmlns:rr="` + publicRDFNS + `" xmlns:ten="` + publicRSSNS + `">
<ten:channel rr:about="urn:feed">
<ten:title>Discovery</ten:title>
<ten:link>https://source.invalid/</ten:link>
<ten:description>Local</ten:description>
<ten:items><rr:Seq><rr:li rr:resource="urn:b"/><rr:li rr:resource="urn:a"/></rr:Seq></ten:items>
</ten:channel>
<ten:item rr:about="urn:a"><ten:title>A</ten:title><ten:link>https://source.invalid/a</ten:link></ten:item>
<ten:item rr:about="urn:unused"><ten:title>Unused</ten:title></ten:item>
<ten:item rr:about="urn:b"><ten:title>B</ten:title><ten:link>https://source.invalid/b</ten:link></ten:item>
</rr:RDF>`

const publicEmptySeqDoc = `<rdf:RDF xmlns:rdf="` + publicRDFNS + `" xmlns="` + publicRSSNS + `">
<channel rdf:about="urn:feed">
<title>Discovery</title><link>https://source.invalid/</link><description>Local</description>
<items><rdf:Seq></rdf:Seq></items>
</channel>
<item rdf:about="urn:a"><title>A</title></item>
</rdf:RDF>`

const publicBadRefDoc = `<rdf:RDF xmlns:rdf="` + publicRDFNS + `" xmlns="` + publicRSSNS + `">
<channel rdf:about="urn:feed">
<title>Discovery</title><link>https://source.invalid/</link><description>Local</description>
<items><rdf:Seq><rdf:li rdf:resource="urn:missing"/></rdf:Seq></items>
</channel>
<item rdf:about="urn:a"><title>A</title></item>
</rdf:RDF>`

const publicRSS2Doc = `<rss version="2.0"><channel><title>RSS2</title>
<item><title>A</title></item><item><title>B</title></item>
</channel></rss>`

const publicAtomDoc = `<feed xmlns="http://www.w3.org/2005/Atom">
<title>Atom</title>
<entry><title>A</title></entry>
<entry><title>B</title></entry>
</feed>`

const publicJSONDoc = `{"version":"https://jsonfeed.org/version/1.1","title":"JSON",
"items":[{"id":"a"},{"id":"b"}]}`

func publicGUIDs(feed *gofeed.Feed) []string {
	guids := make([]string, len(feed.Items))
	for i, item := range feed.Items {
		guids[i] = item.GUID
	}
	return guids
}

// newSequenceServer returns an in-process HTTP server serving the test
// documents by path. Where the sandbox forbids loopback binding the test is
// skipped rather than crashing inside httptest.
func newSequenceServer(t *testing.T) *httptest.Server {
	t.Helper()
	probe, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Skipf("loopback binding unavailable in this environment: %v", err)
	}
	probe.Close()

	documents := map[string]string{
		"/valid":    publicSequenceDoc,
		"/prefixed": publicSequenceDocPrefixed,
		"/empty":    publicEmptySeqDoc,
		"/broken":   publicBadRefDoc,
		"/rss2":     publicRSS2Doc,
	}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		doc, ok := documents[r.URL.Path]
		if !ok {
			http.NotFound(w, r)
			return
		}
		w.Header().Set("Content-Type", "application/rss+xml")
		_, _ = w.Write([]byte(doc))
	}))
	t.Cleanup(server.Close)
	return server
}

// orderCapturingTranslator records the titles on the native feed it receives
// and otherwise delegates to the default RSS translation.
type orderCapturingTranslator struct {
	defaultTranslator gofeed.Translator
	nativeTitles      []string
}

func (t *orderCapturingTranslator) Translate(feed interface{}) (*gofeed.Feed, error) {
	if native, ok := feed.(*rss.Feed); ok {
		t.nativeTitles = make([]string, len(native.Items))
		for i, item := range native.Items {
			t.nativeTitles[i] = item.Title
		}
	}
	return t.defaultTranslator.Translate(feed)
}

func TestRSS1SequencePublic(t *testing.T) {
	t.Run("switch off preserves the physical order", func(t *testing.T) {
		fp := gofeed.NewParser()
		feed, err := fp.ParseString(publicSequenceDoc)
		require.NoError(t, err)
		assert.Equal(t, []string{"urn:a", "urn:unused", "urn:b"}, publicGUIDs(feed))
	})

	t.Run("Parse and ParseString follow the declared order", func(t *testing.T) {
		fp := gofeed.NewParser()
		fp.UseRSS1Sequence = true

		fromString, err := fp.ParseString(publicSequenceDoc)
		require.NoError(t, err)
		assert.Equal(t, []string{"urn:b", "urn:a"}, publicGUIDs(fromString))

		fromReader, err := fp.Parse(strings.NewReader(publicSequenceDoc))
		require.NoError(t, err)
		assert.Equal(t, []string{"urn:b", "urn:a"}, publicGUIDs(fromReader))
		assert.Equal(t, "Discovery", fromReader.Title)
	})

	t.Run("different prefixes resolve through namespace URIs", func(t *testing.T) {
		fp := gofeed.NewParser()
		fp.UseRSS1Sequence = true
		feed, err := fp.ParseString(publicSequenceDocPrefixed)
		require.NoError(t, err)
		assert.Equal(t, []string{"urn:b", "urn:a"}, publicGUIDs(feed))
	})

	t.Run("empty Seq produces an empty item list", func(t *testing.T) {
		fp := gofeed.NewParser()
		fp.UseRSS1Sequence = true
		feed, err := fp.ParseString(publicEmptySeqDoc)
		require.NoError(t, err)
		require.NotNil(t, feed.Items)
		assert.Empty(t, feed.Items)
	})

	t.Run("a bad reference fails with ErrInvalidSequence and a nil feed", func(t *testing.T) {
		fp := gofeed.NewParser()
		fp.UseRSS1Sequence = true
		feed, err := fp.ParseString(publicBadRefDoc)
		require.Error(t, err)
		assert.Nil(t, feed)
		assert.True(t, errors.Is(err, rss.ErrInvalidSequence))
		assert.Contains(t, err.Error(), "urn:missing")
	})

	t.Run("the same bad document parses with the switch off", func(t *testing.T) {
		fp := gofeed.NewParser()
		feed, err := fp.ParseString(publicBadRefDoc)
		require.NoError(t, err)
		assert.Equal(t, []string{"urn:a"}, publicGUIDs(feed))
	})

	t.Run("the original feed keeps the selected order", func(t *testing.T) {
		fp := gofeed.NewParser()
		fp.UseRSS1Sequence = true
		fp.KeepOriginalFeed = true

		feed, err := fp.ParseString(publicSequenceDoc)
		require.NoError(t, err)
		native, ok := feed.OriginalFeed().(*rss.Feed)
		require.True(t, ok)
		require.Len(t, native.Items, len(feed.Items))
		for i, item := range feed.Items {
			assert.Equal(t, item.GUID, native.Items[i].GUID.Value)
		}
		assert.Equal(t, []string{"B", "A"}, []string{
			native.Items[0].Title, native.Items[1].Title,
		})
	})

	t.Run("a custom translator receives the ordered native feed", func(t *testing.T) {
		fp := gofeed.NewParser()
		fp.UseRSS1Sequence = true
		translator := &orderCapturingTranslator{defaultTranslator: &gofeed.DefaultRSSTranslator{}}
		fp.RSSTranslator = translator

		feed, err := fp.ParseString(publicSequenceDoc)
		require.NoError(t, err)
		assert.Equal(t, []string{"B", "A"}, translator.nativeTitles)
		assert.Equal(t, []string{"urn:b", "urn:a"}, publicGUIDs(feed))
	})

	t.Run("ParseURLWithContext applies the sequence over HTTP", func(t *testing.T) {
		server := newSequenceServer(t)
		fp := gofeed.NewParser()
		fp.UseRSS1Sequence = true
		fp.KeepOriginalFeed = true
		// Reach the loopback server directly, bypassing any ambient proxy.
		transport := &http.Transport{}
		fp.Client = &http.Client{Transport: transport}
		t.Cleanup(transport.CloseIdleConnections)

		feed, err := fp.ParseURLWithContext(server.URL+"/valid", context.Background())
		require.NoError(t, err)
		assert.Equal(t, []string{"urn:b", "urn:a"}, publicGUIDs(feed))
		native, ok := feed.OriginalFeed().(*rss.Feed)
		require.True(t, ok)
		assert.Len(t, native.Items, 2)

		// ParseURL shares the same selection path as ParseURLWithContext.
		plain, err := fp.ParseURL(server.URL + "/valid")
		require.NoError(t, err)
		assert.Equal(t, []string{"urn:b", "urn:a"}, publicGUIDs(plain))

		prefixed, err := fp.ParseURLWithContext(server.URL+"/prefixed", context.Background())
		require.NoError(t, err)
		assert.Equal(t, []string{"urn:b", "urn:a"}, publicGUIDs(prefixed))

		empty, err := fp.ParseURLWithContext(server.URL+"/empty", context.Background())
		require.NoError(t, err)
		assert.Empty(t, empty.Items)

		failed, err := fp.ParseURLWithContext(server.URL+"/broken", context.Background())
		require.Error(t, err)
		assert.Nil(t, failed)
		assert.True(t, errors.Is(err, rss.ErrInvalidSequence))
	})

	t.Run("the option does not touch RSS 2.0, Atom or JSON feeds", func(t *testing.T) {
		fp := gofeed.NewParser()
		fp.UseRSS1Sequence = true

		rss2, err := fp.ParseString(publicRSS2Doc)
		require.NoError(t, err)
		assert.Equal(t, []string{"", ""}, publicGUIDs(rss2))
		assert.Equal(t, "A", rss2.Items[0].Title)

		atom, err := fp.ParseString(publicAtomDoc)
		require.NoError(t, err)
		assert.Equal(t, "Atom", atom.Title)
		assert.Equal(t, "A", atom.Items[0].Title)

		jsonFeed, err := fp.ParseString(publicJSONDoc)
		require.NoError(t, err)
		assert.Equal(t, "JSON", jsonFeed.Title)
		assert.Equal(t, "a", jsonFeed.Items[0].GUID)
	})

	t.Run("one parser handles consecutive channels without shared state", func(t *testing.T) {
		fp := gofeed.NewParser()
		fp.UseRSS1Sequence = true

		first, err := fp.ParseString(publicSequenceDoc)
		require.NoError(t, err)
		assert.Equal(t, []string{"urn:b", "urn:a"}, publicGUIDs(first))

		second, err := fp.ParseString(publicBadRefDoc)
		require.Error(t, err)
		assert.Nil(t, second)
		assert.True(t, errors.Is(err, rss.ErrInvalidSequence))

		// The failed parse must not leave any index behind on the shared parser.
		third, err := fp.ParseString(publicEmptySeqDoc)
		require.NoError(t, err)
		assert.Empty(t, third.Items)

		fourth, err := fp.ParseString(publicSequenceDoc)
		require.NoError(t, err)
		assert.Equal(t, []string{"urn:b", "urn:a"}, publicGUIDs(fourth))
	})

	t.Run("concurrent parses with one configured parser stay isolated", func(t *testing.T) {
		fp := gofeed.NewParser()
		fp.UseRSS1Sequence = true

		otherDoc := strings.NewReplacer(
			"urn:a", "urn:x", "urn:b", "urn:y", "urn:unused", "urn:z",
			">A<", ">X<", ">B<", ">Y<", ">Unused<", ">Z<",
		).Replace(publicSequenceDoc)

		var wg sync.WaitGroup
		for i := 0; i < 32; i++ {
			wg.Add(2)
			go func() {
				defer wg.Done()
				feed, err := fp.ParseString(publicSequenceDoc)
				assert.NoError(t, err)
				assert.Equal(t, []string{"urn:b", "urn:a"}, publicGUIDs(feed))
			}()
			go func() {
				defer wg.Done()
				feed, err := fp.ParseString(otherDoc)
				assert.NoError(t, err)
				assert.Equal(t, []string{"urn:y", "urn:x"}, publicGUIDs(feed))
			}()
		}
		wg.Wait()
	})
}
