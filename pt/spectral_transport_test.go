package pt

import (
	"math"
	"math/rand"
	"sort"
	"testing"
)

// This test suite exercises the spectral sampler added in spectral.go. It is
// written against the same package so it can inspect unexported optics
// helpers, and every numeric reference (Snell, Fresnel, piecewise linear
// integrals, the 2D circle tracer) is implemented independently here.
// Standard library only.

const spectralTestEps = 1e-9

func approx(a, b, tol float64) bool {
	return math.Abs(a-b) <= tol*(1+math.Max(math.Abs(a), math.Abs(b)))
}

func mustSpectrum(t *testing.T, wavelengths, values []float64) Spectrum {
	t.Helper()
	s, err := NewSpectrum(wavelengths, values)
	if err != nil {
		t.Fatalf("unexpected spectrum error: %v", err)
	}
	return s
}

func constSpectrum(value float64) Spectrum {
	s, _ := NewSpectrum([]float64{400, 700}, []float64{value, value})
	return s
}

// triangleSpectrum builds a symmetric triangle peaking at peak with the given
// support half width. Its analytic area is base*height/2 = width.
func triangleSpectrum(lo, peak, hi float64) Spectrum {
	s, _ := NewSpectrum([]float64{lo, peak, hi}, []float64{0, 1, 0})
	return s
}

// polyMul multiplies two polynomials given coefficient-first.
func polyMul(a, b []float64) []float64 {
	out := make([]float64, len(a)+len(b)-1)
	for i := range a {
		for j := range b {
			out[i+j] += a[i] * b[j]
		}
	}
	return out
}

// integralProduct integrates the product of piecewise linear spectra exactly
// by merging every breakpoint and integrating the resulting polynomial on
// each common segment. No grid approximation is involved.
func integralProduct(spectra ...Spectrum) float64 {
	set := map[float64]struct{}{}
	for _, s := range spectra {
		for _, w := range s.Wavelengths() {
			set[w] = struct{}{}
		}
	}
	xs := make([]float64, 0, len(set))
	for x := range set {
		xs = append(xs, x)
	}
	sort.Float64s(xs)
	total := 0.0
	for k := 1; k < len(xs); k++ {
		x0, x1 := xs[k-1], xs[k]
		length := x1 - x0
		poly := []float64{1}
		for _, s := range spectra {
			v0, v1 := s.At(x0), s.At(x1)
			linear := []float64{v0, (v1 - v0) / length}
			poly = polyMul(poly, linear)
		}
		for i, c := range poly {
			total += c * math.Pow(length, float64(i+1)) / float64(i+1)
		}
	}
	return total
}

// integralRange integrates one spectrum over [lo, hi] with exact trapezoids.
func integralRange(s Spectrum, lo, hi float64) float64 {
	ws := s.Wavelengths()
	points := append([]float64{lo}, ws...)
	points = append(points, hi)
	sort.Float64s(points)
	total := 0.0
	for i := 1; i < len(points); i++ {
		x0, x1 := points[i-1], points[i]
		if x0 < lo || x1 > hi || x1 <= x0 {
			continue
		}
		total += 0.5 * (s.At(x0) + s.At(x1)) * (x1 - x0)
	}
	return total
}

// seedWhere returns a seed whose first n Float64 draws all satisfy pred.
func seedWhere(n int, pred func(float64) bool) int64 {
	for seed := int64(1); ; seed++ {
		rnd := rand.New(rand.NewSource(seed))
		ok := true
		for i := 0; i < n; i++ {
			if !pred(rnd.Float64()) {
				ok = false
				break
			}
		}
		if ok {
			return seed
		}
	}
}

func emptyCompiledScene() *Scene {
	scene := &Scene{}
	scene.Compile()
	return scene
}

// ---------------------------------------------------------------------------
// TestSpectralSensor: wavelength measure, separate from optical transport.
// ---------------------------------------------------------------------------

