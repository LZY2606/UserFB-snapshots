package astisub_test

import (
	"bytes"
	"strings"
	"testing"
	"time"

	"github.com/asticode/go-astisub"
	"github.com/asticode/go-astisub/task_support/taskcheck"
)

// rubyRun is a compact expectation for one styled run of a ruby side.
type rubyRun struct {
	text  string
	color *astisub.Color
}

// assertRuby checks that li carries exactly the given pairs, run by run and
// color by color.
func assertRuby(t *testing.T, c *taskcheck.C, li astisub.LineItem, want [][2][]rubyRun) {
	t.Helper()
	c.Check(li.Ruby != nil, "expected a ruby line item, got text %q", li.Text)
	if li.Ruby == nil {
		return
	}
	c.Check(len(li.Ruby.Pairs) == len(want), "expected %d ruby pair(s), got %d", len(want), len(li.Ruby.Pairs))
	if len(li.Ruby.Pairs) != len(want) {
		return
	}
	for i, pair := range li.Ruby.Pairs {
		assertRubyRuns(c, "pair %d base", i, pair.Base, want[i][0])
		assertRubyRuns(c, "pair %d text", i, pair.Text, want[i][1])
	}
}

func assertRubyRuns(c *taskcheck.C, what string, idx int, got []astisub.LineItem, want []rubyRun) {
	if len(got) != len(want) {
		c.Check(false, "%s %d: expected %d run(s), got %d", what, idx, len(want), len(got))
		return
	}
	for j := range want {
		c.Check(got[j].Text == want[j].text, "%s %d run %d: expected text %q, got %q", what, idx, j, want[j].text, got[j].Text)
		if want[j].color == nil {
			c.Check(got[j].InlineStyle == nil || got[j].InlineStyle.TTMLColor == nil,
				"%s %d run %d: expected no color, got %v", what, idx, j, got[j].InlineStyle)
			continue
		}
		c.Check(got[j].InlineStyle != nil && got[j].InlineStyle.TTMLColor != nil,
			"%s %d run %d: expected color %s, got none", what, idx, j, want[j].color.HTMLString())
		if got[j].InlineStyle != nil && got[j].InlineStyle.TTMLColor != nil {
			c.Check(got[j].InlineStyle.TTMLColor.HTMLString() == want[j].color.HTMLString(),
				"%s %d run %d: expected color %s, got %s", what, idx, j,
				want[j].color.HTMLString(), got[j].InlineStyle.TTMLColor.HTMLString())
		}
	}
}

// assertLineLayout checks the sequence of plain text and ruby items of a line.
// Each entry is either a plain string or the pair count of a ruby item.
func assertLineLayout(t *testing.T, c *taskcheck.C, line astisub.Line, want []interface{}) {
	t.Helper()
	c.Check(len(line.Items) == len(want), "expected %d item(s) on line, got %d", len(want), len(line.Items))
	if len(line.Items) != len(want) {
		return
	}
	for i, w := range want {
		switch v := w.(type) {
		case string:
			c.Check(line.Items[i].Ruby == nil, "item %d: expected plain text %q, got ruby", i, v)
			c.Check(line.Items[i].Text == v, "item %d: expected plain text %q, got %q", i, v, line.Items[i].Text)
		case int:
			c.Check(line.Items[i].Ruby != nil, "item %d: expected a ruby item", i)
			if line.Items[i].Ruby != nil {
				c.Check(len(line.Items[i].Ruby.Pairs) == v, "item %d: expected %d pair(s), got %d", i, v, len(line.Items[i].Ruby.Pairs))
			}
		default:
			c.Check(false, "item %d: unsupported expectation %T", i, w)
		}
	}
}

func rubyText(pairs ...*astisub.RubyPair) *astisub.Ruby {
	r := &astisub.Ruby{}
	for _, p := range pairs {
		r.Pairs = append(r.Pairs, *p)
	}
	return r
}

