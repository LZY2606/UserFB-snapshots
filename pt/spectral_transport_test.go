package pt

import (
	"math"
	"math/rand"
	"sort"
	"testing"
)

// ---------------------------------------------------------------------------
// independent reference helpers (own node partitions, own Snell/Fresnel)
// ---------------------------------------------------------------------------

// refSpectrum is the test's own piecewise-linear spectrum, evaluated and
// integrated without touching the implementation under test.
type refSpectrum struct {
	w, v []float64
}

func (s refSpectrum) eval(x float64) float64 {
	if x < s.w[0] || x > s.w[len(s.w)-1] {
		return 0
	}
	for i := 0; i+1 < len(s.w); i++ {
		if x <= s.w[i+1] {
			return s.v[i] + (s.v[i+1]-s.v[i])*(x-s.w[i])/(s.w[i+1]-s.w[i])
		}
	}
	return s.v[len(s.v)-1]
}

func (s refSpectrum) integral() float64 {
	total := 0.0
	for i := 0; i+1 < len(s.w); i++ {
		total += 0.5 * (s.v[i] + s.v[i+1]) * (s.w[i+1] - s.w[i])
	}
	return total
}

// integralRange integrates over [lo, hi]; exact because the spectrum is
// linear between its own nodes.
func (s refSpectrum) integralRange(lo, hi float64) float64 {
	pts := []float64{lo}
	for _, w := range s.w {
		if w > lo && w < hi {
			pts = append(pts, w)
		}
	}
	pts = append(pts, hi)
	total := 0.0
	for i := 0; i+1 < len(pts); i++ {
		a, b := pts[i], pts[i+1]
		total += 0.5 * (s.eval(a) + s.eval(b)) * (b - a)
	}
	return total
}

func unionNodes(specs ...refSpectrum) []float64 {
	seen := map[float64]bool{}
	var out []float64
	for _, s := range specs {
		for _, w := range s.w {
			if !seen[w] {
				seen[w] = true
				out = append(out, w)
			}
		}
	}
	sort.Float64s(out)
	return out
}

// productIntegral3 exactly integrates a*b*c, each piecewise linear, by
// expanding the cubic product on every segment of the union partition.
func productIntegral3(a, b, c refSpectrum) float64 {
	nodes := unionNodes(a, b, c)
	total := 0.0
	for i := 0; i+1 < len(nodes); i++ {
		w0, w1 := nodes[i], nodes[i+1]
		h := w1 - w0
		a0, a1 := a.eval(w0), a.eval(w1)
		b0, b1 := b.eval(w0), b.eval(w1)
		c0, c1 := c.eval(w0), c.eval(w1)
		ka, kb, kc := (a1-a0)/h, (b1-b0)/h, (c1-c0)/h
		total += a0*b0*c0*h +
			(ka*b0*c0+a0*kb*c0+a0*b0*kc)*h*h/2 +
			(ka*kb*c0+ka*b0*kc+a0*kb*kc)*h*h*h/3 +
			ka*kb*kc*h*h*h*h/4
	}
	return total
}

// fresnelR is the test's own unpolarized Fresnel reflectance.
func fresnelR(ni, nt, cosI float64) (r float64, tir bool) {
	eta := ni / nt
	sinT2 := eta * eta * (1 - cosI*cosI)
	if sinT2 >= 1 {
		return 1, true
	}
	cosT := math.Sqrt(1 - sinT2)
	rs := (ni*cosI - nt*cosT) / (ni*cosI + nt*cosT)
	rp := (nt*cosI - ni*cosT) / (nt*cosI + ni*cosT)
	return (rs*rs + rp*rp) / 2, false
}

// snell is the test's own refraction of unit direction i against the
// oriented unit normal n (n opposes i).
func snell(i, n Vector, ni, nt float64) Vector {
	cosI := -i.Dot(n)
	eta := ni / nt
	cosT := math.Sqrt(1 - eta*eta*(1-cosI*cosI))
	return i.MulScalar(eta).Add(n.MulScalar(eta*cosI - cosT))
}

func reflectDir(i, n Vector) Vector {
	return i.Sub(n.MulScalar(2 * i.Dot(n)))
}

// sphereHit is the test's own ray-sphere intersection (smallest t > 1e-9).
func sphereHit(o, d, c Vector, r float64) (float64, bool) {
	oc := o.Sub(c)
	b := oc.Dot(d)
	cc := oc.Dot(oc) - r*r
	disc := b*b - cc
	if disc < 0 {
		return 0, false
	}
	sq := math.Sqrt(disc)
	if t := -b - sq; t > 1e-9 {
		return t, true
	}
	if t := -b + sq; t > 1e-9 {
		return t, true
	}
	return 0, false
}

func mustSpectrum(t *testing.T, w, v []float64) Spectrum {
	t.Helper()
	s, err := NewSpectrum(w, v)
	if err != nil {
		t.Fatalf("NewSpectrum(%v, %v): %v", w, v, err)
	}
	return s
}

