package render_test

import (
	"math"
	"math/rand"
	"testing"

	"github.com/hunterloftis/pbr/pkg/bsdf"
	"github.com/hunterloftis/pbr/pkg/camera"
	"github.com/hunterloftis/pbr/pkg/geom"
	"github.com/hunterloftis/pbr/pkg/material"
	"github.com/hunterloftis/pbr/pkg/render"
	"github.com/hunterloftis/pbr/pkg/rgb"
	"github.com/hunterloftis/pbr/pkg/surface"
	"github.com/hunterloftis/pbr/task_support/taskcheck"
)

// testMetal is chosen so that 1/m and 1/(1-m) have no small-integer
// commensurations: recovered (metal, non-metal) choice counts are unique.
const testMetal = 0.37

const testEmission = 20.0

// flatEnv is a constant environment (pkg/env is unavailable in this build).
type flatEnv struct{ e rgb.Energy }

func (f flatEnv) At(geom.Dir) rgb.Energy { return f.e }

func testEnv() flatEnv { return flatEnv{rgb.Energy{X: 2, Y: 3, Z: 4}} }

func metalMaterial() *material.Uniform {
	return &material.Uniform{
		Color:       rgb.Energy{X: 0.7, Y: 0.5, Z: 0.3},
		Metalness:   testMetal,
		Roughness:   0.4,
		Specularity: 0.05,
	}
}

func lightMaterial() *material.Uniform {
	return &material.Uniform{
		Color:    rgb.Energy{X: 1, Y: 0.8, Z: 0.6},
		Emission: testEmission,
	}
}

// sharedScene builds a sphere over a floor, both sharing mat, plus an
// optional emissive sphere light.
func sharedScene(mat, lightMat *material.Uniform) (*render.Scene, *geom.Ray) {
	sphere := surface.UnitSphere(mat).Shift(geom.Vec{Y: 0.2})
	floor := surface.UnitCube(mat).Shift(geom.Vec{Y: -0.55}).Scale(geom.Vec{X: 20, Y: 0.1, Z: 20})
	surfs := []render.Surface{sphere, floor}
	if lightMat != nil {
		light := surface.UnitSphere(lightMat).Shift(geom.Vec{Y: 5}).Scale(geom.Vec{X: 6, Y: 6, Z: 6})
		surfs = append(surfs, light)
	}
	dir, _ := geom.Vec{X: -1.5, Y: -1, Z: 0}.Unit()
	ray := geom.NewRay(geom.Vec{X: 1.5, Y: 1.2}, dir)
	return render.NewScene(nil, surface.NewTree(surfs...), testEnv()), ray
}

func trace(t *testing.T, scene *render.Scene, ray *geom.Ray, seed int64, depth int, direct bool, params []render.GradientParam) (rgb.Energy, []rgb.Energy) {
	t.Helper()
	e, g, err := render.TraceGradient(scene, ray, rand.New(rand.NewSource(seed)), depth, direct, params)
	if err != nil {
		t.Fatalf("TraceGradient: %v", err)
	}
	return e, g
}

// decompose recovers integers a, n with s = a/m - (n-a)/(1-m), n <= nmax:
// a metal choices and n-a non-metal choices produce score s.
func decompose(s, m float64, nmax int) (a, n int, ok bool) {
	for n = 0; n <= nmax; n++ {
		af := m * (s*(1-m) + float64(n))
		ai := int(math.Round(af))
		if ai < 0 || ai > n || math.Abs(af-float64(ai)) > 1e-6 {
			continue
		}
		recon := float64(ai)/m - float64(n-ai)/(1-m)
		if math.Abs(recon-s) > 1e-9*math.Max(1, math.Abs(s)) {
			continue
		}
		return ai, n, true
	}
	return 0, 0, false
}

// decomposeScore recovers the choice counts behind a per-iteration gradient
// increment dG = dE * s. All nonzero components of dE must agree.
func decomposeScore(dE, dG rgb.Energy, m float64, nmax int) (a, n int, ok bool) {
	found := false
	es := []float64{dE.X, dE.Y, dE.Z}
	gs := []float64{dG.X, dG.Y, dG.Z}
	for i := range es {
		if math.Abs(es[i]) < 1e-9 {
			if math.Abs(gs[i]) > 1e-9 {
				return 0, 0, false // gradient without contribution
			}
			continue
		}
		ai, ni, ok := decompose(gs[i]/es[i], m, nmax)
		if !ok {
			return 0, 0, false
		}
		if !found {
			a, n, found = ai, ni, true
		} else if a != ai || n != ni {
			return 0, 0, false
		}
	}
	return a, n, found
}