func run(text string, color *astisub.Color) astisub.LineItem {
	li := astisub.LineItem{Text: text}
	if color != nil {
		li.InlineStyle = &astisub.StyleAttributes{TTMLColor: color}
	}
	return li
}

// TestRubyWebVTT covers WebVTT reading and writing: many pairs in one ruby,
// several colored runs per side, colorless sides, and ordering with adjacent
// ruby annotations and normal text.
func TestRubyWebVTT(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// One ruby, two pairs, two colored base runs and a colorless one.
		video := "WEBVTT\n\n00:00:01.000 --> 00:00:03.000\n" +
			"<ruby><c.red>東</c><c.blue>京</c><rt>とうきょう</rt>駅<rt>えき</rt></ruby>\n"
		s, err := astisub.ReadFromWebVTT(strings.NewReader(video))
		c.Check(err == nil, "reading WebVTT failed: %v", err)
		if err != nil {
			return
		}
		c.Check(len(s.Items) == 1, "expected 1 item, got %d", len(s.Items))
		if len(s.Items) != 1 {
			return
		}
		c.Check(s.Items[0].StartAt == time.Second, "expected start 1s, got %s", s.Items[0].StartAt)
		c.Check(s.Items[0].EndAt == 3*time.Second, "expected end 3s, got %s", s.Items[0].EndAt)
		c.Check(s.Items[0].Lines[0].String() == "東京駅", "expected plain text %q, got %q", "東京駅", s.Items[0].Lines[0].String())

		assertLineLayout(t, c, s.Items[0].Lines[0], []interface{}{2})
		assertRuby(t, c, s.Items[0].Lines[0].Items[0], [][2][]rubyRun{
			{
				{{text: "東", color: astisub.ColorRed}, {text: "京", color: astisub.ColorBlue}},
				{{text: "とうきょう"}},
			},
			{
				{{text: "駅"}},
				{{text: "えき"}},
			},
		})

		// Writing back produces the same markup and round-trips.
		var b bytes.Buffer
		c.Check(s.WriteToWebVTT(&b) == nil, "writing WebVTT failed")
		c.Check(strings.Contains(b.String(), "<ruby>"), "WebVTT output lost the ruby container: %s", b.String())
		c.Check(strings.Contains(b.String(), "<rt>とうきょう</rt>駅<rt>えき</rt>"), "WebVTT output lost a pair: %s", b.String())
		back, err := astisub.ReadFromWebVTT(bytes.NewReader(b.Bytes()))
		c.Check(err == nil, "reading written WebVTT failed: %v", err)
		if err == nil {
			c.Check(len(back.Items) == 1, "expected 1 item after WebVTT round trip, got %d", len(back.Items))
			if len(back.Items) == 1 {
				assertRuby(t, c, back.Items[0].Lines[0].Items[0], [][2][]rubyRun{
					{
						{{text: "東", color: astisub.ColorRed}, {text: "京", color: astisub.ColorBlue}},
						{{text: "とうきょう"}},
					},
					{
						{{text: "駅"}},
						{{text: "えき"}},
					},
				})
			}
		}

		// Adjacent rubies and normal text keep their order, and a colorless
		// side is allowed on both sides.
		video = "WEBVTT\n\n00:00:01.000 --> 00:00:03.000\n" +
			"前<ruby>東<rt>とう</rt></ruby>中<ruby><c.green>京</c>駅<rt>きょうえき</rt></ruby>後\n"
		s, err = astisub.ReadFromWebVTT(strings.NewReader(video))
		c.Check(err == nil, "reading observed-order WebVTT failed: %v", err)
		if err != nil {
			return
		}
		c.Check(s.Items[0].Lines[0].String() == "前東中京駅後", "expected %q, got %q", "前東中京駅後", s.Items[0].Lines[0].String())
		assertLineLayout(t, c, s.Items[0].Lines[0], []interface{}{"前", 1, "中", 1, "後"})
		if len(s.Items[0].Lines[0].Items) == 5 {
			assertRuby(t, c, s.Items[0].Lines[0].Items[1], [][2][]rubyRun{
				{{{text: "東"}}, {{text: "とう"}}},
			})
			assertRuby(t, c, s.Items[0].Lines[0].Items[3], [][2][]rubyRun{
				{{{text: "京", color: astisub.ColorGreen}, {text: "駅"}}, {{text: "きょうえき"}}},
			})
		}

		// Each ruby is written as its own container, in order.
		b.Reset()
		c.Check(s.WriteToWebVTT(&b) == nil, "writing observed-order WebVTT failed")
		out := b.String()
		c.Check(strings.Contains(out, "前<ruby>東<rt>とう</rt></ruby>中<ruby><c.green>京</c>駅<rt>きょうえき</rt></ruby>後"),
			"unexpected WebVTT output: %s", out)

		// A ruby with an uncolored base and a multi-run annotation keeps the
		// pair count.
		video = "WEBVTT\n\n00:00:04.000 --> 00:00:05.000\n" +
			"<ruby><c.red>日</c><c.blue>本</c><rt>に</rt><c.yellow>ほん</c></ruby>\n"
		s, err = astisub.ReadFromWebVTT(strings.NewReader(video))
		c.Check(err == nil, "reading multi-run annotation failed: %v", err)
		if err != nil {
			return
		}
		if len(s.Items) == 1 && len(s.Items[0].Lines) == 1 && len(s.Items[0].Lines[0].Items) == 1 {
			assertRuby(t, c, s.Items[0].Lines[0].Items[0], [][2][]rubyRun{
				{
					{{text: "日", color: astisub.ColorRed}, {text: "本", color: astisub.ColorBlue}},
					{{text: "に"}, {text: "ほん", color: astisub.ColorYellow}},
				},
			})
			c.Check(s.Items[0].Lines[0].String() == "日本", "expected base text only, got %q", s.Items[0].Lines[0].String())
		}
	})
}