func TestSpectralSensor(t *testing.T) {
	t.Run("triangular_analytic_areas", func(t *testing.T) {
		tri := triangleSpectrum(400, 500, 600)
		// Same triangle expressed on a different node partition.
		sub := mustSpectrum(t,
			[]float64{400, 450, 500, 550, 600},
			[]float64{0, 0.5, 1, 0.5, 0})
		const want = 100.0 // base 200 nm * height 1 / 2
		targetCheck(t, approx(tri.Integral(), want, 1e-12), "triangle area")
		targetCheck(t, approx(sub.Integral(), want, 1e-12), "subdivided triangle area")
		targetCheck(t, approx(tri.Integral(), sub.Integral(), 1e-12), "node partition must not change the integral")
		// A constant spectrum integrates over its physical support length.
		c := constSpectrum(0.25)
		targetCheck(t, approx(c.Integral(), 0.25*300, 1e-12), "constant spectrum area")
		// Zero outside the node range.
		targetCheck(t, tri.At(399) == 0 && tri.At(601) == 0 && tri.At(700) == 0, "spectrum vanishes outside nodes")
		t.Logf("triangle integrals: coarse=%.9g fine=%.9g constant=%.9g", tri.Integral(), sub.Integral(), c.Integral())
	})

	t.Run("cdf_inversion_and_pdf", func(t *testing.T) {
		red := triangleSpectrum(400, 500, 600)
		green := mustSpectrum(t, []float64{450, 550, 650}, []float64{0, 1, 0})
		blue := mustSpectrum(t, []float64{500, 600, 700}, []float64{0, 1, 0})
		sensor, err := NewSensor(red, green, blue)
		if err != nil {
			t.Fatal(err)
		}
		sampler, err := NewSpectralSampler(emptyCompiledScene(), map[Shape]SpectralMaterial{}, constSpectrum(1), sensor, 0)
		if err != nil {
			t.Fatal(err)
		}
		// CDF endpoints and monotonicity.
		targetCheck(t, approx(sampler.cdfValue[0], 0, 1e-15), "CDF starts at zero")
		targetCheck(t, approx(sampler.cdfValue[len(sampler.cdfValue)-1], 1, 1e-15), "CDF ends at one")
		mono := true
		for i := 1; i < len(sampler.cdfValue); i++ {
			if sampler.cdfValue[i] < sampler.cdfValue[i-1]-1e-15 {
				mono = false
			}
		}
		targetCheck(t, mono, "CDF is non-decreasing")
		// Independent exact CDF evaluation at the sampled wavelengths must
		// reproduce the uniform draws that produced them.
		norm := sensor.responseIntegral()
		us := []float64{0.01, 0.1, 0.25, 0.5, 0.75, 0.9, 0.99}
		maxErr := 0.0
		for _, u := range us {
			lambda := sampler.SampleWavelength(u)
			mass := 0.0
			for i := 1; i < len(sampler.cdfNodes); i++ {
				w0, w1 := sampler.cdfNodes[i-1], sampler.cdfNodes[i]
				hi := math.Min(lambda, w1)
				if hi > w0 {
					mass += 0.5 * (sampler.summedResponse(w0) + sampler.summedResponse(hi)) * (hi - w0) / norm
				}
			}
			err := math.Abs(mass - u)
			maxErr = math.Max(maxErr, err)
		}
		targetCheck(t, maxErr < 1e-10, "inverse CDF reproduces uniform draws")
		// PDF values and normalization.
		targetCheck(t, approx(sampler.WavelengthPDF(500), sampler.summedResponse(500)/norm, 1e-15), "PDF shape")
		targetCheck(t, sampler.WavelengthPDF(399) == 0 && sampler.WavelengthPDF(701) == 0, "PDF vanishes outside [400,700]")
		pdfMass := 0.0
		for i := 1; i < len(sampler.cdfNodes); i++ {
			w0, w1 := sampler.cdfNodes[i-1], sampler.cdfNodes[i]
			pdfMass += 0.5 * (sampler.WavelengthPDF(w0) + sampler.WavelengthPDF(w1)) * (w1 - w0)
		}
		targetCheck(t, approx(pdfMass, 1, 1e-12), "PDF integrates to one")
		t.Logf("cdf nodes=%d max inverse error=%.3e pdf mass=%.9g", len(sampler.cdfNodes), maxErr, pdfMass)
	})

	t.Run("sampling_frequency", func(t *testing.T) {
		red := triangleSpectrum(400, 500, 600)
		green := mustSpectrum(t, []float64{450, 550, 650}, []float64{0, 1, 0})
		blue := mustSpectrum(t, []float64{500, 600, 700}, []float64{0, 1, 0})
		sensor, _ := NewSensor(red, green, blue)
		sampler, err := NewSpectralSampler(emptyCompiledScene(), map[Shape]SpectralMaterial{}, constSpectrum(1), sensor, 0)
		if err != nil {
			t.Fatal(err)
		}
		const n = 200000
		const bins = 12
		rnd := rand.New(rand.NewSource(20261125))
		counts := make([]int, bins)
		firstWavelengths := make([]float64, 5)
		for i := 0; i < n; i++ {
			lambda := sampler.SampleWavelength(rnd.Float64())
			if i < len(firstWavelengths) {
				firstWavelengths[i] = lambda
			}
			idx := int((lambda - 400) / 300 * bins)
			if idx >= bins {
				idx = bins - 1
			}
			counts[idx]++
		}
		maxErr := 0.0
		for b := 0; b < bins; b++ {
			lo := 400 + float64(b)*300/bins
			hi := 400 + float64(b+1)*300/bins
			want := (integralRange(red, lo, hi) + integralRange(green, lo, hi) + integralRange(blue, lo, hi)) / sensor.responseIntegral()
			got := float64(counts[b]) / n
			maxErr = math.Max(maxErr, math.Abs(got-want))
			targetCheck(t, math.Abs(got-want) < 0.02, "histogram bin mass")
		}
		t.Logf("samples=%d first wavelengths=%v max bin error=%.3e", n, firstWavelengths, maxErr)
	})

	t.Run("spectral_integral_estimate", func(t *testing.T) {
		// Importance-sampled Monte Carlo of int f d lambda must match the
		// exact piecewise linear integral. f shares the response endpoints so
		// the ratio f/p stays bounded (both vanish together); its interior
		// node partition is deliberately different.
		response := triangleSpectrum(400, 550, 700)
		sensor, _ := NewSensor(response, constSpectrum(0), constSpectrum(0))
		sampler, err := NewSpectralSampler(emptyCompiledScene(), map[Shape]SpectralMaterial{}, constSpectrum(1), sensor, 0)
		if err != nil {
			t.Fatal(err)
		}
		f := triangleSpectrum(400, 600, 700)
		want := integralProduct(f, response)
		const n = 100000
		rnd := rand.New(rand.NewSource(424242))
		sum := 0.0
		for i := 0; i < n; i++ {
			lambda := sampler.SampleWavelength(rnd.Float64())
			p := sampler.WavelengthPDF(lambda)
			if p <= 0 {
				continue // both g and p vanish at the response endpoints
			}
			// The transported estimator is g/p with g = f*response, matching
			// the L*S/p weighting used by the sampler itself.
			sum += f.At(lambda) * response.At(lambda) / p
		}
		got := sum / n
		targetCheck(t, approx(got, want, 0.01), "importance-sampled spectral integral")
		t.Logf("integral samples=%d exact=%.9g estimated=%.9g error=%.3e", n, want, got, math.Abs(got-want))
	})

	t.Run("validation_and_copy", func(t *testing.T) {
		if _, err := NewSpectrum([]float64{500}, []float64{1}); err == nil {
			targetCheck(t, false, "single node must be rejected")
		}
		targetCheck(t, true, "reached validation cases")
		bad := []struct {
			w, v []float64
		}{
			{[]float64{500, 500}, []float64{0, 1}},    // not strictly increasing
			{[]float64{399, 500}, []float64{0, 1}},    // below range
			{[]float64{500, 701}, []float64{0, 1}},    // above range
			{[]float64{400, 500}, []float64{0, -0.1}}, // negative value
			{[]float64{400, 500}, []float64{0, math.NaN()}},
			{[]float64{400, math.Inf(1)}, []float64{0, 1}},
		}
		for _, b := range bad {
			if _, err := NewSpectrum(b.w, b.v); err == nil {
				t.Fatalf("expected error for %v", b)
			}
		}
		// Zero total sensor response is invalid.
		zero := constSpectrum(0)
		if _, err := NewSensor(zero, zero, zero); err == nil {
			t.Fatal("zero-response sensor must be rejected")
		}
		// Albedo / reflectance bounded by one.
		scene := emptyCompiledScene()
		goodSensor, _ := NewSensor(triangleSpectrum(400, 500, 600), zero, zero)
		if _, err := NewSpectralSampler(scene,
			map[Shape]SpectralMaterial{}, constSpectrum(1), goodSensor, 0); err != nil {
			t.Fatal(err)
		}
		over := constSpectrum(1.01)
		sphere := NewSphere(V(0, 0, -3), 1, DiffuseMaterial(White))
		badScene := &Scene{}
		badScene.Add(sphere)
		badScene.Compile()
		if _, err := NewSpectralSampler(badScene,
			map[Shape]SpectralMaterial{sphere: {Scatter: SpectralLambert, Albedo: over}},
			constSpectrum(1), goodSensor, 1); err == nil {
			t.Fatal("albedo above one must be rejected")
		}
		// Copy isolation: mutating constructor inputs must not change the sampler.
		ws := []float64{400, 500, 600}
		vs := []float64{0, 1, 0}
		spec, _ := NewSpectrum(ws, vs)
		ws[1] = 450
		vs[1] = 0.25
		targetCheck(t, approx(spec.At(500), 1, 1e-15), "spectrum copies its inputs")
	})
}

