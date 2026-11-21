package render3d

import (
	"math"
	"math/rand"
	"runtime"
	"sync"
	"testing"

	"github.com/unixpickle/model3d/model3d"
	"github.com/unixpickle/model3d/task_support/taskcheck"
)

// guidedTestTriangles builds two axis-aligned triangles in
// the z=0 plane of unequal area. They do not overlap.
func guidedTestTriangles() []*model3d.Triangle {
	return []*model3d.Triangle{
		// Area 2.
		{model3d.XYZ(0, 0, 0), model3d.XYZ(2, 0, 0), model3d.XYZ(0, 2, 0)},
		// Area 0.5.
		{model3d.XYZ(4, 0, 0), model3d.XYZ(5, 0, 0), model3d.XYZ(5, 1, 0)},
	}
}

func guidedTestColors() []Color {
	return []Color{
		renderColor(1, 2, 3),
		renderColor(4, 0.5, 0.25),
	}
}

func renderColor(r, g, b float64) Color {
	return Color{X: r, Y: g, Z: b}
}

func TestGuidedDistribution(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		tris := guidedTestTriangles()
		colors := guidedTestColors()
		areas := []float64{tris[0].Area(), tris[1].Area()}
		c.Check(math.Abs(areas[0]-2) < 1e-12, "big triangle area should be 2, got %v", areas[0])
		c.Check(math.Abs(areas[1]-0.5) < 1e-12, "small triangle area should be 0.5, got %v", areas[1])

		// Unequal areas, different colors, and a guide
		// inversely related to brightness.
		guide := []float64{0.2, 8}
		light, err := NewGuidedMeshAreaLight(tris, colors, guide)
		c.Check(err == nil, "constructor: %v", err)
		dl, ok := light.(AreaDensityLight)
		c.Check(ok, "guided light must implement AreaDensityLight")

		// TotalEmission is the area integral of the true
		// colors and never involves the guides.
		wantEmission := areas[0]*colors[0].Sum() + areas[1]*colors[1].Sum()
		c.Check(math.Abs(light.TotalEmission()-wantEmission) < 1e-10,
			"TotalEmission=%v want %v", light.TotalEmission(), wantEmission)

		// Empirical face selection follows area*guide.
		masses := []float64{areas[0] * guide[0], areas[1] * guide[1]}
		rng := rand.New(rand.NewSource(7))
		counts := []int{0, 0}
		samples := 60000
		for i := 0; i < samples; i++ {
			point, normal, emission := light.SampleLight(rng)
			_ = normal
			idx := -1
			for k, tri := range tris {
				if insideTriangle(tri, point) {
					idx = k
					break
				}
			}
			c.Check(idx >= 0, "sampled point must land on a face: %v", point)
			if idx >= 0 {
				counts[idx]++
				// SampleLight reports the true color of the
				// face, not a guide-weighted color.
				c.Check(emission == colors[idx], "emission %v != face color %v",
					emission, colors[idx])
			}
		}
		massTotal := masses[0] + masses[1]
		for k := range counts {
			got := float64(counts[k]) / float64(samples)
			want := masses[k] / massTotal
			c.Check(math.Abs(got-want) < 0.02, "face %d selection %v want %v", k, got, want)
		}

		// The analytical area density on each face is
		// mass/(totalMass*area), and integrates to one.
		interior0 := model3d.XYZ(0.5, 0.5, 0)
		interior1 := model3d.XYZ(4.7, 0.4, 0)
		normal := model3d.Z(1)
		for k, p := range []model3d.Coord3D{interior0, interior1} {
			got := dl.LightDensity(p, normal)
			want := masses[k] / (massTotal * areas[k])
			c.Check(math.Abs(got-want) < 1e-10, "face %d density %v want %v", k, got, want)
		}
		// Monte Carlo integral of the density: E[1/p_A] over
		// the proposal equals the total surface area.
		rng2 := rand.New(rand.NewSource(8))
		invSum := 0.0
		for i := 0; i < 20000; i++ {
			p, n, _ := light.SampleLight(rng2)
			d := dl.LightDensity(p, n)
			c.Check(d > 0, "density at sampled point must be positive")
			invSum += 1 / d
		}
		c.Check(math.Abs(invSum/20000-(areas[0]+areas[1])) < 0.12,
			"density integral mismatch: %v", invSum/20000)

		// Changing the guide changes the proposal but not
		// the linear radiance.
		guide2 := []float64{8, 0.2}
		light2, err := NewGuidedMeshAreaLight(tris, colors, guide2)
		c.Check(err == nil, "constructor: %v", err)
		c.Check(math.Abs(light2.TotalEmission()-wantEmission) < 1e-12,
			"emission must be guide-independent")
		dl2 := light2.(AreaDensityLight)
		c.Check(dl2.LightDensity(interior0, normal) != dl.LightDensity(interior0, normal),
			"density must respond to the guide")
		c.Check(dl2.LightDensity(interior0, normal) > dl.LightDensity(interior0, normal),
			"larger guide must raise the face density")

		// All-zero guide falls back to area-proportional
		// sampling with a uniform 1/totalArea density.
		fallback, err := NewGuidedMeshAreaLight(tris, colors, []float64{0, 0})
		c.Check(err == nil, "all-zero guide constructor: %v", err)
		dlFallback := fallback.(AreaDensityLight)
		uniform := 1 / (areas[0] + areas[1])
		c.Check(math.Abs(dlFallback.LightDensity(interior0, normal)-uniform) < 1e-12,
			"fallback density %v", dlFallback.LightDensity(interior0, normal))
		c.Check(math.Abs(dlFallback.LightDensity(interior1, normal)-uniform) < 1e-12,
			"fallback density must be uniform")
		rng3 := rand.New(rand.NewSource(9))
		var areaCount [2]int
		for i := 0; i < 40000; i++ {
			p, _, _ := fallback.SampleLight(rng3)
			if insideTriangle(tris[0], p) {
				areaCount[0]++
			} else {
				areaCount[1]++
			}
		}
		gotFrac := float64(areaCount[0]) / 40000
		c.Check(math.Abs(gotFrac-areas[0]/(areas[0]+areas[1])) < 0.02,
			"area fallback selection %v", gotFrac)

		// Off-surface points and normal mismatches give 0.
		c.Check(dl.LightDensity(model3d.XYZ(0.5, 0.5, 0.5), normal) == 0,
			"off-surface point must have zero density")
		c.Check(dl.LightDensity(model3d.XYZ(10, 10, 0), normal) == 0,
			"point outside every face must have zero density")
		c.Check(dl.LightDensity(interior0, model3d.Z(-1)) == 0,
			"normal mismatch must give zero density")

		// A black face with a positive guide can still be
		// sampled, while an emitting face with a zero guide
		// is rejected unless every guide is zero.
		blackColors := []Color{renderColor(0, 0, 0), renderColor(1, 1, 1)}
		blackLight, err := NewGuidedMeshAreaLight(tris, blackColors, []float64{3, 1})
		c.Check(err == nil, "black face with positive guide: %v", err)
		rng4 := rand.New(rand.NewSource(10))
		sawBlack := false
		for i := 0; i < 2000; i++ {
			_, _, e := blackLight.SampleLight(rng4)
			if e.Sum() == 0 {
				sawBlack = true
			}
		}
		c.Check(sawBlack, "black face must remain sampleable directly")
		_, err = NewGuidedMeshAreaLight(tris, colors, []float64{0, 1})
		c.Check(err != nil, "emitting face with zero guide must be rejected")
		_, err = NewGuidedMeshAreaLight(tris, colors, []float64{0, 0})
		c.Check(err == nil, "all-zero guide must be allowed despite emission")

		// Constructor validation.
		_, err = NewGuidedMeshAreaLight(nil, colors, guide)
		c.Check(err != nil, "empty triangles must be rejected")
		_, err = NewGuidedMeshAreaLight(tris, colors[:1], guide)
		c.Check(err != nil, "mismatched colors must be rejected")
		_, err = NewGuidedMeshAreaLight(tris, colors, guide[:1])
		c.Check(err != nil, "mismatched guides must be rejected")
		badTri := &model3d.Triangle{model3d.XYZ(0, 0, 0), model3d.XYZ(1, 0, 0), model3d.XYZ(2, 0, 0)}
		_, err = NewGuidedMeshAreaLight([]*model3d.Triangle{badTri},
			[]Color{renderColor(1, 1, 1)}, []float64{1})
		c.Check(err != nil, "degenerate triangle must be rejected")
		_, err = NewGuidedMeshAreaLight(tris, colors, []float64{-1, 1})
		c.Check(err != nil, "negative guide must be rejected")
		_, err = NewGuidedMeshAreaLight(tris, colors, []float64{math.NaN(), 1})
		c.Check(err != nil, "NaN guide must be rejected")
		_, err = NewGuidedMeshAreaLight(tris,
			[]Color{renderColor(-1, 0, 0), colors[1]}, guide)
		c.Check(err != nil, "negative color must be rejected")

		// Cast reports the material of the face actually
		// hit, from either side (double-sided emission).
		rayDown := &model3d.Ray{Origin: model3d.XYZ(0.5, 0.5, 1), Direction: model3d.Z(-1)}
		_, mat, ok := light.Cast(rayDown)
		c.Check(ok, "ray from above must hit the light")
		c.Check(ok && mat.Emission() == colors[0], "Cast must return hit-face emission")
		rayUp := &model3d.Ray{Origin: model3d.XYZ(0.5, 0.5, -1), Direction: model3d.Z(1)}
		_, mat, ok = light.Cast(rayUp)
		c.Check(ok, "ray from below must hit the double-sided light")
		c.Check(ok && mat.Emission() == colors[0], "back side must use the same emission")
		_, _, ok = light.Cast(&model3d.Ray{
			Origin: model3d.XYZ(10, 0.5, 1), Direction: model3d.Z(-1),
		})
		c.Check(!ok, "ray missing every face must not collide")

		// Shared edges are assigned to exactly one face in
		// fixed order, even when neighboring faces share
		// the edge.
		shared := sharedEdgeLight(t, c)
		_ = shared

		// Input mutation cannot change a constructed light.
		mutationSnapshot(t, c, tris, colors, guide)
	})
}

