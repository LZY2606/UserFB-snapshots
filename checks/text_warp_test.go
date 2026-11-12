package main

import (
	"image"
	"image/color"
	"math"
	"os"
	"path/filepath"
	"testing"

	"github.com/tdewolff/canvas"
	"github.com/tdewolff/canvas/renderers/svg"
)

// renderRecord is one RenderPath call captured by the recording renderer.
type renderRecord struct {
	path  *canvas.Path
	style canvas.Style
	m     canvas.Matrix
}

// recordRenderer collects warped paths without rasterizing anything.
type recordRenderer struct {
	records []renderRecord
}

func (r *recordRenderer) Size() (float64, float64) { return 300.0, 300.0 }
func (r *recordRenderer) RenderPath(p *canvas.Path, style canvas.Style, m canvas.Matrix) {
	r.records = append(r.records, renderRecord{p.Copy(), style, m})
}
func (r *recordRenderer) RenderText(*canvas.Text, canvas.Matrix) {}
func (r *recordRenderer) RenderImage(image.Image, canvas.Matrix) {}

func warpTestFont(t *testing.T) *canvas.FontFamily {
	t.Helper()
	family := canvas.NewFontFamily("dejavu-serif-warp")
	for _, path := range []string{"resources/DejaVuSerif.ttf", "../resources/DejaVuSerif.ttf"} {
		if err := family.LoadFontFile(path, canvas.FontRegular); err == nil {
			return family
		}
	}
	t.Fatalf("load font: resources/DejaVuSerif.ttf not found")
	return nil
}

// txtLines extracts the spans of the single line via WalkLines.
func txtLines(txt *canvas.Text) []canvas.TextSpan {
	var spans []canvas.TextSpan
	txt.WalkLines(func(_ float64, s []canvas.TextSpan) {
		spans = s
	})
	return spans
}

func quadPos(p0, p1, p2 canvas.Point, t float64) canvas.Point {
	mt := 1.0 - t
	return canvas.Point{
		X: mt*mt*p0.X + 2*mt*t*p1.X + t*t*p2.X,
		Y: mt*mt*p0.Y + 2*mt*t*p1.Y + t*t*p2.Y,
	}
}

func cubePos(p0, p1, p2, p3 canvas.Point, t float64) canvas.Point {
	mt := 1.0 - t
	return canvas.Point{
		X: mt*mt*mt*p0.X + 3*mt*mt*t*p1.X + 3*mt*t*t*p2.X + t*t*t*p3.X,
		Y: mt*mt*mt*p0.Y + 3*mt*mt*t*p1.Y + 3*mt*t*t*p2.Y + t*t*t*p3.Y,
	}
}

// arcPos samples an elliptical arc through its flattened geometry, so the
// expectation stays independent of the warp implementation.
func arcPos(start, end canvas.Point, rx, ry, rotDeg float64, large, sweep bool, t float64) canvas.Point {
	p := &canvas.Path{}
	p.MoveTo(start.X, start.Y)
	p.ArcTo(rx, ry, rotDeg, large, sweep, end.X, end.Y)
	coords := p.Flatten(0.001).Coords()
	if len(coords) < 2 {
		return start
	}
	f := t * float64(len(coords)-1)
	i := int(math.Min(f, float64(len(coords)-2)))
	return coords[i].Interpolate(coords[i+1], f-float64(i))
}

