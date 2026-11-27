package astisub_test

import (
	"bytes"
	"strings"
	"testing"
	"time"

	"github.com/asticode/go-astisub"
	"github.com/asticode/go-astisub/task_support/taskcheck"
)

// rubyParagraph is the worked example from the task: two pairs whose bases and
// readings both carry their own colors.
const rubyParagraph = `<p begin="00:00:01.000" end="00:00:03.000">
  <span tts:ruby="container">
    <span tts:ruby="base"><span tts:color="#ff0000">東</span><span tts:color="#0000ff">京</span></span>
    <span tts:ruby="text">とうきょう</span>
  </span>
  <span tts:ruby="container">
    <span tts:ruby="base">駅</span>
    <span tts:ruby="text">えき</span>
  </span>
</p>`

func rubyTTML(paragraphs ...string) string {
	return `<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling">
  <body><div>` + strings.Join(paragraphs, "\n") + `</div></body>
</tt>`
}

func mustReadWebVTT(t *testing.T, s string) *astisub.Subtitles {
	t.Helper()
	o, err := astisub.ReadFromWebVTT(strings.NewReader(s))
	if err != nil {
		t.Fatalf("reading WebVTT: %v", err)
	}
	return o
}

func mustReadTTML(t *testing.T, s string) *astisub.Subtitles {
	t.Helper()
	o, err := astisub.ReadFromTTML(strings.NewReader(s))
	if err != nil {
		t.Fatalf("reading TTML: %v", err)
	}
	return o
}

func writeWebVTT(t *testing.T, s *astisub.Subtitles) string {
	t.Helper()
	var b bytes.Buffer
	if err := s.WriteToWebVTT(&b); err != nil {
		t.Fatalf("writing WebVTT: %v", err)
	}
	return b.String()
}

func writeTTML(t *testing.T, s *astisub.Subtitles) string {
	t.Helper()
	var b bytes.Buffer
	if err := s.WriteToTTML(&b); err != nil {
		t.Fatalf("writing TTML: %v", err)
	}
	return b.String()
}

// rubyColor returns the TTML color of a run as a hex string, or "".
func rubyColor(li astisub.LineItem) string {
	if li.InlineStyle == nil || li.InlineStyle.TTMLColor == nil {
		return ""
	}
	return li.InlineStyle.TTMLColor.HTMLString()
}

// rubyReadings joins the annotation runs of a base run.
func rubyReadings(li astisub.LineItem) string {
	if li.Ruby == nil {
		return ""
	}
	var sb strings.Builder
	for _, r := range li.Ruby.Text {
		sb.WriteString(r.Text)
	}
	return sb.String()
}

// rubyReadingsColored joins annotation text and colors, e.g. "とう#008000".
func rubyReadingsColored(li astisub.LineItem) string {
	if li.Ruby == nil {
		return ""
	}
	var parts []string
	for _, r := range li.Ruby.Text {
		parts = append(parts, r.Text+rubyColor(r))
	}
	return strings.Join(parts, "|")
}

