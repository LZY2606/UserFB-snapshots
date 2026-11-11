package astisub_test

import (
	"bytes"
	"strings"
	"testing"
	"time"

	"github.com/asticode/go-astisub"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// ttmlTimingExpected is one expected static item produced from a timed TTML p.
type ttmlTimingExpected struct {
	begin time.Duration
	end   time.Duration
	text  string
}

func readTTMLTiming(t *testing.T, raw string) *astisub.Subtitles {
	t.Helper()
	s, err := astisub.ReadFromTTML(strings.NewReader(raw))
	require.NoError(t, err)
	return s
}

func requireTTMLTimingItems(t *testing.T, s *astisub.Subtitles, expected []ttmlTimingExpected) {
	t.Helper()
	require.Len(t, s.Items, len(expected))
	for i, want := range expected {
		assert.Equal(t, want.begin, s.Items[i].StartAt, "item %d start", i)
		assert.Equal(t, want.end, s.Items[i].EndAt, "item %d end", i)
		assert.Equal(t, want.text, s.Items[i].String(), "item %d text", i)
	}
}

// TestTTMLTimingTreeAncestorContainers exercises body/div/nested-div timing,
// unbounded containers, inheritance of a parent's end, and clipping of a
// descendant's dur by an ancestor.
func TestTTMLTimingTreeAncestorContainers(t *testing.T) {
	raw := `<tt xmlns="http://www.w3.org/ns/ttml" frameRate="25" tickRate="100">
	<body begin="5s" end="25s">
		<div begin="2s" end="20s">
			<div begin="1s"><p begin="1s" end="20s">A<span begin="4s" dur="10s">B</span>C</p></div>
		</div>
	</body>
	</tt>`
	s := readTTMLTiming(t, raw)
	requireTTMLTimingItems(t, s, []ttmlTimingExpected{
		{9 * time.Second, 13 * time.Second, "AC"},
		{13 * time.Second, 23 * time.Second, "ABC"},
		{23 * time.Second, 25 * time.Second, "AC"},
	})
}

// TestTTMLTimingTreeAncestorClipping covers the nested span example: C's 10s
// dur is clipped by its parent's end at 18s (not 23s).
func TestTTMLTimingTreeAncestorClipping(t *testing.T) {
	raw := `<tt xmlns="http://www.w3.org/ns/ttml"><body><div><p begin="10s" end="20s">A<span begin="2s" end="8s">B<span begin="1s" dur="10s">C</span>D</span>E</p></div></body></tt>`
	s := readTTMLTiming(t, raw)
	requireTTMLTimingItems(t, s, []ttmlTimingExpected{
		{10 * time.Second, 12 * time.Second, "AE"},
		{12 * time.Second, 13 * time.Second, "ABDE"},
		{13 * time.Second, 18 * time.Second, "ABCDE"},
		{18 * time.Second, 20 * time.Second, "AE"},
	})
}

// TestTTMLTimingTreeSiblingOverlap checks overlapping sibling spans and that
// overlapping source p elements stay independent and keep their source order.
func TestTTMLTimingTreeSiblingOverlap(t *testing.T) {
	raw := `<tt xmlns="http://www.w3.org/ns/ttml"><body><div>
		<p begin="0s" end="10s">A<span begin="1s" end="7s">B</span><span begin="4s" end="5s">C</span>D</p>
		<p begin="2s" end="4s">P2</p>
	</div></body></tt>`
	s := readTTMLTiming(t, raw)
	requireTTMLTimingItems(t, s, []ttmlTimingExpected{
		{0, 1 * time.Second, "AD"},
		{1 * time.Second, 4 * time.Second, "ABD"},
		{4 * time.Second, 5 * time.Second, "ABCD"},
		{5 * time.Second, 7 * time.Second, "ABD"},
		{7 * time.Second, 10 * time.Second, "AD"},
		// The second p starts earlier than the first p's fragments but is emitted
		// after them because p order is preserved.
		{2 * time.Second, 4 * time.Second, "P2"},
	})
	assert.Equal(t, 2*time.Second, s.Items[len(s.Items)-1].StartAt)
}

// TestTTMLTimingTreeNestedStyles checks inline style inheritance through
// body/div/p/span, named style references on spans, regions, and that a sibling
// cannot pollute another sibling's effective style.
func TestTTMLTimingTreeNestedStyles(t *testing.T) {
	raw := `<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling">
	<head>
		<styling><style xml:id="s1" tts:color="yellow" tts:fontWeight="bold"/></styling>
		<layout><region xml:id="r1"/></layout>
	</head>
	<body tts:color="red" tts:fontWeight="bold" region="r1">
		<div tts:color="green">
			<p begin="0s" end="4s"><span tts:color="blue">A</span>B<span style="s1">C<span>E</span></span><span>D</span></p>
		</div>
	</body>
</tt>`
	s := readTTMLTiming(t, raw)
	require.Len(t, s.Items, 1)
	item := s.Items[0]
	assert.Equal(t, "ABCED", item.String())
	require.NotNil(t, item.Region)
	assert.Equal(t, "r1", item.Region.ID)
	require.NotNil(t, item.InlineStyle)
	assert.Equal(t, astisub.ColorGreen, item.InlineStyle.TTMLColor)
	require.NotNil(t, item.InlineStyle.TTMLFontWeight)
	assert.Equal(t, "bold", *item.InlineStyle.TTMLFontWeight)

	items := item.Lines[0].Items
	require.Len(t, items, 5)

	// A: its own blue wins over the inherited green and stays bold.
	assert.Equal(t, astisub.ColorBlue, items[0].InlineStyle.TTMLColor)
	assert.Equal(t, "bold", *items[0].InlineStyle.TTMLFontWeight)
	assert.Nil(t, items[0].Style)

	// B/C inherit the div color and body weight; C keeps the single named style.
	assert.Equal(t, astisub.ColorGreen, items[1].InlineStyle.TTMLColor)
	assert.Nil(t, items[1].Style)
	assert.Equal(t, astisub.ColorGreen, items[2].InlineStyle.TTMLColor)
	require.NotNil(t, items[2].Style)
	assert.Equal(t, "s1", items[2].Style.ID)

	// E is a nested span without its own style, so it keeps the nearest span
	// reference s1.
	assert.Equal(t, astisub.ColorGreen, items[3].InlineStyle.TTMLColor)
	require.NotNil(t, items[3].Style)
	assert.Equal(t, "s1", items[3].Style.ID)

	// D must not see A's blue: siblings never mutate a shared parent style and
	// a p's own style is not copied onto line items.
	assert.Equal(t, astisub.ColorGreen, items[4].InlineStyle.TTMLColor)
	assert.Nil(t, items[4].Style)
}

// TestTTMLTimingTreeColorOnlySwitch ensures adjacent fragments that differ only
// in effective color are not merged even though their text is identical.
func TestTTMLTimingTreeColorOnlySwitch(t *testing.T) {
	raw := `<tt xmlns="http://www.w3.org/ns/ttml"><body><div><p begin="0s" end="6s"><span begin="0s" end="3s" color="red">X</span><span begin="3s" end="6s" color="blue">X</span></p></div></body></tt>`
	s := readTTMLTiming(t, raw)
	requireTTMLTimingItems(t, s, []ttmlTimingExpected{
		{0, 3 * time.Second, "X"},
		{3 * time.Second, 6 * time.Second, "X"},
	})
	require.Len(t, s.Items[0].Lines, 1)
	require.Len(t, s.Items[0].Lines[0].Items, 1)
	require.Len(t, s.Items[1].Lines, 1)
	require.Len(t, s.Items[1].Lines[0].Items, 1)
	assert.Equal(t, astisub.ColorRed, s.Items[0].Lines[0].Items[0].InlineStyle.TTMLColor)
	assert.Equal(t, astisub.ColorBlue, s.Items[1].Lines[0].Items[0].InlineStyle.TTMLColor)
}

// TestTTMLTimingTreeDurAndEnd checks that the earlier of dur and end wins, in
// both directions.
func TestTTMLTimingTreeDurAndEnd(t *testing.T) {
	raw := `<tt xmlns="http://www.w3.org/ns/ttml"><body><div>
		<p begin="0s" end="10s">A<span begin="1s" end="9s" dur="2s">B</span>C</p>
		<p begin="10s" end="20s">A<span begin="1s" end="3s" dur="8s">B</span>C</p>
	</div></body></tt>`
	s := readTTMLTiming(t, raw)
	requireTTMLTimingItems(t, s, []ttmlTimingExpected{
		// dur (1s + 2s = 3s) is earlier than end (9s).
		{0, 1 * time.Second, "AC"},
		{1 * time.Second, 3 * time.Second, "ABC"},
		{3 * time.Second, 10 * time.Second, "AC"},
		// end (10s + 3s = 13s) is earlier than dur (11s + 8s = 19s).
		{10 * time.Second, 11 * time.Second, "AC"},
		{11 * time.Second, 13 * time.Second, "ABC"},
		{13 * time.Second, 20 * time.Second, "AC"},
	})
}

// TestTTMLTimingTreeLineBreaksAndSpace ensures br line breaks survive per
// interval and xml:space="preserve" keeps significant whitespace.
func TestTTMLTimingTreeLineBreaksAndSpace(t *testing.T) {
	raw := `<tt xmlns="http://www.w3.org/ns/ttml"><body><div><p begin="0s" end="4s" xml:space="preserve"><span begin="0s" end="2s">A<br/>B</span> <span begin="2s" end="4s">C</span></p></div></body></tt>`
	s := readTTMLTiming(t, raw)
	requireTTMLTimingItems(t, s, []ttmlTimingExpected{
		{0, 2 * time.Second, "A - B "},
		{2 * time.Second, 4 * time.Second, " C"},
	})
}

// TestTTMLTimingTreeMixedTextSpace checks that documents using nested timing
// treat mixed text as xml:space="preserve", while legacy non-timed nested
// documents keep dropping insignificant whitespace between elements.
func TestTTMLTimingTreeMixedTextSpace(t *testing.T) {
	timed := `<tt xmlns="http://www.w3.org/ns/ttml"><body><div><p begin="0s" end="2s"><span begin="0s" end="1s">A</span> <span begin="1s" end="2s">B</span></p></div></body></tt>`
	s := readTTMLTiming(t, timed)
	requireTTMLTimingItems(t, s, []ttmlTimingExpected{
		{0, 1 * time.Second, "A "},
		{1 * time.Second, 2 * time.Second, " B"},
	})

	legacy := `<tt xmlns="http://www.w3.org/ns/ttml"><body><div><p begin="0s" end="2s"><span>A</span> <span>B</span></p></div></body></tt>`
	s = readTTMLTiming(t, legacy)
	requireTTMLTimingItems(t, s, []ttmlTimingExpected{
		{0, 2 * time.Second, "AB"},
	})
}

// TestTTMLTimingTreeErrors covers the unsupported/missing timing cases. Every
// error must mention the offending node.
func TestTTMLTimingTreeErrors(t *testing.T) {
	cases := []struct {
		name string
		raw  string
		part string
	}{
		{
			name: "missing frameRate",
			raw:  `<tt><body><div><p begin="0s" end="10s">A<span dur="2f">B</span></p></div></body></tt>`,
			part: "requires frameRate",
		},
		{
			name: "missing tickRate",
			raw:  `<tt><body><div><p begin="0s" end="10s">A<span dur="3t">B</span></p></div></body></tt>`,
			part: "requires tickRate",
		},
		{
			name: "negative duration",
			raw:  `<tt><body><div><p begin="0s" end="10s">A<span dur="-1s">B</span></p></div></body></tt>`,
			part: "negative",
		},
		{
			name: "inverted interval",
			raw:  `<tt><body><div><p begin="5s" end="3s">A</p></div></body></tt>`,
			part: "inverted",
		},
		{
			name: "sequential container",
			raw:  `<tt><body><div timeContainer="seq"><p begin="0s" end="3s">A</p></div></body></tt>`,
			part: "timeContainer",
		},
		{
			name: "repeat",
			raw:  `<tt><body><div><p begin="0s" end="3s" repeatCount="2">A</p></div></body></tt>`,
			part: "repeat",
		},
		{
			name: "smpte timed span",
			raw:  `<tt timeBase="smpte" frameRate="25"><body><div><p begin="0s" end="3s">A<span begin="1s">B</span></p></div></body></tt>`,
			part: "requires timeBase",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := astisub.ReadFromTTML(strings.NewReader(tc.raw))
			require.Error(t, err)
			assert.Contains(t, err.Error(), tc.part)
		})
	}
}