// sampleSourcePath evaluates every segment of p at uniform parameters, returning
// dense glyph-space sample chains (one per subpath) translated by (tx,ty).
func sampleSourcePath(p *canvas.Path, tx, ty, steps float64) [][]canvas.Point {
	chains := [][]canvas.Point{}
	chain := []canvas.Point{}
	scanner := p.Scanner()
	flush := func() {
		if len(chain) > 1 {
			chains = append(chains, chain)
		}
		chain = []canvas.Point{}
	}
	for scanner.Scan() {
		cmd := scanner.Cmd()
		start := scanner.Start()
		end := scanner.End()
		var pos func(float64) canvas.Point
		switch cmd {
		case canvas.MoveToCmd:
			flush()
			chain = append(chain, canvas.Point{X: end.X + tx, Y: end.Y + ty})
			continue
		case canvas.LineToCmd, canvas.CloseCmd:
			pos = func(u float64) canvas.Point { return start.Interpolate(end, u) }
		case canvas.QuadToCmd:
			cp := scanner.CP1()
			pos = func(u float64) canvas.Point { return quadPos(start, cp, end, u) }
		case canvas.CubeToCmd:
			cp1, cp2 := scanner.CP1(), scanner.CP2()
			pos = func(u float64) canvas.Point { return cubePos(start, cp1, cp2, end, u) }
		case canvas.ArcToCmd:
			rx, ry, rotDeg, large, sweep := scanner.Arc()
			pos = func(u float64) canvas.Point {
				return arcPos(start, end, rx, ry, rotDeg, large, sweep, u)
			}
		default:
			continue
		}
		n := int(steps)
		for i := 1; i <= n; i++ {
			q := pos(float64(i) / steps)
			chain = append(chain, canvas.Point{X: q.X + tx, Y: q.Y + ty})
		}
	}
	flush()
	return chains
}

// distToChains is the minimum distance from q to any segment of the sample chains.
func distToChains(q canvas.Point, chains [][]canvas.Point) float64 {
	d := math.Inf(1)
	for _, chain := range chains {
		if dd := distToPolyline(q, chain); dd < d {
			d = dd
		}
	}
	return d
}

// hausdorff returns the symmetric (two-sided) maximum distance between dense
// expected chains and the rendered polyline.
func hausdorff(expected [][]canvas.Point, rendered []canvas.Point) float64 {
	h := 0.0
	for _, chain := range expected {
		for _, q := range chain {
			if d := distToPolyline(q, rendered); d > h {
				h = d
			}
		}
	}
	for _, q := range rendered {
		if d := distToChains(q, expected); d > h {
			h = d
		}
	}
	return h
}

// recordCoords returns the rendered coordinates after applying each record's
// transformation matrix, so distances are measured in post-transform units.
func recordCoords(recs []renderRecord) []canvas.Point {
	coords := []canvas.Point{}
	for _, rec := range recs {
		for _, q := range rec.path.Coords() {
			coords = append(coords, rec.m.Dot(q))
		}
	}
	return coords
}

// distPointSegment is the distance from q to segment ab.
func distPointSegment(q, a, b canvas.Point) float64 {
	ab := b.Sub(a)
	l := ab.Dot(ab)
	if l == 0.0 {
		return q.Sub(a).Length()
	}
	u := math.Min(1.0, math.Max(0.0, q.Sub(a).Dot(ab)/l))
	return q.Sub(a.Interpolate(b, u)).Length()
}

func distToPolyline(q canvas.Point, poly []canvas.Point) float64 {
	d := math.Inf(1)
	for i := 0; i+1 < len(poly); i++ {
		if dd := distPointSegment(q, poly[i], poly[i+1]); dd < d {
			d = dd
		}
	}
	return d
}

// maxSagitta measures the strongest departure from straightness of a polyline.
func maxSagitta(coords []canvas.Point) float64 {
	best := 0.0
	for i := 1; i+1 < len(coords); i++ {
		if d := distPointSegment(coords[i], coords[i-1], coords[i+1]); d > best {
			best = d
		}
	}
	return best
}

