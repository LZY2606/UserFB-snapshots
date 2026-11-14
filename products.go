package opengraph

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/url"
	"regexp"
	"sort"
	"strings"

	"golang.org/x/net/html"
)

// Bare JSON-LD entity types recognized by the product collector.
const (
	ldTypeProductGroup = "ProductGroup"
	ldTypeProduct      = "Product"
	ldTypeOffer        = "Offer"
)

var (
	ldTypeNames     = []string{ldTypeProductGroup, ldTypeProduct, ldTypeOffer}
	asciiWhitespace = " \t\n\r\f\v"
	currencyPattern = regexp.MustCompile(`^[A-Z]{3}$`)
	// Non-negative decimal text without an exponent, e.g. "19.90", "0", "1200".
	pricePattern = regexp.MustCompile(`^[0-9]+(?:\.[0-9]+)?$`)
)

// ldIndex holds every entity collected from the JSON-LD scripts of one
// document. A collector is never reused between documents.
type ldIndex struct {
	base         *url.URL
	defs         map[string]map[string]any // @id -> defining object
	typed        map[string]string         // @id -> bare type name
	groups       []string                  // ProductGroup ids, discovery order
	products     []string                  // Product ids, discovery order
	offers       []string                  // Offer ids, discovery order
	memberships  map[string]map[string]bool
	routedOffers map[string]map[string]bool // product id -> candidate offer ids
}

// collectProducts extracts ProductGroup/Product/Offer entities from the
// inline application/ld+json scripts under node and builds normalized product
// rows. It constructs everything in local state and only replaces
// OpenGraph.Products after the whole extraction succeeds, so a failure keeps
// any previously collected rows untouched.
func (og *OpenGraph) collectProducts(node *html.Node) error {
	base, err := url.Parse(og.productDocURL)
	if err != nil || !base.IsAbs() || (base.Scheme != "http" && base.Scheme != "https") {
		return fmt.Errorf("products: document address must be an absolute http(s) URL: %q", og.productDocURL)
	}

	idx := &ldIndex{
		base:         base,
		defs:         map[string]map[string]any{},
		typed:        map[string]string{},
		memberships:  map[string]map[string]bool{},
		routedOffers: map[string]map[string]bool{},
	}

	for _, script := range collectLDJSONScripts(node) {
		var decoded any
		decoder := json.NewDecoder(bytes.NewReader([]byte(script)))
		decoder.UseNumber()
		if err := decoder.Decode(&decoded); err != nil {
			// A broken script is skipped; it must not fail the whole page.
			continue
		}
		var trailing any
		if err := decoder.Decode(&trailing); err != io.EOF {
			// Multiple JSON values or other trailing data make the script invalid.
			continue
		}
		if err := idx.indexNode(decoded); err != nil {
			return err
		}
	}

	if err := idx.buildRelations(); err != nil {
		return err
	}

	rows, err := idx.buildRows()
	if err != nil {
		return err
	}
	og.Products = rows
	return nil
}

// collectLDJSONScripts returns the text of inline, non-template
// application/ld+json scripts. Scripts carrying a src attribute are ignored.
func collectLDJSONScripts(node *html.Node) []string {
	var scripts []string
	var scan func(*html.Node)
	scan = func(n *html.Node) {
		if n.Type == html.ElementNode && n.Data == "script" &&
			scriptTypeIsLDJSON(n) && !scriptHasSrc(n) && !insideTemplate(n) {
			var buf strings.Builder
			for child := n.FirstChild; child != nil; child = child.NextSibling {
				if child.Type == html.TextNode {
					buf.WriteString(child.Data)
				}
			}
			scripts = append(scripts, buf.String())
		}
		for child := n.FirstChild; child != nil; child = child.NextSibling {
			scan(child)
		}
	}
	scan(node)
	return scripts
}

func scriptTypeIsLDJSON(n *html.Node) bool {
	for _, attr := range n.Attr {
		if attr.Key == "type" {
			return strings.EqualFold(strings.Trim(attr.Val, asciiWhitespace), "application/ld+json")
		}
	}
	return false
}

func scriptHasSrc(n *html.Node) bool {
	for _, attr := range n.Attr {
		if attr.Key == "src" {
			return true
		}
	}
	return false
}

func insideTemplate(n *html.Node) bool {
	for parent := n.Parent; parent != nil; parent = parent.Parent {
		if parent.Type == html.ElementNode && parent.Data == "template" {
			return true
		}
	}
	return false
}

