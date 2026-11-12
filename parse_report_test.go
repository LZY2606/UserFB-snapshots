package pagser

import (
	"errors"
	"fmt"
	"strings"
	"sync"
	"testing"

	"github.com/PuerkitoBio/goquery"
)

// --- fixtures -------------------------------------------------------------

// reportHTML is a two-level page: groups containing items. It deliberately
// contains:
//   - group 0: a bad child item (Price "bad" cannot convert to int),
//   - group 1: an int8 overflow item plus a failing plain nested struct,
//   - group 2: a required field whose selector is missing,
//   - group 3: an item whose handler returns a sentinel error.
const reportHTML = `
<html><body>
<title>Catalog Report</title>
<div class="group" data-id="g0">
	<h2>Group A</h2>
	<span class="code">CA</span>
	<span class="must">ok</span>
	<span class="note">n0</span>
	<ul>
		<li class="item"><span class="name">A-1</span><span class="price" data-value="10">10</span><span class="tiny">1</span><span class="code"></span></li>
		<li class="item"><span class="name">A-2</span><span class="price" data-value="20">20</span><span class="tiny">2</span><span class="code"></span></li>
		<li class="item"><span class="name">A-3</span><span class="price" data-value="bad">bad</span><span class="tiny">3</span><span class="code"></span></li>
		<li class="item"><span class="name">A-4</span><span class="price" data-value="40">40</span><span class="tiny">4</span><span class="code"></span></li>
	</ul>
</div>
<div class="group" data-id="g1">
	<h2>Group B</h2>
	<span class="code">CB</span>
	<span class="must">ok</span>
	<span class="note">n1</span>
	<ul>
		<li class="item"><span class="name">B-1</span><span class="price" data-value="50">50</span><span class="tiny">300</span><span class="code"></span></li>
		<li class="item"><span class="name">B-2</span><span class="price" data-value="60">60</span><span class="tiny">6</span><span class="code"></span></li>
	</ul>
	<div class="meta"><span class="level">oops</span></div>
</div>
<div class="group" data-id="g2">
	<h2>Group C</h2>
	<span class="code">CC</span>
	<ul>
		<li class="item"><span class="name">C-1</span><span class="price" data-value="70">70</span><span class="tiny">7</span><span class="code"></span></li>
	</ul>
</div>
<div class="group" data-id="g3">
	<h2>Group D</h2>
	<span class="code">CD</span>
	<span class="must">ok</span>
	<span class="note">n3</span>
	<ul>
		<li class="item"><span class="name">D-1</span><span class="price" data-value="80">80</span><span class="tiny">8</span><span class="code"></span></li>
		<li class="item"><span class="name">EVIL</span><span class="price" data-value="90">90</span><span class="tiny">9</span><span class="code"></span></li>
	</ul>
</div>
</body></html>`

// handlerCounters proves every handler runs exactly once per matched node:
// report mode never retries a handler after a failure.
type handlerCounters struct {
	mu sync.Mutex
	m  map[string]int
}

func (c *handlerCounters) inc(name string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.m[name]++
}

func (c *handlerCounters) get(name string) int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.m[name]
}

func (c *handlerCounters) reset() {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.m = map[string]int{}
}

var reportCounters = &handlerCounters{m: map[string]int{}}

var (
	errEvilName = errors.New("evil name rejected")
	errRootBoom = errors.New("root field exploded")
)

// --- target structs -------------------------------------------------------

type reportItem struct {
	Name         string `pagser:".name->ItemName()"`
	Price        int    `pagser:".price->attr(data-value)"`
	Tiny         int8   `pagser:".tiny"`
	Code         string `pagser:".code"`
	CodeHit      string `pagserRequired:"true" pagser:".code->CodeHitHandler()"`
	ParentScope  string `pagser:".name->GroupOnly()"`
	RootScope    string `pagser:".name->RootOnly()"`
	SameNameCall string `pagser:".name->SameName()"`
}

// reportMeta is a plain (non-slice) nested struct: it is not a record
// boundary, so its failures are attributed to the enclosing group record.
type reportMeta struct {
	Level int `pagser:".level"`
}

