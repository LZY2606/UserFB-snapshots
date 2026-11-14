package sitemap

import (
	"encoding/xml"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"time"
)

// Index is a structure of <sitemapindex>
type Index struct {
	XMLName xml.Name `xml:"sitemapindex"`
	Sitemap []parts  `xml:"sitemap"`
}

// parts is a structure of <sitemap> in <sitemapindex>
type parts struct {
	Loc     string `xml:"loc"`
	LastMod string `xml:"lastmod"`
}

// Sitemap is a structure of <sitemap>
type Sitemap struct {
	XMLName xml.Name `xml:"urlset"`
	URL     []URL    `xml:"url"`
}

// URL is a structure of <url> in <sitemap>
type URL struct {
	Loc        string  `xml:"loc"`
	LastMod    string  `xml:"lastmod"`
	ChangeFreq string  `xml:"changefreq"`
	Priority   float32 `xml:"priority"`
}

var (
	// fetch is page acquisition function
	fetch = func(URL string, options interface{}) ([]byte, error) {
		var body []byte

		res, err := http.Get(URL)
		if err != nil {
			return body, err
		}
		defer res.Body.Close()

		return io.ReadAll(res.Body)
	}

	// Time interval to be used in Index.get
	interval = time.Second
)

/*
Get is fetch and parse sitemap.xml/sitemapindex.xml

If sitemap.xml or sitemapindex.xml has some problems, This function return error.

・When sitemap.xml/sitemapindex.xml could not retrieved.
・When sitemap.xml/sitemapindex.xml is empty.
・When sitemap.xml/sitemapindex.xml has format problems.
・When sitemapindex.xml contains a sitemap.xml URL that cannot be retrieved.
・When sitemapindex.xml contains a sitemap.xml that is empty
・When sitemapindex.xml contains a sitemap.xml that has format problems.

Nested sitemapindex documents are expanded recursively. A document referenced
from several indexes is downloaded only once per call.

If you want to ignore these errors, use the ForceGet function.
*/
func Get(URL string, options interface{}) (Sitemap, error) {
	fetcher := newDocFetcher(options)
	data, err := fetcher.fetchDoc(URL, true)
	if err != nil {
		return Sitemap{}, err
	}

	isIndex, idx, smap, err := classifyRoot(URL, data)
	if err != nil {
		return Sitemap{}, err
	} else if !isIndex {
		return smap, nil
	}

	crawl := &indexCrawl{fetcher: fetcher, ignoreErr: false}
	return crawl.run(URL, idx)
}

/*
ForceGet is fetch and parse sitemap.xml/sitemapindex.xml.
The difference with the Get function is that it ignores some errors.

Errors to Ignore:

・When sitemapindex.xml contains a sitemap.xml URL that cannot be retrieved.
・When sitemapindex.xml contains a sitemap.xml that is empty
・When sitemapindex.xml contains a sitemap.xml that has format problems.

Errors not to Ignore:

・When sitemap.xml/sitemapindex.xml could not retrieved.
・When sitemap.xml/sitemapindex.xml is empty.
・When sitemap.xml/sitemapindex.xml has format problems.

Nested sitemapindex documents are expanded recursively. A document referenced
from several indexes is downloaded only once per call.

If you want **not** to ignore some errors, use the Get function.
*/
func ForceGet(URL string, options interface{}) (Sitemap, error) {
	fetcher := newDocFetcher(options)
	data, err := fetcher.fetchDoc(URL, true)
	if err != nil {
		return Sitemap{}, err
	}

	isIndex, idx, smap, err := classifyRoot(URL, data)
	if err != nil {
		return Sitemap{}, err
	} else if !isIndex {
		return smap, nil
	}

	crawl := &indexCrawl{fetcher: fetcher, ignoreErr: true}
	return crawl.run(URL, idx)
}

// docFetcher is the single production download path used by Get, ForceGet and
// Session. A document is downloaded at most once per run and options are
// passed through to the configured fetch closure.
type docFetcher struct {
	options interface{}
	// cache holds every body known this round, freshly downloaded or reused.
	cache map[string][]byte
	// fresh marks documents actually downloaded this round. A reused body in
	// cache can therefore be replaced by a later fresh download.
	fresh map[string]bool
}

func newDocFetcher(options interface{}) *docFetcher {
	return &docFetcher{options: options, cache: map[string][]byte{}, fresh: map[string]bool{}}
}

// fetchDoc downloads a document, honoring the per-run cache. Root documents are
// fetched immediately; every other fetch waits for the configured interval,
// matching the historical Index.get behavior.
func (f *docFetcher) fetchDoc(docURL string, isRoot bool) ([]byte, error) {
	if body, ok := f.cache[docURL]; ok {
		return body, nil
	}
	if !isRoot {
		time.Sleep(interval)
	}

	body, err := fetch(docURL, f.options)
	if err != nil {
		return nil, err
	}
	f.cache[docURL] = body
	f.fresh[docURL] = true
	return body, nil
}

// fetchFresh downloads a document unless it was already downloaded this round.
// A body that was only reused from a previous round is bypassed so later,
// conflicting evidence can force the download the rules require.
func (f *docFetcher) fetchFresh(docURL string) ([]byte, error) {
	if f.fresh[docURL] {
		return f.cache[docURL], nil
	}

	time.Sleep(interval)
	body, err := fetch(docURL, f.options)
	if err != nil {
		return nil, err
	}
	f.cache[docURL] = body
	f.fresh[docURL] = true
	return body, nil
}

// cacheReused records a body carried over from a previous round without
// counting it as a download.
func (f *docFetcher) cacheReused(docURL string, body []byte) {
	f.cache[docURL] = body
}