// TestRubyTTML covers TTML reading and writing in both structures (simple and
// grouped), color inheritance, xml:space preservation, br handling and
// bidirectional conversion.
func TestRubyTTML(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		simple := `<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling">
  <body>
    <div>
      <p begin="00:00:01.000" end="00:00:03.000">
        <span tts:ruby="container" xml:space="preserve">
          <span tts:ruby="base"><span tts:color="red" tts:ruby="base">東</span><span tts:color="blue">京</span></span>
          <span tts:ruby="text">とうきょう</span>
        </span>
      </p>
    </div>
  </body>
</tt>`
		s, err := astisub.ReadFromTTML(strings.NewReader(simple))
		c.Check(err == nil, "reading simple TTML failed: %v", err)
		if err != nil {
			return
		}
		c.Check(len(s.Items) == 1, "expected 1 item, got %d", len(s.Items))
		if len(s.Items) != 1 {
			return
		}
		c.Check(s.Items[0].StartAt == time.Second, "expected start 1s, got %s", s.Items[0].StartAt)
		c.Check(s.Items[0].Lines[0].String() == "東京", "expected base text %q, got %q", "東京", s.Items[0].Lines[0].String())
		assertLineLayout(t, c, s.Items[0].Lines[0], []interface{}{1})
		assertRuby(t, c, s.Items[0].Lines[0].Items[0], [][2][]rubyRun{
			{
				{{text: "東", color: astisub.ColorRed}, {text: "京", color: astisub.ColorBlue}},
				{{text: "とうきょう"}},
			},
		})

		// Grouped structure: baseContainer + textContainer, colors inherited
		// from the container and overridden by the inner span.
		grouped := `<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling">
  <body>
    <div>
      <p begin="00:00:01.000" end="00:00:03.000">
        <span tts:ruby="container" tts:color="green" xml:space="preserve">
          <span tts:ruby="baseContainer">
            <span tts:ruby="base"><span tts:color="red">東</span><span>京</span></span>
            <span tts:ruby="base">駅</span>
          </span>
          <span tts:ruby="textContainer">
            <span tts:ruby="text">とうきょう</span>
            <span tts:ruby="text"><span tts:color="blue">えき</span></span>
          </span>
        </span>
      </p>
    </div>
  </body>
</tt>`
		s, err = astisub.ReadFromTTML(strings.NewReader(grouped))
		c.Check(err == nil, "reading grouped TTML failed: %v", err)
		if err != nil {
			return
		}
		c.Check(len(s.Items) == 1 && len(s.Items[0].Lines) == 1, "unexpected line count in grouped TTML")
		if len(s.Items) != 1 || len(s.Items[0].Lines) != 1 {
			return
		}
		c.Check(s.Items[0].Lines[0].String() == "東京駅", "expected base text %q, got %q", "東京駅", s.Items[0].Lines[0].String())
		assertLineLayout(t, c, s.Items[0].Lines[0], []interface{}{2})
		assertRuby(t, c, s.Items[0].Lines[0].Items[0], [][2][]rubyRun{
			{
				{{text: "東", color: astisub.ColorRed}, {text: "京", color: astisub.ColorGreen}},
				{{text: "とうきょう", color: astisub.ColorGreen}},
			},
			{
				{{text: "駅", color: astisub.ColorGreen}},
				{{text: "えき", color: astisub.ColorBlue}},
			},
		})

		// Writing normalizes one pair to a simple container and several pairs
		// to a grouped one, never interleaving pairs under a simple container.
		var b bytes.Buffer
		c.Check(s.WriteToTTML(&b) == nil, "writing grouped TTML failed")
		out := b.String()
		c.Check(strings.Contains(out, `tts:ruby="container"`), "TTML output lost the ruby container: %s", out)
		c.Check(strings.Contains(out, `tts:ruby="baseContainer"`) && strings.Contains(out, `tts:ruby="textContainer"`),
			"TTML output did not use grouped containers for two pairs: %s", out)
		c.Check(strings.Count(out, `tts:ruby="base"`) == 2, "expected 2 base spans, got %d: %s", strings.Count(out, `tts:ruby="base"`), out)
		c.Check(strings.Count(out, `tts:ruby="text"`) == 2, "expected 2 text spans, got %d: %s", strings.Count(out, `tts:ruby="text"`), out)

		back, err := astisub.ReadFromTTML(bytes.NewReader(b.Bytes()))
		c.Check(err == nil, "reading written TTML failed: %v", err)
		if err == nil && len(back.Items) == 1 && len(back.Items[0].Lines) == 1 && len(back.Items[0].Lines[0].Items) == 1 {
			assertRuby(t, c, back.Items[0].Lines[0].Items[0], [][2][]rubyRun{
				{
					{{text: "東", color: astisub.ColorRed}, {text: "京", color: astisub.ColorGreen}},
					{{text: "とうきょう", color: astisub.ColorGreen}},
				},
				{
					{{text: "駅", color: astisub.ColorGreen}},
					{{text: "えき", color: astisub.ColorBlue}},
				},
			})
		}

		// xml:space="preserve" keeps spaces inside the base and annotation, a
		// br between groups starts a new line, and indentation is not text.
		spaces := `<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling">
  <body xml:space="preserve">
    <div>
      <p begin="00:00:01.000" end="00:00:03.000">
        <span tts:ruby="container">
          <span tts:ruby="base">東 京</span>
          <span tts:ruby="text">とう きょう</span>
        </span>
        <br/>
        <span tts:ruby="container">
          <span tts:ruby="base">駅</span>
          <span tts:ruby="text">えき</span>
        </span>
      </p>
    </div>
  </body>
</tt>`
		s, err = astisub.ReadFromTTML(strings.NewReader(spaces))
		c.Check(err == nil, "reading xml:space TTML failed: %v", err)
		if err != nil {
			return
		}
		c.Check(len(s.Items) == 1, "expected 1 item, got %d", len(s.Items))
		if len(s.Items) != 1 {
			return
		}
		c.Check(len(s.Items[0].Lines) == 2, "expected 2 lines around br, got %d", len(s.Items[0].Lines))
		if len(s.Items[0].Lines) != 2 {
			return
		}
		c.Check(s.Items[0].Lines[0].String() == "東 京", "expected preserved spaces %q, got %q", "東 京", s.Items[0].Lines[0].String())
		assertRuby(t, c, s.Items[0].Lines[0].Items[0], [][2][]rubyRun{
			{{{text: "東 京"}}, {{text: "とう きょう"}}},
		})
		c.Check(s.Items[0].Lines[1].String() == "駅", "expected %q, got %q", "駅", s.Items[0].Lines[1].String())

		// TTML -> WebVTT -> TTML keeps both pairs and both colors.
		var vtt bytes.Buffer
		c.Check(s.Items[0].Lines != nil && s.WriteToWebVTT(&vtt) == nil, "writing WebVTT from TTML failed")
		round, err := astisub.ReadFromWebVTT(bytes.NewReader(vtt.Bytes()))
		c.Check(err == nil, "reading WebVTT from TTML failed: %v", err)
		if err == nil && len(round.Items) == 1 {
			var ttmlOut bytes.Buffer
			c.Check(round.WriteToTTML(&ttmlOut) == nil, "writing TTML from WebVTT failed")
			final, err := astisub.ReadFromTTML(bytes.NewReader(ttmlOut.Bytes()))
			c.Check(err == nil, "reading final TTML failed: %v", err)
			if err == nil && len(final.Items) == 1 && len(final.Items[0].Lines) == 2 {
				c.Check(final.Items[0].Lines[0].String() == "東 京", "TTML->WebVTT->TTML lost base text: %q", final.Items[0].Lines[0].String())
			}
		}
	})
}