type reportGroup struct {
	ID       string        `pagser:"->attr(data-id)"`
	Title    string        `pagser:"h2->SameName()"`
	Code     string        `pagser:".code"`
	Items    []*reportItem `pagser:"ul .item"`
	Meta     reportMeta    `pagser:".meta"`
	Absent   string        `pagser:".nope"`
	Must     string        `pagserRequired:"true" pagser:".must"`
	Note     string        `pagser:".note"`
	GroupTag string        `pagser:"h2->GroupOnly()"`
}

type reportPage struct {
	Title    string            `pagser:"title"`
	Groups   []reportGroup     `pagser:".group"`
	Prefix   string            `pagser:"title->Prefixed()"`
	SameCall string            `pagser:"title->SameName()"`
	Settings map[string]string // untagged, prefilled reference configuration
}

// --- handlers: same name at every level -----------------------------------

func (it reportItem) SameName(node *goquery.Selection, args ...string) (interface{}, error) {
	reportCounters.inc("item.SameName")
	return "item:" + strings.TrimSpace(node.Text()), nil
}

func (g reportGroup) SameName(node *goquery.Selection, args ...string) (interface{}, error) {
	reportCounters.inc("group.SameName")
	return "group:" + strings.TrimSpace(node.Text()), nil
}

func (pg *reportPage) SameName(node *goquery.Selection, args ...string) (interface{}, error) {
	reportCounters.inc("root.SameName")
	return "root:" + strings.TrimSpace(node.Text()), nil
}

// ItemName returns a sentinel, identity-preserving error for EVIL rows.
func (it reportItem) ItemName(node *goquery.Selection, args ...string) (interface{}, error) {
	reportCounters.inc("item.ItemName")
	text := strings.TrimSpace(node.Text())
	if text == "EVIL" {
		return nil, fmt.Errorf("%w: %s", errEvilName, text)
	}
	return text, nil
}

// CodeHitHandler runs even on empty-text nodes: an empty node is still a hit.
func (it reportItem) CodeHitHandler(node *goquery.Selection, args ...string) (interface{}, error) {
	reportCounters.inc("item.CodeHitHandler")
	return strings.TrimSpace(node.Text()), nil
}

// GroupOnly is only defined on the group; items reach it through the
// ancestor chain and observe the partially assembled parent candidate.
func (g reportGroup) GroupOnly(node *goquery.Selection, args ...string) (interface{}, error) {
	reportCounters.inc("group.GroupOnly")
	return "group:" + g.ID + ":" + strings.TrimSpace(node.Text()), nil
}

// RootOnly is only defined on the root; it reads prefilled configuration.
func (pg *reportPage) RootOnly(node *goquery.Selection, args ...string) (interface{}, error) {
	reportCounters.inc("root.RootOnly")
	return "root:" + pg.Settings["env"] + ":" + strings.TrimSpace(node.Text()), nil
}

func (pg *reportPage) Prefixed(node *goquery.Selection, args ...string) (interface{}, error) {
	reportCounters.inc("root.Prefixed")
	return pg.Settings["prefix"] + strings.TrimSpace(node.Text()), nil
}

// --- the main test --------------------------------------------------------

