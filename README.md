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

## TTML multi-reference styles

TTML allows a `style` attribute to reference several space-separated styles
(`style="bold blue"`), and a referenced style may itself reference several
styles. References are applied in order: a later reference overrides the
same-named attributes of an earlier one, and the node's own `tts:` attributes
win last.

- A **single** reference keeps the historical representation: the node's
  `Style` (on `Style`, `Region`, `Item` and `LineItem`) points at the declared
  style and, for head styles, the parent is available through `Style.Style`.
- **Multiple** references are exposed through the extension field
  `Style.StyleReferences` (`*TTMLStyleReferences`), whose `IDs()` and
  `Styles()` methods return the ordered references. The node's `Style` then
  points at a shared *synthetic* style whose `InlineStyle` holds the composed
  effective attributes (including the derived WebVTT layout fields), so the
  existing `InlineStyle`/`Style` consumers keep working unchanged. Synthetic
  styles are deterministic, shared by every node with the same reference
  list, and never emitted back to TTML.
- On write, the original reference lists are rendered verbatim as
  space-separated `style` attributes, so a write/read cycle preserves every
  node's effective TTML attributes without accumulating new styles. Missing
  references and reference cycles (a shared diamond ancestor is not a cycle)
  make `ReadFromTTML`/`OpenFile` fail with an error containing the style ID
  chain.

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