// insideTriangle reports whether p lies in t's plane and
// inside its boundary.
func insideTriangle(t *model3d.Triangle, p model3d.Coord3D) bool {
	bary, dist := triangleBarycentric(t, p)
	if dist > areaDensityEpsilon {
		return false
	}
	for _, b := range bary {
		if b < -areaDensityEpsilon {
			return false
		}
	}
	return true
}

// sharedEdgeLight builds two triangles sharing an edge and
// checks unique edge ownership plus aggregate normalization.
func sharedEdgeLight(t *testing.T, c *taskcheck.C) AreaDensityLight {
	tris := []*model3d.Triangle{
		{model3d.XYZ(0, 0, 0), model3d.XYZ(1, 0, 0), model3d.XYZ(0, 1, 0)},
		{model3d.XYZ(1, 0, 0), model3d.XYZ(1, 1, 0), model3d.XYZ(0, 1, 0)},
	}
	colors := []Color{renderColor(1, 1, 1), renderColor(2, 2, 2)}
	guides := []float64{1, 3}
	light, err := NewGuidedMeshAreaLight(tris, colors, guides)
	c.Check(err == nil, "shared-edge constructor: %v", err)
	dl := light.(AreaDensityLight)

	// Points strictly on the shared edge.
	edgePoints := []model3d.Coord3D{
		model3d.XYZ(0.5, 0.5, 0),
		model3d.XYZ(0.25, 0.75, 0),
		model3d.XYZ(1, 1, 0),
	}
	n := model3d.Z(1)
	areas := [2]float64{tris[0].Area(), tris[1].Area()}
	totalMass := areas[0]*guides[0] + areas[1]*guides[1]
	analytic := [2]float64{
		areas[0] * guides[0] / (totalMass * areas[0]),
		areas[1] * guides[1] / (totalMass * areas[1]),
	}
	for _, p := range edgePoints {
		d := dl.LightDensity(p, n)
		matched := math.Abs(d-analytic[0]) < 1e-10 || math.Abs(d-analytic[1]) < 1e-10
		c.Check(d > 0 && matched,
			"edge point %v must map to exactly one face, got density %v", p, d)
	}
	// A dense grid integrates to one over the union.
	rng := rand.New(rand.NewSource(11))
	inv := 0.0
	for i := 0; i < 30000; i++ {
		p, nn, _ := light.SampleLight(rng)
		inv += 1 / dl.LightDensity(p, nn)
	}
	totalArea := tris[0].Area() + tris[1].Area()
	c.Check(math.Abs(inv/30000-totalArea) < 0.03, "shared-edge density integral: %v", inv/30000)
	return dl
}