// classifyRoot identifies the type of an entry document using Parse and
// ParseIndex, preserving the historical root-document error behavior.
func classifyRoot(docURL string, data []byte) (bool, Index, Sitemap, error) {
	idx, idxErr := ParseIndex(data)
	smap, smapErr := Parse(data)

	if idxErr != nil && smapErr != nil {
		return false, Index{}, Sitemap{}, fmt.Errorf("URL is not a sitemap or sitemapindex: %s", docURL)
	} else if idxErr != nil {
		return false, idx, smap, nil
	}
	return true, idx, smap, nil
}

// classifyChild identifies the type of a document referenced from an index.
// Raw xml.Unmarshal is used so parse errors keep their historical wording
// (e.g. EOF for an empty referenced document).
func classifyChild(data []byte) (bool, Index, Sitemap, error) {
	var idx Index
	idxErr := xml.Unmarshal(data, &idx)

	var smap Sitemap
	smapErr := xml.Unmarshal(data, &smap)

	if idxErr != nil && smapErr != nil {
		return false, idx, smap, smapErr
	} else if idxErr != nil {
		return false, idx, smap, nil
	}
	return true, idx, smap, nil
}

// indexCrawl recursively expands sitemapindex documents.
type indexCrawl struct {
	fetcher   *docFetcher
	ignoreErr bool
}

func (c *indexCrawl) run(rootURL string, root Index) (Sitemap, error) {
	urls, err := c.visitIndex(rootURL, root, map[string]bool{})
	if err != nil {
		return Sitemap{}, err
	}
	return Sitemap{URL: urls}, nil
}

func (c *indexCrawl) visitIndex(docURL string, idx Index, ancestors map[string]bool) ([]URL, error) {
	var collected []URL

	for _, s := range idx.Sitemap {
		childURL, err := resolveDocURL(docURL, s.Loc)
		if err != nil {
			if c.ignoreErr {
				continue
			}
			return nil, fmt.Errorf("failed to parse %s in sitemapindex.xml: %v", s.Loc, err)
		}

		if ancestors[childURL] {
			return nil, fmt.Errorf("sitemapindex cycle detected at %s", childURL)
		}

		data, err := c.fetcher.fetchDoc(childURL, false)
		if err != nil {
			if c.ignoreErr {
				continue
			}
			return nil, fmt.Errorf("failed to retrieve %s in sitemapindex.xml: %v", childURL, err)
		}

		isIndex, childIdx, childSmap, err := classifyChild(data)
		if err != nil {
			if c.ignoreErr {
				continue
			}
			return nil, fmt.Errorf("failed to parse %s in sitemapindex.xml: %v", childURL, err)
		}

		if isIndex {
			nextAncestors := make(map[string]bool, len(ancestors)+1)
			for k := range ancestors {
				nextAncestors[k] = true
			}
			nextAncestors[docURL] = true

			more, err := c.visitIndex(childURL, childIdx, nextAncestors)
			if err != nil {
				return nil, err
			}
			collected = append(collected, more...)
		} else {
			collected = append(collected, childSmap.URL...)
		}
	}

	return collected, nil
}

// resolveDocURL resolves a loc declared in a sitemap document against the
// declaring document's absolute URL. Fragments are removed from the identity
// and query strings are never merged: standard reference resolution keeps the
// parent query only when the child loc does not declare one.
func resolveDocURL(parentURL, loc string) (string, error) {
	base, err := url.Parse(parentURL)
	if err != nil {
		return "", err
	}

	ref, err := url.Parse(loc)
	if err != nil {
		return "", err
	}

	resolved := base.ResolveReference(ref)
	resolved.Fragment = ""
	return resolved.String(), nil
}

// docIdentity strips the fragment from an absolute document URL.
func docIdentity(docURL string) (string, error) {
	u, err := url.Parse(docURL)
	if err != nil {
		return "", err
	}
	u.Fragment = ""
	return u.String(), nil
}

// ReadSitemap is a function that reads a file and returns a Sitemap structure.
func ReadSitemap(path string) (Sitemap, error) {
	if _, err := os.Stat(path); err != nil {
		return Sitemap{}, fmt.Errorf("file not found %s", path)
	}

	data, err := os.ReadFile(path)
	if err != nil {
		return Sitemap{}, fmt.Errorf("failed to read file %s", path)
	}

	return Parse(data)
}

// ReadSitemapIndex is a function that reads a file and returns a Index structure.
func ReadSitemapIndex(path string) (Index, error) {
	if _, err := os.Stat(path); err != nil {
		return Index{}, fmt.Errorf("file not found %s", path)
	}

	data, err := os.ReadFile(path)
	if err != nil {
		return Index{}, fmt.Errorf("failed to read file %s", path)
	}

	return ParseIndex(data)
}

// Parse create Sitemap data from text
func Parse(data []byte) (Sitemap, error) {
	var smap Sitemap
	if len(data) == 0 {
		return smap, fmt.Errorf("sitemap.xml is empty")
	}

	err := xml.Unmarshal(data, &smap)
	return smap, err
}

// ParseIndex create Index data from text
func ParseIndex(data []byte) (Index, error) {
	var idx Index
	if len(data) == 0 {
		return idx, fmt.Errorf("sitemapindex.xml is empty")
	}

	err := xml.Unmarshal(data, &idx)
	return idx, err
}

// SetInterval change Time interval to be used in Index.get
func SetInterval(time time.Duration) {
	interval = time
}

// SetFetch change fetch closure
func SetFetch(f func(URL string, options interface{}) ([]byte, error)) {
	fetch = f
}