func TestParseReport(t *testing.T) {
	reportCounters.reset()
	p := New()

	type counterSnapshot struct {
		itemName, itemSameName, codeHit, groupSameName, rootSameName, prefixed int
	}
	run := func(t *testing.T, entry string, parse func(v interface{}) (*ParseReport, error)) (*ParseReport, reportPage, counterSnapshot) {
		reportCounters.reset()
		page := reportPage{
			Settings: map[string]string{"env": "test", "prefix": "P-"},
		}
		report, err := parse(&page)
		if err != nil {
			t.Fatalf("%s entry: unexpected error: %v", entry, err)
		}
		if report == nil {
			t.Fatalf("%s entry: nil report", entry)
		}
		snap := counterSnapshot{
			itemName:      reportCounters.get("item.ItemName"),
			itemSameName:  reportCounters.get("item.SameName"),
			codeHit:       reportCounters.get("item.CodeHitHandler"),
			groupSameName: reportCounters.get("group.SameName"),
			rootSameName:  reportCounters.get("root.SameName"),
			prefixed:      reportCounters.get("root.Prefixed"),
		}
		return report, page, snap
	}

	// String entry.
	reportStr, pageStr, snapStr := run(t, "string", func(v interface{}) (*ParseReport, error) {
		return p.ParseReport(v, reportHTML)
	})

	// Selection entry over the exact same page.
	doc, err := goquery.NewDocumentFromReader(strings.NewReader(reportHTML))
	if err != nil {
		t.Fatal(err)
	}
	reportSel, pageSel, snapSel := run(t, "selection", func(v interface{}) (*ParseReport, error) {
		return p.ParseSelectionReport(v, doc.Selection)
	})

	// --- expected issues, original DOM indexes retained -------------------
	wantIssues := []struct {
		path     string
		category ReportIssueCategory
		match    func(issue ReportIssue) bool
	}{
		{"Groups[0].Items[2].Price", ReportIssueConversion, nil},
		{"Groups[1].Items[0].Tiny", ReportIssueConversion, nil},
		{"Groups[1].Meta.Level", ReportIssueConversion, nil},
		{"Groups[2].Must", ReportIssueMissing, nil},
		{"Groups[3].Items[1].Name", ReportIssueFunc, func(issue ReportIssue) bool {
			return errors.Is(issue, errEvilName)
		}},
	}

	checkReport := func(t *testing.T, entry string, report *ParseReport) {
		if len(report.Issues) != len(wantIssues) {
			t.Fatalf("%s entry: got %d issues %#v, want %d", entry, len(report.Issues), report.Issues, len(wantIssues))
		}
		for i, want := range wantIssues {
			got := report.Issues[i]
			if got.FieldPath != want.path {
				t.Fatalf("%s entry: issue %d path = %q, want %q", entry, i, got.FieldPath, want.path)
			}
			if got.Category != want.category {
				t.Fatalf("%s entry: issue %d category = %q, want %q", entry, i, got.Category, want.category)
			}
			if got.Tag == "" {
				t.Fatalf("%s entry: issue %d has empty raw tag", entry, i)
			}
			if errors.Unwrap(got) == nil {
				t.Fatalf("%s entry: issue %d is not unwrappable: %v", entry, i, got)
			}
			if want.match != nil && !want.match(got) {
				t.Fatalf("%s entry: issue %d identity check failed: %v", entry, i, got)
			}
		}
	}
	checkReport(t, "string", reportStr)
	checkReport(t, "selection", reportSel)

	// --- surviving output --------------------------------------------------
	checkOutput := func(t *testing.T, entry string, page reportPage) {
		// Groups g1 (nested struct failure) and g2 (required missing) are
		// dropped; g0 and g3 survive in original order.
		if len(page.Groups) != 2 {
			t.Fatalf("%s entry: got %d groups, want 2: %#v", entry, len(page.Groups), page.Groups)
		}
		if page.Groups[0].ID != "g0" || page.Groups[1].ID != "g3" {
			t.Fatalf("%s entry: surviving group order = %q, %q", entry, page.Groups[0].ID, page.Groups[1].ID)
		}

		g0 := page.Groups[0]
		// The bad child at DOM index 2 is gone, the rest keep their order.
		if len(g0.Items) != 3 {
			t.Fatalf("%s entry: g0 got %d items, want 3", entry, len(g0.Items))
		}
		wantNames := []string{"A-1", "A-2", "A-4"}
		for i, name := range wantNames {
			if g0.Items[i].Name != name {
				t.Fatalf("%s entry: g0 item %d = %q, want %q", entry, i, g0.Items[i].Name, name)
			}
		}
		if g0.Items[2].Price != 40 {
			t.Fatalf("%s entry: g0 last price = %d, want 40", entry, g0.Items[2].Price)
		}
		// Empty node is still a hit, required or not.
		if g0.Items[0].Code != "" || g0.Items[0].CodeHit != "" {
			t.Fatalf("%s entry: empty node handling wrong: %#v", entry, g0.Items[0])
		}
		// Optional missing node keeps the zero value and produces no issue.
		if g0.Absent != "" {
			t.Fatalf("%s entry: optional missing field = %q, want empty", entry, g0.Absent)
		}
		// Same-named method resolves to the nearest enclosing object.
		if g0.Title != "group:Group A" {
			t.Fatalf("%s entry: group method = %q", entry, g0.Title)
		}
		if g0.Items[0].SameNameCall != "item:A-1" {
			t.Fatalf("%s entry: item method = %q", entry, g0.Items[0].SameNameCall)
		}
		// Ancestor method lookup: item -> group -> root.
		if g0.Items[0].ParentScope != "group:g0:A-1" {
			t.Fatalf("%s entry: parent method = %q", entry, g0.Items[0].ParentScope)
		}
		if g0.Items[0].RootScope != "root:test:A-1" {
			t.Fatalf("%s entry: root method = %q", entry, g0.Items[0].RootScope)
		}
		if g0.GroupTag != "group:g0:Group A" {
			t.Fatalf("%s entry: group self method = %q", entry, g0.GroupTag)
		}

		g3 := page.Groups[1]
		if len(g3.Items) != 1 || g3.Items[0].Name != "D-1" {
			t.Fatalf("%s entry: g3 items = %#v, want only D-1", entry, g3.Items)
		}

		// Root-level methods, including one reading prefilled configuration.
		if page.Title != "Catalog Report" {
			t.Fatalf("%s entry: title = %q", entry, page.Title)
		}
		if page.SameCall != "root:Catalog Report" {
			t.Fatalf("%s entry: root same-name method = %q", entry, page.SameCall)
		}
		if page.Prefix != "P-Catalog Report" {
			t.Fatalf("%s entry: Prefixed = %q", entry, page.Prefix)
		}
		if page.Settings["env"] != "test" {
			t.Fatalf("%s entry: prefilled reference config lost", entry)
		}
	}
	checkOutput(t, "string", pageStr)
	checkOutput(t, "selection", pageSel)

	// --- handler counters: every matched node, exactly once ---------------
	// 9 item nodes in the page, each invoking its item handlers once; the
	// 4 groups and 2 root fields likewise. No handler is retried after a
	// failure, and each entry point behaves identically.
	checkCounters := func(entry string, snap counterSnapshot) {
		if snap.itemName != 9 {
			t.Fatalf("%s entry: ItemName called %d times, want 9", entry, snap.itemName)
		}
		if snap.itemSameName != 9 {
			t.Fatalf("%s entry: item SameName called %d times, want 9", entry, snap.itemSameName)
		}
		if snap.codeHit != 9 {
			t.Fatalf("%s entry: CodeHitHandler called %d times, want 9 (empty nodes still hit)", entry, snap.codeHit)
		}
		if snap.groupSameName != 4 {
			t.Fatalf("%s entry: group SameName called %d times, want 4", entry, snap.groupSameName)
		}
		if snap.rootSameName != 1 {
			t.Fatalf("%s entry: root SameName called %d times, want 1", entry, snap.rootSameName)
		}
		if snap.prefixed != 1 {
			t.Fatalf("%s entry: Prefixed called %d times, want 1", entry, snap.prefixed)
		}
	}
	checkCounters("string", snapStr)
	checkCounters("selection", snapSel)

	// --- independence between calls, reports and pointer elements ----------
	pageStr.Groups[0].ID = "mutated"
	pageStr.Groups[0].Items[0].Name = "mutated"
	reportStr.Issues[0].FieldPath = "mutated"

	page2 := reportPage{Settings: map[string]string{"env": "test", "prefix": "P-"}}
	report2, err := p.ParseReport(&page2, reportHTML)
	if err != nil {
		t.Fatal(err)
	}
	if page2.Groups[0].ID != "g0" || page2.Groups[0].Items[0].Name != "A-1" {
		t.Fatalf("second parse aliased first parse output: %#v", page2.Groups[0])
	}
	if report2.Issues[0].FieldPath != "Groups[0].Items[2].Price" {
		t.Fatalf("second parse report aliased mutated report: %q", report2.Issues[0].FieldPath)
	}

	// --- root field failure on the same page: error and no commit ----------
	var badRoot reportBadRoot
	badRoot.Marker = "keep"
	rootReport, rootErr := p.ParseReport(&badRoot, reportHTML)
	if rootErr == nil || !errors.Is(rootErr, errRootBoom) {
		t.Fatalf("root failure error = %v, want unwrappable sentinel", rootErr)
	}
	if badRoot.Title != "" || badRoot.Marker != "keep" {
		t.Fatalf("root failure must not commit partial data: %#v", badRoot)
	}
	if rootReport == nil || len(rootReport.Issues) == 0 {
		t.Fatalf("root failure report = %#v", rootReport)
	}
}

