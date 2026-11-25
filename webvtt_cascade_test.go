package astisub_test

import (
	"bytes"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/asticode/go-astisub"
	"github.com/asticode/go-astisub/task_support/taskcheck"
)

// These tests exercise the WebVTT STYLE -> cue cascade that feeds
// WriteToTTML. Inputs are hand-written and assertions target concrete colors,
// weights and related values observed after a TTML save/re-read, plus a final
// WebVTT save/re-read cycle.

// --- small helpers ---------------------------------------------------------

func cascadeReadVTT(c *taskcheck.C, src string) *astisub.Subtitles {
	s, err := astisub.ReadFromWebVTT(strings.NewReader(src))
	c.Check(err == nil, "ReadFromWebVTT returned error: %v", err)
	if err != nil {
		return nil
	}
	return s
}

func cascadeTTMLBytes(c *taskcheck.C, s *astisub.Subtitles) []byte {
	var buf bytes.Buffer
	err := s.WriteToTTML(&buf)
	c.Check(err == nil, "WriteToTTML returned error: %v", err)
	return buf.Bytes()
}

func cascadeRereadTTML(c *taskcheck.C, s *astisub.Subtitles) *astisub.Subtitles {
	b := cascadeTTMLBytes(c, s)
	r, err := astisub.ReadFromTTML(bytes.NewReader(b))
	c.Check(err == nil, "ReadFromTTML returned error: %v\nTTML:\n%s", err, string(b))
	if err != nil {
		return nil
	}
	return r
}

func cascadeFindSpan(s *astisub.Subtitles, text string) *astisub.LineItem {
	for _, item := range s.Items {
		for lineIdx := range item.Lines {
			line := &item.Lines[lineIdx]
			for itemIdx := range line.Items {
				if line.Items[itemIdx].Text == text {
					return &line.Items[itemIdx]
				}
			}
		}
	}
	return nil
}

func cascadeCheckColor(c *taskcheck.C, s *astisub.Subtitles, text, wantHex, label string) {
	sp := cascadeFindSpan(s, text)
	if sp == nil {
		c.Check(false, "%s: span %q not found", label, text)
		return
	}
	if sp.InlineStyle == nil || sp.InlineStyle.TTMLColor == nil {
		c.Check(false, "%s/%q: missing color, want %s", label, text, wantHex)
		return
	}
	got := sp.InlineStyle.TTMLColor.HTMLString()
	c.Check(strings.EqualFold(got, wantHex), "%s/%q: color=%s want %s", label, text, got, wantHex)
}

func cascadeCheckBackgroundColor(c *taskcheck.C, s *astisub.Subtitles, text, wantHex, label string) {
	sp := cascadeFindSpan(s, text)
	if sp == nil {
		c.Check(false, "%s: span %q not found", label, text)
		return
	}
	if sp.InlineStyle == nil || sp.InlineStyle.TTMLBackgroundColor == nil {
		c.Check(false, "%s/%q: missing background color, want %s", label, text, wantHex)
		return
	}
	got := sp.InlineStyle.TTMLBackgroundColor.HTMLString()
	c.Check(strings.EqualFold(got, wantHex), "%s/%q: background=%s want %s", label, text, got, wantHex)
}

func cascadeCheckAttr(c *taskcheck.C, s *astisub.Subtitles, text, want, label string, get func(*astisub.StyleAttributes) *string) {
	sp := cascadeFindSpan(s, text)
	if sp == nil {
		c.Check(false, "%s: span %q not found", label, text)
		return
	}
	if sp.InlineStyle == nil {
		c.Check(false, "%s/%q: nil style, want %q", label, text, want)
		return
	}
	p := get(sp.InlineStyle)
	if p == nil {
		c.Check(false, "%s/%q: attribute not exported, want %q", label, text, want)
		return
	}
	c.Check(*p == want, "%s/%q: value=%q want %q", label, text, *p, want)
}

