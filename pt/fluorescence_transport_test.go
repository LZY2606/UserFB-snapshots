package pt

// Verification for the spectral fluorescence transport. The three groups
// are wrapped in taskcheck.Run; every content assertion goes through
// c.Check. Reference values come from independent quadrature or closed-form
// expressions written in this file, never from the production sampler.

import (
	"math"
	"math/rand"
	"testing"

	"github.com/fogleman/pt/task_support/taskcheck"
)

// ---------------------------------------------------------------------------
// small independent numerical helpers (test-only reference code)
// ---------------------------------------------------------------------------

// refTrapezoid integrates f over [a, b] with n uniform subdivisions.
func refTrapezoid(a, b float64, n int, f func(float64) float64) float64 {
	h := (b - a) / float64(n)
	sum := 0.5 * (f(a) + f(b))
	for i := 1; i < n; i++ {
		sum += f(a + float64(i)*h)
	}
	return sum * h
}

// refMean returns the sample mean and standard error of the mean.
func refMean(xs []float64) (mean, se float64) {
	for _, x := range xs {
		mean += x
	}
	mean /= float64(len(xs))
	var variance float64
	for _, x := range xs {
		variance += (x - mean) * (x - mean)
	}
	variance /= float64(len(xs) - 1)
	se = math.Sqrt(variance / float64(len(xs)))
	return
}

func flatSpectrum(a, b, v float64) Spectrum {
	return Spectrum{[]float64{a, b}, []float64{v, v}}
}

func constTable(exc, em []float64, c float64) *FluorescenceTable {
	data := make([][]float64, len(exc))
	for i := range data {
		data[i] = make([]float64, len(em))
		for j := range data[i] {
			data[i][j] = c
		}
	}
	return &FluorescenceTable{
		Excitation: Spectrum{exc, make([]float64, len(exc))},
		Emission:   Spectrum{em, make([]float64, len(em))},
		Data:       data,
	}
}

// ---------------------------------------------------------------------------
// TestFluoroKernel: fixed-lo kernel values, exact segmented integrals and
// inverse-CDF sampling on two differently spaced axes.
// ---------------------------------------------------------------------------