// --- root failure: nothing is committed -----------------------------------

type reportBadRoot struct {
	Title  string `pagser:"title->BoomRoot()"`
	Need   string `pagserRequired:"true" pagser:".ghost"`
	Marker string // untagged, prefilled
}

func (r *reportBadRoot) BoomRoot(node *goquery.Selection, args ...string) (interface{}, error) {
	reportCounters.inc("root.boom")
	return nil, fmt.Errorf("%w: title", errRootBoom)
}

func (r *reportBadRoot) GhostProbe(node *goquery.Selection, args ...string) (interface{}, error) {
	reportCounters.inc("root.GhostProbe")
	return "should never happen", nil
}

func TestParseReportRootFailure(t *testing.T) {
	reportCounters.reset()
	p := New()
	target := reportBadRoot{Marker: "keep-me"}
	report, err := p.ParseReport(&target, reportHTML)
	if err == nil {
		t.Fatal("expected error for root record failure")
	}
	if !errors.Is(err, errRootBoom) {
		t.Fatalf("root error does not unwrap to sentinel: %v", err)
	}
	if report == nil || len(report.Issues) != 2 {
		t.Fatalf("report = %#v, want 2 issues", report)
	}
	categories := map[ReportIssueCategory]bool{}
	for _, issue := range report.Issues {
		categories[issue.Category] = true
	}
	if !categories[ReportIssueFunc] || !categories[ReportIssueMissing] {
		t.Fatalf("missing expected categories: %#v", categories)
	}
	// The caller's target is untouched: no partial commit.
	if target.Title != "" || target.Marker != "keep-me" {
		t.Fatalf("target committed despite root failure: %#v", target)
	}
	if got := reportCounters.get("root.boom"); got != 1 {
		t.Fatalf("BoomRoot called %d times, want 1 (no rerun)", got)
	}
}