func cascadeCheckWeight(c *taskcheck.C, s *astisub.Subtitles, text, want string, label ...string) {
	lab := "weight"
	if len(label) > 0 {
		lab = label[0]
	}
	cascadeCheckAttr(c, s, text, want, lab, func(sa *astisub.StyleAttributes) *string { return sa.TTMLFontWeight })
}
func cascadeCheckStyle(c *taskcheck.C, s *astisub.Subtitles, text, want string, label ...string) {
	lab := "style"
	if len(label) > 0 {
		lab = label[0]
	}
	cascadeCheckAttr(c, s, text, want, lab, func(sa *astisub.StyleAttributes) *string { return sa.TTMLFontStyle })
}
func cascadeCheckDecoration(c *taskcheck.C, s *astisub.Subtitles, text, want string, label ...string) {
	lab := "decoration"
	if len(label) > 0 {
		lab = label[0]
	}
	cascadeCheckAttr(c, s, text, want, lab, func(sa *astisub.StyleAttributes) *string { return sa.TTMLTextDecoration })
}

func cascadeStyleAttributes(s *astisub.Subtitles) *astisub.StyleAttributes {
	for _, st := range s.Styles {
		if st != nil && st.InlineStyle != nil && len(st.InlineStyle.WebVTTStyles) > 0 {
			return st.InlineStyle
		}
	}
	return nil
}

// cascadeExpectedSpan describes one ordered span in the export test.
type cascadeExpectedSpan struct {
	text string
	fg   string
	bg   string
	w    string
	fs   string
	dec  string
}

func cascadeCheckOrdered(c *taskcheck.C, r *astisub.Subtitles, want []cascadeExpectedSpan, label string) {
	if r == nil || len(r.Items) == 0 {
		c.Check(false, "%s: no items after re-read", label)
		return
	}
	var got []astisub.LineItem
	for _, ln := range r.Items[0].Lines {
		got = append(got, ln.Items...)
	}
	if len(got) != len(want) {
		texts := make([]string, len(got))
		for i, li := range got {
			texts[i] = li.Text
		}
		c.Check(false, "%s: got %d spans %v want %d", label, len(got), texts, len(want))
		return
	}
	c.Check(true, "%s: span count %d", label, len(want))
	for i, w := range want {
		li := got[i]
		c.Check(li.Text == w.text, "%s span %d: text=%q want %q", label, i, li.Text, w.text)
		if li.InlineStyle == nil {
			c.Check(false, "%s span %d (%q): nil style", label, i, w.text)
			continue
		}
		fg := ""
		if li.InlineStyle.TTMLColor != nil {
			fg = li.InlineStyle.TTMLColor.HTMLString()
		}
		bg := ""
		if li.InlineStyle.TTMLBackgroundColor != nil {
			bg = li.InlineStyle.TTMLBackgroundColor.HTMLString()
		}
		weight, style, dec := "<nil>", "<nil>", "<nil>"
		if li.InlineStyle.TTMLFontWeight != nil {
			weight = *li.InlineStyle.TTMLFontWeight
		}
		if li.InlineStyle.TTMLFontStyle != nil {
			style = *li.InlineStyle.TTMLFontStyle
		}
		if li.InlineStyle.TTMLTextDecoration != nil {
			dec = *li.InlineStyle.TTMLTextDecoration
		}
		c.Check(strings.EqualFold(fg, w.fg), "%s span %d (%q): fg=%s want %s", label, i, w.text, fg, w.fg)
		c.Check(strings.EqualFold(bg, w.bg), "%s span %d (%q): bg=%s want %s", label, i, w.text, bg, w.bg)
		c.Check(weight == w.w, "%s span %d (%q): weight=%s want %s", label, i, w.text, weight, w.w)
		c.Check(style == w.fs, "%s span %d (%q): style=%s want %s", label, i, w.text, style, w.fs)
		c.Check(dec == w.dec, "%s span %d (%q): decoration=%s want %s", label, i, w.text, dec, w.dec)
	}
}

// --- TestCueCascade: cascade semantics --------------------------------------