// TestRubyEdit builds a ruby directly (no parsing), edits it, and checks every
// export reflects the edit rather than any cached source text.
func TestRubyEdit(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Build the same two pairs as in the WebVTT example, by hand.
		tokyo := run("東", astisub.ColorRed)
		kyo := run("京", astisub.ColorBlue)
		ruby := rubyText(
			&astisub.RubyPair{
				Base: []astisub.LineItem{tokyo, kyo},
				Text: []astisub.LineItem{run("とうきょう", nil)},
			},
			&astisub.RubyPair{
				Base: []astisub.LineItem{run("駅", nil)},
				Text: []astisub.LineItem{run("えき", nil)},
			},
		)

		s := astisub.NewSubtitles()
		s.Items = append(s.Items, &astisub.Item{
			StartAt: time.Second,
			EndAt:   3 * time.Second,
			Lines: []astisub.Line{{Items: []astisub.LineItem{
				{Text: "前"},
				{Ruby: ruby},
				{Text: "後"},
			}}},
		})

		// The plain-text view only shows the base text.
		c.Check(s.Items[0].Lines[0].String() == "前東京駅後", "expected %q, got %q", "前東京駅後", s.Items[0].Lines[0].String())
		var srt bytes.Buffer
		c.Check(s.WriteToSRT(&srt) == nil, "writing SRT failed")
		c.Check(strings.Contains(srt.String(), "前東京駅後"), "SRT lost the base text: %s", srt.String())
		c.Check(!strings.Contains(srt.String(), "とうきょう"), "SRT leaked the annotation: %s", srt.String())

		// Write once before editing to make sure the edit is not masked by a
		// parser cache; then edit through the public representation.
		var before bytes.Buffer
		c.Check(s.WriteToTTML(&before) == nil, "writing pre-edit TTML failed")
		c.Check(strings.Contains(before.String(), "とうきょう"), "pre-edit TTML missing annotation: %s", before.String())

		li := &s.Items[0].Lines[0].Items[1]
		c.Check(li.Ruby == ruby, "the constructed ruby must be reachable through the item")
		li.Ruby.Pairs[0].Text[0].Text = "Tokyo"
		li.Ruby.Pairs[0].Base[0].InlineStyle.TTMLColor = astisub.ColorGreen

		// SRT reflects only the base text; line String is unchanged.
		srt.Reset()
		c.Check(s.WriteToSRT(&srt) == nil, "writing edited SRT failed")
		c.Check(strings.Contains(srt.String(), "前東京駅後"), "edited SRT lost base text: %s", srt.String())
		c.Check(!strings.Contains(srt.String(), "Tokyo"), "SRT leaked the edited annotation: %s", srt.String())

		// TTML -> WebVTT -> TTML keeps two pairs with the edited annotation and
		// the edited color on the first base run only.
		var ttmlBuf bytes.Buffer
		c.Check(s.WriteToTTML(&ttmlBuf) == nil, "writing edited TTML failed")
		c.Check(strings.Contains(ttmlBuf.String(), "Tokyo"), "edited TTML missing the new annotation: %s", ttmlBuf.String())
		c.Check(!strings.Contains(ttmlBuf.String(), "とうきょう"), "edited TTML still contains the old annotation: %s", ttmlBuf.String())

		fromTTML, err := astisub.ReadFromTTML(bytes.NewReader(ttmlBuf.Bytes()))
		c.Check(err == nil, "reading edited TTML failed: %v", err)
		if err != nil {
			return
		}
		if len(fromTTML.Items) != 1 || len(fromTTML.Items[0].Lines) != 1 || len(fromTTML.Items[0].Lines[0].Items) != 3 {
			c.Check(false, "edited TTML changed the item layout")
			return
		}
		assertRuby(t, c, fromTTML.Items[0].Lines[0].Items[1], [][2][]rubyRun{
			{
				{{text: "東", color: astisub.ColorGreen}, {text: "京", color: astisub.ColorBlue}},
				{{text: "Tokyo"}},
			},
			{
				{{text: "駅"}},
				{{text: "えき"}},
			},
		})

		var vttBuf bytes.Buffer
		c.Check(fromTTML.WriteToWebVTT(&vttBuf) == nil, "writing edited WebVTT failed")
		c.Check(strings.Contains(vttBuf.String(), "<rt>Tokyo</rt>"), "edited WebVTT missing the new annotation: %s", vttBuf.String())
		c.Check(strings.Contains(vttBuf.String(), "<c.green>東</c>"), "edited WebVTT missing the new color: %s", vttBuf.String())

		fromVTT, err := astisub.ReadFromWebVTT(bytes.NewReader(vttBuf.Bytes()))
		c.Check(err == nil, "reading edited WebVTT failed: %v", err)
		if err != nil {
			return
		}
		var finalBuf bytes.Buffer
		c.Check(fromVTT.WriteToTTML(&finalBuf) == nil, "writing final TTML failed")
		final, err := astisub.ReadFromTTML(bytes.NewReader(finalBuf.Bytes()))
		c.Check(err == nil, "reading final TTML failed: %v", err)
		if err != nil {
			return
		}
		if len(final.Items) != 1 || len(final.Items[0].Lines) != 1 || len(final.Items[0].Lines[0].Items) != 3 {
			c.Check(false, "final TTML changed the item layout")
			return
		}
		c.Check(final.Items[0].Lines[0].String() == "前東京駅後", "final plain text changed: %q", final.Items[0].Lines[0].String())
		assertRuby(t, c, final.Items[0].Lines[0].Items[1], [][2][]rubyRun{
			{
				{{text: "東", color: astisub.ColorGreen}, {text: "京", color: astisub.ColorBlue}},
				{{text: "Tokyo"}},
			},
			{
				{{text: "駅"}},
				{{text: "えき"}},
			},
		})
	})
}
