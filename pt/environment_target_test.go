package pt

import (
	"math"
	"math/rand"
	"sync"
	"testing"
)

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

func envMustBuild(t *testing.T, w, h int, pixels []Color) *EnvironmentMap {
	t.Helper()
	m, err := NewEnvironmentMap(w, h, pixels)
	if err != nil {
		t.Fatalf("NewEnvironmentMap(%d, %d): %v", w, h, err)
	}
	if m == nil {
		t.Fatalf("NewEnvironmentMap(%d, %d): nil map", w, h)
	}
	return m
}

func envConstantPixels(w, h int, c Color) []Color {
	pixels := make([]Color, w*h)
	for i := range pixels {
		pixels[i] = c
	}
	return pixels
}

// envSingleCellPixels returns a 64x32 map where only the northern-hemisphere
// cell (row 8, column 16) is lit, plus the radiance of that cell. Row 8 of 32
// spans latitudes ~39-45 degrees: far from both the pole and the horizon.
func envSingleCellPixels() ([]Color, Color) {
	const w, h = 64, 32
	lit := Color{1.0, 0.8, 0.6}
	pixels := make([]Color, w*h)
	pixels[8*w+16] = lit
	return pixels, lit
}

// envDirection converts texture-space (u, v) and rotation angle to a unit
// direction, mirroring the documented mapping.
func envDirection(u, v, angle float64) Vector {
	a := 2*math.Pi*u - math.Pi - angle
	y := math.Cos(math.Pi * v)
	s := math.Sqrt(math.Max(0, 1-y*y))
	return Vector{s * math.Cos(a), y, s * math.Sin(a)}
}

// envCellOf locates the texel of a direction using only the documented
// mapping (independent of the implementation).
func envCellOf(w, h int, d Vector, angle float64) (col, row int) {
	u := (math.Atan2(d.Z, d.X) + angle + math.Pi) / (2 * math.Pi)
	u -= math.Floor(u)
	v := math.Acos(math.Max(-1, math.Min(1, d.Y))) / math.Pi
	col = int(math.Floor(u * float64(w)))
	if col >= w {
		col = w - 1
	}
	row = int(math.Floor(v * float64(h)))
	if row >= h {
		row = h - 1
	}
	return col, row
}

// envIndependentPDF recomputes the solid-angle PDF straight from raw pixels:
// texel probability proportional to luminance times true spherical area,
// uniform within the texel.
func envIndependentPDF(pixels []Color, w, h int, d Vector, angle float64) float64 {
	total := 0.0
	for r := 0; r < h; r++ {
		area := 2 * math.Pi / float64(w) *
			(math.Cos(math.Pi*float64(r)/float64(h)) - math.Cos(math.Pi*float64(r+1)/float64(h)))
		for c := 0; c < w; c++ {
			p := pixels[r*w+c]
			total += (0.2126*p.R + 0.7152*p.G + 0.0722*p.B) * area
		}
	}
	if total == 0 {
		return 1 / (4 * math.Pi)
	}
	col, row := envCellOf(w, h, d, angle)
	p := pixels[row*w+col]
	lum := 0.2126*p.R + 0.7152*p.G + 0.0722*p.B
	return lum / total
}

// envIntegratePDF numerically integrates the map PDF over the sphere with a
// midpoint quadrature on an independent qw x qh grid.
func envIntegratePDF(m *EnvironmentMap, angle float64, qw, qh int) float64 {
	total := 0.0
	for j := 0; j < qh; j++ {
		cos0 := math.Cos(math.Pi * float64(j) / float64(qh))
		cos1 := math.Cos(math.Pi * float64(j+1) / float64(qh))
		area := 2 * math.Pi / float64(qw) * (cos0 - cos1)
		y := (cos0 + cos1) / 2
		s := math.Sqrt(math.Max(0, 1-y*y))
		for i := 0; i < qw; i++ {
			a := 2*math.Pi*(float64(i)+0.5)/float64(qw) - math.Pi
			d := Vector{s * math.Cos(a), y, s * math.Sin(a)}
			total += m.PDF(d, angle) * area
		}
	}
	return total
}

// envCellFraction returns the cosine-weighted solid angle of a texel row band
// cell divided by pi: dPhi*(sin^2(latUp)-sin^2(latLo))/(2*pi). This is the
// probability that a cosine-weighted bounce from a +Y surface lands in the
// cell, so the expected exit color is albedo * cellRGB * fraction.
func envCellFraction(w, h, row int) float64 {
	dPhi := 2 * math.Pi / float64(w)
	latUp := math.Pi/2 - math.Pi*float64(row)/float64(h)
	latLo := math.Pi/2 - math.Pi*float64(row+1)/float64(h)
	su, sl := math.Sin(latUp), math.Sin(latLo)
	return dPhi * (su*su - sl*sl) / (2 * math.Pi)
}

func envPlaneScene(env *EnvironmentMap, albedo Color, angle float64) *Scene {
	scene := &Scene{Environment: env, TextureAngle: angle}
	scene.Add(NewPlane(Vector{}, Vector{0, 1, 0}, DiffuseMaterial(albedo)))
	scene.Compile()
	return scene
}

