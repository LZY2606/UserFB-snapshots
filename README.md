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

## WebVTT cue styles in TTML

`WriteToTTML` resolves the supported WebVTT `STYLE` subset from the current
`StyleAttributes.WebVTTStyles` each time it is called. Edits to STYLE lines or
to a run's `InlineStyle.WebVTTTags` are therefore reflected in the next export
without mutating the subtitles value.

```go
src := strings.NewReader(`WEBVTT

STYLE
::cue(.speaker) {
  color: #123456;
  background-color: white;
  font-weight: bold;
}
::cue(b) { font-style: italic; }

00:00:01.000 --> 00:00:03.000
<c.speaker>A <b>bold</b> cue</c>
`)
s, _ := astisub.ReadFromWebVTT(src)

// Edit the current STYLE slice in place or replace it.
style := s.Styles["astisub-webvtt-default-style-id"].InlineStyle
style.WebVTTStyles = []string{
    "::cue(.narrator) { color: #654321; font-weight: bold; }",
    "::cue(b) { font-style: italic; }",
}

// Or edit the complete tag stack carried by one run.
run := &s.Items[0].Lines[0].Items[0]
run.InlineStyle.WebVTTTags[0].Classes[0] = "narrator"

var ttml bytes.Buffer
_ = s.WriteToTTML(&ttml)

// The TTML can be read back span-by-span and/or saved as WebVTT again.
reread, _ := astisub.ReadFromTTML(bytes.NewReader(ttml.Bytes()))
var vtt bytes.Buffer
_ = reread.WriteToWebVTT(&vtt)
```

The supported subset is `::cue`, `::cue(c|b|i|u)`, `::cue(.class)`, and
compounds such as `::cue(c.a.b)`, comma grouping, whitespace, and `/* comments */`.
Supported properties are `color`, `background-color` (the eight WebVTT named
colors and `#RRGGBB`), `font-weight`, `font-style`, `text-decoration`, including
`inherit`, `initial`, ASCII case-insensitive keywords, and `!important`.
Unknown selectors/properties/values are ignored with CSS rule/declaration
recovery; inputs without a valid supported declaration keep the previous
conversion path.

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
