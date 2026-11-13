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

# Ruby annotations (WebVTT / TTML)

Ruby annotations (base text plus its reading) are modelled in the public
`Subtitles` tree, not as raw markup, so they can be edited and written back to
either format.

A ruby is a `LineItem` whose `Ruby` field is set. Its `Text` field is unused;
`Line.String()` and plain-text outputs such as SRT render the base text only.

```go
// Create a ruby with two pairs: 東京/とうきょう and 駅/えき.
tokyo := &astisub.Ruby{Pairs: []astisub.RubyPair{
    {
        Base: []astisub.LineItem{
            {Text: "東", InlineStyle: &astisub.StyleAttributes{TTMLColor: astisub.ColorGreen}},
            {Text: "京", InlineStyle: &astisub.StyleAttributes{TTMLColor: astisub.ColorBlue}},
        },
        Text: []astisub.LineItem{{Text: "とうきょう"}},
    },
    {
        Base: []astisub.LineItem{{Text: "駅"}},
        Text: []astisub.LineItem{{Text: "えき"}},
    },
}}

s := astisub.NewSubtitles()
s.Items = append(s.Items, &astisub.Item{
    StartAt: time.Second,
    EndAt:   3 * time.Second,
    Lines:   []astisub.Line{{Items: []astisub.LineItem{{Ruby: tokyo}}}},
})

// Edit the annotation and one base run's color; both writers pick this up.
tokyo.Pairs[0].Text[0].Text = "Tokyo"
tokyo.Pairs[0].Base[0].InlineStyle.TTMLColor = astisub.ColorGreen

// Write to either format; Line.String() still gives "東京駅".
s.WriteToTTML(w)
s.WriteToWebVTT(w)
```

Each `RubyPair` has a `Base` and a `Text` side, and each side is a list of
styled runs so a side can be split by color (or left colorless). The number of
pairs is what matters: splitting a side never adds or removes a pair.

Reading behaves as follows:

- WebVTT: `<ruby>` groups one or more base/`<rt>` pairs:
  `<ruby><c.red>東</c><c.blue>京</c><rt>とうきょう</rt>駅<rt>えき</rt></ruby>`
  is two pairs. Adjacent ruby annotations and normal text around them keep
  their order.
- TTML: `tts:ruby="container"` holds either a simple structure (one `base`
  then one `text`) or a grouped one (`baseContainer` and `textContainer` with
  one `base`/`text` per pair, matched by position). Inline `tts:color`
  inherits from outer spans and is overridden by inner ones; splitting a side
  into several colored spans never changes the pair count. Text spaces inside
  `xml:space="preserve"` are kept, as are `br` breaks between groups.

Writing normalizes one pair to a simple container and several pairs to a
grouped container, so pairs are never interleaved under a simple container.

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