// --- required missing never invokes the handler ----------------------------

type reportRequiredMissingTarget struct {
	Ghost string `pagserRequired:"true" pagser:".ghost->GhostProbe()"`
}

func (r reportRequiredMissingTarget) GhostProbe(node *goquery.Selection, args ...string) (interface{}, error) {
	reportCounters.inc("struct.GhostProbe")
	return "nope", nil
}

func TestParseReportRequiredMissingSkipsHandler(t *testing.T) {
	reportCounters.reset()
	p := New()
	var target reportRequiredMissingTarget
	report, err := p.ParseReport(&target, reportHTML)
	if err == nil {
		t.Fatal("expected root error from missing required field")
	}
	if report == nil || len(report.Issues) != 1 || report.Issues[0].Category != ReportIssueMissing {
		t.Fatalf("report = %#v", report)
	}
	if got := reportCounters.get("struct.GhostProbe"); got != 0 {
		t.Fatalf("missing required field invoked its handler %d times", got)
	}
}

// --- primitive slice conversion failure ------------------------------------

func TestParseReportPrimitiveSlice(t *testing.T) {
	p := New()
	html := `<ul><li>1</li><li>2</li><li>nope</li><li>4</li><li>oops</li></ul>`
	var target struct {
		Nums []int `pagser:"ul li"`
	}
	report, err := p.ParseReport(&target, reportHTMLWrapper(html))
	// A primitive slice is part of the root record, not a record boundary:
	// its failure aborts the root record and commits nothing.
	if err == nil {
		t.Fatal("expected root record error for bad primitive slice element")
	}
	if len(report.Issues) != 2 {
		t.Fatalf("issues = %#v, want diagnostics for both bad elements", report.Issues)
	}
	wantPaths := []string{"Nums[2]", "Nums[4]"}
	for i, issue := range report.Issues {
		if issue.FieldPath != wantPaths[i] || issue.Category != ReportIssueConversion {
			t.Fatalf("issue %d = %#v", i, issue)
		}
		if issue.Tag != "ul li" {
			t.Fatalf("issue %d raw tag = %q", i, issue.Tag)
		}
	}
	if len(target.Nums) != 0 {
		t.Fatalf("root record with bad slice must not commit partial slice: %#v", target.Nums)
	}
}

