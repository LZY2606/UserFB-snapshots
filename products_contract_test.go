package opengraph

import (
	"encoding/json"
	"io"
	"net/http"
	"reflect"
	"strings"
	"testing"

	"github.com/otiai10/opengraph/v2/task_support/taskcheck"
	"golang.org/x/net/html"
)

// Fixtures live entirely in inline HTML; no network listener is required.
// Every entity has exactly one definition; edges between the two scripts are
// expressed with pure references so cross-script resolution is exercised.
const relationsPage = `<!doctype html><html><head>
<meta property="og:title" content="Catalog">
<script type="application/ld+json">
{"@context":"https://schema.org","@graph":[
 {"@id":"#g1","@type":"ProductGroup","name":"Group One","variesBy":["color","size"],"color":"blue","size":"M",
  "hasVariant":{"@list":[{"@id":"#p1"},{"@id":"#p2"},{"@id":"#p4"}]}},
 {"@id":"#g2","@type":["Thing","ProductGroup"],"name":"Group Two","variesBy":["color"],"hasVariant":{"@id":"#p4"}},
 {"@id":"#p2","@type":"Product","sku":"SKU2","color":null},
 {"@id":"#p5","@type":"Product","sku":"SKU5","name":"Lonely"}
]}</script>
<script type="application/ld+json">
{"@graph":[
 {"@id":"#p1","@type":"Product","sku":"SKU1","name":"Shirt","color":"red","isVariantOf":{"@id":"#g1"}},
 {"@id":"#p3","@type":"product","sku":"SKU3","name":"","color":"green","size":"L","isVariantOf":{"@id":"#g1"}},
 {"@id":"#p4","@type":"Product","sku":"SKU4","name":"P Four","color":"black","isVariantOf":[{"@id":"#g1"},{"@id":"#g2"}]}
]}</script>
</head><body>catalog</body></html>`

func mustParseProducts(t *testing.T, page string, collect bool) *OpenGraph {
	t.Helper()
	og := New("https://shop.invalid/catalog")
	og.Intent.CollectProducts = collect
	if err := og.Parse(strings.NewReader(page)); err != nil {
		t.Fatalf("parse: %v", err)
	}
	return og
}

func productRow(rows []map[string]any, group, id string) map[string]any {
	wantID := "https://shop.invalid/catalog" + id
	wantGroup := ""
	if group != "" {
		wantGroup = "https://shop.invalid/catalog" + group
	}
	for _, row := range rows {
		if row["group"] == wantGroup && row["id"] == wantID {
			return row
		}
	}
	return nil
}

