package pt

import (
	"math"
	"math/rand"
	"testing"

	"github.com/fogleman/pt/task_support/taskcheck"
)

func fluoroTestSpectrum(c *taskcheck.C, nodes, vals []float64) Spectrum {
	s, err := NewSpectrum(nodes, vals)
	c.Check(err == nil, "NewSpectrum(%v, %v) error: %v", nodes, vals, err)
	return s
}

func fluoroTestZero(c *taskcheck.C) Spectrum {
	return fluoroTestSpectrum(c, []float64{400, 700}, []float64{0, 0})
}

// fluoroTestQuad2D integrates f over [400,700]^2 by the midpoint rule
// on a grid aligned with integer wavelengths. This is the independent
// quadrature used to cross-check the analytic spectral integrals.
func fluoroTestQuad2D(step float64, f func(li, lo float64) float64) float64 {
	n := int(300/step + 0.5)
	sum := 0.0
	for i := 0; i < n; i++ {
		li := 400 + (float64(i)+0.5)*step
		for j := 0; j < n; j++ {
			lo := 400 + (float64(j)+0.5)*step
			sum += f(li, lo)
		}
	}
	return sum * step * step
}

func TestFluoroKernel(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		zero := fluoroTestZero(c)
		albedo := fluoroTestSpectrum(c, []float64{400, 700}, []float64{0.2, 0.2})
		// excitation and emission grids with different axes
		exc := []float64{400, 470, 550}
		emi := []float64{430, 520, 600, 680}
		table := [][]float64{
			{0.00, 0.10, 0.20, 0.05},
			{0.30, 0.00, 0.40, 0.10},
			{0.10, 0.20, 0.00, 0.30},
		}
		mat, err := NewFluorescentMaterial(albedo, zero, exc, emi, table)
		c.Check(err == nil, "NewFluorescentMaterial: %v", err)

		// kernel values at nodes: K = T * li / lo, zero for li > lo
		k := mat.Kernel(400, 520)
		c.Check(math.Abs(k-0.10*400/520) < 1e-12, "kernel at node: got %v", k)
		c.Check(mat.Kernel(550, 520) == 0, "kernel must be zero for li>lo (table entry is %v)", table[2][1])
		c.Check(mat.Kernel(550, 430) == 0, "kernel must be zero for li>lo")
		c.Check(mat.Kernel(470, 520) == 0, "kernel with zero table entry")
		c.Check(mat.Kernel(400, 700) == 0, "kernel zero outside emission grid")

		// bilinear interpolation inside a rectangle
		tm := mat.Table(475, 560)
		c.Check(math.Abs(tm-0.19375) < 1e-12, "bilinear midpoint: got %v want 0.19375", tm)
		c.Check(mat.Table(390, 560) == 0 && mat.Table(475, 690) == 0, "table must be zero outside grids")

		// constant table: closed-form integral with lo truncating a cell
		ct := [][]float64{{0.02, 0.02}, {0.02, 0.02}, {0.02, 0.02}}
		cmat, err := NewFluorescentMaterial(albedo, zero, exc, []float64{420, 660}, ct)
		c.Check(err == nil, "constant material: %v", err)
		refI := 0.02 * (500*500 - 400*400) / (2 * 500)
		gotI := cmat.ConversionIntegral(500)
		c.Check(math.Abs(gotI-refI) < 1e-9, "truncated integral: got %v want %v", gotI, refI)
		refI2 := 0.02 * (430*430 - 400*400) / (2 * 430)
		gotI2 := cmat.ConversionIntegral(430)
		c.Check(math.Abs(gotI2-refI2) < 1e-9, "truncated integral near edge: got %v want %v", gotI2, refI2)
		c.Check(cmat.ConversionIntegral(410) == 0, "integral zero below emission grid")
		c.Check(cmat.ConversionIntegral(700) == 0, "integral zero outside emission grid")
		c.Check(cmat.ConversionIntegral(400) == 0, "integral zero at excitation minimum")

		// density queries agree with kernel / integral
		badPDF := 0
		for _, li := range []float64{405, 469.5, 471, 499.9} {
			want := cmat.Kernel(li, 500) / refI
			if math.Abs(cmat.ExcitationPDF(li, 500)-want) > 1e-12 {
				badPDF++
			}
		}
		c.Check(badPDF == 0, "ExcitationPDF disagrees with Kernel/Integral (%d cases)", badPDF)
		c.Check(cmat.ExcitationPDF(510, 500) == 0, "pdf must be zero for li>lo")

		// sampled excitation wavelengths: range, density and frequencies
		N := 20000
		rnd := rand.New(rand.NewSource(5))
		p1 := (470.0*470 - 400*400) / (500.0*500 - 400*400)
		count1, outOfRange, badSamplePDF := 0, 0, 0
		for i := 0; i < N; i++ {
			li, pdf := cmat.SampleExcitation(500, rnd.Float64())
			if li < 400 || li > 500 {
				outOfRange++
			}
			if math.Abs(pdf-cmat.ExcitationPDF(li, 500)) > 1e-9 {
				badSamplePDF++
			}
			if li < 470 {
				count1++
			}
		}
		sigma := math.Sqrt(float64(N) * p1 * (1 - p1))
		freqErr := math.Abs(float64(count1) - float64(N)*p1)
		c.Check(outOfRange == 0, "sampled li outside [400, 500]: %d", outOfRange)
		c.Check(badSamplePDF == 0, "returned pdf disagrees with ExcitationPDF: %d", badSamplePDF)
		c.Check(freqErr < 5*sigma, "cell frequency off: |%d - %.1f| = %.1f >= 5*%.1f", count1, float64(N)*p1, freqErr, sigma)
		t.Logf("FLUORO_KERNEL_FREQ N=%d p1=%.6f got=%.6f err=%.1f sigma=%.1f", N, p1, float64(count1)/float64(N), freqErr, sigma)

		// invalid input must produce errors
		_, err = NewSpectrum([]float64{500}, []float64{1})
		c.Check(err != nil, "single node must fail")
		_, err = NewSpectrum([]float64{500, 400}, []float64{1, 1})
		c.Check(err != nil, "decreasing nodes must fail")
		_, err = NewSpectrum([]float64{300, 500}, []float64{1, 1})
		c.Check(err != nil, "node outside [400,700] must fail")
		_, err = NewSpectrum([]float64{400, 500}, []float64{1, -1})
		c.Check(err != nil, "negative value must fail")
		badAlbedo := fluoroTestSpectrum(c, []float64{400, 700}, []float64{1.5, 1.5})
		_, err = NewFluorescentMaterial(badAlbedo, zero, exc, emi, table)
		c.Check(err != nil, "elastic albedo > 1 must fail")
		_, err = NewFluorescentMaterial(albedo, zero, exc, emi, [][]float64{{1, 2}})
		c.Check(err != nil, "table dimension mismatch must fail")
		negTable := [][]float64{{0, 0, 0, 0}, {0, -0.1, 0, 0}, {0, 0, 0, 0}}
		_, err = NewFluorescentMaterial(albedo, zero, exc, emi, negTable)
		c.Check(err != nil, "negative table value must fail")
		_, err = NewSensor(zero, zero, zero)
		c.Check(err != nil, "all-black sensor must fail")
	})
}

