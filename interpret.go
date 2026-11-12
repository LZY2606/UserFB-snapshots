// Copyright 2014 The Go Authors.  All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Interpreting of PDF page content streams, shared by Page.Content
// and Page.LogicalText so the two views of a page cannot drift apart.

package pdf

import (
	"fmt"
	"strings"
	"unicode/utf16"
)

// A markedRange is an open marked-content range (BDC or BMC ... EMC)
// in the content stream being interpreted.
type markedRange struct {
	tag    string // marked content tag, without the leading slash
	actual bool   // the range carries an ActualText replacement
	text   string // decoded ActualText value (valid when actual is set)
	glyphs []Text // glyphs drawn inside the range
}

// A formFrame records one active Do (Form XObject) call.
type formFrame struct {
	name string // resource name used by the Do operator
	ptr  objptr // identity of the form's stream object
}

// A locatedError is a content interpretation failure that already
// carries the description of the stream it occurred in.
type locatedError struct {
	err error
}

func (e *locatedError) Error() string { return e.err.Error() }

// An interpreter walks a page or Form XObject content stream, evaluating
// the graphics and text operators. Page.Content uses it in glyph mode;
// Page.LogicalText uses it in logical mode, which additionally tracks
// marked content and executes Form XObjects.
type interpreter struct {
	logical bool // collect LogicalRun records; handle marked content and Do

	res  Value // current resource dictionary (page's or enclosing form's)
	g    gstate
	gstk []gstate
	enc  TextEncoding

	text []Text // glyph output (Content mode)
	rect []Rect

	runs  []LogicalRun  // logical output (LogicalText mode)
	marks []markedRange // open marked-content ranges

	chain    []formFrame // active Do calls, outermost first
	markBase []int       // len(marks) at entry of each active form
	ctx      string      // description of the stream being interpreted
}

func newInterpreter(p Page) *interpreter {
	return &interpreter{
		res: p.Resources(),
		g:   gstate{Th: 1, CTM: ident},
		enc: &nopEncoder{},
		ctx: "page contents",
	}
}

// interpret evaluates the operators in strm. Failures are reported as
// panics tagged with the location of strm; Page.LogicalText converts
// them to errors, while Page.Content keeps the historical panics.
func (i *interpreter) interpret(strm Value) {
	ctx := i.ctx
	defer func() {
		if r := recover(); r != nil {
			if _, ok := r.(*locatedError); ok {
				panic(r) // already carries the innermost location
			}
			panic(&locatedError{fmt.Errorf("%s: %v", ctx, r)})
		}
	}()
	Interpret(strm, func(stk *Stack, op string) {
		n := stk.Len()
		args := make([]Value, n)
		for k := n - 1; k >= 0; k-- {
			args[k] = stk.Pop()
		}
		i.op(op, args)
	})
}

// emit records one drawn glyph.
func (i *interpreter) emit(t Text) {
	if !i.logical {
		i.text = append(i.text, t)
		return
	}
	covered := false
	for k := range i.marks {
		if i.marks[k].actual {
			i.marks[k].glyphs = append(i.marks[k].glyphs, t)
			covered = true
		}
	}
	if !covered {
		i.runs = append(i.runs, LogicalRun{Text: t.S, Glyphs: []Text{t}})
	}
}

// insideActual reports whether any open marked-content range
// carries an ActualText replacement.
func (i *interpreter) insideActual() bool {
	for k := range i.marks {
		if i.marks[k].actual {
			return true
		}
	}
	return false
}

func (i *interpreter) showText(s string) {
	g := &i.g
	n := 0
	for _, ch := range i.enc.Decode(s) {
		Trm := matrix{{g.Tfs * g.Th, 0, 0}, {0, g.Tfs, 0}, {0, g.Trise, 1}}.mul(g.Tm).mul(g.CTM)
		w0 := g.Tf.Width(int(s[n]))
		n++
		if ch != ' ' {
			f := g.Tf.BaseFont()
			if j := strings.Index(f, "+"); j >= 0 {
				f = f[j+1:]
			}
			i.emit(Text{f, Trm[0][0], Trm[2][0], Trm[2][1], w0 / 1000 * Trm[0][0], string(ch)})
		}
		tx := w0/1000*g.Tfs + g.Tc
		if ch == ' ' {
			tx += g.Tw
		}
		tx *= g.Th
		g.Tm = matrix{{1, 0, 0}, {0, 1, 0}, {tx, 0, 1}}.mul(g.Tm)
	}
}