func TestRubyWebVTT(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		source := `WEBVTT

1
00:00:01.000 --> 00:00:03.000
<ruby><c.red>東</c><c.blue>京</c><rt>とうきょう</rt>駅<rt>えき</rt></ruby>

2
00:00:04.000 --> 00:00:06.000
前<ruby>東<rt>とう</rt></ruby><ruby>京<rt>きょう</rt></ruby>後

3
00:00:07.000 --> 00:00:09.000
<ruby><c.green>東</c>京<rt><c.yellow>とう</c>きょう</rt></ruby>

4
00:00:10.000 --> 00:00:12.000
<c.red>普通</c>文字
`
		s := mustReadWebVTT(t, source)
		c.Check(len(s.Items) == 4, "expected 4 cues, got %d", len(s.Items))

		// Same ruby, two basse runs and two readings: both colored runs belong
		// to the first pair, and must not become two bases.
		line := s.Items[0].Lines[0]
		c.Check(line.String() == "東京駅", "plain text should be 東京駅, got %q", line.String())
		c.Check(len(line.Items) == 3, "first cue should expose 3 base runs, got %d", len(line.Items))
		c.Check(line.Items[0].Text == "東", "run 0 text = %q", line.Items[0].Text)
		c.Check(line.Items[1].Text == "京", "run 1 text = %q", line.Items[1].Text)
		c.Check(line.Items[2].Text == "駅", "run 2 text = %q", line.Items[2].Text)
		c.Check(line.Items[0].Ruby != nil && line.Items[0].Ruby == line.Items[1].Ruby,
			"東 and 京 must share one ruby pair")
		c.Check(line.Items[2].Ruby != nil && line.Items[2].Ruby != line.Items[0].Ruby,
			"駅 must have its own ruby pair")
		c.Check(rubyReadings(line.Items[0]) == "とうきょう", "first reading = %q", rubyReadings(line.Items[0]))
		c.Check(rubyReadings(line.Items[2]) == "えき", "second reading = %q", rubyReadings(line.Items[2]))
		c.Check(rubyColor(line.Items[0]) == "#ff0000", "東 color = %q", rubyColor(line.Items[0]))
		c.Check(rubyColor(line.Items[1]) == "#0000ff", "京 color = %q", rubyColor(line.Items[1]))

		// Adjacent rubies and plain text before/after keep their order.
		line = s.Items[1].Lines[0]
		c.Check(line.String() == "前東京後", "cue 2 plain text = %q", line.String())
		c.Check(len(line.Items) == 4, "cue 2 should expose 4 runs, got %d", len(line.Items))
		c.Check(line.Items[0].Text == "前" && line.Items[0].Ruby == nil, "leading plain text lost")
		c.Check(line.Items[3].Text == "後" && line.Items[3].Ruby == nil, "trailing plain text lost")
		c.Check(rubyReadings(line.Items[1]) == "とう" && rubyReadings(line.Items[2]) == "きょう",
			"adjacent readings = %q / %q", rubyReadings(line.Items[1]), rubyReadings(line.Items[2]))

		// Colored reading runs stay separate and keep their own color.
		line = s.Items[2].Lines[0]
		c.Check(line.String() == "東京", "cue 3 plain text = %q", line.String())
		c.Check(rubyColor(line.Items[0]) == "#008000", "cue 3 東 color = %q", rubyColor(line.Items[0]))
		c.Check(rubyReadingsColored(line.Items[0]) == "とう#ffff00|きょう",
			"cue 3 reading runs = %q", rubyReadingsColored(line.Items[0]))

		// Plain color runs without ruby are untouched.
		c.Check(s.Items[3].Lines[0].String() == "普通文字", "cue 4 text = %q", s.Items[3].Lines[0].String())
		c.Check(rubyColor(s.Items[3].Lines[0].Items[0]) == "#ff0000", "cue 4 普通 color = %q", rubyColor(s.Items[3].Lines[0].Items[0]))

		// Line.String and SRT use base text only.
		var srt bytes.Buffer
		if err := s.WriteToSRT(&srt); err != nil {
			t.Fatalf("writing SRT: %v", err)
		}
		c.Check(strings.Contains(srt.String(), "東京駅"), "SRT missing base text: %s", srt.String())
		c.Check(!strings.Contains(srt.String(), "とうきょう"), "SRT leaked readings: %s", srt.String())

		// WebVTT -> WebVTT keeps pairs separated and plain text ordered.
		out := writeWebVTT(t, s)
		c.Check(strings.Contains(out, "<ruby><c.red>東</c><c.blue>京</c><rt>とうきょう</rt></ruby>"),
			"first ruby was not written as one pair: %s", out)
		c.Check(strings.Contains(out, "<ruby>駅<rt>えき</rt></ruby>"), "second ruby missing: %s", out)
		c.Check(strings.Contains(out, "前<ruby>東<rt>とう</rt></ruby><ruby>京<rt>きょう</rt></ruby>後"),
			"adjacent rubies lost: %s", out)

		back := mustReadWebVTT(t, out)
		c.Check(len(back.Items) == 4, "roundtrip cue count = %d", len(back.Items))
		c.Check(back.Items[0].Lines[0].String() == "東京駅", "roundtrip cue 1 text = %q", back.Items[0].Lines[0].String())
		c.Check(rubyReadings(back.Items[0].Lines[0].Items[1]) == "とうきょう",
			"roundtrip reading = %q", rubyReadings(back.Items[0].Lines[0].Items[1]))
		c.Check(rubyColor(back.Items[0].Lines[0].Items[1]) == "#0000ff",
			"roundtrip 京 color = %q", rubyColor(back.Items[0].Lines[0].Items[1]))
		c.Check(rubyReadingsColored(back.Items[2].Lines[0].Items[0]) == "とう#ffff00|きょう",
			"roundtrip reading runs = %q", rubyReadingsColored(back.Items[2].Lines[0].Items[0]))
	})
}

