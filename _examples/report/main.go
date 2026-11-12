package main

import (
	"encoding/json"
	"errors"
	"log"
	"strings"

	"github.com/PuerkitoBio/goquery"
	"github.com/foolin/pagser"
)

// A catalog page where one row carries a price that cannot be converted.
// In report mode the bad row is dropped with a diagnostic instead of
// aborting the whole batch, while every valid row is still returned.
const rawReportHtml = `
<!doctype html>
<html>
<body>
	<title>Parts catalog</title>
	<ul class="parts">
		<li class="part">
			<span class="name">Bolt</span>
			<span class="price">120</span>
			<span class="stock" data-qty="8"></span>
			<span class="sku">SKU-BOLT</span>
		</li>
		<li class="part">
			<span class="name">Washer</span>
			<span class="price">not-a-number</span>
			<span class="stock" data-qty="3"></span>
			<!-- .sku is absent: the required field reports "missing" -->
		</li>
		<li class="part">
			<span class="name">Nut</span>
			<span class="price">300</span>
			<span class="stock" data-qty="400"></span>
			<span class="sku">SKU-NUT</span>
		</li>
	</ul>
</body>
</html>
`

type Part struct {
	Name  string `pagser:".name"`
	Price int    `pagser:".price"`
	// Stock is int8: the value 400 overflows its bit width and is reported
	// as a conversion problem instead of being silently truncated.
	Stock int8 `pagser:".stock->attr(data-qty)"`
	// Sku is required: a selector that matches nothing reports "missing"
	// and its handler is never invoked. An empty node still counts as hit.
	Sku string `pagserRequired:"true" pagser:".sku->text()"`
}

type Catalog struct {
	Title string `pagser:"title"`
	Parts []Part `pagser:".parts .part"`
}

func main() {
	p := pagser.New()

	// String entry point: ParseReport returns the surviving records in the
	// target struct plus a report of every dropped field.
	var catalog Catalog
	report, err := p.ParseReport(&catalog, rawReportHtml)
	if err != nil {
		// A non-nil error means the root record itself failed, so nothing
		// was committed into catalog.
		log.Fatalf("root record failed: %v", err)
	}

	// Consume the records that survived validation, in original order.
	for i, part := range catalog.Parts {
		log.Printf("part[%d]: %s price=%d stock=%d", i, part.Name, part.Price, part.Stock)
	}

	// Print every collected problem with its Go field path and the original
	// error, which remains reachable through errors.Unwrap / errors.Is.
	for _, issue := range report.Issues {
		log.Printf("issue path=%s category=%s tag=%q cause=%v",
			issue.FieldPath, issue.Category, issue.Tag, errors.Unwrap(issue))
	}

	log.Printf("catalog json: %s", toJson(catalog))

	// The selection entry point shares the exact same semantics when you
	// already hold a goquery document.
	doc, docErr := goquery.NewDocumentFromReader(strings.NewReader(rawReportHtml))
	if docErr != nil {
		log.Fatal(docErr)
	}
	var fromSelection Catalog
	selectionReport, selErr := p.ParseSelectionReport(&fromSelection, doc.Selection)
	if selErr != nil {
		log.Fatal(selErr)
	}
	log.Printf("selection entry collected %d issue(s) and %d part(s)",
		len(selectionReport.Issues), len(fromSelection.Parts))
}

func toJson(v interface{}) string {
	data, _ := json.MarshalIndent(v, "", "\t")
	return string(data)
}