// emitExplicit checks dG == dE / emission componentwise (the explicit
// emission derivative of a contribution involving the tracked light).
func emitExplicit(dE, dG rgb.Energy, emission float64) bool {
	es := []float64{dE.X, dE.Y, dE.Z}
	gs := []float64{dG.X, dG.Y, dG.Z}
	for i := range es {
		if math.Abs(es[i]) < 1e-9 {
			if gs[i] != 0 {
				return false
			}
			continue
		}
		if math.Abs(gs[i]*emission-es[i]) > 1e-9*math.Max(1, math.Abs(es[i])) {
			return false
		}
	}
	return true
}

// TestMaterialScore checks the score-function plumbing path by path with
// the production integrator: single choices, repeated choices of a shared
// parameter, prefix scores for contributions made before later choices,
// the explicit emission term, and environment termination.
func TestMaterialScore(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// --- S1: one sphere, environment only. Every path makes exactly
		// one metalness choice, then escapes or is killed.
		shared1 := metalMaterial()
		params1 := []render.GradientParam{material.Metalness(shared1)}
		scene1 := render.NewScene(nil, surface.NewTree(surface.UnitSphere(shared1)), testEnv())
		ray1 := geom.NewRay(geom.Vec{Z: 2}, geom.Dir{Z: -1})

		sawMetal, sawNonMetal := false, false
		for seed := int64(0); seed < 200; seed++ {
			var e [5]rgb.Energy
			var g [5][]rgb.Energy
			for d := 1; d <= 4; d++ {
				e[d], g[d] = trace(t, scene1, ray1, seed, d, false, params1)
			}
			// Depth 1 ends before any contribution: exactly zero, no fabricated data.
			c.Check(e[1] == rgb.Black, "depth-1 beauty = %v, want zero", e[1])
			c.Check(g[1][0] == rgb.Black, "depth-1 gradient = %v, want zero", g[1][0])
			contributions := 0
			for d := 1; d <= 3; d++ {
				dE := e[d+1].Minus(e[d])
				dG := g[d+1][0].Minus(g[d][0])
				if dE == rgb.Black {
					c.Check(dG == rgb.Black, "zero contribution carried gradient %v", dG)
					continue
				}
				contributions++
				a, n, ok := decomposeScore(dE, dG, testMetal, 1)
				c.Check(ok, "single-choice score of %v / %v does not decompose", dG, dE)
				if ok {
					c.Check(n == 1, "recovered %d choices on a one-choice path", n)
					if a == 1 {
						sawMetal = true
					} else {
						sawNonMetal = true
					}
				}
			}
			c.Check(contributions <= 1, "convex sphere path produced %d contributions", contributions)
		}
		c.Check(sawMetal, "never observed the metal branch (positive score)")
		c.Check(sawNonMetal, "never observed the non-metal branch (negative score)")

		// --- S2: sphere and floor share one material. Paths can accumulate
		// several choices of the same parameter into one channel.
		shared2 := metalMaterial()
		params2 := []render.GradientParam{material.Metalness(shared2)}
		scene2, ray2 := sharedScene(shared2, nil)
		sawRepeat := false
		for seed := int64(0); seed < 400; seed++ {
			var e [6]rgb.Energy
			var g [6][]rgb.Energy
			for d := 1; d <= 5; d++ {
				e[d], g[d] = trace(t, scene2, ray2, seed, d, false, params2)
			}
			contributions := 0
			for d := 1; d <= 4; d++ {
				dE := e[d+1].Minus(e[d])
				dG := g[d+1][0].Minus(g[d][0])
				if dE == rgb.Black {
					c.Check(dG == rgb.Black, "zero contribution carried gradient %v", dG)
					continue
				}
				contributions++
				_, n, ok := decomposeScore(dE, dG, testMetal, d)
				c.Check(ok, "shared-material score of %v / %v does not decompose", dG, dE)
				if ok {
					c.Check(n >= 1, "contribution with no preceding choice")
					if n >= 2 {
						sawRepeat = true
					}
				}
			}
			c.Check(contributions <= 1, "env-only scene produced %d contributions", contributions)
		}
		c.Check(sawRepeat, "never accumulated two shared-parameter choices in one path")

		// --- S3: shared metalness material + tracked emissive light,
		// direct lighting on. Contributions happen at several bounces, so
		// each must carry only the score of choices made before it.
		shared3 := metalMaterial()
		light3 := lightMaterial()
		params3 := []render.GradientParam{material.Metalness(shared3), material.Emission(light3)}
		scene3, ray3 := sharedScene(shared3, light3)
		valid, sawEnvFinal, sawLightFinal, sawN2, sawC0 := 0, false, false, false, false
		for seed := int64(0); seed < 800 && valid < 80; seed++ {
			var e [8]rgb.Energy
			var g [8][]rgb.Energy
			for d := 1; d <= 7; d++ {
				e[d], g[d] = trace(t, scene3, ray3, seed, d, true, params3)
			}
			// Only analyze paths that terminated before the depth cap.
			if e[6] != e[5] || e[7] != e[6] {
				continue
			}
			valid++
			g[0] = make([]rgb.Energy, 2) // depth-0 baseline: zero energy, zero gradient
			prevN, prevD := 0, -1
			for d := 0; d <= 6; d++ {
				dE := e[d+1].Minus(e[d])
				dGm := g[d+1][0].Minus(g[d][0])
				dGe := g[d+1][1].Minus(g[d][1])
				if dE == rgb.Black {
					c.Check(dGm == rgb.Black && dGe == rgb.Black, "zero contribution carried gradient %v / %v", dGm, dGe)
					continue
				}
				_, n, ok := decomposeScore(dE, dGm, testMetal, d+1)
				c.Check(ok, "prefix score of %v / %v at bounce %d does not decompose", dGm, dE, d)
				if ok {
					if d == 0 {
						c.Check(n == 1, "first contribution carried %d choices, want exactly 1", n)
						sawC0 = true
					}
					c.Check(n >= prevN, "choice count went backwards: %d < %d", n, prevN)
					c.Check(n-prevN <= d-prevD, "choice count jumped by %d in %d bounces", n-prevN, d-prevD)
					prevN, prevD = n, d
					if n >= 2 {
						sawN2 = true
					}
				}
			}
			// Emission channel: non-final contributions are direct-lighting
			// terms and must equal beauty/emission exactly; the final
			// contribution is either that or an environment escape (zero).
			for d := 0; d <= 6; d++ {
				dE := e[d+1].Minus(e[d])
				if dE == rgb.Black {
					continue
				}
				dGe := g[d+1][1].Minus(g[d][1])
				isFinal := true
				for d2 := d + 1; d2 <= 6; d2++ {
					if e[d2+1].Minus(e[d2]) != rgb.Black {
						isFinal = false
						break
					}
				}
				if !isFinal {
					c.Check(emitExplicit(dE, dGe, testEmission), "direct-light gradient %v != beauty/emission of %v", dGe, dE)
				} else {
					zero := dGe == rgb.Black
					c.Check(zero || emitExplicit(dE, dGe, testEmission), "final emission gradient %v inconsistent with %v", dGe, dE)
					if zero {
						sawEnvFinal = true
					} else {
						sawLightFinal = true
					}
				}
			}
		}
		c.Check(valid >= 80, "only %d terminated paths analyzed", valid)
		c.Check(sawC0, "never saw a first-bounce direct-light contribution")
		c.Check(sawN2, "never saw a contribution after two choices")
		c.Check(sawEnvFinal, "never saw environment termination")
		c.Check(sawLightFinal, "never saw an emission-carrying final contribution")

		// --- S4: floor + light, no direct lighting. The single contribution
		// is either a light hit (explicit emission term) or an environment
		// escape (exactly zero emission gradient).
		floorMat := metalMaterial()
		light4 := lightMaterial()
		params4 := []render.GradientParam{material.Metalness(floorMat), material.Emission(light4)}
		floor := surface.UnitCube(floorMat).Shift(geom.Vec{Y: -0.55}).Scale(geom.Vec{X: 20, Y: 0.1, Z: 20})
		lamp := surface.UnitSphere(light4).Shift(geom.Vec{Y: 6}).Scale(geom.Vec{X: 6, Y: 6, Z: 6})
		scene4 := render.NewScene(nil, surface.NewTree(floor, lamp), testEnv())
		ray4 := geom.NewRay(geom.Vec{Y: 2}, geom.Dir{Y: -1})
		sawHit, sawEscape := false, false
		for seed := int64(0); seed < 400; seed++ {
			var e [6]rgb.Energy
			var g [6][]rgb.Energy
			for d := 1; d <= 5; d++ {
				e[d], g[d] = trace(t, scene4, ray4, seed, d, false, params4)
			}
			for d := 1; d <= 4; d++ {
				dE := e[d+1].Minus(e[d])
				dGm := g[d+1][0].Minus(g[d][0])
				dGe := g[d+1][1].Minus(g[d][1])
				if dE == rgb.Black {
					c.Check(dGm == rgb.Black && dGe == rgb.Black, "zero contribution carried gradient %v / %v", dGm, dGe)
					continue
				}
				_, n, ok := decomposeScore(dE, dGm, testMetal, d)
				c.Check(ok, "floor-path score of %v / %v does not decompose", dGm, dE)
				if ok {
					c.Check(n == 1, "floor path recovered %d choices, want 1", n)
				}
				if dGe == rgb.Black {
					sawEscape = true // environment termination: no emission gradient
				} else {
					c.Check(emitExplicit(dE, dGe, testEmission), "light-hit gradient %v != beauty/emission of %v", dGe, dE)
					sawHit = true
				}
			}
		}
		c.Check(sawEscape, "never saw environment termination")
		c.Check(sawHit, "never saw a direct emitter hit")
	})
}