// mutationSnapshot constructs a light, records its
// observable behavior, mutates every input slice, and
// checks the light is unchanged.
func mutationSnapshot(t *testing.T, c *taskcheck.C, tris []*model3d.Triangle,
	colors []Color, guide []float64) {
	light, err := NewGuidedMeshAreaLight(tris, colors, guide)
	c.Check(err == nil, "snapshot constructor: %v", err)
	dl := light.(AreaDensityLight)
	rng := rand.New(rand.NewSource(12))
	type snap struct {
		point    model3d.Coord3D
		normal   model3d.Coord3D
		emission Color
	}
	snaps := make([]snap, 200)
	for i := range snaps {
		p, n, e := light.SampleLight(rng)
		snaps[i] = snap{p, n, e}
	}
	emissionBefore := light.TotalEmission()
	densityBefore := dl.LightDensity(model3d.XYZ(0.5, 0.5, 0), model3d.Z(1))

	// Mutate the caller's data aggressively.
	tris[0][0] = model3d.XYZ(99, 99, 99)
	colors[0] = renderColor(100, 100, 100)
	guide[0] = 1e6
	guide[1] = -50

	rng2 := rand.New(rand.NewSource(12))
	for i := range snaps {
		p, n, e := light.SampleLight(rng2)
		c.Check(p == snaps[i].point && n == snaps[i].normal && e == snaps[i].emission,
			"snapshot changed at sample %d", i)
	}
	c.Check(light.TotalEmission() == emissionBefore, "TotalEmission changed after mutation")
	c.Check(dl.LightDensity(model3d.XYZ(0.5, 0.5, 0), model3d.Z(1)) == densityBefore,
		"LightDensity changed after mutation")
	_, mat, ok := light.Cast(&model3d.Ray{
		Origin: model3d.XYZ(0.5, 0.5, 1), Direction: model3d.Z(-1),
	})
	c.Check(ok && mat.Emission() != renderColor(100, 100, 100),
		"Cast material changed after mutation")
}