func TestProductRelations(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		og := mustParseProducts(t, relationsPage, true)

		// Rows are ordered by group id, then product id. The empty group
		// (ungrouped product) sorts first.
		c.Check(len(og.Products) == 5, "want 5 rows, got %d: %v", len(og.Products), og.Products)
		order := [][2]string{}
		for _, row := range og.Products {
			order = append(order, [2]string{row["group"].(string), row["id"].(string)})
		}
		g1 := "https://shop.invalid/catalog#g1"
		g2 := "https://shop.invalid/catalog#g2"
		p1 := "https://shop.invalid/catalog#p1"
		p3 := "https://shop.invalid/catalog#p3"
		p4 := "https://shop.invalid/catalog#p4"
		p5 := "https://shop.invalid/catalog#p5"
		c.Check(reflect.DeepEqual(order, [][2]string{
			{"", p5},
			{g1, p1},
			{g1, p3},
			{g1, p4},
			{g2, p4},
		}), "row order mismatch: %v", order)

		// p1: own color wins, size inherited from the group default.
		row := productRow(og.Products, "#g1", "#p1")
		c.Check(row != nil, "p1 grouped row missing")
		c.Check(row["sku"] == "SKU1", "p1 sku: %v", row["sku"])
		c.Check(row["name"] == "Shirt", "p1 name: %v", row["name"])
		c.Check(reflect.DeepEqual(row["dimensions"], map[string]string{"color": "red", "size": "M"}),
			"p1 dimensions: %v", row["dimensions"])
		c.Check(reflect.DeepEqual(row["offers"], []map[string]any{}), "p1 offers must be non-nil empty: %v", row["offers"])

		// p3: explicit empty name is preserved; both dimensions come from
		// the product itself; membership is declared via isVariantOf.
		row = productRow(og.Products, "#g1", "#p3")
		c.Check(row != nil && row["name"] == "", "p3 empty name: %v", row)
		c.Check(reflect.DeepEqual(row["dimensions"], map[string]string{"color": "green", "size": "L"}),
			"p3 dimensions: %v", row["dimensions"])

		// p4 belongs to two groups and is emitted once per group; in g1 it
		// inherits the group size default, g2 only varies by color.
		rowG1 := productRow(og.Products, "#g1", "#p4")
		rowG2 := productRow(og.Products, "#g2", "#p4")
		c.Check(rowG1 != nil && rowG2 != nil, "p4 should appear in both groups")
		c.Check(reflect.DeepEqual(rowG1["dimensions"], map[string]string{"color": "black", "size": "M"}),
			"p4 g1 dimensions: %v", rowG1["dimensions"])
		c.Check(reflect.DeepEqual(rowG2["dimensions"], map[string]string{"color": "black"}),
			"p4 g2 dimensions: %v", rowG2["dimensions"])

		// p2 is skipped because its explicit null blocks the color dimension.
		c.Check(productRow(og.Products, "#g1", "#p2") == nil, "p2 row must be skipped")

		// p5 has no group: one row with an empty group and empty dimensions.
		row = productRow(og.Products, "", "#p5")
		c.Check(row != nil, "ungrouped p5 row missing")
		c.Check(row["group"] == "" && row["sku"] == "SKU5" && row["name"] == "Lonely",
			"p5 row: %v", row)
		c.Check(reflect.DeepEqual(row["dimensions"], map[string]string{}), "p5 dimensions non-nil empty: %v", row["dimensions"])

		// Every row carries the fixed six keys with the required types.
		for _, r := range og.Products {
			_, okG := r["group"].(string)
			_, okI := r["id"].(string)
			_, okS := r["sku"].(string)
			_, okN := r["name"].(string)
			_, okD := r["dimensions"].(map[string]string)
			_, okO := r["offers"].([]map[string]any)
			c.Check(okG && okI && okS && okN && okD && okO, "row shape wrong: %v", r)
		}

		// Legacy meta tag results are untouched by the new feature.
		c.Check(og.Title == "Catalog", "meta title still parsed: %q", og.Title)

		// A missing reference target fails the whole extraction.
		missing := `<!doctype html><html><head><script type="application/ld+json">
{"@graph":[{"@id":"#g","@type":"ProductGroup","hasVariant":{"@id":"#ghost"}}]}</script></head></html>`
		errOG := New("https://shop.invalid/catalog")
		errOG.Intent.CollectProducts = true
		c.Check(errOG.Parse(strings.NewReader(missing)) != nil, "missing reference must error")

		// A type-mismatched reference fails the whole extraction.
		mismatch := `<!doctype html><html><head><script type="application/ld+json">
{"@graph":[{"@id":"#g","@type":"ProductGroup","hasVariant":{"@id":"#o"}},
{"@id":"#o","@type":"Offer","price":"1","priceCurrency":"USD"}]}</script></head></html>`
		errOG = New("https://shop.invalid/catalog")
		errOG.Intent.CollectProducts = true
		c.Check(errOG.Parse(strings.NewReader(mismatch)) != nil, "type mismatch must error")

		// Two definitions of the same @id fail the extraction regardless
		// of script order.
		dup := `<!doctype html><html><head>
<script type="application/ld+json">{"@id":"#p","@type":"Product","sku":"A"}</script>
<script type="application/ld+json">{"@id":"#p","@type":"Product","sku":"B"}</script>
</head></html>`
		errOG = New("https://shop.invalid/catalog")
		errOG.Intent.CollectProducts = true
		c.Check(errOG.Parse(strings.NewReader(dup)) != nil, "duplicate definition must error")

		// variesBy de-duplication, group name fallback, whitespace/case
		// insensitive @type, and empty-string dimension blocking.
		mini := `<!doctype html><html><head><script type="application/ld+json">
{"@graph":[
 {"@id":"#gm","@type":"ProductGroup","name":"Group Name","variesBy":["color","color","size"],
  "color":"teal","size":"","hasVariant":[{"@id":"#m1"},{"@id":"#m2"},{"@id":"#m3"}]},
 {"@id":"#m1","@type":"Product","sku":"M1"},
 {"@id":"#m2","@type":"  Product  ","sku":"M2","size":"L"},
 {"@id":"#m3","@type":"Product","sku":"M3","color":""}
]}</script></head></html>`
		miniOG := mustParseProducts(t, mini, true)
		c.Check(len(miniOG.Products) == 1, "blocked dimensions drop rows, one remains: %v", miniOG.Products)
		only := miniOG.Products[0]
		c.Check(only["id"] == "https://shop.invalid/catalog#m2", "m2 is the survivor: %v", only)
		c.Check(only["name"] == "Group Name", "name falls back to group: %v", only["name"])
		c.Check(reflect.DeepEqual(only["dimensions"], map[string]string{"color": "teal", "size": "L"}),
			"deduped variesBy and group default inheritance: %v", only["dimensions"])
	})
}

