[![GoReportCard](http://goreportcard.com/badge/github.com/asticode/go-astisub)](http://goreportcard.com/report/github.com/asticode/go-astisub)
[![GoDoc](https://godoc.org/github.com/asticode/go-astisub?status.svg)](https://godoc.org/github.com/asticode/go-astisub)
[![Test](https://github.com/asticode/go-astisub/actions/workflows/test.yml/badge.svg)](https://github.com/asticode/go-astisub/actions/workflows/test.yml)
[![Coveralls](https://coveralls.io/repos/github/asticode/go-astisub/badge.svg?branch=master)](https://coveralls.io/github/asticode/go-astisub)

This is a Golang library to manipulate subtitles. 

It allows you to manipulate `srt`, `stl`, `ttml`, `ssa/ass`, `webvtt` and `teletext` files for now.

Available operations are `parsing`, `writing`, `applying linear correction`, `syncing`, `fragmenting`, `unfragmenting`, `merging` and `optimizing`.

# Installation

To install the library:

    go get github.com/asticode/go-astisub

To install the CLI:

    go install github.com/asticode/go-astisub/cmd/astisub        

# Using the library in your code

WARNING: the code below doesn't handle errors for readibility purposes. However you SHOULD!

```go
// Open subtitles
s1, _ := astisub.OpenFile("/path/to/example.ttml")
s2, _ := astisub.ReadFromSRT(bytes.NewReader([]byte("1\n00:01:00.000 --> 00:02:00.000\nCredits")))

// Add a duration to every subtitles (syncing)
s1.Add(-2*time.Second)

// Fragment the subtitles
s1.Fragment(2*time.Second)

// Merge subtitles
s1.Merge(s2)

// Optimize subtitles
s1.Optimize()

// Unfragment the subtitles
s1.Unfragment()

// Apply linear correction
s1.ApplyLinearCorrection(1*time.Second, 2*time.Second, 5*time.Second, 7*time.Second)

// Write subtitles
s1.Write("/path/to/example.srt")
var buf = &bytes.Buffer{}
s2.WriteToTTML(buf)
```

# Ruby (furigana) annotations

WebVTT `<ruby>`/`<rt>` and TTML `tts:ruby` are read and written through the
regular `Line`/`LineItem` model. A pair is expressed by one `*Ruby` value shared
by every base `LineItem` of that pair: their `Text` fields, in order, are the
base text, and `Ruby.Text` holds the annotation runs. Base text lives in
`LineItem.Text` and `LineItem.InlineStyle`, so `Line.String()`, SRT and the
plain text of other formats never include the annotation.

```go
// Create a pair directly, without parsing anything.
r := &astisub.Ruby{Text: []astisub.LineItem{{Text: "とうきょう"}}}
sub := astisub.NewSubtitles()
sub.Items = []*astisub.Item{{
    StartAt: time.Second,
    EndAt:   3 * time.Second,
    Lines: []astisub.Line{
        {Items: []astisub.LineItem{
            {Text: "東", Ruby: r, InlineStyle: &astisub.StyleAttributes{TTMLColor: astisub.ColorGreen}},
            {Text: "京", Ruby: r},
        }},
    },
}}

// Edit an existing pair: the reading and each run's color are normal fields.
li := &sub.Items[0].Lines[0].Items[0]
li.Ruby.Text = []astisub.LineItem{{Text: "Tokyo"}}
li.InlineStyle.TTMLColor = astisub.ColorGreen

// Both writers export the edits; both readers rebuild the same pairs.
sub.WriteToTTML(w)
sub.WriteToWebVTT(w)
```

WebVTT ruby with several colored runs per side, e.g.
`<ruby><c.red>東</c><c.blue>京</c><rt>とうきょう</rt>駅<rt>えき</rt></ruby>`,
is read as two pairs and written back the same way. TTML supports both a simple
`container` holding one `base` and one `text`, and a grouped `container` with a
`baseContainer` and a `textContainer` whose children pair by position. Inline
`tts:color` inherits from outer to inner; when a pair has several runs they are
written nested inside one `base`/`text` so the run boundaries and colors
survive. Writers emit simple containers only.

Not covered: double-sided ruby, ruby delimiters, line breaks inside a pair,
timed spans and style references inside ruby.

# Using the CLI

If **astisub** has been installed properly you can:

- convert any type of subtitle to any other type of subtitle:

        astisub convert -i example.srt -o example.ttml

- apply linear correction to any type of subtitle:

        astisub apply-linear-correction -i example.srt -a1 1s -d1 2s -a2 5s -d2 7s -o example.out.srt

- fragment any type of subtitle:

        astisub fragment -i example.srt -f 2s -o example.out.srt

- merge any type of subtitle into any other type of subtitle:

        astisub merge -i example.srt -i example.ttml -o example.out.srt

- optimize any type of subtitle:

        astisub optimize -i example.srt -o example.out.srt

- unfragment any type of subtitle:

        astisub unfragment -i example.srt -o example.out.srt

- sync any type of subtitle:

        astisub sync -i example.srt -s "-2s" -o example.out.srt

# Features and roadmap

- [x] parsing
- [x] writing
- [x] syncing
- [x] fragmenting/unfragmenting
- [x] merging
- [x] ordering
- [x] optimizing
- [x] linear correction
- [x] .srt
- [x] .ttml
- [x] .vtt
- [x] .stl
- [x] .ssa/.ass
- [x] .teletext
- [ ] .smi