func TestGuidedStrategies(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		proposal, lightV, eyeV, light := syntheticStrategyPath(c)

		// Concurrent reverse lookups while sampling, to
		// exercise the no-cached-state requirement.
		var wg sync.WaitGroup
		for worker := 0; worker < runtime.NumCPU()*2; worker++ {
			wg.Add(1)
			go func(seed int64) {
				defer wg.Done()
				rng := rand.New(rand.NewSource(seed))
				dl := light.(AreaDensityLight)
				for i := 0; i < 2000; i++ {
					p, n, _ := light.SampleLight(rng)
					d := dl.LightDensity(p, n)
					if !(d > 0) {
						c.Check(false, "concurrent density lookup failed at %v", p)
						return
					}
				}
			}(int64(worker + 1))
		}
		wg.Wait()

		// Mixed legacy/new joins and aggregate density.
		aggregateChecks(c, lightV, light)

		// Enumerate every (eye prefix, light prefix) split
		// and compare the callback density against an
		// independently assembled reference value.
		cache := newBptPathCache(8)
		eyePath := &bptEyePath{bptPath{Points: eyeV}}
		lightPath := &bptLightPath{bptPath{Points: lightV}}
		enumerated := 0
		allPathCombinations(eyePath, lightPath, cache, proposal,
			func(got float64, intensity Color, p1, p2 model3d.Coord3D) {
				if intensity.Sum() <= 0 {
					return
				}
				enumerated++
				joined := cache.JoinedPath

				// The joined path's own strategy densities
				// must match a from-scratch reference
				// enumeration, including the pure backward
				// term and every light-starting split.
				wantStrategies := referenceStrategyDensities(joined.Points, proposal)
				var gotStrategies []float64
				joined.Densities(proposal, 8, 8, func(d float64) {
					gotStrategies = append(gotStrategies, d)
				})
				c.Check(len(gotStrategies) == len(wantStrategies),
					"strategy count %d want %d", len(gotStrategies), len(wantStrategies))
				for i := range wantStrategies {
					if i >= len(gotStrategies) {
						break
					}
					c.Check(math.Abs(gotStrategies[i]-wantStrategies[i]) < 1e-9,
						"strategy %d density %v want %v", i, gotStrategies[i],
						wantStrategies[i])
				}

				// Balance weights over the full strategy set
				// form a partition of unity; power-2 weights
				// stay in [0,1] and also sum to one.
				checkWeights(c, wantStrategies, got, 1)
				checkWeights(c, wantStrategies, got, 2)
			})
		c.Check(enumerated >= 2, "expected non-trivial connection splits, got %d", enumerated)

		// Independently enumerate every (eye prefix, light
		// prefix) split so joined paths of three, four and
		// five vertices are all covered, including splits
		// whose connection geometry contributes nothing in
		// the live callback above.
		splitCount := 0
		for i := 1; i <= len(eyeV); i++ {
			for j := 1; j <= len(lightV); j++ {
				subEye := bptEyePath{bptPath{Points: eyeV[:i]}}
				subLight := bptLightPath{bptPath{Points: lightV[:j]}}
				combinePaths(subEye, subLight, cache)
				joined := cache.JoinedPath
				nv := len(joined.Points)
				if nv < 3 || nv > 5 {
					continue
				}
				splitCount++
				wantStrategies := referenceStrategyDensities(joined.Points, proposal)
				var gotStrategies []float64
				joined.Densities(proposal, 8, 8, func(d float64) {
					gotStrategies = append(gotStrategies, d)
				})
				c.Check(len(gotStrategies) == len(wantStrategies),
					"%d-vertex split: strategy count %d want %d", nv,
					len(gotStrategies), len(wantStrategies))
				for k := range wantStrategies {
					if k >= len(gotStrategies) {
						break
					}
					if positiveFinite(wantStrategies[k]) {
						c.Check(math.Abs(gotStrategies[k]-wantStrategies[k]) < 1e-9,
							"%d-vertex split strategy %d: %v want %v", nv, k,
							gotStrategies[k], wantStrategies[k])
					} else {
						// Degenerate connection geometry: the
						// production path drops this strategy
						// before MIS, so its raw density must
						// not be a usable positive number.
						c.Check(!positiveFinite(gotStrategies[k]),
							"%d-vertex split strategy %d must be invalid, got %v", nv, k,
							gotStrategies[k])
					}
				}
				// MIS weights are only defined when every
				// strategy survives the geometry gates.
				if allPositiveFinite(wantStrategies) {
					for k := range wantStrategies {
						checkWeights(c, wantStrategies, wantStrategies[k], 1)
						checkWeights(c, wantStrategies, wantStrategies[k], 2)
					}
				}
			}
		}
		c.Check(splitCount >= 4, "expected 3-5 vertex splits, got %d", splitCount)

		// The root density of every strategy is re-queried
		// at its own emitting endpoint: a point that is on
		// no configured light has zero proposal density,
		// while the sampled light endpoint keeps its own.
		root := lightV[0]
		atRoot := proposal.areaDensity(root.Point, root.Normal, root.Emission)
		c.Check(atRoot > 0, "sampled light endpoint needs positive proposal density")
		foreign := model3d.XYZ(40, 40, 40)
		c.Check(proposal.areaDensity(foreign, root.Normal, root.Emission) == 0,
			"foreign endpoint must not reuse the sampled endpoint density")

		// Solid-angle Jacobian: p_w = p_A * 4*pi*r^2 /
		// |cos(theta)|. Check it against the explicit
		// geometric conversion.
		y := model3d.XYZ(2, -3, 1.5)
		x := root.Point
		theta := math.Abs(root.Normal.Dot(y.Sub(x).Normalize()))
		r2 := x.Dist(y)
		r2 *= r2
		wantSolid := atRoot * 4 * math.Pi * r2 / theta
		gotSolid := solidAngleDensity(proposal, x, root.Normal, root.Emission, y)
		c.Check(math.Abs(gotSolid-wantSolid) < 1e-9,
			"solid-angle density %v want %v", gotSolid, wantSolid)
	})
}