func TestTextWarp(t *testing.T) {
	family := warpTestFont(t)
	face := family.Face(12.0, canvas.Black, canvas.FontRegular, canvas.FontNormal)

	t.Run("straight", func(t *testing.T) {
		txt := canvas.NewTextLine(face, "Hole O B", canvas.Left)
		baseline := canvas.MustParseSVGPath("M0 0 L120 0")

		warped := &recordRenderer{}
		if err := txt.RenderOnPath(warped, baseline, 0.0, canvas.Identity, 0.01); err != nil {
			t.Fatalf("RenderOnPath: %v", err)
		}
		if len(warped.records) == 0 {
			t.Fatal("no paths rendered")
		}

		flat := &recordRenderer{}
		txt.RenderAsPath(flat, canvas.Identity, 0.0)
		if len(flat.records) != len(warped.records) {
			t.Fatalf("render call count differs: flat=%d warped=%d", len(flat.records), len(warped.records))
		}

		expected := [][]canvas.Point{}
		txt.WalkLines(func(y float64, spans []canvas.TextSpan) {
			for _, span := range spans {
				p, _, err := span.Face.ToPath(span.Text)
				if err != nil {
					t.Fatalf("glyph path: %v", err)
				}
				expected = append(expected, sampleSourcePath(p, span.X, y, 32.0)...)
			}
		})
		if d := hausdorff(expected, recordCoords(warped.records)); d > 0.02 {
			t.Fatalf("straight baseline deviates from flat text: hausdorff=%.4fmm", d)
		}
		for _, rec := range warped.records {
			if rec.style.Fill.Color != canvas.Black {
				t.Fatalf("fill color changed: %v", rec.style.Fill.Color)
			}
		}

		// glyph side bearings may overhang the baseline ends; such parts extend
		// linearly along the endpoint tangent. 'f' overhangs its own advance width.
		ftxt := canvas.NewTextLine(face, "f", canvas.Left)
		fspan := txtLines(ftxt)[0]
		fsrc, _, err := fspan.Face.ToPath("f")
		if err != nil {
			t.Fatalf("glyph path f: %v", err)
		}
		fb := fsrc.Bounds()
		if fb.X1 > fspan.Width+1e-9 {
			exact := &canvas.Path{}
			exact.MoveTo(0.0, 0.0)
			exact.LineTo(fspan.Width, 0.0)
			over := &recordRenderer{}
			if err := ftxt.RenderOnPath(over, exact, 0.0, canvas.Identity, 0.01); err != nil {
				t.Fatalf("RenderOnPath with end overhang: %v", err)
			}
			maxX := math.Inf(-1)
			for _, q := range recordCoords(over.records) {
				if q.X > maxX {
					maxX = q.X
				}
			}
			if maxX < fb.X1-0.02 {
				t.Fatalf("end overhang truncated: maxX=%.4f expected ~%.4f", maxX, fb.X1)
			}
			// a horizontal baseline preserves the y coordinate even in overhang
			for _, q := range recordCoords(over.records) {
				if q.Y < fb.Y0-0.02 || q.Y > fb.Y1+0.02 {
					t.Fatalf("overhang point %v outside glyph y-bounds %v", q, fb)
				}
			}
		}

		// inputs must not be modified
		baselineData := append([]float64(nil), baseline.Data()...)
		txtWidth := txt.Width
		_ = txt.RenderOnPath(&recordRenderer{}, baseline, 0.0, canvas.Identity, 0.01)
		if len(baseline.Data()) != len(baselineData) {
			t.Fatal("baseline path was modified")
		}
		for i, v := range baseline.Data() {
			if v != baselineData[i] {
				t.Fatalf("baseline path data changed at %d", i)
			}
		}
		if txt.Width != txtWidth || txt.Lines() != 1 {
			t.Fatal("text object was modified")
		}
	})

	t.Run("arc_reference", func(t *testing.T) {
		const R = 40.0
		txt := canvas.NewTextLine(face, "O", canvas.Left)
		span := txtLines(txt)[0]
		if span.X+span.Width > R*math.Pi/2.0 {
			t.Fatalf("test text too wide for reference arc")
		}
		// quarter circle centered at (0,R), CCW, starting at the origin with T(0)=(1,0)
		baseline := &canvas.Path{}
		baseline.MoveTo(0.0, 0.0)
		baseline.ArcTo(R, R, 0.0, false, true, R, R)

		const tol = 0.05
		rec := &recordRenderer{}
		if err := txt.RenderOnPath(rec, baseline, 0.0, canvas.Identity, tol); err != nil {
			t.Fatalf("RenderOnPath: %v", err)
		}

		// independent analytic expectation, not generated by the warping code
		expect := func(x, y float64) canvas.Point {
			return canvas.Point{
				X: (R - y) * math.Sin(x/R),
				Y: R - (R-y)*math.Cos(x/R),
			}
		}
		source, _, err := span.Face.ToPath(span.Text)
		if err != nil {
			t.Fatalf("glyph path: %v", err)
		}
		expected := [][]canvas.Point{}
		for _, chain := range sampleSourcePath(source, span.X, 0.0, 48.0) {
			mapped := make([]canvas.Point, len(chain))
			for i, q := range chain {
				mapped[i] = expect(q.X, q.Y)
			}
			expected = append(expected, mapped)
		}
		if d := hausdorff(expected, recordCoords(rec.records)); d > tol {
			t.Fatalf("arc warp exceeds tolerance: hausdorff=%.4fmm tol=%.4fmm", d, tol)
		}
	})

	t.Run("curved_outline", func(t *testing.T) {
		// cubic baseline with strongly varying curvature
		baseline := canvas.MustParseSVGPath("M0 0 C30 25 70 -25 100 5")
		txt := canvas.NewTextLine(face, "OBO", canvas.Left)

		rec := &recordRenderer{}
		if err := txt.RenderOnPath(rec, baseline, 0.0, canvas.Identity, 0.02); err != nil {
			t.Fatalf("RenderOnPath: %v", err)
		}
		if len(rec.records) != 1 {
			t.Fatalf("expected one rendered path for one span, got %d", len(rec.records))
		}
		full := rec.records[0].path
		subs := full.Split()
		if len(subs) < 4 {
			t.Fatalf("expected outer and inner contours for OBO, got %d subpaths", len(subs))
		}

		minArea := math.Inf(1)
		minI := 0
		polys := make([]*canvas.Polyline, len(subs))
		for i, sub := range subs {
			if !sub.Closed() {
				t.Fatal("warped contour is not closed")
			}
			polys[i] = canvas.PolylineFromPathCoords(sub)
			if a := polys[i].Area(); a < minArea {
				minArea, minI = a, i
			}
		}

		// the smallest contour is a hole: under NonZero its centroid must stay empty
		// when evaluated against the combined outer+inner path
		hole := polys[minI]
		center := hole.Centroid()
		if full.Contains(center.X, center.Y, canvas.NonZero) {
			t.Fatal("interior of O/B hole is filled")
		}
		// the inner edge itself must be bent by the varying-curvature baseline
		if sag := maxSagitta(hole.Coords()); sag < 0.05 {
			t.Fatalf("inner contour is nearly straight, sagitta=%.4fmm", sag)
		}
	})

	t.Run("shaping_and_decorations", func(t *testing.T) {
		// shaping results are reused: fi-ligature and AV kerning
		glyphs := face.Glyphs("office")
		if len(glyphs) >= len("office") {
			t.Fatalf("expected ligature shaping, got %d glyphs for %d runes", len(glyphs), len("office"))
		}
		if avWidth := face.TextWidth("AV"); avWidth >= face.TextWidth("A")+face.TextWidth("V") {
			t.Fatal("AV kerning was lost")
		}

		baseline := canvas.MustParseSVGPath("M0 0 C20 18 60 -18 110 4")

		plain := canvas.NewTextLine(face, "AV office", canvas.Left)
		recPlain := &recordRenderer{}
		if err := plain.RenderOnPath(recPlain, baseline, 0.0, canvas.Identity, 0.02); err != nil {
			t.Fatalf("RenderOnPath plain: %v", err)
		}

		underFace := family.Face(12.0, canvas.Red, canvas.FontRegular, canvas.FontNormal, canvas.FontUnderline)
		under := canvas.NewTextLine(underFace, "AV office", canvas.Left)
		recUnder := &recordRenderer{}
		if err := under.RenderOnPath(recUnder, baseline, 0.0, canvas.Identity, 0.02); err != nil {
			t.Fatalf("RenderOnPath underline: %v", err)
		}
		if len(recUnder.records) <= len(recPlain.records) {
			t.Fatalf("underline path missing: plain=%d underlined=%d renders", len(recPlain.records), len(recUnder.records))
		}
		var decoCoords []canvas.Point
		for i := len(recPlain.records); i < len(recUnder.records); i++ {
			decoCoords = append(decoCoords, recUnder.records[i].path.Coords()...)
		}
		if sag := maxSagitta(decoCoords); sag < 0.05 {
			t.Fatalf("underline stayed straight, sagitta=%.4fmm", sag)
		}
		// the underline carries the face color
		for i := len(recPlain.records); i < len(recUnder.records); i++ {
			if recUnder.records[i].style.Fill.Color != canvas.Red {
				t.Fatalf("underline color lost: %v", recUnder.records[i].style.Fill.Color)
			}
		}

		// rich text mixes font sizes and colors on one line
		small := family.Face(8.0, canvas.Blue, canvas.FontRegular, canvas.FontNormal)
		rt := canvas.NewRichText(face)
		rt.WriteFace(small, "small")
		rt.WriteString(" BIG")
		rich := rt.ToText(0.0, 0.0, canvas.Left, canvas.Top, 0.0, 0.0)
		if rich.Lines() != 1 {
			t.Fatalf("expected single rich line, got %d", rich.Lines())
		}
		colors := map[color.RGBA]bool{}
		sizes := map[float64]bool{}
		for _, span := range txtLines(rich) {
			colors[span.Face.Fill.Color] = true
			sizes[span.Face.Size] = true
		}
		if !colors[canvas.Black] || !colors[canvas.Blue] || len(colors) < 2 {
			t.Fatalf("rich text colors lost: %v", colors)
		}
		if len(sizes) < 2 {
			t.Fatalf("rich text font sizes lost: %v", sizes)
		}
		recRich := &recordRenderer{}
		if err := rich.RenderOnPath(recRich, baseline, 0.0, canvas.Identity, 0.02); err != nil {
			t.Fatalf("RenderOnPath rich: %v", err)
		}
		seen := map[color.RGBA]bool{}
		for _, rec := range recRich.records {
			seen[rec.style.Fill.Color] = true
		}
		if !seen[canvas.Black] || !seen[canvas.Blue] {
			t.Fatalf("rendered rich colors lost: %v", seen)
		}
	})

	t.Run("transformed_tolerance", func(t *testing.T) {
		const R = 50.0
		baseline := &canvas.Path{}
		baseline.MoveTo(0.0, 0.0)
		baseline.ArcTo(R, R, 0.0, false, true, R, R)

		m := canvas.Identity.Shear(0.3, 0.2).Scale(2.0, 0.5).Translate(15.0, -8.0)
		txt := canvas.NewTextLine(face, "Warp", canvas.Left)
		span := txtLines(txt)[0]
		source, _, err := span.Face.ToPath(span.Text)
		if err != nil {
			t.Fatalf("glyph path: %v", err)
		}
		expect := func(x, y float64) canvas.Point {
			q := canvas.Point{
				X: (R - y) * math.Sin(x/R),
				Y: R - (R-y)*math.Cos(x/R),
			}
			return m.Dot(q)
		}
		expected := [][]canvas.Point{}
		for _, chain := range sampleSourcePath(source, span.X, 0.0, 48.0) {
			mapped := make([]canvas.Point, len(chain))
			for i, q := range chain {
				mapped[i] = expect(q.X, q.Y)
			}
			expected = append(expected, mapped)
		}

		coarse := &recordRenderer{}
		if err := txt.RenderOnPath(coarse, baseline, 0.0, m, 0.2); err != nil {
			t.Fatalf("coarse warp: %v", err)
		}
		fine := &recordRenderer{}
		if err := txt.RenderOnPath(fine, baseline, 0.0, m, 0.02); err != nil {
			t.Fatalf("fine warp: %v", err)
		}
		nCoarse := len(recordCoords(coarse.records))
		nFine := len(recordCoords(fine.records))
		if nFine <= nCoarse {
			t.Fatalf("finer tolerance did not refine geometry: coarse=%d fine=%d vertices", nCoarse, nFine)
		}
		// errors are measured in post-transform coordinate units
		if d := hausdorff(expected, recordCoords(coarse.records)); d > 0.2 {
			t.Fatalf("coarse transformed warp off by %.4f post-m units", d)
		}
		if d := hausdorff(expected, recordCoords(fine.records)); d > 0.02 {
			t.Fatalf("fine transformed warp off by %.4f post-m units", d)
		}
	})

	t.Run("rejected_input", func(t *testing.T) {
		txt := canvas.NewTextLine(face, "Reject", canvas.Left)
		good := canvas.MustParseSVGPath("M0 0 L120 0")

		rt := canvas.NewRichText(face)
		rt.WritePath(canvas.MustParseSVGPath("M0 0 L2 0 L2 2 Z"), canvas.Red, canvas.Baseline)
		rt.WriteString("x")
		withObject := rt.ToText(0.0, 0.0, canvas.Left, canvas.Top, 0.0, 0.0)

		rtVert := canvas.NewRichText(face)
		rtVert.SetWritingMode(canvas.VerticalRL)
		rtVert.WriteString("vertical")
		vertical := rtVert.ToText(0.0, 0.0, canvas.Left, canvas.Top, 0.0, 0.0)

		multiLine := canvas.NewTextLine(face, "a\nb", canvas.Left)

		cases := map[string]struct {
			txt      *canvas.Text
			baseline *canvas.Path
			offset   float64
			m        canvas.Matrix
			tol      float64
		}{
			"empty_baseline":      {txt, &canvas.Path{}, 0.0, canvas.Identity, 0.01},
			"closed_baseline":     {txt, canvas.MustParseSVGPath("M0 0 L10 0 L10 5 L0 5 Z"), 0.0, canvas.Identity, 0.01},
			"cusp":                {txt, canvas.MustParseSVGPath("M0 0 L10 0 L10 5"), 0.0, canvas.Identity, 0.01},
			"reversal":            {txt, canvas.MustParseSVGPath("M0 0 L10 0 L5 0"), 0.0, canvas.Identity, 0.01},
			"in_segment_turnback": {txt, canvas.MustParseSVGPath("M0 0 Q10 5 5 0"), 0.0, canvas.Identity, 0.01},
			"negative_offset":     {txt, good, -1.0, canvas.Identity, 0.01},
			"zero_tolerance":      {txt, good, 0.0, canvas.Identity, 0.0},
			"negative_tolerance":  {txt, good, 0.0, canvas.Identity, -0.01},
			"singular_matrix":     {txt, good, 0.0, canvas.Identity.Scale(1.0, 0.0), 0.01},
			"advance_too_long":    {txt, canvas.MustParseSVGPath("M0 0 L2 0"), 0.0, canvas.Identity, 0.01},
			"multi_line":          {multiLine, good, 0.0, canvas.Identity, 0.01},
			"embedded_object":     {withObject, good, 0.0, canvas.Identity, 0.01},
			"vertical_text":       {vertical, good, 0.0, canvas.Identity, 0.01},
		}
		for name, tc := range cases {
			t.Run(name, func(t *testing.T) {
				rec := &recordRenderer{}
				err := tc.txt.RenderOnPath(rec, tc.baseline, tc.offset, tc.m, tc.tol)
				if err == nil {
					t.Fatalf("expected error for %s", name)
				}
				if len(rec.records) != 0 {
					t.Fatalf("renderer received %d paths before %s was rejected", len(rec.records), name)
				}
			})
		}
	})

	// demo artifact: holes, rich text and underline on a curved baseline
	if out := os.Getenv("CANVAS_WARP_OUT"); out != "" {
		writeWarpSVG(t, out, family)
	}
}