func TestRubyTTML(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		simple := rubyTTML(
			rubyParagraph,
			`<p begin="00:00:04.000" end="00:00:06.000">前<span tts:ruby="container"><span tts:ruby="base">東</span><span tts:ruby="text">とう</span></span><span tts:ruby="container"><span tts:ruby="base">京</span><span tts:ruby="text">きょう</span></span>後</p>`,
		)
		s := mustReadTTML(t, simple)
		c.Check(len(s.Items) == 2, "expected 2 items, got %d", len(s.Items))

		// Simple structure: container > base + text.
		line := s.Items[0].Lines[0]
		c.Check(line.String() == "東京駅", "simple TTML plain text = %q", line.String())
		c.Check(len(line.Items) == 3, "simple TTML base runs = %d", len(line.Items))
		c.Check(line.Items[0].Ruby != nil && line.Items[0].Ruby == line.Items[1].Ruby,
			"simple TTML pairs must be shared")
		c.Check(rubyReadings(line.Items[0]) == "とうきょう", "simple TTML reading = %q", rubyReadings(line.Items[0]))
		c.Check(rubyColor(line.Items[0]) == "#ff0000" && rubyColor(line.Items[1]) == "#0000ff",
			"simple TTML base colors = %q / %q", rubyColor(line.Items[0]), rubyColor(line.Items[1]))

		// Adjacent containers and surrounding plain text keep order.
		line = s.Items[1].Lines[0]
		c.Check(line.String() == "前東京後", "adjacent TTML text = %q", line.String())
		c.Check(len(line.Items) == 4, "adjacent TTML runs = %d", len(line.Items))
		c.Check(line.Items[0].Text == "前" && line.Items[3].Text == "後", "surrounding text lost")

		// Grouped structure: baseContainer and textContainer pair by position.
		grouped := rubyTTML(`<p begin="00:00:01.000" end="00:00:03.000">
  <span tts:ruby="container">
    <span tts:ruby="baseContainer">
      <span tts:ruby="base" tts:color="#ff0000">東</span>
      <span tts:ruby="base"><span tts:color="#00ff00">京</span></span>
      <span tts:ruby="base">駅</span>
    </span>
    <span tts:ruby="textContainer">
      <span tts:ruby="text">とう</span>
      <span tts:ruby="text">きょう</span>
      <span tts:ruby="text">えき</span>
    </span>
  </span>
</p>`)
		g := mustReadTTML(t, grouped)
		line = g.Items[0].Lines[0]
		c.Check(line.String() == "東京駅", "grouped TTML plain text = %q", line.String())
		c.Check(len(line.Items) == 3, "grouped TTML base runs = %d", len(line.Items))
		for i, want := range []string{"とう", "きょう", "えき"} {
			c.Check(rubyReadings(line.Items[i]) == want, "grouped pair %d reading = %q", i, rubyReadings(line.Items[i]))
		}
		c.Check(line.Items[0].Ruby != line.Items[1].Ruby, "grouped pairs must stay separate")
		c.Check(rubyColor(line.Items[0]) == "#ff0000", "grouped 東 color = %q", rubyColor(line.Items[0]))
		c.Check(rubyColor(line.Items[1]) == "#00ff00", "grouped 京 color = %q", rubyColor(line.Items[1]))

		// Inline color inheritance: outer blue, inner yellow overrides.
		inherited := rubyTTML(`<p begin="00:00:01.000" end="00:00:03.000">
  <span tts:ruby="container" tts:color="#0000ff">
    <span tts:ruby="base">東<span tts:color="#ffff00">京</span></span>
    <span tts:ruby="text">とうきょう</span>
  </span>
</p>`)
		inh := mustReadTTML(t, inherited)
		line = inh.Items[0].Lines[0]
		c.Check(len(line.Items) == 2, "inherited base runs = %d", len(line.Items))
		c.Check(rubyColor(line.Items[0]) == "#0000ff", "inherited 東 color = %q", rubyColor(line.Items[0]))
		c.Check(rubyColor(line.Items[1]) == "#ffff00", "inherited 京 color = %q", rubyColor(line.Items[1]))
		c.Check(rubyColor(line.Items[0].Ruby.Text[0]) == "#0000ff", "inherited reading color = %q", rubyColor(line.Items[0].Ruby.Text[0]))

		// Layout indentation is not content; xml:space="preserve" keeps spaces;
		// a br between groups stays a line break.
		layout := rubyTTML(`<p begin="00:00:01.000" end="00:00:03.000">
  <span tts:ruby="container">
    <span tts:ruby="base">東</span>
    <span tts:ruby="text">とう</span>
  </span>
  <br/>
  <span xml:space="preserve"> そのまま </span>
</p>`)
		l := mustReadTTML(t, layout)
		c.Check(len(l.Items[0].Lines) == 2, "br should preserve 2 lines, got %d", len(l.Items[0].Lines))
		c.Check(l.Items[0].Lines[0].String() == "東", "first line = %q", l.Items[0].Lines[0].String())
		c.Check(l.Items[0].Lines[1].String() == " そのまま ", "preserved line = %q", l.Items[0].Lines[1].String())

		// Writing normalizes to simple containers that can be read back with the
		// same pairs, and each container holds one base and one text.
		out := writeTTML(t, g)
		c.Check(strings.Contains(out, `tts:ruby="container"`), "output lost ruby containers: %s", out)
		c.Check(!strings.Contains(out, `tts:ruby="baseContainer"`), "output kept grouped structure: %s", out)
		reread := mustReadTTML(t, out)
		c.Check(reread.Items[0].Lines[0].String() == "東京駅", "reread text = %q", reread.Items[0].Lines[0].String())
		for i, want := range []string{"とう", "きょう", "えき"} {
			c.Check(rubyReadings(reread.Items[0].Lines[0].Items[i]) == want,
				"reread pair %d reading = %q", i, rubyReadings(reread.Items[0].Lines[0].Items[i]))
		}

		// TTML -> WebVTT keeps the pairs separated.
		vtt := writeWebVTT(t, g)
		c.Check(strings.Contains(vtt, "<ruby><c.red>東</c><rt>とう</rt></ruby>"), "TTML->VTT first pair: %s", vtt)
		c.Check(strings.Contains(vtt, "<ruby><c.lime>京</c><rt>きょう</rt></ruby>"), "TTML->VTT second pair: %s", vtt)
		c.Check(strings.Contains(vtt, "<ruby>駅<rt>えき</rt></ruby>"), "TTML->VTT third pair: %s", vtt)
	})
}