// syntheticStrategyPath builds a non-coplanar 4-vertex
// path (light root, two diffuse vertices, and an eye
// position) together with a guided light whose triangle
// contains the root point.
func syntheticStrategyPath(c *taskcheck.C) (*lightProposal, []*bptPathVertex,
	[]*bptPathVertex, AreaLight) {
	// Non-coplanar points (triple product verified below).
	p0 := model3d.XYZ(0, 0, 2)
	p1 := model3d.XYZ(0, 0.3, 0)
	p2 := model3d.XYZ(1, 0, 1)
	p3 := model3d.XYZ(2, 1, 2)
	v1 := p1.Sub(p0)
	v2 := p2.Sub(p0)
	v3 := p3.Sub(p0)
	c.Check(math.Abs(v1.Dot(v2.Cross(v3))) > 0.1, "test path must be non-coplanar")

	// Light triangle in the z=2 plane, wound so the normal
	// faces the path below it.
	tri := &model3d.Triangle{
		model3d.XYZ(1, -1, 2), model3d.XYZ(-1, -1, 2), model3d.XYZ(0, 1, 2),
	}
	n0 := tri.Normal()
	c.Check(n0.Dot(p1.Sub(p0)) > 0, "light normal should face the path")
	// A second, distant triangle makes the proposal
	// guide-dependent at the root.
	other := &model3d.Triangle{
		model3d.XYZ(10, 10, 2), model3d.XYZ(12, 10, 2), model3d.XYZ(10, 12, 2),
	}
	light, err := NewGuidedMeshAreaLight(
		[]*model3d.Triangle{tri, other},
		[]Color{renderColor(3, 3, 3), renderColor(1, 1, 1)},
		[]float64{0.25, 4},
	)
	c.Check(err == nil, "strategy light constructor: %v", err)
	proposal := newLightProposal(light)
	c.Check(!proposal.legacy, "guided light must use area densities")

	lambert := &LambertMaterial{DiffuseColor: NewColor(0.5)}
	mk := func(point, normal model3d.Coord3D, source, dest model3d.Coord3D,
		mat Material, emission Color) *bptPathVertex {
		v := &bptPathVertex{
			Point:         point,
			Normal:        normal,
			Source:        source,
			Dest:          dest,
			Material:      mat,
			Emission:      emission,
			RouletteScale: 1.0,
		}
		v.EvalMaterial()
		return v
	}

	root := mk(p0, n0, n0.Scale(-1), p1.Sub(p0).Normalize(), nil, renderColor(3, 3, 3))
	// Source follows photon propagation (light to eye), so
	// normals are chosen as the half-vector between source
	// and dest to satisfy the Lambertian hemisphere rules.
	s1, d1 := p1.Sub(p0).Normalize(), p2.Sub(p1).Normalize()
	n1 := d1.Sub(s1).Normalize()
	x1 := mk(p1, n1, s1, d1, lambert, Color{})
	s2, d2 := p2.Sub(p1).Normalize(), p3.Sub(p2).Normalize()
	n2 := d2.Sub(s2).Normalize()
	x2 := mk(p2, n2, s2, d2, lambert, Color{})
	c.Check(x1.SourceDensity > 0 && x1.DestDensity > 0, "x1 densities must be positive")
	c.Check(x2.SourceDensity > 0 && x2.DestDensity > 0, "x2 densities must be positive")

	// Emissive eye-path endpoint at a fifth, non-coplanar
	// point, so pure eye-path splits are enumerated too.
	p4 := model3d.XYZ(1.5, -1.5, 3.75)
	c.Check(math.Abs(p4.Sub(p0).Dot(v2.Cross(v3))) > 0.01, "fifth point must be non-coplanar")
	emissiveMat := &LambertMaterial{
		DiffuseColor:  NewColor(0.4),
		EmissionColor: renderColor(2, 2, 2),
	}
	s4 := p3.Sub(p4).Normalize()
	e1 := mk(p4, model3d.XYZ(0, 0, 1).Sub(s4).Normalize(), s4, model3d.XYZ(0, 0, 1),
		emissiveMat, renderColor(2, 2, 2))
	c.Check(e1.SourceDensity > 0, "eye endpoint density must be positive")

	lightV := []*bptPathVertex{root, x1, x2}
	eyeV := []*bptPathVertex{x2, e1}
	return proposal, lightV, eyeV, light
}

// referenceStrategyDensities independently enumerates the
// directional density of every strategy that could produce
// the joined path P, whose first vertex is the light
// endpoint. The formulas follow the solid-angle Jacobian
// documented on AreaDensityLight.
func referenceStrategyDensities(P []*bptPathVertex, proposal *lightProposal) []float64 {
	var out []float64

	// Pure backward strategy: only material source
	// densities, no light proposal involved.
	backward := 1.0
	for i := 1; i < len(P); i++ {
		backward *= P[i].SourceDensity
	}
	out = append(out, backward)

	root := P[0]
	lightDensity := proposal.areaDensity(root.Point, root.Normal, root.Emission)
	if !(lightDensity > 0) {
		return out
	}
	r2 := func(i int) float64 {
		d := P[i].Point.Sub(P[i-1].Point)
		return 4 * math.Pi * d.Dot(d)
	}
	// Strategy with one light vertex (direct connection
	// from the sampled endpoint). P[1] is the connection
	// vertex, so no source density is consumed there.
	out = append(out, lightDensity*
		productSourceDensities(P[2:])*r2(1)/P[0].DestDot())

	// Longer light-starting strategies.
	d := lightDensity
	for i := 1; i+1 < len(P); i++ {
		d *= P[i-1].DestDensity
		d *= P[i].SourceDot() / P[i-1].DestDot()
		out = append(out, d*productSourceDensities(P[i+2:])*r2(i+1)/P[i].DestDot())
	}
	return out
}

func positiveFinite(x float64) bool {
	return x > 0 && !math.IsNaN(x) && !math.IsInf(x, 0)
}

func allPositiveFinite(xs []float64) bool {
	for _, x := range xs {
		if !positiveFinite(x) {
			return false
		}
	}
	return true
}

func productSourceDensities(vs []*bptPathVertex) float64 {
	p := 1.0
	for _, v := range vs {
		p *= v.SourceDensity
	}
	return p
}

// solidAngleDensity converts an area proposal density to
// the library's unit-sphere directional convention.
func solidAngleDensity(proposal *lightProposal, x, normal model3d.Coord3D,
	emission Color, y model3d.Coord3D) float64 {
	pA := proposal.areaDensity(x, normal, emission)
	dir := y.Sub(x)
	r2 := dir.Dot(dir)
	cos := math.Abs(normal.Dot(dir.Normalize()))
	return pA * 4 * math.Pi * r2 / cos
}