func TestCueCascade(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// 1. Initial values, built-in b/i/u defaults and nested inheritance.
		def := cascadeReadVTT(c, "WEBVTT\n\nSTYLE\n::cue(.enable) { color: #00ff00; }\n\n00:00:01.000 --> 00:00:02.000\nplain<b>bold</b><i>ital</i><u>under</u><b><i>deepbi</i></b>\n")
		if def == nil {
			return
		}
		rd := cascadeRereadTTML(c, def)
		if rd == nil {
			return
		}
		cascadeCheckColor(c, rd, "plain", "#ffffff", "initial fg")
		cascadeCheckBackgroundColor(c, rd, "plain", "#000000", "initial bg")
		cascadeCheckWeight(c, rd, "plain", "normal")
		cascadeCheckStyle(c, rd, "plain", "normal")
		cascadeCheckDecoration(c, rd, "plain", "none")
		cascadeCheckColor(c, rd, "bold", "#ffffff", "b fg stays initial")
		cascadeCheckWeight(c, rd, "bold", "bold")
		cascadeCheckStyle(c, rd, "bold", "normal")
		cascadeCheckWeight(c, rd, "ital", "normal")
		cascadeCheckStyle(c, rd, "ital", "italic")
		cascadeCheckDecoration(c, rd, "ital", "none")
		cascadeCheckDecoration(c, rd, "under", "underline")
		cascadeCheckWeight(c, rd, "deepbi", "bold")
		cascadeCheckStyle(c, rd, "deepbi", "italic")

		// 2. CSS explicitly switches b/i/u back off; explicit normal/none are
		//    still exported as concrete attributes.
		off := cascadeReadVTT(c, "WEBVTT\n\nSTYLE\n::cue(b) { font-weight: normal; }\n::cue(i) { font-style: normal; }\n::cue(u) { text-decoration: none; }\n\n00:00:01.000 --> 00:00:02.000\n<b>offb</b><i>offi</i><u>offu</u><b><i>offbi</i></b>\n")
		if off == nil {
			return
		}
		ro := cascadeRereadTTML(c, off)
		if ro == nil {
			return
		}
		cascadeCheckWeight(c, ro, "offb", "normal")
		cascadeCheckStyle(c, ro, "offi", "normal")
		cascadeCheckDecoration(c, ro, "offu", "none")
		cascadeCheckWeight(c, ro, "offbi", "normal")
		cascadeCheckStyle(c, ro, "offbi", "normal")

		// 3. Nested inheritance across several class/type levels.
		inh := cascadeReadVTT(c, "WEBVTT\n\nSTYLE\n::cue { color: #ff0000; font-weight: bold; }\n::cue(.it) { font-style: italic; }\n::cue(.bg) { background-color: #0000ff; }\n\n00:00:01.000 --> 00:00:02.000\nroottext<c.it>ItText<c.bg>BgText</c></c><b>BoldOnly</b>\n")
		if inh == nil {
			return
		}
		ri := cascadeRereadTTML(c, inh)
		if ri == nil {
			return
		}
		cascadeCheckColor(c, ri, "roottext", "#ff0000", "root rule")
		cascadeCheckWeight(c, ri, "roottext", "bold")
		cascadeCheckColor(c, ri, "ItText", "#ff0000", "inherit fg into .it")
		cascadeCheckWeight(c, ri, "ItText", "bold", "inherit weight into .it")
		cascadeCheckStyle(c, ri, "ItText", "italic")
		cascadeCheckColor(c, ri, "BgText", "#ff0000", "nested inherit through .bg")
		cascadeCheckStyle(c, ri, "BgText", "italic", "grandparent style inherits")
		cascadeCheckBackgroundColor(c, ri, "BgText", "#0000ff", ".bg own background")
		cascadeCheckWeight(c, ri, "BoldOnly", "bold")
		cascadeCheckColor(c, ri, "BoldOnly", "#ff0000", "b inherits root color")

		// 4. Same-node priority: specificity, order and important. Important
		//    is only compared within a node, so a root important color does not
		//    beat a child's ordinary declaration.
		prio := cascadeReadVTT(c, "WEBVTT\n\nSTYLE\n::cue(c.x) { color: #111111; }\n::cue(.x) { color: #222222; }\n::cue(.y) { font-weight: normal; }\n::cue(.y) { font-weight: bold; }\n::cue(.k) { color: #ff0000 !important; }\n::cue(.k.z) { color: #0000ff; }\n::cue { color: #ff0000 !important; }\n::cue(.blue) { color: #0000ff; }\n\n00:00:01.000 --> 00:00:02.000\nRootImp<c.x>SpecHi</c><c.y>OrderWin</c><c.k.z>ImpWin</c><c.blue>ChildBlue</c>\n")
		if prio == nil {
			return
		}
		rp := cascadeRereadTTML(c, prio)
		if rp == nil {
			return
		}
		cascadeCheckColor(c, rp, "RootImp", "#ff0000", "root important applies at root")
		cascadeCheckColor(c, rp, "SpecHi", "#111111", "type+class specificity beats class")
		cascadeCheckWeight(c, rp, "OrderWin", "bold")
		cascadeCheckColor(c, rp, "ImpWin", "#ff0000", "important beats higher specificity same node")
		cascadeCheckColor(c, rp, "ChildBlue", "#0000ff", "root important does not reach child direct decl")

		// 5. Comma grouping, comments with trap characters, and recovery from
		//    invalid declarations / unknown selectors.
		rec := cascadeReadVTT(c, "WEBVTT\n\nSTYLE\n/* leading ; } { ::cue(.fake) { color: #000000; } trap */\n::cue(.g1), ::cue(.g2) { color: #abcdef; font-weight: bold; }\n::cue(.cm) { color: #123123; /* mid ; } trap */ font-style: italic; }\n::cue(.bad) { color: notacolor; font-weight: 900; font-style: oblique; text-decoration: blink; background-color: #12; unknown-prop: 1; color: #010203; }\n::cue(.ok) { color: #040506; }\n::cue(.zz):hover { color: #000000; }\ndiv { color: #111111; }\n::cue(.survive) { color: #0a0a0a; }\n\n00:00:01.000 --> 00:00:02.000\n<c.g1>G1</c><c.g2>G2</c><c.cm>Cm</c><c.bad>Bad</c><c.ok>Ok</c><c.survive>Surv</c>\n")
		if rec == nil {
			return
		}
		rr := cascadeRereadTTML(c, rec)
		if rr == nil {
			return
		}
		cascadeCheckColor(c, rr, "G1", "#abcdef", "group selector first")
		cascadeCheckColor(c, rr, "G2", "#abcdef", "group selector second")
		cascadeCheckWeight(c, rr, "G2", "bold")
		cascadeCheckColor(c, rr, "Cm", "#123123", "comment does not hide color")
		cascadeCheckStyle(c, rr, "Cm", "italic")
		cascadeCheckColor(c, rr, "Bad", "#010203", "recover to next valid declaration")
		cascadeCheckWeight(c, rr, "Bad", "normal")
		cascadeCheckStyle(c, rr, "Bad", "normal")
		cascadeCheckDecoration(c, rr, "Bad", "none")
		cascadeCheckBackgroundColor(c, rr, "Bad", "#000000", "invalid hex ignored -> initial")
		cascadeCheckColor(c, rr, "Ok", "#040506", "rule after bad declaration intact")
		cascadeCheckColor(c, rr, "Surv", "#0a0a0a", "rule after unknown selectors intact")

		// 6. inherit/initial keywords resolve concretely.
		kw := cascadeReadVTT(c, "WEBVTT\n\nSTYLE\n::cue { color: #102030; font-style: italic; }\n::cue(.inh) { color: inherit; font-style: inherit; }\n::cue(.ini) { color: initial; font-weight: bold; font-weight: initial; }\n\n00:00:01.000 --> 00:00:02.000\n<c.inh>Inh</c><c.ini>Ini</c>\n")
		if kw == nil {
			return
		}
		rk := cascadeRereadTTML(c, kw)
		if rk == nil {
			return
		}
		cascadeCheckColor(c, rk, "Inh", "#102030", "inherit takes parent")
		cascadeCheckStyle(c, rk, "Inh", "italic", "inherit style takes parent")
		cascadeCheckColor(c, rk, "Ini", "#ffffff", "initial color")
		cascadeCheckWeight(c, rk, "Ini", "normal", "initial keyword resets bold")
	})
}

