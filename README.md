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

# WebVTT STYLE cascade

WebVTT `STYLE` blocks are preserved on read and written back verbatim. When a
document's `STYLE` blocks contain at least one supported, valid declaration,
writing to TTML no longer keeps only the built-in color classes: the five
supported properties are re-cascaded from the current `STYLE` lines and each
run's current tag stack every time `WriteToTTML` runs. Editing
`StyleAttributes.WebVTTStyles` or a `LineItem`'s `WebVTTTags` is therefore
reflected by the very next export, without mutating the input objects.

The supported subset is:

- selectors: `::cue`, `::cue(c)`, `::cue(b)`, `::cue(i)`, `::cue(u)`,
  `::cue(.a)`, `::cue(c.a.b)`; comma groups, whitespace and `/* comments */`
  are allowed, class names are case-sensitive
- properties: `color` and `background-color` (the eight WebVTT named colors
  plus `#RRGGBB`), `font-weight` (`normal`/`bold`), `font-style`
  (`normal`/`italic`), `text-decoration` (`none`/`underline`), plus the
  `inherit` and `initial` keywords; declarations may carry `!important`
- cascade: per node, `!important` wins, then specificity (root 0, type 1,
  each class 10), then source order; unspecified properties inherit the
  parent node, starting from white text on a black background. Built-in
  `b`/`i`/`u` tags and color classes remain the lowest-priority defaults and
  can be switched off explicitly (e.g. `font-weight: normal`).

```go
input := `WEBVTT

STYLE
::cue { color: red; }
::cue(.brand) { color: #123456; font-weight: bold; }

00:00:01.000 --> 00:00:03.000
plain <c.brand>branded</c>
`
s, _ := astisub.ReadFromWebVTT(strings.NewReader(input))

// First export cascades the STYLE blocks into the TTML spans.
var buf bytes.Buffer
_ = s.WriteToTTML(&buf) // <span tts:color="#123456" tts:fontWeight="bold"...>

// Edit the rule in place: the next export uses the new value immediately.
for _, style := range s.Styles {
    if style.InlineStyle != nil && len(style.InlineStyle.WebVTTStyles) > 0 {
        style.InlineStyle.WebVTTStyles = []string{
            "::cue(.brand) { color: green; font-weight: normal; }",
        }
    }
}
buf.Reset()
_ = s.WriteToTTML(&buf) // the branded span is now green with normal weight
```

Documents without `STYLE` blocks, STYLE blocks containing no supported
declaration, and natively parsed TTML objects keep their previous behavior:
the cascade path is only enabled when a supported, valid declaration exists.

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
