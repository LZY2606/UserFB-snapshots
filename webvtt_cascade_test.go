package astisub_test

import (
	"bytes"
	"encoding/xml"
	"io"
	"io/ioutil"
	"strings"
	"testing"
	"time"

	"github.com/asticode/go-astisub"
	"github.com/asticode/go-astisub/task_support/taskcheck"
)

// ttmlSpan is one decoded <span> (or the attributes of its parent <p>).
type ttmlSpan struct {
	text  string
	attrs map[string]string
}

// ttmlCue decodes the parts of a TTML <p> the cascade tests assert on.
type ttmlCue struct {
	attrs map[string]string
	spans []ttmlSpan
}

func ttmlAttrMap(se xml.StartElement) map[string]string {
	m := map[string]string{}
	for _, a := range se.Attr {
		m[a.Name.Local] = a.Value
	}
	return m
}

// decodeTTMLCues walks the TTML token stream and records every <p> with its
// attributes and direct <span> children (<br> only separates lines).
func decodeTTMLCues(t *testing.T, data []byte) []ttmlCue {
	t.Helper()
	dec := xml.NewDecoder(bytes.NewReader(data))
	var cues []ttmlCue
	var current *ttmlCue
	var inSpan *ttmlSpan
	for {
		tok, err := dec.Token()
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatalf("decoding ttml failed: %v", err)
		}
		switch el := tok.(type) {
		case xml.StartElement:
			switch el.Name.Local {
			case "p":
				current = &ttmlCue{attrs: ttmlAttrMap(el)}
			case "span":
				inSpan = &ttmlSpan{attrs: ttmlAttrMap(el)}
			}
		case xml.CharData:
			if inSpan != nil {
				inSpan.text += string(el)
			}
		case xml.EndElement:
			switch el.Name.Local {
			case "span":
				if current != nil && inSpan != nil {
					current.spans = append(current.spans, *inSpan)
				}
				inSpan = nil
			case "p":
				if current != nil {
					cues = append(cues, *current)
				}
				current = nil
			}
		}
	}
	return cues
}

func spanByText(cues []ttmlCue, text string) (ttmlSpan, bool) {
	for _, cue := range cues {
		for _, span := range cue.spans {
			if span.text == text {
				return span, true
			}
		}
	}
	return ttmlSpan{}, false
}

// cascadeAttrs projects the five cascade properties out of a decoded attribute map.
func cascadeAttrs(attrs map[string]string) map[string]string {
	out := map[string]string{}
	for _, key := range []string{"color", "backgroundColor", "fontWeight", "fontStyle", "textDecoration"} {
		if v, ok := attrs[key]; ok {
			out[key] = v
		}
	}
	return out
}

func mapsEqual(a, b map[string]string) bool {
	if len(a) != len(b) {
		return false
	}
	for k, v := range a {
		if b[k] != v {
			return false
		}
	}
	return true
}

// writeTTML exports the subtitles and returns the decoded cues plus the raw bytes.
func writeTTML(t *testing.T, s *astisub.Subtitles) ([]ttmlCue, []byte) {
	t.Helper()
	buf := &bytes.Buffer{}
	if err := s.WriteToTTML(buf); err != nil {
		t.Fatalf("WriteToTTML failed: %v", err)
	}
	return decodeTTMLCues(t, buf.Bytes()), buf.Bytes()
}

func readWebVTT(t *testing.T, input string) *astisub.Subtitles {
	t.Helper()
	s, err := astisub.ReadFromWebVTT(strings.NewReader(input))
	if err != nil {
		t.Fatalf("ReadFromWebVTT failed: %v", err)
	}
	return s
}

// webVTTStyleHolder returns the style object currently carrying STYLE lines.
func webVTTStyleHolder(s *astisub.Subtitles) *astisub.Style {
	for _, st := range s.Styles {
		if st.InlineStyle != nil && len(st.InlineStyle.WebVTTStyles) > 0 {
			return st
		}
	}
	return nil
}