// checkWeights verifies MIS weights for a strategy set.
func checkWeights(c *taskcheck.C, strategies []float64, self float64, power float64) {
	denom := 0.0
	for _, d := range strategies {
		if d <= 0 {
			continue
		}
		if power == 1 {
			denom += d
		} else {
			denom += math.Pow(d, power)
		}
	}
	c.Check(denom > 0, "MIS denominator must be positive")
	if denom == 0 {
		return
	}
	var selfWeight float64
	if power == 1 {
		selfWeight = self / denom
	} else {
		selfWeight = math.Pow(self, power) / denom
	}
	c.Check(selfWeight >= 0 && selfWeight <= 1+1e-12, "MIS weight out of range: %v",
		selfWeight)

	// Summing every strategy's weight for the same path
	// yields one.
	sum := 0.0
	for _, d := range strategies {
		if d <= 0 {
			continue
		}
		if power == 1 {
			sum += d / denom
		} else {
			sum += math.Pow(d, power) / denom
		}
	}
	c.Check(math.Abs(sum-1) < 1e-9, "MIS weights must sum to one, got %v", sum)
}

// aggregateChecks covers joined lights, legacy mixing,
// black sub-lights, and selection-probability accounting.
func aggregateChecks(c *taskcheck.C, lightV []*bptPathVertex, guided AreaLight) {
	legacySphere := NewSphereAreaLight(&model3d.Sphere{
		Center: model3d.XYZ(-20, -20, -20), Radius: 1,
	}, NewColor(5))
	_, isDensity := interface{}(legacySphere).(AreaDensityLight)
	c.Check(!isDensity, "sphere light must remain a legacy AreaLight")

	// A mix with a legacy light cannot expose area
	// densities and uses the legacy proposal instead.
	mixed := JoinAreaLights(guided, legacySphere)
	_, mixedDensity := mixed.(AreaDensityLight)
	c.Check(!mixedDensity, "mixed legacy/new join must fall back to the legacy protocol")
	mixedProposal := newLightProposal(mixed)
	c.Check(mixedProposal.legacy, "mixed join proposal must be legacy")

	// All-density joins evaluate densities including the
	// actual sub-light selection probability.
	guided2, err := NewGuidedMeshAreaLight(
		[]*model3d.Triangle{{
			model3d.XYZ(-30, 0, 0), model3d.XYZ(-29, 0, 0), model3d.XYZ(-30, 1, 0),
		}},
		[]Color{renderColor(2, 2, 2)},
		[]float64{1},
	)
	c.Check(err == nil, "second guided light: %v", err)
	fullJoin := JoinAreaLights(guided, guided2)
	dj, ok := fullJoin.(AreaDensityLight)
	c.Check(ok, "all-density join must implement AreaDensityLight")
	root := lightV[0]
	subDensity := guided.(AreaDensityLight).LightDensity(root.Point, root.Normal)
	selectionProb := guided.TotalEmission() / fullJoin.TotalEmission()
	want := selectionProb * subDensity
	c.Check(math.Abs(dj.LightDensity(root.Point, root.Normal)-want) < 1e-10,
		"join density %v want %v", dj.LightDensity(root.Point, root.Normal), want)

	// Black sub-lights never participate in selection.
	blackLight, err := NewGuidedMeshAreaLight(
		[]*model3d.Triangle{{
			model3d.XYZ(50, 0, 0), model3d.XYZ(51, 0, 0), model3d.XYZ(50, 1, 0),
		}},
		[]Color{renderColor(0, 0, 0)}, []float64{1},
	)
	c.Check(err == nil, "black guided light: %v", err)
	withBlack := JoinAreaLights(guided, blackLight)
	c.Check(withBlack.TotalEmission() == guided.TotalEmission(),
		"black sub-light must not change total emission")
	rng := rand.New(rand.NewSource(13))
	for i := 0; i < 500; i++ {
		_, _, e := withBlack.SampleLight(rng)
		c.Check(e.Sum() > 0, "black sub-light must never be selected")
	}

	// An all-black aggregate stays finite and never
	// produces NaNs or out-of-bounds behavior.
	allBlack := JoinAreaLights(blackLight)
	c.Check(allBlack.TotalEmission() == 0, "all-black join has zero emission")
	p, _, e := allBlack.SampleLight(rand.New(rand.NewSource(14)))
	c.Check(math.IsNaN(p.Sum()) == false && math.IsNaN(e.Sum()) == false,
		"all-black join sampling must stay finite")
	if bd, ok := allBlack.(AreaDensityLight); ok {
		c.Check(bd.LightDensity(root.Point, root.Normal) == 0,
			"all-black join density must be zero")
	}
}