// doForm executes the Form XObject named by the Do operator.
// The caller's graphics state, resources, and marked-content ranges
// are preserved across the call; the form's own Matrix is composed
// with the current transformation matrix, and the form's own Resources
// take precedence over the caller's.
func (i *interpreter) doForm(name string) {
	xo := i.res.Key("XObject").Key(name)
	if xo.Kind() != Stream {
		panic(fmt.Errorf("Do /%s: XObject not found or not a stream", name))
	}
	if xo.Key("Subtype").Name() != "Form" {
		// Images and other XObjects contribute no text.
		return
	}
	ptr := xo.ptr
	if ptr != (objptr{}) {
		for _, f := range i.chain {
			if f.ptr == ptr {
				panic(fmt.Errorf("Do /%s: recursive Form XObject (object %d %d)", name, ptr.id, ptr.gen))
			}
		}
	}

	savedG, savedRes, savedEnc, savedCtx := i.g, i.res, i.enc, i.ctx
	markDepth := len(i.marks)
	i.chain = append(i.chain, formFrame{name, ptr})
	i.markBase = append(i.markBase, markDepth)
	i.ctx = fmt.Sprintf("%s, Do /%s", savedCtx, name)
	defer func() {
		i.g, i.res, i.enc, i.ctx = savedG, savedRes, savedEnc, savedCtx
		i.chain = i.chain[:len(i.chain)-1]
		i.markBase = i.markBase[:len(i.markBase)-1]
	}()

	m := ident
	if mx := xo.Key("Matrix"); mx.Kind() == Array && mx.Len() == 6 {
		for k := 0; k < 6; k++ {
			m[k/2][k%2] = mx.Index(k).Float64()
		}
	}
	i.g.CTM = m.mul(i.g.CTM)
	if res := xo.Key("Resources"); !res.IsNull() {
		i.res = res
	}
	i.interpret(xo)
	if len(i.marks) != markDepth {
		panic(fmt.Errorf("unclosed marked content in Form XObject /%s", name))
	}
}

func (i *interpreter) op(op string, args []Value) {
	g := &i.g
	switch op {
	default:
		//fmt.Println(op, args)
		return

	case "cm": // update g.CTM
		if len(args) != 6 {
			panic("bad g.Tm")
		}
		var m matrix
		for k := 0; k < 6; k++ {
			m[k/2][k%2] = args[k].Float64()
		}
		m[2][2] = 1
		g.CTM = m.mul(g.CTM)

	case "gs": // set parameters from graphics state resource
		gs := i.res.Key("ExtGState").Key(args[0].Name())
		font := gs.Key("Font")
		if font.Kind() == Array && font.Len() == 2 {
			//fmt.Println("FONT", font)
		}

	case "f": // fill
	case "g": // setgray
	case "l": // lineto
	case "m": // moveto

	case "cs": // set colorspace non-stroking
	case "scn": // set color non-stroking

	case "re": // append rectangle to path
		if len(args) != 4 {
			panic("bad re")
		}
		x, y, w, h := args[0].Float64(), args[1].Float64(), args[2].Float64(), args[3].Float64()
		i.rect = append(i.rect, Rect{Point{x, y}, Point{x + w, y + h}})

	case "q": // save graphics state (does not save marked content)
		i.gstk = append(i.gstk, i.g)

	case "Q": // restore graphics state (does not pop marked content)
		n := len(i.gstk) - 1
		i.g = i.gstk[n]
		i.gstk = i.gstk[:n]

	case "BT": // begin text (reset text matrix and line matrix)
		g.Tm = ident
		g.Tlm = g.Tm

	case "ET": // end text

	case "T*": // move to start of next line
		x := matrix{{1, 0, 0}, {0, 1, 0}, {0, -g.Tl, 1}}
		g.Tlm = x.mul(g.Tlm)
		g.Tm = g.Tlm

	case "Tc": // set character spacing
		if len(args) != 1 {
			panic("bad g.Tc")
		}
		g.Tc = args[0].Float64()

	case "TD": // move text position and set leading
		if len(args) != 2 {
			panic("bad Td")
		}
		g.Tl = -args[1].Float64()
		fallthrough
	case "Td": // move text position
		if len(args) != 2 {
			panic("bad Td")
		}
		tx := args[0].Float64()
		ty := args[1].Float64()
		x := matrix{{1, 0, 0}, {0, 1, 0}, {tx, ty, 1}}
		g.Tlm = x.mul(g.Tlm)
		g.Tm = g.Tlm

	case "Tf": // set text font and size
		if len(args) != 2 {
			panic("bad TL")
		}
		f := args[0].Name()
		g.Tf = Font{i.res.Key("Font").Key(f)}
		i.enc = g.Tf.Encoder()
		if i.enc == nil {
			println("no cmap for", f)
			i.enc = &nopEncoder{}
		}
		g.Tfs = args[1].Float64()

	case "\"": // set spacing, move to next line, and show text
		if len(args) != 3 {
			panic("bad \" operator")
		}
		g.Tw = args[0].Float64()
		g.Tc = args[1].Float64()
		args = args[2:]
		fallthrough
	case "'": // move to next line and show text
		if len(args) != 1 {
			panic("bad ' operator")
		}
		x := matrix{{1, 0, 0}, {0, 1, 0}, {0, -g.Tl, 1}}
		g.Tlm = x.mul(g.Tlm)
		g.Tm = g.Tlm
		fallthrough
	case "Tj": // show text
		if len(args) != 1 {
			panic("bad Tj operator")
		}
		i.showText(args[0].RawString())

	case "TJ": // show text, allowing individual glyph positioning
		v := args[0]
		for k := 0; k < v.Len(); k++ {
			x := v.Index(k)
			if x.Kind() == String {
				i.showText(x.RawString())
			} else {
				tx := -x.Float64() / 1000 * g.Tfs * g.Th
				g.Tm = matrix{{1, 0, 0}, {0, 1, 0}, {tx, 0, 1}}.mul(g.Tm)
			}
		}

	case "TL": // set text leading
		if len(args) != 1 {
			panic("bad TL")
		}
		g.Tl = args[0].Float64()

	case "Tm": // set text matrix and line matrix
		if len(args) != 6 {
			panic("bad g.Tm")
		}
		var m matrix
		for k := 0; k < 6; k++ {
			m[k/2][k%2] = args[k].Float64()
		}
		m[2][2] = 1
		g.Tm = m
		g.Tlm = m

	case "Tr": // set text rendering mode
		if len(args) != 1 {
			panic("bad Tr")
		}
		g.Tmode = int(args[0].Int64())

	case "Ts": // set text rise
		if len(args) != 1 {
			panic("bad Ts")
		}
		g.Trise = args[0].Float64()

	case "Tw": // set word spacing
		if len(args) != 1 {
			panic("bad g.Tw")
		}
		g.Tw = args[0].Float64()

	case "Tz": // set horizontal text scaling
		if len(args) != 1 {
			panic("bad Tz")
		}
		g.Th = args[0].Float64() / 100

	case "BMC": // begin marked-content range
		if !i.logical {
			return
		}
		if len(args) != 1 {
			panic("bad BMC")
		}
		i.marks = append(i.marks, markedRange{tag: args[0].Name()})

	case "BDC": // begin marked-content range with property list
		if !i.logical {
			return
		}
		if len(args) != 2 {
			panic("bad BDC")
		}
		rng := markedRange{tag: args[0].Name()}
		var props Value
		switch args[1].Kind() {
		case Dict:
			props = args[1]
		case Name:
			props = i.res.Key("Properties").Key(args[1].Name())
			if props.Kind() != Dict {
				panic(fmt.Errorf("BDC /%s: property list /%s not found or not a dictionary",
					rng.tag, args[1].Name()))
			}
		default:
			panic(fmt.Errorf("BDC /%s: property list is %v, want dictionary or name",
				rng.tag, args[1].Kind()))
		}
		if at := props.Key("ActualText"); !at.IsNull() {
			if at.Kind() != String {
				panic(fmt.Errorf("BDC /%s: ActualText is not a string", rng.tag))
			}
			s, err := decodeActualText(at.RawString())
			if err != nil {
				panic(fmt.Errorf("BDC /%s: invalid ActualText: %v", rng.tag, err))
			}
			rng.actual = true
			rng.text = s
		}
		i.marks = append(i.marks, rng)

	case "EMC": // end marked-content range
		if !i.logical {
			return
		}
		base := 0
		if n := len(i.markBase); n > 0 {
			base = i.markBase[n-1]
		}
		if len(i.marks) <= base {
			panic("EMC without open marked content")
		}
		rng := i.marks[len(i.marks)-1]
		i.marks = i.marks[:len(i.marks)-1]
		// Only the outermost effective ActualText replacement emits a
		// run; replacements nested inside it are discarded, and their
		// glyphs have already been collected by the outer range.
		if rng.actual && !i.insideActual() {
			i.runs = append(i.runs, LogicalRun{Text: rng.text, Glyphs: rng.glyphs})
		}

	case "Do": // invoke named XObject
		if !i.logical {
			return
		}
		if len(args) != 1 {
			panic("bad Do")
		}
		i.doForm(args[0].Name())
	}
}

