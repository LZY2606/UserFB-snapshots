package pdf

import (
	"bytes"
	"fmt"
	"reflect"
	"strings"
	"testing"
	"unicode/utf16"
)

// docBuilder assembles a complete PDF file in memory: indirect objects,
// cross-reference table, page tree, fonts, content streams, and Form
// XObjects, without any external PDF tool.
type docBuilder struct {
	objs []string
	kids []int
}

func newDoc() *docBuilder {
	return &docBuilder{objs: []string{
		"<< /Type /Catalog /Pages 2 0 R >>",
		"", // Pages object, filled in by build
	}}
}

// obj adds an indirect object and returns its object number.
func (d *docBuilder) obj(body string) int {
	d.objs = append(d.objs, body)
	return len(d.objs)
}

// stream adds an indirect stream object and returns its object number.
func (d *docBuilder) stream(dict, data string) int {
	return d.obj(fmt.Sprintf("<< %s /Length %d >>\nstream\n%s\nendstream", dict, len(data), data))
}

// page adds a page with the given resource dictionary and content stream.
func (d *docBuilder) page(resources, content string) {
	stm := d.stream("", content)
	d.kids = append(d.kids, d.obj(fmt.Sprintf(
		"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Resources %s /Contents %d 0 R >>",
		resources, stm)))
}

// build assembles the final PDF file bytes.
func (d *docBuilder) build() []byte {
	var kids strings.Builder
	for _, k := range d.kids {
		fmt.Fprintf(&kids, "%d 0 R ", k)
	}
	d.objs[1] = fmt.Sprintf("<< /Type /Pages /Count %d /Kids [%s] >>", len(d.kids), kids.String())

	var b bytes.Buffer
	b.WriteString("%PDF-1.4\n")
	offset := []int{0}
	for i, o := range d.objs {
		offset = append(offset, b.Len())
		fmt.Fprintf(&b, "%d 0 obj\n%s\nendobj\n", i+1, o)
	}
	xref := b.Len()
	fmt.Fprintf(&b, "xref\n0 %d\n0000000000 65535 f \n", len(d.objs)+1)
	for _, o := range offset[1:] {
		fmt.Fprintf(&b, "%010d 00000 n \n", o)
	}
	fmt.Fprintf(&b, "trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n", len(d.objs)+1, xref)
	return b.Bytes()
}

// fontDef returns a simple Type1 font dictionary with fixed 600-unit widths.
func fontDef(base string) string {
	return fmt.Sprintf("<< /Type /Font /Subtype /Type1 /BaseFont /%s /Encoding /WinAnsiEncoding /FirstChar 32 /LastChar 126 /Widths [%s]>>",
		base, strings.Repeat("600 ", 95))
}

func openPage(t *testing.T, data []byte, num int) Page {
	t.Helper()
	r, err := NewReader(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		t.Fatal(err)
	}
	p := r.Page(num)
	if p.V.IsNull() {
		t.Fatalf("page %d not found", num)
	}
	return p
}

// utf16be encodes s as a UTF-16BE PDF text string with BOM.
func utf16be(s string) string {
	u := utf16.Encode([]rune(s))
	b := make([]byte, 2+2*len(u))
	b[0], b[1] = 0xfe, 0xff
	for i, c := range u {
		b[2+2*i] = byte(c >> 8)
		b[3+2*i] = byte(c)
	}
	return string(b)
}

// hexOf renders s as a PDF hex string body.
func hexOf(s string) string {
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		fmt.Fprintf(&b, "%02X", s[i])
	}
	return b.String()
}

// runTexts flattens the logical text of each run for comparison.
func runTexts(runs []LogicalRun) []string {
	var out []string
	for _, r := range runs {
		out = append(out, r.Text)
	}
	return out
}

// glyphTexts flattens the glyph text of a run for comparison.
func glyphTexts(r LogicalRun) string {
	var b strings.Builder
	for _, g := range r.Glyphs {
		b.WriteString(g.S)
	}
	return b.String()
}