// ---------------------------------------------------------------------------
// TestSpectralMedia: Snell/Fresnel references and dielectric stack handling.
// ---------------------------------------------------------------------------

// referenceFresnel is an independent unpolarized Fresnel implementation.
func referenceFresnel(cosI, etaI, etaT float64) float64 {
	sinI2 := 1 - cosI*cosI
	sinT2 := etaI * etaI / (etaT * etaT) * sinI2
	if sinT2 > 1 {
		return 1
	}
	cosT := math.Sqrt(1 - sinT2)
	rs := (etaI*cosI - etaT*cosT) / (etaI*cosI + etaT*cosT)
	rp := (etaT*cosI - etaI*cosT) / (etaT*cosI + etaI*cosT)
	return 0.5 * (rs*rs + rp*rp)
}

// referenceRefract applies Snell's law independently in vector form.
func referenceRefract(direction, normal Vector, etaI, etaT float64) (Vector, bool) {
	cosI := -direction.Dot(normal)
	eta := etaI / etaT
	sinT2 := eta * eta * (1 - cosI*cosI)
	if sinT2 > 1 {
		return Vector{}, true
	}
	cosT := math.Sqrt(1 - sinT2)
	return direction.MulScalar(eta).Add(normal.MulScalar(eta*cosI - cosT)), false
}

func cauchyIndex(a, b, wavelength float64) float64 {
	u := wavelength / 1000
	return a + b/(u*u)
}

func glassTestScene(t *testing.T) (*Scene, *Sphere, *Sphere) {
	outer := NewSphere(V(0, 0, 0), 2, ClearMaterial(1.4, 0))
	inner := NewSphere(V(0.3, 0, 0), 0.8, ClearMaterial(1.6, 0))
	scene := &Scene{}
	scene.Add(outer)
	scene.Add(inner)
	scene.Compile()
	return scene, outer.(*Sphere), inner.(*Sphere)
}

func glassTestSampler(t *testing.T, scene *Scene) *SpectralSampler {
	zero := constSpectrum(0)
	sensor, _ := NewSensor(triangleSpectrum(400, 500, 600), zero, zero)
	materials := map[Shape]SpectralMaterial{}
	for _, shape := range scene.Shapes {
		switch sp := shape.(*Sphere); sp.Radius {
		case 2:
			materials[shape] = SpectralMaterial{Scatter: SpectralDielectric, IndexA: 1.4, IndexB: 0.1}
		default:
			materials[shape] = SpectralMaterial{Scatter: SpectralDielectric, IndexA: 1.6, IndexB: 0.2}
		}
	}
	sampler, err := NewSpectralSampler(scene, materials, constSpectrum(1), sensor, 8)
	if err != nil {
		t.Fatal(err)
	}
	return sampler
}