// decodeActualText decodes an ActualText string following the PDF text
// string rules: a leading FEFF byte order mark selects big-endian
// UTF-16, otherwise the bytes are PDFDocEncoding.
func decodeActualText(raw string) (string, error) {
	if len(raw) >= 2 && raw[0] == 0xfe && raw[1] == 0xff {
		if len(raw)%2 != 0 {
			return "", fmt.Errorf("UTF-16BE string has odd length %d", len(raw))
		}
		u := make([]uint16, 0, (len(raw)-2)/2)
		for k := 2; k < len(raw); k += 2 {
			u = append(u, uint16(raw[k])<<8|uint16(raw[k+1]))
		}
		for k := 0; k < len(u); k++ {
			switch {
			case 0xd800 <= u[k] && u[k] < 0xdc00:
				if k+1 >= len(u) || u[k+1] < 0xdc00 || u[k+1] >= 0xe000 {
					return "", fmt.Errorf("unpaired high surrogate U+%04X", u[k])
				}
				k++
			case 0xdc00 <= u[k] && u[k] < 0xe000:
				return "", fmt.Errorf("unpaired low surrogate U+%04X", u[k])
			}
		}
		return string(utf16.Decode(u)), nil
	}
	for k := 0; k < len(raw); k++ {
		if pdfDocEncoding[raw[k]] == noRune {
			return "", fmt.Errorf("byte 0x%02x is not representable in PDFDocEncoding", raw[k])
		}
	}
	return pdfDocDecode(raw), nil
}