func TestFluoroKernel(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// deliberately different grids on the two axes
		exc := []float64{400, 440, 480, 520}
		em := []float64{430, 470, 510, 550, 600}
		const tc = 0.002
		tab := constTable(exc, em, tc)

		// density: bilinear constant table gives K = c*li/lo inside the
		// rectangle, zero for li > lo and zero outside
		c.Check(near(tab.Kernel(420, 500), tc*420/500, 1e-15),
			"kernel interior value got %g want %g", tab.Kernel(420, 500), tc*420/500)
		c.Check(tab.Kernel(520, 470) == 0, "kernel must vanish for li > lo, got %g", tab.Kernel(520, 470))
		c.Check(tab.Kernel(390, 500) == 0, "kernel zero below excitation grid, got %g", tab.Kernel(390, 500))
		c.Check(tab.Kernel(420, 620) == 0, "kernel zero above emission grid, got %g", tab.Kernel(420, 620))

		// closed form for a constant table truncated at u = min(lo, max exc):
		// I(lo) = c/(2 lo) (u^2 - a^2)
		closedForm := func(lo float64) float64 {
			if lo < em[0] || lo <= exc[0] {
				return 0
			}
			u := math.Min(lo, exc[len(exc)-1])
			return tc / (2 * lo) * (u*u - exc[0]*exc[0])
		}
		for _, lo := range []float64{430, 460, 500, 510, 550, 600} {
			got := sliceKernel(tab, lo).integral()
			want := closedForm(lo)
			// independent fine trapezoid over the truncated excitation range
			ref := 0.0
			if lo > exc[0] && lo >= em[0] {
				u := math.Min(lo, exc[len(exc)-1])
				ref = refTrapezoid(exc[0], u, 4000, func(li float64) float64 {
					if li > lo {
						return 0
					}
					return tab.Eval(li, lo) * li / lo
				})
			}
			c.Check(near(got, want, 1e-10*math.Max(1, want)),
				"analytic I(%g)=%g differs from closed form %g", lo, got, want)
			c.Check(near(got, ref, 1e-4*math.Max(1, ref)),
				"analytic I(%g)=%g differs from quadrature %g", lo, got, ref)
		}

		// truncation landing inside an excitation cell: lo = 460 splits [440,480]
		k := sliceKernel(tab, 460)
		c.Check(len(k.segments) == 2, "expected 2 truncated segments, got %d", len(k.segments))
		c.Check(near(k.segments[1].b, 460, 1e-12), "last segment must end at lo, got %g", k.segments[1].b)

		// non-constant, li-linear table: T(li, lo) = 1e-5 (li - 400)
		lin := make([][]float64, len(exc))
		for i, x := range exc {
			lin[i] = make([]float64, len(em))
			for j := range em {
				lin[i][j] = 1e-5 * (x - 400)
			}
		}
		linTab := &FluorescenceTable{
			Excitation: Spectrum{exc, make([]float64, len(exc))},
			Emission:   Spectrum{em, make([]float64, len(em))},
			Data:       lin,
		}
		const loLin = 500
		refLin := refTrapezoid(400, 500, 4000, func(li float64) float64 {
			return linTab.Eval(li, loLin) * li / loLin
		})
		gotLin := sliceKernel(linTab, loLin).integral()
		c.Check(near(gotLin, refLin, 1e-4*refLin),
			"linear-table integral %g vs quadrature %g", gotLin, refLin)

		// sampling: frequencies and the sample/PDF contract
		const nSamples = 40000
		const loSample = 500.0
		rnd := rand.New(rand.NewSource(101))
		ks := sliceKernel(tab, loSample)
		total := ks.integral()
		bins := 8
		counts := make([]int, bins)
		lo := 400.0
		hi := math.Min(loSample, exc[len(exc)-1])
		sumLi := 0.0
		for i := 0; i < nSamples; i++ {
			u := rnd.Float64()
			li := ks.sample(u)
			pdf := ks.pdf(li) / total
			c.Check(li >= 400-spectralTol && li <= loSample+spectralTol,
				"sampled li %g outside [400, lo=%g]", li, loSample)
			c.Check(near(pdf, tab.Kernel(li, loSample)/total, 1e-10*math.Max(1, pdf)),
				"PDF %g != K/I %g at li=%g", pdf, tab.Kernel(li, loSample)/total, li)
			sumLi += li
			b := int((li - lo) / (hi - lo) * float64(bins))
			if b >= bins {
				b = bins - 1
			}
			counts[b]++
		}
		// analytic mean for constant T on [a, u]:
		// E[li] = 2/3 (u^3-a^3)/(u^2-a^2)
		wantMean := 2.0 / 3 * (hi*hi*hi - lo*lo*lo) / (hi*hi - lo*lo)
		gotMean := sumLi / nSamples
		c.Check(math.Abs(gotMean-wantMean) < 1.0,
			"sampled mean li %g vs analytic %g", gotMean, wantMean)
		for b, cnt := range counts {
			a := lo + float64(b)*(hi-lo)/float64(bins)
			bb := lo + float64(b+1)*(hi-lo)/float64(bins)
			binI := refTrapezoid(a, bb, 200, func(li float64) float64 { return tab.Kernel(li, loSample) })
			expected := float64(nSamples) * binI / total
			c.Check(math.Abs(float64(cnt)-expected) < 0.1*expected,
				"bin %d count %d vs expected %.1f", b, cnt, expected)
		}
		t.Logf("KERNEL n=%d mean_li=%.4f analytic_mean=%.4f I(500)=%.8f",
			nSamples, gotMean, wantMean, total)
	})
}

// ---------------------------------------------------------------------------
// TestFluoroAdjoint: independent 2D quadrature of elastic + conversion
// spectral integrals, then a blue-light / green-sensor direction and energy
// check.
// ---------------------------------------------------------------------------