func TestSpectralMedia(t *testing.T) {
	t.Run("snell_and_fresnel_reference", func(t *testing.T) {
		normal := V(0, 0, 1)
		maxFErr, maxDErr := 0.0, 0.0
		for _, degrees := range []float64{0, 15, 30, 45, 60, 75} {
			theta := degrees * math.Pi / 180
			// Incident direction converging onto the surface from above.
			d := V(math.Sin(theta), 0, -math.Cos(theta))
			etaI, etaT := 1.0, 1.5
			got := dielectricFresnel(d, normal, etaI, etaT)
			want := referenceFresnel(math.Cos(theta), etaI, etaT)
			maxFErr = math.Max(maxFErr, math.Abs(got-want))
			refDir, tir := refractDirection(d, normal, etaI, etaT)
			wantDir, wantTir := referenceRefract(d, normal, etaI, etaT)
			targetCheck(t, tir == wantTir, "TIR flag agreement")
			if !tir {
				maxDErr = math.Max(maxDErr, math.Abs(refDir.Sub(wantDir).Length()))
				sinT := etaI / etaT * math.Sin(theta)
				targetCheck(t, approx(math.Abs(refDir.Dot(normal)), math.Sqrt(1-sinT*sinT), 1e-12), "Snell angle")
			}
		}
		// Normal incidence Fresnel equals ((n1-n2)/(n1+n2))^2.
		normalF := dielectricFresnel(V(0, 0, -1), normal, 1, 1.5)
		targetCheck(t, approx(normalF, 0.04, 1e-12), "normal incidence Fresnel")
		targetCheck(t, maxFErr < 1e-12 && maxDErr < 1e-12, "reference formula agreement")
		t.Logf("max Fresnel error=%.3e max direction error=%.3e", maxFErr, maxDErr)
	})

	t.Run("nested_sphere_stack", func(t *testing.T) {
		scene, outer, inner := glassTestScene(t)
		sampler := glassTestSampler(t, scene)
		const lambda = 550.0
		nOuter := cauchyIndex(1.4, 0.1, lambda)
		nInner := cauchyIndex(1.6, 0.2, lambda)
		targetCheck(t, cauchyIndex(1.5, 0.2, 450) > cauchyIndex(1.5, 0.2, 650), "blue bends more than red (dispersion)")
		// Ray starts at the inner sphere center and travels outward.
		ray := Ray{V(0.3, 0, 0), V(1, 0, 0)}
		stack := sampler.initialMediumStack(ray.Origin)
		targetCheck(t, len(stack) == 2 && stack[0] == outer && stack[1] == inner, "initial nested stack")
		// The outward ray needs two draws; the outside ray below needs four.
		// Reusing the seed for a fresh rnd, require the first four to clear
		// the small normal-incidence Fresnel thresholds.
		seed := seedWhere(4, func(f float64) bool { return f > 0.9 })
		_, events := sampler.TraceWavelengthPath(lambda, ray, rand.New(rand.NewSource(seed)))
		targetCheck(t, len(events) >= 2, "two interface crossings")
		e0, e1 := events[0], events[1]
		targetCheck(t, e0.Shape == inner && !e0.Entering, "first event leaves inner sphere")
		targetCheck(t, approx(e0.EtaI, nInner, 1e-12), "inner glass index on exit")
		targetCheck(t, approx(e0.EtaT, nOuter, 1e-12), "exiting inner sphere enters outer glass, not air")
		targetCheck(t, e0.Transmission && e0.StackDepth == 1, "stack pops to outer")
		targetCheck(t, e1.Shape == outer && !e1.Entering && approx(e1.EtaT, 1, 1e-12), "second event leaves outer sphere to air")
		targetCheck(t, e1.StackDepth == 0, "stack empty after outer exit")
		// A ray from outside must push both spheres in order.
		outRay := Ray{V(-5, 0, 0), V(1, 0, 0)}
		_, outEvents := sampler.TraceWavelengthPath(lambda, outRay, rand.New(rand.NewSource(seed)))
		targetCheck(t, outEvents[0].Entering && approx(outEvents[0].EtaI, 1, 1e-12) && outEvents[0].StackDepth == 1, "enter outer from air")
		targetCheck(t, outEvents[1].Entering && approx(outEvents[1].EtaT, nInner, 1e-12) && outEvents[1].StackDepth == 2, "enter inner from outer glass")
		t.Logf("exit etas: inner->outer %.6g->%.6g, outer->air %.6g->1", e0.EtaI, e0.EtaT, e1.EtaI)
	})

	t.Run("reflection_keeps_stack", func(t *testing.T) {
		// Off-center start inside a single glass sphere. At normal-ish exit
		// the air interface Fresnel is ((n-1)/(n+1))^2 = 0.04 for n = 1.5, so
		// one draw below 0.03 forces reflection without a costly seed search.
		sphere := NewSphere(V(0, 0, 0), 1, ClearMaterial(1.5, 0))
		scene := &Scene{}
		scene.Add(sphere)
		scene.Compile()
		zero := constSpectrum(0)
		sensor, _ := NewSensor(triangleSpectrum(400, 500, 600), zero, zero)
		sampler, err := NewSpectralSampler(scene,
			map[Shape]SpectralMaterial{sphere: {Scatter: SpectralDielectric, IndexA: 1.5, IndexB: 0}},
			constSpectrum(1), sensor, 8)
		if err != nil {
			t.Fatal(err)
		}
		seed := seedWhere(1, func(f float64) bool { return f < 0.03 })
		ray := Ray{V(0.2, 0, 0), V(1, 0, 0)}
		_, events := sampler.TraceWavelengthPath(550, ray, rand.New(rand.NewSource(seed)))
		targetCheck(t, len(events) >= 2, "reflection stays inside and hits again")
		e0 := events[0]
		targetCheck(t, e0.Reflection && !e0.Transmission, "first event is reflection")
		targetCheck(t, e0.Shape == sphere && e0.StackDepth == 1, "reflection does not pop the medium stack")
		targetCheck(t, events[1].Shape == sphere && !events[1].Entering, "reflected ray remains inside the same sphere")
		// Geometric reflection check against an independent formula.
		wantDir := ray.Direction.Sub(e0.Normal.MulScalar(2 * ray.Direction.Dot(e0.Normal)))
		targetCheck(t, approx(e0.Direction.Sub(wantDir).Length(), 0, 1e-12), "mirror reflection direction")
		t.Logf("seed=%d reflected direction=%v stack depth=%d", seed, e0.Direction, e0.StackDepth)
	})

	t.Run("total_internal_reflection", func(t *testing.T) {
		// Grazing ray inside a glass sphere: sin(theta_i) = 0.9 and n=1.5,
		// well beyond the critical angle asin(1/1.5).
		sphere := NewSphere(V(0, 0, 0), 1, ClearMaterial(1.5, 0))
		scene := &Scene{}
		scene.Add(sphere)
		scene.Compile()
		zero := constSpectrum(0)
		sensor, _ := NewSensor(triangleSpectrum(400, 500, 600), zero, zero)
		materials := map[Shape]SpectralMaterial{
			sphere: {Scatter: SpectralDielectric, IndexA: 1.5, IndexB: 0},
		}
		sampler, err := NewSpectralSampler(scene, materials, constSpectrum(1), sensor, 8)
		if err != nil {
			t.Fatal(err)
		}
		cosI := math.Sqrt(1 - 0.9*0.9)
		targetCheck(t, approx(referenceFresnel(cosI, 1.5, 1), 1, 1e-12), "reference predicts TIR")
		ray := Ray{V(0, -0.9, 0), V(1, 0, 0)}
		_, events := sampler.TraceWavelengthPath(550, ray, rand.New(rand.NewSource(7)))
		targetCheck(t, len(events) >= 3, "TIR keeps the ray bouncing inside")
		for i, e := range events {
			targetCheck(t, e.Fresnel == 1 && e.Reflection && e.StackDepth == 1, "TIR event reflects with stack intact")
			if i >= 3 {
				break
			}
		}
		t.Logf("TIR events=%d directions[0]=%v stack=%d", len(events), events[0].Direction, events[0].StackDepth)
	})

	t.Run("geometry_and_coefficient_rejection", func(t *testing.T) {
		zero := constSpectrum(0)
		sensor, _ := NewSensor(triangleSpectrum(400, 500, 600), zero, zero)
		build := func(shapes ...Shape) error {
			scene := &Scene{}
			materials := map[Shape]SpectralMaterial{}
			for _, sh := range shapes {
				scene.Add(sh)
				materials[sh] = SpectralMaterial{Scatter: SpectralDielectric, IndexA: 1.5, IndexB: 0}
			}
			scene.Compile()
			_, err := NewSpectralSampler(scene, materials, constSpectrum(1), sensor, 4)
			return err
		}
		targetCheck(t, build(NewSphere(V(0, 0, 0), 1, DiffuseMaterial(White)), NewSphere(V(3, 0, 0), 1, DiffuseMaterial(White))) == nil, "strictly separate spheres accepted")
		if err := build(NewSphere(V(0, 0, 0), 1, DiffuseMaterial(White)), NewSphere(V(1, 0, 0), 1, DiffuseMaterial(White))); err == nil {
			t.Fatal("intersecting spheres must be rejected")
		}
		if err := build(NewSphere(V(0, 0, 0), 1, DiffuseMaterial(White)), NewSphere(V(2, 0, 0), 1, DiffuseMaterial(White))); err == nil {
			t.Fatal("tangent spheres must be rejected")
		}
		// Dielectric on a non-sphere, and invalid Cauchy coefficients.
		plane := NewPlane(V(0, -1, 0), V(0, 1, 0), DiffuseMaterial(White))
		scene := &Scene{}
		scene.Add(plane)
		scene.Compile()
		if _, err := NewSpectralSampler(scene,
			map[Shape]SpectralMaterial{plane: {Scatter: SpectralDielectric, IndexA: 1.5}},
			constSpectrum(1), sensor, 4); err == nil {
			t.Fatal("dielectric plane must be rejected")
		}
		sphere := NewSphere(V(0, 0, 0), 1, DiffuseMaterial(White))
		scene2 := &Scene{}
		scene2.Add(sphere)
		scene2.Compile()
		for _, bad := range []SpectralMaterial{
			{Scatter: SpectralDielectric, IndexA: 0.9, IndexB: 0},
			{Scatter: SpectralDielectric, IndexA: 1.5, IndexB: -0.1},
			{Scatter: SpectralDielectric, IndexA: math.NaN(), IndexB: 0},
		} {
			if _, err := NewSpectralSampler(scene2, map[Shape]SpectralMaterial{sphere: bad}, constSpectrum(1), sensor, 4); err == nil {
				t.Fatal("bad dielectric coefficients must be rejected")
			}
		}
		// Unsupported geometry (transformed/parametric shapes) is rejected.
		cube := NewCube(V(-1, -1, -1), V(1, 1, 1), DiffuseMaterial(White))
		scene3 := &Scene{}
		scene3.Add(cube)
		scene3.Compile()
		if _, err := NewSpectralSampler(scene3,
			map[Shape]SpectralMaterial{cube: {Scatter: SpectralLambert, Albedo: constSpectrum(0.5)}},
			constSpectrum(1), sensor, 2); err == nil {
			t.Fatal("non-primitive shapes must be rejected")
		}
	})
}