// envPlaneSceneExtra builds a plane scene with additional shapes, compiling
// the k-d tree only after every shape has been added.
func envPlaneSceneExtra(env *EnvironmentMap, albedo Color, angle float64, extra ...Shape) *Scene {
	scene := &Scene{Environment: env, TextureAngle: angle}
	scene.Add(NewPlane(Vector{}, Vector{0, 1, 0}, DiffuseMaterial(albedo)))
	for _, shape := range extra {
		scene.Add(shape)
	}
	scene.Compile()
	return scene
}

// envMeanColor averages calls sampler.Sample results, one fresh fixed-seed
// RNG per call.
func envMeanColor(scene *Scene, sampler *DefaultSampler, ray Ray, calls int, seedBase int64) Color {
	sum := Black
	for i := 0; i < calls; i++ {
		rnd := rand.New(rand.NewSource(seedBase + int64(i)))
		sum = sum.Add(sampler.Sample(scene, ray, rnd))
	}
	return sum.DivScalar(float64(calls))
}

func envRelErr(got, want Color) float64 {
	err := 0.0
	for _, pair := range [][2]float64{{got.R, want.R}, {got.G, want.G}, {got.B, want.B}} {
		d := math.Abs(pair[0] - pair[1])
		if pair[1] != 0 {
			d /= math.Abs(pair[1])
		}
		if d > err {
			err = d
		}
	}
	return err
}

func envCheckClose(t *testing.T, what string, got, want Color, tol float64) {
	t.Helper()
	if err := envRelErr(got, want); err > tol {
		t.Errorf("%s: got %+v want %+v (rel err %.4f > %.4f)", what, got, want, err, tol)
	}
}

func envFiniteColor(c Color) bool {
	for _, x := range []float64{c.R, c.G, c.B} {
		if math.IsNaN(x) || math.IsInf(x, 0) {
			return false
		}
	}
	return true
}

// ---------------------------------------------------------------------------
// distribution tests
// ---------------------------------------------------------------------------

