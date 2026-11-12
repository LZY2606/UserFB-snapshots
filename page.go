// Copyright 2014 The Go Authors.  All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package pdf

import (
	"fmt"
	"strings"
)

// A Page represent a single page in a PDF file.
// The methods interpret a Page dictionary stored in V.
type Page struct {
	V Value
}

// Page returns the page for the given page number.
// Page numbers are indexed starting at 1, not 0.
// If the page is not found, Page returns a Page with p.V.IsNull().
func (r *Reader) Page(num int) Page {
	num-- // now 0-indexed
	page := r.Trailer().Key("Root").Key("Pages")
Search:
	for page.Key("Type").Name() == "Pages" {
		count := int(page.Key("Count").Int64())
		if count < num {
			return Page{}
		}
		kids := page.Key("Kids")
		for i := 0; i < kids.Len(); i++ {
			kid := kids.Index(i)
			if kid.Key("Type").Name() == "Pages" {
				c := int(kid.Key("Count").Int64())
				if num < c {
					page = kid
					continue Search
				}
				num -= c
				continue
			}
			if kid.Key("Type").Name() == "Page" {
				if num == 0 {
					return Page{kid}
				}
				num--
			}
		}
		break
	}
	return Page{}
}

// NumPage returns the number of pages in the PDF file.
func (r *Reader) NumPage() int {
	return int(r.Trailer().Key("Root").Key("Pages").Key("Count").Int64())
}

func (p Page) findInherited(key string) Value {
	for v := p.V; !v.IsNull(); v = v.Key("Parent") {
		if r := v.Key(key); !r.IsNull() {
			return r
		}
	}
	return Value{}
}

/*
func (p Page) MediaBox() Value {
	return p.findInherited("MediaBox")
}

func (p Page) CropBox() Value {
	return p.findInherited("CropBox")
}
*/

// Resources returns the resources dictionary associated with the page.
func (p Page) Resources() Value {
	return p.findInherited("Resources")
}

// Fonts returns a list of the fonts associated with the page.
func (p Page) Fonts() []string {
	return p.Resources().Key("Font").Keys()
}

// Font returns the font with the given name associated with the page.
func (p Page) Font(name string) Font {
	return Font{p.Resources().Key("Font").Key(name)}
}

// A Font represent a font in a PDF file.
// The methods interpret a Font dictionary stored in V.
type Font struct {
	V Value
}

// BaseFont returns the font's name (BaseFont property).
func (f Font) BaseFont() string {
	return f.V.Key("BaseFont").Name()
}

// FirstChar returns the code point of the first character in the font.
func (f Font) FirstChar() int {
	return int(f.V.Key("FirstChar").Int64())
}

// LastChar returns the code point of the last character in the font.
func (f Font) LastChar() int {
	return int(f.V.Key("LastChar").Int64())
}

// Widths returns the widths of the glyphs in the font.
// In a well-formed PDF, len(f.Widths()) == f.LastChar()+1 - f.FirstChar().
func (f Font) Widths() []float64 {
	x := f.V.Key("Widths")
	var out []float64
	for i := 0; i < x.Len(); i++ {
		out = append(out, x.Index(i).Float64())
	}
	return out
}

// Width returns the width of the given code point.
func (f Font) Width(code int) float64 {
	first := f.FirstChar()
	last := f.LastChar()
	if code < first || last < code {
		return 0
	}
	return f.V.Key("Widths").Index(code - first).Float64()
}

// Encoder returns the encoding between font code point sequences and UTF-8.
func (f Font) Encoder() TextEncoding {
	enc := f.V.Key("Encoding")
	switch enc.Kind() {
	case Name:
		switch enc.Name() {
		case "WinAnsiEncoding":
			return &byteEncoder{&winAnsiEncoding}
		case "MacRomanEncoding":
			return &byteEncoder{&macRomanEncoding}
		case "Identity-H":
			// TODO: Should be big-endian UCS-2 decoder
			return &nopEncoder{}
		default:
			println("unknown encoding", enc.Name())
			return &nopEncoder{}
		}
	case Dict:
		return &dictEncoder{enc.Key("Differences")}
	case Null:
		// ok, try ToUnicode
	default:
		println("unexpected encoding", enc.String())
		return &nopEncoder{}
	}

	toUnicode := f.V.Key("ToUnicode")
	if toUnicode.Kind() == Dict {
		m := readCmap(toUnicode)
		if m == nil {
			return &nopEncoder{}
		}
		return m
	}

	return &byteEncoder{&pdfDocEncoding}
}

type dictEncoder struct {
	v Value
}

func (e *dictEncoder) Decode(raw string) (text string) {
	r := make([]rune, 0, len(raw))
	for i := 0; i < len(raw); i++ {
		ch := rune(raw[i])
		n := -1
		for j := 0; j < e.v.Len(); j++ {
			x := e.v.Index(j)
			if x.Kind() == Integer {
				n = int(x.Int64())
				continue
			}
			if x.Kind() == Name {
				if int(raw[i]) == n {
					r := nameToRune[x.Name()]
					if r != 0 {
						ch = r
						break
					}
				}
				n++
			}
		}
		r = append(r, ch)
	}
	return string(r)
}