func TestFluoroAdjoint(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		exc := []float64{400, 450, 500, 550}
		em := []float64{420, 480, 540, 600, 660}
		tab := constTable(exc, em, 0.003)
		rho := Spectrum{[]float64{400, 500, 600, 700}, []float64{0.2, 0.4, 0.3, 0.1}}

		// independent 2D trapezoid of K over the rectangle with li <= lo
		const n = 300
		independent := 0.0
		for i := 0; i < n; i++ {
			li := exc[0] + (float64(i)+0.5)*(exc[len(exc)-1]-exc[0])/n
			rowMass := 0.0
			for j := 0; j < n; j++ {
				loV := em[0] + (float64(j)+0.5)*(em[len(em)-1]-em[0])/n
				if li <= loV {
					rowMass += tab.Eval(li, loV) * li / loV
				}
			}
			independent += rowMass / n * (em[len(em)-1] - em[0])
		}
		independent /= n
		independent *= exc[len(exc)-1] - exc[0]

		// production side: integrate the exact segmented I(lo) over lo
		analytic := refTrapezoid(em[0], em[len(em)-1], 400, func(loV float64) float64 {
			return sliceKernel(tab, loV).integral()
		})
		c.Check(near(analytic, independent, 5e-3*independent),
			"2D kernel mass analytic=%.6f vs independent quadrature=%.6f", analytic, independent)

		// elastic albedo spectral integral, independently re-integrated
		rhoRef := refTrapezoid(400, 700, 4000, func(x float64) float64 { return rho.Eval(x) })
		c.Check(near(rho.Integral(), rhoRef, 1e-4*rhoRef),
			"albedo integral %g vs quadrature %g", rho.Integral(), rhoRef)
		// combined elastic + conversion spectral budget on the emission grid
		combinedRef := rhoRef + independent
		combinedGot := rho.Integral() + analytic
		c.Check(near(combinedGot, combinedRef, 5e-3*combinedRef),
			"combined spectral integral %g vs %g", combinedGot, combinedRef)

		// blue-only environment light, green-only sensor, fluorescent sphere
		scene := &Scene{}
		sphere := NewSphere(V(0, 0, 0), 1, DiffuseMaterial(Black))
		scene.Add(sphere)
		scene.Compile()
		blueEnv := Spectrum{[]float64{400, 450, 500}, []float64{1, 1, 1}}
		// excitation reaches at most 500 nm, exactly the blue environment band
		ftab := constTable([]float64{400, 450, 500}, []float64{500, 550, 600}, 0.004)
		mats := map[Shape]SpectralMaterial{
			sphere: {
				Emission: flatSpectrum(400, 700, 0),
				Albedo:   flatSpectrum(400, 700, 0),
				Table:    ftab,
			},
		}
		zero := flatSpectrum(400, 700, 0)
		green := Spectrum{[]float64{500, 550, 600}, []float64{1, 1, 1}}
		sensor := Sensor{[3]Spectrum{zero, green, zero}}
		sampler, err := NewFluorescentSampler(scene, mats, blueEnv, sensor, 4)
		c.Check(err == nil, "sampler construction: %v", err)

		// direction: every conversion must shorten the wavelength, and the
		// walk then escapes exactly once into the blue environment
		var events []FluoroEvent
		L := sampler.Transport(scene, Ray{V(0, 0, 4), V(0, 0, -1)}, 550,
			rand.New(rand.NewSource(7)), &events)
		c.Check(len(events) == 2, "want fluorescence + environment events, got %d: %+v", len(events), events)
		if len(events) == 2 {
			c.Check(events[0].Branch == "fluorescence", "first event %s", events[0].Branch)
			c.Check(events[0].WavelengthOut < events[0].WavelengthIn,
				"conversion %g -> %g must go to shorter wavelength",
				events[0].WavelengthIn, events[0].WavelengthOut)
			c.Check(events[1].Branch == "environment", "second event %s", events[1].Branch)
			c.Check(events[1].Emission == 1, "blue environment radiance %g", events[1].Emission)
		}
		c.Check(L > 0, "transported radiance should be positive, got %g", L)

		// energy: E[G] = integral over lo of I(lo), with p uniform on [500,600]
		const nMc = 4000
		gs := make([]float64, nMc)
		rs := make([]float64, nMc)
		bs := make([]float64, nMc)
		rnd := rand.New(rand.NewSource(202))
		for i := range gs {
			col := sampler.Sample(scene, Ray{V(0, 0, 4), V(0, 0, -1)}, rnd)
			rs[i], gs[i], bs[i] = col.R, col.G, col.B
		}
		gMean, gSE := refMean(gs)
		rMean, _ := refMean(rs)
		bMean, _ := refMean(bs)
		// closed-form reference: c/2 * (500^2-400^2) * ln(600/500)
		refG := 0.004 / 2 * (500*500 - 400*400) * math.Log(600.0/500.0)
		c.Check(math.Abs(gMean-refG) < 4*gSE,
			"green mean %.5f vs reference %.5f (SE %.5f)", gMean, refG, gSE)
		c.Check(rMean < 4*gSE && bMean < 4*gSE,
			"red/blue must stay dark, got R=%.5f B=%.5f", rMean, bMean)

		// doubling T must double the signal (linear energy ratio)
		doubleMats := map[Shape]SpectralMaterial{
			sphere: {
				Emission: flatSpectrum(400, 700, 0),
				Albedo:   flatSpectrum(400, 700, 0),
				Table:    constTable([]float64{400, 450, 500}, []float64{500, 550, 600}, 0.008),
			},
		}
		doubleSampler, err := NewFluorescentSampler(scene, doubleMats, blueEnv, sensor, 4)
		c.Check(err == nil, "double-T sampler: %v", err)
		dgs := make([]float64, nMc)
		rnd2 := rand.New(rand.NewSource(202))
		for i := range dgs {
			dgs[i] = doubleSampler.Sample(scene, Ray{V(0, 0, 4), V(0, 0, -1)}, rnd2).G
		}
		dMean, dSE := refMean(dgs)
		ratio := dMean / gMean
		c.Check(math.Abs(ratio-2) < 0.05,
			"doubling T gave energy ratio %.4f, want 2 (SEs %.5f/%.5f)", ratio, gSE, dSE)
		t.Logf("ADJOINT n=%d green=%.5f+/-%.5f reference=%.5f ratio=%.4f 2D_mass=%.6f vs %.6f",
			nMc, gMean, gSE, refG, ratio, analytic, independent)
	})
}