// TestCueCascade covers the stylesheet engine itself through the public
// ReadFromWebVTT -> WriteToTTML entry points: selector matching, nested
// inheritance, per-node priority, grouping/comments, error recovery, the
// b/i/u off-switch and the legacy fallback.
func TestCueCascade(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Scenario A: root declarations, inheritance and switching the
		// built-in b/i/u effects off explicitly.
		inputA := "WEBVTT\n\nSTYLE\n" +
			"::cue {\n" +
			"  color: red;\n" +
			"  background-color: #101010;\n" +
			"}\n" +
			"::cue(.blue) { color: blue; }\n" +
			"::cue(b) { font-weight: normal; }\n" +
			"::cue(i) { font-style: normal; }\n" +
			"::cue(u) { text-decoration: none; }\n" +
			"\n00:00:01.000 --> 00:00:03.000\n" +
			"root <c.blue>blue</c> <b>nb</b> <i>ni</i> <u>nu</u> <b><i>deep</i></b>\n"
		cuesA, _ := writeTTML(t, readWebVTT(t, inputA))

		rootWant := map[string]string{
			"color": "#ff0000", "backgroundColor": "#101010",
			"fontWeight": "normal", "fontStyle": "normal", "textDecoration": "none",
		}
		rootSpan, ok := spanByText(cuesA, "root ")
		c.Check(ok, "scenario A: root run exported")
		c.Check(mapsEqual(cascadeAttrs(rootSpan.attrs), rootWant), "scenario A: root computed style got %v want %v", cascadeAttrs(rootSpan.attrs), rootWant)
		c.Check(mapsEqual(cascadeAttrs(cuesA[0].attrs), rootWant), "scenario A: cue root (<p>) computed style got %v want %v", cascadeAttrs(cuesA[0].attrs), rootWant)

		blueSpan, ok := spanByText(cuesA, "blue")
		c.Check(ok, "scenario A: blue run exported")
		c.Check(blueSpan.attrs["color"] == "#0000ff", "scenario A: class color got %q want #0000ff", blueSpan.attrs["color"])
		c.Check(blueSpan.attrs["backgroundColor"] == "#101010", "scenario A: background inherited got %q", blueSpan.attrs["backgroundColor"])

		bSpan, _ := spanByText(cuesA, "nb")
		c.Check(bSpan.attrs["fontWeight"] == "normal", "scenario A: <b> switched off got %q want normal", bSpan.attrs["fontWeight"])
		c.Check(bSpan.attrs["color"] == "#ff0000", "scenario A: <b> inherits root color got %q", bSpan.attrs["color"])
		iSpan, _ := spanByText(cuesA, "ni")
		c.Check(iSpan.attrs["fontStyle"] == "normal", "scenario A: <i> switched off got %q want normal", iSpan.attrs["fontStyle"])
		uSpan, _ := spanByText(cuesA, "nu")
		c.Check(uSpan.attrs["textDecoration"] == "none", "scenario A: <u> switched off got %q want none", uSpan.attrs["textDecoration"])

		// Nested stack <b><i>: both tag rules apply on their own node and
		// the color/background inherit all the way from the cue root.
		deep, _ := spanByText(cuesA, "deep")
		c.Check(deep.attrs["fontWeight"] == "normal", "scenario A: nested <b> switched off got %q", deep.attrs["fontWeight"])
		c.Check(deep.attrs["fontStyle"] == "normal", "scenario A: nested <i> switched off got %q", deep.attrs["fontStyle"])
		c.Check(deep.attrs["color"] == "#ff0000", "scenario A: nested run inherits root color got %q", deep.attrs["color"])
		c.Check(deep.attrs["backgroundColor"] == "#101010", "scenario A: nested run inherits root background got %q", deep.attrs["backgroundColor"])

		// Scenario B: !important only competes inside one node, plus
		// specificity and source order on the same node.
		inputB := "WEBVTT\n\nSTYLE\n" +
			"::cue { color: red !important; }\n" +
			"::cue(.blue) { color: blue; }\n" +
			"::cue(c) { color: yellow; }\n" +
			"::cue(.hot) { color: green; }\n" +
			"::cue(c.hot) { color: blue !important; }\n" +
			"::cue(.hot) { color: yellow; }\n" +
			"\n00:00:01.000 --> 00:00:03.000\n" +
			"plain <c.blue>B</c> <c.hot>H</c>\n"
		cuesB, _ := writeTTML(t, readWebVTT(t, inputB))
		plainB, _ := spanByText(cuesB, "plain ")
		c.Check(plainB.attrs["color"] == "#ff0000", "scenario B: root important red got %q", plainB.attrs["color"])
		spanB, _ := spanByText(cuesB, "B")
		c.Check(spanB.attrs["color"] == "#0000ff", "scenario B: child class beats root !important and low-specificity type rule, got %q", spanB.attrs["color"])
		spanH, _ := spanByText(cuesB, "H")
		c.Check(spanH.attrs["color"] == "#0000ff", "scenario B: same-node !important beats later normal declarations, got %q", spanH.attrs["color"])

		inputOrder := "WEBVTT\n\nSTYLE\n" +
			"::cue(.tie) { color: green; }\n" +
			"::cue(.tie) { color: yellow; }\n" +
			"\n00:00:01.000 --> 00:00:03.000\n" +
			"<c.tie>T</c>\n"
		cuesOrder, _ := writeTTML(t, readWebVTT(t, inputOrder))
		spanT, _ := spanByText(cuesOrder, "T")
		c.Check(spanT.attrs["color"] == "#ffff00", "scenario B: equal specificity resolved by source order got %q want #ffff00", spanT.attrs["color"])

		// Scenario C: selector groups, comments (brackets and semicolons
		// inside them are inert), invalid declaration/selector recovery,
		// case-sensitive classes, inherit and initial.
		inputC := "WEBVTT\n\nSTYLE\n" +
			"/* comment containing ; { } ::cue(.z){color:red} and (parens) */\n" +
			"::cue(b), ::cue(i), ::cue(voice::cue) { color: #a1b2c3; }\n" +
			"::cue { bogus-property: 1; color: green; font-weight: BOLD; }\n" +
			"::cue(.1bad) { color: red; }\n" +
			"::cue(c.Win) { color: magenta; }\n" +
			"::cue(.init) { color: initial; }\n" +
			"::cue(.inh) { color: inherit; font-weight: inherit; }\n" +
			"::cue(.badval) { color: not-a-color; font-style: italic; }\n" +
			"::cue(.after) { color: yellow; }\n" +
			"\n00:00:01.000 --> 00:00:03.000\n" +
			"plain <b>gb</b> <c.Win>W</c> <c.init>I</c> <c.inh>H</c> <c.badval>V</c> <c.after>A</c> <c.win>lower</c>\n"
		cuesC, _ := writeTTML(t, readWebVTT(t, inputC))
		plainC, _ := spanByText(cuesC, "plain ")
		c.Check(plainC.attrs["color"] == "#008000", "scenario C: root color after unknown declaration got %q want #008000", plainC.attrs["color"])
		c.Check(plainC.attrs["fontWeight"] == "bold", "scenario C: ASCII case-insensitive keyword got %q want bold", plainC.attrs["fontWeight"])
		gb, _ := spanByText(cuesC, "gb")
		c.Check(gb.attrs["color"] == "#a1b2c3", "scenario C: grouped selector still matches after unsupported group member got %q", gb.attrs["color"])
		win, _ := spanByText(cuesC, "W")
		c.Check(win.attrs["color"] == "#ff00ff", "scenario C: case-sensitive class match got %q want #ff00ff", win.attrs["color"])
		lower, _ := spanByText(cuesC, "lower")
		c.Check(lower.attrs["color"] == "#008000", "scenario C: lowercase class does not match .Win, inherits root got %q", lower.attrs["color"])
		initSpan, _ := spanByText(cuesC, "I")
		c.Check(initSpan.attrs["color"] == "#ffffff", "scenario C: initial resets to white got %q want #ffffff", initSpan.attrs["color"])
		inhSpan, _ := spanByText(cuesC, "H")
		c.Check(inhSpan.attrs["color"] == "#008000", "scenario C: inherit takes parent color got %q want #008000", inhSpan.attrs["color"])
		c.Check(inhSpan.attrs["fontWeight"] == "bold", "scenario C: inherit takes parent weight got %q want bold", inhSpan.attrs["fontWeight"])
		badval, _ := spanByText(cuesC, "V")
		c.Check(badval.attrs["fontStyle"] == "italic", "scenario C: valid declaration kept after invalid color got %q want italic", badval.attrs["fontStyle"])
		c.Check(badval.attrs["color"] == "#008000", "scenario C: illegal color ignored, inherits root got %q", badval.attrs["color"])
		after, _ := spanByText(cuesC, "A")
		c.Check(after.attrs["color"] == "#ffff00", "scenario C: later valid rule not swallowed got %q want #ffff00", after.attrs["color"])

		// inherit on the root itself resolves to the initial value.
		inputRootInherit := "WEBVTT\n\nSTYLE\n" +
			"::cue { font-weight: inherit; text-decoration: inherit; }\n" +
			"\n00:00:01.000 --> 00:00:03.000\nroot\n"
		cuesRI, _ := writeTTML(t, readWebVTT(t, inputRootInherit))
		c.Check(cuesRI[0].attrs["fontWeight"] == "normal", "root inherit falls back to initial weight got %q", cuesRI[0].attrs["fontWeight"])
		c.Check(cuesRI[0].attrs["textDecoration"] == "none", "root inherit falls back to initial decoration got %q", cuesRI[0].attrs["textDecoration"])

		// Scenario D: multiple STYLE blocks concatenate in read order.
		inputD := "WEBVTT\n\nSTYLE\n::cue(.x) { color: red; }\n\nSTYLE\n::cue(.x) { color: blue; }\n" +
			"\n00:00:01.000 --> 00:00:03.000\n<c.x>X</c>\n"
		cuesD, _ := writeTTML(t, readWebVTT(t, inputD))
		spanX, _ := spanByText(cuesD, "X")
		c.Check(spanX.attrs["color"] == "#0000ff", "scenario D: second STYLE block wins on tie got %q want #0000ff", spanX.attrs["color"])

		// Scenario E: class name grammar (letter/underscore start, then
		// digits/underscore/hyphen); illegal selectors are dropped.
		inputE := "WEBVTT\n\nSTYLE\n" +
			"::cue { color: black; }\n" +
			"::cue(c._a) { color: red; }\n" +
			"::cue(.a-1_b2) { color: blue; }\n" +
			"::cue(.1x) { color: yellow; }\n" +
			"::cue(.A) { color: green; }\n" +
			"::cue(c.multi.one) { color: cyan; }\n" +
			"::cue(.multi) { color: yellow; }\n" +
			"\n00:00:01.000 --> 00:00:03.000\n" +
			"<c._a>X</c> <c.a-1_b2>Y</c> <c.1x>Z</c> <c.a>w</c> <c.multi.one>M</c>\n"
		cuesE, _ := writeTTML(t, readWebVTT(t, inputE))
		spanEX, _ := spanByText(cuesE, "X")
		c.Check(spanEX.attrs["color"] == "#ff0000", "scenario E: underscore class got %q want #ff0000", spanEX.attrs["color"])
		spanEY, _ := spanByText(cuesE, "Y")
		c.Check(spanEY.attrs["color"] == "#0000ff", "scenario E: hyphen/digit class got %q want #0000ff", spanEY.attrs["color"])
		spanEZ, _ := spanByText(cuesE, "Z")
		c.Check(spanEZ.attrs["color"] == "#000000", "scenario E: illegal class selector ignored, inherits root got %q", spanEZ.attrs["color"])
		spanEw, _ := spanByText(cuesE, "w")
		c.Check(spanEw.attrs["color"] == "#000000", "scenario E: class case mismatch ignored, inherits root got %q", spanEw.attrs["color"])
		spanEM, _ := spanByText(cuesE, "M")
		c.Check(spanEM.attrs["color"] == "#00ffff", "scenario E: two-class selector specificity 21 beats single class got %q want #00ffff", spanEM.attrs["color"])

		// Scenario F: STYLE blocks without any supported declaration keep
		// the legacy path (no five-property explosion), and so do inputs
		// without STYLE at all.
		inputInactive := "WEBVTT\n\nSTYLE\n::cue { background-image: url(noise.png); }\n" +
			"\n00:00:01.000 --> 00:00:03.000\n<c.red>R</c> plain\n"
		cuesInactive, _ := writeTTML(t, readWebVTT(t, inputInactive))
		redLegacy, _ := spanByText(cuesInactive, "R")
		c.Check(redLegacy.attrs["color"] == "#ff0000", "scenario F: built-in red still exported on inactive sheet got %q", redLegacy.attrs["color"])
		plainLegacy, _ := spanByText(cuesInactive, " plain")
		_, hasWeight := plainLegacy.attrs["fontWeight"]
		c.Check(!hasWeight, "scenario F: inactive sheet must not synthesize fontWeight, got %v", plainLegacy.attrs)

		inputNoStyle := "WEBVTT\n\n00:00:01.000 --> 00:00:03.000\n" +
			"<b>B</b><i>I</i><u>U</u><c.bg_cyan>bg</c>\n"
		cuesNoStyle, _ := writeTTML(t, readWebVTT(t, inputNoStyle))
		bgLegacy, _ := spanByText(cuesNoStyle, "bg")
		c.Check(bgLegacy.attrs["backgroundColor"] == "#00ffff", "scenario F: built-in bg class on legacy path got %q", bgLegacy.attrs["backgroundColor"])
		bLegacy, _ := spanByText(cuesNoStyle, "B")
		_, bHasWeight := bLegacy.attrs["fontWeight"]
		c.Check(!bHasWeight, "scenario F: legacy <b> never emitted fontWeight, got %v", bLegacy.attrs)

		// Scenario G: a native TTML document never enters the cascade.
		nativeIn, err := ioutil.ReadFile("./testdata/example-in.ttml")
		c.Check(err == nil, "scenario G: reading native ttml fixture: %v", err)
		nativeSubs, err := astisub.ReadFromTTML(bytes.NewReader(nativeIn))
		c.Check(err == nil, "scenario G: ReadFromTTML: %v", err)
		nativeBuf := &bytes.Buffer{}
		c.Check(nativeSubs.WriteToTTML(nativeBuf) == nil, "scenario G: WriteToTTML native")
		golden, err := ioutil.ReadFile("./testdata/example-out.ttml")
		c.Check(err == nil, "scenario G: reading golden ttml: %v", err)
		c.Check(bytes.Equal(nativeBuf.Bytes(), golden), "scenario G: native TTML output unchanged from golden")
	})
}