// A TextEncoding represents a mapping between
// font code points and UTF-8 text.
type TextEncoding interface {
	// Decode returns the UTF-8 text corresponding to
	// the sequence of code points in raw.
	Decode(raw string) (text string)
}

type nopEncoder struct {
}

func (e *nopEncoder) Decode(raw string) (text string) {
	return raw
}

type byteEncoder struct {
	table *[256]rune
}

func (e *byteEncoder) Decode(raw string) (text string) {
	r := make([]rune, 0, len(raw))
	for i := 0; i < len(raw); i++ {
		r = append(r, e.table[raw[i]])
	}
	return string(r)
}

type cmap struct {
	space   [4][][2]string
	bfrange []bfrange
}

func (m *cmap) Decode(raw string) (text string) {
	var r []rune
Parse:
	for len(raw) > 0 {
		for n := 1; n <= 4 && n <= len(raw); n++ {
			for _, space := range m.space[n-1] {
				if space[0] <= raw[:n] && raw[:n] <= space[1] {
					text := raw[:n]
					raw = raw[n:]
					for _, bf := range m.bfrange {
						if len(bf.lo) == n && bf.lo <= text && text <= bf.hi {
							if bf.dst.Kind() == String {
								s := bf.dst.RawString()
								if bf.lo != text {
									b := []byte(s)
									b[len(b)-1] += text[len(text)-1] - bf.lo[len(bf.lo)-1]
									s = string(b)
								}
								r = append(r, []rune(utf16Decode(s))...)
								continue Parse
							}
							if bf.dst.Kind() == Array {
								fmt.Printf("array %v\n", bf.dst)
							} else {
								fmt.Printf("unknown dst %v\n", bf.dst)
							}
							r = append(r, noRune)
							continue Parse
						}
					}
					fmt.Printf("no text for %q", text)
					r = append(r, noRune)
					continue Parse
				}
			}
		}
		println("no code space found")
		r = append(r, noRune)
		raw = raw[1:]
	}
	return string(r)
}

type bfrange struct {
	lo  string
	hi  string
	dst Value
}

func readCmap(toUnicode Value) *cmap {
	n := -1
	var m cmap
	ok := true
	Interpret(toUnicode, func(stk *Stack, op string) {
		if !ok {
			return
		}
		switch op {
		case "findresource":
			category := stk.Pop()
			key := stk.Pop()
			fmt.Println("findresource", key, category)
			stk.Push(newDict())
		case "begincmap":
			stk.Push(newDict())
		case "endcmap":
			stk.Pop()
		case "begincodespacerange":
			n = int(stk.Pop().Int64())
		case "endcodespacerange":
			if n < 0 {
				println("missing begincodespacerange")
				ok = false
				return
			}
			for i := 0; i < n; i++ {
				hi, lo := stk.Pop().RawString(), stk.Pop().RawString()
				if len(lo) == 0 || len(lo) != len(hi) {
					println("bad codespace range")
					ok = false
					return
				}
				m.space[len(lo)-1] = append(m.space[len(lo)-1], [2]string{lo, hi})
			}
			n = -1
		case "beginbfrange":
			n = int(stk.Pop().Int64())
		case "endbfrange":
			if n < 0 {
				panic("missing beginbfrange")
			}
			for i := 0; i < n; i++ {
				dst, srcHi, srcLo := stk.Pop(), stk.Pop().RawString(), stk.Pop().RawString()
				m.bfrange = append(m.bfrange, bfrange{srcLo, srcHi, dst})
			}
		case "defineresource":
			category := stk.Pop().Name()
			value := stk.Pop()
			key := stk.Pop().Name()
			fmt.Println("defineresource", key, value, category)
			stk.Push(value)
		default:
			println("interp\t", op)
		}
	})
	if !ok {
		return nil
	}
	return &m
}

type matrix [3][3]float64

var ident = matrix{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}

func (x matrix) mul(y matrix) matrix {
	var z matrix
	for i := 0; i < 3; i++ {
		for j := 0; j < 3; j++ {
			for k := 0; k < 3; k++ {
				z[i][j] += x[i][k] * y[k][j]
			}
		}
	}
	return z
}

// A Text represents a single piece of text drawn on a page.
type Text struct {
	Font     string  // the font used
	FontSize float64 // the font size, in points (1/72 of an inch)
	X        float64 // the X coordinate, in points, increasing left to right
	Y        float64 // the Y coordinate, in points, increasing bottom to top
	W        float64 // the width of the text, in points
	S        string  // the actual UTF-8 text
}

// A Rect represents a rectangle.
type Rect struct {
	Min, Max Point
}

// A Point represents an X, Y pair.
type Point struct {
	X float64
	Y float64
}

// Content describes the basic content on a page: the text and any drawn rectangles.
type Content struct {
	Text []Text
	Rect []Rect
}