// indexNode recursively registers objects, arrays and @graph entries.
// @context is never expanded, and reference URLs are never fetched.
func (idx *ldIndex) indexNode(v any) error {
	switch t := v.(type) {
	case map[string]any:
		// @context is documentation, not an entity container.
		for key, value := range t {
			if key == "@context" {
				continue
			}
			if err := idx.indexNode(value); err != nil {
				return err
			}
		}
		if typ, ok := idx.recognizedType(t); ok {
			if err := idx.register(t, typ); err != nil {
				return err
			}
		}
	case []any:
		for _, item := range t {
			if err := idx.indexNode(item); err != nil {
				return err
			}
		}
	}
	return nil
}

// recognizedType returns the bare, case-insensitive @type name when the
// object declares one of the three supported entities. @type may be a string
// or an array of strings.
func (idx *ldIndex) recognizedType(obj map[string]any) (string, bool) {
	raw, present := obj["@type"]
	if !present {
		return "", false
	}
	switch t := raw.(type) {
	case string:
		name := strings.Trim(t, asciiWhitespace)
		for _, candidate := range ldTypeNames {
			if strings.EqualFold(name, candidate) {
				return candidate, true
			}
		}
	case []any:
		for _, item := range t {
			name, ok := item.(string)
			if !ok {
				continue
			}
			name = strings.Trim(name, asciiWhitespace)
			for _, candidate := range ldTypeNames {
				if strings.EqualFold(name, candidate) {
					return candidate, true
				}
			}
		}
	}
	return "", false
}

// register indexes a defining entity. An object carrying only @id is a
// reference and therefore has no definition to register.
func (idx *ldIndex) register(obj map[string]any, typ string) error {
	rawID, present := obj["@id"]
	if !present {
		// The three entities require a non-empty @id.
		return nil
	}
	id, ok := rawID.(string)
	if !ok || strings.Trim(id, asciiWhitespace) == "" {
		return nil
	}
	resolved, ok := idx.resolveID(id)
	if !ok {
		return fmt.Errorf("products: invalid @id %q", id)
	}
	// {"@id": "..."} alone is a reference, not a definition.
	if len(obj) == 1 {
		return nil
	}
	if _, duplicated := idx.defs[resolved]; duplicated {
		return fmt.Errorf("products: duplicate definition for @id %q", resolved)
	}
	idx.defs[resolved] = obj
	idx.typed[resolved] = typ
	switch typ {
	case ldTypeProductGroup:
		idx.groups = append(idx.groups, resolved)
	case ldTypeProduct:
		idx.products = append(idx.products, resolved)
	case ldTypeOffer:
		idx.offers = append(idx.offers, resolved)
	}
	return nil
}

// resolveID resolves a possibly relative @id against the document address,
// keeping any fragment, and requires an absolute http(s) result.
func (idx *ldIndex) resolveID(raw string) (string, bool) {
	ref, err := url.Parse(raw)
	if err != nil {
		return "", false
	}
	resolved := idx.base.ResolveReference(ref)
	if !resolved.IsAbs() || (resolved.Scheme != "http" && resolved.Scheme != "https") {
		return "", false
	}
	return resolved.String(), true
}

// relationEntries normalizes a relationship field that may hold one object,
// one reference, an array, or an ordered @list array.
func relationEntries(v any) []map[string]any {
	if m, ok := v.(map[string]any); ok {
		if listed, isList := m["@list"]; isList {
			v = listed
		} else {
			return []map[string]any{m}
		}
	}
	items, ok := v.([]any)
	if !ok {
		return nil
	}
	var entries []map[string]any
	for _, item := range items {
		if m, ok := item.(map[string]any); ok {
			entries = append(entries, m)
		}
	}
	return entries
}

// refID returns the resolved @id of a pure reference. Inline objects carry
// their own definition and resolve the same way once indexed.
func (idx *ldIndex) refID(entry map[string]any) (string, bool) {
	raw, ok := entry["@id"].(string)
	if !ok || strings.Trim(raw, asciiWhitespace) == "" {
		return "", false
	}
	return idx.resolveID(raw)
}

// requireTarget resolves a relationship entry to an indexed entity of the
// expected type. Missing targets and type mismatches fail the extraction.
func (idx *ldIndex) requireTarget(entry map[string]any, wantType string) (string, error) {
	id, ok := idx.refID(entry)
	if !ok {
		return "", fmt.Errorf("products: relation entry without a valid @id")
	}
	got, known := idx.typed[id]
	if !known {
		return "", fmt.Errorf("products: reference target %q is not defined", id)
	}
	if got != wantType {
		return "", fmt.Errorf("products: reference target %q has type %s, want %s", id, got, wantType)
	}
	return id, nil
}

func (idx *ldIndex) addMembership(groupID, productID string) {
	members, ok := idx.memberships[groupID]
	if !ok {
		members = map[string]bool{}
		idx.memberships[groupID] = members
	}
	members[productID] = true
}