func writeWarpSVG(t *testing.T, outDir string, family *canvas.FontFamily) {
	t.Helper()
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		t.Fatalf("mkdir output: %v", err)
	}
	width, height := 160.0, 70.0
	baseline := canvas.MustParseSVGPath("M10 35 C45 60 95 8 150 32")

	face := family.Face(11.0, canvas.Black, canvas.FontRegular, canvas.FontNormal, canvas.FontUnderline)
	red := family.Face(14.0, canvas.Red, canvas.FontRegular, canvas.FontNormal)
	blue := family.Face(9.0, canvas.Blue, canvas.FontRegular, canvas.FontNormal)
	rt := canvas.NewRichText(face)
	rt.WriteFace(red, "OB")
	rt.WriteString(" curved ")
	rt.WriteFace(blue, "label")
	txt := rt.ToText(0.0, 0.0, canvas.Left, canvas.Top, 0.0, 0.0)

	f, err := os.Create(filepath.Join(outDir, "text-warp.svg"))
	if err != nil {
		t.Fatalf("create svg: %v", err)
	}
	defer f.Close()
	r := svg.New(f, width, height, nil)

	stroke := canvas.DefaultStyle
	stroke.Fill = canvas.Paint{}
	stroke.Stroke = canvas.Paint{Color: canvas.Grey}
	stroke.StrokeWidth = 0.2
	r.RenderPath(baseline, stroke, canvas.Identity)

	if err := txt.RenderOnPath(r, baseline, 2.0, canvas.Identity, 0.02); err != nil {
		t.Fatalf("demo warp: %v", err)
	}
	if err := r.Close(); err != nil {
		t.Fatalf("close svg: %v", err)
	}
}