func TestLogicalTextMarked(t *testing.T) {
	simpleDoc := func(content string) []byte {
		d := newDoc()
		font := d.obj(fontDef("Helvetica"))
		d.page(fmt.Sprintf("<< /Font << /F1 %d 0 R >> >>", font), content)
		return d.build()
	}

	t.Run("BasicActualText", func(t *testing.T) {
		p := openPage(t, simpleDoc("BT /F1 10 Tf 10 20 Td /Span << /ActualText (field) >> BDC (AB) Tj EMC (C) Tj ET"), 1)
		runs, err := p.LogicalText()
		if err != nil {
			t.Fatal(err)
		}
		if len(runs) != 2 {
			t.Fatalf("runs = %v, want 2 runs", runTexts(runs))
		}
		if runs[0].Text != "field" {
			t.Errorf("runs[0].Text = %q, want %q", runs[0].Text, "field")
		}
		if got := glyphTexts(runs[0]); got != "AB" {
			t.Errorf("runs[0] glyphs = %q, want %q", got, "AB")
		}
		// Glyph positions come from the real text operators, not the
		// replacement text: A at (10,20), B one 600-unit glyph later.
		g := runs[0].Glyphs
		if g[0].X != 10 || g[0].Y != 20 || g[1].X != 16 || g[1].Y != 20 {
			t.Errorf("glyph positions = (%g,%g) (%g,%g), want (10,20) (16,20)",
				g[0].X, g[0].Y, g[1].X, g[1].Y)
		}
		if runs[1].Text != "C" || len(runs[1].Glyphs) != 1 || runs[1].Glyphs[0].S != "C" {
			t.Errorf("runs[1] = %+v, want plain run C", runs[1])
		}
		// The legacy Content view still reports the drawn glyphs.
		var s strings.Builder
		for _, v := range p.Content().Text {
			s.WriteString(v.S)
		}
		if s.String() != "ABC" {
			t.Errorf("Content text = %q, want %q", s.String(), "ABC")
		}
	})

	t.Run("UTF16ActualText", func(t *testing.T) {
		want := "中文\U00020000" // BMP and non-BMP replacement text
		content := "BT /F1 10 Tf 0 0 Td /Span << /ActualText <" + hexOf(utf16be(want)) + "> >> BDC (xy) Tj EMC"
		p := openPage(t, simpleDoc(content), 1)
		runs, err := p.LogicalText()
		if err != nil {
			t.Fatal(err)
		}
		if len(runs) != 1 || runs[0].Text != want {
			t.Fatalf("runs = %q, want single run %q", runTexts(runs), want)
		}
		if got := glyphTexts(runs[0]); got != "xy" {
			t.Errorf("glyphs = %q, want %q", got, "xy")
		}
	})

	t.Run("PDFDocEncodingActualText", func(t *testing.T) {
		// 0x80 is U+2022 BULLET in PDFDocEncoding.
		p := openPage(t, simpleDoc("BT /F1 10 Tf 0 0 Td /Span << /ActualText <80> >> BDC (x) Tj EMC"), 1)
		runs, err := p.LogicalText()
		if err != nil {
			t.Fatal(err)
		}
		if len(runs) != 1 || runs[0].Text != "•" {
			t.Fatalf("runs = %q, want single run %q", runTexts(runs), "•")
		}
	})

	t.Run("EmptyActualText", func(t *testing.T) {
		// An empty replacement is valid and suppresses the drawn text.
		p := openPage(t, simpleDoc("BT /F1 10 Tf 0 0 Td /Span << /ActualText () >> BDC (AB) Tj EMC (C) Tj ET"), 1)
		runs, err := p.LogicalText()
		if err != nil {
			t.Fatal(err)
		}
		if len(runs) != 2 {
			t.Fatalf("runs = %q, want 2 runs", runTexts(runs))
		}
		if runs[0].Text != "" || glyphTexts(runs[0]) != "AB" {
			t.Errorf("runs[0] = %+v, want empty replacement over AB", runs[0])
		}
		if runs[1].Text != "C" {
			t.Errorf("runs[1].Text = %q, want %q", runs[1].Text, "C")
		}
	})

	t.Run("NestedActualText", func(t *testing.T) {
		// Only the outermost effective replacement emits a run.
		content := "BT /F1 10 Tf 0 0 Td " +
			"/Span << /ActualText (outer) >> BDC " +
			"/Span << /ActualText (inner) >> BDC (x) Tj EMC " +
			"(y) Tj EMC"
		p := openPage(t, simpleDoc(content), 1)
		runs, err := p.LogicalText()
		if err != nil {
			t.Fatal(err)
		}
		if len(runs) != 1 || runs[0].Text != "outer" {
			t.Fatalf("runs = %q, want single run %q", runTexts(runs), "outer")
		}
		if got := glyphTexts(runs[0]); got != "xy" {
			t.Errorf("glyphs = %q, want %q", got, "xy")
		}
	})

	t.Run("AcrossTextObjects", func(t *testing.T) {
		// A replacement range may span multiple BT/ET pairs.
		content := "/Span << /ActualText (both) >> BDC " +
			"BT /F1 10 Tf 0 0 Td (A) Tj ET " +
			"BT 0 20 Td (B) Tj ET EMC"
		p := openPage(t, simpleDoc(content), 1)
		runs, err := p.LogicalText()
		if err != nil {
			t.Fatal(err)
		}
		if len(runs) != 1 || runs[0].Text != "both" || glyphTexts(runs[0]) != "AB" {
			t.Fatalf("runs = %+v, want single run %q over AB", runs, "both")
		}
	})

	t.Run("AcrossGraphicsState", func(t *testing.T) {
		// q/Q must not save or pop the marked-content stack.
		content := "/Span << /ActualText (qq) >> BDC q " +
			"BT /F1 10 Tf 0 0 Td (A) Tj ET Q EMC"
		p := openPage(t, simpleDoc(content), 1)
		runs, err := p.LogicalText()
		if err != nil {
			t.Fatal(err)
		}
		if len(runs) != 1 || runs[0].Text != "qq" || glyphTexts(runs[0]) != "A" {
			t.Fatalf("runs = %+v, want single run %q over A", runs, "qq")
		}
	})

	t.Run("PropertyNameLookup", func(t *testing.T) {
		// The BDC property may be a name resolved through Resources/Properties.
		d := newDoc()
		font := d.obj(fontDef("Helvetica"))
		res := fmt.Sprintf("<< /Font << /F1 %d 0 R >> /Properties << /P1 << /ActualText (named) >> >> >>", font)
		d.page(res, "/Span /P1 BDC BT /F1 10 Tf 0 0 Td (Z) Tj ET EMC")
		p := openPage(t, d.build(), 1)
		runs, err := p.LogicalText()
		if err != nil {
			t.Fatal(err)
		}
		if len(runs) != 1 || runs[0].Text != "named" || glyphTexts(runs[0]) != "Z" {
			t.Fatalf("runs = %+v, want single run %q over Z", runs, "named")
		}
	})

	t.Run("PlainMarkedContent", func(t *testing.T) {
		// BMC/BDC without ActualText nest but do not swallow text.
		content := "/P BMC BT /F1 10 Tf 0 0 Td (A) Tj /Span << /MCID 0 >> BDC (B) Tj EMC EMC"
		p := openPage(t, simpleDoc(content), 1)
		runs, err := p.LogicalText()
		if err != nil {
			t.Fatal(err)
		}
		if got := runTexts(runs); !reflect.DeepEqual(got, []string{"A", "B"}) {
			t.Fatalf("runs = %q, want [A B]", got)
		}
	})

	t.Run("RepeatCalls", func(t *testing.T) {
		// Consecutive calls on the same page must not leak mark or
		// graphics state between runs.
		p := openPage(t, simpleDoc("BT /F1 10 Tf 0 0 Td /Span << /ActualText (x) >> BDC (A) Tj EMC (B) Tj ET"), 1)
		first, err := p.LogicalText()
		if err != nil {
			t.Fatal(err)
		}
		second, err := p.LogicalText()
		if err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(first, second) {
			t.Fatalf("repeated calls differ:\nfirst:  %+v\nsecond: %+v", first, second)
		}
	})
}