// ---------------------------------------------------------------------------
// TestSpectralTransport: full Scene tree intersection plus the sampler.
// ---------------------------------------------------------------------------

func lambertScene(albedo Spectrum) (*Scene, Shape) {
	sphere := NewSphere(V(0, 0, -3), 1, DiffuseMaterial(White))
	scene := &Scene{}
	scene.Add(sphere)
	scene.Compile()
	return scene, sphere
}

func TestSpectralTransport(t *testing.T) {
	zero := constSpectrum(0)
	hitRay := Ray{V(0, 0, 0), V(0, 0, -1)}

	t.Run("environment_only_and_max_bounces", func(t *testing.T) {
		scene := emptyCompiledScene()
		sensor, _ := NewSensor(triangleSpectrum(400, 500, 600), zero, zero)
		sampler, err := NewSpectralSampler(scene, map[Shape]SpectralMaterial{}, constSpectrum(0.7), sensor, 0)
		if err != nil {
			t.Fatal(err)
		}
		targetCheck(t, approx(sampler.TraceWavelength(530, hitRay, rand.New(rand.NewSource(1))), 0.7, 1e-12), "empty scene returns environment")
		// MaxBounces = 0: a surface hit contributes nothing.
		scene2, sphere := lambertScene(constSpectrum(0.5))
		sampler2, err := NewSpectralSampler(scene2,
			map[Shape]SpectralMaterial{sphere: {Scatter: SpectralLambert, Albedo: constSpectrum(0.5)}},
			constSpectrum(0.7), sensor, 0)
		if err != nil {
			t.Fatal(err)
		}
		targetCheck(t, sampler2.TraceWavelength(530, hitRay, rand.New(rand.NewSource(1))) == 0, "zero bounces suppresses surface events")
		missRay := Ray{V(0, 0, 0), V(1, 0, 0)}
		targetCheck(t, approx(sampler2.TraceWavelength(530, missRay, rand.New(rand.NewSource(1))), 0.7, 1e-12), "escape still allowed at zero bounces")
	})

	t.Run("lambert_constant_exact", func(t *testing.T) {
		albedo := constSpectrum(0.5)
		scene, sphere := lambertScene(albedo)
		sensor, _ := NewSensor(triangleSpectrum(400, 500, 600), zero, zero)
		sampler, err := NewSpectralSampler(scene,
			map[Shape]SpectralMaterial{sphere: {Scatter: SpectralLambert, Albedo: albedo}},
			constSpectrum(0.8), sensor, 4)
		if err != nil {
			t.Fatal(err)
		}
		// Convex sphere: after the Lambert event the ray escapes, so the
		// scalar radiance is exactly albedo*environment at this wavelength.
		for _, lambda := range []float64{420, 500, 580} {
			got := sampler.TraceWavelength(lambda, hitRay, rand.New(rand.NewSource(9)))
			targetCheck(t, approx(got, 0.4, 1e-12), "one-bounce Lambert radiance")
		}
		// Single-channel sensor with constant radiance: every sample carries
		// exactly L * responseArea, so the Monte Carlo estimate has zero noise.
		const n = 20000
		rnd := rand.New(rand.NewSource(11))
		var sumR float64
		for i := 0; i < n; i++ {
			c := sampler.Sample(scene, hitRay, rnd)
			sumR += c.R
		}
		got := sumR / n
		want := integralProduct(constSpectrum(0.4), sensor.Red())
		targetCheck(t, approx(got, want, 1e-9), "constant-radiance sensor integral is exact sample by sample")
		t.Logf("samples=%d exact=%.9g got=%.9g error=%.3e", n, want, got, math.Abs(got-want))
	})

	t.Run("sensor_integral_spectra", func(t *testing.T) {
		albedo := mustSpectrum(t, []float64{400, 700}, []float64{0.2, 0.8})
		env := constSpectrum(1.2)
		scene, sphere := lambertScene(albedo)
		red := triangleSpectrum(400, 500, 600)
		green := mustSpectrum(t, []float64{450, 550, 650}, []float64{0, 1, 0})
		blue := mustSpectrum(t, []float64{500, 600, 700}, []float64{0, 1, 0})
		sensor, _ := NewSensor(red, green, blue)
		sampler, err := NewSpectralSampler(scene,
			map[Shape]SpectralMaterial{sphere: {Scatter: SpectralLambert, Albedo: albedo}},
			env, sensor, 4)
		if err != nil {
			t.Fatal(err)
		}
		wants := [3]float64{
			integralProduct(env, albedo, red),
			integralProduct(env, albedo, green),
			integralProduct(env, albedo, blue),
		}
		const n = 60000
		rnd := rand.New(rand.NewSource(2026))
		var sums [3]float64
		for i := 0; i < n; i++ {
			c := sampler.Sample(scene, hitRay, rnd)
			sums[0] += c.R
			sums[1] += c.G
			sums[2] += c.B
		}
		for ch, name := range []string{"R", "G", "B"} {
			got := sums[ch] / n
			targetCheck(t, approx(got, wants[ch], 0.03), "channel "+name+" spectral product integral")
			t.Logf("channel %s samples=%d exact=%.9g estimated=%.9g relative error=%.3e", name, n, wants[ch], got, math.Abs(got-wants[ch])/wants[ch])
		}
		// Fixed seed must reproduce bit-identical results.
		rnd1, rnd2 := rand.New(rand.NewSource(2026)), rand.New(rand.NewSource(2026))
		c1 := sampler.Sample(scene, hitRay, rnd1)
		c2 := sampler.Sample(scene, hitRay, rnd2)
		targetCheck(t, c1 == c2, "fixed seed rerun is identical")
	})

	t.Run("direct_emission", func(t *testing.T) {
		emission := mustSpectrum(t, []float64{400, 700}, []float64{1.5, 2.5})
		scene, sphere := lambertScene(constSpectrum(0))
		sensor, _ := NewSensor(triangleSpectrum(400, 500, 600), zero, zero)
		sampler, err := NewSpectralSampler(scene,
			map[Shape]SpectralMaterial{sphere: {
				Scatter:  SpectralLambert,
				Albedo:   constSpectrum(0),
				Emission: emission,
			}}, constSpectrum(0), sensor, 2)
		if err != nil {
			t.Fatal(err)
		}
		lambda := 550.0
		got := sampler.TraceWavelength(lambda, hitRay, rand.New(rand.NewSource(3)))
		want := emission.At(lambda)
		targetCheck(t, approx(got, want, 1e-12), "directly hit emitter radiance")
		_, events := sampler.TraceWavelengthPath(lambda, hitRay, rand.New(rand.NewSource(3)))
		targetCheck(t, approx(events[0].Emission, want, 1e-12), "emission recorded on the event")
	})

	t.Run("mirror_reflection", func(t *testing.T) {
		reflectance := constSpectrum(0.5)
		sphere := NewSphere(V(0, 0, -3), 1, SpecularMaterial(White, 1))
		scene := &Scene{}
		scene.Add(sphere)
		scene.Compile()
		sensor, _ := NewSensor(triangleSpectrum(400, 500, 600), zero, zero)
		sampler, err := NewSpectralSampler(scene,
			map[Shape]SpectralMaterial{sphere: {Scatter: SpectralMirror, Reflectance: reflectance}},
			constSpectrum(0.8), sensor, 2)
		if err != nil {
			t.Fatal(err)
		}
		lambda := 500.0
		got, events := sampler.TraceWavelengthPath(lambda, hitRay, rand.New(rand.NewSource(4)))
		targetCheck(t, approx(got, 0.4, 1e-12), "mirror multiplies reflected environment")
		independent := hitRay.Direction.Sub(events[0].Normal.MulScalar(2 * hitRay.Direction.Dot(events[0].Normal)))
		targetCheck(t, approx(events[0].Direction.Sub(independent).Length(), 0, 1e-12), "mirror direction matches reference")
		t.Logf("mirror outgoing direction=%v radiance=%.9g", events[0].Direction, got)
	})

	t.Run("transmission_radiance_factor", func(t *testing.T) {
		// Forcing transmission through a full sphere: in and out factors
		// (1/n)^2 * n^2 cancel, so a constant environment is unchanged. This
		// also proves Fresnel is not multiplied a second time.
		sphere := NewSphere(V(0, 0, -3), 1, ClearMaterial(1.5, 0))
		scene := &Scene{}
		scene.Add(sphere)
		scene.Compile()
		sensor, _ := NewSensor(triangleSpectrum(400, 500, 600), zero, zero)
		sampler, err := NewSpectralSampler(scene,
			map[Shape]SpectralMaterial{sphere: {Scatter: SpectralDielectric, IndexA: 1.5, IndexB: 0.1}},
			constSpectrum(0.9), sensor, 4)
		if err != nil {
			t.Fatal(err)
		}
		seed := seedWhere(2, func(f float64) bool { return f > 0.95 })
		got, events := sampler.TraceWavelengthPath(550, hitRay, rand.New(rand.NewSource(seed)))
		targetCheck(t, len(events) == 2 && events[0].Transmission && events[1].Transmission, "two transmissions")
		targetCheck(t, approx(got, 0.9, 1e-12), "radiance-form (eta_i/eta_t)^2 factors cancel over the sphere")
	})

	t.Run("dispersion_vs_2d_cross_section", func(t *testing.T) {
		const a, b = 1.5, 0.3
		sphere := NewSphere(V(0, 0, 0), 1, ClearMaterial(a, 0))
		scene := &Scene{}
		scene.Add(sphere)
		scene.Compile()
		zero := constSpectrum(0)
		sensor, _ := NewSensor(triangleSpectrum(400, 500, 600), zero, zero)
		sampler, err := NewSpectralSampler(scene,
			map[Shape]SpectralMaterial{sphere: {Scatter: SpectralDielectric, IndexA: a, IndexB: b}},
			constSpectrum(1), sensor, 4)
		if err != nil {
			t.Fatal(err)
		}
		ray := Ray{V(-3, 0.2, 0), V(1, 0, 0)}
		circles := []circle2d{{0, 0, 1, func(lambda float64) float64 { return cauchyIndex(a, b, lambda) }}}
		seed := seedWhere(2, func(f float64) bool { return f > 0.95 })
		screenY := map[float64]float64{}
		for _, lambda := range []float64{450, 550, 650} {
			_, events := sampler.TraceWavelengthPath(lambda, ray, rand.New(rand.NewSource(seed)))
			ref := traceCircles2d(circles, -3, 0.2, 1, 0, lambda, 4)
			targetCheck(t, len(events) == len(ref) && len(ref) == 2, "two interfaces in both tracers")
			for i, e := range events {
				// The 1e-7 ray-origin shift perturbs later hit geometry, so
				// positions and directions agree to slightly looser bounds;
				// the interface indices are exact.
				targetCheck(t, approx(e.Position.X, ref[i].x, 1e-6) && approx(e.Position.Y, ref[i].y, 1e-6), "event position matches 2D reference")
				targetCheck(t, approx(e.Direction.X, ref[i].dx, 1e-6) && approx(e.Direction.Y, ref[i].dy, 1e-6), "event direction matches 2D reference")
				targetCheck(t, approx(e.EtaI, ref[i].etaI, 1e-12) && approx(e.EtaT, ref[i].etaT, 1e-12), "interface indices match 2D reference")
			}
			// Screen intersection behind the sphere proves wavelengths land
			// at different positions: real spatial dispersion, not an image.
			last := ref[len(ref)-1]
			ts := (3 - last.x) / last.dx
			screenY[lambda] = last.y + last.dy*ts
			t.Logf("lambda=%.1f nm n=%.6g exit pos=(%.6g,%.6g) dir=(%.6g,%.6g) screen y=%.6g",
				lambda, cauchyIndex(a, b, lambda), last.x, last.y, last.dx, last.dy, screenY[lambda])
		}
		targetCheck(t, math.Abs(screenY[450]-screenY[650]) > 0.01, "blue and red reach different screen positions")
		targetCheck(t, (screenY[450]-screenY[550])*(screenY[550]-screenY[650]) > 0, "dispersion is monotonic in wavelength")
	})

	t.Run("dispersion_splits_light_onto_emitter", func(t *testing.T) {
		// A small emissive patch is placed from the independent 2D reference
		// at the blue (450 nm) landing point. Only the blue wavelength is
		// refracted onto it; green and red rays miss and escape into a black
		// environment. This is spatial color separation through real paths.
		const a, b = 1.5, 0.3
		circles := []circle2d{{0, 0, 1, func(lambda float64) float64 { return cauchyIndex(a, b, lambda) }}}
		ref450 := traceCircles2d(circles, -3, 0.2, 1, 0, 450, 4)
		last := ref450[len(ref450)-1]
		blueY := last.y + last.dy*((3-last.x)/last.dx)
		// Triangle in the x=3 plane; at z=0 its interior spans roughly
		// y in [blueY-0.05, blueY+0.008], excluding the other wavelengths.
		patch := NewTriangle(
			V(3, blueY-0.05, -0.15),
			V(3, blueY-0.05, 0.15),
			V(3, blueY+0.008, 0),
			Vector{}, Vector{}, Vector{}, DiffuseMaterial(White))
		sphere := NewSphere(V(0, 0, 0), 1, ClearMaterial(a, 0))
		scene := &Scene{}
		scene.Add(sphere)
		scene.Add(patch)
		scene.Compile()
		zero := constSpectrum(0)
		sensor, _ := NewSensor(triangleSpectrum(400, 500, 600), zero, zero)
		emission := constSpectrum(2)
		sampler, err := NewSpectralSampler(scene,
			map[Shape]SpectralMaterial{
				sphere: {Scatter: SpectralDielectric, IndexA: a, IndexB: b},
				patch:  {Scatter: SpectralLambert, Albedo: zero, Emission: emission},
			},
			zero, sensor, 4)
		if err != nil {
			t.Fatal(err)
		}
		ray := Ray{V(-3, 0.2, 0), V(1, 0, 0)}
		seed := seedWhere(2, func(f float64) bool { return f > 0.95 })
		radiance := map[float64]float64{}
		for _, lambda := range []float64{450, 550, 650} {
			got, events := sampler.TraceWavelengthPath(lambda, ray, rand.New(rand.NewSource(seed)))
			radiance[lambda] = got
			if lambda == 450 {
				hitEmitter := false
				for _, e := range events {
					if e.Shape == patch {
						hitEmitter = true
						targetCheck(t, approx(e.Emission, 2, 1e-12), "blue path records the emitter's emission")
					}
				}
				targetCheck(t, hitEmitter, "blue wavelength reaches the emissive patch")
			} else {
				for _, e := range events {
					targetCheck(t, e.Shape != patch, "longer wavelength must miss the patch")
				}
			}
		}
		targetCheck(t, approx(radiance[450], 2, 1e-9), "blue carries the emitter radiance")
		targetCheck(t, radiance[550] == 0 && radiance[650] == 0, "green and red escape into black background")
		t.Logf("patch center y=%.6g radiances: blue=%.6g green=%.6g red=%.6g", blueY, radiance[450], radiance[550], radiance[650])
	})

	t.Run("plane_and_triangle_primitives", func(t *testing.T) {
		zero := constSpectrum(0)
		sensor, _ := NewSensor(triangleSpectrum(400, 500, 600), zero, zero)
		// An infinite Lambert plane: the cosine bounce points into the normal
		// hemisphere and escapes, so radiance is albedo*environment exactly.
		plane := NewPlane(V(0, -1, 0), V(0, 1, 0), DiffuseMaterial(White))
		scene := &Scene{}
		scene.Add(plane)
		scene.Compile()
		sampler, err := NewSpectralSampler(scene,
			map[Shape]SpectralMaterial{plane: {Scatter: SpectralLambert, Albedo: constSpectrum(0.5)}},
			constSpectrum(0.8), sensor, 2)
		if err != nil {
			t.Fatal(err)
		}
		downRay := Ray{V(0, 0, 0), V(0, -1, 0)}
		got, events := sampler.TraceWavelengthPath(550, downRay, rand.New(rand.NewSource(5)))
		targetCheck(t, approx(got, 0.4, 1e-12), "plane Lambert one-bounce radiance")
		targetCheck(t, events[0].Shape == plane && events[0].Direction.Y > 0, "plane bounce stays in the normal hemisphere")
		// A triangle struck through the scene tree behaves identically.
		triangle := NewTriangle(
			V(-2, -1, -2), V(2, -1, -2), V(0, -1, 2),
			Vector{}, Vector{}, Vector{}, DiffuseMaterial(White))
		scene2 := &Scene{}
		scene2.Add(triangle)
		scene2.Compile()
		sampler2, err := NewSpectralSampler(scene2,
			map[Shape]SpectralMaterial{triangle: {Scatter: SpectralMirror, Reflectance: constSpectrum(0.25)}},
			constSpectrum(0.8), sensor, 2)
		if err != nil {
			t.Fatal(err)
		}
		got2, events2 := sampler2.TraceWavelengthPath(550, downRay, rand.New(rand.NewSource(6)))
		targetCheck(t, approx(got2, 0.2, 1e-12), "triangle mirror radiance")
		independent := downRay.Direction.Sub(events2[0].Normal.MulScalar(2 * downRay.Direction.Dot(events2[0].Normal)))
		targetCheck(t, approx(events2[0].Direction.Sub(independent).Length(), 0, 1e-12), "triangle mirror direction")
		t.Logf("plane event=%v triangle event dir=%v radiance=%.9g", events[0].Direction, events2[0].Direction, got2)
	})
}