const offersPage = `<!doctype html><html><head>
<script type="application/ld+json">
{"@graph":[
 {"@id":"#g","@type":"ProductGroup","variesBy":["color"],"color":"blue",
  "hasVariant":[{"@id":"#p1"},{"@id":"#p2"}],"offers":[{"@id":"#o8"}]},
 {"@id":"#p1","@type":"Product","sku":"P1","offers":[{"@id":"#o1"},{"@id":"#o2"},{"@id":"#o7"},{"@id":"#o7"}]},
 {"@id":"#p2","@type":"Product","sku":"P2"}
]}</script>
<script type="application/ld+json">
{"@graph":[
 {"@id":"#o1","@type":"Offer","price":"19.90","priceCurrency":"USD","url":"/buy/o1"},
 {"@id":"#o2","@type":"Offer","price":5,"priceCurrency":"EUR","itemOffered":{"@id":"#p2"}},
 {"@id":"#o3","@type":"Offer","price":"1e3","priceCurrency":"USD","itemOffered":{"@id":"#p1"}},
 {"@id":"#o4","@type":"Offer","price":"-1","priceCurrency":"USD","itemOffered":{"@id":"#p1"}},
 {"@id":"#o5","@type":"Offer","price":"12.00","priceCurrency":"usd","itemOffered":{"@id":"#p1"}},
 {"@id":"#o6","@type":"Offer","price":"10","priceCurrency":"USD","itemOffered":[{"@id":"#p1"},{"@id":"#p2"}]},
 {"@id":"#o7","@type":"Offer","price":"20","priceCurrency":"USD","itemOffered":{"@id":"#p1"}},
 {"@id":"#o8","@type":"Offer","price":"99","priceCurrency":"USD"}
]}</script>
</head><body>offers</body></html>`