func TestEnvironmentDistribution(t *testing.T) {
	t.Run("RejectsBadInput", func(t *testing.T) {
		good := envConstantPixels(2, 2, Color{1, 1, 1})
		cases := []struct {
			name   string
			w, h   int
			pixels []Color
		}{
			{"zero width", 0, 2, good},
			{"zero height", 2, 0, good},
			{"negative size", -1, 2, good},
			{"overflow", math.MaxInt/2 - 1, 3, nil},
			{"overflow swapped", 3, math.MaxInt/2 - 1, nil},
			{"short", 2, 2, good[:3]},
			{"long", 2, 2, append(append([]Color{}, good...), Color{1, 1, 1})},
			{"negative component", 2, 2, []Color{{1, 1, 1}, {-1e-9, 0, 0}, {0, 0, 0}, {0, 0, 0}}},
			{"NaN", 2, 2, []Color{{1, 1, 1}, {math.NaN(), 0, 0}, {0, 0, 0}, {0, 0, 0}}},
			{"+Inf", 2, 2, []Color{{1, 1, 1}, {math.Inf(1), 0, 0}, {0, 0, 0}, {0, 0, 0}}},
			{"-Inf", 2, 2, []Color{{1, 1, 1}, {0, math.Inf(-1), 0}, {0, 0, 0}, {0, 0, 0}}},
		}
		for _, c := range cases {
			m, err := NewEnvironmentMap(c.w, c.h, c.pixels)
			if err == nil || m != nil {
				t.Errorf("%s: expected nil map and error, got map=%v err=%v", c.name, m, err)
			}
		}
	})

	t.Run("Normalization", func(t *testing.T) {
		// pseudo-random luminance pattern, deterministic
		const w, h = 64, 32
		pixels := make([]Color, w*h)
		for i := range pixels {
			x := math.Sin(float64(i)*12.9898) * 43758.5453
			lum := 0.05 + (x - math.Floor(x))
			pixels[i] = Color{lum, 0.5 * lum, 0.25 * lum}
		}
		m := envMustBuild(t, w, h, pixels)
		for _, angle := range []float64{0, 0.7, -2.3} {
			got := envIntegratePDF(m, angle, 4*w, 4*h)
			if math.Abs(got-1) > 1e-9 {
				t.Errorf("angle=%v: spherical integral of PDF = %.12f, want 1", angle, got)
			}
		}
	})

	t.Run("CellFrequencies", func(t *testing.T) {
		const w, h = 16, 8
		pixels := make([]Color, w*h)
		for i := range pixels {
			x := math.Sin(float64(i)*78.233) * 43758.5453
			lum := 0.05 + (x - math.Floor(x))
			pixels[i] = Color{lum, lum, lum}
		}
		m := envMustBuild(t, w, h, pixels)
		const n = 1000000
		counts := make([]int, w*h)
		rnd := rand.New(rand.NewSource(42))
		for i := 0; i < n; i++ {
			d, _, _ := m.Sample(rnd, 0.25)
			col, row := envCellOf(w, h, d, 0.25)
			counts[row*w+col]++
		}
		for r := 0; r < h; r++ {
			area := 2 * math.Pi / float64(w) *
				(math.Cos(math.Pi*float64(r)/float64(h)) - math.Cos(math.Pi*float64(r+1)/float64(h)))
			for c := 0; c < w; c++ {
				i := r*w + c
				// expected probability from the map's own PDF: p = pdf * area
				d := envDirection((float64(c)+0.5)/float64(w), (float64(r)+0.5)/float64(h), 0.25)
				exp := m.PDF(d, 0.25) * area
				obs := float64(counts[i]) / n
				sigma := math.Sqrt(exp * (1 - exp) / n)
				if math.Abs(obs-exp) > 6*sigma+1e-9 {
					t.Errorf("cell (%d,%d): observed %.7f expected %.7f (6sigma=%.7f)", c, r, obs, exp, 6*sigma)
				}
			}
		}
	})

	t.Run("WithinCellUniform", func(t *testing.T) {
		pixels, _ := envSingleCellPixels()
		m := envMustBuild(t, 64, 32, pixels)
		const n = 100000
		rnd := rand.New(rand.NewSource(7))
		cos0 := math.Cos(math.Pi * 8 / 32)
		cos1 := math.Cos(math.Pi * 9 / 32)
		a0 := 2*math.Pi*16/64 - math.Pi
		a1 := 2*math.Pi*17/64 - math.Pi
		sumY, sumA := 0.0, 0.0
		minY, maxY := 1.0, -1.0
		minA, maxA := math.Inf(1), math.Inf(-1)
		for i := 0; i < n; i++ {
			d, _, pdf := m.Sample(rnd, 0)
			if math.Abs(d.Length()-1) > 1e-9 {
				t.Fatalf("non-unit direction %+v", d)
			}
			col, row := envCellOf(64, 32, d, 0)
			if col != 16 || row != 8 {
				t.Fatalf("sample escaped the single lit cell: col=%d row=%d", col, row)
			}
			if pdf <= 0 || math.IsNaN(pdf) || math.IsInf(pdf, 0) {
				t.Fatalf("bad pdf %v", pdf)
			}
			y := d.Y
			a := math.Atan2(d.Z, d.X)
			sumY += y
			sumA += a
			minY = math.Min(minY, y)
			maxY = math.Max(maxY, y)
			minA = math.Min(minA, a)
			maxA = math.Max(maxA, a)
		}
		// cos(theta) must be uniform in [cos1, cos0], longitude uniform in [a0, a1]
		meanY := sumY / n
		wantY := (cos0 + cos1) / 2
		sigmaY := (cos0 - cos1) / math.Sqrt(12*n)
		if math.Abs(meanY-wantY) > 5*sigmaY {
			t.Errorf("mean cos(theta) %.6f want %.6f (5sigma=%.6f)", meanY, wantY, 5*sigmaY)
		}
		meanA := sumA / n
		wantA := (a0 + a1) / 2
		sigmaA := (a1 - a0) / math.Sqrt(12*n)
		if math.Abs(meanA-wantA) > 5*sigmaA {
			t.Errorf("mean longitude %.6f want %.6f (5sigma=%.6f)", meanA, wantA, 5*sigmaA)
		}
		if minY < cos1-1e-12 || maxY > cos0+1e-12 {
			t.Errorf("cos(theta) out of texel band: [%.6f, %.6f] not in [%.6f, %.6f]", minY, maxY, cos1, cos0)
		}
		if minA < a0-1e-9 || maxA > a1+1e-9 {
			t.Errorf("longitude out of texel band: [%.6f, %.6f] not in [%.6f, %.6f]", minA, maxA, a0, a1)
		}
		if maxY-minY < 0.9*(cos0-cos1) || maxA-minA < 0.9*(a1-a0) {
			t.Errorf("samples not spread over the texel: dY=%.4f dA=%.4f", maxY-minY, maxA-minA)
		}
	})

	t.Run("ForwardBackwardPDF", func(t *testing.T) {
		const w, h = 32, 16
		pixels := make([]Color, w*h)
		for i := range pixels {
			x := math.Sin(float64(i)*3.7) * 12345.6789
			lum := 0.1 + (x - math.Floor(x))
			pixels[i] = Color{lum, 0.3 * lum, 0.7 * lum}
		}
		m := envMustBuild(t, w, h, pixels)
		const angle = 0.45
		// query PDF against an independent recomputation from raw pixels
		rnd := rand.New(rand.NewSource(11))
		for i := 0; i < 10000; i++ {
			y := rnd.Float64()*2 - 1
			a := rnd.Float64() * 2 * math.Pi
			s := math.Sqrt(math.Max(0, 1-y*y))
			d := Vector{s * math.Cos(a), y, s * math.Sin(a)}
			got := m.PDF(d, angle)
			want := envIndependentPDF(pixels, w, h, d, angle)
			if math.Abs(got-want) > 1e-9*want {
				t.Fatalf("PDF(%+v) = %.12g, independent recompute %.12g", d, got, want)
			}
		}
		// pdf returned by Sample must match the PDF query at the same direction
		checked := 0
		for i := 0; i < 50000; i++ {
			d, _, pdf := m.Sample(rnd, angle)
			u := (math.Atan2(d.Z, d.X) + angle + math.Pi) / (2 * math.Pi)
			u -= math.Floor(u)
			v := math.Acos(math.Max(-1, math.Min(1, d.Y))) / math.Pi
			fu := u*float64(w) - math.Floor(u*float64(w))
			fv := v*float64(h) - math.Floor(v*float64(h))
			if math.Min(fu, 1-fu) < 1e-9 || math.Min(fv, 1-fv) < 1e-9 {
				continue // on a texel boundary; cell lookup may round either way
			}
			checked++
			if got := m.PDF(d, angle); got != pdf {
				t.Fatalf("Sample pdf %.12g but PDF query gives %.12g", pdf, got)
			}
		}
		if checked < 49000 {
			t.Fatalf("too many boundary skips: %d", 50000-checked)
		}
	})

	t.Run("RotationAndSeam", func(t *testing.T) {
		const w, h = 8, 4
		pixels := make([]Color, w*h)
		for i := range pixels {
			pixels[i] = Color{float64(i + 1), 0, 0}
		}
		m := envMustBuild(t, w, h, pixels)
		// rotating by `angle` must shift the lookup by exactly that angle
		const angle = 0.7
		for c := 0; c < w; c++ {
			for r := 0; r < h; r++ {
				u := (float64(c) + 0.5) / float64(w)
				v := (float64(r) + 0.5) / float64(h)
				d0 := envDirection(u, v, 0)
				d1 := envDirection(u+angle/(2*math.Pi), v, 0)
				a := m.Evaluate(d0, angle)
				b := m.Evaluate(d1, 0)
				if a != b {
					t.Errorf("rotation mismatch at cell (%d,%d): %v vs %v", c, r, a, b)
				}
				pa := m.PDF(d0, angle)
				pb := m.PDF(d1, 0)
				if pa != pb {
					t.Errorf("rotation PDF mismatch at cell (%d,%d): %v vs %v", c, r, pa, pb)
				}
			}
		}
		// the longitude seam: azimuth just below +pi and just above -pi must
		// land in the last and first columns respectively
		y := 0.0 // v = 0.5 -> row 2 of 4
		s := 1.0
		dPlus := Vector{s * math.Cos(math.Pi-1e-7), y, s * math.Sin(math.Pi-1e-7)}
		dMinus := Vector{s * math.Cos(-math.Pi+1e-7), y, s * math.Sin(-math.Pi+1e-7)}
		if got, want := m.Evaluate(dPlus, 0), pixels[2*w+w-1]; got != want {
			t.Errorf("seam+: got %v want %v", got, want)
		}
		if got, want := m.Evaluate(dMinus, 0), pixels[2*w+0]; got != want {
			t.Errorf("seam-: got %v want %v", got, want)
		}
		// texels are constant: two directions inside one texel agree exactly
		d1 := envDirection(0.26, 0.4, 0)
		d2 := envDirection(0.27, 0.44, 0)
		if m.Evaluate(d1, 0) != m.Evaluate(d2, 0) {
			t.Errorf("texel not constant: %v vs %v", m.Evaluate(d1, 0), m.Evaluate(d2, 0))
		}
	})

	t.Run("AllBlack", func(t *testing.T) {
		m := envMustBuild(t, 8, 4, make([]Color, 32))
		rnd := rand.New(rand.NewSource(3))
		want := 1 / (4 * math.Pi)
		for i := 0; i < 10000; i++ {
			d, radiance, pdf := m.Sample(rnd, 0.9)
			if pdf != want {
				t.Fatalf("uniform fallback pdf = %v, want %v", pdf, want)
			}
			if radiance != Black {
				t.Fatalf("black map radiance = %v", radiance)
			}
			if math.Abs(d.Length()-1) > 1e-9 {
				t.Fatalf("non-unit direction %+v", d)
			}
			if got := m.PDF(d, 0.9); got != want {
				t.Fatalf("uniform fallback PDF query = %v, want %v", got, want)
			}
		}
		if got := m.Evaluate(Vector{0, 1, 0}, 0); got != Black {
			t.Fatalf("black map Evaluate = %v", got)
		}
	})

	t.Run("SingleTexel", func(t *testing.T) {
		m := envMustBuild(t, 1, 1, []Color{{2, 3, 4}})
		want := 1 / (4 * math.Pi)
		rnd := rand.New(rand.NewSource(5))
		for i := 0; i < 1000; i++ {
			d, radiance, pdf := m.Sample(rnd, 1.1)
			if math.Abs(pdf-want) > 1e-12 {
				t.Fatalf("1x1 pdf = %v, want %v", pdf, want)
			}
			if radiance != (Color{2, 3, 4}) {
				t.Fatalf("1x1 radiance = %v", radiance)
			}
			if math.Abs(d.Length()-1) > 1e-9 {
				t.Fatalf("non-unit direction %+v", d)
			}
		}
		if got := envIntegratePDF(m, 0.3, 64, 32); math.Abs(got-1) > 1e-9 {
			t.Fatalf("1x1 normalization = %v", got)
		}
	})

	t.Run("PolarTexels", func(t *testing.T) {
		const w, h = 4, 64
		pixels := make([]Color, w*h)
		for c := 0; c < w; c++ {
			pixels[c] = Color{1, 1, 1} // north-pole row only
		}
		m := envMustBuild(t, w, h, pixels)
		if got := envIntegratePDF(m, 0, 8*w, 2*h); math.Abs(got-1) > 1e-9 {
			t.Fatalf("polar normalization = %v", got)
		}
		rnd := rand.New(rand.NewSource(9))
		cosBoundary := math.Cos(math.Pi / h)
		for i := 0; i < 10000; i++ {
			d, radiance, pdf := m.Sample(rnd, 0)
			if radiance != (Color{1, 1, 1}) {
				t.Fatalf("polar sample radiance = %v", radiance)
			}
			if d.Y < cosBoundary-1e-12 {
				t.Fatalf("polar sample outside row band: y=%v", d.Y)
			}
			if pdf <= 0 || math.IsNaN(pdf) || math.IsInf(pdf, 0) {
				t.Fatalf("polar pdf = %v", pdf)
			}
		}
	})

	t.Run("InputSnapshot", func(t *testing.T) {
		pixels := []Color{{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {10, 11, 12}}
		m := envMustBuild(t, 2, 2, pixels)
		pixels[0] = Color{100, 100, 100}
		pixels[3] = Color{200, 200, 200}
		d := envDirection(0.25, 0.25, 0) // texel (0,0)
		if got := m.Evaluate(d, 0); got != (Color{1, 2, 3}) {
			t.Fatalf("map borrowed input slice: Evaluate = %v", got)
		}
	})

	t.Run("ExtremeDynamicRange", func(t *testing.T) {
		pixels := []Color{
			{1e-200, 1e-200, 1e-200}, {1e200, 1e200, 1e200},
			{1e-150, 1e-150, 1e-150}, {1e150, 1e150, 1e150},
			{1e-200, 1e-200, 1e-200}, {1e-200, 1e-200, 1e-200},
			{1e-200, 1e-200, 1e-200}, {1e-200, 1e-200, 1e-200},
		}
		m := envMustBuild(t, 4, 2, pixels)
		if got := envIntegratePDF(m, 0, 64, 32); math.Abs(got-1) > 1e-9 {
			t.Fatalf("extreme-range normalization = %v", got)
		}
		rnd := rand.New(rand.NewSource(13))
		bright := 0
		for i := 0; i < 20000; i++ {
			d, _, pdf := m.Sample(rnd, 0)
			if pdf <= 0 || math.IsNaN(pdf) || math.IsInf(pdf, 0) {
				t.Fatalf("extreme-range pdf = %v", pdf)
			}
			if math.Abs(d.Length()-1) > 1e-9 {
				t.Fatalf("non-unit direction %+v", d)
			}
			col, row := envCellOf(4, 2, d, 0)
			if row == 0 && (col == 1 || col == 3) {
				bright++
			}
		}
		if bright < 19000 {
			t.Fatalf("bright texels should dominate sampling: %d/20000", bright)
		}
		// a uniformly tiny map must not degenerate either
		tiny := envConstantPixels(4, 2, Color{1e-200, 1e-200, 1e-200})
		mt := envMustBuild(t, 4, 2, tiny)
		if got := envIntegratePDF(mt, 0, 64, 32); math.Abs(got-1) > 1e-9 {
			t.Fatalf("tiny-range normalization = %v", got)
		}
		want := 1 / (4 * math.Pi)
		if got := mt.PDF(Vector{0, 1, 0}, 0); math.Abs(got-want) > 1e-9*want {
			t.Fatalf("tiny-range PDF = %v, want %v", got, want)
		}
	})

	t.Run("QueriesConsumeNoRandomness", func(t *testing.T) {
		pixels, _ := envSingleCellPixels()
		m := envMustBuild(t, 64, 32, pixels)
		rndA := rand.New(rand.NewSource(17))
		rndB := rand.New(rand.NewSource(17))
		d1, r1, p1 := m.Sample(rndA, 0.2)
		for i := 0; i < 100; i++ {
			m.Evaluate(Vector{0, 1, 0}, 0.2)
			m.PDF(Vector{0, 1, 0}, 0.2)
		}
		d2, r2, p2 := m.Sample(rndB, 0.2)
		if d1 != d2 || r1 != r2 || p1 != p2 {
			t.Fatalf("queries consumed randomness: %+v/%v/%v vs %+v/%v/%v", d1, r1, p1, d2, r2, p2)
		}
	})
}

// ---------------------------------------------------------------------------
// lighting tests against analytic references
// ---------------------------------------------------------------------------

func TestEnvironmentMIS(t *testing.T) {
	albedo := Color{0.4, 0.5, 0.6}
	sky := Color{0.3, 0.6, 0.9}
	ray := Ray{Vector{0, 1, 0}, Vector{0, -1, 0}}

	t.Run("ConstantSky", func(t *testing.T) {
		m := envMustBuild(t, 8, 4, envConstantPixels(8, 4, sky))
		scene := envPlaneScene(m, albedo, 0)
		want := albedo.Mul(sky)
		// without MIS every bounce escapes to the constant sky: exact
		off := NewSampler(4, 1)
		got := envMeanColor(scene, off, ray, 2000, 1000)
		envCheckClose(t, "constant sky, MIS off", got, want, 1e-9)
		// with MIS the same analytic limit must be reached
		on := NewSampler(4, 1)
		on.EnvironmentMIS = true
		got = envMeanColor(scene, on, ray, 100000, 2000)
		envCheckClose(t, "constant sky, MIS on", got, want, 0.02)
	})

	t.Run("SingleBrightCell", func(t *testing.T) {
		pixels, lit := envSingleCellPixels()
		m := envMustBuild(t, 64, 32, pixels)
		scene := envPlaneScene(m, albedo, 0)
		want := albedo.Mul(lit).MulScalar(envCellFraction(64, 32, 8))
		on := NewSampler(4, 1)
		on.EnvironmentMIS = true
		got := envMeanColor(scene, on, ray, 100000, 3000)
		envCheckClose(t, "single cell, MIS on", got, want, 0.02)
		// cosine-only convergence needs many more samples
		off := NewSampler(16, 1)
		got = envMeanColor(scene, off, ray, 1500000, 7000)
		envCheckClose(t, "single cell, MIS off", got, want, 0.02)
	})

	t.Run("VarianceReduction", func(t *testing.T) {
		pixels, _ := envSingleCellPixels()
		m := envMustBuild(t, 64, 32, pixels)
		scene := envPlaneScene(m, albedo, 0)
		const seeds = 16
		const calls = 8192
		batchMeans := func(mis bool) []Color {
			sampler := NewSampler(1, 1)
			sampler.EnvironmentMIS = mis
			means := make([]Color, seeds)
			for i := 0; i < seeds; i++ {
				means[i] = envMeanColor(scene, sampler, ray, calls, int64(50000+i*1000000))
			}
			return means
		}
		variance := func(means []Color) float64 {
			mean := Black
			for _, c := range means {
				mean = mean.Add(c)
			}
			mean = mean.DivScalar(float64(len(means)))
			v := 0.0
			for _, c := range means {
				d := c.Sub(mean)
				v += d.R*d.R + d.G*d.G + d.B*d.B
			}
			return v / float64(len(means)-1)
		}
		on := batchMeans(true)
		off := batchMeans(false)
		varOn, varOff := variance(on), variance(off)
		if varOn <= 0 || varOff <= 0 {
			t.Fatalf("degenerate variances: on=%g off=%g", varOn, varOff)
		}
		ratio := varOff / varOn
		if ratio < 4 {
			t.Fatalf("variance reduction %.2fx < 4x (on=%g off=%g)", ratio, varOn, varOff)
		}
		t.Logf("variance reduction: %.1fx (on=%.3g off=%.3g)", ratio, varOn, varOff)
	})

	t.Run("Occluder", func(t *testing.T) {
		pixels, lit := envSingleCellPixels()
		m := envMustBuild(t, 64, 32, pixels)
		want := albedo.Mul(lit).MulScalar(envCellFraction(64, 32, 8))
		// direction to the center of the lit cell
		dc := envDirection(16.5/64, 8.5/32, 0)
		blocker := NewSphere(dc.MulScalar(2), 0.4, DiffuseMaterial(Black))
		scene := envPlaneSceneExtra(m, albedo, 0, blocker)
		on := NewSampler(4, 1)
		on.EnvironmentMIS = true
		got := envMeanColor(scene, on, ray, 20000, 90000)
		if got.R > 1e-9 || got.G > 1e-9 || got.B > 1e-9 {
			t.Errorf("occluded result = %+v, want ~black", got)
		}
		// an occluder on the opposite side must not change the estimate
		shifted := NewSphere(Vector{-dc.X, dc.Y, -dc.Z}.MulScalar(2), 0.4, DiffuseMaterial(Black))
		scene2 := envPlaneSceneExtra(m, albedo, 0, shifted)
		got = envMeanColor(scene2, on, ray, 100000, 95000)
		envCheckClose(t, "shifted occluder", got, want, 0.02)
	})
}

// ---------------------------------------------------------------------------
// branch behavior tests
// ---------------------------------------------------------------------------

func TestEnvironmentBranches(t *testing.T) {
	albedo := Color{0.4, 0.5, 0.6}
	sky := Color{0.3, 0.6, 0.9}
	ray := Ray{Vector{0, 1, 0}, Vector{0, -1, 0}}

	t.Run("TwoDiffuseBounces", func(t *testing.T) {
		m := envMustBuild(t, 8, 4, envConstantPixels(8, 4, sky))
		newScene := func() *Scene {
			return envPlaneSceneExtra(m, albedo, 0,
				NewSphere(Vector{0, 2.5, 0}, 1, DiffuseMaterial(Color{0.5, 0.5, 0.5})))
		}
		on := NewSampler(4, 2)
		on.EnvironmentMIS = true
		off := NewSampler(4, 2)
		gotOn := envMeanColor(newScene(), on, ray, 100000, 11000)
		gotOff := envMeanColor(newScene(), off, ray, 100000, 12000)
		if !envFiniteColor(gotOn) || !envFiniteColor(gotOff) {
			t.Fatalf("non-finite results: on=%v off=%v", gotOn, gotOff)
		}
		// both estimators are unbiased for the same two-bounce integral
		if err := envRelErr(gotOn, gotOff); err > 0.05 {
			t.Errorf("MIS on/off disagree: on=%+v off=%+v (rel err %.4f)", gotOn, gotOff, err)
		}
		// sanity: a gray sphere (albedo 0.5) can only darken the open-sky value
		open := albedo.Mul(sky)
		if gotOn.R > open.R*1.05 || gotOn.R < open.R*0.2 {
			t.Errorf("two-bounce result out of sane range: got %+v open-sky %+v", gotOn, open)
		}
	})

	t.Run("SpecularEscapeUnweighted", func(t *testing.T) {
		// 4x2 map with a distinct color per texel
		pixels := make([]Color, 8)
		for i := range pixels {
			pixels[i] = Color{float64(i+1) / 10, float64(i+2) / 10, float64(i+3) / 10}
		}
		m := envMustBuild(t, 4, 2, pixels)
		scene := &Scene{Environment: m}
		scene.Add(NewPlane(Vector{}, Vector{0, 1, 0}, MetallicMaterial(White, 0, 0)))
		scene.Compile()
		sampler := NewSampler(1, 1)
		sampler.EnvironmentMIS = true
		// straight down -> straight up (the pole)
		up := Ray{Vector{0, 1, 0}, Vector{0, -1, 0}}
		got := sampler.Sample(scene, up, rand.New(rand.NewSource(1)))
		if want := m.Evaluate(Vector{0, 1, 0}, 0); got != want {
			t.Errorf("mirror sky = %+v, want %+v", got, want)
		}
		// angled ray -> deterministic reflected direction
		d := Vector{-1, -1, 0}.Normalize()
		got = sampler.Sample(scene, Ray{Vector{1, 1, 0}, d}, rand.New(rand.NewSource(2)))
		refl := Vector{-1, 1, 0}.Normalize()
		if want := m.Evaluate(refl, 0); got != want {
			t.Errorf("angled mirror sky = %+v, want %+v", got, want)
		}
	})

	t.Run("RefractionEscapeUnweighted", func(t *testing.T) {
		m := envMustBuild(t, 8, 4, envConstantPixels(8, 4, sky))
		newScene := func() *Scene {
			scene := &Scene{Environment: m}
			scene.Add(NewSphere(Vector{}, 1, ClearMaterial(1.5, 0)))
			scene.Compile()
			return scene
		}
		through := Ray{Vector{0, 0, 5}, Vector{0, 0, -1}}
		on := NewSampler(1, 8)
		on.EnvironmentMIS = true
		off := NewSampler(1, 8)
		// transparent branches never trigger environment sampling, so both
		// configurations must consume identical random streams
		gotOn := envMeanColor(newScene(), on, through, 2000, 21000)
		gotOff := envMeanColor(newScene(), off, through, 2000, 21000)
		if gotOn != gotOff {
			t.Errorf("refraction: MIS on/off differ: %+v vs %+v", gotOn, gotOff)
		}
		envCheckClose(t, "refraction sky", gotOn, sky, 0.02)
	})

	t.Run("DepthLimit", func(t *testing.T) {
		// at the bounce limit only the environment strategy remains (weight 1)
		m := envMustBuild(t, 8, 4, envConstantPixels(8, 4, sky))
		scene := envPlaneScene(m, albedo, 0)
		on := NewSampler(4, 0)
		on.EnvironmentMIS = true
		got := envMeanColor(scene, on, ray, 100000, 31000)
		envCheckClose(t, "depth limit, constant sky", got, albedo.Mul(sky), 0.02)
		// same for the single bright cell: pure environment sampling hits the
		// same analytic reference
		pixels, lit := envSingleCellPixels()
		m1 := envMustBuild(t, 64, 32, pixels)
		scene1 := envPlaneScene(m1, albedo, 0)
		want := albedo.Mul(lit).MulScalar(envCellFraction(64, 32, 8))
		got = envMeanColor(scene1, on, ray, 100000, 32000)
		envCheckClose(t, "depth limit, single cell", got, want, 0.02)
		// without MIS nothing can be collected at the depth limit
		off := NewSampler(4, 0)
		if got := envMeanColor(scene1, off, ray, 100, 33000); got != Black {
			t.Errorf("depth limit without MIS = %+v, want black", got)
		}
	})

	t.Run("SpecularModes", func(t *testing.T) {
		m := envMustBuild(t, 8, 4, envConstantPixels(8, 4, sky))
		newScene := func() *Scene {
			return envPlaneSceneExtra(m, albedo, 0,
				NewSphere(Vector{1.5, 1.5, 0}, 0.8, MetallicMaterial(White, 0.3, 0)))
		}
		modes := []SpecularMode{SpecularModeNaive, SpecularModeFirst, SpecularModeAll}
		means := make([]Color, len(modes))
		for i, mode := range modes {
			sampler := NewSampler(4, 2)
			sampler.SpecularMode = mode
			sampler.EnvironmentMIS = true
			means[i] = envMeanColor(newScene(), sampler, ray, 50000, int64(41000+i*1000000))
			if !envFiniteColor(means[i]) {
				t.Fatalf("mode %d: non-finite result %+v", mode, means[i])
			}
		}
		for i := 1; i < len(modes); i++ {
			if err := envRelErr(means[i], means[0]); err > 0.10 {
				t.Errorf("mode %d vs naive: %+v vs %+v (rel err %.4f)", modes[i], means[i], means[0], err)
			}
		}
	})

	t.Run("ConcurrentScenesSharedMap", func(t *testing.T) {
		pixels, lit := envSingleCellPixels()
		m := envMustBuild(t, 64, 32, pixels)
		sceneA := envPlaneScene(m, albedo, 0)
		sceneB := envPlaneScene(m, albedo, 0.9)
		sampler := NewSampler(4, 1)
		sampler.EnvironmentMIS = true
		want := albedo.Mul(lit).MulScalar(envCellFraction(64, 32, 8))
		const workers = 8
		results := make([][2]Color, workers)
		var wg sync.WaitGroup
		for g := 0; g < workers; g++ {
			wg.Add(1)
			go func(g int) {
				defer wg.Done()
				base := int64(61000 + g*2000000)
				results[g][0] = envMeanColor(sceneA, sampler, ray, 20000, base)
				results[g][1] = envMeanColor(sceneB, sampler, ray, 20000, base+1000000)
			}(g)
		}
		wg.Wait()
		for g := 0; g < workers; g++ {
			envCheckClose(t, "concurrent scene angle=0", results[g][0], want, 0.03)
			envCheckClose(t, "concurrent scene angle=0.9", results[g][1], want, 0.03)
		}
	})

	t.Run("CameraSkyUnweighted", func(t *testing.T) {
		pixels, lit := envSingleCellPixels()
		m := envMustBuild(t, 64, 32, pixels)
		scene := &Scene{Environment: m}
		scene.Compile()
		sampler := NewSampler(1, 1)
		sampler.EnvironmentMIS = true
		d := envDirection(16.5/64, 8.5/32, 0)
		if got := sampler.Sample(scene, Ray{Vector{}, d}, rand.New(rand.NewSource(1))); got != lit {
			t.Errorf("camera sky at lit cell = %+v, want %+v", got, lit)
		}
		dark := envDirection(0.9, 0.9, 0)
		if got := sampler.Sample(scene, Ray{Vector{}, dark}, rand.New(rand.NewSource(2))); got != Black {
			t.Errorf("camera sky at dark cell = %+v, want black", got)
		}
	})

	t.Run("CompatibilityNoEnvironment", func(t *testing.T) {
		// scenes without an Environment map must be bit-identical with the
		// switch on or off: same RGB and same random consumption
		newScene := func() *Scene {
			scene := &Scene{Color: Color{0.25, 0.5, 0.75}}
			scene.Add(NewPlane(Vector{}, Vector{0, 1, 0}, DiffuseMaterial(Color{0.4, 0.6, 0.8})))
			scene.Add(NewSphere(Vector{0, 3, 0}, 0.5, LightMaterial(White, 4)))
			scene.Compile()
			return scene
		}
		on := NewSampler(2, 3)
		on.EnvironmentMIS = true
		off := NewSampler(2, 3)
		gotOn := envMeanColor(newScene(), on, ray, 500, 71000)
		gotOff := envMeanColor(newScene(), off, ray, 500, 71000)
		if gotOn != gotOff {
			t.Errorf("no-environment results differ: on=%+v off=%+v", gotOn, gotOff)
		}
	})

	t.Run("CompatibilityDirectLightingOff", func(t *testing.T) {
		// DirectLighting=false disables the new environment sampling; the sky
		// is still reachable by ordinary bounces
		pixels, _ := envSingleCellPixels()
		m := envMustBuild(t, 64, 32, pixels)
		newScene := func() *Scene { return envPlaneScene(m, albedo, 0) }
		on := NewSampler(2, 2)
		on.DirectLighting = false
		on.EnvironmentMIS = true
		off := NewSampler(2, 2)
		off.DirectLighting = false
		gotOn := envMeanColor(newScene(), on, ray, 5000, 81000)
		gotOff := envMeanColor(newScene(), off, ray, 5000, 81000)
		if gotOn != gotOff {
			t.Errorf("DirectLighting=false results differ: on=%+v off=%+v", gotOn, gotOff)
		}
		if gotOn.R <= 0 {
			t.Errorf("DirectLighting=false: sky not reachable by bounces: %+v", gotOn)
		}
	})
}