func reportHTMLWrapper(inner string) string {
	return "<html><body>" + inner + "</body></html>"
}

// --- handler side effects on untagged candidate fields ---------------------

type reportSideEffectRow struct {
	Name   string `pagser:".name->Stamp()"`
	Price  int    `pagser:".price->attr(data-v)"`
	Digest string // untagged: written only by a handler, must survive commit
}

func (r *reportSideEffectRow) Stamp(node *goquery.Selection, args ...string) (interface{}, error) {
	reportCounters.inc("row.Stamp")
	name := strings.TrimSpace(node.Text())
	r.Digest = "stamped:" + name
	return name, nil
}

type reportSideEffectPage struct {
	Rows []reportSideEffectRow `pagser:".row"`
}

func TestParseReportHandlerSideEffects(t *testing.T) {
	reportCounters.reset()
	p := New()
	html := `
		<div class="row"><span class="name">ok-1</span><span class="price" data-v="1">x</span></div>
		<div class="row"><span class="name">bad</span><span class="price" data-v="nope">x</span></div>
		<div class="row"><span class="name">ok-2</span><span class="price" data-v="2">x</span></div>`
	var target reportSideEffectPage
	report, err := p.ParseReport(&target, reportHTMLWrapper(html))
	if err != nil {
		t.Fatal(err)
	}
	if len(report.Issues) != 1 || report.Issues[0].FieldPath != "Rows[1].Price" {
		t.Fatalf("issues = %#v", report.Issues)
	}
	if len(target.Rows) != 2 {
		t.Fatalf("rows = %#v, want 2 survivors", target.Rows)
	}
	for i, wantName := range []string{"ok-1", "ok-2"} {
		if target.Rows[i].Name != wantName {
			t.Fatalf("row %d name = %q", i, target.Rows[i].Name)
		}
		if target.Rows[i].Digest != "stamped:"+wantName {
			t.Fatalf("untagged field mutation lost on surviving row %d: %q", i, target.Rows[i].Digest)
		}
	}
	// Stamp ran for all three DOM rows exactly once, including the dropped one.
	if got := reportCounters.get("row.Stamp"); got != 3 {
		t.Fatalf("Stamp called %d times, want 3 (no reruns, dropped row still ran once)", got)
	}
}

// --- registered function fallback, including its errors --------------------

var errRegisteredBoom = errors.New("registered function boom")

func TestParseReportRegisteredFunc(t *testing.T) {
	p := New()
	p.RegisterFunc("UpperText", func(node *goquery.Selection, args ...string) (interface{}, error) {
		return strings.ToUpper(strings.TrimSpace(node.Text())), nil
	})
	p.RegisterFunc("BoomFunc", func(node *goquery.Selection, args ...string) (interface{}, error) {
		return nil, fmt.Errorf("%w at %s", errRegisteredBoom, node.Text())
	})
	html := `
		<div class="r"><span class="t">alpha</span></div>
		<div class="r"><span class="t">beta</span></div>`
	var target struct {
		Rows []struct {
			Title string `pagser:".t->UpperText()"`
			Bad   string `pagser:".t->BoomFunc()"`
		} `pagser:".r"`
	}
	report, err := p.ParseReport(&target, reportHTMLWrapper(html))
	if err != nil {
		t.Fatal(err)
	}
	if len(report.Issues) != 2 {
		t.Fatalf("issues = %#v", report.Issues)
	}
	for i, issue := range report.Issues {
		if issue.FieldPath != fmt.Sprintf("Rows[%d].Bad", i) || issue.Category != ReportIssueFunc {
			t.Fatalf("issue %d = %#v", i, issue)
		}
		if !errors.Is(issue, errRegisteredBoom) {
			t.Fatalf("issue %d does not unwrap to registered error", i)
		}
	}
	if len(target.Rows) != 0 {
		t.Fatalf("every record carries the bad field, want no survivors: %#v", target.Rows)
	}
}