// ---------------------------------------------------------------------------
// TestFluoroTransport: real Scene.Intersect walks through the sampler.
// ---------------------------------------------------------------------------

func TestFluoroTransport(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const envValue = 0.6
		env := flatSpectrum(400, 700, envValue)

		// floor plane: one guaranteed surface event then escape
		floor := NewPlane(V(0, 0, 0), V(0, 0, 1), DiffuseMaterial(Black))
		scene := &Scene{}
		scene.Add(floor)
		scene.Compile()
		mats := map[Shape]SpectralMaterial{
			floor: {
				Emission: flatSpectrum(400, 700, 0),
				Albedo:   flatSpectrum(400, 700, 0.7),
			},
		}
		sensor := Sensor{[3]Spectrum{
			flatSpectrum(400, 700, 1), flatSpectrum(400, 700, 1), flatSpectrum(400, 700, 1),
		}}
		sampler, err := NewFluorescentSampler(scene, mats, env, sensor, 5)
		c.Check(err == nil, "construction: %v", err)

		// degenerate zero-conversion path: L = rho * env exactly
		ray := Ray{V(0, 0, 1), V(0, 0, -1)}
		for _, seed := range []int64{1, 2, 3} {
			L := sampler.Transport(scene, ray, 550, rand.New(rand.NewSource(seed)), nil)
			c.Check(near(L, 0.7*envValue, 1e-12),
				"elastic-only transport L=%g want %g", L, 0.7*envValue)
		}

		// depth boundary: MaxBounces=0 processes no surface event
		zeroBounce, err := NewFluorescentSampler(scene, mats, env, sensor, 0)
		c.Check(err == nil, "zero-bounce construction: %v", err)
		var blocked []FluoroEvent
		L0 := zeroBounce.Transport(scene, ray, 550, rand.New(rand.NewSource(4)), &blocked)
		c.Check(L0 == 0, "blocked ray at MaxBounces=0 must return 0, got %g", L0)
		c.Check(len(blocked) == 1 && blocked[0].Branch == "depth",
			"expected one depth record, got %+v", blocked)
		// but an escaping ray still collects the environment
		escapeRay := Ray{V(0, 0, 1), V(0, 0, 1)}
		Le := zeroBounce.Transport(scene, escapeRay, 470, rand.New(rand.NewSource(5)), nil)
		c.Check(near(Le, envValue, 1e-12), "environment escape L=%g want %g", Le, envValue)

		// two material events with two wavelength changes inside a slab of
		// infinite planes: every cosine ray is guaranteed to reach the
		// opposite plane, and MaxBounces=2 ends the walk right after event 2
		ceiling := NewPlane(V(0, 0, 2), V(0, 0, -1), DiffuseMaterial(Black))
		slab := &Scene{}
		slab.Add(floor)
		slab.Add(ceiling)
		slab.Compile()
		f1 := constTable([]float64{400, 450, 500}, []float64{520, 570, 620}, 0.005)
		f2 := constTable([]float64{400, 430, 460}, []float64{400, 480, 550}, 0.005)
		slabMats := map[Shape]SpectralMaterial{
			floor: {
				Emission: flatSpectrum(400, 700, 0),
				Albedo:   flatSpectrum(400, 700, 0),
				Table:    f1,
			},
			ceiling: {
				Emission: flatSpectrum(400, 700, 0),
				Albedo:   flatSpectrum(400, 700, 0),
				Table:    f2,
			},
		}
		twoStep, err := NewFluorescentSampler(slab, slabMats, env, sensor, 2)
		c.Check(err == nil, "slab construction: %v", err)
		const slabRuns = 200
		for i := 0; i < slabRuns; i++ {
			var events []FluoroEvent
			twoStep.Transport(slab, Ray{V(0, 0, 1), V(0, 0, -1)}, 590,
				rand.New(rand.NewSource(int64(300+i))), &events)
			c.Check(len(events) == 3, "run %d: want 2 fluorescence + depth record, got %+v", i, events)
			if len(events) != 3 {
				continue
			}
			c.Check(events[0].Branch == "fluorescence" && events[1].Branch == "fluorescence",
				"run %d branches %s,%s", i, events[0].Branch, events[1].Branch)
			c.Check(events[2].Branch == "depth", "run %d final branch %s", i, events[2].Branch)
			c.Check(events[1].WavelengthOut < events[0].WavelengthOut &&
				events[0].WavelengthOut < events[0].WavelengthIn,
				"run %d wavelengths must strictly shorten: %g -> %g -> %g", i,
				events[0].WavelengthIn, events[0].WavelengthOut, events[1].WavelengthOut)
			c.Check(events[1].Throughput > 0 && events[1].Throughput <= events[0].Throughput+1e-12,
				"run %d cumulative throughput not monotone: %+v", i, events)
		}

		// input copies: mutating the caller's slices and map after
		// construction must not change fixed-seed results
		before := sampler.Transport(scene, ray, 550, rand.New(rand.NewSource(99)), nil)
		mats[floor] = SpectralMaterial{}
		env.Values[0] = 999
		sensor.Responses[0].Values[0] = 999
		after := sampler.Transport(scene, ray, 550, rand.New(rand.NewSource(99)), nil)
		c.Check(near(before, after, 1e-15), "construction must copy inputs: %g vs %g", before, after)
		c.Check(sampler.KernelIntegral(floor, 550) == 0, "floor has no table, I must be 0")

		// fixed-seed reproducibility, including event traces
		var evA, evB []FluoroEvent
		la := twoStep.Transport(slab, Ray{V(0, 0, 1), V(0, 0, -1)}, 590, rand.New(rand.NewSource(55)), &evA)
		lb := twoStep.Transport(slab, Ray{V(0, 0, 1), V(0, 0, -1)}, 590, rand.New(rand.NewSource(55)), &evB)
		c.Check(la == lb && len(evA) == len(evB), "fixed seed must reproduce scalar L")
		sameTrace := true
		for i := range evA {
			if evA[i] != evB[i] {
				sameTrace = false
			}
		}
		c.Check(sameTrace, "fixed seed must reproduce event trace")

		// construction errors
		badSensor := Sensor{[3]Spectrum{
			flatSpectrum(400, 700, 0), flatSpectrum(400, 700, 0), flatSpectrum(400, 700, 0),
		}}
		_, err = NewFluorescentSampler(scene, mats, env, badSensor, 1)
		c.Check(err != nil, "all-black sensor must be rejected")
		badNodes := Spectrum{[]float64{400, 400}, []float64{1, 1}}
		_, err = NewFluorescentSampler(scene, map[Shape]SpectralMaterial{
			floor: {Emission: badNodes, Albedo: flatSpectrum(400, 700, 0.5)},
		}, env, sensor, 1)
		c.Check(err != nil, "non-strict nodes must be rejected")
		badAlbedo := Spectrum{[]float64{400, 700}, []float64{1.2, 1.2}}
		_, err = NewFluorescentSampler(scene, map[Shape]SpectralMaterial{
			floor: {Emission: flatSpectrum(400, 700, 0), Albedo: badAlbedo},
		}, env, sensor, 1)
		c.Check(err != nil, "albedo > 1 must be rejected")
		_, err = NewFluorescentSampler(scene, map[Shape]SpectralMaterial{}, env, sensor, 1)
		c.Check(err != nil, "missing shape binding must be rejected")
		boxScene := &Scene{}
		boxScene.Add(NewCube(V(-1, -1, -1), V(1, 1, 1), DiffuseMaterial(Black)))
		boxScene.Compile()
		_, err = NewFluorescentSampler(boxScene,
			map[Shape]SpectralMaterial{boxScene.Shapes[0]: {
				Emission: flatSpectrum(400, 700, 0), Albedo: flatSpectrum(400, 700, 0.5),
			}}, env, sensor, 1)
		c.Check(err != nil, "unsupported geometry must be rejected")

		// end-to-end: hand the sampler to a real Renderer and compare the
		// small image against an independent Monte Carlo reference
		renderScene := &Scene{}
		ball := NewSphere(V(0, 0, 1.2), 1, DiffuseMaterial(Black))
		renderScene.Add(floor)
		renderScene.Add(ball)
		renderScene.Compile()
		greenSensor := Sensor{[3]Spectrum{
			flatSpectrum(400, 700, 0),
			Spectrum{[]float64{500, 550, 600}, []float64{1, 1, 1}},
			flatSpectrum(400, 700, 0),
		}}
		renderMats := map[Shape]SpectralMaterial{
			floor: {
				Emission: flatSpectrum(400, 700, 0),
				Albedo:   flatSpectrum(400, 700, 0.6),
			},
			ball: {
				Emission: flatSpectrum(400, 700, 0),
				Albedo:   flatSpectrum(400, 700, 0),
				Table:    constTable([]float64{400, 450, 500}, []float64{500, 550, 600}, 0.004),
			},
		}
		renderSampler, err := NewFluorescentSampler(renderScene, renderMats, blueEnvForRender(), greenSensor, 4)
		c.Check(err == nil, "render sampler: %v", err)
		camera := LookAt(V(0, -4, 1.5), V(0, 0, 1), V(0, 0, 1), 45)
		const W, H, spp = 8, 8, 64
		renderer := NewRenderer(renderScene, &camera, renderSampler, W, H)
		renderer.SamplesPerPixel = spp
		renderer.NumCPU = 1
		renderer.Verbose = false
		img := renderer.Render()
		bounds := img.Bounds()
		var imgSum, imgMax float64
		for y := bounds.Min.Y; y < bounds.Max.Y; y++ {
			for x := bounds.Min.X; x < bounds.Max.X; x++ {
				// linear-space values straight from the accumulation buffer
				col := renderer.Buffer.Color(x, y)
				imgSum += col.G
				if col.G > imgMax {
					imgMax = col.G
				}
				c.Check(col.R == 0 && col.B == 0, "off-channel pixel (%d,%d) = %+v", x, y, col)
			}
		}
		imgMean := imgSum / float64(W*H)
		c.Check(imgMax > 0, "rendered image must not be black")

		// independent reference: Sample() on the same camera rays with a
		// separate random stream and a larger budget
		const refN = 3000
		refs := make([]float64, refN)
		refRnd := rand.New(rand.NewSource(909))
		for i := 0; i < refN; i++ {
			x := refRnd.Intn(W)
			y := refRnd.Intn(H)
			r := camera.CastRay(x, y, W, H, refRnd.Float64(), refRnd.Float64(), refRnd)
			refs[i] = renderSampler.Sample(renderScene, r, refRnd).G
		}
		refMean, refSE := refMean(refs)
		c.Check(math.Abs(imgMean-refMean) < 5*math.Max(refSE, 1e-9),
			"image mean %.5f vs MC reference %.5f", imgMean, refMean)
		t.Logf("TRANSPORT image %dx%d spp=%d mean_g=%.5f reference_n=%d reference=%.5f se=%.5f",
			W, H, spp, imgMean, refN, refMean, refSE)
	})
}