func TestLogicalTextForms(t *testing.T) {
	t.Run("MatrixAndRepeat", func(t *testing.T) {
		// The same form called twice produces independent glyphs at each
		// call site; the form's Matrix composes with the caller's CTM.
		d := newDoc()
		f1 := d.obj(fontDef("Helvetica"))
		f2 := d.obj(fontDef("Courier"))
		fm := d.stream(fmt.Sprintf("/Type /XObject /Subtype /Form /Matrix [1 0 0 1 100 0] /Resources << /Font << /F2 %d 0 R >> >>", f2),
			"BT /F2 12 Tf 0 0 Td (Q) Tj ET")
		d.page(fmt.Sprintf("<< /Font << /F1 %d 0 R >> /XObject << /Fm1 %d 0 R >> >>", f1, fm),
			"q 1 0 0 1 5 0 cm /Fm1 Do Q q 1 0 0 1 50 0 cm /Fm1 Do Q")
		p := openPage(t, d.build(), 1)
		runs, err := p.LogicalText()
		if err != nil {
			t.Fatal(err)
		}
		if len(runs) != 2 {
			t.Fatalf("runs = %q, want 2 runs", runTexts(runs))
		}
		for k, wantX := range []float64{105, 150} {
			g := runs[k].Glyphs
			if runs[k].Text != "Q" || len(g) != 1 {
				t.Fatalf("runs[%d] = %+v, want plain run Q", k, runs[k])
			}
			if g[0].X != wantX || g[0].Y != 0 {
				t.Errorf("runs[%d] glyph at (%g,%g), want (%g,0)", k, g[0].X, g[0].Y, wantX)
			}
			if g[0].Font != "Courier" || g[0].FontSize != 12 {
				t.Errorf("runs[%d] font = %s %g, want Courier 12", k, g[0].Font, g[0].FontSize)
			}
		}
	})

	t.Run("InheritResources", func(t *testing.T) {
		// A form without its own Resources uses the caller's.
		d := newDoc()
		f1 := d.obj(fontDef("Helvetica"))
		fm := d.stream("/Type /XObject /Subtype /Form", "BT /F1 10 Tf 3 4 Td (H) Tj ET")
		d.page(fmt.Sprintf("<< /Font << /F1 %d 0 R >> /XObject << /Fm1 %d 0 R >> >>", f1, fm),
			"/Fm1 Do")
		p := openPage(t, d.build(), 1)
		runs, err := p.LogicalText()
		if err != nil {
			t.Fatal(err)
		}
		if len(runs) != 1 || runs[0].Text != "H" {
			t.Fatalf("runs = %q, want single run H", runTexts(runs))
		}
		g := runs[0].Glyphs[0]
		if g.X != 3 || g.Y != 4 || g.Font != "Helvetica" {
			t.Errorf("glyph = %+v, want Helvetica at (3,4)", g)
		}
	})

	t.Run("OuterActualTextCoversForm", func(t *testing.T) {
		// An ActualText range opened by the caller covers all text drawn
		// by the form it invokes.
		d := newDoc()
		f1 := d.obj(fontDef("Helvetica"))
		fm := d.stream("/Type /XObject /Subtype /Form", "BT /F1 10 Tf 0 0 Td (XY) Tj ET")
		d.page(fmt.Sprintf("<< /Font << /F1 %d 0 R >> /XObject << /Fm1 %d 0 R >> >>", f1, fm),
			"/Span << /ActualText (covered) >> BDC /Fm1 Do EMC")
		p := openPage(t, d.build(), 1)
		runs, err := p.LogicalText()
		if err != nil {
			t.Fatal(err)
		}
		if len(runs) != 1 || runs[0].Text != "covered" {
			t.Fatalf("runs = %q, want single run %q", runTexts(runs), "covered")
		}
		if got := glyphTexts(runs[0]); got != "XY" {
			t.Errorf("glyphs = %q, want %q", got, "XY")
		}
	})

	t.Run("ResourceRestore", func(t *testing.T) {
		// The form's own Resources override the caller's during the call;
		// the caller's state is restored afterwards.
		d := newDoc()
		f1 := d.obj(fontDef("Helvetica"))
		f2 := d.obj(fontDef("Courier"))
		fm := d.stream(fmt.Sprintf("/Type /XObject /Subtype /Form /Resources << /Font << /F2 %d 0 R >> >>", f2),
			"BT /F2 10 Tf 0 0 Td (M) Tj ET")
		d.page(fmt.Sprintf("<< /Font << /F1 %d 0 R >> /XObject << /Fm1 %d 0 R >> >>", f1, fm),
			"BT /F1 10 Tf 0 0 Td (A) Tj ET /Fm1 Do BT 0 20 Td (B) Tj ET")
		p := openPage(t, d.build(), 1)
		runs, err := p.LogicalText()
		if err != nil {
			t.Fatal(err)
		}
		if got := runTexts(runs); !reflect.DeepEqual(got, []string{"A", "M", "B"}) {
			t.Fatalf("runs = %q, want [A M B]", got)
		}
		if got := runs[0].Glyphs[0].Font; got != "Helvetica" {
			t.Errorf("first glyph font = %s, want Helvetica", got)
		}
		if got := runs[1].Glyphs[0].Font; got != "Courier" {
			t.Errorf("form glyph font = %s, want Courier", got)
		}
		if g := runs[2].Glyphs[0]; g.Font != "Helvetica" || g.Y != 20 {
			t.Errorf("last glyph = %+v, want Helvetica at Y=20 (caller state restored)", g)
		}
	})
}