// buildRelations merges hasVariant and isVariantOf declarations (in both
// directions) and routes offers through their itemOffered targets.
func (idx *ldIndex) buildRelations() error {
	for _, groupID := range idx.groups {
		group := idx.defs[groupID]
		for _, entry := range relationEntries(group["hasVariant"]) {
			productID, err := idx.requireTarget(entry, ldTypeProduct)
			if err != nil {
				return err
			}
			idx.addMembership(groupID, productID)
		}
	}
	for _, productID := range idx.products {
		product := idx.defs[productID]
		for _, entry := range relationEntries(product["isVariantOf"]) {
			groupID, err := idx.requireTarget(entry, ldTypeProductGroup)
			if err != nil {
				return err
			}
			idx.addMembership(groupID, productID)
		}
	}

	// Every offer routed via itemOffered must point at a known Product.
	for _, offerID := range idx.offers {
		offer := idx.defs[offerID]
		for _, entry := range relationEntries(offer["itemOffered"]) {
			productID, err := idx.requireTarget(entry, ldTypeProduct)
			if err != nil {
				return err
			}
			idx.offerCandidates(productID)[offerID] = true
		}
	}

	// Direct product.offers references are candidates too, but an offer
	// carrying itemOffered must point back at this product.
	for _, productID := range idx.products {
		product := idx.defs[productID]
		candidates := idx.offerCandidates(productID)
		for _, entry := range relationEntries(product["offers"]) {
			offerID, err := idx.requireTarget(entry, ldTypeOffer)
			if err != nil {
				return err
			}
			if offered, ok := idx.defs[offerID]["itemOffered"]; ok && !idx.offerMentionsProduct(offered, productID) {
				continue
			}
			candidates[offerID] = true
		}
	}
	return nil
}

func (idx *ldIndex) offerCandidates(productID string) map[string]bool {
	candidates, ok := idx.routedOffers[productID]
	if !ok {
		candidates = map[string]bool{}
		idx.routedOffers[productID] = candidates
	}
	return candidates
}

func (idx *ldIndex) offerMentionsProduct(v any, productID string) bool {
	for _, entry := range relationEntries(v) {
		id, ok := idx.refID(entry)
		if ok && id == productID {
			return true
		}
	}
	return false
}

// dimensionNames returns the de-duplicated variesBy property names of a group,
// preserving first-seen order. Entries may be plain property-name strings,
// PropertyValue objects, references, or an ordered @list.
func (idx *ldIndex) dimensionNames(group map[string]any) []string {
	raw := group["variesBy"]
	if m, ok := raw.(map[string]any); ok {
		if listed, isList := m["@list"]; isList {
			raw = listed
		}
	}
	values, ok := raw.([]any)
	if !ok {
		// A single entry instead of an array is accepted too.
		if text, name := varyName(raw); name != "" {
			return []string{text}
		}
		return nil
	}
	var names []string
	seen := map[string]bool{}
	for _, item := range values {
		_, name := varyName(item)
		if name == "" || seen[name] {
			continue
		}
		seen[name] = true
		names = append(names, name)
	}
	return names
}

func varyName(item any) (string, string) {
	switch t := item.(type) {
	case string:
		name := strings.Trim(t, asciiWhitespace)
		return name, name
	case map[string]any:
		if name, ok := t["name"].(string); ok && name != "" {
			return name, name
		}
		if raw, ok := t["@id"].(string); ok && raw != "" {
			if i := strings.LastIndexAny(raw, "#/"); i >= 0 {
				return raw, raw[i+1:]
			}
			return raw, raw
		}
	}
	return "", ""
}

// buildRows turns the indexed entities into the fixed-shape product rows.
func (idx *ldIndex) buildRows() ([]map[string]any, error) {
	rows := []map[string]any{}

	grouped := map[string]bool{}
	groupIDs := append([]string{}, idx.groups...)
	sort.Strings(groupIDs)
	for _, groupID := range groupIDs {
		group := idx.defs[groupID]
		dimensions := idx.dimensionNames(group)
		memberIDs := make([]string, 0, len(idx.memberships[groupID]))
		for productID := range idx.memberships[groupID] {
			memberIDs = append(memberIDs, productID)
		}
		sort.Strings(memberIDs)
		for _, productID := range memberIDs {
			grouped[productID] = true
			row, ok, err := idx.buildRow(groupID, group, productID, dimensions)
			if err != nil {
				return nil, err
			}
			if ok {
				rows = append(rows, row)
			}
		}
	}

	// Products without any group still get a single row with an empty group.
	ungrouped := make([]string, 0)
	for _, productID := range idx.products {
		if !grouped[productID] {
			ungrouped = append(ungrouped, productID)
		}
	}
	sort.Strings(ungrouped)
	for _, productID := range ungrouped {
		row, ok, err := idx.buildRow("", nil, productID, nil)
		if err != nil {
			return nil, err
		}
		if ok {
			rows = append(rows, row)
		}
	}

	sort.SliceStable(rows, func(i, j int) bool {
		if rows[i]["group"] != rows[j]["group"] {
			return rows[i]["group"].(string) < rows[j]["group"].(string)
		}
		return rows[i]["id"].(string) < rows[j]["id"].(string)
	})
	return rows, nil
}

