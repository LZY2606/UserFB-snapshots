package canvas

import (
	"fmt"
	"math"
)

// RenderOnPath renders the text's unhinted glyph outlines warped onto the given
// open baseline path, calling r.RenderPath for each flattened contour and for the
// text's solid decorations (such as underlines). The baseline is parameterized by
// arc length s in its input direction with unit tangent T and left normal
// N=(-T.Y,T.X). A glyph-space point (x,y), as produced by RenderAsPath with the
// identity matrix and zero resolution, is mapped to
//
//	C(offset+x) + y*N(offset+x)
//
// after which matrix m is applied. Glyph overhang past either baseline endpoint is
// extended linearly using the endpoint tangent. The warped contours are flattened
// to polylines whose two-sided deviation from the true warped outlines does not
// exceed tolerance, measured in the coordinate units after applying m. The
// original text, font faces and baseline are not modified.
func (t *Text) RenderOnPath(r Renderer, baseline *Path, offset float64, m Matrix, tolerance float64) error {
	if err := validateWarpInputs(t, baseline, offset, m, tolerance); err != nil {
		return err
	}

	// Build the arc-length frame before submitting anything to the renderer so
	// that a degenerate baseline is reported without partial output. The frame
	// chord sag is measured before m, so the post-transform error is bounded by
	// smax times the sag; half of the tolerance budget goes to the frame and half
	// to the source-curve subdivision.
	_, smax := matrixSingularValues(m)
	frame, err := newBaselineFrame(baseline, 0.5*tolerance/smax)
	if err != nil {
		return err
	}
	warper := &pathWarper{
		frame:       frame,
		offset:      offset,
		m:           m,
		subdivLimit: 0.5 * tolerance,
	}

	// Decorations use the same warp so that underlines follow the curved baseline.
	var decoErr error
	t.WalkDecorations(func(paint Paint, p *Path) {
		if decoErr != nil {
			return
		}
		q, err := warper.warpPath(p, 0.0, 0.0)
		if err != nil {
			decoErr = err
			return
		}
		style := DefaultStyle
		style.Fill = paint
		r.RenderPath(q, style, m)
	})
	if decoErr != nil {
		return decoErr
	}

	for _, line := range t.lines {
		for _, span := range line.spans {
			// Resolution zero disables hinting and pixel grid alignment.
			p, _, err := span.Face.toPath(span.Glyphs, span.Face.PPEM(0))
			if err != nil {
				return err
			}
			q, err := warper.warpPath(p, span.X, -line.y)
			if err != nil {
				return err
			}
			style := DefaultStyle
			style.Fill = span.Face.Fill
			r.RenderPath(q, style, m)
		}
	}
	return nil
}

// validateWarpInputs verifies all preconditions. It must run before any call to
// the renderer so that rejected inputs never produce partial drawing.
func validateWarpInputs(t *Text, baseline *Path, offset float64, m Matrix, tolerance float64) error {
	if t == nil {
		return fmt.Errorf("text warp: text is nil")
	}
	if baseline == nil || baseline.Empty() {
		return fmt.Errorf("text warp: baseline path is empty")
	}
	if math.IsNaN(offset) || math.IsInf(offset, 0) || offset < 0.0 {
		return fmt.Errorf("text warp: offset must be a finite non-negative number, got %v", offset)
	}
	for _, row := range m {
		for _, v := range row {
			if math.IsNaN(v) || math.IsInf(v, 0) {
				return fmt.Errorf("text warp: transformation matrix contains non-finite values")
			}
		}
	}
	if Equal(m.Det(), 0.0) {
		return fmt.Errorf("text warp: transformation matrix must be invertible")
	}
	if math.IsNaN(tolerance) || math.IsInf(tolerance, 0) || tolerance <= 0.0 {
		return fmt.Errorf("text warp: tolerance must be a finite positive number, got %v", tolerance)
	}
	if t.WritingMode != HorizontalTB {
		return fmt.Errorf("text warp: vertical writing modes are not supported")
	}
	if len(t.lines) != 1 {
		return fmt.Errorf("text warp: only single-line text is supported, got %d lines", len(t.lines))
	}
	for _, span := range t.lines[0].spans {
		if !span.IsText() {
			return fmt.Errorf("text warp: embedded images and path objects are not supported")
		}
		if span.Rotation != 0.0 {
			return fmt.Errorf("text warp: rotated glyphs are not supported")
		}
	}

	frameLength := baseline.Length()
	xmax := 0.0
	for _, span := range t.lines[0].spans {
		if xmax < span.X+span.Width {
			xmax = span.X + span.Width
		}
	}
	if offset+xmax > frameLength+Epsilon {
		return fmt.Errorf("text warp: text advance %.6gmm exceeds the remaining baseline length %.6gmm", offset+xmax, frameLength)
	}
	return nil
}

