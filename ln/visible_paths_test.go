package ln

import (
	"math"
	"math/rand"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func approxVector(a, b Vector, eps float64) bool {
	return math.Abs(a.X-b.X) <= eps && math.Abs(a.Y-b.Y) <= eps && math.Abs(a.Z-b.Z) <= eps
}

// referenceHidden is an independent, straightforward implementation of the
// visibility rule: the open segment eye -> point crosses the sphere interior
// when the view quadratic has two real roots in t that straddle (0, 1).
func referenceHidden(eye, point Vector, sphere *Sphere) bool {
	d := point.Sub(eye)
	w := eye.Sub(sphere.Center)
	a := d.Dot(d)
	b := 2 * d.Dot(w)
	c := w.Dot(w) - sphere.Radius*sphere.Radius
	if a == 0 {
		return false // eye == point, the open segment is empty
	}
	disc := b*b - 4*a*c
	// A discriminant within floating-point scale of zero is a tangency: the
	// ray touches the sphere at one point without crossing its interior.
	if disc <= 1e-12*math.Max(b*b, math.Abs(4*a*c)) {
		return false
	}
	s := math.Sqrt(disc)
	t1 := (-b - s) / (2 * a)
	t2 := (-b + s) / (2 * a)
	return t1 < 1 && t2 > 0
}

func clipScene(t *testing.T, eye Vector, spheres []*Sphere, paths Paths) Paths {
	t.Helper()
	s := &Scene{}
	for _, sphere := range spheres {
		s.Add(sphere)
	}
	out, err := s.ClipVisiblePaths(paths, eye)
	if err != nil {
		t.Fatalf("ClipVisiblePaths returned error: %v", err)
	}
	return out
}

// checkLineAgainstReference maps the clipped output back onto the source
// segment parameter u and compares visibility against brute-force ray tests.
func checkLineAgainstReference(t *testing.T, eye, a, b Vector, spheres []*Sphere, out Paths) {
	t.Helper()
	v := b.Sub(a)
	var covered []realInterval
	for _, p := range out {
		for i := 0; i+1 < len(p); i++ {
			u0 := p[i].Sub(a).Dot(v) / v.Dot(v)
			u1 := p[i+1].Sub(a).Dot(v) / v.Dot(v)
			if u1 < u0 {
				t.Fatalf("output segment runs against source direction: %v -> %v", p[i], p[i+1])
			}
			covered = append(covered, realInterval{u0, u1})
		}
	}
	for k := 0; k <= 20000; k++ {
		u := float64(k) / 20000
		point := a.Add(v.MulScalar(u))
		wantHidden := false
		for _, sphere := range spheres {
			if referenceHidden(eye, point, sphere) {
				wantHidden = true
				break
			}
		}
		gotVisible := false
		for _, iv := range covered {
			if u >= iv.lo-1e-11 && u <= iv.hi+1e-11 {
				gotVisible = true
				break
			}
		}
		if gotVisible == wantHidden {
			t.Fatalf("visibility mismatch at u=%v point=%v: hidden=%v but output visible=%v", u, point, wantHidden, gotVisible)
		}
	}
}

func TestVisiblePathsTarget(t *testing.T) {
	eye := Vector{0, 0, 5}
	line := Path{{-1, 0, 0}, {1, 0, 0}}

	t.Run("HeadlineNumericGap", func(t *testing.T) {
		sphere := NewSphere(Vector{0, 0, 2.5}, 0.1)
		out := clipScene(t, eye, []*Sphere{sphere}, Paths{line})
		if len(out) != 2 {
			t.Fatalf("expected 2 visible pieces, got %d: %s", len(out), out.String())
		}
		for _, p := range out {
			if len(p) != 2 {
				t.Fatalf("each piece must have exactly 2 vertices, got %d", len(p))
			}
		}
		want := 0.200160192256
		assertVec := func(got Vector, x float64) {
			if math.Abs(got.X-x) > 1e-8 || got.Y != 0 || got.Z != 0 {
				t.Fatalf("boundary point %v not within 1e-8 of (%v,0,0)", got, x)
			}
		}
		assertVec(out[0][0], -1)
		assertVec(out[0][1], -want)
		assertVec(out[1][0], want)
		assertVec(out[1][1], 1)
		checkLineAgainstReference(t, eye, line[0], line[1], []*Sphere{sphere}, out)
	})

	t.Run("TinyRadiusGap", func(t *testing.T) {
		sphere := NewSphere(Vector{0, 0, 2.5}, 1e-5)
		out := clipScene(t, eye, []*Sphere{sphere}, Paths{line})
		if len(out) != 2 {
			t.Fatalf("tiny sphere must still open a gap, got %d paths", len(out))
		}
		for _, x := range []float64{out[0][1].X, out[1][0].X} {
			if math.Abs(math.Abs(x)-2e-5) > 1e-7 {
				t.Fatalf("tiny gap boundary %v not near ±2e-5", x)
			}
		}
		checkLineAgainstReference(t, eye, line[0], line[1], []*Sphere{sphere}, out)
	})

	t.Run("LineThroughSphereSurfaces", func(t *testing.T) {
		// Segment crosses the front and back surface of the sphere.
		sphere := NewSphere(Vector{}, 0.5)
		a, b := Vector{-1, 0, -1}, Vector{1, 0, 1}
		out := clipScene(t, eye, []*Sphere{sphere}, Paths{{a, b}})
		if len(out) != 2 {
			t.Fatalf("expected 2 pieces, got %d", len(out))
		}
		// Points on the front surface stay visible; the hidden side is cut.
		front := Vector{0, 0, 2.6}
		through := clipScene(t, eye, []*Sphere{NewSphere(Vector{0, 0, 2.5}, 0.1)},
			Paths{{{0, 0, 2.7}, {0, 0, 2.3}}})
		if len(through) != 1 || !approxVector(through[0][len(through[0])-1], front, 1e-12) {
			t.Fatalf("front-surface point should be the closing endpoint, got %s", through.String())
		}
		checkLineAgainstReference(t, eye, a, b, []*Sphere{sphere}, out)
	})

	t.Run("SphereBehindLineAndBehindEye", func(t *testing.T) {
		for _, center := range []Vector{{0, 0, -3}, {0, 0, 8}} {
			sphere := NewSphere(center, 0.5)
			out := clipScene(t, eye, []*Sphere{sphere}, Paths{line})
			if len(out) != 1 || len(out[0]) != 2 ||
				out[0][0] != line[0] || out[0][1] != line[1] {
				t.Fatalf("sphere at %v must not occlude, got %s", center, out.String())
			}
		}
	})

	t.Run("TwoSpheresSeparated", func(t *testing.T) {
		// Spheres close to the line cast narrow, separated shadow cones.
		spheres := []*Sphere{
			NewSphere(Vector{-0.5, 0, 1}, 0.1),
			NewSphere(Vector{0.5, 0, 1}, 0.1),
		}
		out := clipScene(t, eye, spheres, Paths{line})
		if len(out) != 3 {
			t.Fatalf("expected 3 pieces (left, connected middle, right), got %d: %s", len(out), out.String())
		}
		if len(out[1]) != 2 {
			t.Fatalf("middle visible piece should be a single segment, got %d vertices", len(out[1]))
		}
		if out[0][1].X >= out[1][0].X || out[1][1].X >= out[2][0].X {
			t.Fatalf("pieces out of order: %s", out.String())
		}
		checkLineAgainstReference(t, eye, line[0], line[1], spheres, out)
	})

	t.Run("TwoSpheresOverlapping", func(t *testing.T) {
		spheres := []*Sphere{
			NewSphere(Vector{-0.05, 0, 2.5}, 0.1),
			NewSphere(Vector{0.05, 0, 2.5}, 0.1),
		}
		out := clipScene(t, eye, spheres, Paths{line})
		if len(out) != 2 {
			t.Fatalf("overlapping gaps must merge into one, got %d paths", len(out))
		}
		// Adjacent/covered intervals must not leave a phantom sliver: every
		// gap boundary belongs to one of the two spheres' tangent solutions.
		checkLineAgainstReference(t, eye, line[0], line[1], spheres, out)
	})

	t.Run("TangentDoesNotSplit", func(t *testing.T) {
		// A sight line exactly tangent to the shadow cone of the sphere: the
		// line grazes the sphere at one point but never enters its interior.
		sphere := NewSphere(Vector{0, 0, 2.5}, 0.1)
		// Tangent locus on the z=0 plane is the circle
		// (z_c^2 - r^2)(x^2 + y^2) = r^2 * D^2, so a horizontal tangent
		// line sits at the headline gap radius.
		yTan := math.Sqrt((0.1 * 0.1 * 5 * 5) / (2.5*2.5 - 0.1*0.1))
		tangent := Path{{-1, yTan, 0}, {1, yTan, 0}}
		out := clipScene(t, eye, []*Sphere{sphere}, Paths{tangent})
		if len(out) != 1 || len(out[0]) != 2 {
			t.Fatalf("tangent sight line must stay one untouched segment, got %s", out.String())
		}
		if out[0][0] != tangent[0] || out[0][1] != tangent[1] {
			t.Fatalf("tangent segment endpoints changed: %s", out.String())
		}
		// And against the reference rule, the grazing point itself is visible.
		grazing := Vector{0, yTan, 0}
		if referenceHidden(eye, grazing, sphere) {
			t.Fatal("reference rule disagrees: tangent point should be visible")
		}
		checkLineAgainstReference(t, eye, tangent[0], tangent[1], []*Sphere{sphere}, out)
	})

	t.Run("PolylineKeepsSharedVisibleVertices", func(t *testing.T) {
		sphere := NewSphere(Vector{0, 0, 2.5}, 0.1)
		poly := Path{{-1, 0, 0}, {-0.5, 0, 0}, {0.5, 0, 0}, {1, 0, 0}}
		out := clipScene(t, eye, []*Sphere{sphere}, Paths{poly})
		if len(out) != 2 {
			t.Fatalf("expected 2 paths around the gap, got %d", len(out))
		}
		if len(out[0]) != 3 || out[0][1] != (Vector{-0.5, 0, 0}) {
			t.Fatalf("left path should keep the shared -0.5 vertex, got %s", out[0].String())
		}
		if len(out[1]) != 3 || out[1][1] != (Vector{0.5, 0, 0}) {
			t.Fatalf("right path should keep the shared 0.5 vertex, got %s", out[1].String())
		}
		if math.Abs(math.Abs(out[0][2].X)-0.200160192256) > 1e-8 ||
			math.Abs(math.Abs(out[1][0].X)-0.200160192256) > 1e-8 {
			t.Fatalf("gap boundaries wrong: %s | %s", out[0].String(), out[1].String())
		}
	})

	t.Run("ReversePathKeepsDirection", func(t *testing.T) {
		sphere := NewSphere(Vector{0, 0, 2.5}, 0.1)
		reverse := Path{{1, 0, 0}, {-1, 0, 0}}
		out := clipScene(t, eye, []*Sphere{sphere}, Paths{reverse})
		if len(out) != 2 {
			t.Fatalf("expected 2 pieces, got %d", len(out))
		}
		if out[0][0].X != 1 || out[0][1].X <= 0 {
			t.Fatalf("first reversed piece should start at x=1, got %s", out[0].String())
		}
		if out[1][1].X != -1 || out[1][0].X >= 0 {
			t.Fatalf("second reversed piece should end at x=-1, got %s", out[1].String())
		}
	})

	t.Run("IndependentPathsNeverMerged", func(t *testing.T) {
		sphere := NewSphere(Vector{0, 0, 2.5}, 0.1)
		out := clipScene(t, eye, []*Sphere{sphere}, Paths{line, line})
		if len(out) != 4 {
			t.Fatalf("two independent source paths must yield 4 pieces, got %d", len(out))
		}
	})

	t.Run("EmptySceneAndDegenerateInputs", func(t *testing.T) {
		empty := &Scene{}
		out, err := empty.ClipVisiblePaths(Paths{line}, eye)
		if err != nil || len(out) != 1 || !reflect.DeepEqual(out[0], line) {
			t.Fatalf("empty scene should pass the path through, got %v err=%v", out, err)
		}
		// Duplicate adjacent vertices collapse; a lone point is dropped.
		out, err = empty.ClipVisiblePaths(Paths{
			{{1, 0, 0}, {1, 0, 0}, {2, 0, 0}},
			{{1, 0, 0}},
			nil,
		}, eye)
		if err != nil {
			t.Fatal(err)
		}
		if len(out) != 1 || len(out[0]) != 2 {
			t.Fatalf("expected one deduplicated 2-vertex path, got %s", out.String())
		}
	})

	t.Run("InputsAreNotMutated", func(t *testing.T) {
		sphere := NewSphere(Vector{0, 0, 2.5}, 0.1)
		savedSphere := *sphere
		input := Paths{{{-1, 0, 0}, {1, 0, 0}}}
		savedPaths := append(Paths(nil), input...)
		scene := &Scene{}
		scene.Add(sphere)
		if _, err := scene.ClipVisiblePaths(input, eye); err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(*sphere, savedSphere) {
			t.Fatal("sphere was modified")
		}
		if !reflect.DeepEqual(input, savedPaths) {
			t.Fatal("input paths were modified")
		}
		if len(scene.Shapes) != 1 {
			t.Fatal("scene shape list changed")
		}
	})

	t.Run("ValidationErrors", func(t *testing.T) {
		goodSphere := NewSphere(Vector{}, 1)
		cases := []struct {
			name  string
			scene *Scene
			paths Paths
			eye   Vector
		}{
			{"eye inside sphere", func() *Scene {
				s := &Scene{}
				s.Add(NewSphere(Vector{0, 0, 5}, 1))
				return s
			}(), Paths{line}, Vector{0, 0, 5.5}},
			{"eye on sphere surface", func() *Scene {
				s := &Scene{}
				s.Add(NewSphere(Vector{0, 0, 5}, 1))
				return s
			}(), Paths{line}, Vector{0, 0, 6}},
			{"zero radius", func() *Scene {
				s := &Scene{}
				s.Add(NewSphere(Vector{}, 0))
				return s
			}(), Paths{line}, eye},
			{"negative radius", func() *Scene {
				s := &Scene{}
				s.Add(NewSphere(Vector{}, -0.5))
				return s
			}(), Paths{line}, eye},
			{"NaN radius", func() *Scene {
				s := &Scene{}
				s.Add(&Sphere{Center: Vector{}, Radius: math.NaN()})
				return s
			}(), Paths{line}, eye},
			{"non-finite eye", &Scene{Shapes: []Shape{goodSphere}}, Paths{line}, Vector{math.Inf(1), 0, 5}},
			{"non-finite path vertex", &Scene{Shapes: []Shape{goodSphere}}, Paths{{{math.NaN(), 0, 0}, {1, 0, 0}}}, eye},
			{"non-finite sphere center", func() *Scene {
				s := &Scene{}
				s.Add(&Sphere{Center: Vector{0, math.Inf(-1), 0}, Radius: 1})
				return s
			}(), Paths{line}, eye},
			{"non-sphere shape", &Scene{Shapes: []Shape{NewCube(Vector{-1, -1, -1}, Vector{1, 1, 1})}}, Paths{line}, eye},
			{"mixed sphere and cube", &Scene{Shapes: []Shape{goodSphere, NewCube(Vector{-1, -1, -1}, Vector{1, 1, 1})}}, Paths{line}, eye},
		}
		for _, tc := range cases {
			t.Run(tc.name, func(t *testing.T) {
				out, err := tc.scene.ClipVisiblePaths(tc.paths, tc.eye)
				if err == nil {
					t.Fatalf("expected error, got paths: %s", out.String())
				}
				if out != nil {
					t.Fatalf("error must not publish partial results, got %d paths", len(out))
				}
			})
		}
	})

	t.Run("HiddenSharedVertexBreaksPaths", func(t *testing.T) {
		sphere := NewSphere(Vector{0, 0, 2.5}, 0.1)
		poly := Path{{-1, 0, 0}, {0, 0, 0}, {1, 0, 0}}
		out := clipScene(t, eye, []*Sphere{sphere}, Paths{poly})
		if len(out) != 2 {
			t.Fatalf("hidden shared vertex must break the polyline into 2 paths, got %d", len(out))
		}
		for _, p := range out {
			for _, v := range p {
				if v == (Vector{0, 0, 0}) {
					t.Fatalf("hidden vertex must not appear in output: %s", p.String())
				}
			}
		}
		if math.Abs(math.Abs(out[0][1].X)-0.200160192256) > 1e-8 ||
			math.Abs(math.Abs(out[1][0].X)-0.200160192256) > 1e-8 {
			t.Fatalf("cut boundaries wrong: %s | %s", out[0].String(), out[1].String())
		}
	})

	t.Run("ContainedIntervalsMerge", func(t *testing.T) {
		// The small sphere's shadow is fully contained in the big one's.
		spheres := []*Sphere{
			NewSphere(Vector{0, 0, 2.5}, 0.15),
			NewSphere(Vector{0, 0, 2.5}, 0.05),
		}
		out := clipScene(t, eye, spheres, Paths{line})
		if len(out) != 2 {
			t.Fatalf("contained shadow must not create extra pieces, got %d", len(out))
		}
		want := math.Sqrt((0.15 * 0.15 * 25) / (2.5*2.5 - 0.15*0.15))
		if math.Abs(math.Abs(out[0][1].X)-want) > 1e-8 ||
			math.Abs(math.Abs(out[1][0].X)-want) > 1e-8 {
			t.Fatalf("boundaries should match the outer sphere %v, got %s | %s", want, out[0].String(), out[1].String())
		}
		checkLineAgainstReference(t, eye, line[0], line[1], spheres, out)
	})

	t.Run("RandomizedAgreement", func(t *testing.T) {
		rng := rand.New(rand.NewSource(42))
		trials := 0
		for trial := 0; trial < 400 && trials < 200; trial++ {
			n := 1 + rng.Intn(3)
			var spheres []*Sphere
			valid := true
			for i := 0; i < n; i++ {
				center := Vector{rng.NormFloat64() * 2, rng.NormFloat64() * 2, 0.5 + rng.Float64()*4}
				radius := 0.02 + rng.Float64()*0.4
				if eye.Distance(center) <= radius {
					valid = false
					break
				}
				spheres = append(spheres, NewSphere(center, radius))
			}
			if !valid {
				continue
			}
			a := Vector{rng.NormFloat64(), rng.NormFloat64(), rng.NormFloat64() * 0.5}
			b := Vector{rng.NormFloat64(), rng.NormFloat64(), rng.NormFloat64() * 0.5}
			if a == b {
				continue
			}
			trials++
			out := clipScene(t, eye, spheres, Paths{{a, b}})
			for _, p := range out {
				if len(p) < 2 {
					t.Fatalf("trial %d emitted a path with %d vertices", trials, len(p))
				}
			}
			checkLineAgainstReference(t, eye, a, b, spheres, out)
		}
		if trials < 200 {
			t.Fatalf("expected 200 valid random trials, got %d", trials)
		}
	})

	t.Run("PointAtEyeIsVisible", func(t *testing.T) {
		sphere := NewSphere(Vector{0, 0, 2.5}, 0.1)
		// The ray from the eye straight down passes through the sphere, but
		// the vertex at the eye itself is visible.
		out := clipScene(t, eye, []*Sphere{sphere}, Paths{{eye, {0, 0, 0}}})
		if len(out) != 1 || out[0][0] != eye || len(out[0]) != 2 {
			t.Fatalf("segment starting at the eye should keep it as a visible vertex, got %s", out.String())
		}
	})

	t.Run("WritesVisibilitySVG", func(t *testing.T) {
		sphere := NewSphere(Vector{0, 0, 2.5}, 0.1)
		out, err := (&Scene{Shapes: []Shape{sphere}}).ClipVisiblePaths(Paths{line}, eye)
		if err != nil {
			t.Fatal(err)
		}
		map2D := func(p Vector, xmin, xmax, y float64) Vector {
			u := (p.X - xmin) / (xmax - xmin)
			return Vector{40 + u*720, y, 0}
		}
		// clipX keeps the parts of a 3D polyline whose x lies in [xmin, xmax].
		clipX := func(p Path, xmin, xmax float64) Path {
			var kept Path
			for i := 0; i+1 < len(p); i++ {
				a, b := p[i], p[i+1]
				lo, hi := math.Min(a.X, b.X), math.Max(a.X, b.X)
				lo, hi = math.Max(lo, xmin), math.Min(hi, xmax)
				if lo > hi {
					continue
				}
				at := func(x float64) Vector {
					u := (x - a.X) / (b.X - a.X)
					return a.Add(b.Sub(a).MulScalar(u))
				}
				kept = append(kept, at(lo), at(hi))
			}
			return kept
		}
		var rows Paths
		for _, p := range out {
			full, zoom := Path{}, Path{}
			for _, v := range p {
				full = append(full, map2D(v, -1.1, 1.1, 300))
			}
			for _, v := range clipX(p, -0.25, 0.25) {
				zoom = append(zoom, map2D(v, -0.25, 0.25, 120))
			}
			rows = append(rows, full)
			if len(zoom) >= 2 {
				rows = append(rows, zoom)
			}
		}
		dir := filepath.Join("..", "test", "out")
		if err := os.MkdirAll(dir, 0755); err != nil {
			t.Fatal(err)
		}
		svgPath := filepath.Join(dir, "visibility.svg")
		if err := rows.WriteToSVG(svgPath, 800, 400); err != nil {
			t.Fatal(err)
		}
		data, err := os.ReadFile(svgPath)
		if err != nil {
			t.Fatal(err)
		}
		svg := string(data)
		if !strings.Contains(svg, "<svg") || strings.Count(svg, "<polyline") != 4 {
			t.Fatalf("visibility.svg should contain 4 polylines (2 rows x 2 gap pieces), got:\n%s", svg)
		}
	})
}