func TestFluoroAdjoint(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		zero := fluoroTestZero(c)
		albedo := fluoroTestSpectrum(c, []float64{400, 700}, []float64{0.2, 0.2})
		exc := []float64{400, 470, 550}
		emi := []float64{430, 520, 600, 680}
		table := [][]float64{
			{0.00, 0.10, 0.20, 0.05},
			{0.30, 0.00, 0.40, 0.10},
			{0.10, 0.20, 0.00, 0.30},
		}
		mat, err := NewFluorescentMaterial(albedo, zero, exc, emi, table)
		c.Check(err == nil, "NewFluorescentMaterial: %v", err)

		// elastic + conversion spectral integral, cross-checked against
		// an independent two-dimensional quadrature of the kernel
		elastic := 0.0
		conversion := 0.0
		const M = 6000
		for i := 0; i < M; i++ {
			l0 := 400 + 300*float64(i)/M
			l1 := 400 + 300*float64(i+1)/M
			elastic += 0.5 * (mat.Albedo(l0) + mat.Albedo(l1)) * (l1 - l0)
			conversion += 0.5 * (mat.ConversionIntegral(l0) + mat.ConversionIntegral(l1)) * (l1 - l0)
		}
		quad := fluoroTestQuad2D(0.5, func(li, lo float64) float64 { return mat.Kernel(li, lo) })
		c.Check(math.Abs(elastic-60) < 1e-9, "elastic integral: got %v want 60", elastic)
		c.Check(math.Abs(quad-conversion)/conversion < 2e-3, "conversion integral: quad %v vs cells %v", quad, conversion)
		t.Logf("FLUORO_ADJOINT_QUAD elastic=%.6f conversion_quad=%.6f conversion_cells=%.6f relerr=%.3g",
			elastic, quad, conversion, math.Abs(quad-conversion)/conversion)

		// sensor sees only a green band, environment emits only a blue band
		g := fluoroTestSpectrum(c, []float64{400, 539, 540, 560, 561, 700}, []float64{0, 0, 1, 1, 0, 0})
		sensor, err := NewSensor(zero, g, zero)
		c.Check(err == nil, "green sensor: %v", err)
		envBlue := fluoroTestSpectrum(c, []float64{400, 439, 440, 460, 461, 700}, []float64{0, 0, 2, 2, 0, 0})
		exc2 := []float64{400, 460, 520}
		emi2 := []float64{500, 560, 620}
		tab2 := [][]float64{{0.5, 0.5, 0.5}, {0.5, 0.5, 0.5}, {0.5, 0.5, 0.5}}
		fmat, err := NewFluorescentMaterial(zero, zero, exc2, emi2, tab2)
		c.Check(err == nil, "fluorescent material: %v", err)
		scene := &Scene{}
		plane := NewPlane(V(0, 0, 0), V(0, 0, 1), DiffuseMaterial(White))
		scene.Add(plane)
		sampler, err := NewFluorescentSampler(scene, map[Shape]*FluorescentMaterial{plane: fmat}, envBlue, sensor, 1)
		c.Check(err == nil, "NewFluorescentSampler: %v", err)
		ray := Ray{V(0, 0, 1), V(0, 0, -1)}

		// reference: E[G] = integral of g(ls) * env(li) * K(li, ls) dli dls
		ref := fluoroTestQuad2D(0.5, func(li, lo float64) float64 {
			return g.Value(lo) * envBlue.Value(li) * fmat.Kernel(li, lo)
		})
		// same without the li/lo energy ratio (plain T transport)
		refNoRatio := fluoroTestQuad2D(0.5, func(li, lo float64) float64 {
			return g.Value(lo) * envBlue.Value(li) * fmat.Table(li, lo)
		})

		N := 20000
		rnd := rand.New(rand.NewSource(7))
		mean, m2, badDir := 0.0, 0.0, 0
		for i := 0; i < N; i++ {
			col := sampler.Sample(scene, ray, rnd)
			if col.R != 0 || col.B != 0 {
				badDir++
			}
			x := col.G
			d := x - mean
			mean += d / float64(i+1)
			m2 += d * (x - mean)
		}
		stderr := math.Sqrt(m2/float64(N-1)) / math.Sqrt(float64(N))
		c.Check(badDir == 0, "green-only sensor produced %d non-green samples", badDir)
		c.Check(math.Abs(mean-ref) < 5*stderr, "adjoint energy: mean %v vs ref %v (stderr %v)", mean, ref, stderr)
		c.Check(math.Abs(mean-refNoRatio) > 5*stderr, "li/lo energy ratio not distinguishable: mean %v ref-without-ratio %v", mean, refNoRatio)
		t.Logf("FLUORO_ADJOINT_ENERGY N=%d mean=%.4f ref=%.4f ref_no_ratio=%.4f stderr=%.4f", N, mean, ref, refNoRatio, stderr)
	})
}

