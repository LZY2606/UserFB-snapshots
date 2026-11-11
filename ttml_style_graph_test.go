package astisub

import (
	"bytes"
	"crypto/sha1"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// ttmlEffectiveSnapshot captures the TTML properties this feature cares about.
type ttmlEffectiveSnapshot struct {
	color       string
	fontWeight  string
	origin      string
	extent      string
	writingMode string
	fontSize    string
}

func snapshotOf(sa *StyleAttributes) ttmlEffectiveSnapshot {
	if sa == nil {
		return ttmlEffectiveSnapshot{}
	}
	snap := ttmlEffectiveSnapshot{
		fontWeight:  ptrValue(sa.TTMLFontWeight),
		origin:      ptrValue(sa.TTMLOrigin),
		extent:      ptrValue(sa.TTMLExtent),
		writingMode: ptrValue(sa.TTMLWritingMode),
		fontSize:    ptrValue(sa.TTMLFontSize),
	}
	if sa.TTMLColor != nil {
		snap.color = sa.TTMLColor.HTMLString()
	}
	return snap
}

func ptrValue(p *string) string {
	if p == nil {
		return ""
	}
	return *p
}

// effectiveAttributes recomputes the attributes effectively applying to a node
// from its referenced (possibly synthetic) style and its own inline style.
func effectiveAttributes(style *Style, inline *StyleAttributes) *StyleAttributes {
	out := &StyleAttributes{}
	var walk func(*Style)
	walk = func(st *Style) {
		if st == nil {
			return
		}
		// A synthetic style already carries the fully composed attributes.
		if st.StyleReferences != nil {
			out.mergeTTMLOverrides(st.InlineStyle)
			return
		}
		walk(st.Style)
		out.mergeTTMLOverrides(st.InlineStyle)
	}
	walk(style)
	out.mergeTTMLOverrides(inline)
	return out
}

const ttmlGraphDoc = `<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling">
<head>
<styling>
<style xml:id="base" tts:color="red" tts:extent="80% 20%" tts:fontSize="120%"/>
<style xml:id="bold" style="base" tts:fontWeight="bold"/>
<style xml:id="blue" tts:color="blue"/>
<style xml:id="onlyred" tts:color="red"/>
<style xml:id="onlyblue" tts:color="blue"/>
<style xml:id="mid1" style="base" tts:color="red"/>
<style xml:id="mid2" style="base" tts:fontWeight="bold"/>
<style xml:id="combo" style="mid1 mid2"/>
<style xml:id="ra" tts:origin="10% 20%" tts:extent="30% 40%"/>
<style xml:id="rb" tts:writingMode="tblr"/>
<style xml:id="wide" tts:extent="80% 20%"/>
<style xml:id="narrow" tts:extent="40% 10%"/>
<style xml:id="forwardchild" style="forwardparent" tts:fontWeight="bold"/>
<style xml:id="forwardparent" tts:color="red"/>
</styling>
<layout>
<region xml:id="r1" style="ra rb"/>
<region xml:id="r2" style="wide narrow"/>
</layout>
</head>
<body>
<div>
<p begin="1s" end="2s" style="bold blue" tts:color="green">first</p>
<p begin="2s" end="3s" style="bold">second</p>
<p begin="3s" end="4s" style="onlyred onlyblue">third</p>
<p begin="4s" end="5s" style="onlyblue onlyred">fourth</p>
<p begin="5s" end="6s" style="combo">fifth</p>
<p begin="6s" end="7s" region="r1">sixth</p>
<p begin="7s" end="8s" region="r2">seventh</p>
<p begin="8s" end="9s" style="wide narrow">eighth<br/>line</p>
<p begin="9s" end="10s" style="forwardchild">ninth <span style="bold blue">tenth</span></p>
</div>
</body>
</tt>`

func TestTTMLStyleGraph(t *testing.T) {
	t.Run("composition order and inline override", func(t *testing.T) {
		s, err := ReadFromTTML(strings.NewReader(ttmlGraphDoc))
		if err != nil {
			t.Fatalf("read failed: %v", err)
		}

		// First cue: style="bold blue" plus an inline green color.
		first := s.Items[0]
		if first.Style == nil || first.Style.StyleReferences == nil {
			t.Fatalf("expected multi-reference synthetic style, got %#v", first.Style)
		}
		if ids := first.Style.StyleReferences.IDs(); len(ids) != 2 || ids[0] != "bold" || ids[1] != "blue" {
			t.Fatalf("unexpected reference order: %v", ids)
		}
		eff := snapshotOf(effectiveAttributes(first.Style, first.InlineStyle))
		if eff.color != "#008000" { // inline green wins over blue and red
			t.Fatalf("expected green effective color, got %q", eff.color)
		}
		if eff.fontWeight != "bold" {
			t.Fatalf("expected bold inherited through bold->base chain, got %q", eff.fontWeight)
		}
		if eff.extent != "80% 20%" { // blue must not wipe base's extent
			t.Fatalf("expected extent from base, got %q", eff.extent)
		}
		if eff.fontSize != "120%" {
			t.Fatalf("expected font size from base to survive, got %q", eff.fontSize)
		}

		// Second cue: single reference keeps the public single-parent shape.
		second := s.Items[1]
		if second.Style != s.Styles["bold"] || second.Style.StyleReferences != nil {
			t.Fatalf("single reference must keep using the declared style")
		}
		eff = snapshotOf(effectiveAttributes(second.Style, second.InlineStyle))
		if eff.color != "#ff0000" || eff.fontWeight != "bold" || eff.extent != "80% 20%" {
			t.Fatalf("unexpected bold-only effective attributes: %+v", eff)
		}

		// Third/fourth cues: same refs in opposite order resolve differently.
		third := snapshotOf(effectiveAttributes(s.Items[2].Style, s.Items[2].InlineStyle))
		fourth := snapshotOf(effectiveAttributes(s.Items[3].Style, s.Items[3].InlineStyle))
		if third.color != "#0000ff" {
			t.Fatalf("later reference must win: expected blue, got %q", third.color)
		}
		if fourth.color != "#ff0000" {
			t.Fatalf("later reference must win: expected red, got %q", fourth.color)
		}
		if s.Items[2].Style == s.Items[3].Style {
			t.Fatalf("different reference orders must produce distinct synthetic styles")
		}
	})

	t.Run("diamond sharing and forward references", func(t *testing.T) {
		s, err := ReadFromTTML(strings.NewReader(ttmlGraphDoc))
		if err != nil {
			t.Fatalf("read failed: %v", err)
		}
		// combo -> mid1/mid2 -> base: base is a shared ancestor, not a cycle.
		combo := s.Items[4]
		eff := snapshotOf(effectiveAttributes(combo.Style, combo.InlineStyle))
		if eff.color != "#ff0000" || eff.fontWeight != "bold" || eff.fontSize != "120%" || eff.extent != "80% 20%" {
			t.Fatalf("diamond composition mismatch: %+v", eff)
		}
		// Forward reference: forwardchild is declared before forwardparent.
		ninth := s.Items[8]
		eff = snapshotOf(effectiveAttributes(ninth.Style, ninth.InlineStyle))
		if eff.color != "#ff0000" || eff.fontWeight != "bold" {
			t.Fatalf("forward reference composition mismatch: %+v", eff)
		}
		// Both mid styles keep referencing the same shared parent style.
		if s.Styles["mid1"].Style != s.Styles["base"] || s.Styles["mid2"].Style != s.Styles["base"] {
			t.Fatalf("shared parent reference lost")
		}
	})

	t.Run("declared styles are not mutated per node", func(t *testing.T) {
		s, err := ReadFromTTML(strings.NewReader(ttmlGraphDoc))
		if err != nil {
			t.Fatalf("read failed: %v", err)
		}
		bold := s.Styles["bold"]
		if bold.InlineStyle.TTMLColor != nil || bold.InlineStyle.TTMLExtent != nil {
			t.Fatalf("declared style must keep only its own attributes, got %+v", bold.InlineStyle)
		}
		if bold.Style != s.Styles["base"] {
			t.Fatalf("single parent link lost")
		}
		// Two nodes with the same reference list share one synthetic style.
		if s.Items[0].Style != s.Items[8].Lines[0].Items[1].Style {
			t.Fatalf("identical multi-reference lists must share the synthetic style object")
		}
	})

	t.Run("region layout derives from the composition", func(t *testing.T) {
		s, err := ReadFromTTML(strings.NewReader(ttmlGraphDoc))
		if err != nil {
			t.Fatalf("read failed: %v", err)
		}
		r1 := s.Regions["r1"]
		eff := r1.Style.InlineStyle
		if ptrValue(eff.TTMLOrigin) != "10% 20%" || ptrValue(eff.TTMLExtent) != "30% 40%" || ptrValue(eff.TTMLWritingMode) != "tblr" {
			t.Fatalf("region composition mismatch: %+v", eff)
		}
		if eff.WebVTTWidth != "30%" || eff.WebVTTLines != 8 {
			t.Fatalf("unexpected derived region dimensions: width=%q lines=%d", eff.WebVTTWidth, eff.WebVTTLines)
		}
		if eff.WebVTTSize != "40%" { // tb writing mode swaps width/height for cue size
			t.Fatalf("expected cue size 40%% from vertical writing mode, got %q", eff.WebVTTSize)
		}
		if eff.WebVTTRegionAnchor != "0%,100%" || eff.WebVTTViewportAnchor != "10%,60%" {
			t.Fatalf("unexpected derived anchors: %q %q", eff.WebVTTRegionAnchor, eff.WebVTTViewportAnchor)
		}
		if eff.WebVTTLine != "20%" || eff.WebVTTPosition == nil || eff.WebVTTPosition.XPosition != "10%" {
			t.Fatalf("unexpected derived origin cue settings: line=%q position=%#v", eff.WebVTTLine, eff.WebVTTPosition)
		}

		// r2: the second reference overrides extent; no stale derived size.
		r2 := s.Regions["r2"].Style.InlineStyle
		if ptrValue(r2.TTMLExtent) != "40% 10%" || r2.WebVTTWidth != "40%" || r2.WebVTTLines != 2 || r2.WebVTTSize != "40%" {
			t.Fatalf("overridden extent left stale layout values: %+v", r2)
		}

		// Same guarantee on a cue carrying a multi-reference style.
		cue := s.Items[7].Style.InlineStyle
		if ptrValue(cue.TTMLExtent) != "40% 10%" || cue.WebVTTSize != "40%" || cue.WebVTTLines != 2 {
			t.Fatalf("cue extent override mismatch: %+v", cue)
		}

		// The existing WebVTT conversion chain must render the composed layout.
		var vtt bytes.Buffer
		if err := s.WriteToWebVTT(&vtt); err != nil {
			t.Fatalf("webvtt write failed: %v", err)
		}
		out := vtt.String()
		for _, expected := range []string{
			"id:r1",
			"width:30%",
			"lines:8",
			"viewportanchor:10%,60%",
			"regionanchor:0%,100%",
			"region:r1",
			"size:40%",
		} {
			if !strings.Contains(out, expected) {
				t.Fatalf("webvtt output missing %q:\n%s", expected, out)
			}
		}
	})

	t.Run("roundtrip preserves effective attributes and references", func(t *testing.T) {
		s1, err := ReadFromTTML(strings.NewReader(ttmlGraphDoc))
		if err != nil {
			t.Fatalf("read failed: %v", err)
		}
		var first bytes.Buffer
		if err := s1.WriteToTTML(&first); err != nil {
			t.Fatalf("write failed: %v", err)
		}
		xml1 := first.String()
		for _, expected := range []string{
			`style="bold blue"`,
			`style="mid1 mid2"`,
			`style="ra rb"`,
			`style="wide narrow"`,
			`style="base"`,
		} {
			if !strings.Contains(xml1, expected) {
				t.Fatalf("written TTML missing %s:\n%s", expected, xml1)
			}
		}
		if strings.Contains(xml1, "astisub_merged_") {
			t.Fatalf("synthetic styles must not be emitted to TTML:\n%s", xml1)
		}

		s2, err := ReadFromTTML(bytes.NewReader(first.Bytes()))
		if err != nil {
			t.Fatalf("re-read failed: %v", err)
		}
		if len(s2.Items) != len(s1.Items) {
			t.Fatalf("event count changed: %d vs %d", len(s2.Items), len(s1.Items))
		}
		for i, item1 := range s1.Items {
			item2 := s2.Items[i]
			if item1.StartAt != item2.StartAt || item1.EndAt != item2.EndAt || item1.String() != item2.String() {
				t.Fatalf("item %d timing/text mismatch: %v %q vs %v %q", i, item1.StartAt, item1.String(), item2.StartAt, item2.String())
			}
			got1 := snapshotOf(effectiveAttributes(item1.Style, item1.InlineStyle))
			got2 := snapshotOf(effectiveAttributes(item2.Style, item2.InlineStyle))
			if got1 != got2 {
				t.Fatalf("item %d effective attributes changed after roundtrip: %+v vs %+v", i, got1, got2)
			}
			if len(item1.Lines) != len(item2.Lines) {
				t.Fatalf("item %d line count changed", i)
			}
			for j, line1 := range item1.Lines {
				line2 := item2.Lines[j]
				if len(line1.Items) != len(line2.Items) {
					t.Fatalf("line item count changed")
				}
				for k, li1 := range line1.Items {
					li2 := line2.Items[k]
					if li1.Text != li2.Text {
						t.Fatalf("line item text changed: %q vs %q", li1.Text, li2.Text)
					}
					if snapshotOf(effectiveAttributes(li1.Style, li1.InlineStyle)) != snapshotOf(effectiveAttributes(li2.Style, li2.InlineStyle)) {
						t.Fatalf("line item %q effective attributes changed", li1.Text)
					}
				}
			}
		}
		// The direct-child span keeps its multi-reference through the roundtrip.
		span := s2.Items[8].Lines[0].Items[1]
		if span.Style == nil || span.Style.StyleReferences == nil {
			t.Fatalf("span multi-reference lost after roundtrip")
		}
		if ids := span.Style.StyleReferences.IDs(); len(ids) != 2 || ids[0] != "bold" || ids[1] != "blue" {
			t.Fatalf("span reference list changed: %v", ids)
		}
		for id, r1 := range s1.Regions {
			r2, ok := s2.Regions[id]
			if !ok {
				t.Fatalf("region %s missing after roundtrip", id)
			}
			if snapshotOf(effectiveAttributes(r1.Style, r1.InlineStyle)) != snapshotOf(effectiveAttributes(r2.Style, r2.InlineStyle)) {
				t.Fatalf("region %s effective attributes changed", id)
			}
		}

		// A second write must be byte-identical: no accumulating synthetic styles.
		var second bytes.Buffer
		if err := s2.WriteToTTML(&second); err != nil {
			t.Fatalf("second write failed: %v", err)
		}
		if first.String() != second.String() {
			t.Fatalf("repeated write/read produced different TTML output")
		}
	})

	t.Run("synthetic style ids are deterministic and avoid collisions", func(t *testing.T) {
		s1, err := ReadFromTTML(strings.NewReader(ttmlGraphDoc))
		if err != nil {
			t.Fatalf("read failed: %v", err)
		}
		s2, err := ReadFromTTML(strings.NewReader(ttmlGraphDoc))
		if err != nil {
			t.Fatalf("read failed: %v", err)
		}
		id1 := s1.Items[0].Style.ID
		id2 := s2.Items[0].Style.ID
		if id1 != id2 {
			t.Fatalf("synthetic id not deterministic: %q vs %q", id1, id2)
		}
		sum := sha1.Sum([]byte("bold blue"))
		collidingID := "astisub_merged_" + hex.EncodeToString(sum[:8])
		doc := strings.Replace(ttmlGraphDoc,
			`<style xml:id="blue" tts:color="blue"/>`,
			`<style xml:id="blue" tts:color="blue"/><style xml:id="`+collidingID+`" tts:color="purple"/>`, 1)
		s3, err := ReadFromTTML(strings.NewReader(doc))
		if err != nil {
			t.Fatalf("read with colliding id failed: %v", err)
		}
		if s3.Items[0].Style.ID != collidingID+"_1" {
			t.Fatalf("expected collision suffix, got %q", s3.Items[0].Style.ID)
		}
	})

	t.Run("body and div style inheritance still applies", func(t *testing.T) {
		xml := `<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling"><head><styling>
<style xml:id="a" tts:color="red"/><style xml:id="b" tts:fontWeight="bold"/>
</styling></head><body style="a b"><div><p begin="1s" end="2s">inherited</p></div>
<div style="a"><p begin="2s" end="3s">overridden</p></div></body></tt>`
		s, err := ReadFromTTML(strings.NewReader(xml))
		if err != nil {
			t.Fatalf("read failed: %v", err)
		}
		first := snapshotOf(effectiveAttributes(s.Items[0].Style, s.Items[0].InlineStyle))
		if first.color != "#ff0000" || first.fontWeight != "bold" {
			t.Fatalf("body multi-reference inheritance mismatch: %+v", first)
		}
		second := snapshotOf(effectiveAttributes(s.Items[1].Style, s.Items[1].InlineStyle))
		if second.color != "#ff0000" || second.fontWeight != "" {
			t.Fatalf("div style should override body style: %+v", second)
		}
	})

	t.Run("cycles and missing references", func(t *testing.T) {
		cases := []struct {
			name string
			xml  string
			want []string
		}{
			{
				name: "direct cycle",
				xml:  `<tt xmlns="http://www.w3.org/ns/ttml"><head><styling><style xml:id="a" style="b"/><style xml:id="b" style="a"/></styling></head><body><div><p begin="1s" end="2s">x</p></div></body></tt>`,
				want: []string{"cycle", "a", "b"},
			},
			{
				name: "self cycle",
				xml:  `<tt xmlns="http://www.w3.org/ns/ttml"><head><styling><style xml:id="c" style="c"/></styling></head><body><div><p begin="1s" end="2s">x</p></div></body></tt>`,
				want: []string{"cycle", "c"},
			},
			{
				name: "cycle through multi reference",
				xml:  `<tt xmlns="http://www.w3.org/ns/ttml"><head><styling><style xml:id="x" style="y"/><style xml:id="y" style="x z"/><style xml:id="z" tts:color="red"/></styling></head><body><div><p begin="1s" end="2s">x</p></div></body></tt>`,
				want: []string{"cycle", "x", "y"},
			},
			{
				name: "unused style with missing reference still errors",
				xml:  `<tt xmlns="http://www.w3.org/ns/ttml"><head><styling><style xml:id="a" style="ghost"/></styling></head><body><div><p begin="1s" end="2s">x</p></div></body></tt>`,
				want: []string{"ghost", "doesn't exist"},
			},
			{
				name: "cue missing second reference",
				xml:  `<tt xmlns="http://www.w3.org/ns/ttml"><head><styling><style xml:id="a" tts:color="red"/></styling></head><body><div><p begin="1s" end="2s" style="a ghost">x</p></div></body></tt>`,
				want: []string{"ghost", "doesn't exist"},
			},
			{
				name: "region missing reference",
				xml:  `<tt xmlns="http://www.w3.org/ns/ttml"><head><layout><region xml:id="r1" style="ghost"/></layout></head><body><div><p begin="1s" end="2s" region="r1">x</p></div></body></tt>`,
				want: []string{"ghost", "doesn't exist"},
			},
		}
		for _, tc := range cases {
			t.Run(tc.name, func(t *testing.T) {
				_, err := ReadFromTTML(strings.NewReader(tc.xml))
				if err == nil {
					t.Fatalf("expected an error")
				}
				msg := err.Error()
				for _, fragment := range tc.want {
					if !strings.Contains(msg, fragment) {
						t.Fatalf("error %q does not contain %q", msg, fragment)
					}
				}
			})
		}
	})

	t.Run("whitespace tokenization", func(t *testing.T) {
		xml := `<tt xmlns="http://www.w3.org/ns/ttml"><head><styling>
<style xml:id="a" tts:color="red"/><style xml:id="b" tts:fontWeight="bold"/>
</styling></head><body><div><p begin="1s" end="2s" style="  a		b
">x</p></div></body></tt>`
		s, err := ReadFromTTML(strings.NewReader(xml))
		if err != nil {
			t.Fatalf("read failed: %v", err)
		}
		eff := snapshotOf(effectiveAttributes(s.Items[0].Style, s.Items[0].InlineStyle))
		if eff.color != "#ff0000" || eff.fontWeight != "bold" {
			t.Fatalf("whitespace-separated references mismatch: %+v", eff)
		}
	})

	t.Run("open file accepts multi references", func(t *testing.T) {
		xml := `<tt xmlns="http://www.w3.org/ns/ttml"><head><styling>
<style xml:id="a" tts:color="red"/><style xml:id="b" tts:fontWeight="bold"/>
</styling></head><body><div><p begin="1s" end="2s" style="a b">x</p></div></body></tt>`
		path := filepath.Join(t.TempDir(), "multi.ttml")
		if err := os.WriteFile(path, []byte(xml), 0o600); err != nil {
			t.Fatalf("write temp file failed: %v", err)
		}
		s, err := OpenFile(path)
		if err != nil {
			t.Fatalf("OpenFile rejected multi references: %v", err)
		}
		eff := snapshotOf(effectiveAttributes(s.Items[0].Style, s.Items[0].InlineStyle))
		if eff.color != "#ff0000" || eff.fontWeight != "bold" {
			t.Fatalf("OpenFile composition mismatch: %+v", eff)
		}
	})

	t.Run("optimize keeps transitively referenced styles", func(t *testing.T) {
		xml := `<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling"><head><styling>
<style xml:id="base" tts:color="red" tts:extent="80% 20%"/>
<style xml:id="bold" style="base" tts:fontWeight="bold"/>
<style xml:id="blue" tts:color="blue"/>
<style xml:id="unused" tts:color="green"/>
</styling></head><body><div>
<p begin="1s" end="2s" style="bold blue">x</p>
</div></body></tt>`
		s, err := ReadFromTTML(strings.NewReader(xml))
		if err != nil {
			t.Fatalf("read failed: %v", err)
		}
		s.Optimize()
		if _, ok := s.Styles["unused"]; ok {
			t.Fatalf("genuinely unused style was not removed")
		}
		// The cue only points at a synthetic style; bold, blue and base must
		// remain reachable through its reference list after optimization.
		for _, id := range []string{"base", "bold", "blue"} {
			if _, ok := s.Styles[id]; !ok {
				t.Fatalf("transitively referenced style %s was removed", id)
			}
		}
		var out bytes.Buffer
		if err := s.WriteToTTML(&out); err != nil {
			t.Fatalf("write after optimize failed: %v", err)
		}
		if !strings.Contains(out.String(), `style="bold blue"`) {
			t.Fatalf("reference list lost after optimize:\n%s", out.String())
		}
		if _, err := ReadFromTTML(bytes.NewReader(out.Bytes())); err != nil {
			t.Fatalf("optimized output no longer parses: %v", err)
		}
	})
}