// TestGradientTransport compares gradient means against independent
// enumerations of the material branch probabilities and finite-difference
// references, and verifies that recording leaves the random stream and the
// legacy trace untouched.
func TestGradientTransport(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// --- Metalness expectation identity. Because the branch BSDFs do
		// not depend on m, dE[L]/dm = E[L|metal] - E[L|non-metal]; the two
		// conditional expectations are enumerated independently with
		// forced-branch materials (m=1 and m=0). Statistical check: the
		// tolerance is 8 standard errors of the difference of means.
		const K = 2000
		mid := metalMaterial()
		params := []render.GradientParam{material.Metalness(mid)}
		sceneMid := render.NewScene(nil, surface.NewTree(surface.UnitSphere(mid)), testEnv())
		forced := func(m float64) *render.Scene {
			mat := metalMaterial()
			mat.Metalness = m
			return render.NewScene(nil, surface.NewTree(surface.UnitSphere(mat)), testEnv())
		}
		sceneMetal, sceneNonMetal := forced(1), forced(0)
		sceneHi, sceneLo := forced(testMetal+0.2), forced(testMetal-0.2)
		ray := geom.NewRay(geom.Vec{Z: 2}, geom.Dir{Z: -1})

		var gSum, gSq, rSum, rSq, fSum, fSq rgb.Energy
		for seed := int64(0); seed < K; seed++ {
			_, g := trace(t, sceneMid, ray, seed, 3, false, params)
			eMetal, _ := trace(t, sceneMetal, ray, seed, 3, false, nil)
			eNon, _ := trace(t, sceneNonMetal, ray, seed, 3, false, nil)
			eHi, _ := trace(t, sceneHi, ray, seed, 3, false, nil)
			eLo, _ := trace(t, sceneLo, ray, seed, 3, false, nil)
			ref := eMetal.Minus(eNon)
			fd := eHi.Minus(eLo).Scaled(1 / 0.4)
			gSum = gSum.Plus(g[0])
			gSq = gSq.Plus(g[0].Times(g[0]))
			rSum = rSum.Plus(ref)
			rSq = rSq.Plus(ref.Times(ref))
			fSum = fSum.Plus(fd)
			fSq = fSq.Plus(fd.Times(fd))
		}
		stats := func(sum, sq rgb.Energy) (mean, sem rgb.Energy) {
			n := float64(K)
			mean = sum.Scaled(1 / n)
			v := sq.Scaled(1 / n).Minus(mean.Times(mean))
			return mean, rgb.Energy{
				X: math.Sqrt(math.Max(0, v.X) / n),
				Y: math.Sqrt(math.Max(0, v.Y) / n),
				Z: math.Sqrt(math.Max(0, v.Z) / n),
			}
		}
		gMean, gSem := stats(gSum, gSq)
		rMean, rSem := stats(rSum, rSq)
		fMean, fSem := stats(fSum, fSq)
		for i := 0; i < 3; i++ {
			gm := []float64{gMean.X, gMean.Y, gMean.Z}[i]
			rm := []float64{rMean.X, rMean.Y, rMean.Z}[i]
			fm := []float64{fMean.X, fMean.Y, fMean.Z}[i]
			semGR := math.Hypot([]float64{gSem.X, gSem.Y, gSem.Z}[i], []float64{rSem.X, rSem.Y, rSem.Z}[i])
			semGF := math.Hypot([]float64{gSem.X, gSem.Y, gSem.Z}[i], []float64{fSem.X, fSem.Y, fSem.Z}[i])
			c.Check(math.Abs(gm-rm) <= 8*semGR+1e-6,
				"channel %d: gradient mean %.6f != branch enumeration %.6f (sem %.6f)", i, gm, rm, semGR)
			c.Check(math.Abs(gm-fm) <= 8*semGF+1e-6,
				"channel %d: gradient mean %.6f != finite difference %.6f (sem %.6f)", i, gm, fm, semGF)
		}
		// The compared quantity must be genuinely nonzero, not noise around zero.
		biggest := math.Max(math.Abs(rMean.X), math.Max(math.Abs(rMean.Y), math.Abs(rMean.Z)))
		c.Check(biggest > 0.05, "reference derivative magnitude %.6f too small to be meaningful", biggest)

		// --- Emission expectation: beauty is linear in Emission, so a
		// finite difference of two real renders is an unbiased reference.
		diffuser := &material.Uniform{Color: rgb.Energy{X: 0.8, Y: 0.8, Z: 0.8}, Roughness: 0.5, Specularity: 0.04}
		emitScene := func(emission float64) (*render.Scene, *geom.Ray) {
			lamp := &material.Uniform{Color: rgb.Energy{X: 1, Y: 0.9, Z: 0.8}, Emission: emission}
			ball := surface.UnitSphere(diffuser)
			light := surface.UnitSphere(lamp).Shift(geom.Vec{Y: 5}).Scale(geom.Vec{X: 6, Y: 6, Z: 6})
			s := render.NewScene(nil, surface.NewTree(ball, light), flatEnv{rgb.Energy{X: 0.5, Y: 0.6, Z: 0.7}})
			return s, geom.NewRay(geom.Vec{Y: 1.5}, geom.Dir{Y: -1})
		}
		tracked := &material.Uniform{Color: rgb.Energy{X: 1, Y: 0.9, Z: 0.8}, Emission: 8}
		ball := surface.UnitSphere(diffuser)
		trackedLamp := surface.UnitSphere(tracked).Shift(geom.Vec{Y: 5}).Scale(geom.Vec{X: 6, Y: 6, Z: 6})
		sceneT := render.NewScene(nil, surface.NewTree(ball, trackedLamp), flatEnv{rgb.Energy{X: 0.5, Y: 0.6, Z: 0.7}})
		paramsT := []render.GradientParam{material.Emission(tracked)}
		rayT := geom.NewRay(geom.Vec{Y: 1.5}, geom.Dir{Y: -1})
		sceneHiE, rayHi := emitScene(8.4)
		sceneLoE, rayLo := emitScene(7.6)

		var egSum, egSq, efSum, efSq rgb.Energy
		for seed := int64(0); seed < K; seed++ {
			_, g := trace(t, sceneT, rayT, seed, 4, false, paramsT)
			eHi, _ := trace(t, sceneHiE, rayHi, seed, 4, false, nil)
			eLo, _ := trace(t, sceneLoE, rayLo, seed, 4, false, nil)
			fd := eHi.Minus(eLo).Scaled(1 / 0.8)
			egSum = egSum.Plus(g[0])
			egSq = egSq.Plus(g[0].Times(g[0]))
			efSum = efSum.Plus(fd)
			efSq = efSq.Plus(fd.Times(fd))
		}
		egMean, egSem := stats(egSum, egSq)
		efMean, efSem := stats(efSum, efSq)
		for i := 0; i < 3; i++ {
			gm := []float64{egMean.X, egMean.Y, egMean.Z}[i]
			fm := []float64{efMean.X, efMean.Y, efMean.Z}[i]
			sem := math.Hypot([]float64{egSem.X, egSem.Y, egSem.Z}[i], []float64{efSem.X, efSem.Y, efSem.Z}[i])
			c.Check(math.Abs(gm-fm) <= 8*sem+1e-6,
				"channel %d: emission gradient mean %.6f != finite difference %.6f (sem %.6f)", i, gm, fm, sem)
			c.Check(gm > 6*[]float64{egSem.X, egSem.Y, egSem.Z}[i],
				"channel %d: emission gradient %.6f not significantly positive", i, gm)
		}

		// --- Recording must not consume path randomness: identical beauty
		// and identical next random number with recording on and off, and
		// identical output to the legacy production trace entry point.
		shared := metalMaterial()
		lamp := lightMaterial()
		paramsR := []render.GradientParam{material.Metalness(shared), material.Emission(lamp)}
		sceneR, rayR := sharedScene(shared, lamp)
		seed := int64(0)
		var e1 rgb.Energy
		var g1 []rgb.Energy
		var next1 float64
		for ; ; seed++ {
			rnd1 := rand.New(rand.NewSource(seed))
			var err error
			e1, g1, err = render.TraceGradient(sceneR, rayR, rnd1, 5, true, paramsR)
			if err != nil {
				t.Fatalf("TraceGradient: %v", err)
			}
			next1 = rnd1.Float64()
			if e1 != rgb.Black {
				break
			}
		}
		rnd2 := rand.New(rand.NewSource(seed))
		e2, g2, err := render.TraceGradient(sceneR, rayR, rnd2, 5, true, nil)
		c.Check(err == nil, "TraceGradient without params: %v", err)
		next2 := rnd2.Float64()
		c.Check(e1 == e2, "recording changed beauty: %v != %v", e1, e2)
		c.Check(len(g2) == 0, "no params returned %d gradient channels", len(g2))
		c.Check(next1 == next2, "recording consumed randomness: next %.17g != %.17g", next1, next2)
		legacy := render.SeedProductionTrace(sceneR, rayR, seed, 5, true)
		c.Check(e1 == legacy, "gradient trace diverged from legacy trace: %v != %v", e1, legacy)
		nonzero := false
		for _, gc := range g1 {
			if gc != rgb.Black {
				nonzero = true
			}
		}
		c.Check(nonzero, "recorded gradients unexpectedly all zero")

		// --- A custom material that is never declared differentiable keeps
		// working untouched; an absent tracked material yields a zero channel.
		custom := surface.UnitSphere(customMat{})
		sceneC := render.NewScene(nil, surface.NewTree(custom), testEnv())
		absent := metalMaterial()
		paramsC := []render.GradientParam{material.Metalness(absent)}
		ec, gc := trace(t, sceneC, geom.NewRay(geom.Vec{Z: 2}, geom.Dir{Z: -1}), 7, 3, false, paramsC)
		c.Check(!math.IsNaN(ec.X) && ec.Mean() >= 0, "custom material beauty broken: %v", ec)
		c.Check(gc[0] == rgb.Black, "absent material produced gradient %v", gc[0])
	})
}