func TestFluoroTransport(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		zero := fluoroTestZero(c)
		envFlat := fluoroTestSpectrum(c, []float64{400, 700}, []float64{1.5, 1.5})
		// three response shapes with equal integrals (150 nm each)
		senR := fluoroTestSpectrum(c, []float64{400, 700}, []float64{1, 0})
		senG := fluoroTestSpectrum(c, []float64{400, 550, 700}, []float64{0, 1, 0})
		senB := fluoroTestSpectrum(c, []float64{400, 700}, []float64{0.5, 0.5})
		sensor, err := NewSensor(senR, senG, senB)
		c.Check(err == nil, "sensor: %v", err)
		c.Check(math.Abs(sensor.Total()-450) < 1e-9, "sensor total: got %v want 450", sensor.Total())

		// (a) constant environment, empty scene, MaxBounces = 0:
		// only environment escapes are allowed
		empty := &Scene{}
		s0, err := NewFluorescentSampler(empty, map[Shape]*FluorescentMaterial{}, envFlat, sensor, 0)
		c.Check(err == nil, "empty scene sampler: %v", err)
		c.Check(s0.MaxBounces() == 0, "MaxBounces: got %v", s0.MaxBounces())
		ray := Ray{V(0, 0, 0), V(0, 0, 1)}
		_, ev := s0.SampleWithEvents(empty, ray, rand.New(rand.NewSource(1)))
		c.Check(len(ev) == 1 && ev[0].Kind == FluoroEventEscape, "empty scene events: %+v", ev)
		N := 4000
		rnd := rand.New(rand.NewSource(2))
		var mean, m2 [3]float64
		for i := 0; i < N; i++ {
			col := s0.Sample(empty, ray, rnd)
			for k, x := range []float64{col.R, col.G, col.B} {
				d := x - mean[k]
				mean[k] += d / float64(i+1)
				m2[k] += d * (x - mean[k])
			}
		}
		for k, name := range []string{"R", "G", "B"} {
			stderr := math.Sqrt(m2[k]/float64(N-1)) / math.Sqrt(float64(N))
			c.Check(math.Abs(mean[k]-225) < 5*stderr, "env %s: mean %v vs ref 225 (stderr %v)", name, mean[k], stderr)
			t.Logf("FLUORO_TRANSPORT_ENV N=%d channel=%s mean=%.4f ref=225 stderr=%.4f", N, name, mean[k], stderr)
		}

		// (b) zero conversion table degenerates to pure elastic transport
		albedoHalf := fluoroTestSpectrum(c, []float64{400, 700}, []float64{0.5, 0.5})
		zeroTable := [][]float64{{0, 0}, {0, 0}}
		elasticMat, err := NewFluorescentMaterial(albedoHalf, zero, []float64{400, 700}, []float64{400, 700}, zeroTable)
		c.Check(err == nil, "elastic material: %v", err)
		c.Check(elasticMat.ConversionIntegral(550) == 0, "zero table must give zero conversion")
		planeScene := &Scene{}
		plane := NewPlane(V(0, 0, 0), V(0, 0, 1), DiffuseMaterial(White))
		planeScene.Add(plane)
		sE, err := NewFluorescentSampler(planeScene, map[Shape]*FluorescentMaterial{plane: elasticMat}, envFlat, sensor, 8)
		c.Check(err == nil, "elastic sampler: %v", err)
		hitRay := Ray{V(0, 0, 1), V(0, 0, -1)}
		_, ev = sE.SampleWithEvents(planeScene, hitRay, rand.New(rand.NewSource(3)))
		c.Check(len(ev) == 2 && ev[0].Kind == FluoroEventElastic && ev[1].Kind == FluoroEventEscape,
			"elastic events: %+v", ev)
		c.Check(ev[0].LambdaIn == ev[0].LambdaOut, "elastic event must keep wavelength")
		c.Check(math.Abs(ev[0].Weight-0.5) < 1e-12, "elastic weight: got %v want 0.5", ev[0].Weight)
		rnd = rand.New(rand.NewSource(4))
		meanG, m2G := 0.0, 0.0
		for i := 0; i < N; i++ {
			x := sE.Sample(planeScene, hitRay, rnd).G
			d := x - meanG
			meanG += d / float64(i+1)
			m2G += d * (x - meanG)
		}
		stderrG := math.Sqrt(m2G/float64(N-1)) / math.Sqrt(float64(N))
		c.Check(math.Abs(meanG-112.5) < 5*stderrG, "elastic G: mean %v vs ref 112.5 (stderr %v)", meanG, stderrG)
		t.Logf("FLUORO_TRANSPORT_ELASTIC N=%d meanG=%.4f ref=112.5 stderr=%.4f", N, meanG, stderrG)

		// (c) two wavelength conversions in a row between parallel planes
		excC := []float64{400, 450, 500}
		emiC := []float64{400, 450, 500, 550, 600}
		tabC := [][]float64{
			{0.4, 0.4, 0.4, 0.4, 0.4},
			{0.4, 0.4, 0.4, 0.4, 0.4},
			{0.4, 0.4, 0.4, 0.4, 0.4},
		}
		convMat, err := NewFluorescentMaterial(zero, zero, excC, emiC, tabC)
		c.Check(err == nil, "conversion material: %v", err)
		sandwich := &Scene{}
		low := NewPlane(V(0, 0, -1), V(0, 0, 1), DiffuseMaterial(White))
		high := NewPlane(V(0, 0, 1), V(0, 0, -1), DiffuseMaterial(White))
		sandwich.Add(low)
		sandwich.Add(high)
		greenBand := fluoroTestSpectrum(c, []float64{400, 539, 540, 560, 561, 700}, []float64{0, 0, 1, 1, 0, 0})
		greenSensor, err := NewSensor(zero, greenBand, zero)
		c.Check(err == nil, "green sensor: %v", err)
		sC, err := NewFluorescentSampler(sandwich, map[Shape]*FluorescentMaterial{low: convMat, high: convMat}, zero, greenSensor, 3)
		c.Check(err == nil, "sandwich sampler: %v", err)
		midRay := Ray{V(0, 0, 0), V(0, 0, 1)}
		l1, ev1 := sC.Trace(midRay, 555, rand.New(rand.NewSource(99)))
		l2, ev2 := sC.Trace(midRay, 555, rand.New(rand.NewSource(99)))
		same := l1 == l2 && len(ev1) == len(ev2)
		for i := range ev1 {
			if ev1[i] != ev2[i] {
				same = false
			}
		}
		c.Check(same, "fixed seed must reproduce the path exactly")
		c.Check(len(ev1) == 4, "expected 4 events, got %+v", ev1)
		convOK := len(ev1) == 4
		weight := 1.0
		for k := 0; k < 3 && convOK; k++ {
			e := ev1[k]
			if e.Kind != FluoroEventConvert || e.LambdaOut >= e.LambdaIn {
				convOK = false
				break
			}
			if k > 0 && ev1[k].LambdaIn != ev1[k-1].LambdaOut {
				convOK = false
				break
			}
			if e.LambdaOut < 400 || e.LambdaOut > 500 {
				convOK = false
				break
			}
			weight *= convMat.ConversionIntegral(e.LambdaIn)
			if math.Abs(e.Weight-weight) > 1e-9*weight {
				convOK = false
				break
			}
		}
		convOK = convOK && ev1[3].Kind == FluoroEventTruncate
		c.Check(convOK, "two conversion chain broken: %+v", ev1)
		c.Check(l1 == 0, "sandwich without environment must give zero radiance, got %v", l1)
		t.Logf("FLUORO_TRANSPORT_CHAIN events=%v->%v->%v->%s weights=%.4g,%.4g,%.4g",
			ev1[0].LambdaIn, ev1[1].LambdaOut, ev1[2].LambdaOut, ev1[3].Kind, ev1[0].Weight, ev1[1].Weight, ev1[2].Weight)

		// (d) depth boundary: MaxBounces = 0 truncates surface hits
		sD, err := NewFluorescentSampler(planeScene, map[Shape]*FluorescentMaterial{plane: elasticMat}, envFlat, sensor, 0)
		c.Check(err == nil, "depth-0 sampler: %v", err)
		colD, evD := sD.SampleWithEvents(planeScene, hitRay, rand.New(rand.NewSource(6)))
		c.Check(colD == Black, "MaxBounces=0 with a hit must be black, got %v", colD)
		c.Check(len(evD) == 1 && evD[0].Kind == FluoroEventTruncate, "depth-0 events: %+v", evD)
		_, err = NewFluorescentSampler(planeScene, map[Shape]*FluorescentMaterial{plane: elasticMat}, envFlat, sensor, -1)
		c.Check(err != nil, "negative MaxBounces must fail")

		// (e) constructor copies all input slices and maps
		valsCopy := []float64{1.5, 1.5}
		nodesCopy := []float64{400, 700}
		sp, err := NewSpectrum(nodesCopy, valsCopy)
		c.Check(err == nil, "spectrum: %v", err)
		valsCopy[0] = 99
		nodesCopy[0] = 401
		c.Check(sp.Value(450) == 1.5, "NewSpectrum must copy its inputs")
		excE := []float64{400, 450, 500}
		emiE := []float64{400, 450, 500, 550, 600}
		tabE := [][]float64{
			{0.4, 0.4, 0.4, 0.4, 0.4},
			{0.4, 0.4, 0.4, 0.4, 0.4},
			{0.4, 0.4, 0.4, 0.4, 0.4},
		}
		albV := []float64{0.5, 0.5}
		albE, err := NewSpectrum([]float64{400, 700}, albV)
		c.Check(err == nil, "albedo: %v", err)
		senGV := []float64{0, 1, 0}
		senGE, err := NewSpectrum([]float64{400, 550, 700}, senGV)
		c.Check(err == nil, "sensor spectrum: %v", err)
		sensorE, err := NewSensor(senGE, senGE, senGE)
		c.Check(err == nil, "sensor: %v", err)
		matE, err := NewFluorescentMaterial(albE, zero, excE, emiE, tabE)
		c.Check(err == nil, "material: %v", err)
		matsE := map[Shape]*FluorescentMaterial{plane: matE}
		sCopy, err := NewFluorescentSampler(planeScene, matsE, envFlat, sensorE, 2)
		c.Check(err == nil, "copy sampler: %v", err)
		c1 := sCopy.Sample(planeScene, hitRay, rand.New(rand.NewSource(42)))
		albV[0] = 99
		senGV[1] = 99
		excE[1] = 401
		tabE[0][0] = 99
		delete(matsE, plane)
		c2 := sCopy.Sample(planeScene, hitRay, rand.New(rand.NewSource(42)))
		c.Check(c1 == c2, "sampler must be isolated from input mutation: %v vs %v", c1, c2)

		// constructor validation
		_, err = NewFluorescentSampler(nil, matsE, envFlat, sensorE, 1)
		c.Check(err != nil, "nil scene must fail")
		_, err = NewFluorescentSampler(planeScene, map[Shape]*FluorescentMaterial{}, envFlat, sensorE, 1)
		c.Check(err != nil, "missing shape binding must fail")
		wrapped := &Scene{}
		wrapped.Add(NewTransformedShape(NewSphere(V(0, 0, 0), 1, DiffuseMaterial(White)), Identity()))
		_, err = NewFluorescentSampler(wrapped, map[Shape]*FluorescentMaterial{wrapped.Shapes[0]: matE}, envFlat, sensorE, 1)
		c.Check(err != nil, "unsupported geometry must fail")
		texScene := &Scene{}
		texMat := DiffuseMaterial(White)
		texMat.Texture = &ColorTexture{1, 1, []Color{White}}
		texSphere := NewSphere(V(0, 0, 0), 1, texMat)
		texScene.Add(texSphere)
		_, err = NewFluorescentSampler(texScene, map[Shape]*FluorescentMaterial{texSphere: matE}, envFlat, sensorE, 1)
		c.Check(err != nil, "textured shape must fail")

		// (f) full renderer integration: blue environment, fluorescent
		// plane, green-only sensor
		envBlue := fluoroTestSpectrum(c, []float64{400, 439, 440, 460, 461, 700}, []float64{0, 0, 2, 2, 0, 0})
		excF := []float64{400, 460, 520}
		emiF := []float64{500, 560, 620}
		tabF := [][]float64{{0.5, 0.5, 0.5}, {0.5, 0.5, 0.5}, {0.5, 0.5, 0.5}}
		fmat, err := NewFluorescentMaterial(zero, zero, excF, emiF, tabF)
		c.Check(err == nil, "render material: %v", err)
		renderScene := &Scene{}
		floor := NewPlane(V(0, 0, 0), V(0, 0, 1), DiffuseMaterial(White))
		renderScene.Add(floor)
		greenOnly, err := NewSensor(zero, greenBand, zero)
		c.Check(err == nil, "green-only sensor: %v", err)
		sR, err := NewFluorescentSampler(renderScene, map[Shape]*FluorescentMaterial{floor: fmat}, envBlue, greenOnly, 1)
		c.Check(err == nil, "render sampler: %v", err)
		camera := LookAt(V(0, 0, 3), V(0, 0, 0), V(0, 1, 0), 45)
		renderer := NewRenderer(renderScene, &camera, sR, 16, 16)
		renderer.SamplesPerPixel = 8
		renderer.NumCPU = 1
		renderer.Verbose = false
		img := renderer.Render()
		ref := fluoroTestQuad2D(0.5, func(li, lo float64) float64 {
			return greenBand.Value(lo) * envBlue.Value(li) * fmat.Kernel(li, lo)
		})
		nPix := 0
		meanPix, m2Pix, sumR, sumB := 0.0, 0.0, 0.0, 0.0
		for y := 0; y < 16; y++ {
			for x := 0; x < 16; x++ {
				px := renderer.Buffer.Color(x, y)
				sumR += px.R
				sumB += px.B
				d := px.G - meanPix
				nPix++
				meanPix += d / float64(nPix)
				m2Pix += d * (px.G - meanPix)
			}
		}
		stderrPix := math.Sqrt(m2Pix/float64(nPix-1)) / math.Sqrt(float64(nPix))
		nonBlack := 0
		for y := 0; y < 16; y++ {
			for x := 0; x < 16; x++ {
				r, g, b, _ := img.At(x, y).RGBA()
				if r|g|b != 0 {
					nonBlack++
				}
			}
		}
		c.Check(sumR == 0 && sumB == 0, "green-only render leaked into R=%v B=%v", sumR, sumB)
		c.Check(meanPix > 0, "rendered image is black")
		c.Check(nonBlack > 100, "too few non-black pixels: %d", nonBlack)
		c.Check(math.Abs(meanPix-ref) < 5*stderrPix, "render mean %v vs ref %v (stderr %v)", meanPix, ref, stderrPix)
		t.Logf("FLUORO_TRANSPORT_RENDER pixels=%d samples=%d meanG=%.4f ref=%.4f stderr=%.4f nonblack=%d",
			nPix, nPix*8, meanPix, ref, stderrPix, nonBlack)
	})
}