// sensorFixture builds a sensor from three triangular responses on
// different node partitions, plus a sampler on an empty scene that only
// serves the wavelength distribution.
func sensorFixture(t *testing.T) (refSpectrum, refSpectrum, refSpectrum, *SpectralSampler, float64) {
	t.Helper()
	sr := refSpectrum{[]float64{400, 550, 700}, []float64{0, 1, 0}}
	sg := refSpectrum{[]float64{420, 500, 650, 700}, []float64{0, 0.5, 1, 0}}
	sb := refSpectrum{[]float64{400, 480, 560}, []float64{0.2, 1, 0}}
	sensor, err := NewSensor(
		mustSpectrum(t, sr.w, sr.v),
		mustSpectrum(t, sg.w, sg.v),
		mustSpectrum(t, sb.w, sb.v),
	)
	if err != nil {
		t.Fatalf("NewSensor: %v", err)
	}
	scene := &Scene{}
	smp, err := NewSpectralSampler(scene, map[Shape]SpectralMaterial{}, Spectrum{}, sensor, 0)
	if err != nil {
		t.Fatalf("NewSpectralSampler: %v", err)
	}
	z := sr.integral() + sg.integral() + sb.integral()
	return sr, sg, sb, smp, z
}

// ---------------------------------------------------------------------------
// TestSpectralSensor: CDF, PDF, sampling frequency and spectral integrals
// ---------------------------------------------------------------------------

func TestSpectralSensor(t *testing.T) {
	sr, sg, sb, smp, z := sensorFixture(t)

	t.Run("PDFAndIntegral", func(t *testing.T) {
		// analytic areas: Sr is a triangle, 1/2 * 300 * 1 = 150
		targetCheck(t, math.Abs(sr.integral()-150) < 1e-12, "Sr area", sr.integral())
		for _, lambda := range []float64{400, 410, 470, 540, 620, 690, 700} {
			sum := sr.eval(lambda) + sg.eval(lambda) + sb.eval(lambda)
			want := sum / z
			got := smp.WavelengthPDF(lambda)
			targetCheck(t, math.Abs(got-want) < 1e-12,
				"pdf mismatch", "lambda", lambda, "got", got, "want", want)
		}
		// outside support the density must vanish
		targetCheck(t, smp.WavelengthPDF(399) == 0 && smp.WavelengthPDF(701) == 0,
			"pdf outside support")
		// spectral integral recovered from the pdf normalization
		lambda := 540.0
		sum := sr.eval(lambda) + sg.eval(lambda) + sb.eval(lambda)
		gotZ := sum / smp.WavelengthPDF(lambda)
		targetCheck(t, math.Abs(gotZ-z) < 1e-9, "integral", "got", gotZ, "want", z)
		t.Logf("SENSOR_PDF Z=%.9g (areas %.9g %.9g %.9g) pdf(540)=%.9g",
			z, sr.integral(), sg.integral(), sb.integral(), smp.WavelengthPDF(540))
	})

	t.Run("CDFAndFrequencies", func(t *testing.T) {
		rnd := rand.New(rand.NewSource(7))
		const n = 200000
		samples := make([]float64, n)
		for i := range samples {
			l, pdf := smp.SampleWavelength(rnd)
			targetCheck(t, l >= 400 && l <= 700, "wavelength out of range", l)
			targetCheck(t, pdf > 0, "non-positive pdf", l, pdf)
			samples[i] = l
		}
		for _, lambda := range []float64{450, 530, 620, 680} {
			want := (sr.integralRange(400, lambda) +
				sg.integralRange(400, lambda) +
				sb.integralRange(400, lambda)) / z
			emp := 0
			for _, l := range samples {
				if l < lambda {
					emp++
				}
			}
			got := float64(emp) / n
			targetCheck(t, math.Abs(got-want) < 0.01,
				"cdf mismatch", "lambda", lambda, "got", got, "want", want)
			t.Logf("SENSOR_CDF lambda=%.1f cdf=%.6f empirical=%.6f err=%.6f n=%d",
				lambda, want, got, got-want, n)
		}
		// per-segment frequencies on the union node partition
		nodes := unionNodes(sr, sg, sb)
		for i := 0; i+1 < len(nodes); i++ {
			lo, hi := nodes[i], nodes[i+1]
			want := (sr.integralRange(lo, hi) +
				sg.integralRange(lo, hi) +
				sb.integralRange(lo, hi)) / z
			emp := 0
			for _, l := range samples {
				if l >= lo && l < hi {
					emp++
				}
			}
			got := float64(emp) / n
			sigma := math.Sqrt(want*(1-want)/n + 1e-12)
			tol := 6 * sigma
			if tol < 1e-3 {
				tol = 1e-3
			}
			targetCheck(t, math.Abs(got-want) < tol,
				"segment frequency", "segment", lo, hi, "got", got, "want", want)
			t.Logf("SENSOR_SEGMENT [%.0f,%.0f] p=%.6f empirical=%.6f err=%.6f",
				lo, hi, want, got, got-want)
		}
	})

	t.Run("Validation", func(t *testing.T) {
		bad := []struct{ w, v []float64 }{
			{[]float64{500}, []float64{1}},                   // too few nodes
			{[]float64{400, 400}, []float64{0, 0}},           // not increasing
			{[]float64{500, 400}, []float64{0, 0}},           // decreasing
			{[]float64{399, 700}, []float64{0, 0}},           // below 400
			{[]float64{400, 701}, []float64{0, 0}},           // above 700
			{[]float64{400, 700}, []float64{0, -1}},          // negative value
			{[]float64{400, 700}, []float64{0, math.NaN()}},  // NaN value
			{[]float64{400, 700}, []float64{0, math.Inf(1)}}, // Inf value
		}
		for i, c := range bad {
			_, err := NewSpectrum(c.w, c.v)
			targetCheck(t, err != nil, "invalid spectrum accepted", "case", i)
		}
		zero := mustSpectrum(t, []float64{400, 700}, []float64{0, 0})
		_, err := NewSensor(zero, zero, zero)
		targetCheck(t, err != nil, "zero-response sensor accepted")
		_, err = NewSpectrum([]float64{400, 700}, []float64{1})
		targetCheck(t, err != nil, "length mismatch accepted")
	})
}