// TestCueCascadeEdits verifies that recomputation always uses the current
// STYLE lines and the current WebVTTTags, that exports do not mutate input
// objects, and that runs sharing one InlineStyle pointer stay isolated.
func TestCueCascadeEdits(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		input := "WEBVTT\n\nSTYLE\n::cue(.brand) { color: #123456; }\n" +
			"\n00:00:01.000 --> 00:00:03.000\n<c.brand>A</c><c.red>B</c>\n"
		s := readWebVTT(t, input)

		cues, _ := writeTTML(t, s)
		spanA, _ := spanByText(cues, "A")
		c.Check(spanA.attrs["color"] == "#123456", "initial brand color got %q want #123456", spanA.attrs["color"])
		spanB, _ := spanByText(cues, "B")
		c.Check(spanB.attrs["color"] == "#ff0000", "built-in red default got %q want #ff0000", spanB.attrs["color"])

		// First rule edit.
		holder := webVTTStyleHolder(s)
		c.Check(holder != nil, "STYLE holder found")
		holder.InlineStyle.WebVTTStyles = []string{"::cue(.brand) { color: #654321; }"}
		cues, _ = writeTTML(t, s)
		spanA, _ = spanByText(cues, "A")
		c.Check(spanA.attrs["color"] == "#654321", "first rule edit reflected got %q want #654321", spanA.attrs["color"])

		// Second rule edit (class/rule re-edited again).
		holder.InlineStyle.WebVTTStyles = []string{"::cue(.brand) { color: #abcdef; }", "::cue(.extra) { color: green; }"}
		cues, _ = writeTTML(t, s)
		spanA, _ = spanByText(cues, "A")
		c.Check(spanA.attrs["color"] == "#abcdef", "second rule edit reflected got %q want #abcdef", spanA.attrs["color"])

		// Tag edit: move run B onto the new class.
		var runB *astisub.LineItem
		for li := range s.Items[0].Lines[0].Items {
			if s.Items[0].Lines[0].Items[li].Text == "B" {
				runB = &s.Items[0].Lines[0].Items[li]
			}
		}
		c.Check(runB != nil, "run B located")
		runB.InlineStyle.WebVTTTags = []astisub.WebVTTTag{{Name: "c", Classes: []string{"extra"}}}
		cues, _ = writeTTML(t, s)
		spanB, _ = spanByText(cues, "B")
		c.Check(spanB.attrs["color"] == "#008000", "WebVTTTags edit reflected got %q want #008000", spanB.attrs["color"])

		// Removing the tags of run A makes it fall back to the root (no
		// root color rule here -> initial white).
		var runA *astisub.LineItem
		for li := range s.Items[0].Lines[0].Items {
			if s.Items[0].Lines[0].Items[li].Text == "A" {
				runA = &s.Items[0].Lines[0].Items[li]
			}
		}
		c.Check(runA != nil, "run A located")
		runA.InlineStyle.WebVTTTags = nil
		cues, _ = writeTTML(t, s)
		spanA, _ = spanByText(cues, "A")
		c.Check(spanA.attrs["color"] == "#ffffff", "untagged run resolves to initial color got %q want #ffffff", spanA.attrs["color"])

		// Editing the styles down to unsupported declarations switches the
		// whole document back to the legacy path: the five-property
		// synthesis disappears (only the attributes the legacy parser
		// derived are emitted).
		holder.InlineStyle.WebVTTStyles = []string{"::cue { background-image: url(x); }"}
		cues, _ = writeTTML(t, s)
		spanB, _ = spanByText(cues, "B")
		for _, prop := range []string{"fontWeight", "fontStyle", "textDecoration", "backgroundColor"} {
			_, present := spanB.attrs[prop]
			c.Check(!present, "inactive edited sheet uses legacy path: %s not synthesized, got %v", prop, spanB.attrs)
		}

		// Exports must not rewrite input objects.
		s2 := readWebVTT(t, input)
		firstRun := s2.Items[0].Lines[0].Items[0]
		beforeColor := firstRun.InlineStyle.TTMLColor
		beforeStyleLines := append([]string(nil), webVTTStyleHolder(s2).InlineStyle.WebVTTStyles...)
		_, raw1 := writeTTML(t, s2)
		_, raw2 := writeTTML(t, s2)
		c.Check(bytes.Equal(raw1, raw2), "consecutive exports are byte-identical")
		c.Check(firstRun.InlineStyle.TTMLColor == beforeColor, "export does not mutate run TTMLColor pointer")
		c.Check(strings.Join(webVTTStyleHolder(s2).InlineStyle.WebVTTStyles, "\n") == strings.Join(beforeStyleLines, "\n"),
			"export does not mutate WebVTTStyles")
		c.Check(firstRun.Text == "A", "export does not mutate run text")
		c.Check(firstRun.InlineStyle.TTMLFontWeight == nil, "cascade result is not written back into InlineStyle")

		// Shared InlineStyle pointer: two runs render identically and the
		// export never writes computed values into the shared object.
		shared := "WEBVTT\n\nSTYLE\n::cue(.brand) { color: #123456; font-weight: bold; }\n" +
			"\n00:00:01.000 --> 00:00:02.000\n<c.brand>one</c>\n" +
			"\n00:00:02.000 --> 00:00:03.000\n<c.brand>two</c>\n"
		s3 := readWebVTT(t, shared)
		sharedStyle := s3.Items[0].Lines[0].Items[0].InlineStyle
		s3.Items[1].Lines[0].Items[0].InlineStyle = sharedStyle
		cuesShared, _ := writeTTML(t, s3)
		one, ok1 := spanByText(cuesShared, "one")
		two, ok2 := spanByText(cuesShared, "two")
		c.Check(ok1 && ok2, "both shared-pointer runs exported")
		c.Check(mapsEqual(cascadeAttrs(one.attrs), cascadeAttrs(two.attrs)), "shared pointer renders identically: %v vs %v", one.attrs, two.attrs)
		c.Check(sharedStyle.TTMLColor == nil && sharedStyle.TTMLFontWeight == nil, "shared InlineStyle not polluted by export, got %+v", sharedStyle)
	})
}