// matrixSingularValues returns the two singular values of the linear part of m,
// ordered smallest first.
func matrixSingularValues(m Matrix) (float64, float64) {
	a, b := m[0][0], m[0][1]
	c, d := m[1][0], m[1][1]
	// squared singular values are the eigenvalues of M^T M, see Matrix.Eigen
	mtm := Matrix{{a*a + c*c, a*b + c*d, 0.0}, {a*b + c*d, b*b + d*d, 0.0}}
	lambda1, lambda2, _, _ := mtm.Eigen()
	smin := math.Sqrt(math.Max(0.0, math.Min(lambda1, lambda2)))
	smax := math.Sqrt(math.Max(0.0, math.Max(lambda1, lambda2)))
	return smin, smax
}

// frameSeg keeps the analytic tangent evaluator of one original baseline segment.
type frameSeg struct {
	tan func(t float64) Point
}

// frameSample is one node of the piecewise linear arc-length parameterization.
type frameSample struct {
	s      float64 // cumulative arc length along the baseline
	p      Point   // baseline position C(s)
	seg    int     // owning baseline segment
	tlocal float64 // segment parameter of this node, in [0,1]
}

// baselineFrame is an arc-length parameterization of an open baseline path.
type baselineFrame struct {
	samples []frameSample
	segs    []frameSeg
	length  float64
}

// At returns the baseline position, unit tangent and left normal at arc length s.
// Values outside [0, Length] extend the endpoint frame linearly along the endpoint
// tangent, which realizes the required overhang behavior.
func (f *baselineFrame) At(s float64) (Point, Point, Point) {
	first, last := f.samples[0], f.samples[len(f.samples)-1]
	if s <= first.s {
		tan := f.segs[first.seg].tan(first.tlocal)
		return first.p.Add(tan.Mul(s - first.s)), tan, leftNormal(tan)
	}
	if s >= last.s {
		tan := f.segs[last.seg].tan(last.tlocal)
		return last.p.Add(tan.Mul(s - last.s)), tan, leftNormal(tan)
	}

	// binary search for the surrounding samples
	lo, hi := 0, len(f.samples)-1
	for hi-lo > 1 {
		mid := (lo + hi) / 2
		if f.samples[mid].s <= s {
			lo = mid
		} else {
			hi = mid
		}
	}
	q0, q1 := f.samples[lo], f.samples[hi]
	u := (s - q0.s) / (q1.s - q0.s)
	p := q0.p.Interpolate(q1.p, u)

	// evaluate the tangent analytically from the owning segment, so the frame
	// normal is exact even where the position is a chord approximation. An
	// interval can straddle one original segment boundary.
	var tan Point
	if q0.seg == q1.seg {
		tlocal := q0.tlocal + u*(q1.tlocal-q0.tlocal)
		tan = f.segs[q0.seg].tan(tlocal)
	} else if u < 0.5 {
		tan = f.segs[q0.seg].tan(q0.tlocal)
	} else {
		tan = f.segs[q1.seg].tan(q1.tlocal)
	}
	return p, tan, leftNormal(tan)
}

func leftNormal(t Point) Point {
	return Point{-t.Y, t.X}
}