// ---------------------------------------------------------------------------
// TestSpectralMedia: nested spheres, interior starts, stack discipline, TIR
// ---------------------------------------------------------------------------

func mediaSensor(t *testing.T) Sensor {
	t.Helper()
	s := mustSpectrum(t, []float64{400, 700}, []float64{1, 1})
	sensor, err := NewSensor(s, s, s)
	if err != nil {
		t.Fatalf("NewSensor: %v", err)
	}
	return sensor
}

func TestSpectralMedia(t *testing.T) {
	one := mustSpectrum(t, []float64{400, 700}, []float64{1, 1})
	zeroA := mustSpectrum(t, []float64{400, 700}, []float64{0, 0})
	sensor := mediaSensor(t)

	t.Run("RefractionDirectionAndFresnel", func(t *testing.T) {
		// interior start inside a single n=1.5 sphere; detector placed on
		// the independently computed exit ray
		glass := NewSphere(V(0, 0, 0), 1, Material{})
		o, dir := V(0.1, 0, -0.2), V(0, 0, 1)
		ni := 1.5
		t1, ok := sphereHit(o, dir, V(0, 0, 0), 1)
		targetCheck(t, ok, "no sphere hit in reference")
		p1 := o.Add(dir.MulScalar(t1))
		n1 := p1.Normalize().Negate() // oriented against dir
		cosI := -dir.Dot(n1)
		fr, tir := fresnelR(ni, 1, cosI)
		targetCheck(t, !tir, "unexpected TIR in reference")
		exit := snell(dir, n1, ni, 1)
		det := NewSphere(p1.Add(exit.MulScalar(3)), 0.4, Material{})
		scene := &Scene{}
		scene.Add(glass)
		scene.Add(det)
		smp, err := NewSpectralSampler(scene, map[Shape]SpectralMaterial{
			glass: DielectricMaterial(ni, 0, Spectrum{}),
			det:   LambertMaterial(zeroA, one),
		}, Spectrum{}, sensor, 2)
		targetCheck(t, err == nil, "constructor", err)
		rnd := rand.New(rand.NewSource(11))
		const n = 20000
		lambda := 550.0
		hits := 0
		sum := 0.0
		for i := 0; i < n; i++ {
			l := smp.Trace(Ray{o, dir}, lambda, rnd)
			if l > 0 {
				hits++
			}
			sum += l
		}
		pWant := 1 - fr
		pGot := float64(hits) / n
		mean := sum / n
		meanWant := pWant * ni * ni // radiance scaling (ni/1)^2, emission 1
		sigma := math.Sqrt(pWant*(1-pWant))/math.Sqrt(n)
		targetCheck(t, math.Abs(pGot-pWant) < 5*sigma+1e-4,
			"transmit fraction", "got", pGot, "want", pWant)
		targetCheck(t, math.Abs(mean-meanWant) < 5*sigma*ni*ni+1e-4,
			"mean radiance", "got", mean, "want", meanWant)
		t.Logf("MEDIA_REFRACT lambda=%.1f exitDir=(%.6f,%.6f,%.6f) pTransmit=%.5f got=%.5f mean=%.5f want=%.5f err=%.5f n=%d",
			lambda, exit.X, exit.Y, exit.Z, pWant, pGot, mean, meanWant, mean-meanWant, n)
	})

	t.Run("ReflectionKeepsMediumStack", func(t *testing.T) {
		// the ray must reflect at the first boundary (probability F) and
		// then, still inside the medium, refract out at the second
		// boundary toward a detector placed on the doubly-computed path
		glass := NewSphere(V(0, 0, 0), 1, Material{})
		o, dir := V(0.1, 0, -0.2), V(0, 0, 1)
		ni := 1.5
		t1, _ := sphereHit(o, dir, V(0, 0, 0), 1)
		p1 := o.Add(dir.MulScalar(t1))
		n1 := p1.Normalize().Negate()
		cosI := -dir.Dot(n1)
		fr, _ := fresnelR(ni, 1, cosI)
		refl := reflectDir(dir, n1)
		// second boundary point along the internal chord
		t2, ok := sphereHit(p1, refl, V(0, 0, 0), 1)
		targetCheck(t, ok, "no second boundary in reference")
		p2 := p1.Add(refl.MulScalar(t2))
		n2 := p2.Normalize().Negate()
		cosI2 := -refl.Dot(n2)
		fr2, tir2 := fresnelR(ni, 1, cosI2)
		targetCheck(t, !tir2, "unexpected TIR at second boundary")
		exit2 := snell(refl, n2, ni, 1)
		det := NewSphere(p2.Add(exit2.MulScalar(3)), 0.4, Material{})
		scene := &Scene{}
		scene.Add(glass)
		scene.Add(det)
		smp, err := NewSpectralSampler(scene, map[Shape]SpectralMaterial{
			glass: DielectricMaterial(ni, 0, Spectrum{}),
			det:   LambertMaterial(zeroA, one),
		}, Spectrum{}, sensor, 3)
		targetCheck(t, err == nil, "constructor", err)
		rnd := rand.New(rand.NewSource(13))
		const n = 50000
		lambda := 550.0
		hits := 0
		sum := 0.0
		for i := 0; i < n; i++ {
			l := smp.Trace(Ray{o, dir}, lambda, rnd)
			if l > 0 {
				hits++
			}
			sum += l
		}
		// correct stack: F1 * (1-F2) * (ni/1)^2; a popped stack would
		// refract with the wrong ratio and miss the detector entirely
		pWant := fr * (1 - fr2)
		pGot := float64(hits) / n
		mean := sum / n
		meanWant := pWant * ni * ni
		sigma := math.Sqrt(pWant*(1-pWant)) / math.Sqrt(n)
		targetCheck(t, math.Abs(pGot-pWant) < 5*sigma+1e-4,
			"reflect-transmit fraction", "got", pGot, "want", pWant)
		targetCheck(t, math.Abs(mean-meanWant) < 5*sigma*ni*ni+1e-4,
			"mean radiance", "got", mean, "want", meanWant)
		t.Logf("MEDIA_STACK lambda=%.1f F1=%.5f F2=%.5f p=%.5f got=%.5f mean=%.5f want=%.5f err=%.5f n=%d",
			lambda, fr, fr2, pWant, pGot, mean, meanWant, mean-meanWant, n)
	})

	t.Run("TotalInternalReflection", func(t *testing.T) {
		// 53.1 deg incidence: above the critical angle for n=1.5 (41.8
		// deg) but below it for n=1.1 (65.4 deg)
		o, dir := V(0.8, 0, -0.2), V(0, 0, 1)
		lambda := 550.0
		const n = 5000
		for _, cfg := range []struct {
			ni      float64
			wantL   float64
			wantEsc bool
		}{
			{1.5, 0, false},
			{1.1, 1.1 * 1.1, true},
		} {
			glass := NewSphere(V(0, 0, 0), 1, Material{})
			scene := &Scene{}
			scene.Add(glass)
			smp, err := NewSpectralSampler(scene, map[Shape]SpectralMaterial{
				glass: DielectricMaterial(cfg.ni, 0, Spectrum{}),
			}, one, sensor, 8)
			targetCheck(t, err == nil, "constructor", err)
			t1, _ := sphereHit(o, dir, V(0, 0, 0), 1)
			p1 := o.Add(dir.MulScalar(t1))
			cosI := p1.Normalize().Dot(dir)
			_, tir := fresnelR(cfg.ni, 1, cosI)
			targetCheck(t, tir != cfg.wantEsc, "reference TIR classification", cfg.ni)
			rnd := rand.New(rand.NewSource(17))
			esc := 0
			for i := 0; i < n; i++ {
				l := smp.Trace(Ray{o, dir}, lambda, rnd)
				if cfg.wantEsc {
					if math.Abs(l-cfg.wantL) < 1e-9 {
						esc++
					} else {
						targetCheck(t, l == 0, "unexpected radiance", l)
					}
				} else {
					targetCheck(t, l == 0, "TIR path escaped", l)
				}
			}
			if cfg.wantEsc {
				targetCheck(t, esc == n, "not all paths escaped", esc, n)
			}
			t.Logf("MEDIA_TIR n=%.1f lambda=%.1f cosI=%.4f tir=%v escaped=%d/%d",
				cfg.ni, lambda, cosI, tir, esc, n)
		}
	})

	t.Run("EccentricNestedSpheres", func(t *testing.T) {
		// eccentric nesting: inner sphere offset from the outer center;
		// ray starts inside the inner sphere and must cross inner->outer
		// (not inner->air) before leaving to the environment
		outer := NewSphere(V(0, 0, 0), 2, Material{})
		inner := NewSphere(V(0.9, 0, 0), 0.6, Material{})
		nOut, nIn := 1.5, 2.0
		o := V(0.9, 0, 0.2)
		dir := V(0.4, 0, 1).Normalize()
		// interface 1: inner (n=2) -> outer medium (n=1.5)
		t1, ok := sphereHit(o, dir, V(0.9, 0, 0), 0.6)
		targetCheck(t, ok, "no inner hit in reference")
		p1 := o.Add(dir.MulScalar(t1))
		g1 := p1.Sub(V(0.9, 0, 0)).Normalize()
		n1 := g1.Negate() // exiting, orient against dir
		cosI1 := -dir.Dot(n1)
		f1, tir1 := fresnelR(nIn, nOut, cosI1)
		targetCheck(t, !tir1, "unexpected TIR at inner boundary")
		d1 := snell(dir, n1, nIn, nOut)
		// interface 2: outer medium (n=1.5) -> air
		t2, ok := sphereHit(p1, d1, V(0, 0, 0), 2)
		targetCheck(t, ok, "no outer hit in reference")
		p2 := p1.Add(d1.MulScalar(t2))
		n2 := p2.Normalize().Negate()
		cosI2 := -d1.Dot(n2)
		f2, tir2 := fresnelR(nOut, 1, cosI2)
		targetCheck(t, !tir2, "unexpected TIR at outer boundary")
		d2 := snell(d1, n2, nOut, 1)
		det := NewSphere(p2.Add(d2.MulScalar(3)), 0.4, Material{})
		scene := &Scene{}
		scene.Add(outer)
		scene.Add(inner)
		scene.Add(det)
		smp, err := NewSpectralSampler(scene, map[Shape]SpectralMaterial{
			outer: DielectricMaterial(nOut, 0, Spectrum{}),
			inner: DielectricMaterial(nIn, 0, Spectrum{}),
			det:   LambertMaterial(zeroA, one),
		}, Spectrum{}, sensor, 3)
		targetCheck(t, err == nil, "constructor", err)
		rnd := rand.New(rand.NewSource(19))
		const n = 50000
		lambda := 550.0
		hits := 0
		sum := 0.0
		for i := 0; i < n; i++ {
			l := smp.Trace(Ray{o, dir}, lambda, rnd)
			if l > 0 {
				hits++
			}
			sum += l
		}
		pWant := (1 - f1) * (1 - f2)
		pGot := float64(hits) / n
		mean := sum / n
		meanWant := pWant * nIn * nIn // telescoped (nIn/1)^2, emission 1
		sigma := math.Sqrt(pWant*(1-pWant)) / math.Sqrt(n)
		targetCheck(t, math.Abs(pGot-pWant) < 5*sigma+1e-4,
			"nested transmit fraction", "got", pGot, "want", pWant)
		targetCheck(t, math.Abs(mean-meanWant) < 5*sigma*nIn*nIn+1e-4,
			"nested mean radiance", "got", mean, "want", meanWant)
		t.Logf("MEDIA_NESTED lambda=%.1f p1=(%.4f,%.4f,%.4f) d1=(%.4f,%.4f,%.4f) p2=(%.4f,%.4f,%.4f) d2=(%.4f,%.4f,%.4f) p=%.5f got=%.5f mean=%.5f want=%.5f err=%.5f n=%d",
			lambda, p1.X, p1.Y, p1.Z, d1.X, d1.Y, d1.Z, p2.X, p2.Y, p2.Z, d2.X, d2.Y, d2.Z,
			pWant, pGot, mean, meanWant, mean-meanWant, n)
	})

	t.Run("Validation", func(t *testing.T) {
		mkScene := func(shapes ...Shape) *Scene {
			s := &Scene{}
			for _, sh := range shapes {
				s.Add(sh)
			}
			return s
		}
		diel := DielectricMaterial(1.5, 0, Spectrum{})
		// tangent, intersecting and internally tangent dielectric spheres
		for i, pair := range [][2]Vector{
			{V(0, 0, 0), V(2, 0, 0)},   // externally tangent
			{V(0, 0, 0), V(1.5, 0, 0)}, // intersecting
			{V(0, 0, 0), V(0.5, 0, 0)}, // internally tangent (r2 = 0.5)
		} {
			r2 := 1.0
			if i == 2 {
				r2 = 0.5
			}
			s1 := NewSphere(pair[0], 1, Material{})
			s2 := NewSphere(pair[1], r2, Material{})
			_, err := NewSpectralSampler(mkScene(s1, s2), map[Shape]SpectralMaterial{
				s1: diel, s2: diel,
			}, Spectrum{}, sensor, 1)
			targetCheck(t, err != nil, "invalid sphere pair accepted", "case", i)
		}
		// strictly nested and strictly separate are both legal
		for i, pair := range [][2]Vector{
			{V(0, 0, 0), V(0.4, 0, 0)}, // nested (r2 = 0.5)
			{V(0, 0, 0), V(3, 0, 0)},   // separate
		} {
			r2 := 1.0
			if i == 0 {
				r2 = 0.5
			}
			s1 := NewSphere(pair[0], 1, Material{})
			s2 := NewSphere(pair[1], r2, Material{})
			_, err := NewSpectralSampler(mkScene(s1, s2), map[Shape]SpectralMaterial{
				s1: diel, s2: diel,
			}, Spectrum{}, sensor, 1)
			targetCheck(t, err == nil, "valid sphere pair rejected", "case", i, err)
		}
		// dielectric on a non-sphere
		pl := NewPlane(V(0, 0, 0), V(0, 1, 0), Material{})
		_, err := NewSpectralSampler(mkScene(pl), map[Shape]SpectralMaterial{
			pl: diel,
		}, Spectrum{}, sensor, 1)
		targetCheck(t, err != nil, "dielectric plane accepted")
		// bad Cauchy coefficients
		for i, m := range []SpectralMaterial{
			DielectricMaterial(0.5, 0, Spectrum{}),
			DielectricMaterial(1.5, -1, Spectrum{}),
			DielectricMaterial(math.NaN(), 0, Spectrum{}),
			DielectricMaterial(1.5, math.Inf(1), Spectrum{}),
		} {
			sp := NewSphere(V(0, 0, 0), 1, Material{})
			_, err := NewSpectralSampler(mkScene(sp), map[Shape]SpectralMaterial{
				sp: m,
			}, Spectrum{}, sensor, 1)
			targetCheck(t, err != nil, "bad dielectric constants accepted", "case", i)
		}
		// reflectance above 1
		sp := NewSphere(V(0, 0, 0), 1, Material{})
		big := mustSpectrum(t, []float64{400, 700}, []float64{1.5, 1.5})
		_, err = NewSpectralSampler(mkScene(sp), map[Shape]SpectralMaterial{
			sp: MirrorMaterial(big, Spectrum{}),
		}, Spectrum{}, sensor, 1)
		targetCheck(t, err != nil, "reflectance > 1 accepted")
		_, err = NewSpectralSampler(mkScene(sp), map[Shape]SpectralMaterial{
			sp: LambertMaterial(big, Spectrum{}),
		}, Spectrum{}, sensor, 1)
		targetCheck(t, err != nil, "albedo > 1 accepted")
		// missing material
		_, err = NewSpectralSampler(mkScene(sp), map[Shape]SpectralMaterial{},
			Spectrum{}, sensor, 1)
		targetCheck(t, err != nil, "missing material accepted")
		// unsupported shape type (transformed shapes are not supported)
		ts := NewTransformedShape(NewSphere(V(0, 0, 0), 1, Material{}), Identity())
		_, err = NewSpectralSampler(mkScene(ts), map[Shape]SpectralMaterial{
			ts: LambertMaterial(zeroA, Spectrum{}),
		}, Spectrum{}, sensor, 1)
		targetCheck(t, err != nil, "transformed shape accepted")
		// negative MaxBounces
		_, err = NewSpectralSampler(mkScene(sp), map[Shape]SpectralMaterial{
			sp: LambertMaterial(zeroA, Spectrum{}),
		}, Spectrum{}, sensor, -1)
		targetCheck(t, err != nil, "negative MaxBounces accepted")
	})
}