// --- value element []T record boundaries with nested value slices ----------

func TestParseReportValueRecordsAndNestedFilter(t *testing.T) {
	p := New()
	html := `
		<div class="g">
			<span class="gname">G1</span>
			<div class="i"><span class="v">1</span></div>
			<div class="i"><span class="v">nope</span></div>
			<div class="i"><span class="v">3</span></div>
		</div>
		<div class="g">
			<span class="gname">G2</span>
			<div class="i"><span class="v">4</span></div>
		</div>`
	var target struct {
		Groups []struct {
			Name  string `pagser:".gname"`
			Items []struct {
				Value int `pagser:".v"`
			} `pagser:".i"`
		} `pagser:".g"`
	}
	report, err := p.ParseReport(&target, reportHTMLWrapper(html))
	if err != nil {
		t.Fatal(err)
	}
	if len(report.Issues) != 1 || report.Issues[0].FieldPath != "Groups[0].Items[1].Value" {
		t.Fatalf("issues = %#v", report.Issues)
	}
	if len(target.Groups) != 2 {
		t.Fatalf("groups = %#v", target.Groups)
	}
	if len(target.Groups[0].Items) != 2 {
		t.Fatalf("nested bad child must be filtered while the parent survives: %#v", target.Groups[0].Items)
	}
	if target.Groups[0].Items[0].Value != 1 || target.Groups[0].Items[1].Value != 3 {
		t.Fatalf("surviving children = %#v", target.Groups[0].Items)
	}
	if len(target.Groups[1].Items) != 1 || target.Groups[1].Items[0].Value != 4 {
		t.Fatalf("second group = %#v", target.Groups[1])
	}
}

// --- builtin slice-returning function and uint bit-width overflow ----------

func TestParseReportBuiltinSlicesAndUintOverflow(t *testing.T) {
	p := New()
	html := `
		<div class="kw" data-v="a,b,c">x</div>
		<div class="u16">70000</div>
		<div class="u16ok">65535</div>`

	// A clean parse exercises the builtin slice-returning function and an
	// in-range value at the uint16 boundary.
	var okTarget struct {
		Tags  []string `pagser:".kw->attrSplit(data-v)"`
		MaxOk uint16   `pagser:".u16ok"`
	}
	if _, err := p.ParseReport(&okTarget, reportHTMLWrapper(html)); err != nil {
		t.Fatal(err)
	}
	if got := strings.Join(okTarget.Tags, ","); got != "a,b,c" {
		t.Fatalf("builtin attrSplit slice = %q, want a,b,c", got)
	}
	if okTarget.MaxOk != 65535 {
		t.Fatalf("in-range uint16 = %d, want 65535", okTarget.MaxOk)
	}

	// The out-of-range value must be a conversion error, not a truncation.
	var badTarget struct {
		Big uint16 `pagser:".u16"`
	}
	report, err := p.ParseReport(&badTarget, reportHTMLWrapper(html))
	if err == nil {
		t.Fatal("expected root record error from the overflowing uint16 field")
	}
	if badTarget.Big != 0 {
		t.Fatalf("failed root record must not commit truncated value: %d", badTarget.Big)
	}
	if len(report.Issues) != 1 {
		t.Fatalf("issues = %#v", report.Issues)
	}
	issue := report.Issues[0]
	if issue.FieldPath != "Big" || issue.Category != ReportIssueConversion {
		t.Fatalf("issue = %#v", issue)
	}
	if !strings.Contains(errors.Unwrap(issue).Error(), "overflows uint16") {
		t.Fatalf("overflow message wrong: %v", errors.Unwrap(issue))
	}
}

// --- custom Config (tag name, function symbol) stays in effect -------------
type reportCustomConfigData struct {
	Title string `query:"title@TitleCase()"`
	Rows  []struct {
		Price int `query:".price@text()"`
	} `query:".row"`
}

func (reportCustomConfigData) TitleCase(node *goquery.Selection, args ...string) (interface{}, error) {
	text := strings.TrimSpace(node.Text())
	if text == "" {
		return text, nil
	}
	return strings.ToUpper(text[:1]) + text[1:], nil
}