// newBaselineFrame scans the baseline commands, rejects cusps and degenerate
// tangents, and adaptively samples each segment until chord sag stays below the
// pre-transform error budget.
func newBaselineFrame(p *Path, sagBudget float64) (*baselineFrame, error) {
	if p.Closed() {
		return nil, fmt.Errorf("text warp: baseline must be an open path")
	}
	sagBudget = math.Max(sagBudget, 1e-12)

	type segment struct {
		pos func(t float64) Point
		tan func(t float64) Point
	}
	segments := []segment{}
	moveCount := 0
	var prevTan Point
	hasPrev := false

	scanner := p.Scanner()
	for scanner.Scan() {
		cmd := scanner.Cmd()
		start := scanner.Start()
		end := scanner.End()
		switch cmd {
		case MoveToCmd:
			moveCount++
			if moveCount > 1 {
				return nil, fmt.Errorf("text warp: baseline must be a single connected path")
			}
			hasPrev = false
			continue
		case LineToCmd, CloseCmd:
			if start.Equals(end) {
				continue // zero-length segment, skip
			}
			tan := end.Sub(start).Norm(1.0)
			if hasPrev && tangentBreak(prevTan, tan) {
				return nil, fmt.Errorf("text warp: baseline has a cusp or tangent discontinuity")
			}
			segments = append(segments, segment{
				pos: func(t float64) Point { return start.Interpolate(end, t) },
				tan: func(float64) Point { return tan },
			})
			prevTan, hasPrev = tan, true
		case QuadToCmd:
			cp := scanner.CP1()
			pos := func(t float64) Point { return quadraticBezierPos(start, cp, end, t) }
			tan := func(t float64) Point { return quadraticBezierDeriv(start, cp, end, t).Norm(1.0) }
			if err := checkSmoothSegment(hasPrev, prevTan, tan); err != nil {
				return nil, err
			}
			segments = append(segments, segment{pos: pos, tan: tan})
			prevTan, hasPrev = tan(1.0), true
		case CubeToCmd:
			cp1 := scanner.CP1()
			cp2 := scanner.CP2()
			pos := func(t float64) Point { return cubicBezierPos(start, cp1, cp2, end, t) }
			tan := func(t float64) Point { return cubicBezierDeriv(start, cp1, cp2, end, t).Norm(1.0) }
			if err := checkSmoothSegment(hasPrev, prevTan, tan); err != nil {
				return nil, err
			}
			segments = append(segments, segment{pos: pos, tan: tan})
			prevTan, hasPrev = tan(1.0), true
		case ArcToCmd:
			rx, ry, rotDeg, large, sweep := scanner.Arc()
			phi := rotDeg * math.Pi / 180.0
			cx, cy, theta0, theta1 := ellipseToCenter(start.X, start.Y, rx, ry, phi, large, sweep, end.X, end.Y)
			pos := func(t float64) Point {
				return EllipsePos(rx, ry, phi, cx, cy, theta0+t*(theta1-theta0))
			}
			tan := func(t float64) Point {
				return ellipseDeriv(rx, ry, phi, sweep, theta0+t*(theta1-theta0)).Norm(1.0)
			}
			if err := checkSmoothSegment(hasPrev, prevTan, tan); err != nil {
				return nil, err
			}
			segments = append(segments, segment{pos: pos, tan: tan})
			prevTan, hasPrev = tan(1.0), true
		}
	}
	if len(segments) == 0 {
		return nil, fmt.Errorf("text warp: baseline has no length")
	}

	f := &baselineFrame{}
	for i, seg := range segments {
		f.segs = append(f.segs, frameSeg{tan: seg.tan})
		nodes, segLen := sampleFrameSegment(seg.pos, i, sagBudget, f.length)
		if i == 0 {
			f.samples = append(f.samples, nodes[0])
			nodes = nodes[1:]
		}
		f.samples = append(f.samples, nodes...)
		f.length += segLen
	}
	return f, nil
}

// tangentJoinLimit is the maximum direction change allowed at the join of two
// baseline segments; anything larger is a cusp and must be rejected.
const tangentJoinLimit = 0.5 * math.Pi / 180.0

// tangentBreak reports whether two unit tangents differ by more than the join
// limit; reversals and true cusps fail the test outright.
func tangentBreak(a, b Point) bool {
	return a.Dot(b) < math.Cos(tangentJoinLimit)
}

// checkSmoothSegment verifies a curved segment has a well-defined, non-reversing
// tangent along its length and joins the previous segment tangentially. A zero
// derivative is a degenerate cusp; a tangent flip of more than 90 degrees between
// adjacent samples marks a turnback hidden inside a single curve command.
func checkSmoothSegment(hasPrev bool, prevTan Point, tan func(float64) Point) error {
	const n = 64
	var previous Point
	for i := 0; i <= n; i++ {
		d := tan(float64(i) / float64(n))
		if math.IsNaN(d.X) || math.IsNaN(d.Y) || Equal(d.Length(), 0.0) {
			return fmt.Errorf("text warp: baseline contains a degenerate segment without a tangent")
		}
		if i > 0 && previous.Dot(d) < 0.0 {
			return fmt.Errorf("text warp: baseline has a cusp, turnback or tangent discontinuity")
		}
		previous = d
	}
	if hasPrev && tangentBreak(prevTan, tan(0.0)) {
		return fmt.Errorf("text warp: baseline has a cusp or tangent discontinuity")
	}
	return nil
}