// TestTTMLTimingTreeWriteRoundTrip writes the static fragments to real TTML,
// SRT and WebVTT output and reads them back.
func TestTTMLTimingTreeWriteRoundTrip(t *testing.T) {
	raw := `<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling">
	<head><layout><region xml:id="r1"/></layout></head>
	<body region="r1"><div><p begin="10s" end="20s">A<span begin="2s" end="8s" tts:color="red" tts:fontWeight="bold">B<span begin="1s" dur="10s" tts:color="blue">C</span>D</span>E</p></div></body>
	</tt>`
	s := readTTMLTiming(t, raw)
	requireTTMLTimingItems(t, s, []ttmlTimingExpected{
		{10 * time.Second, 12 * time.Second, "AE"},
		{12 * time.Second, 13 * time.Second, "ABDE"},
		{13 * time.Second, 18 * time.Second, "ABCDE"},
		{18 * time.Second, 20 * time.Second, "AE"},
	})

	// TTML round trip: intervals, text, region and effective colors are kept.
	var ttml bytes.Buffer
	require.NoError(t, s.WriteToTTML(&ttml))
	reread, err := astisub.ReadFromTTML(bytes.NewReader(ttml.Bytes()))
	require.NoError(t, err)
	require.Len(t, reread.Items, len(s.Items))
	for i := range s.Items {
		assert.Equal(t, s.Items[i].StartAt, reread.Items[i].StartAt)
		assert.Equal(t, s.Items[i].EndAt, reread.Items[i].EndAt)
		assert.Equal(t, s.Items[i].String(), reread.Items[i].String())
		require.NotNil(t, reread.Items[i].Region)
		assert.Equal(t, "r1", reread.Items[i].Region.ID)
		require.Len(t, reread.Items[i].Lines, len(s.Items[i].Lines))
		for j := range s.Items[i].Lines {
			require.Len(t, reread.Items[i].Lines[j].Items, len(s.Items[i].Lines[j].Items))
			for k := range s.Items[i].Lines[j].Items {
				a := s.Items[i].Lines[j].Items[k]
				b := reread.Items[i].Lines[j].Items[k]
				assert.Equal(t, a.Text, b.Text)
				assert.Equal(t, a.InlineStyle.TTMLColor, b.InlineStyle.TTMLColor)
				assert.Equal(t, a.InlineStyle.TTMLFontWeight, b.InlineStyle.TTMLFontWeight)
			}
		}
	}

	// Plain-text exports keep one cue per static fragment instead of collapsing
	// the whole sentence into a single cue.
	var srt bytes.Buffer
	require.NoError(t, s.WriteToSRT(&srt))
	assert.Equal(t, len(s.Items), strings.Count(srt.String(), "-->"))

	var vtt bytes.Buffer
	require.NoError(t, s.WriteToWebVTT(&vtt))
	assert.Equal(t, len(s.Items), strings.Count(vtt.String(), "-->"))

	srtRead, err := astisub.ReadFromSRT(bytes.NewReader(srt.Bytes()))
	require.NoError(t, err)
	requireTTMLTimingItems(t, srtRead, []ttmlTimingExpected{
		{10 * time.Second, 12 * time.Second, "AE"},
		{12 * time.Second, 13 * time.Second, "ABDE"},
		{13 * time.Second, 18 * time.Second, "ABCDE"},
		{18 * time.Second, 20 * time.Second, "AE"},
	})
}