func TestProductOffers(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		og := mustParseProducts(t, offersPage, true)
		p1 := productRow(og.Products, "#g", "#p1")
		p2 := productRow(og.Products, "#g", "#p2")
		c.Check(p1 != nil && p2 != nil, "product rows present: %v", og.Products)

		offers1, ok := p1["offers"].([]map[string]any)
		c.Check(ok, "p1 offers typed slice")
		// o3 (exponent), o4 (negative), o5 (lowercase currency) are
		// unqualified and skipped; o2 points elsewhere so p1's direct
		// reference is ignored; o8 has no itemOffered and must not be
		// broadcast from the group. The repeated o7 reference stays once.
		c.Check(len(offers1) == 3, "p1 offer count: %v", offers1)
		ids := []string{}
		for _, o := range offers1 {
			ids = append(ids, o["id"].(string))
		}
		c.Check(reflect.DeepEqual(ids, []string{
			"https://shop.invalid/catalog#o1",
			"https://shop.invalid/catalog#o6",
			"https://shop.invalid/catalog#o7",
		}), "p1 offers sorted/deduped: %v", ids)

		c.Check(offers1[0]["price"] == "19.90", "decimal text preserved: %v", offers1[0]["price"])
		c.Check(offers1[0]["currency"] == "USD", "currency: %v", offers1[0]["currency"])
		c.Check(offers1[0]["url"] == "https://shop.invalid/buy/o1", "relative offer url resolved: %v", offers1[0]["url"])
		c.Check(offers1[1]["price"] == "10" && offers1[1]["url"] == "", "missing url stays empty: %v", offers1[1])
		c.Check(offers1[2]["price"] == "20", "routed offer present: %v", offers1[2])

		// p2 receives offers routed through itemOffered, including the
		// shared o6; the JSON number 5 keeps its decimal text.
		offers2, ok := p2["offers"].([]map[string]any)
		c.Check(ok && len(offers2) == 2, "p2 offers: %v", p2["offers"])
		c.Check(offers2[0]["id"] == "https://shop.invalid/catalog#o2" && offers2[0]["price"] == "5",
			"json number price text: %v", offers2[0])
		c.Check(offers2[1]["id"] == "https://shop.invalid/catalog#o6", "shared offer routed to p2: %v", offers2)

		// Inline objects inside relationship fields and nested @graph
		// entries are indexed recursively; a group offer is routed only to
		// its itemOffered member, never broadcast.
		nested := `<!doctype html><html><head><script type="application/ld+json">
{"@graph":[
 {"@id":"#gn","@type":"ProductGroup","variesBy":["color"],"color":"red",
  "hasVariant":[{"@id":"#q1",
                 "@type":"Product","sku":"Q1","isVariantOf":{"@id":"#gn"}}],
  "offers":[{"@id":"#oq","@type":"Offer","price":"3","priceCurrency":"USD",
             "itemOffered":{"@id":"#q1"}}]},
 {"wrapper":{"@graph":[{"@id":"#q2","@type":"Product","sku":"Q2"}]}}
]}</script></head></html>`
		nestedOG := mustParseProducts(t, nested, true)
		c.Check(len(nestedOG.Products) == 2, "inline and nested-graph entities indexed: %v", nestedOG.Products)
		q1 := productRow(nestedOG.Products, "#gn", "#q1")
		c.Check(q1 != nil, "inline hasVariant product belongs to the group")
		q1Offers := q1["offers"].([]map[string]any)
		c.Check(len(q1Offers) == 1 && q1Offers[0]["id"] == "https://shop.invalid/catalog#oq",
			"group offer routed via itemOffered only: %v", q1Offers)

		// Reordering scripts must not change the rendered rows: the same
		// single definitions are split across two scripts in both orders.
		scriptDefs := `{"@id":"#pd","@type":"Product","sku":"PD","offers":{"@id":"#od"}}`
		scriptOffer := `{"@id":"#od","@type":"Offer","price":"19.90","priceCurrency":"USD"}`
		pageNormal := `<!doctype html><html><head>
<script type="application/ld+json">` + scriptDefs + `</script>
<script type="application/ld+json">` + scriptOffer + `</script></head></html>`
		pageSwapped := `<!doctype html><html><head>
<script type="application/ld+json">` + scriptOffer + `</script>
<script type="application/ld+json">` + scriptDefs + `</script></head></html>`
		ogNormal := mustParseProducts(t, pageNormal, true)
		ogSwapped := mustParseProducts(t, pageSwapped, true)
		c.Check(reflect.DeepEqual(ogNormal.Products, ogSwapped.Products),
			"script reorder changes rows: %v vs %v", ogNormal.Products, ogSwapped.Products)
	})
}

const collectionPageA = `<!doctype html><html><head>
<meta property="og:title" content="Page A">
<script type="application/ld+json">{"@id":"#pa","@type":"Product","sku":"A"}</script>
</head><body>a</body></html>`