// ---------------------------------------------------------------------------
// TestSpectralTransport: full Scene tree + Sampler integration
// ---------------------------------------------------------------------------

func TestSpectralTransport(t *testing.T) {
	sr, sg, sb, _, _ := sensorFixture(t)
	mkSensor := func() Sensor {
		sensor, err := NewSensor(
			mustSpectrum(t, sr.w, sr.v),
			mustSpectrum(t, sg.w, sg.v),
			mustSpectrum(t, sb.w, sb.v),
		)
		if err != nil {
			t.Fatalf("NewSensor: %v", err)
		}
		return sensor
	}

	t.Run("LambertProductIntegral", func(t *testing.T) {
		// constant-ish environment and a Lambert albedo: L = a*env after
		// one bounce, so channel means equal the analytic integrals of
		// albedo*env*response
		albedo := refSpectrum{[]float64{400, 550, 700}, []float64{0.2, 0.8, 0.4}}
		env := refSpectrum{[]float64{400, 700}, []float64{0.8, 1.2}}
		sphere := NewSphere(V(0, 0, -3), 1, Material{})
		scene := &Scene{}
		scene.Add(sphere)
		smp, err := NewSpectralSampler(scene, map[Shape]SpectralMaterial{
			sphere: LambertMaterial(mustSpectrum(t, albedo.w, albedo.v), Spectrum{}),
		}, mustSpectrum(t, env.w, env.v), mkSensor(), 1)
		targetCheck(t, err == nil, "constructor", err)
		ray := Ray{V(0, 0, 0), V(0, 0, -1)}
		want := Color{
			R: productIntegral3(albedo, env, sr),
			G: productIntegral3(albedo, env, sg),
			B: productIntegral3(albedo, env, sb),
		}
		const n = 200000
		rnd := rand.New(rand.NewSource(99))
		var sum, sum2 Color
		for i := 0; i < n; i++ {
			c := smp.Sample(scene, ray, rnd)
			sum = sum.Add(c)
			sum2 = sum2.Add(c.Mul(c))
		}
		mean := sum.DivScalar(n)
		varR := sum2.R/n - mean.R*mean.R
		varG := sum2.G/n - mean.G*mean.G
		varB := sum2.B/n - mean.B*mean.B
		seR := math.Sqrt(varR / n)
		seG := math.Sqrt(varG / n)
		seB := math.Sqrt(varB / n)
		targetCheck(t, math.Abs(mean.R-want.R) < 5*seR,
			"R mean", "got", mean.R, "want", want.R, "se", seR)
		targetCheck(t, math.Abs(mean.G-want.G) < 5*seG,
			"G mean", "got", mean.G, "want", want.G, "se", seG)
		targetCheck(t, math.Abs(mean.B-want.B) < 5*seB,
			"B mean", "got", mean.B, "want", want.B, "se", seB)
		targetCheck(t, scene.RayCount() > 0, "scene tree not used")
		t.Logf("TRANSPORT_LAMBERT mean=(%.6f,%.6f,%.6f) want=(%.6f,%.6f,%.6f) err=(%.6f,%.6f,%.6f) se=(%.6f,%.6f,%.6f) n=%d rays=%d",
			mean.R, mean.G, mean.B, want.R, want.G, want.B,
			mean.R-want.R, mean.G-want.G, mean.B-want.B, seR, seG, seB, n, scene.RayCount())
		// fixed seed reproduces the exact same result
		c1 := smp.Sample(scene, ray, rand.New(rand.NewSource(123)))
		c2 := smp.Sample(scene, ray, rand.New(rand.NewSource(123)))
		targetCheck(t, c1 == c2, "same seed, different result", c1, c2)
		// RGB reference path (old DefaultSampler) on an equivalent scene
		rgbScene := &Scene{Color: Color{1, 1, 1}}
		rgbScene.Add(NewSphere(V(0, 0, -3), 1, DiffuseMaterial(Color{0.5, 0.5, 0.5})))
		rgbScene.Compile()
		ds := NewSampler(1, 1)
		ds.DirectLighting = false
		rgb := ds.Sample(rgbScene, ray, rand.New(rand.NewSource(41)))
		t.Logf("RGB_REFERENCE old=(%.6f,%.6f,%.6f)", rgb.R, rgb.G, rgb.B)
	})

	t.Run("DispersionPaths", func(t *testing.T) {
		// dispersive glass sphere: n(lambda) = 1.5 + 0.05/(lambda/1000)^2.
		// Two emissive detectors are placed on the independently computed
		// 2D cross-section exit paths for 450 nm and 680 nm; each
		// wavelength must arrive at its own detector.
		const A, B = 1.5, 0.05
		nAt := func(lambda float64) float64 {
			x := lambda / 1000
			return A + B/(x*x)
		}
		glass := NewSphere(V(0, 0, 0), 1, Material{})
		o, dir := V(-5, 0.6, 0), V(1, 0, 0)
		// interface-by-interface 2D reference path through the sphere
		type refPath struct {
			p1, d1, p2, d2 Vector
			f1, f2         float64
		}
		compute := func(lambda float64) refPath {
			nl := nAt(lambda)
			t1, ok := sphereHit(o, dir, V(0, 0, 0), 1)
			targetCheck(t, ok, "no entry hit in reference")
			p1 := o.Add(dir.MulScalar(t1))
			g1 := p1.Normalize() // outward; dir.g1 < 0 so it opposes dir
			cosI1 := -dir.Dot(g1)
			f1, tir := fresnelR(1, nl, cosI1)
			targetCheck(t, !tir, "TIR at entry in reference")
			d1 := snell(dir, g1, 1, nl)
			t2, ok := sphereHit(p1, d1, V(0, 0, 0), 1)
			targetCheck(t, ok, "no exit hit in reference")
			p2 := p1.Add(d1.MulScalar(t2))
			g2 := p2.Normalize().Negate() // oriented against d1
			cosI2 := -d1.Dot(g2)
			f2, tir := fresnelR(nl, 1, cosI2)
			targetCheck(t, !tir, "TIR at exit in reference")
			d2 := snell(d1, g2, nl, 1)
			return refPath{p1, d1, p2, d2, f1, f2}
		}
		const lambdaA, lambdaB = 450.0, 680.0
		pa := compute(lambdaA)
		pb := compute(lambdaB)
		// wavelengths must genuinely separate
		sep := pa.d2.Sub(pb.d2).Length()
		targetCheck(t, sep > 0.05, "exit directions do not separate", sep)
		detA := NewSphere(pa.p2.Add(pa.d2.MulScalar(4)), 0.1, Material{})
		detB := NewSphere(pb.p2.Add(pb.d2.MulScalar(4)), 0.1, Material{})
		detSep := detA.(*Sphere).Center.Sub(detB.(*Sphere).Center).Length()
		targetCheck(t, detSep > 0.2, "detectors overlap", detSep)
		emA := mustSpectrum(t, []float64{400, 700}, []float64{1, 1})
		emB := mustSpectrum(t, []float64{400, 700}, []float64{2, 2})
		zeroA := mustSpectrum(t, []float64{400, 700}, []float64{0, 0})
		scene := &Scene{}
		scene.Add(glass)
		scene.Add(detA)
		scene.Add(detB)
		smp, err := NewSpectralSampler(scene, map[Shape]SpectralMaterial{
			glass: DielectricMaterial(A, B, Spectrum{}),
			detA:  LambertMaterial(zeroA, emA),
			detB:  LambertMaterial(zeroA, emB),
		}, Spectrum{}, mkSensor(), 3)
		targetCheck(t, err == nil, "constructor", err)
		const n = 20000
		run := func(lambda, eDet float64, rp refPath, rnd *rand.Rand) (float64, float64) {
			hits := 0
			sum := 0.0
			for i := 0; i < n; i++ {
				l := smp.Trace(Ray{o, dir}, lambda, rnd)
				if l > 0 {
					hits++
					// throughput telescopes to 1 (air->glass->air), so
					// every positive sample must equal this detector's
					// emission and no other detector's
					targetCheck(t, math.Abs(l-eDet) < 1e-6,
						"wrong detector or throughput", "lambda", lambda, "L", l)
				}
				sum += l
			}
			pWant := (1 - rp.f1) * (1 - rp.f2)
			pGot := float64(hits) / n
			mean := sum / n
			meanWant := pWant * eDet
			sigma := math.Sqrt(pWant*(1-pWant)) / math.Sqrt(n)
			targetCheck(t, math.Abs(pGot-pWant) < 5*sigma+1e-4,
				"arrival fraction", "lambda", lambda, "got", pGot, "want", pWant)
			targetCheck(t, math.Abs(mean-meanWant) < 5*sigma*eDet+1e-4,
				"mean radiance", "lambda", lambda, "got", mean, "want", meanWant)
			t.Logf("TRANSPORT_DISPERSION lambda=%.1f n=%.6f exitDir=(%.6f,%.6f,%.6f) p=%.5f got=%.5f mean=%.5f want=%.5f err=%.5f n=%d",
				lambda, nAt(lambda), rp.d2.X, rp.d2.Y, rp.d2.Z, pWant, pGot, mean, meanWant, mean-meanWant, n)
			return pGot, mean
		}
		run(lambdaA, 1, pa, rand.New(rand.NewSource(23)))
		run(lambdaB, 2, pb, rand.New(rand.NewSource(29)))
		t.Logf("TRANSPORT_DISPERSION_SEP dDir=%.6f dDet=%.6f", sep, detSep)
	})

	t.Run("InternalChordInterface", func(t *testing.T) {
		// a tiny detector inside the glass on the 450 nm internal chord:
		// the 450 nm path hits it, the 680 nm path must miss it
		const A, B = 1.5, 0.05
		nAt := func(lambda float64) float64 {
			x := lambda / 1000
			return A + B/(x*x)
		}
		glass := NewSphere(V(0, 0, 0), 1, Material{})
		o, dir := V(-5, 0.6, 0), V(1, 0, 0)
		chordMid := func(lambda float64) (Vector, float64) {
			nl := nAt(lambda)
			t1, _ := sphereHit(o, dir, V(0, 0, 0), 1)
			p1 := o.Add(dir.MulScalar(t1))
			g1 := p1.Normalize()
			cosI1 := -dir.Dot(g1)
			f1, _ := fresnelR(1, nl, cosI1)
			d1 := snell(dir, g1, 1, nl)
			t2, _ := sphereHit(p1, d1, V(0, 0, 0), 1)
			return p1.Add(d1.MulScalar(t2 / 2)), f1
		}
		const lambdaA, lambdaB = 450.0, 680.0
		midA, f1 := chordMid(lambdaA)
		midB, _ := chordMid(lambdaB)
		chordSep := midA.Sub(midB).Length()
		detIn := NewSphere(midA, 0.015, Material{})
		targetCheck(t, chordSep > 0.015, "chords do not separate", chordSep)
		emIn := mustSpectrum(t, []float64{400, 700}, []float64{3, 3})
		zeroA := mustSpectrum(t, []float64{400, 700}, []float64{0, 0})
		scene := &Scene{}
		scene.Add(glass)
		scene.Add(detIn)
		smp, err := NewSpectralSampler(scene, map[Shape]SpectralMaterial{
			glass: DielectricMaterial(A, B, Spectrum{}),
			detIn: LambertMaterial(zeroA, emIn),
		}, Spectrum{}, mkSensor(), 2)
		targetCheck(t, err == nil, "constructor", err)
		const n = 20000
		nA := nAt(lambdaA)
		rnd := rand.New(rand.NewSource(31))
		sum := 0.0
		for i := 0; i < n; i++ {
			sum += smp.Trace(Ray{o, dir}, lambdaA, rnd)
		}
		mean := sum / n
		meanWant := (1 - f1) * 3 / (nA * nA) // radiance scaling (1/nA)^2
		pWant := 1 - f1
		sigma := math.Sqrt(pWant*(1-pWant)) / math.Sqrt(n)
		targetCheck(t, math.Abs(mean-meanWant) < 5*sigma*3/(nA*nA)+1e-4,
			"internal chord mean", "got", mean, "want", meanWant)
		// the other wavelength's chord misses the detector completely
		rnd2 := rand.New(rand.NewSource(37))
		maxL := 0.0
		for i := 0; i < n; i++ {
			if l := smp.Trace(Ray{o, dir}, lambdaB, rnd2); l > maxL {
				maxL = l
			}
		}
		targetCheck(t, maxL == 0, "680nm chord hit the 450nm detector", maxL)
		t.Logf("TRANSPORT_CHORD lambdaA=%.1f nA=%.6f chordSep=%.6f mean=%.5f want=%.5f err=%.5f maxL(680nm)=%.3g n=%d",
			lambdaA, nA, chordSep, mean, meanWant, mean-meanWant, maxL, n)
	})

	t.Run("MaxBouncesZeroEscape", func(t *testing.T) {
		env := mustSpectrum(t, []float64{400, 700}, []float64{1, 1})
		sphere := NewSphere(V(0, 0, -3), 1, Material{})
		scene := &Scene{}
		scene.Add(sphere)
		smp, err := NewSpectralSampler(scene, map[Shape]SpectralMaterial{
			sphere: LambertMaterial(mustSpectrum(t, []float64{400, 700}, []float64{0.5, 0.5}), Spectrum{}),
		}, env, mkSensor(), 0)
		targetCheck(t, err == nil, "constructor", err)
		rnd := rand.New(rand.NewSource(43))
		lambda := 550.0
		lHit := smp.Trace(Ray{V(0, 0, 0), V(0, 0, -1)}, lambda, rnd)
		targetCheck(t, lHit == 0, "surface event processed with MaxBounces=0", lHit)
		lSky := smp.Trace(Ray{V(0, 0, 0), V(0, 0, 1)}, lambda, rnd)
		targetCheck(t, lSky == 1, "environment escape wrong", lSky)
		t.Logf("TRANSPORT_BOUNCE0 lambda=%.1f hit=%.3g sky=%.3g", lambda, lHit, lSky)
	})
}