// buildRow builds one row. The boolean is false when a required dimension
// cannot be resolved; such a row is skipped.
func (idx *ldIndex) buildRow(groupID string, group map[string]any, productID string, dimensions []string) (map[string]any, bool, error) {
	product := idx.defs[productID]

	dims := map[string]string{}
	for _, name := range dimensions {
		value, blocked := stringField(product, name)
		if blocked {
			return nil, false, nil
		}
		if !value.ok {
			value, blocked = stringField(group, name)
			if blocked {
				return nil, false, nil
			}
		}
		if !value.ok {
			// The group requires a dimension nobody provides.
			return nil, false, nil
		}
		dims[name] = value.text
	}

	name := fallbackName(product, group)
	sku, _ := product["sku"].(string)

	offers, err := idx.buildOffers(productID)
	if err != nil {
		return nil, false, err
	}

	return map[string]any{
		"group":      groupID,
		"id":         productID,
		"sku":        sku,
		"name":       name,
		"dimensions": dims,
		"offers":     offers,
	}, true, nil
}

type stringFieldValue struct {
	text string
	ok   bool
}

// stringField reads a string field. A present null or empty string blocks
// inheritance explicitly; an absent or non-string key lets the caller fall
// back to the group default.
func stringField(obj map[string]any, key string) (stringFieldValue, bool) {
	if obj == nil {
		return stringFieldValue{}, false
	}
	raw, present := obj[key]
	if !present {
		return stringFieldValue{}, false
	}
	if raw == nil {
		return stringFieldValue{}, true
	}
	text, ok := raw.(string)
	if !ok {
		return stringFieldValue{}, false
	}
	if text == "" {
		return stringFieldValue{}, true
	}
	return stringFieldValue{text: text, ok: true}, false
}

func fallbackName(product, group map[string]any) string {
	if raw, present := product["name"]; present {
		// An explicit null or empty string is preserved as an empty name
		// and blocks inheritance of the group default.
		if text, ok := raw.(string); ok {
			return text
		}
		return ""
	}
	if group != nil {
		if text, ok := group["name"].(string); ok {
			return text
		}
	}
	return ""
}

// buildOffers renders the candidate offers of one product, de-duplicated by
// offer id and sorted by id. Unqualified offers are skipped.
func (idx *ldIndex) buildOffers(productID string) ([]map[string]any, error) {
	candidates := idx.routedOffers[productID]
	offerIDs := make([]string, 0, len(candidates))
	for offerID := range candidates {
		offerIDs = append(offerIDs, offerID)
	}
	sort.Strings(offerIDs)

	offers := []map[string]any{}
	for _, offerID := range offerIDs {
		offer, ok := idx.offerRow(offerID)
		if !ok {
			continue
		}
		offers = append(offers, offer)
	}
	return offers, nil
}

func (idx *ldIndex) offerRow(offerID string) (map[string]any, bool) {
	obj := idx.defs[offerID]

	price, ok := decimalPrice(obj["price"])
	if !ok {
		return nil, false
	}
	currency, ok := obj["priceCurrency"].(string)
	if !ok || !currencyPattern.MatchString(currency) {
		return nil, false
	}

	offerURL := ""
	if raw, present := obj["url"]; present {
		if text, ok := raw.(string); ok && text != "" {
			ref, err := url.Parse(text)
			if err != nil {
				return nil, false
			}
			resolved := idx.base.ResolveReference(ref)
			if !resolved.IsAbs() || (resolved.Scheme != "http" && resolved.Scheme != "https") {
				return nil, false
			}
			offerURL = resolved.String()
		}
	}

	return map[string]any{
		"id":       offerID,
		"url":      offerURL,
		"price":    price,
		"currency": currency,
	}, true
}

// decimalPrice accepts a non-negative decimal string or a JSON number and
// preserves the original decimal text. Exponents are rejected.
func decimalPrice(raw any) (string, bool) {
	switch t := raw.(type) {
	case string:
		if !pricePattern.MatchString(t) {
			return "", false
		}
		return t, true
	case json.Number:
		text := t.String()
		if strings.ContainsAny(text, "eE") || !pricePattern.MatchString(text) {
			return "", false
		}
		return text, true
	default:
		return "", false
	}
}