// --- TestCueCascadeEdits: live edits and pointer isolation ------------------

func TestCueCascadeEdits(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const src = "WEBVTT\n\nSTYLE\n::cue(.brand) { color: #123456; font-weight: bold; }\n\n00:00:01.000 --> 00:00:03.000\n<c.brand>A</c><c.red>B</c>\n"
		s := cascadeReadVTT(c, src)
		if s == nil || len(s.Items) != 1 || len(s.Items[0].Lines) != 1 {
			c.Check(false, "unexpected parsed structure")
			return
		}
		if len(s.Items[0].Lines[0].Items) != 2 {
			c.Check(false, "expected 2 runs, got %d", len(s.Items[0].Lines[0].Items))
			return
		}

		// Initial export reflects CSS class and built-in red.
		r0 := cascadeRereadTTML(c, s)
		if r0 == nil {
			return
		}
		cascadeCheckColor(c, r0, "A", "#123456", "initial css")
		cascadeCheckWeight(c, r0, "A", "bold")
		cascadeCheckColor(c, r0, "B", "#ff0000", "builtin red")

		// Consecutive exports are identical and do not mutate the input.
		b1 := cascadeTTMLBytes(c, s)
		b2 := cascadeTTMLBytes(c, s)
		c.Check(bytes.Equal(b1, b2), "consecutive WriteToTTML outputs differ")
		itemA := &s.Items[0].Lines[0].Items[0]
		itemB := &s.Items[0].Lines[0].Items[1]
		saA := itemA.InlineStyle
		saB := itemB.InlineStyle
		c.Check(saA != nil && saB != nil, "run styles missing")
		if saA == nil || saB == nil {
			return
		}
		oldAColor := saA.TTMLColor
		oldAWeight := saA.TTMLFontWeight
		oldBColor := saB.TTMLColor
		tagsBefore := append([]astisub.WebVTTTag(nil), saA.WebVTTTags...)
		_ = cascadeTTMLBytes(c, s)
		c.Check(saA.TTMLColor == oldAColor, "export mutated A TTMLColor pointer")
		c.Check(saA.TTMLFontWeight == oldAWeight, "export set A TTMLFontWeight")
		c.Check(saB.TTMLColor == oldBColor, "export mutated B TTMLColor pointer")
		c.Check(reflect.DeepEqual(saA.WebVTTTags, tagsBefore), "export mutated A WebVTTTags")
		c.Check(oldBColor == astisub.ColorRed, "B parse-derived red was not the shared red color")

		// Edit the STYLE content: new color, italic, bold dropped.
		sa := cascadeStyleAttributes(s)
		c.Check(sa != nil, "could not locate STYLE-bearing StyleAttributes")
		if sa == nil {
			return
		}
		originalStyles := append([]string(nil), sa.WebVTTStyles...)
		sa.WebVTTStyles = []string{"::cue(.brand) { color: #abcdef; font-style: italic; }"}
		r1 := cascadeRereadTTML(c, s)
		cascadeCheckColor(c, r1, "A", "#abcdef", "after style edit color")
		cascadeCheckStyle(c, r1, "A", "italic", "after style edit style")
		cascadeCheckWeight(c, r1, "A", "normal", "dropped bold recomputed to normal")
		cascadeCheckColor(c, r1, "B", "#ff0000", "B unaffected by style edit")

		// Edit the rule a second time.
		sa.WebVTTStyles = []string{"::cue(.brand) { color: #0b0b0b; font-weight: normal; }"}
		r2 := cascadeRereadTTML(c, s)
		cascadeCheckColor(c, r2, "A", "#0b0b0b", "second rule edit color")
		cascadeCheckWeight(c, r2, "A", "normal", "second rule edit weight")
		cascadeCheckStyle(c, r2, "A", "normal", "removed italic recomputed to normal")

		// Reverting the STYLE yields byte-identical TTML, proving no state drift.
		sa.WebVTTStyles = originalStyles
		b3 := cascadeTTMLBytes(c, s)
		c.Check(bytes.Equal(b1, b3), "reverted STYLE export is not identical to the first export")

		// Edit a LineItem's current WebVTTTags: point A's c tag at built-in red.
		c.Check(len(saA.WebVTTTags) == 1, "A expected one tag, got %d", len(saA.WebVTTTags))
		if len(saA.WebVTTTags) == 1 {
			saA.WebVTTTags[0].Classes = []string{"red"}
			r3 := cascadeRereadTTML(c, s)
			cascadeCheckColor(c, r3, "A", "#ff0000", "after WebVTTTags class edit")
			cascadeCheckWeight(c, r3, "A", "normal", "class edit loses bold rule")
		}

		// Shared InlineStyle pointer: inline timestamps make two runs share one
		// StyleAttributes. Export must not write computed state into it.
		sharedSrc := "WEBVTT\n\nSTYLE\n::cue(b) { color: #123456; }\n\n00:00:01.000 --> 00:00:03.000\n<b>Hi<00:00:01.500>There</b>\n"
		sh := cascadeReadVTT(c, sharedSrc)
		if sh == nil {
			return
		}
		shRuns := sh.Items[0].Lines[0].Items
		c.Check(len(shRuns) == 2, "timestamp split expected 2 runs, got %d", len(shRuns))
		if len(shRuns) == 2 {
			c.Check(shRuns[0].InlineStyle == shRuns[1].InlineStyle, "timestamp runs do not share InlineStyle as designed")
			sharedSA := shRuns[0].InlineStyle
			c.Check(sharedSA != nil && sharedSA.TTMLFontWeight == nil, "precondition: shared weight should be unset")
			rs := cascadeRereadTTML(c, sh)
			cascadeCheckColor(c, rs, "Hi", "#123456", "shared run Hi")
			cascadeCheckColor(c, rs, "There", "#123456", "shared run There")
			cascadeCheckWeight(c, rs, "There", "bold", "shared run builtin bold")
			c.Check(sharedSA != nil && sharedSA.TTMLFontWeight == nil, "export polluted shared InlineStyle weight")
			c.Check(sharedSA != nil && sharedSA.TTMLColor == nil, "export wrote computed color into shared InlineStyle")
			c.Check(shRuns[1].StartAt == 1500*time.Millisecond, "word start changed: %v", shRuns[1].StartAt)
		}

		// Explicit two runs referencing the exact same StyleAttributes pointer.
		shared := &astisub.StyleAttributes{WebVTTTags: []astisub.WebVTTTag{{Name: "i"}}}
		manual := astisub.NewSubtitles()
		manual.Styles["cue-style"] = &astisub.Style{ID: "cue-style", InlineStyle: &astisub.StyleAttributes{
			WebVTTStyles: []string{"::cue(i) { color: #abcdef; }"},
		}}
		manual.Items = []*astisub.Item{{
			StartAt: time.Second, EndAt: 2 * time.Second,
			Lines: []astisub.Line{{Items: []astisub.LineItem{
				{Text: "P", InlineStyle: shared},
				{Text: "Q", InlineStyle: shared},
			}}},
		}}
		rm := cascadeRereadTTML(c, manual)
		cascadeCheckColor(c, rm, "P", "#abcdef", "shared-pointer P")
		cascadeCheckColor(c, rm, "Q", "#abcdef", "shared-pointer Q")
		cascadeCheckStyle(c, rm, "P", "italic")
		c.Check(shared.TTMLFontStyle == nil, "export mutated manually shared StyleAttributes")
	})
}