// sampleFrameSegment recursively subdivides a segment until every chord's sag is
// below sagBudget, returning nodes with cumulative arc length starting at s0.
func sampleFrameSegment(pos func(float64) Point, segIndex int, sagBudget, s0 float64) ([]frameSample, float64) {
	samples := []frameSample{}
	var length float64

	var add func(t0, t1 float64, p0, p1 Point, depth int)
	add = func(t0, t1 float64, p0, p1 Point, depth int) {
		tm := (t0 + t1) / 2.0
		pm := pos(tm)
		chord := p1.Sub(p0)
		sag := 0.0
		if chord.Length() > Epsilon {
			sag = math.Abs(chord.PerpDot(pm.Sub(p0))) / chord.Length()
		}
		if depth < 40 && sag > sagBudget {
			add(t0, tm, p0, pm, depth+1)
			add(tm, t1, pm, p1, depth+1)
			return
		}
		length += p1.Sub(p0).Length()
		samples = append(samples, frameSample{s0 + length, p1, segIndex, t1})
	}

	p0 := pos(0.0)
	samples = append(samples, frameSample{s0, p0, segIndex, 0.0})
	add(0.0, 1.0, p0, pos(1.0), 0)
	return samples, length
}

// pathWarper maps glyph-space paths through the baseline frame and flattens them.
type pathWarper struct {
	frame       *baselineFrame
	offset      float64
	m           Matrix
	subdivLimit float64
}

// point applies the warp to a glyph-space point.
func (w *pathWarper) point(x, y float64) Point {
	s := w.offset + x
	c, _, n := w.frame.At(s)
	return c.Add(n.Mul(y))
}

// postDistance returns the length of a pre-transform vector after applying the
// linear part of m (translation does not affect distances).
func (w *pathWarper) postDistance(v Point) float64 {
	return Point{w.m[0][0]*v.X + w.m[0][1]*v.Y, w.m[1][0]*v.X + w.m[1][1]*v.Y}.Length()
}

// warpPath flattens and warps a source path translated by (tx,ty), preserving the
// subpath structure, winding and Close commands so that holes remain holes.
func (w *pathWarper) warpPath(p *Path, tx, ty float64) (*Path, error) {
	out := &Path{}
	scanner := p.Scanner()
	const maxDepth = 48

	for scanner.Scan() {
		cmd := scanner.Cmd()
		start := scanner.Start()
		end := scanner.End()

		if cmd == MoveToCmd {
			// for a MoveTo the scanner's Start is the previous pen position; the
			// move target is End
			q := w.point(end.X+tx, end.Y+ty)
			out.MoveTo(q.X, q.Y)
			continue
		}

		var pos func(t float64) Point
		switch cmd {
		case CloseCmd:
			if start.Equals(end) {
				out.Close()
				continue
			}
			pos = func(t float64) Point { return start.Interpolate(end, t) }
		case LineToCmd:
			pos = func(t float64) Point { return start.Interpolate(end, t) }
		case QuadToCmd:
			cp := scanner.CP1()
			pos = func(t float64) Point { return quadraticBezierPos(start, cp, end, t) }
		case CubeToCmd:
			cp1 := scanner.CP1()
			cp2 := scanner.CP2()
			pos = func(t float64) Point { return cubicBezierPos(start, cp1, cp2, end, t) }
		case ArcToCmd:
			rx, ry, rotDeg, large, sweep := scanner.Arc()
			phi := rotDeg * math.Pi / 180.0
			cx, cy, theta0, theta1 := ellipseToCenter(start.X, start.Y, rx, ry, phi, large, sweep, end.X, end.Y)
			pos = func(t float64) Point {
				return EllipsePos(rx, ry, phi, cx, cy, theta0+t*(theta1-theta0))
			}
		default:
			return nil, fmt.Errorf("text warp: unsupported path command %v", cmd)
		}

		warped := func(t float64) Point {
			q := pos(t)
			return w.point(q.X+tx, q.Y+ty)
		}
		p0 := warped(0.0)
		p1 := warped(1.0)

		var subdivide func(pa, pb Point, t0, t1 float64, depth int)
		subdivide = func(pa, pb Point, t0, t1 float64, depth int) {
			tm := (t0 + t1) / 2.0
			pm := warped(tm)
			chord := pb.Sub(pa)
			var deviation Point
			if chord.Length() <= Epsilon {
				deviation = pm.Sub(pa)
			} else {
				u := pm.Sub(pa).Dot(chord) / chord.Dot(chord)
				u = math.Min(1.0, math.Max(0.0, u))
				deviation = pm.Sub(pa.Interpolate(pb, u))
			}
			if depth < maxDepth && w.postDistance(deviation) > w.subdivLimit {
				subdivide(pa, pm, t0, tm, depth+1)
				subdivide(pm, pb, tm, t1, depth+1)
				return
			}
			out.LineTo(pb.X, pb.Y)
		}
		subdivide(p0, p1, 0.0, 1.0, 0)

		if cmd == CloseCmd {
			out.Close()
		}
	}
	return out, nil
}