func blueEnvForRender() Spectrum {
	return Spectrum{[]float64{400, 450, 500}, []float64{1, 1, 1}}
}

// TestFluoroSensorEdge covers disjoint response bands and exact CDF
// inversion at the u=0 and u=1 endpoints, where tail cells have zero mass.
func TestFluoroSensorEdge(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		scene := &Scene{}
		floor := NewPlane(V(0, 0, 0), V(0, 0, 1), DiffuseMaterial(Black))
		scene.Add(floor)
		scene.Compile()
		mats := map[Shape]SpectralMaterial{
			floor: {
				Emission: flatSpectrum(400, 700, 0),
				Albedo:   flatSpectrum(400, 700, 0.5),
			},
		}
		// disjoint blue and red bands with an empty gap
		blue := Spectrum{[]float64{400, 450, 500}, []float64{1, 1, 1}}
		red := Spectrum{[]float64{600, 650, 700}, []float64{1, 1, 1}}
		zero := flatSpectrum(400, 700, 0)
		sensor := Sensor{[3]Spectrum{red, zero, blue}}
		sampler, err := NewFluorescentSampler(scene, mats, flatSpectrum(400, 700, 1), sensor, 2)
		c.Check(err == nil, "disjoint-band construction: %v", err)

		for _, u := range []float64{0, 1, 0.5} {
			ls := sampler.sampleSensorWavelength(u)
			c.Check(!math.IsNaN(ls) && ls >= SpectralMin && ls <= SpectralMax,
				"u=%g produced invalid sensor wavelength %g", u, ls)
		}
		// endpoints of the normalized CDF land on the outer grid nodes
		c.Check(near(sampler.sampleSensorWavelength(0), 400, 1e-9), "CDF start must be 400 nm")
		c.Check(near(sampler.sampleSensorWavelength(1), 700, 1e-9), "CDF end must be 700 nm")
		// the gap itself has zero density
		c.Check(sampler.SensorPDF(550) == 0, "density must vanish in the response gap")

		// Monte Carlo estimate of the band means stays inside the bands
		rnd := rand.New(rand.NewSource(11))
		for i := 0; i < 2000; i++ {
			ls := sampler.sampleSensorWavelength(rnd.Float64())
			inBand := (ls >= 400-spectralTol && ls <= 500+spectralTol) ||
				(ls >= 600-spectralTol && ls <= 700+spectralTol)
			c.Check(inBand, "wavelength %g sampled outside both response bands", ls)
		}
	})
}

func near(a, b, tol float64) bool {
	return math.Abs(a-b) <= tol
}