// --- TestCueCascadeExport: span-by-span TTML + WebVTT cycle -----------------

func TestCueCascadeExport(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const src = "WEBVTT\n\nRegion: id=r1 width=40% lines=3\n\nSTYLE\n::cue(.a) { color: #ff0000; font-weight: bold; }\n::cue(.b) { background-color: #00ff00; font-style: italic; }\n::cue(.u) { text-decoration: underline; }\n\n00:00:01.000 --> 00:00:03.000 region:r1 align:center\n<c.a>Alpha</c><c.b>Beta</c><b>Gamma</b><i>Delta</i><u>Epsilon</u><c.a><i>Zeta</i></c>\n\n00:00:04.000 --> 00:00:05.000\nLineOne\nLineTwo\n"
		s := cascadeReadVTT(c, src)
		if s == nil || len(s.Items) != 2 {
			c.Check(false, "expected 2 items, got %d", len(s.Items))
			return
		}
		c.Check(s.Items[0].StartAt == time.Second, "start changed: %v", s.Items[0].StartAt)
		c.Check(s.Items[0].EndAt == 3*time.Second, "end changed: %v", s.Items[0].EndAt)
		c.Check(s.Items[0].Region != nil && s.Items[0].Region.ID == "r1", "region not parsed")
		c.Check(s.Items[1].Lines[0].String() == "LineOne" && s.Items[1].Lines[1].String() == "LineTwo", "newline text not parsed")

		want := []cascadeExpectedSpan{
			{text: "Alpha", fg: "#ff0000", bg: "#000000", w: "bold", fs: "normal", dec: "none"},
			{text: "Beta", fg: "#ffffff", bg: "#00ff00", w: "normal", fs: "italic", dec: "none"},
			{text: "Gamma", fg: "#ffffff", bg: "#000000", w: "bold", fs: "normal", dec: "none"},
			{text: "Delta", fg: "#ffffff", bg: "#000000", w: "normal", fs: "italic", dec: "none"},
			{text: "Epsilon", fg: "#ffffff", bg: "#000000", w: "normal", fs: "normal", dec: "underline"},
			{text: "Zeta", fg: "#ff0000", bg: "#000000", w: "bold", fs: "italic", dec: "none"},
		}

		// TTML export text keeps region/layout.
		ttmlBytes := cascadeTTMLBytes(c, s)
		ttmlText := string(ttmlBytes)
		c.Check(strings.Contains(ttmlText, `region="r1"`), "TTML missing region reference")
		c.Check(strings.Contains(ttmlText, `tts:textAlign="center"`), "TTML missing cue alignment")

		// Re-read TTML and verify every span in order.
		rt, err := astisub.ReadFromTTML(bytes.NewReader(ttmlBytes))
		c.Check(err == nil, "ReadFromTTML direct error: %v", err)
		if rt == nil {
			return
		}
		c.Check(rt.Items[0].StartAt == time.Second && rt.Items[0].EndAt == 3*time.Second, "TTML timing changed")
		cascadeCheckOrdered(c, rt, want, "ttml-reread")
		c.Check(len(rt.Items) == 2 && len(rt.Items[1].Lines) == 2, "TTML newline structure changed")
		if len(rt.Items) == 2 && len(rt.Items[1].Lines) == 2 {
			c.Check(rt.Items[1].Lines[0].String() == "LineOne", "TTML line one text changed")
			c.Check(rt.Items[1].Lines[1].String() == "LineTwo", "TTML line two text changed")
		}

		// WebVTT save -> re-read -> TTML export -> re-read, same span values.
		var vtt bytes.Buffer
		c.Check(s.WriteToWebVTT(&vtt) == nil, "WriteToWebVTT error")
		c.Check(strings.Contains(vtt.String(), "::cue(.a)"), "edited css rule missing from WebVTT save")
		sv, err := astisub.ReadFromWebVTT(bytes.NewReader(vtt.Bytes()))
		c.Check(err == nil, "WebVTT re-read error: %v\nVTT:\n%s", err, vtt.String())
		if sv == nil {
			return
		}
		rv := cascadeRereadTTML(c, sv)
		cascadeCheckOrdered(c, rv, want, "vtt-cycle")
		c.Check(len(sv.Items[0].Lines[0].Items) == 6, "vtt cycle span count changed: %d", len(sv.Items[0].Lines[0].Items))
	})
}