func TestGuidedRender(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Render 1: mostly direct lighting. Two extreme
		// guide sets must converge to the same linear
		// radiance despite opposite proposals (balance).
		directScene, directLight, cam := guidedBoxScene()
		energyA, samplesA, errA := renderGuidedEnergy(c, cam, directScene, directLight, 4000, 0, 0)
		energyB, samplesB, errB := renderGuidedEnergy(c, cam, directScene, directLight, 4000, 1, 0)
		c.Check(errA == nil && errB == nil, "direct renders failed")
		directRel := math.Abs(energyA-energyB) / math.Max(energyA, energyB)
		t.Logf("DIRECT samples=%d/%d energyA=%.6f energyB=%.6f relErr=%.4f",
			samplesA, samplesB, energyA, energyB, directRel)
		c.Check(directRel < 0.08, "guide changes must not shift direct radiance: %v", directRel)
		c.Check(samplesA > 0 && samplesB > 0, "renders must consume samples")
		c.Check(math.IsNaN(energyA) == false && math.IsNaN(energyB) == false,
			"energies must be finite")

		// Balance and power-2 heuristics both produce
		// finite, comparable results on the same proposal.
		energyBalance, _, _ := renderGuidedEnergy(c, cam, directScene, directLight, 4000, 0, 0)
		energyPower, _, _ := renderGuidedEnergy(c, cam, directScene, directLight, 4000, 0, 2)
		powerRel := math.Abs(energyBalance-energyPower) / math.Max(energyBalance, energyPower)
		t.Logf("HEURISTIC samples=%d balance=%.6f power2=%.6f relErr=%.4f",
			samplesA, energyBalance, energyPower, powerRel)
		c.Check(powerRel < 0.08, "heuristics must agree: %v", powerRel)

		// Render 2: two diffuse bounces via a blocker.
		bounceScene, bounceLight, bounceCam := guidedBounceScene()
		eA, sA, _ := renderGuidedEnergy(c, bounceCam, bounceScene, bounceLight, 12000, 0, 0)
		eB, sB, _ := renderGuidedEnergy(c, bounceCam, bounceScene, bounceLight, 12000, 1, 0)
		bounceRel := math.Abs(eA-eB) / math.Max(eA, eB)
		t.Logf("BOUNCE samples=%d/%d energyA=%.6f energyB=%.6f relErr=%.4f",
			sA, sB, eA, eB, bounceRel)
		c.Check(bounceRel < 0.12, "two-bounce radiance must be guide-independent: %v", bounceRel)

		// A fixed seed must reproduce the exact same
		// per-sample radiance for guided and legacy lights;
		// the multi-goroutine renderer schedules pixels
		// non-deterministically, so determinism is checked at
		// the single-sample rayColor boundary, where the RNG
		// stream is fully controlled.
		deterministicRayColor(c, directScene, directLight(0), cam)
		legacyScene, legacyLight := testingScene()
		deterministicRayColor(c, legacyScene, legacyLight, cam)

		// An emitter outside the configured light set still
		// contributes through pure eye paths; a zero
		// competing light density must not erase it.
		foreignEmitterChecks(c)
	})
}

// guidedCeilingLight builds the four-triangle ceiling light
// with two opposite guide profiles.
func guidedCeilingLight(mode int) (AreaLight, []*model3d.Triangle) {
	corners := [4]model3d.Coord3D{
		model3d.XYZ(-2, -2, 3), model3d.XYZ(2, -2, 3),
		model3d.XYZ(2, 2, 3), model3d.XYZ(-2, 2, 3),
	}
	center := model3d.XYZ(0, 0, 3)
	tris := make([]*model3d.Triangle, 4)
	for i := range corners {
		tris[i] = &model3d.Triangle{corners[(i+1)%4], corners[i], center}
	}
	emission := []Color{
		renderColor(8, 1, 1), renderColor(1, 8, 1),
		renderColor(1, 1, 8), renderColor(6, 6, 1),
	}
	var guide []float64
	if mode == 0 {
		guide = []float64{100, 0.01, 5, 0.2}
	} else {
		guide = []float64{0.01, 100, 0.2, 5}
	}
	light, err := NewGuidedMeshAreaLight(tris, emission, guide)
	if err != nil {
		panic(err)
	}
	return light, tris
}

func guidedBoxScene() (Object, func(int) AreaLight, *Camera) {
	ground := &ColliderObject{
		Collider: model3d.MeshToCollider(
			model3d.NewMeshRect(model3d.XYZ(-8, -8, -8), model3d.XYZ(8, 8, 0)).Scale(-1),
		),
		Material: &LambertMaterial{DiffuseColor: NewColor(0.5)},
	}
	light0, _ := guidedCeilingLight(0)
	light1, _ := guidedCeilingLight(1)
	scene := JoinedObject{ground, light0}
	cam := NewCameraAt(model3d.XYZ(0, -6, 1.5), model3d.XYZ(0, 0, 1.2), math.Pi/3)
	lightFor := func(mode int) AreaLight {
		if mode == 0 {
			return light0
		}
		return light1
	}
	return scene, lightFor, cam
}

func guidedBounceScene() (Object, func(int) AreaLight, *Camera) {
	ground := &ColliderObject{
		Collider: model3d.MeshToCollider(
			model3d.NewMeshRect(model3d.XYZ(-8, -8, -8), model3d.XYZ(8, 8, 0)).Scale(-1),
		),
		Material: &LambertMaterial{DiffuseColor: NewColor(0.6)},
	}
	blocker := &ColliderObject{
		Collider: &model3d.Sphere{Center: model3d.XYZ(0, 0, 0.8), Radius: 0.8},
		Material: &LambertMaterial{DiffuseColor: NewColor(0.7)},
	}
	light0, _ := guidedCeilingLight(0)
	light1, _ := guidedCeilingLight(1)
	scene := JoinedObject{ground, blocker, light0}
	cam := NewCameraAt(model3d.XYZ(-3.5, -5, 1.2), model3d.XYZ(0, 0, 1), math.Pi/3.2)
	lightFor := func(mode int) AreaLight {
		if mode == 0 {
			return light0
		}
		return light1
	}
	return scene, lightFor, cam
}