func TestParseReportCustomConfig(t *testing.T) {
	cfg := Config{TagName: "query", FuncSymbol: "@"}
	p, err := NewWithConfig(cfg)
	if err != nil {
		t.Fatal(err)
	}
	html := `<title>catalog</title><div class="row"><span class="price">1</span></div><div class="row"><span class="price">x</span></div>`
	var target reportCustomConfigData
	report, err := p.ParseReport(&target, reportHTMLWrapper(html))
	if err != nil {
		t.Fatal(err)
	}
	if target.Title != "Catalog" {
		t.Fatalf("custom tag/func config not applied: title=%q", target.Title)
	}
	if len(target.Rows) != 1 || target.Rows[0].Price != 1 {
		t.Fatalf("rows = %#v", target.Rows)
	}
	if len(report.Issues) != 1 || report.Issues[0].FieldPath != "Rows[1].Price" {
		t.Fatalf("report = %#v", report.Issues)
	}
}

// --- configuration errors are raised before any handler runs ---------------

type reportUnexportedTarget struct {
	OK     string `pagser:"title"`
	hidden string `pagser:"title"` //nolint:unused
}

func (t2 reportUnexportedTarget) CfgProbe(node *goquery.Selection, args ...string) (interface{}, error) {
	reportCounters.inc("config.probe")
	return "probe", nil
}

type reportBadMethodTarget struct {
	Title string `pagser:"title->BadSignature()"`
}

func (r reportBadMethodTarget) BadSignature(node *goquery.Selection) {
	reportCounters.inc("config.BadSignature")
}

type reportWrongArgTarget struct {
	Title string `pagser:"title->WrongArg()"`
}

// WrongArg looks handler-like but takes a string instead of *goquery.Selection;
// it must be rejected during validation rather than panicking at call time.
func (r reportWrongArgTarget) WrongArg(node string, args ...string) (interface{}, error) {
	reportCounters.inc("config.WrongArg")
	return node, nil
}

type reportUnsupportedTarget struct {
	Ch chan int `pagser:"title"`
}

func TestParseReportConfigErrors(t *testing.T) {
	reportCounters.reset()
	p := New()

	var unexported reportUnexportedTarget
	if _, err := p.ParseReport(&unexported, reportHTML); err == nil ||
		!strings.Contains(err.Error(), "unexported") {
		t.Fatalf("unexported tagged field: err = %v", err)
	}
	if got := reportCounters.get("config.probe"); got != 0 {
		t.Fatalf("handler ran before configuration error: %d", got)
	}

	var badMethod reportBadMethodTarget
	if _, err := p.ParseReport(&badMethod, reportHTML); err == nil ||
		!strings.Contains(err.Error(), "BadSignature") {
		t.Fatalf("illegal method signature: err = %v", err)
	}

	var wrongArg reportWrongArgTarget
	if _, err := p.ParseReport(&wrongArg, reportHTML); err == nil ||
		!strings.Contains(err.Error(), "WrongArg") {
		t.Fatalf("wrong first argument type: err = %v", err)
	}
	if got := reportCounters.get("config.WrongArg"); got != 0 {
		t.Fatalf("invalid handler ran instead of being rejected: %d", got)
	}

	var unsupported reportUnsupportedTarget
	if _, err := p.ParseReport(&unsupported, reportHTML); err == nil ||
		!strings.Contains(err.Error(), "unsupported") {
		t.Fatalf("unsupported shape: err = %v", err)
	}

	if _, err := p.ParseReport(reportPage{}, reportHTML); err == nil ||
		!strings.Contains(err.Error(), "non-pointer") {
		t.Fatalf("non-pointer target: err = %v", err)
	}
	var nilPage *reportPage
	if _, err := p.ParseReport(nilPage, reportHTML); err == nil ||
		!strings.Contains(err.Error(), "nil") {
		t.Fatalf("nil pointer target: err = %v", err)
	}

	doc, docErr := goquery.NewDocumentFromReader(strings.NewReader(reportHTML))
	if docErr != nil {
		t.Fatal(docErr)
	}
	if _, err := p.ParseSelectionReport(&unexported, doc.Selection); err == nil {
		t.Fatal("selection entry must apply the same configuration validation")
	}
}
