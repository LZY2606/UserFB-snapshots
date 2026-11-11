package rss_test

import (
	"strings"
	"sync"
	"testing"

	"github.com/mmcdole/gofeed/rss"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

const (
	rdfNS = "http://www.w3.org/1999/02/22-rdf-syntax-ns#"
	rssNS = "http://purl.org/rss/1.0/"
)

// sequenceDoc is the baseline layout: the channel declares B then A, while
// the document physically stores A, Unused, B.
const sequenceDoc = `<rdf:RDF xmlns:rdf="` + rdfNS + `" xmlns="` + rssNS + `">
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

// sequenceDocPrefixed binds the vocabularies to non-standard prefixes to
// prove matching uses namespace URIs, not the "rdf" spelling.
const sequenceDocPrefixed = `<rr:RDF xmlns:rr="` + rdfNS + `" xmlns:ten="` + rssNS + `">
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

// foreignDeclarationDoc puts the items/Seq/li names in a foreign namespace;
// none of them are the RSS 1.0 declaration, so physical order must stand.
const foreignDeclarationDoc = `<rdf:RDF xmlns:rdf="` + rdfNS + `" xmlns="` + rssNS + `" xmlns:ex="https://ex.invalid/ns">
<channel rdf:about="urn:feed">
<title>Discovery</title>
<link>https://source.invalid/</link>
<description>Local</description>
<ex:items><ex:Seq><ex:li ex:resource="urn:b"/></ex:Seq></ex:items>
</channel>
<item rdf:about="urn:a"><title>A</title></item>
<item rdf:about="urn:b"><title>B</title></item>
</rdf:RDF>`

// foreignMemberDoc mixes a foreign-namespaced li and resource attribute into
// a real RDF Seq; only the RDF li with an RDF resource is a member.
const foreignMemberDoc = `<rdf:RDF xmlns:rdf="` + rdfNS + `" xmlns="` + rssNS + `" xmlns:ex="https://ex.invalid/ns">
<channel rdf:about="urn:feed">
<title>Discovery</title><link>https://source.invalid/</link><description>Local</description>
<items><rdf:Seq><ex:li ex:resource="urn:b"/><rdf:li rdf:resource="urn:a"/></rdf:Seq></items>
</channel>
<item rdf:about="urn:a"><title>A</title></item>
<item rdf:about="urn:b"><title>B</title></item>
</rdf:RDF>`

// foreignAboutDoc gives an item only a foreign-namespaced about attribute.
const foreignAboutDoc = `<rdf:RDF xmlns:rdf="` + rdfNS + `" xmlns="` + rssNS + `" xmlns:ex="https://ex.invalid/ns">
<channel rdf:about="urn:feed">
<title>Discovery</title><link>https://source.invalid/</link><description>Local</description>
<items><rdf:Seq><rdf:li rdf:resource="urn:a"/></rdf:Seq></items>
</channel>
<item ex:about="urn:a"><title>A</title></item>
</rdf:RDF>`

func parseRaw(t *testing.T, doc string, useSequence bool) *rss.Feed {
	t.Helper()
	fp := &rss.Parser{UseRSS1Sequence: useSequence}
	feed, err := fp.Parse(strings.NewReader(doc))
	require.NoError(t, err)
	require.NotNil(t, feed)
	return feed
}

func parseRawError(t *testing.T, doc string) error {
	t.Helper()
	fp := &rss.Parser{UseRSS1Sequence: true}
	feed, err := fp.Parse(strings.NewReader(doc))
	require.Nil(t, feed, "an invalid sequence must return a nil feed")
	require.Error(t, err)
	return err
}

func rawTitles(feed *rss.Feed) []string {
	titles := make([]string, len(feed.Items))
	for i, item := range feed.Items {
		titles[i] = item.Title
	}
	return titles
}

func rawGUIDs(feed *rss.Feed) []string {
	guids := make([]string, len(feed.Items))
	for i, item := range feed.Items {
		if item.GUID != nil {
			guids[i] = item.GUID.Value
		}
	}
	return guids
}

func TestRSS1SequenceRaw(t *testing.T) {
	t.Run("switch off keeps physical order", func(t *testing.T) {
		feed := parseRaw(t, sequenceDoc, false)
		assert.Equal(t, []string{"A", "Unused", "B"}, rawTitles(feed))
	})

	t.Run("switch on follows declared order and drops unnamed items", func(t *testing.T) {
		feed := parseRaw(t, sequenceDoc, true)
		assert.Equal(t, []string{"B", "A"}, rawTitles(feed))
		assert.Equal(t, []string{"urn:b", "urn:a"}, rawGUIDs(feed))
		// Field translation is untouched: links still come from each item.
		assert.Equal(t, "https://source.invalid/b", feed.Items[0].Link)
		assert.Equal(t, "https://source.invalid/a", feed.Items[1].Link)
	})

	t.Run("namespace URIs rather than prefix spellings", func(t *testing.T) {
		feed := parseRaw(t, sequenceDocPrefixed, true)
		assert.Equal(t, []string{"B", "A"}, rawTitles(feed))
	})

	t.Run("empty sequence yields an empty item list", func(t *testing.T) {
		doc := strings.Replace(sequenceDoc,
			`<rdf:Seq><rdf:li rdf:resource="urn:b"/><rdf:li rdf:resource="urn:a"/></rdf:Seq>`,
			`<rdf:Seq></rdf:Seq>`, 1)
		feed := parseRaw(t, doc, true)
		require.NotNil(t, feed.Items)
		assert.Empty(t, feed.Items)
	})

	t.Run("no declaration falls back to physical order", func(t *testing.T) {
		doc := strings.Replace(sequenceDoc,
			`<items><rdf:Seq><rdf:li rdf:resource="urn:b"/><rdf:li rdf:resource="urn:a"/></rdf:Seq></items>`,
			``, 1)
		feed := parseRaw(t, doc, true)
		assert.Equal(t, []string{"A", "Unused", "B"}, rawTitles(feed))
	})

	t.Run("foreign namespace declaration is ignored", func(t *testing.T) {
		feed := parseRaw(t, foreignDeclarationDoc, true)
		assert.Equal(t, []string{"A", "B"}, rawTitles(feed))
	})

	t.Run("an RDF-namespace items wrapper is not the RSS declaration", func(t *testing.T) {
		doc := strings.Replace(sequenceDoc,
			`<items><rdf:Seq>`, `<rdf:items><rdf:Seq>`, 1)
		doc = strings.Replace(doc, `</rdf:Seq></items>`,
			`</rdf:Seq></rdf:items>`, 1)
		feed := parseRaw(t, doc, true)
		assert.Equal(t, []string{"A", "Unused", "B"}, rawTitles(feed))
	})

	t.Run("foreign namespace members and attributes are ignored", func(t *testing.T) {
		feed := parseRaw(t, foreignMemberDoc, true)
		assert.Equal(t, []string{"A"}, rawTitles(feed))
	})

	t.Run("repeated references keep repeated positions", func(t *testing.T) {
		doc := strings.Replace(sequenceDoc,
			`<rdf:li rdf:resource="urn:b"/><rdf:li rdf:resource="urn:a"/>`,
			`<rdf:li rdf:resource="urn:a"/><rdf:li rdf:resource="urn:a"/>`, 1)
		feed := parseRaw(t, doc, true)
		require.Len(t, feed.Items, 2)
		assert.Same(t, feed.Items[0], feed.Items[1])
		assert.Equal(t, []string{"A", "A"}, rawTitles(feed))
	})

	t.Run("identity is rdf about even when guid or link differ", func(t *testing.T) {
		doc := strings.Replace(sequenceDoc,
			`<item rdf:about="urn:a"><title>A</title><link>https://source.invalid/a</link></item>`,
			`<item rdf:about="urn:a"><title>A</title><link>https://elsewhere.invalid/a</link><guid>different-guid</guid></item>`, 1)
		feed := parseRaw(t, doc, true)
		assert.Equal(t, []string{"B", "A"}, rawTitles(feed))
		assert.Equal(t, "different-guid", feed.Items[1].GUID.Value)
		assert.Equal(t, "https://elsewhere.invalid/a", feed.Items[1].Link)
	})

	t.Run("resource values match exactly without normalization", func(t *testing.T) {
		doc := strings.Replace(sequenceDoc, `rdf:resource="urn:a"`, `rdf:resource="urn:a/"`, 1)
		err := parseRawError(t, doc)
		assert.ErrorIs(t, err, rss.ErrInvalidSequence)
		assert.Contains(t, err.Error(), "urn:a/")
	})

	t.Run("unreferenced duplicate about does not invalidate the sequence", func(t *testing.T) {
		doc := strings.Replace(sequenceDoc,
			`<item rdf:about="urn:unused"><title>Unused</title></item>`,
			`<item rdf:about="urn:unused"><title>Unused One</title></item>`+
				`<item rdf:about="urn:unused"><title>Unused Two</title></item>`, 1)
		feed := parseRaw(t, doc, true)
		assert.Equal(t, []string{"B", "A"}, rawTitles(feed))
	})

	t.Run("unreferenced item without identity does not invalidate the sequence", func(t *testing.T) {
		doc := strings.Replace(sequenceDoc,
			`<item rdf:about="urn:unused"><title>Unused</title></item>`,
			`<item><title>Anonymous</title></item>`, 1)
		feed := parseRaw(t, doc, true)
		assert.Equal(t, []string{"B", "A"}, rawTitles(feed))
	})

	t.Run("option has no effect on RSS 2.0", func(t *testing.T) {
		doc := `<rss version="2.0"><channel><title>RSS2</title>` +
			`<item><title>A</title></item><item><title>B</title></item>` +
			`</channel></rss>`
		feed := parseRaw(t, doc, true)
		assert.Equal(t, "2.0", feed.Version)
		assert.Equal(t, []string{"A", "B"}, rawTitles(feed))
	})

	t.Run("option has no effect on RSS 0.9", func(t *testing.T) {
		doc := `<rdf:RDF xmlns:rdf="` + rdfNS + `" xmlns="http://channel.netscape.com/rdf/simple/0.9/">
<channel rdf:about="urn:feed"><title>Old</title>
<items><rdf:Seq><rdf:li rdf:resource="urn:b"/></rdf:Seq></items></channel>
<item rdf:about="urn:a"><title>A</title></item>
<item rdf:about="urn:b"><title>B</title></item>
</rdf:RDF>`
		feed := parseRaw(t, doc, true)
		assert.Equal(t, "0.9", feed.Version)
		assert.Equal(t, []string{"A", "B"}, rawTitles(feed))
	})

	errorCases := []struct {
		name string
		doc  string
		want string
	}{
		{
			name: "multiple Seq declarations",
			doc: strings.Replace(sequenceDoc, `</rdf:Seq>`,
				`</rdf:Seq><rdf:Seq><rdf:li rdf:resource="urn:a"/></rdf:Seq>`, 1),
			want: "multiple item sequences",
		},
		{
			name: "multiple items wrappers each declaring a Seq",
			doc: strings.Replace(sequenceDoc,
				`</items>`,
				`</items><items><rdf:Seq><rdf:li rdf:resource="urn:a"/></rdf:Seq></items>`, 1),
			want: "multiple item sequences",
		},
		{
			name: "li missing resource",
			doc: strings.Replace(sequenceDoc,
				`<rdf:li rdf:resource="urn:b"/>`, `<rdf:li/>`, 1),
			want: "resource",
		},
		{
			name: "li with empty resource",
			doc: strings.Replace(sequenceDoc,
				`<rdf:li rdf:resource="urn:b"/>`, `<rdf:li rdf:resource=""/>`, 1),
			want: "resource",
		},
		{
			name: "RDF li carrying only a foreign-namespace resource",
			doc: strings.Replace(sequenceDoc,
				`<rdf:li rdf:resource="urn:b"/>`,
				`<rdf:li xmlns:ex="https://ex.invalid/ns" ex:resource="urn:b"/>`, 1),
			want: "resource",
		},
		{
			name: "reference to an unknown item",
			doc: strings.Replace(sequenceDoc, `rdf:resource="urn:b"`,
				`rdf:resource="urn:missing"`, 1),
			want: "urn:missing",
		},
		{
			name: "foreign about is not an RDF identity",
			doc:  foreignAboutDoc,
			want: "urn:a",
		},
		{
			name: "referenced about maps to two items",
			doc: strings.Replace(sequenceDoc,
				`<item rdf:about="urn:unused"><title>Unused</title></item>`,
				`<item rdf:about="urn:b"><title>B Two</title></item>`, 1),
			want: "urn:b",
		},
	}
	for _, tc := range errorCases {
		t.Run("error: "+tc.name, func(t *testing.T) {
			err := parseRawError(t, tc.doc)
			assert.ErrorIs(t, err, rss.ErrInvalidSequence)
			assert.Contains(t, err.Error(), tc.want)
		})
	}

	t.Run("sequence flaws do not matter when the switch is off", func(t *testing.T) {
		doc := strings.Replace(sequenceDoc, `rdf:resource="urn:b"`,
			`rdf:resource="urn:missing"`, 1)
		feed := parseRaw(t, doc, false)
		assert.Equal(t, []string{"A", "Unused", "B"}, rawTitles(feed))
	})

	t.Run("one parser reused across different channels keeps no per-page state", func(t *testing.T) {
		fp := &rss.Parser{UseRSS1Sequence: true}

		first, err := fp.Parse(strings.NewReader(sequenceDoc))
		require.NoError(t, err)
		assert.Equal(t, []string{"B", "A"}, rawTitles(first))

		noDeclaration := strings.Replace(sequenceDoc,
			`<items><rdf:Seq><rdf:li rdf:resource="urn:b"/><rdf:li rdf:resource="urn:a"/></rdf:Seq></items>`,
			``, 1)
		second, err := fp.Parse(strings.NewReader(noDeclaration))
		require.NoError(t, err)
		assert.Equal(t, []string{"A", "Unused", "B"}, rawTitles(second))

		again, err := fp.Parse(strings.NewReader(sequenceDocPrefixed))
		require.NoError(t, err)
		assert.Equal(t, []string{"B", "A"}, rawTitles(again))
	})

	t.Run("concurrent parses share one parser without borrowing items", func(t *testing.T) {
		fp := &rss.Parser{UseRSS1Sequence: true}
		otherDoc := strings.NewReplacer(
			"urn:a", "urn:x", "urn:b", "urn:y", "urn:unused", "urn:z",
			">A<", ">X<", ">B<", ">Y<", ">Unused<", ">Z<",
		).Replace(sequenceDoc)

		var wg sync.WaitGroup
		for i := 0; i < 32; i++ {
			wg.Add(2)
			go func() {
				defer wg.Done()
				feed, err := fp.Parse(strings.NewReader(sequenceDoc))
				assert.NoError(t, err)
				assert.Equal(t, []string{"B", "A"}, rawTitles(feed))
			}()
			go func() {
				defer wg.Done()
				feed, err := fp.Parse(strings.NewReader(otherDoc))
				assert.NoError(t, err)
				assert.Equal(t, []string{"Y", "X"}, rawTitles(feed))
			}()
		}
		wg.Wait()
	})
}