// TestCueCascadeExport covers the round trips: TTML re-import checked span by
// span, WebVTT save/re-read, stability across repeated and post-edit exports,
// and preservation of timing, text, line breaks, word starts and regions.
func TestCueCascadeExport(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		input := "WEBVTT\n\n" +
			"REGION\nid:r1\nwidth:50%\n\n" +
			"STYLE\n" +
			"::cue { color: red; }\n" +
			"::cue(.b) { font-weight: bold; background-color: #001122; }\n\n" +
			"00:00:01.000 --> 00:00:03.000 region:r1\n" +
			"line one <c.b>bold</c>\n" +
			"second <00:00:02.000>word\n"
		s := readWebVTT(t, input)

		// Timing/region/word-start on the parsed object.
		c.Check(s.Items[0].StartAt == time.Second && s.Items[0].EndAt == 3*time.Second, "cue timing parsed")
		c.Check(s.Items[0].Region != nil && s.Items[0].Region.ID == "r1", "region linked to cue")
		var wordItem *astisub.LineItem
		for li := range s.Items[0].Lines[1].Items {
			if s.Items[0].Lines[1].Items[li].Text == "word" {
				wordItem = &s.Items[0].Lines[1].Items[li]
			}
		}
		c.Check(wordItem != nil && wordItem.StartAt == 2*time.Second, "inline word start timestamp parsed")

		cues, rawTTML := writeTTML(t, s)
		c.Check(len(cues) == 1, "one cue exported")
		c.Check(cues[0].attrs["region"] == "r1", "region attribute exported on <p>")
		c.Check(cues[0].attrs["color"] == "#ff0000", "root color on <p> got %q", cues[0].attrs["color"])
		c.Check(cues[0].attrs["backgroundColor"] == "#000000", "initial background exported explicitly got %q", cues[0].attrs["backgroundColor"])

		plainWant := map[string]string{
			"color": "#ff0000", "backgroundColor": "#000000",
			"fontWeight": "normal", "fontStyle": "normal", "textDecoration": "none",
		}
		plain, ok := spanByText(cues, "line one ")
		c.Check(ok, "plain span present")
		c.Check(mapsEqual(cascadeAttrs(plain.attrs), plainWant), "plain span attrs got %v want %v", cascadeAttrs(plain.attrs), plainWant)
		bold, ok := spanByText(cues, "bold")
		c.Check(ok, "bold span present")
		c.Check(bold.attrs["fontWeight"] == "bold", "bold weight got %q", bold.attrs["fontWeight"])
		c.Check(bold.attrs["backgroundColor"] == "#001122", "bold background got %q", bold.attrs["backgroundColor"])
		c.Check(bold.attrs["color"] == "#ff0000", "bold span inherits root color got %q", bold.attrs["color"])
		second, ok := spanByText(cues, "second ")
		c.Check(ok && second.attrs["color"] == "#ff0000", "second line span present and styled")
		word, ok := spanByText(cues, "word")
		c.Check(ok && word.attrs["color"] == "#ff0000", "word span present and styled")
		c.Check(len(cues[0].spans) == 4, "both lines exported: 4 text spans total, got %d", len(cues[0].spans))

		// Re-import the TTML and verify every span again, one by one.
		reread, err := astisub.ReadFromTTML(bytes.NewReader(rawTTML))
		c.Check(err == nil, "TTML re-import: %v", err)
		c.Check(len(reread.Items) == 1, "re-imported cue count")
		c.Check(reread.Items[0].StartAt == time.Second && reread.Items[0].EndAt == 3*time.Second, "re-imported timing preserved")
		c.Check(reread.Items[0].Region != nil && reread.Items[0].Region.ID == "r1", "re-imported region preserved")
		c.Check(reread.Items[0].Lines[0].String() == "line one bold" && reread.Items[0].Lines[1].String() == "second word", "re-imported text and line breaks preserved, got %q / %q", reread.Items[0].Lines[0].String(), reread.Items[0].Lines[1].String())
		reCues, _ := writeTTML(t, reread)
		for _, want := range []struct {
			text string
			want map[string]string
		}{
			{"line one ", plainWant},
			{"bold", map[string]string{"color": "#ff0000", "backgroundColor": "#001122", "fontWeight": "bold", "fontStyle": "normal", "textDecoration": "none"}},
			{"second ", plainWant},
			{"word", plainWant},
		} {
			got, found := spanByText(reCues, want.text)
			c.Check(found, "re-imported span %q present", want.text)
			c.Check(mapsEqual(cascadeAttrs(got.attrs), want.want), "re-imported span %q attrs got %v want %v", want.text, cascadeAttrs(got.attrs), want.want)
		}

		// WebVTT save + re-read must retain STYLE, tags and word starts,
		// and re-exporting produces the same cascade values.
		vttBuf := &bytes.Buffer{}
		c.Check(s.WriteToWebVTT(vttBuf) == nil, "WriteToWebVTT")
		c.Check(strings.Contains(vttBuf.String(), "::cue(.b)"), "STYLE block saved to WebVTT")
		c.Check(strings.Contains(vttBuf.String(), "<c.b>"), "WebVTT tags saved")
		c.Check(strings.Contains(vttBuf.String(), "<00:00:02.000>"), "inline timestamp saved")
		vttSubs, err := astisub.ReadFromWebVTT(bytes.NewReader(vttBuf.Bytes()))
		c.Check(err == nil, "WebVTT re-import: %v", err)
		var rereadWord *astisub.LineItem
		for li := range vttSubs.Items[0].Lines[1].Items {
			if vttSubs.Items[0].Lines[1].Items[li].Text == "word" {
				rereadWord = &vttSubs.Items[0].Lines[1].Items[li]
			}
		}
		c.Check(rereadWord != nil && rereadWord.StartAt == 2*time.Second, "word start survives WebVTT round trip")
		vttCues, _ := writeTTML(t, vttSubs)
		vttBold, _ := spanByText(vttCues, "bold")
		c.Check(vttBold.attrs["fontWeight"] == "bold" && vttBold.attrs["backgroundColor"] == "#001122", "WebVTT round trip keeps cascade result, got %v", vttBold.attrs)
		vttPlain, _ := spanByText(vttCues, "line one ")
		c.Check(mapsEqual(cascadeAttrs(vttPlain.attrs), plainWant), "WebVTT round trip plain span got %v want %v", cascadeAttrs(vttPlain.attrs), plainWant)

		// Change a rule after round trips and confirm the next export uses
		// the new rule; then confirm stability of consecutive exports.
		holder := webVTTStyleHolder(vttSubs)
		c.Check(holder != nil, "round-tripped document keeps STYLE holder")
		holder.InlineStyle.WebVTTStyles = []string{"::cue(.b) { color: #445566; font-weight: normal; }"}
		editedCues, editedRaw := writeTTML(t, vttSubs)
		editedBold, _ := spanByText(editedCues, "bold")
		c.Check(editedBold.attrs["color"] == "#445566", "post-roundtrip rule edit reflected got %q", editedBold.attrs["color"])
		c.Check(editedBold.attrs["fontWeight"] == "normal", "explicit normal exported after edit got %q", editedBold.attrs["fontWeight"])
		_, editedRaw2 := writeTTML(t, vttSubs)
		c.Check(bytes.Equal(editedRaw, editedRaw2), "repeated post-edit exports are identical")

		// The original input object stays untouched by every export above.
		c.Check(s.Items[0].Lines[0].Items[1].InlineStyle.TTMLFontWeight == nil, "input InlineStyle never rewritten")
		c.Check(strings.Contains(strings.Join(webVTTStyleHolder(s).InlineStyle.WebVTTStyles, ""), "#001122"), "input STYLE lines untouched")
	})
}