func TestRubyEdit(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Edit the first pair's reading and base color, then go
		// TTML -> WebVTT -> TTML and check the edits and pairing survive.
		s := mustReadTTML(t, rubyTTML(rubyParagraph))
		line := &s.Items[0].Lines[0]
		c.Check(len(line.Items) == 3, "expected 3 base runs, got %d", len(line.Items))
		c.Check(line.Items[0].Ruby == line.Items[1].Ruby, "initial pairing broken")

		line.Items[0].Ruby.Text = []astisub.LineItem{{Text: "Tokyo"}}
		line.Items[0].InlineStyle = &astisub.StyleAttributes{TTMLColor: astisub.ColorGreen}
		line.Items[1].InlineStyle = nil

		vtt := writeWebVTT(t, s)
		c.Check(strings.Contains(vtt, "<ruby><c.green>東</c>京<rt>Tokyo</rt></ruby>"),
			"edit was not exported to WebVTT: %s", vtt)

		viaVTT := mustReadWebVTT(t, vtt)
		ttml := writeTTML(t, viaVTT)
		final := mustReadTTML(t, ttml)

		f := final.Items[0].Lines[0]
		c.Check(f.String() == "東京駅", "final plain text = %q", f.String())
		c.Check(len(f.Items) == 3, "final base runs = %d", len(f.Items))
		c.Check(f.Items[0].Ruby != nil, "first run lost its reading")
		c.Check(rubyReadings(f.Items[0]) == "Tokyo", "edited reading = %q", rubyReadings(f.Items[0]))
		c.Check(rubyColor(f.Items[0]) == "#008000", "edited 東 color = %q", rubyColor(f.Items[0]))
		c.Check(rubyColor(f.Items[1]) == "", "京 should have no color, got %q", rubyColor(f.Items[1]))
		c.Check(f.Items[0].Ruby == f.Items[1].Ruby, "edited pair no longer shared")
		c.Check(rubyReadings(f.Items[2]) == "えき", "second pair reading = %q", rubyReadings(f.Items[2]))
		c.Check(rubyColor(f.Items[2]) == "", "駅 should have no color, got %q", rubyColor(f.Items[2]))

		// Build the same pairs directly from the public model, with no parsing,
		// and write both formats.
		r1 := &astisub.Ruby{Text: []astisub.LineItem{{Text: "Tokyo"}}}
		r2 := &astisub.Ruby{Text: []astisub.LineItem{{Text: "えき"}}}
		built := astisub.NewSubtitles()
		built.Items = []*astisub.Item{{
			StartAt: time.Second,
			EndAt:   3 * time.Second,
			Lines: []astisub.Line{{Items: []astisub.LineItem{
				{Text: "東", Ruby: r1, InlineStyle: &astisub.StyleAttributes{TTMLColor: astisub.ColorGreen}},
				{Text: "京", Ruby: r1},
				{Text: "駅", Ruby: r2},
			}}},
		}}

		builtTTML := writeTTML(t, built)
		fromBuilt := mustReadTTML(t, builtTTML)
		b := fromBuilt.Items[0].Lines[0]
		c.Check(b.String() == "東京駅", "built TTML text = %q", b.String())
		c.Check(len(b.Items) == 3, "built TTML runs = %d", len(b.Items))
		c.Check(rubyReadings(b.Items[0]) == "Tokyo" && rubyColor(b.Items[0]) == "#008000",
			"built TTML first pair = %q / %q", rubyReadings(b.Items[0]), rubyColor(b.Items[0]))
		c.Check(b.Items[0].Ruby == b.Items[1].Ruby, "built TTML pair not shared")
		c.Check(rubyReadings(b.Items[2]) == "えき", "built TTML second reading = %q", rubyReadings(b.Items[2]))

		builtVTT := writeWebVTT(t, built)
		c.Check(strings.Contains(builtVTT, "<ruby><c.green>東</c>京<rt>Tokyo</rt></ruby>"),
			"built WebVTT pair = %s", builtVTT)
		fromVTT := mustReadWebVTT(t, builtVTT)
		v := fromVTT.Items[0].Lines[0]
		c.Check(v.String() == "東京駅", "built WebVTT text = %q", v.String())
		c.Check(rubyReadings(v.Items[0]) == "Tokyo", "built WebVTT reading = %q", rubyReadings(v.Items[0]))
		c.Check(rubyReadings(v.Items[2]) == "えき", "built WebVTT second reading = %q", rubyReadings(v.Items[2]))

		// SRT exports only the base text of built pairs.
		var srt bytes.Buffer
		if err := built.WriteToSRT(&srt); err != nil {
			t.Fatalf("writing SRT: %v", err)
		}
		c.Check(strings.Contains(srt.String(), "東京駅"), "built SRT text: %s", srt.String())
		c.Check(!strings.Contains(srt.String(), "Tokyo"), "built SRT leaked reading: %s", srt.String())
	})
}
