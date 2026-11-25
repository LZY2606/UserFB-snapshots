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

# Styling WebVTT cues when exporting to TTML

WebVTT `STYLE` blocks are preserved on `StyleAttributes.WebVTTStyles`. When you
call `WriteToTTML`, a limited CSS subset is cascaded over each run's current
`WebVTTTags` stack and written onto the resulting TTML spans.

Supported selectors are `::cue` (the cue root), `::cue(c)`, `::cue(b)`,
`::cue(i)`, `::cue(u)`, `::cue(.name)` and `::cue(c.name.other)`, with comma
groups, whitespace and `/* comments */`. Class names start with an ASCII letter
or `_`, may then contain letters, digits, `_` and `-`, and are case sensitive.
Supported properties are `color` and `background-color` (the eight WebVTT named
colors plus `#RRGGBB`), `font-weight` (`normal`/`bold`), `font-style`
(`normal`/`italic`) and `text-decoration` (`none`/`underline`), including the
`inherit` and `initial` keywords and `!important`. Unknown selectors,
properties or invalid values are skipped to the end of the current rule or
declaration. Specificity is `0` for the root, `1` per type and `10` per class;
`!important`, specificity and occurrence order decide same-node winners, and
the five properties inherit (initial values are white text on black background,
normal weight/style and no underline). `b`/`i`/`u` and the built-in color
classes stay as lowest-priority defaults that CSS can switch off.

The cascade is recomputed on every export from the current `WebVTTStyles` and
`WebVTTTags`, so editing either after reading is reflected in the next export.
Exporting never mutates the subtitles. Inputs without a supported valid
declaration, as well as native TTML objects, keep the previous behavior.

```go
src := `WEBVTT

STYLE
::cue(.brand) { color: #123456; font-weight: bold; }

00:00:01.000 --> 00:00:03.000
<c.brand>Hello</c><c.red> red</c>
`
s, _ := astisub.ReadFromWebVTT(strings.NewReader(src))

var ttml bytes.Buffer
_ = s.WriteToTTML(&ttml)
// <span tts:color="#123456" tts:fontWeight="bold" ...>Hello</span>
// <span tts:color="#ff0000" ...> red</span>

// Edit the STYLE content (or a LineItem's WebVTTTags), then export again.
var styleSA *astisub.StyleAttributes
for _, st := range s.Styles {
    if st.InlineStyle != nil && len(st.InlineStyle.WebVTTStyles) > 0 {
        styleSA = st.InlineStyle
    }
}
styleSA.WebVTTStyles = []string{
    "::cue(.brand) { color: #abcdef; font-style: italic; }",
}
ttml.Reset()
_ = s.WriteToTTML(&ttml) // now #abcdef / italic / normal weight
```

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