const collectionPageB = `<!doctype html><html><head>
<meta property="og:title" content="Page B">
<base href="https://other.invalid/">
<meta property="og:url" content="https://og.invalid/ignored">
<script type="application/ld+json">this is { broken json</script>
<template><script type="application/ld+json">{"@id":"#tpl","@type":"Product","sku":"TPL"}</script></template>
<script type="application/ld+json" src="/external.json">{"@id":"#src","@type":"Product","sku":"SRC"}</script>
<script type="application/ld+json">{"@id":"#pb","@type":"Product","sku":"B"}</script>
</head><body>b</body></html>`

type collectionTransport struct{ final string }

func (tr *collectionTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	if req.URL.Path == "/start" {
		return &http.Response{
			StatusCode: 302,
			Header:     http.Header{"Location": {"/final"}},
			Body:       io.NopCloser(strings.NewReader("")),
			Request:    req,
		}, nil
	}
	tr.final = req.URL.String()
	body := `<!doctype html><html><head>
<script type="application/ld+json">{"@id":"#pf","@type":"Product","sku":"F",
"offers":[{"@id":"#of"}]}</script>
<script type="application/ld+json">{"@id":"#of","@type":"Offer","price":"7.50","priceCurrency":"USD","url":"deal?x=1"}</script>
</head><body>final</body></html>`
	return &http.Response{
		StatusCode: 200,
		Header:     http.Header{"Content-Type": {"text/html; charset=utf-8"}},
		Body:       io.NopCloser(strings.NewReader(body)),
		Request:    req,
	}, nil
}