// customMat is a user material that does not participate in gradient tracking.
type customMat struct{}

func (customMat) At(u, v float64, in, norm geom.Dir, rnd *rand.Rand) (geom.Dir, render.BSDF) {
	return norm, bsdf.Lambert{Color: rgb.Energy{X: 0.5, Y: 0.6, Z: 0.7}, Multiplier: 1}
}

func (customMat) Light() rgb.Energy    { return rgb.Black }
func (customMat) Transmit() rgb.Energy { return rgb.Black }

// TestGradientFilm checks the synchronous frame entry point: per-ray
// linearity, shared normalization, signed and zero-influence channels,
// random-stream preservation, result isolation, and input validation.
func TestGradientFilm(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		shared := metalMaterial()
		lamp := &material.Uniform{Color: rgb.Energy{X: 1, Y: 0.95, Z: 0.9}, Emission: 30}
		unused := &material.Uniform{
			Color:       rgb.Energy{X: 0.5, Y: 0.5, Z: 0.5},
			Metalness:   0.5,
			Roughness:   0.4,
			Specularity: 0.05,
		}
		ball1 := surface.UnitSphere(shared).Shift(geom.Vec{X: -0.7, Y: 0.2})
		ball2 := surface.UnitSphere(shared).Shift(geom.Vec{X: 0.7, Y: 0.2})
		light := surface.UnitSphere(lamp).Shift(geom.Vec{Y: 4, Z: 0.5}).Scale(geom.Vec{X: 4, Y: 4, Z: 4})
		floorMat := &material.Uniform{Color: rgb.Energy{X: 0.7, Y: 0.7, Z: 0.7}, Roughness: 0.6, Specularity: 0.04}
		floor := surface.UnitCube(floorMat).Shift(geom.Vec{Y: -0.35}).Scale(geom.Vec{X: 20, Y: 0.1, Z: 20})
		cam := camera.NewSLR().MoveTo(geom.Vec{Y: 1.0, Z: 3.5}).LookAt(geom.Vec{Y: 0.2})
		scene := render.NewScene(cam, surface.NewTree(ball1, ball2, light, floor), flatEnv{rgb.Energy{X: 0.2, Y: 0.25, Z: 0.35}})
		params := []render.GradientParam{
			material.Metalness(shared),
			material.Emission(lamp),
			material.Metalness(unused),
		}
		const w, h, samples, depth = 6, 4, 8, 5
		const seed = int64(11)
		res, err := render.RenderGradient(scene, w, h, samples, depth, true, seed, params)
		c.Check(err == nil, "RenderGradient: %v", err)
		if err != nil {
			return
		}
		c.Check(res.Width == w && res.Height == h, "result size %dx%d", res.Width, res.Height)
		c.Check(res.Samples == samples, "result samples %d", res.Samples)
		c.Check(len(res.Beauty) == w*h && len(res.Channels) == 3, "result buffers %d / %d", len(res.Beauty), len(res.Channels))

		// The frame must equal the per-ray linear sum under one normalization.
		rnd := rand.New(rand.NewSource(seed))
		bSum := make([]rgb.Energy, w*h)
		gSum := make([][]rgb.Energy, 3)
		for ch := range gSum {
			gSum[ch] = make([]rgb.Energy, w*h)
		}
		for y := 0; y < h; y++ {
			for x := 0; x < w; x++ {
				i := y*w + x
				for n := 0; n < samples; n++ {
					rx := float64(x) + rnd.Float64()
					ry := float64(y) + rnd.Float64()
					r := cam.Ray(rx, ry, float64(w), float64(h), rnd)
					e, g, err := render.TraceGradient(scene, r, rnd, depth, true, params)
					if err != nil {
						t.Fatalf("TraceGradient: %v", err)
					}
					bSum[i] = bSum[i].Plus(e)
					for ch := 0; ch < 3; ch++ {
						gSum[ch][i] = gSum[ch][i].Plus(g[ch])
					}
				}
			}
		}
		norm := 1 / float64(samples)
		close := func(a, b rgb.Energy) bool {
			d := a.Minus(b)
			return math.Abs(d.X) < 1e-9 && math.Abs(d.Y) < 1e-9 && math.Abs(d.Z) < 1e-9
		}
		mismatch := 0
		for i := 0; i < w*h; i++ {
			if !close(res.Beauty[i], bSum[i].Scaled(norm)) {
				mismatch++
			}
			for ch := 0; ch < 3; ch++ {
				if !close(res.Channels[ch][i], gSum[ch][i].Scaled(norm)) {
					mismatch++
				}
			}
		}
		c.Check(mismatch == 0, "%d pixel/channel values differ from per-ray sums", mismatch)

		// Beauty is finite and somewhere positive.
		positive, finite := false, true
		for _, b := range res.Beauty {
			if math.IsNaN(b.X+b.Y+b.Z) || math.IsInf(b.X+b.Y+b.Z, 0) {
				finite = false
			}
			if b.Mean() > 0 {
				positive = true
			}
		}
		c.Check(finite, "beauty contains non-finite values")
		c.Check(positive, "beauty is everywhere zero")

		// Metalness gradients are signed and unclamped: both signs appear.
		anyPos, anyNeg := false, false
		for _, g := range res.Channels[0] {
			for _, v := range []float64{g.X, g.Y, g.Z} {
				if v > 0 {
					anyPos = true
				}
				if v < 0 {
					anyNeg = true
				}
			}
		}
		c.Check(anyPos, "metalness channel has no positive gradient")
		c.Check(anyNeg, "metalness channel has no negative gradient")

		// Emission gradients come only from the explicit term: never negative,
		// and positive somewhere the light matters.
		emPos, emNeg := false, false
		for _, g := range res.Channels[1] {
			for _, v := range []float64{g.X, g.Y, g.Z} {
				if v > 0 {
					emPos = true
				}
				if v < 0 {
					emNeg = true
				}
			}
		}
		c.Check(emPos, "emission channel is everywhere zero")
		c.Check(!emNeg, "emission channel has negative gradient")

		// A tracked material that no path touches is exactly zero.
		zeroChannel := true
		for _, g := range res.Channels[2] {
			if g != rgb.Black {
				zeroChannel = false
			}
		}
		c.Check(zeroChannel, "unused-material channel is not exactly zero")

		// Recording does not consume randomness at frame level either.
		resNil, err := render.RenderGradient(scene, w, h, samples, depth, true, seed, nil)
		c.Check(err == nil, "RenderGradient without params: %v", err)
		same := err == nil
		if err == nil {
			for i := range res.Beauty {
				if resNil.Beauty[i] != res.Beauty[i] {
					same = false
				}
			}
		}
		c.Check(same, "recording changed the frame beauty")

		// Results are independent between render instances.
		res2, err := render.RenderGradient(scene, w, h, samples, depth, true, seed, params)
		c.Check(err == nil, "second RenderGradient: %v", err)
		if err == nil {
			c.Check(res2.Beauty[0] == res.Beauty[0], "same seed produced different frames")
			res.Beauty[0] = rgb.Energy{X: -99, Y: -99, Z: -99}
			res.Channels[0][0] = rgb.Energy{X: -99, Y: -99, Z: -99}
			c.Check(res2.Beauty[0] != res.Beauty[0], "mutating one result leaked into another")
			c.Check(res2.Channels[0][0] != res.Channels[0][0], "mutating one channel leaked into another")
		}

		// --- Validation: bad params and sizes are rejected before rendering.
		ray := geom.NewRay(geom.Vec{Z: 2}, geom.Dir{Z: -1})
		simple := render.NewScene(nil, surface.NewTree(surface.UnitSphere(metalMaterial())), testEnv())
		expectErr := func(name string, params []render.GradientParam) {
			_, _, err := render.TraceGradient(simple, ray, rand.New(rand.NewSource(1)), 3, false, params)
			c.Check(err != nil, "%s: expected error, got nil", name)
		}
		m := metalMaterial()
		expectErr("duplicate metalness", []render.GradientParam{material.Metalness(m), material.Metalness(m)})
		l := lightMaterial()
		expectErr("duplicate emission", []render.GradientParam{material.Emission(l), material.Emission(l)})
		expectErr("nil material", []render.GradientParam{{Material: nil, Kind: render.Metalness}})
		expectErr("bad kind", []render.GradientParam{{Material: m, Kind: render.GradientKind(9)}})
		expectErr("unsupported material", []render.GradientParam{{Material: &surface.DefaultMaterial{}, Kind: render.Metalness}})
		for _, bad := range []float64{0, 1, -0.2, 1.5, math.NaN(), math.Inf(1)} {
			bm := metalMaterial()
			bm.Metalness = bad
			expectErr("metalness range", []render.GradientParam{material.Metalness(bm)})
		}
		bm := metalMaterial()
		bm.Transmission = 0.2
		expectErr("transmission", []render.GradientParam{material.Metalness(bm)})
		bm = metalMaterial()
		bm.Roughness = 0
		expectErr("roughness zero", []render.GradientParam{material.Metalness(bm)})
		bm = metalMaterial()
		bm.Roughness = math.NaN()
		expectErr("roughness NaN", []render.GradientParam{material.Metalness(bm)})
		for _, bad := range []float64{0, -2, math.NaN(), math.Inf(1)} {
			bl := lightMaterial()
			bl.Emission = bad
			expectErr("emission range", []render.GradientParam{material.Emission(bl)})
		}
		// A material valid for both kinds may declare both channels.
		both := metalMaterial()
		both.Emission = 2
		_, _, err = render.TraceGradient(simple, ray, rand.New(rand.NewSource(1)), 3, false,
			[]render.GradientParam{material.Metalness(both), material.Emission(both)})
		c.Check(err == nil, "valid dual params rejected: %v", err)

		// Invalid frame dimensions, sample counts, depths, and scenes.
		_, err = render.RenderGradient(scene, 0, h, samples, depth, true, seed, params)
		c.Check(err != nil, "zero width accepted")
		_, err = render.RenderGradient(scene, w, -1, samples, depth, true, seed, params)
		c.Check(err != nil, "negative height accepted")
		_, err = render.RenderGradient(scene, w, h, 0, depth, true, seed, params)
		c.Check(err != nil, "zero samples accepted")
		_, err = render.RenderGradient(scene, w, h, samples, 0, true, seed, params)
		c.Check(err != nil, "zero depth accepted")
		_, err = render.RenderGradient(nil, w, h, samples, depth, true, seed, params)
		c.Check(err != nil, "nil scene accepted")
		noCam := render.NewScene(nil, surface.NewTree(ball1), testEnv())
		_, err = render.RenderGradient(noCam, w, h, samples, depth, true, seed, params)
		c.Check(err != nil, "nil camera accepted")
		_, err = render.RenderGradient(scene, w, h, samples, depth, true, seed,
			[]render.GradientParam{material.Metalness(shared), material.Metalness(shared)})
		c.Check(err != nil, "duplicate frame params accepted")
		_, _, err = render.TraceGradient(simple, nil, rand.New(rand.NewSource(1)), 3, false, nil)
		c.Check(err != nil, "nil ray accepted")
		_, _, err = render.TraceGradient(simple, ray, nil, 3, false, nil)
		c.Check(err != nil, "nil random source accepted")
		_, _, err = render.TraceGradient(simple, ray, rand.New(rand.NewSource(1)), 0, false, nil)
		c.Check(err != nil, "zero depth accepted")
	})
}
