package links_test

import (
	"fmt"
	"strings"

	"github.com/s0rg/crawley/internal/links"
)

// ExampleExtractCSSReferences shows how a stylesheet is decomposed into
// dependency references. Imports are flagged, ordinary resources are not, and
// addresses are reported as written in the source (no base resolution).
func ExampleExtractCSSReferences() {
	const css = `@import "theme/base.css";
@font-face { src: url(fonts/ui.woff2); }
.hero { background: url(images/hero.png); }`

	links.ExtractCSSReferences(strings.NewReader(css), func(ref links.CSSReference) {
		if ref.Import {
			fmt.Println("import:", ref.URL)
		} else {
			fmt.Println("asset: ", ref.URL)
		}
	})

	// Output:
	// import: theme/base.css
	// asset:  fonts/ui.woff2
	// asset:  images/hero.png
}
