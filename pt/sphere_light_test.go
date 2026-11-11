package pt

import (
	"math"
	"math/rand"
	"testing"
)

// sphereLightScene builds a scene with a single white unit sphere light.
// The caller must Compile the scene after adding any further shapes.
func sphereLightScene(center Vector, radius float64) (*Scene, *Sphere) {
	light := NewSphere(center, radius, LightMaterial(White, 1)).(*Sphere)
	scene := &Scene{}
	scene.Add(light)
	return scene, light
}

// stratifiedEstimate averages the Lambertian direct-light estimator over a
// deterministic n x n grid of (u, v) samples.
func stratifiedEstimate(scene *Scene, origin, normal Vector, light *Sphere, n int) Color {
	var sum Color
	for i := 0; i < n; i++ {
		for j := 0; j < n; j++ {
			u := (float64(i) + 0.5) / float64(n)
			v := (float64(j) + 0.5) / float64(n)
			direction, _, pdf, ok := SampleSphereLight(origin, light, u, v)
			if !ok {
				continue
			}
			diffuse := direction.Dot(normal)
			if diffuse <= 0 {
				continue
			}
			ray := Ray{origin, direction}
			hit := scene.Intersect(ray)
			if !hit.Ok() || hit.Shape != light {
				continue
			}
			material := MaterialAt(light, ray.Position(hit.T))
			m := material.Emittance * diffuse / (math.Pi * pdf)
			sum = sum.Add(material.Color.MulScalar(m))
		}
	}
	return sum.DivScalar(float64(n * n))
}

// seededEstimate averages sampler.sampleLight over count samples drawn from
// a fixed-seed rnd.
func seededEstimate(sampler *DefaultSampler, scene *Scene, origin, normal Vector, light Shape, seed int64, count int) Color {
	rnd := rand.New(rand.NewSource(seed))
	n := Ray{origin, normal}
	var sum Color
	for i := 0; i < count; i++ {
		sum = sum.Add(sampler.sampleLight(scene, n, rnd, light))
	}
	return sum.DivScalar(float64(count))
}