func TestProductCollection(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Disabled: Products is never touched and meta results still work.
		off := mustParseProducts(t, collectionPageA, false)
		c.Check(off.Products == nil, "disabled leaves Products nil: %v", off.Products)
		c.Check(off.Title == "Page A", "meta parsing unchanged when disabled")
		sentinel := []map[string]any{{"keep": true}}
		off.Products = sentinel
		c.Check(off.Parse(strings.NewReader(collectionPageA)) == nil, "re-parse disabled")
		c.Check(reflect.DeepEqual(off.Products, sentinel), "disabled run must not overwrite Products")

		// Enabled but no entities: a non-nil empty slice replaces nothing
		// only on success, and JSON omits the field when nil.
		empty := mustParseProducts(t, `<!doctype html><html><head></head><body>none</body></html>`, true)
		c.Check(empty.Products != nil && len(empty.Products) == 0, "no entities gives non-nil empty slice")
		raw, err := json.Marshal(New("https://shop.invalid/x"))
		c.Check(err == nil && !strings.Contains(string(raw), "products"), "nil Products omitted from JSON: %s", raw)

		// A successful run followed by a failing run keeps the old rows,
		// including the maps and slices they contain.
		og := mustParseProducts(t, collectionPageA, true)
		c.Check(len(og.Products) == 1 && og.Products[0]["sku"] == "A", "first page collected: %v", og.Products)
		previous := og.Products
		dup := `<!doctype html><html><head>
<script type="application/ld+json">{"@id":"#x","@type":"Product","sku":"1"}</script>
<script type="application/ld+json">{"@id":"#x","@type":"Product","sku":"2"}</script>
</head></html>`
		c.Check(og.Parse(strings.NewReader(dup)) != nil, "duplicate definition errors")
		c.Check(reflect.ValueOf(og.Products).Pointer() == reflect.ValueOf(previous).Pointer(),
			"old Products slice retained after error")
		c.Check(reflect.DeepEqual(og.Products, previous) && og.Products[0]["sku"] == "A",
			"old row contents retained: %v", og.Products)

		// Continuous collection: the next page starts from a fresh index;
		// broken JSON is skipped, template/src scripts are not read, and
		// neither HTML base nor og:url changes ID/URL resolution.
		c.Check(og.Parse(strings.NewReader(collectionPageB)) == nil, "second page parses")
		c.Check(len(og.Products) == 1, "only the valid inline script is indexed: %v", og.Products)
		only := og.Products[0]
		c.Check(only["id"] == "https://shop.invalid/catalog#pb" && only["sku"] == "B",
			"fresh index, document-base resolution: %v", only)
		c.Check(og.Intent.URL == "https://shop.invalid/catalog", "Intent.URL must not be rewritten: %q", og.Intent.URL)

		// Walk does not modify the input tree.
		doc, err := html.Parse(strings.NewReader(collectionPageA))
		c.Check(err == nil, "parse doc for walk")
		var scriptText string
		var find func(*html.Node)
		find = func(n *html.Node) {
			if n.Type == html.ElementNode && n.Data == "script" && n.FirstChild != nil {
				scriptText = n.FirstChild.Data
			}
			for ch := n.FirstChild; ch != nil; ch = ch.NextSibling {
				find(ch)
			}
		}
		find(doc)
		walked := New("https://shop.invalid/catalog")
		walked.Intent.CollectProducts = true
		c.Check(walked.Walk(doc) == nil, "walk succeeds")
		var afterText string
		var find2 func(*html.Node)
		find2 = func(n *html.Node) {
			if n.Type == html.ElementNode && n.Data == "script" && n.FirstChild != nil {
				afterText = n.FirstChild.Data
			}
			for ch := n.FirstChild; ch != nil; ch = ch.NextSibling {
				find2(ch)
			}
		}
		find2(doc)
		c.Check(afterText == scriptText && len(walked.Products) == 1, "tree unchanged and walk collected rows")

		// The feature is independent of Strict/TrustedTags.
		strict := New("https://shop.invalid/catalog")
		strict.Intent.Strict = true
		strict.Intent.CollectProducts = true
		c.Check(strict.Parse(strings.NewReader(collectionPageA)) == nil, "strict parse")
		c.Check(len(strict.Products) == 1, "products collected under strict mode: %v", strict.Products)
		c.Check(reflect.DeepEqual(strict.Intent.TrustedTags, []string{HTMLMetaTag}),
			"strict limits trusted tags independently of products: %v", strict.Intent.TrustedTags)
		c.Check(strict.Title == "Page A", "og:title meta still parsed in strict mode")

		// Fetch resolves relative IDs and offer URLs against the final
		// response address after redirects, keeping fragments.
		tr := &collectionTransport{}
		fetched := New("https://shop.invalid/start")
		fetched.Intent.HTTPClient = &http.Client{Transport: tr}
		fetched.Intent.CollectProducts = true
		c.Check(fetched.Fetch() == nil, "fetch with redirect")
		c.Check(tr.final == "https://shop.invalid/final", "reached final url: %q", tr.final)
		c.Check(len(fetched.Products) == 1, "fetched product row: %v", fetched.Products)
		c.Check(fetched.Products[0]["id"] == "https://shop.invalid/final#pf", "id resolved against final url: %v", fetched.Products[0]["id"])
		offers := fetched.Products[0]["offers"].([]map[string]any)
		c.Check(len(offers) == 1 && offers[0]["url"] == "https://shop.invalid/deal?x=1",
			"offer url resolved against final url: %v", offers)
		c.Check(fetched.Intent.URL == "https://shop.invalid/start", "Intent keeps the requested url")

		// Whitespace-padded, differently-cased script type is still read.
		cased := `<!doctype html><html><head>
<script type="  Application/LD+JSON  ">{"@id":"#cw","@type":"Product","sku":"CW"}</script>
<script type="application/json">{"@id":"#no","@type":"Product","sku":"NO"}</script>
</head></html>`
		casedOG := mustParseProducts(t, cased, true)
		c.Check(len(casedOG.Products) == 1 && casedOG.Products[0]["sku"] == "CW",
			"only the ld+json script is read: %v", casedOG.Products)

		// A non-http(s) document address fails without touching Products.
		badBase := New("relative/catalog")
		badBase.Intent.CollectProducts = true
		c.Check(badBase.Parse(strings.NewReader(collectionPageA)) != nil, "relative document url must error")
		c.Check(badBase.Products == nil, "no rows produced on invalid document url: %v", badBase.Products)
	})
}
