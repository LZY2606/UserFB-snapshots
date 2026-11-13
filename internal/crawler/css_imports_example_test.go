package crawler_test

import (
	"fmt"

	"github.com/s0rg/crawley/internal/crawler"
)

// ExampleWithCSSImports shows how to enable the optional css dependency
// collection mode. With css scanning on, stylesheets found in html and via
// @import are expanded, while images and fonts are only reported.
func ExampleWithCSSImports() {
	c := crawler.New(
		crawler.WithScanCSS(true),
		crawler.WithCSSImports(true),
	)

	fmt.Println(c.DumpConfig())

	// Output:
	// workers: 1 depth: 0 timeout: 1s +css +css-imports
}