func TestSphereLightTarget(t *testing.T) {
	t.Run("Geometry", func(t *testing.T) {
		_, light := sphereLightScene(Vector{0, 0, 5}, 1)
		origin := Vector{}
		w := Vector{0, 0, 1}
		cosThetaMax := math.Sqrt(1-1.0/25.0)

		// u = 0 points at the sphere center
		d, dist, _, ok := SampleSphereLight(origin, light, 0, 0.3)
		if !ok {
			t.Fatal("expected ok")
		}
		if d.Sub(w).Length() > 1e-12 {
			t.Fatal("u=0 should point at sphere center", d)
		}
		if math.Abs(dist-4) > 1e-9 {
			t.Fatal("u=0 distance should be 4", dist)
		}

		// grid of samples: unit directions inside the cone, rays hit the
		// sphere at the reported distance, pdf matches the cone solid angle
		wantPdf := 1 / (2 * math.Pi * (1 - cosThetaMax))
		for i := 0; i < 32; i++ {
			for j := 0; j < 32; j++ {
				u := (float64(i) + 0.5) / 32
				v := (float64(j) + 0.5) / 32
				d, dist, pdf, ok := SampleSphereLight(origin, light, u, v)
				if !ok {
					t.Fatal("expected ok", u, v)
				}
				if math.Abs(d.Length()-1) > 1e-9 {
					t.Fatal("direction not normalized", d.Length())
				}
				if d.Dot(w) < cosThetaMax-1e-12 {
					t.Fatal("direction outside cone", u, v)
				}
				if math.Abs(pdf-wantPdf) > 1e-9*wantPdf {
					t.Fatal("pdf mismatch", pdf, wantPdf)
				}
				hit := light.Intersect(Ray{origin, d})
				if !hit.Ok() {
					t.Fatal("sampled ray misses the sphere", u, v)
				}
				if math.Abs(hit.T-dist) > 1e-6 {
					t.Fatal("distance mismatch", hit.T, dist)
				}
			}
		}

		// independent pdf check: Monte Carlo fraction of the sphere of
		// directions covered by the cone
		rnd := rand.New(rand.NewSource(11))
		const total = 2000000
		inside := 0
		for i := 0; i < total; i++ {
			if RandomUnitVector(rnd).Dot(w) >= cosThetaMax {
				inside++
			}
		}
		mcPdf := 1 / (4 * math.Pi * float64(inside) / float64(total))
		if math.Abs(mcPdf-wantPdf)/wantPdf > 0.02 {
			t.Fatal("Monte Carlo pdf check failed", mcPdf, wantPdf)
		}
	})

	t.Run("LambertianReference", func(t *testing.T) {
		scene, light := sphereLightScene(Vector{0, 0, 5}, 1)
		scene.Compile()
		normal := Vector{0, 0, 1}

		// stratified estimate of the 0.04 reference
		c := stratifiedEstimate(scene, Vector{}, normal, light, 128)
		if math.Abs(c.R-0.04) > 2e-4 || math.Abs(c.G-0.04) > 2e-4 || math.Abs(c.B-0.04) > 2e-4 {
			t.Fatal("stratified estimate off", c)
		}

		// same value through the sampler with a fixed seed
		sampler := NewDirectSampler()
		sampler.SoftShadows = true
		c = seededEstimate(sampler, scene, Vector{}, normal, light, 7, 20000)
		if math.Abs(c.R-0.04) > 2e-4 || math.Abs(c.G-0.04) > 2e-4 || math.Abs(c.B-0.04) > 2e-4 {
			t.Fatal("sampler estimate off", c)
		}
	})

	t.Run("RotatedScene", func(t *testing.T) {
		// same geometry rotated away from the axes: center (3,0,4),
		// distance 5, normal pointing at the light
		scene, light := sphereLightScene(Vector{3, 0, 4}, 1)
		scene.Compile()
		normal := Vector{0.6, 0, 0.8}

		c := stratifiedEstimate(scene, Vector{}, normal, light, 128)
		if math.Abs(c.R-0.04) > 2e-4 || math.Abs(c.G-0.04) > 2e-4 || math.Abs(c.B-0.04) > 2e-4 {
			t.Fatal("rotated stratified estimate off", c)
		}

		sampler := NewDirectSampler()
		sampler.SoftShadows = true
		c = seededEstimate(sampler, scene, Vector{}, normal, light, 13, 20000)
		if math.Abs(c.R-0.04) > 2e-4 || math.Abs(c.G-0.04) > 2e-4 || math.Abs(c.B-0.04) > 2e-4 {
			t.Fatal("rotated sampler estimate off", c)
		}
	})

	t.Run("Occluder", func(t *testing.T) {
		scene, light := sphereLightScene(Vector{0, 0, 5}, 1)
		// blocker covers the whole light cone as seen from the origin
		scene.Add(NewSphere(Vector{0, 0, 2.5}, 1.5, DiffuseMaterial(White)))
		scene.Compile()

		sampler := NewDirectSampler()
		sampler.SoftShadows = true
		c := seededEstimate(sampler, scene, Vector{}, Vector{0, 0, 1}, light, 17, 1000)
		if c.R != 0 || c.G != 0 || c.B != 0 {
			t.Fatal("occluded light should be black", c)
		}
	})

	t.Run("TwoLights", func(t *testing.T) {
		scene, _ := sphereLightScene(Vector{0, 0, 5}, 1)
		// off-axis so it neither occludes nor is occluded by light1;
		// distance 5, normal.w = 0.8, so it contributes 0.04 * 0.8
		light2 := NewSphere(Vector{3, 0, 4}, 1, LightMaterial(White, 1)).(*Sphere)
		scene.Add(light2)
		scene.Compile()
		normal := Vector{0, 0, 1}
		// expected: 1/25 + 0.8/25 = 0.072
		sampler := NewDirectSampler()
		sampler.SoftShadows = true

		sampler.LightMode = LightModeAll
		c := seededEstimateLights(sampler, scene, Vector{}, normal, 19, 20000)
		if math.Abs(c.R-0.072) > 2e-4 {
			t.Fatal("LightModeAll estimate off", c)
		}

		sampler.LightMode = LightModeRandom
		c = seededEstimateLights(sampler, scene, Vector{}, normal, 23, 200000)
		if math.Abs(c.R-0.072) > 5e-4 {
			t.Fatal("LightModeRandom estimate off", c)
		}
	})

	t.Run("FarSmallLight", func(t *testing.T) {
		_, light := sphereLightScene(Vector{0, 0, 1e8}, 1)
		d, dist, pdf, ok := SampleSphereLight(Vector{}, light, 0.5, 0.25)
		if !ok {
			t.Fatal("expected ok")
		}
		transverse := math.Hypot(d.X, d.Y)
		if transverse == 0 {
			t.Fatal("cone collapsed onto the axis", d)
		}
		if math.Abs(d.Length()-1) > 1e-9 {
			t.Fatal("direction not normalized", d.Length())
		}
		if math.IsNaN(pdf) || math.IsInf(pdf, 0) || pdf <= 0 {
			t.Fatal("pdf not finite and positive", pdf)
		}
		// pdf ~ 1 / (pi * (r/d)^2) = 1e16 / pi
		if math.Abs(pdf-1e16/math.Pi)/(1e16/math.Pi) > 0.01 {
			t.Fatal("pdf off", pdf)
		}
		if math.Abs(dist-1e8)/1e8 > 1e-6 {
			t.Fatal("distance off", dist)
		}
		// the sampled point at the reported distance lands on the sphere
		// (Sphere.Intersect itself cannot resolve a unit sphere at 1e8)
		p := Ray{Vector{}, d}.Position(dist)
		if math.Abs(p.Sub(light.Center).Length()-1) > 1e-4 {
			t.Fatal("sampled point not on far sphere", p)
		}
	})

	t.Run("InvalidInputs", func(t *testing.T) {
		origin := Vector{}
		good := NewSphere(Vector{0, 0, 5}, 1, LightMaterial(White, 1)).(*Sphere)
		cases := []struct {
			name   string
			origin Vector
			light  *Sphere
			u, v   float64
		}{
			{"nil light", origin, nil, 0.5, 0.5},
			{"zero radius", origin, &Sphere{Center: Vector{0, 0, 5}, Radius: 0}, 0.5, 0.5},
			{"negative radius", origin, &Sphere{Center: Vector{0, 0, 5}, Radius: -1}, 0.5, 0.5},
			{"nan radius", origin, &Sphere{Center: Vector{0, 0, 5}, Radius: math.NaN()}, 0.5, 0.5},
			{"inf radius", origin, &Sphere{Center: Vector{0, 0, 5}, Radius: math.Inf(1)}, 0.5, 0.5},
			{"nan origin", Vector{math.NaN(), 0, 0}, good, 0.5, 0.5},
			{"inf origin", Vector{0, math.Inf(1), 0}, good, 0.5, 0.5},
			{"nan center", origin, &Sphere{Center: Vector{0, 0, math.NaN()}, Radius: 1}, 0.5, 0.5},
			{"inside", origin, &Sphere{Center: Vector{0, 0, 0.5}, Radius: 1}, 0.5, 0.5},
			{"on surface", origin, &Sphere{Center: Vector{0, 0, 1}, Radius: 1}, 0.5, 0.5},
			{"u negative", origin, good, -0.1, 0.5},
			{"u one", origin, good, 1, 0.5},
			{"u large", origin, good, 1.5, 0.5},
			{"u nan", origin, good, math.NaN(), 0.5},
			{"v negative", origin, good, 0.5, -0.1},
			{"v one", origin, good, 0.5, 1},
			{"v nan", origin, good, 0.5, math.NaN()},
		}
		for _, c := range cases {
			d, dist, pdf, ok := SampleSphereLight(c.origin, c.light, c.u, c.v)
			if ok || d != (Vector{}) || dist != 0 || pdf != 0 {
				t.Fatalf("%s: expected zero result and ok=false, got %v %v %v %v",
					c.name, d, dist, pdf, ok)
			}
		}
	})

	t.Run("Deterministic", func(t *testing.T) {
		run := func() Color {
			scene, _ := sphereLightScene(Vector{0, 0, 5}, 1)
			scene.Add(NewSphere(Vector{0, 0, 10}, 1, LightMaterial(White, 1)))
			scene.Compile()
			sampler := NewDirectSampler()
			sampler.SoftShadows = true
			sampler.LightMode = LightModeRandom
			return seededEstimateLights(sampler, scene, Vector{}, Vector{0, 0, 1}, 42, 1000)
		}
		a := run()
		b := run()
		if a != b {
			t.Fatal("same seed gave different results", a, b)
		}
		// perturbing the package-level rand must not change anything
		for i := 0; i < 100; i++ {
			rand.Float64()
		}
		c := run()
		if a != c {
			t.Fatal("package-level rand leaked into sampling", a, c)
		}
	})
}

// seededEstimateLights averages sampler.sampleLights (all lights, honoring
// LightMode) over count samples from a fixed-seed rnd.
func seededEstimateLights(sampler *DefaultSampler, scene *Scene, origin, normal Vector, seed int64, count int) Color {
	rnd := rand.New(rand.NewSource(seed))
	n := Ray{origin, normal}
	var sum Color
	for i := 0; i < count; i++ {
		sum = sum.Add(sampler.sampleLights(scene, n, rnd))
	}
	return sum.DivScalar(float64(count))
}