func TestLogicalTextErrors(t *testing.T) {
	fontRef := func(d *docBuilder) (int, string) {
		f := d.obj(fontDef("Helvetica"))
		return f, fmt.Sprintf("<< /Font << /F1 %d 0 R >> >>", f)
	}

	logicalErr := func(t *testing.T, data []byte) error {
		t.Helper()
		runs, err := openPage(t, data, 1).LogicalText()
		if err == nil {
			t.Fatalf("LogicalText succeeded with %q, want error", runTexts(runs))
		}
		return err
	}

	t.Run("RecursiveForm", func(t *testing.T) {
		d := newDoc()
		fmID := len(d.objs) + 1
		d.stream("/Type /XObject /Subtype /Form", "/Fm1 Do") // self-reference
		d.page(fmt.Sprintf("<< /XObject << /Fm1 %d 0 R >> >>", fmID), "/Fm1 Do")
		err := logicalErr(t, d.build())
		if !strings.Contains(err.Error(), "recursive") || !strings.Contains(err.Error(), "/Fm1") {
			t.Errorf("error = %v, want recursive Form XObject /Fm1", err)
		}
	})

	t.Run("IndirectRecursion", func(t *testing.T) {
		d := newDoc()
		fm1ID := len(d.objs) + 1
		fm2ID := fm1ID + 1
		d.stream("/Type /XObject /Subtype /Form", "/Fm2 Do")
		d.stream("/Type /XObject /Subtype /Form", "/Fm1 Do")
		d.page(fmt.Sprintf("<< /XObject << /Fm1 %d 0 R /Fm2 %d 0 R >> >>", fm1ID, fm2ID), "/Fm1 Do")
		err := logicalErr(t, d.build())
		if !strings.Contains(err.Error(), "recursive") {
			t.Errorf("error = %v, want recursion report", err)
		}
		// The call chain identifies both forms.
		if !strings.Contains(err.Error(), "/Fm1") || !strings.Contains(err.Error(), "/Fm2") {
			t.Errorf("error = %v, want call chain naming /Fm1 and /Fm2", err)
		}
	})

	t.Run("SiblingRepeatIsLegal", func(t *testing.T) {
		d := newDoc()
		_, res := fontRef(d)
		fm := d.stream("/Type /XObject /Subtype /Form", "BT /F1 10 Tf 0 0 Td (S) Tj ET")
		d.page(strings.TrimSuffix(res, " >>")+fmt.Sprintf(" /XObject << /Fm1 %d 0 R >> >>", fm),
			"/Fm1 Do /Fm1 Do")
		runs, err := openPage(t, d.build(), 1).LogicalText()
		if err != nil {
			t.Fatal(err)
		}
		if got := runTexts(runs); !reflect.DeepEqual(got, []string{"S", "S"}) {
			t.Fatalf("runs = %q, want [S S]", got)
		}
	})

	t.Run("StrayEMC", func(t *testing.T) {
		d := newDoc()
		_, res := fontRef(d)
		d.page(res, "BT /F1 10 Tf 0 0 Td (A) Tj ET EMC")
		err := logicalErr(t, d.build())
		if !strings.Contains(err.Error(), "EMC") {
			t.Errorf("error = %v, want EMC mismatch", err)
		}
	})

	t.Run("FormEMCCannotPopCaller", func(t *testing.T) {
		// An extra EMC inside a form must not close the caller's range.
		d := newDoc()
		_, res := fontRef(d)
		fm := d.stream("/Type /XObject /Subtype /Form", "EMC")
		d.page(strings.TrimSuffix(res, " >>")+fmt.Sprintf(" /XObject << /Fm1 %d 0 R >> >>", fm),
			"/Span << /ActualText (x) >> BDC /Fm1 Do EMC")
		err := logicalErr(t, d.build())
		if !strings.Contains(err.Error(), "EMC") || !strings.Contains(err.Error(), "/Fm1") {
			t.Errorf("error = %v, want EMC mismatch inside /Fm1", err)
		}
	})

	t.Run("UnclosedMarkedContent", func(t *testing.T) {
		d := newDoc()
		_, res := fontRef(d)
		d.page(res, "BT /F1 10 Tf 0 0 Td /Span << /ActualText (x) >> BDC (A) Tj ET")
		err := logicalErr(t, d.build())
		if !strings.Contains(err.Error(), "unclosed") || !strings.Contains(err.Error(), "/Span") {
			t.Errorf("error = %v, want unclosed /Span", err)
		}
	})

	t.Run("UnclosedMarkedContentInForm", func(t *testing.T) {
		d := newDoc()
		_, res := fontRef(d)
		fm := d.stream("/Type /XObject /Subtype /Form", "BT /F1 10 Tf 0 0 Td /P BMC (A) Tj ET")
		d.page(strings.TrimSuffix(res, " >>")+fmt.Sprintf(" /XObject << /Fm1 %d 0 R >> >>", fm), "/Fm1 Do")
		err := logicalErr(t, d.build())
		if !strings.Contains(err.Error(), "unclosed") || !strings.Contains(err.Error(), "/Fm1") {
			t.Errorf("error = %v, want unclosed marked content in /Fm1", err)
		}
	})

	t.Run("PropertyNotFound", func(t *testing.T) {
		d := newDoc()
		_, res := fontRef(d)
		d.page(res, "/Span /Nope BDC BT /F1 10 Tf 0 0 Td (A) Tj ET EMC")
		err := logicalErr(t, d.build())
		if !strings.Contains(err.Error(), "/Nope") {
			t.Errorf("error = %v, want missing property /Nope", err)
		}
	})

	t.Run("PropertyWrongType", func(t *testing.T) {
		d := newDoc()
		f := d.obj(fontDef("Helvetica"))
		res := fmt.Sprintf("<< /Font << /F1 %d 0 R >> /Properties << /P1 42 >> >>", f)
		d.page(res, "/Span /P1 BDC BT /F1 10 Tf 0 0 Td (A) Tj ET EMC")
		err := logicalErr(t, d.build())
		if !strings.Contains(err.Error(), "/P1") {
			t.Errorf("error = %v, want ill-typed property /P1", err)
		}
	})

	t.Run("ActualTextNotString", func(t *testing.T) {
		d := newDoc()
		_, res := fontRef(d)
		d.page(res, "BT /F1 10 Tf 0 0 Td /Span << /ActualText 42 >> BDC (A) Tj EMC ET")
		err := logicalErr(t, d.build())
		if !strings.Contains(err.Error(), "ActualText") {
			t.Errorf("error = %v, want non-string ActualText", err)
		}
	})

	t.Run("ActualTextBadUTF16", func(t *testing.T) {
		for _, hex := range []string{"FEFF4E", "FEFFD800", "FEFFDC00"} {
			d := newDoc()
			_, res := fontRef(d)
			d.page(res, "BT /F1 10 Tf 0 0 Td /Span << /ActualText <"+hex+"> >> BDC (A) Tj EMC ET")
			err := logicalErr(t, d.build())
			if !strings.Contains(err.Error(), "ActualText") {
				t.Errorf("hex %s: error = %v, want invalid ActualText", hex, err)
			}
		}
	})

	t.Run("BrokenFormStream", func(t *testing.T) {
		d := newDoc()
		_, res := fontRef(d)
		fm := d.stream("/Type /XObject /Subtype /Form", "<GG> Tj") // malformed hex string
		d.page(strings.TrimSuffix(res, " >>")+fmt.Sprintf(" /XObject << /Fm1 %d 0 R >> >>", fm), "/Fm1 Do")
		err := logicalErr(t, d.build())
		if !strings.Contains(err.Error(), "/Fm1") {
			t.Errorf("error = %v, want broken stream in /Fm1", err)
		}
	})

	t.Run("NoStateLeakBetweenPages", func(t *testing.T) {
		// After an error on one page, other pages and repeated calls
		// behave as if nothing happened.
		d := newDoc()
		_, res := fontRef(d)
		d.page(res, "BT /F1 10 Tf 0 0 Td (A) Tj ET EMC")
		d.page(res, "BT /F1 10 Tf 0 0 Td /Span << /ActualText (ok) >> BDC (B) Tj EMC ET")
		data := d.build()
		p1, p2 := openPage(t, data, 1), openPage(t, data, 2)

		if _, err := p1.LogicalText(); err == nil {
			t.Fatal("page 1 LogicalText succeeded, want error")
		}
		runs, err := p2.LogicalText()
		if err != nil {
			t.Fatalf("page 2 LogicalText after page 1 error: %v", err)
		}
		if got := runTexts(runs); !reflect.DeepEqual(got, []string{"ok"}) {
			t.Fatalf("page 2 runs = %q, want [ok]", got)
		}
		if _, err := p1.LogicalText(); err == nil {
			t.Fatal("page 1 LogicalText succeeded on second call, want error")
		}
		// The legacy Content view of the broken page is unaffected.
		var s strings.Builder
		for _, v := range p1.Content().Text {
			s.WriteString(v.S)
		}
		if s.String() != "A" {
			t.Errorf("page 1 Content text = %q, want %q", s.String(), "A")
		}
	})
}

// ExamplePage_LogicalText demonstrates consuming logical fields and
// their original glyph records.
func ExamplePage_LogicalText() {
	d := newDoc()
	font := d.obj(fontDef("Helvetica"))
	d.page(fmt.Sprintf("<< /Font << /F1 %d 0 R >> >>", font),
		"BT /F1 10 Tf 10 20 Td /Span << /ActualText (field) >> BDC (AB) Tj EMC ET")
	data := d.build()
	r, err := NewReader(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		panic(err)
	}
	runs, err := r.Page(1).LogicalText()
	if err != nil {
		panic(err)
	}
	for _, run := range runs {
		fmt.Printf("logical text %q drawn as %d glyph(s)\n", run.Text, len(run.Glyphs))
		for _, g := range run.Glyphs {
			fmt.Printf("  glyph %q at (%.0f, %.0f)\n", g.S, g.X, g.Y)
		}
	}
	// Output:
	// logical text "field" drawn as 2 glyph(s)
	//   glyph "A" at (10, 20)
	//   glyph "B" at (16, 20)
}