// renderGuidedEnergy renders a small image and returns the
// summed pixel energy together with the number of samples
// consumed. guideMode selects the guide profile and power
// selects the balance (1) or power-2 (2) heuristic.
func renderGuidedEnergy(c *taskcheck.C, cam *Camera, scene Object,
	lightFor func(int) AreaLight, samples, guideMode, power int) (float64, int, error) {
	tracer := &BidirPathTracer{
		Camera:         cam,
		Light:          lightFor(guideMode),
		MaxDepth:       4,
		MaxLightDepth:  3,
		NumSamples:     samples,
		PowerHeuristic: float64(power),
	}
	img := NewImage(6, 6)
	tracer.Render(img, scene)
	total := 0.0
	nonBlack := 0
	for _, col := range img.Data {
		v := col.Sum()
		if math.IsNaN(v) || math.IsInf(v, 0) || v < 0 {
			return 0, 0, errInvalidPixel
		}
		if v > 0 {
			nonBlack++
		}
		total += v
	}
	c.Check(nonBlack > 0, "render produced no lit pixels")
	return total, samples * len(img.Data), nil
}

var errInvalidPixel = &renderError{"invalid pixel value"}

type renderError struct{ msg string }

func (e *renderError) Error() string { return e.msg }

// deterministicRayColor renders the same ray twice with an
// identically seeded per-sample RNG and requires the exact
// same color, proving that sampling directions and random
// number consumption are reproducible.
func deterministicRayColor(c *taskcheck.C, scene Object, light AreaLight, cam *Camera) {
	tracer := &BidirPathTracer{
		Camera:        cam,
		Light:         light,
		MaxDepth:      3,
		MaxLightDepth: 3,
	}
	ray := &model3d.Ray{
		Origin:    cam.Origin,
		Direction: cam.Caster(2, 2)(1, 1),
	}
	render := func() Color {
		g := &goInfo{Gen: rand.New(rand.NewSource(424242))}
		return tracer.rayColor(g, scene, ray)
	}
	first := render()
	second := render()
	c.Check(first == second, "fixed-seed ray color changed: %v vs %v", first, second)
}

// foreignEmitterChecks places a bright emissive object in
// the scene but not in the BidirPathTracer's light set, and
// verifies pure eye paths still carry its radiance.
func foreignEmitterChecks(c *taskcheck.C) {
	foreign := &ColliderObject{
		Collider: &model3d.Sphere{Center: model3d.XYZ(2.5, -2, 1.2), Radius: 0.5},
		Material: &LambertMaterial{EmissionColor: renderColor(20, 20, 20)},
	}
	guided, _ := guidedCeilingLight(0)
	ground := &ColliderObject{
		Collider: model3d.MeshToCollider(
			model3d.NewMeshRect(model3d.XYZ(-8, -8, -8), model3d.XYZ(8, 8, 0)).Scale(-1),
		),
		Material: &LambertMaterial{DiffuseColor: NewColor(0.5)},
	}
	scene := JoinedObject{ground, foreign, guided}
	cam := NewCameraAt(model3d.XYZ(0, -5, 1.4), model3d.XYZ(1.5, -1.5, 1.2), math.Pi/3)

	// Direct ray-color call: the ray hits the foreign
	// emitter first, so its eye-path contribution must
	// survive even though the light proposal gives the hit
	// point zero density.
	tracer := &BidirPathTracer{
		Camera:        cam,
		Light:         guided,
		MaxDepth:      2,
		MaxLightDepth: 2,
		NumSamples:    1,
	}
	g := &goInfo{Gen: rand.New(rand.NewSource(15))}
	ray := &model3d.Ray{
		Origin:    cam.Origin,
		Direction: foreign.Max().Mid(foreign.Min()).Sub(cam.Origin).Normalize(),
	}
	col := tracer.rayColor(g, scene, ray)
	c.Check(col.Sum() > 0, "pure eye-path hit on foreign emitter must contribute, got %v", col)
	c.Check(!math.IsNaN(col.Sum()) && !math.IsInf(col.Sum(), 0),
		"foreign emitter contribution must be finite")
	if dl, ok := guided.(AreaDensityLight); ok {
		hit := foreign.Max().Mid(foreign.Min())
		c.Check(dl.LightDensity(hit, model3d.Z(1)) == 0,
			"foreign surface must have zero competing proposal density")
	}

	// A full render stays finite and non-black.
	img := NewImage(5, 5)
	tracer.NumSamples = 800
	tracer.Render(img, scene)
	total := 0.0
	for _, p := range img.Data {
		c.Check(!math.IsNaN(p.Sum()) && !math.IsInf(p.Sum(), 0), "pixel must be finite")
		total += p.Sum()
	}
	c.Check(total > 0, "scene with foreign emitter must render non-black")

	// All-black configured light: the foreign emitter is
	// the only source and eye paths must still light the
	// image, without any NaNs.
	blackTri := &model3d.Triangle{
		model3d.XYZ(-2, -2, 3), model3d.XYZ(-1, -2, 3), model3d.XYZ(-2, -1, 3),
	}
	blackLight, err := NewGuidedMeshAreaLight(
		[]*model3d.Triangle{blackTri},
		[]Color{renderColor(0, 0, 0)}, []float64{1},
	)
	c.Check(err == nil, "black configured light: %v", err)
	blackJoin := JoinAreaLights(blackLight)
	tracer.Light = blackJoin
	tracer.NumSamples = 400
	blackImg := NewImage(4, 4)
	tracer.Render(blackImg, JoinedObject{ground, foreign, blackLight})
	blackTotal := 0.0
	for _, p := range blackImg.Data {
		c.Check(!math.IsNaN(p.Sum()) && !math.IsInf(p.Sum(), 0), "black-light pixel must be finite")
		blackTotal += p.Sum()
	}
	c.Check(blackTotal > 0, "eye-only emitter must survive a black light configuration")
}
