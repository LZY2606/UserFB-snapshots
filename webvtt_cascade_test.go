package astisub_test

import (
	"bytes"
	"strconv"
	"strings"
	"testing"

	"github.com/asticode/go-astisub"
	taskcheck "github.com/asticode/go-astisub/task_support/taskcheck"
)

const defaultWebVTStyleID = "astisub-webvtt-default-style-id"

func checkOK(c *taskcheck.C, ok bool, message string, args ...interface{}) bool {
	c.Check(ok, message, args...)
	return ok
}

type cueSpanWant struct {
	text       string
	color      string
	background string
	weight     string
	style      string
	decoration string
}

func TestCueCascade(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		checkCascadeInheritance(c)
		checkCascadePriority(c)
		checkCascadeGroupingCommentsAndRecovery(c)
		checkCascadeDisableBuiltInTags(c)
		checkCascadeInheritInitial(c)
		checkCascadeMultipleStyleBlocks(c)
		checkCascadeInactiveWithoutSupportedDeclaration(c)
	})
}

func TestCueCascadeEdits(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		input := `WEBVTT

STYLE
::cue(.a) { color: red; }
::cue(.bold) { font-weight: bold; }

00:00:01.000 --> 00:00:03.000
<c.a>Alpha</c><c.beta>Beta</c><b>Bold</b><i>Italic</i>
`
		s := readWebVTTForCueCheck(c, input)
		if s == nil {
			return
		}

		want := []cueSpanWant{
			{text: "Alpha", color: "#ff0000", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
			{text: "Beta", color: "#ffffff", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
			{text: "Bold", color: "#ffffff", background: "#000000", weight: "bold", style: "normal", decoration: "none"},
			{text: "Italic", color: "#ffffff", background: "#000000", weight: "normal", style: "italic", decoration: "none"},
		}
		checkExportedTTMLSpans(c, s, want, "initial edit fixture")
		assertStyleAttributesNotComputed(c, s, "after initial export")

		// Re-exporting without edits must be stable.
		first := exportTTMLForCueCheck(c, s)
		second := exportTTMLForCueCheck(c, s)
		checkOK(c, bytes.Equal(first, second), "continuous TTML exports differ")

		// Edit the STYLE slice currently carried by the parsed subtitle.
		sa := webVTTStyleAttributesForCheck(c, s)
		if sa == nil {
			return
		}
		sa.WebVTTStyles = []string{
			"::cue(.a) { color: #102030; }",
			"::cue(b) { font-weight: normal; }",
			"::cue(.fresh) { background-color: #0a0b0c; }",
		}
		wantAfterRuleEdit := []cueSpanWant{
			{text: "Alpha", color: "#102030", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
			{text: "Beta", color: "#ffffff", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
			{text: "Bold", color: "#ffffff", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
			{text: "Italic", color: "#ffffff", background: "#000000", weight: "normal", style: "italic", decoration: "none"},
		}
		checkExportedTTMLSpans(c, s, wantAfterRuleEdit, "after first rule edit")
		assertStyleAttributesNotComputed(c, s, "after rule-edit export")

		// Edit current tag stacks directly. These are complete stacks for each run.
		line := s.Items[0].Lines[0]
		if checkOK(c, len(line.Items) == 4, "edit fixture run count = %d, want 4", len(line.Items)) {
			line.Items[0].InlineStyle.WebVTTTags[0].Classes[0] = "fresh"
			line.Items[1].InlineStyle.WebVTTTags = []astisub.WebVTTTag{{Name: "c", Classes: []string{"a"}}}
			line.Items[2].InlineStyle.WebVTTTags = nil
			line.Items[3].InlineStyle.WebVTTTags = nil
		}
		wantAfterTagEdit := []cueSpanWant{
			{text: "Alpha", color: "#ffffff", background: "#0a0b0c", weight: "normal", style: "normal", decoration: "none"},
			{text: "Beta", color: "#102030", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
			{text: "Bold", color: "#ffffff", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
			{text: "Italic", color: "#ffffff", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
		}
		checkExportedTTMLSpans(c, s, wantAfterTagEdit, "after tag edit")

		// A second rule edit must replace, not accumulate, earlier edited rules.
		sa.WebVTTStyles = []string{"::cue(.fresh) { color: magenta; background-color: white; }"}
		wantAfterSecondRuleEdit := []cueSpanWant{
			{text: "Alpha", color: "#ff00ff", background: "#ffffff", weight: "normal", style: "normal", decoration: "none"},
			{text: "Beta", color: "#ffffff", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
			{text: "Bold", color: "#ffffff", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
			{text: "Italic", color: "#ffffff", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
		}
		checkExportedTTMLSpans(c, s, wantAfterSecondRuleEdit, "after second rule edit")
	})

	taskcheck.Run(t, func(c *taskcheck.C) {
		input := `WEBVTT

STYLE
::cue(.shared) { color: red; }
::cue(.changed) { color: blue; }

00:00:01.000 --> 00:00:03.000
<c.shared>One</c><c.shared>Two</c>
`
		s := readWebVTTForCueCheck(c, input)
		if s == nil {
			return
		}
		shared := &astisub.StyleAttributes{WebVTTTags: []astisub.WebVTTTag{{
			Name:    "c",
			Classes: []string{"shared"},
		}}}
		line := s.Items[0].Lines[0]
		if checkOK(c, len(line.Items) == 2, "shared fixture run count = %d, want 2", len(line.Items)) {
			line.Items[0].InlineStyle = shared
			line.Items[1].InlineStyle = shared
		}

		want := []cueSpanWant{
			{text: "One", color: "#ff0000", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
			{text: "Two", color: "#ff0000", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
		}
		checkExportedTTMLSpans(c, s, want, "shared pointer")
		checkOK(c, shared.TTMLColor == nil, "export mutated shared TTMLColor")
		checkOK(c, shared.TTMLBackgroundColor == nil, "export mutated shared TTMLBackgroundColor")
		checkOK(c, shared.TTMLFontWeight == nil, "export mutated shared TTMLFontWeight")
		checkOK(c, shared.TTMLFontStyle == nil, "export mutated shared TTMLFontStyle")
		checkOK(c, shared.TTMLTextDecoration == nil, "export mutated shared TTMLTextDecoration")

		sa := webVTTStyleAttributesForCheck(c, s)
		if sa != nil && checkOK(c, len(sa.WebVTTStyles) >= 2, "shared fixture style count = %d", len(sa.WebVTTStyles)) {
			sa.WebVTTStyles[0] = "::cue(.shared) { color: green; }"
		}
		want[0].color = "#008000"
		want[1].color = "#008000"
		checkExportedTTMLSpans(c, s, want, "shared pointer after rule edit")
		checkOK(c, shared.TTMLColor == nil, "re-export cached color into shared pointer")
	})
}

func TestCueCascadeExport(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		input := `WEBVTT

STYLE
::cue(.export) { color: #112233; background-color: #445566; font-weight: bold; font-style: italic; text-decoration: underline; }
::cue(b) { font-weight: normal; }

00:00:01.000 --> 00:00:03.000
Hello <c.export><00:00:01.500>word</c>
Second <i>line</i>
`
		s := readWebVTTForCueCheck(c, input)
		if s == nil {
			return
		}
		if checkOK(c, len(s.Items) == 1, "export item count = %d, want 1", len(s.Items)) {
			checkOK(c, s.Items[0].StartAt.Seconds() == 1, "start = %v, want 1s", s.Items[0].StartAt)
			checkOK(c, s.Items[0].EndAt.Seconds() == 3, "end = %v, want 3s", s.Items[0].EndAt)
			checkOK(c, len(s.Items[0].Lines) == 2, "line count = %d, want 2", len(s.Items[0].Lines))
		}

		ttmlBytes := exportTTMLForCueCheck(c, s)
		if len(ttmlBytes) == 0 {
			return
		}
		ttmlText := string(ttmlBytes)
		checkOK(c, strings.Contains(ttmlText, "#112233"), "TTML missing cascade color #112233: %s", ttmlText)
		checkOK(c, strings.Contains(ttmlText, "#445566"), "TTML missing cascade background #445566: %s", ttmlText)
		checkOK(c, strings.Contains(ttmlText, `tts:fontWeight="normal"`), "TTML missing explicit normal font-weight: %s", ttmlText)
		checkOK(c, strings.Contains(ttmlText, `tts:textDecoration="none"`), "TTML missing explicit none text-decoration: %s", ttmlText)

		rereadTTML, err := astisub.ReadFromTTML(bytes.NewReader(ttmlBytes))
		if !checkOK(c, err == nil, "reread TTML error: %v", err) || rereadTTML == nil {
			return
		}
		ttmlSpans := allCueSpans(rereadTTML)
		checkOK(c, len(ttmlSpans) >= 4, "reread TTML span count = %d, want at least 4", len(ttmlSpans))
		var foundWordStart bool
		for _, span := range ttmlSpans {
			if strings.Contains(span.Text, "word") {
				foundWordStart = true
			}
		}
		checkOK(c, foundWordStart, "TTML reread lost word text; spans=%v", cueSpanTexts(ttmlSpans))

		// WebVTT save/re-read must retain STYLE, tags, timing, lines and word starts.
		var vttBuffer bytes.Buffer
		err = s.WriteToWebVTT(&vttBuffer)
		if !checkOK(c, err == nil, "WriteToWebVTT error: %v", err) {
			return
		}
		vttText := vttBuffer.String()
		checkOK(c, strings.Contains(vttText, "::cue(.export)"), "WebVTT save lost edited/current rule: %s", vttText)
		checkOK(c, strings.Contains(vttText, "<00:00:01.500>"), "WebVTT save lost word timestamp: %s", vttText)

		rereadVTT := readWebVTTForCueCheck(c, vttText)
		if rereadVTT == nil {
			return
		}
		if checkOK(c, len(rereadVTT.Items) == 1, "VTT roundtrip item count = %d", len(rereadVTT.Items)) {
			checkOK(c, rereadVTT.Items[0].StartAt == s.Items[0].StartAt, "VTT roundtrip start = %v, want %v", rereadVTT.Items[0].StartAt, s.Items[0].StartAt)
			checkOK(c, rereadVTT.Items[0].EndAt == s.Items[0].EndAt, "VTT roundtrip end = %v, want %v", rereadVTT.Items[0].EndAt, s.Items[0].EndAt)
			checkOK(c, len(rereadVTT.Items[0].Lines) == 2, "VTT roundtrip line count = %d, want 2", len(rereadVTT.Items[0].Lines))
			var wordStartFound bool
			for _, item := range rereadVTT.Items[0].Lines[0].Items {
				if strings.Contains(item.Text, "word") && item.StartAt.Seconds() == 1.5 {
					wordStartFound = true
				}
			}
			checkOK(c, wordStartFound, "VTT roundtrip lost 1.5s word start: %#v", rereadVTT.Items[0].Lines[0].Items)
		}

		roundtripTTML := exportTTMLForCueCheck(c, rereadVTT)
		roundtripSubtitles := rereadTTMLForCueCheck(c, roundtripTTML)
		if roundtripSubtitles == nil {
			return
		}
		roundtripSpans := allCueSpans(roundtripSubtitles)
		checkOK(c, len(roundtripSpans) == len(ttmlSpans), "VTT->TTML span count = %d, want %d", len(roundtripSpans), len(ttmlSpans))
		for i := 0; i < len(ttmlSpans) && i < len(roundtripSpans); i++ {
			want := cueSpanWantFromLineItem(ttmlSpans[i])
			checkOneCueSpan(c, "webvtt-roundtrip/span#"+strconv.Itoa(i), roundtripSpans[i], want)
		}
	})
}

func checkCascadeInheritance(c *taskcheck.C) {
	input := `WEBVTT

STYLE
::cue(.box) { color: #123456; background-color: #234567; font-weight: bold; font-style: italic; text-decoration: underline; }
::cue(.inner) { color: #abcdef; }

00:00:01.000 --> 00:00:03.000
Plain<c.box>box<b>bold<i>deep</i></b>tail<c.inner>inner</c></c>
`
	s := readWebVTTForCueCheck(c, input)
	if s == nil {
		return
	}
	checkExportedTTMLSpans(c, s, []cueSpanWant{
		{text: "Plain", color: "#ffffff", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
		{text: "box", color: "#123456", background: "#234567", weight: "bold", style: "italic", decoration: "underline"},
		{text: "bold", color: "#123456", background: "#234567", weight: "bold", style: "italic", decoration: "underline"},
		{text: "deep", color: "#123456", background: "#234567", weight: "bold", style: "italic", decoration: "underline"},
		{text: "tail", color: "#123456", background: "#234567", weight: "bold", style: "italic", decoration: "underline"},
		{text: "inner", color: "#abcdef", background: "#234567", weight: "bold", style: "italic", decoration: "underline"},
	}, "nested inheritance")
}

func checkCascadePriority(c *taskcheck.C) {
	input := `WEBVTT

STYLE
::cue(c.specific) { color: red; }
::cue(.specific) { color: green !important; }
::cue(c.specific) { color: blue; }
::cue(.weight) { font-weight: normal; }
::cue(b) { font-weight: bold; }
::cue(.order) { background-color: red; }
::cue(.order) { background-color: blue; }
::cue { color: red !important; }
::cue(.child) { color: blue; }

00:00:01.000 --> 00:00:03.000
<c.specific>Important</c><b.weight>Weight</b><c.order>Order</c>Root<c.child>Child</c>
`
	s := readWebVTTForCueCheck(c, input)
	if s == nil {
		return
	}
	checkExportedTTMLSpans(c, s, []cueSpanWant{
		{text: "Important", color: "#008000", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
		{text: "Weight", color: "#ff0000", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
		{text: "Order", color: "#ff0000", background: "#0000ff", weight: "normal", style: "normal", decoration: "none"},
		{text: "Root", color: "#ff0000", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
		{text: "Child", color: "#0000ff", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
	}, "priority")
}

func checkCascadeGroupingCommentsAndRecovery(c *taskcheck.C) {
	input := `WEBVTT

STYLE
/* ::cue(.note) { color: black; background-color: black; font-weight: bold; ; } */
::cue(.g1), ::cue(.g2) { COLOR: CYAN; BACKGROUND-COLOR: YELLOW; FONT-WEIGHT: BOLD; }
::cue(.bad ??), ::cue(.g3) { color: red; }
::cue(.recover) {
  color: bogus;
  unknown-prop: 1px;
  font-weight: heavy;
  color: #00FF00;
  font-style: italic;
}
::cue(.commented) /* between selector and block */ { color: white; }
::cue(.after) { TEXT-DECORATION: UNDERLINE !IMPORTANT; }

00:00:01.000 --> 00:00:03.000
<c.g1>G1</c><c.g2>G2</c><c.g3>G3</c><c.recover>Recover</c><c.commented>Commented</c><c.after>After</c>
`
	s := readWebVTTForCueCheck(c, input)
	if s == nil {
		return
	}
	checkExportedTTMLSpans(c, s, []cueSpanWant{
		{text: "G1", color: "#00ffff", background: "#ffff00", weight: "bold", style: "normal", decoration: "none"},
		{text: "G2", color: "#00ffff", background: "#ffff00", weight: "bold", style: "normal", decoration: "none"},
		{text: "G3", color: "#ffffff", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
		{text: "Recover", color: "#00ff00", background: "#000000", weight: "normal", style: "italic", decoration: "none"},
		{text: "Commented", color: "#ffffff", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
		{text: "After", color: "#ffffff", background: "#000000", weight: "normal", style: "normal", decoration: "underline"},
	}, "grouping comments recovery")
}

func checkCascadeDisableBuiltInTags(c *taskcheck.C) {
	input := `WEBVTT

STYLE
::cue(b) { font-weight: normal; color: #010101; }
::cue(i) { font-style: normal; }
::cue(u) { text-decoration: none; }

00:00:01.000 --> 00:00:03.000
<b>B</b><i>I</i><u>U</u><b><i><u>All</u></i></b>
`
	s := readWebVTTForCueCheck(c, input)
	if s == nil {
		return
	}
	checkExportedTTMLSpans(c, s, []cueSpanWant{
		{text: "B", color: "#010101", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
		{text: "I", color: "#ffffff", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
		{text: "U", color: "#ffffff", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
		{text: "All", color: "#010101", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
	}, "disable b/i/u")
}

func checkCascadeInheritInitial(c *taskcheck.C) {
	input := `WEBVTT

STYLE
::cue { color: inherit; background-color: inherit; font-weight: inherit; font-style: inherit; text-decoration: inherit; }
::cue(.parent) { color: #010203; background-color: #040506; font-weight: bold; font-style: italic; text-decoration: underline; }
::cue(.inherit) { color: inherit; background-color: inherit; font-weight: inherit; font-style: inherit; text-decoration: inherit; }
::cue(.reset) { color: initial; background-color: initial; font-weight: initial; font-style: initial; text-decoration: initial; }

00:00:01.000 --> 00:00:03.000
Root<c.parent>P<c.inherit>I<c.reset>R</c></c></c>
`
	s := readWebVTTForCueCheck(c, input)
	if s == nil {
		return
	}
	checkExportedTTMLSpans(c, s, []cueSpanWant{
		{text: "Root", color: "#ffffff", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
		{text: "P", color: "#010203", background: "#040506", weight: "bold", style: "italic", decoration: "underline"},
		{text: "I", color: "#010203", background: "#040506", weight: "bold", style: "italic", decoration: "underline"},
		{text: "R", color: "#ffffff", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
	}, "inherit/initial")
}

func checkCascadeMultipleStyleBlocks(c *taskcheck.C) {
	input := `WEBVTT

STYLE
::cue(.multi) { color: red; }

STYLE
::cue(.multi) { color: blue; }

00:00:01.000 --> 00:00:03.000
<c.multi>Multi</c>
`
	s := readWebVTTForCueCheck(c, input)
	if s == nil {
		return
	}
	checkExportedTTMLSpans(c, s, []cueSpanWant{
		{text: "Multi", color: "#0000ff", background: "#000000", weight: "normal", style: "normal", decoration: "none"},
	}, "multiple STYLE blocks")
}

func checkCascadeInactiveWithoutSupportedDeclaration(c *taskcheck.C) {
	input := `WEBVTT

00:00:01.000 --> 00:00:03.000
<b>Bold</b><c.red>Red</c>
`
	s := readWebVTTForCueCheck(c, input)
	if s == nil {
		return
	}
	data := exportTTMLForCueCheck(c, s)
	reread := rereadTTMLForCueCheck(c, data)
	if reread == nil {
		return
	}
	spans := allCueSpans(reread)
	if checkOK(c, len(spans) == 2, "inactive span count = %d, want 2", len(spans)) {
		bold := spans[0]
		checkOK(c, bold.Text == "Bold", "inactive first text = %q", bold.Text)
		checkOK(c, bold.InlineStyle != nil && bold.InlineStyle.TTMLFontWeight == nil,
			"old path should not synthesize cascade font-weight, got %#v", bold.InlineStyle)
		red := spans[1]
		checkColorValue(c, "old built-in color path", red.InlineStyle.TTMLColor, "#ff0000")
	}
}

func readWebVTTForCueCheck(c *taskcheck.C, input string) *astisub.Subtitles {
	s, err := astisub.ReadFromWebVTT(strings.NewReader(input))
	checkOK(c, err == nil, "ReadFromWebVTT error: %v", err)
	if s == nil {
		checkOK(c, false, "ReadFromWebVTT returned nil subtitles")
	}
	return s
}

func webVTTStyleAttributesForCheck(c *taskcheck.C, s *astisub.Subtitles) *astisub.StyleAttributes {
	style := s.Styles[defaultWebVTStyleID]
	ok := checkOK(c, style != nil && style.InlineStyle != nil, "default WebVTT style attributes missing")
	if !ok {
		return nil
	}
	return style.InlineStyle
}

func exportTTMLForCueCheck(c *taskcheck.C, s *astisub.Subtitles) []byte {
	var buffer bytes.Buffer
	err := s.WriteToTTML(&buffer)
	checkOK(c, err == nil, "WriteToTTML error: %v", err)
	return buffer.Bytes()
}

func rereadTTMLForCueCheck(c *taskcheck.C, data []byte) *astisub.Subtitles {
	s, err := astisub.ReadFromTTML(bytes.NewReader(data))
	checkOK(c, err == nil, "ReadFromTTML error: %v\nTTML:\n%s", err, string(data))
	return s
}

func checkExportedTTMLSpans(c *taskcheck.C, s *astisub.Subtitles, want []cueSpanWant, context string) {
	data := exportTTMLForCueCheck(c, s)
	if len(data) == 0 {
		return
	}
	reread := rereadTTMLForCueCheck(c, data)
	if reread == nil {
		return
	}
	got := allCueSpans(reread)
	checkOK(c, len(got) == len(want), "%s: span count = %d, want %d; texts=%v", context, len(got), len(want), cueSpanTexts(got))
	for i := 0; i < len(want) && i < len(got); i++ {
		checkOneCueSpan(c, context+"/span#"+strconv.Itoa(i), got[i], want[i])
	}
}

func checkOneCueSpan(c *taskcheck.C, context string, got astisub.LineItem, want cueSpanWant) {
	checkOK(c, got.Text == want.text, "%s: text = %q, want %q", context, got.Text, want.text)
	if !checkOK(c, got.InlineStyle != nil, "%s: missing InlineStyle for %q", context, want.text) {
		return
	}
	checkColorValue(c, context+": color", got.InlineStyle.TTMLColor, want.color)
	checkColorValue(c, context+": background-color", got.InlineStyle.TTMLBackgroundColor, want.background)
	checkStringValue(c, context+": font-weight", got.InlineStyle.TTMLFontWeight, want.weight)
	checkStringValue(c, context+": font-style", got.InlineStyle.TTMLFontStyle, want.style)
	checkStringValue(c, context+": text-decoration", got.InlineStyle.TTMLTextDecoration, want.decoration)
}

func checkColorValue(c *taskcheck.C, label string, got *astisub.Color, want string) {
	value := "<nil>"
	if got != nil {
		value = got.HTMLString()
	}
	checkOK(c, value == want, "%s = %q, want %q", label, value, want)
}

func checkStringValue(c *taskcheck.C, label string, got *string, want string) {
	value := "<nil>"
	if got != nil {
		value = *got
	}
	checkOK(c, value == want, "%s = %q, want %q", label, value, want)
}

func allCueSpans(s *astisub.Subtitles) []astisub.LineItem {
	var spans []astisub.LineItem
	if s == nil {
		return spans
	}
	for _, item := range s.Items {
		if item == nil {
			continue
		}
		for _, line := range item.Lines {
			spans = append(spans, line.Items...)
		}
	}
	return spans
}

func cueSpanTexts(spans []astisub.LineItem) []string {
	texts := make([]string, 0, len(spans))
	for _, span := range spans {
		texts = append(texts, span.Text)
	}
	return texts
}

func cueSpanWantFromLineItem(span astisub.LineItem) cueSpanWant {
	want := cueSpanWant{text: span.Text}
	if span.InlineStyle == nil {
		return want
	}
	if span.InlineStyle.TTMLColor != nil {
		want.color = span.InlineStyle.TTMLColor.HTMLString()
	}
	if span.InlineStyle.TTMLBackgroundColor != nil {
		want.background = span.InlineStyle.TTMLBackgroundColor.HTMLString()
	}
	if span.InlineStyle.TTMLFontWeight != nil {
		want.weight = *span.InlineStyle.TTMLFontWeight
	}
	if span.InlineStyle.TTMLFontStyle != nil {
		want.style = *span.InlineStyle.TTMLFontStyle
	}
	if span.InlineStyle.TTMLTextDecoration != nil {
		want.decoration = *span.InlineStyle.TTMLTextDecoration
	}
	return want
}

func assertStyleAttributesNotComputed(c *taskcheck.C, s *astisub.Subtitles, context string) {
	for itemIndex, item := range s.Items {
		for lineIndex, line := range item.Lines {
			for spanIndex, span := range line.Items {
				if span.InlineStyle == nil {
					continue
				}
				checkOK(c, span.InlineStyle.TTMLFontWeight == nil || span.InlineStyle.WebVTTBold,
					"%s: item %d line %d span %d received computed font-weight pointer", context, itemIndex, lineIndex, spanIndex)
				checkOK(c, span.InlineStyle.TTMLFontStyle == nil || span.InlineStyle.WebVTTItalics,
					"%s: item %d line %d span %d received computed font-style pointer", context, itemIndex, lineIndex, spanIndex)
				checkOK(c, span.InlineStyle.TTMLTextDecoration == nil || span.InlineStyle.WebVTTUnderline,
					"%s: item %d line %d span %d received computed text-decoration pointer", context, itemIndex, lineIndex, spanIndex)
			}
		}
	}
}
