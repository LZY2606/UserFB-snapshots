package readability

import (
	"bytes"
	"fmt"
	"golang.org/x/net/html"
	"net/url"
	"reflect"
	"strings"
	"testing"
)

func TestSelectionBaseline(t *testing.T) {
	prose := strings.Repeat("独立文章说明，包含足够长的正文和明确内容。", 45)
	raw := `<html><head><title>文字来源</title></head><body><aside aria-hidden="true">KEEP_DUP 隐藏侧栏</aside><article><div>开头<span>甲　</span><b> \t 乙</b>😀<a href="javascript:void(0)">KEEP_DUP</a>` + prose + `</div><p>KEEP_DUP ` + prose + `</p><p>末段 ` + prose + `</p></article></body></html>`
	doc, err := html.Parse(strings.NewReader(raw))
	if err != nil {
		t.Fatal(err)
	}
	var before bytes.Buffer
	html.Render(&before, doc)
	page, _ := url.Parse("https://example.invalid/article")
	ps := NewParser()
	ps.CharThresholds = 100000
	a, err := ps.ParseDocument(doc, page)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Count(a.TextContent, "KEEP_DUP") != 2 || strings.Contains(a.TextContent, "隐藏侧栏") || strings.Contains(a.Content, "javascript:") {
		t.Fatalf("unexpected retained content: %q", a.TextContent)
	}
	if len(ps.attempts) != 4 {
		t.Fatalf("attempts=%d", len(ps.attempts))
	}
	var after bytes.Buffer
	html.Render(&after, doc)
	if before.String() != after.String() {
		t.Fatal("input changed")
	}
	b, err := ps.Parse(strings.NewReader(raw), page)
	if err != nil || a.Content != b.Content || a.TextContent != b.TextContent {
		t.Fatal("entry points differ", err)
	}
	_, has := reflect.TypeOf(a).FieldByName("SelectionTrace")
	fmt.Printf("CURRENT attempts=4 retained_duplicates=2 hidden_removed=true input_unchanged=true entries_equal=true selection_trace_field=%t\n", has)
	fmt.Println("SELECTION_BASELINE_OK")
}