// Independent 2D cross-section ray tracer used to validate dispersion paths.

type circle2d struct {
	cx, cy, radius float64
	index          func(float64) float64
}

type event2d struct {
	x, y, dx, dy float64
	entering     bool
	etaI, etaT   float64
}

func traceCircles2d(circles []circle2d, ox, oy, dx, dy, lambda float64, maxEvents int) []event2d {
	inside := make([]bool, len(circles))
	for i, c := range circles {
		if (ox-c.cx)*(ox-c.cx)+(oy-c.cy)*(oy-c.cy) < c.radius*c.radius {
			inside[i] = true
		}
	}
	mediumIndex := func(exclude int) float64 {
		idx, bestR := 1.0, math.Inf(1)
		for i, c := range circles {
			if i == exclude || !inside[i] {
				continue
			}
			if c.radius < bestR {
				bestR, idx = c.radius, c.index(lambda)
			}
		}
		return idx
	}
	var events []event2d
	x, y := ox, oy
	for step := 0; step < maxEvents; step++ {
		nearest := math.Inf(1)
		hitCircle := -1
		for i, c := range circles {
			ox2, oy2 := x-c.cx, y-c.cy
			b := ox2*dx + oy2*dy
			q := ox2*ox2 + oy2*oy2 - c.radius*c.radius
			disc := b*b - q
			if disc <= 0 {
				continue
			}
			sqrt := math.Sqrt(disc)
			for _, t := range []float64{-b - sqrt, -b + sqrt} {
				if t > 1e-9 && t < nearest {
					nearest, hitCircle = t, i
				}
			}
		}
		if hitCircle < 0 {
			return events
		}
		c := circles[hitCircle]
		x += nearest * dx
		y += nearest * dy
		nx, ny := (x-c.cx)/c.radius, (y-c.cy)/c.radius
		entering := !inside[hitCircle]
		if !entering {
			nx, ny = -nx, -ny // normal opposing the incident ray
		}
		etaI := mediumIndex(-1)
		var etaT float64
		if entering {
			etaT = c.index(lambda)
		} else {
			etaT = mediumIndex(hitCircle)
		}
		cosI := -(dx*nx + dy*ny)
		eta := etaI / etaT
		sinT2 := eta * eta * (1 - cosI*cosI)
		if sinT2 > 1 {
			dot := dx*nx + dy*ny
			dx, dy = dx-2*dot*nx, dy-2*dot*ny
			events = append(events, event2d{x, y, dx, dy, entering, etaI, etaT})
			continue
		}
		cosT := math.Sqrt(1 - sinT2)
		dx = eta*dx + (eta*cosI-cosT)*nx
		dy = eta*dy + (eta*cosI-cosT)*ny
		norm := math.Hypot(dx, dy)
		dx, dy = dx/norm, dy/norm
		inside[hitCircle] = entering
		events = append(events, event2d{x, y, dx, dy, entering, etaI, etaT})
	}
	return events
}
