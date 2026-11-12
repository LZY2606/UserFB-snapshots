package cascadia
import (
 "fmt"
 "strings"
 "testing"
 "golang.org/x/net/html"
)
func TestRelativeBaseline(t *testing.T) {
 doc, err := html.Parse(strings.NewReader(`<main class="outside"><article id="a"><h2><span class="flag">x</span></h2></article><article id="b"></article><aside><span class="flag">y</span></aside></main>`))
 if err != nil { t.Fatal(err) }
 for _, q := range []string{"article:has(.flag)", "article:has(> h2 .flag)", "article:has(.outside .flag)", "article:has(+ aside .flag)"} {
  s, e := Parse(q); if e != nil { t.Fatal(e) }
  nodes := QueryAll(doc, s)
  fmt.Printf("OBSERVED %s count=%d\n", q, len(nodes))
  if q == "article:has(.flag)" && len(nodes) != 1 { t.Fatal("ordinary descendant broken") }
 }
 fmt.Println("BASELINE_RELATIVE_OK")
}