type gstate struct {
	Tc    float64
	Tw    float64
	Th    float64
	Tl    float64
	Tf    Font
	Tfs   float64
	Tmode int
	Trise float64
	Tm    matrix
	Tlm   matrix
	Trm   matrix
	CTM   matrix
}

// Content returns the page's content.
func (p Page) Content() Content {
	i := newInterpreter(p)
	i.interpret(p.V.Key("Contents"))
	return Content{i.text, i.rect}
}

// A LogicalRun is a run of logical text collected from a page.
//
// Text is the logical text content of the run. For text enclosed in a
// marked content range carrying an ActualText property, Text is the
// decoded replacement string (which may be empty, suppressing the
// enclosed text); otherwise Text is the drawn glyph text.
//
// Glyphs records the original drawing records (as reported by
// Page.Content) covered by the run, in drawing order. For a replacement
// run these are the glyphs the ActualText string replaces; for an
// ordinary run it is the single glyph the run reproduces.
type LogicalRun struct {
	Text   string // logical (possibly replaced) text
	Glyphs []Text // original drawn glyphs covered by this run
}

// LogicalText returns the page's logical text content as a sequence of
// runs, in the order the text drawing operators appear in the content
// stream. It is intended for document collection and cleaning pipelines
// that cannot always read fields directly from the drawn glyphs.
//
// Unlike Content, LogicalText understands marked content: text enclosed
// in a marked content range (BDC ... EMC) carrying an ActualText
// property is reported as the decoded replacement string instead of the
// drawn glyphs, and Form XObjects referenced by the Do operator are
// executed so that text nested inside them is collected too. Glyph
// positions always come from the real text drawing operators (Tj, TJ,
// ', "); the replacement text never influences the text matrix.
//
// LogicalText does not infer reading order and does not lay out the page.
//
// A typical consumer extracts logical fields together with the positions
// of the original glyphs:
//
//	runs, err := page.LogicalText()
//	if err != nil {
//		log.Fatal(err)
//	}
//	for _, run := range runs {
//		fmt.Printf("field %q drawn as %d glyph(s)\n", run.Text, len(run.Glyphs))
//		for _, g := range run.Glyphs {
//			fmt.Printf("  glyph %q at (%g, %g) in %s %gpt\n",
//				g.S, g.X, g.Y, g.Font, g.FontSize)
//		}
//	}
//
// If the page content is malformed — recursive Form XObjects, mismatched
// BMC/BDC/EMC markers, missing or ill-typed property dictionaries,
// invalid ActualText strings, or broken Form streams — LogicalText
// returns a non-nil error describing the operator and call location, and
// no partial result.
func (p Page) LogicalText() (runs []LogicalRun, err error) {
	i := newInterpreter(p)
	i.logical = true
	defer func() {
		if r := recover(); r != nil {
			runs = nil
			err = fmt.Errorf("pdf: LogicalText: %v", r)
		}
	}()
	i.interpret(p.V.Key("Contents"))
	if len(i.marks) > 0 {
		var tags []string
		for _, m := range i.marks {
			tags = append(tags, "/"+m.tag)
		}
		return nil, fmt.Errorf("pdf: LogicalText: %s: unclosed marked content %s",
			i.ctx, strings.Join(tags, " "))
	}
	return i.runs, nil
}

// TextVertical implements sort.Interface for sorting
// a slice of Text values in vertical order, top to bottom,
// and then left to right within a line.
type TextVertical []Text

func (x TextVertical) Len() int      { return len(x) }
func (x TextVertical) Swap(i, j int) { x[i], x[j] = x[j], x[i] }
func (x TextVertical) Less(i, j int) bool {
	if x[i].Y != x[j].Y {
		return x[i].Y > x[j].Y
	}
	return x[i].X < x[j].X
}

// TextHorizontal implements sort.Interface for sorting
// a slice of Text values in horizontal order, left to right,
// and then top to bottom within a column.
type TextHorizontal []Text

func (x TextHorizontal) Len() int      { return len(x) }
func (x TextHorizontal) Swap(i, j int) { x[i], x[j] = x[j], x[i] }
func (x TextHorizontal) Less(i, j int) bool {
	if x[i].X != x[j].X {
		return x[i].X < x[j].X
	}
	return x[i].Y > x[j].Y
}

// An Outline is a tree describing the outline (also known as the table of contents)
// of a document.
type Outline struct {
	Title string    // title for this element
	Child []Outline // child elements
}

// Outline returns the document outline.
// The Outline returned is the root of the outline tree and typically has no Title itself.
// That is, the children of the returned root are the top-level entries in the outline.
func (r *Reader) Outline() Outline {
	return buildOutline(r.Trailer().Key("Root").Key("Outlines"))
}

func buildOutline(entry Value) Outline {
	var x Outline
	x.Title = entry.Key("Title").Text()
	for child := entry.Key("First"); child.Kind() == Dict; child = child.Key("Next") {
		x.Child = append(x.Child, buildOutline(child))
	}
	return x
}
