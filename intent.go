package opengraph

import (
	"context"
	"net/http"
)

// Intent represents how to fetch, parse, and complete properties
// of this OpenGraph object.
// This SHOULD NOT have any meaning for "OpenGraph Protocol".
type Intent struct {

	// URL of this intent to fetch an OGP.
	// This does NOT mean `og:url` of the page.
	URL string

	// Context of the web request of this Intent.
	Context context.Context
	// HTTP Client to be used for this intent.
	HTTPClient *http.Client

	// Scrict is just an alias of `TrustedTags`.
	// `Strict == true` means `TrustedTags = ["meta"]`,
	// and `Strict == false` means `TrustedTags == ["meta", "title", "link"]`.
	Strict bool
	// TrustedTags specify which tags to be respected.
	TrustedTags []string

	// Headers to be sent with the HTTP request.
	// If nil, default browser-like headers will be used.
	Headers map[string]string

	// CollectMicroformats enables Microformats2 extraction into
	// OpenGraph.Microformats. It is disabled by default and is independent
	// of Strict and TrustedTags.
	CollectMicroformats bool
}
