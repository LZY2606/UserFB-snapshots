package regress_test

import (
	"fmt"
	"io"
	"net/http"

	h "github.com/gregjones/httpcache"
)

// ExampleTransport_varyVariants shows how alternating requests with different
// Vary selection headers keep both representations in one cache entry. It uses
// an in-process RoundTripper, so no network socket is required.
func ExampleTransport_varyVariants() {
	origin := roundTripFunc(func(req *http.Request) (*http.Response, error) {
		header := http.Header{}
		header.Set("Vary", "Accept-Language")
		header.Set("Cache-Control", "max-age=3600")
		return stringResponse(http.StatusOK, header, req.Header.Get("Accept-Language")), nil
	})

	transport := &h.Transport{
		Transport:           origin,
		Cache:               h.NewMemoryCache(),
		MarkCachedResponses: true,
		VaryVariants:        true,
	}
	client := transport.Client()

	for _, lang := range []string{"en", "fr", "en", "fr"} {
		req, _ := http.NewRequest(http.MethodGet, varyTestURL, nil)
		req.Header.Set("Accept-Language", lang)
		resp, err := client.Do(req)
		if err != nil {
			panic(err)
		}
		body, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		fmt.Printf("%s cached=%v\n", body, resp.Header.Get(h.XFromCache) == "1")
	}
	// Output:
	// en cached=false
	// fr cached=false
	// en cached=true
	// fr cached=true
}

type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(req *http.Request) (*http.Response, error) {
	return f(req)
}
