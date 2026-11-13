package pt

import (
	"math"
	"math/rand"
	"testing"
)

// sppmTestOptions returns a valid baseline configuration.
func sppmTestOptions() SPPMOptions {
	return SPPMOptions{
		PhotonsPerIteration: 1000,
		MaxDepth:            4,
		InitialRadius:       0.5,
		Alpha:               0.7,
		Seed:                1,
	}
}

func sppmLightScene() *Scene {
	scene := &Scene{}
	scene.Add(NewSphere(V(0, 0, 3), 1, LightMaterial(White, 1)))
	scene.Add(NewPlane(V(0, 0, 0), V(0, 0, 1), DiffuseMaterial(Color{0.8, 0.8, 0.8})))
	scene.Compile()
	return scene
}

func TestPhotonTransport(t *testing.T) {
	t.Run("SphereLightPower", func(t *testing.T) {
		// A sphere light of radius r and radiance L emits 4*pi^2*r^2*L; by
		// symmetry half of that power reaches the infinite plane below. Each
		// photon carries the selected light's full power, so with a huge
		// gather radius and alpha=1, Phi per iteration must approach
		// N * 2*pi*r^2*L for a white plane.
		build := func(lights []Shape) (*Scene, Camera) {
			scene := &Scene{}
			for _, l := range lights {
				scene.Add(l)
			}
			scene.Add(NewPlane(V(0, 0, 0), V(0, 0, 1), DiffuseMaterial(White)))
			scene.Compile()
			camera := LookAt(V(10, 0, 10), V(0, 0, 0), V(0, 0, 1), 40)
			return scene, camera
		}
		fluxCheck := func(scene *Scene, camera Camera, photons int, seed int64) (SPPMPixelState, Color) {
			options := sppmTestOptions()
			options.PhotonsPerIteration = photons
			options.InitialRadius = 200
			options.Alpha = 1
			options.Seed = seed
			r, err := NewSPPM(scene, camera, 1, 1, options)
			if err != nil {
				t.Fatal(err)
			}
			if err := r.Step(); err != nil {
				t.Fatal(err)
			}
			st := r.PixelState(0, 0)
			if !st.Visible || st.M == 0 {
				t.Fatalf("no gather: %+v", st)
			}
			return st, r.Radiance(0, 0)
		}
		// single white light, radius 1 at height 3
		scene, camera := build([]Shape{NewSphere(V(0, 0, 3), 1, LightMaterial(White, 1))})
		st, radiance := fluxCheck(scene, camera, 200000, 7)
		want := 200000 * 2 * math.Pi // N * 2*pi*r^2*L per channel
		ok := true
		for _, c := range []float64{st.Phi.R, st.Phi.G, st.Phi.B} {
			if math.Abs(c-want)/want > 0.03 {
				ok = false
			}
		}
		if !ok {
			t.Fatalf("single light flux %v vs %v", st.Phi, want)
		}
		targetCheck(t, ok, "sphere light power reaches plane")

		// two lights of different size and color: total flux must add up
		l2 := Color{1, 0.5, 0.25}.MulScalar(2)
		scene2, camera2 := build([]Shape{
			NewSphere(V(0, 0, 3), 1, LightMaterial(White, 1)),
			NewSphere(V(10, -5, 4), 0.5, LightMaterial(Color{1, 0.5, 0.25}, 2)),
		})
		st2, _ := fluxCheck(scene2, camera2, 200000, 8)
		want2 := Color{
			200000 * 2 * math.Pi * (1 + 0.25*l2.R),
			200000 * 2 * math.Pi * (1 + 0.25*l2.G),
			200000 * 2 * math.Pi * (1 + 0.25*l2.B),
		}
		ok = math.Abs(st2.Phi.R-want2.R)/want2.R <= 0.03 &&
			math.Abs(st2.Phi.G-want2.G)/want2.G <= 0.03 &&
			math.Abs(st2.Phi.B-want2.B)/want2.B <= 0.03
		if !ok {
			t.Fatalf("two light flux %v vs %v", st2.Phi, want2)
		}
		targetCheck(t, ok, "mixed lights keep total energy meaning")

		// the number of photons per iteration must not change exposure:
		// Radiance = Phi/(pi R^2 N) is independent of N
		wantRadiance := 4 * math.Pi * math.Pi / (2 * math.Pi * math.Pi * 200 * 200)
		ok = math.Abs(radiance.R-wantRadiance)/wantRadiance <= 0.03
		if !ok {
			t.Fatalf("radiance %v vs %v", radiance, wantRadiance)
		}
		_, radiance2 := fluxCheck(scene, camera, 50000, 9)
		ok = ok && math.Abs(radiance2.R-wantRadiance)/wantRadiance <= 0.03
		if !ok {
			t.Fatalf("photon count changed exposure: %v vs %v", radiance2, wantRadiance)
		}
		targetCheck(t, ok, "photon count does not change exposure")
	})

	t.Run("MirrorGather", func(t *testing.T) {
		// The visible point sits on the bottom of a diffuse sphere placed
		// off the light axis. Light cannot reach it directly; only photons
		// reflected by the mirror plane below can gather there.
		scene := &Scene{}
		scene.Add(NewSphere(V(0, 0, 8), 1, LightMaterial(White, 1)))
		scene.Add(NewSphere(V(3, 0, 4), 1, DiffuseMaterial(White)))
		scene.Add(NewPlane(V(0, 0, 0), V(0, 0, 1), MetallicMaterial(White, 0, 1)))
		scene.Compile()
		camera := LookAt(V(3, 0, 1.2), V(3, 0, 4), V(0, 1, 0), 30)
		options := sppmTestOptions()
		options.PhotonsPerIteration = 100000
		options.InitialRadius = 0.5
		options.Seed = 11
		r, err := NewSPPM(scene, camera, 3, 3, options)
		if err != nil {
			t.Fatal(err)
		}
		for i := 0; i < 3; i++ {
			if err := r.Step(); err != nil {
				t.Fatal(err)
			}
		}
		st := r.PixelState(1, 1)
		if !st.Visible {
			t.Fatalf("center pixel not visible: %+v", st)
		}
		if st.Normal.Z > -0.9 {
			t.Fatalf("expected downward normal: %+v", st.Normal)
		}
		ok := st.M > 0 && st.Phi.R > 0
		if !ok {
			t.Fatalf("no photons gathered behind mirror: %+v", st)
		}
		targetCheck(t, ok, "photons gather after ideal mirror reflection")
		got := r.Radiance(1, 1)
		targetCheck(t, got.R > 0 && got.G > 0 && got.B > 0, "mirror radiance positive")
	})

	t.Run("GlassGather", func(t *testing.T) {
		// The light is fully enclosed in a clear glass sphere, so photons
		// must refract through the glass before gathering on the floor.
		scene := &Scene{}
		scene.Add(NewSphere(V(0, 0, 2), 0.4, LightMaterial(White, 2)))
		scene.Add(NewSphere(V(0, 0, 2), 1, ClearMaterial(1.5, 0)))
		scene.Add(NewPlane(V(0, 0, 0), V(0, 0, 1), DiffuseMaterial(White)))
		scene.Compile()
		camera := LookAt(V(0, 3, 6), V(0, 0, 0.5), V(0, 0, 1), 30)
		options := sppmTestOptions()
		options.PhotonsPerIteration = 100000
		options.MaxDepth = 8
		options.InitialRadius = 0.5
		options.Seed = 13
		r, err := NewSPPM(scene, camera, 3, 3, options)
		if err != nil {
			t.Fatal(err)
		}
		for i := 0; i < 3; i++ {
			if err := r.Step(); err != nil {
				t.Fatal(err)
			}
		}
		st := r.PixelState(1, 1)
		if !st.Visible {
			t.Fatalf("center pixel not visible: %+v", st)
		}
		ok := st.M > 0 && st.Phi.R > 0
		if !ok {
			t.Fatalf("no photons gathered through glass: %+v", st)
		}
		targetCheck(t, ok, "photons gather after refracting through glass")
		got := r.Radiance(1, 1)
		finite := !math.IsNaN(got.R) && !math.IsInf(got.R, 0) && got.R > 0
		targetCheck(t, finite, "glass radiance finite and positive")
	})

	t.Run("AnalyticPlane", func(t *testing.T) {
		// White unit-radiance sphere light, radius 1, center 3 above the
		// plane origin. The exit radiance at the origin with albedo 0.8 is
		// 0.8/9. The center pixel samples within 1e-3 of the origin.
		seeds := []int64{101, 202, 303, 404}
		var sum, ref float64
		var samples uint64
		ref = 0.8 / 9
		for _, seed := range seeds {
			scene := sppmLightScene()
			camera := LookAt(V(4, 0, 10), V(0, 0, 0), V(0, 1, 0), 0.01)
			options := sppmTestOptions()
			options.PhotonsPerIteration = 200000
			options.MaxDepth = 4
			options.InitialRadius = 0.05
			options.Alpha = 0.9
			options.Seed = seed
			r, err := NewSPPM(scene, camera, 3, 3, options)
			if err != nil {
				t.Fatal(err)
			}
			for i := 0; i < 100; i++ {
				if err := r.Step(); err != nil {
					t.Fatal(err)
				}
			}
			st := r.PixelState(1, 1)
			if !st.Visible {
				t.Fatalf("center pixel not visible: %+v", st)
			}
			d := st.Position.Sub(V(0, 0, 0)).Length()
			if d > 1e-3 {
				t.Fatalf("camera sample too far from origin: %v", d)
			}
			got := r.Radiance(1, 1)
			sum += got.R
			samples += st.M
			t.Logf("seed %d: radiance=%.6f M=%d emitted=%d", seed, got.R, st.M, r.Stats().Emitted)
		}
		mean := sum / float64(len(seeds))
		rel := math.Abs(mean-ref) / ref
		t.Logf("analytic plane: got %.6f want %.6f rel %.4f samples %d", mean, ref, rel, samples)
		ok := rel <= 0.08
		if !ok {
			t.Fatalf("analytic radiance %v vs %v (rel %v)", mean, ref, rel)
		}
		targetCheck(t, ok, "unoccluded plane matches analytic 0.8/9")
	})
}

func TestPhotonGather(t *testing.T) {
	t.Run("IndexMatchesExhaustive", func(t *testing.T) {
		z := V(0, 0, 1)
		points := []sppmGatherPoint{
			{position: V(0, 0, 0), normal: z, radius: 2, pixel: 0},
			{position: V(-5, -5, -5), normal: z, radius: 1, pixel: 1},
			{position: V(0.5, 0, 0), normal: z, radius: 0.1, pixel: 2},
			{position: V(0, 0, 0), normal: z, radius: 2, pixel: 3}, // coincident
			{position: V(0, 0, 0), normal: z.Negate(), radius: 2, pixel: 4},
			{position: V(3, 0, 0), normal: z, radius: 1.5, pixel: 5},
			{position: V(4, 0, 0), normal: z, radius: 2, pixel: 6},
			{position: V(10, 10, 10), normal: z, radius: 0.5, pixel: 7},
		}
		index := newSPPMIndex(points)
		brute := func(x, n, dir Vector) map[int]bool {
			got := map[int]bool{}
			for i := range points {
				if sppmGatherAccept(&points[i], x, n, dir) {
					got[i] = true
				}
			}
			return got
		}
		queries := []struct {
			x, n, dir Vector
		}{
			{V(0, 0, 0), z, z.Negate()},
			{V(0.5, 0, 0), z, z.Negate()},
			{V(2, 0, 0), z, z.Negate()}, // exactly on radius of point 0
			{V(2.0000001, 0, 0), z, z.Negate()},
			{V(-5, -5, -5), z, z.Negate()}, // negative coordinates
			{V(-4.5, -5, -5), z, z.Negate()},
			{V(10, 10, 10), z, z.Negate()},
			{V(100, 100, 100), z, z.Negate()},
			{V(0, 0, 0), z.Negate(), z}, // back face: hemisphere and normal rules
			{V(1, 1, 0), z, V(0, 0, -1)},
		}
		rnd := rand.New(rand.NewSource(1))
		for i := 0; i < 200; i++ {
			x := V(rnd.Float64()*16-8, rnd.Float64()*16-8, rnd.Float64()*16-8)
			n := RandomUnitVector(rnd)
			d := RandomUnitVector(rnd)
			queries = append(queries, struct{ x, n, dir Vector }{x, n, d})
		}
		checked := 0
		for qi, q := range queries {
			want := brute(q.x, q.n, q.dir)
			seen := map[int]int{}
			index.visitCandidates(q.x, func(i int) {
				seen[i]++
				if !sppmGatherAccept(&index.points[i], q.x, q.n, q.dir) {
					delete(seen, i)
				}
			})
			for i, count := range seen {
				if count != 1 {
					t.Fatalf("query %d: point %d reported %d times", qi, i, count)
				}
			}
			got := map[int]bool{}
			for i := range seen {
				got[i] = true
			}
			if len(got) != len(want) {
				t.Fatalf("query %d at %v: got %v want %v", qi, q.x, got, want)
			}
			for i := range want {
				if !got[i] {
					t.Fatalf("query %d at %v: missing %d (got %v want %v)", qi, q.x, i, got, want)
				}
			}
			checked++
		}
		targetCheck(t, checked == len(queries), "index matches exhaustive distance reference")

		// explicit kernel expectations
		accepts := func(x, n, dir Vector) map[int]bool { return brute(x, n, dir) }
		a := accepts(V(2, 0, 0), z, z.Negate())
		ok := a[0] && a[3] && a[5] && a[6] && !a[2] && !a[4]
		targetCheck(t, ok, "boundary distance and coincident points accepted")
		b := accepts(V(0, 0, 0), z, z.Negate())
		ok = b[0] && b[3] && !b[4] // opposite normal rejected
		targetCheck(t, ok, "normal disagreement rejected")
		c := accepts(V(0, 0, 0), z.Negate(), z)
		ok = c[4] && !c[0] // light from behind the visible normal rejected
		targetCheck(t, ok, "hemisphere rule applied")
	})

	t.Run("BoundedCandidateChecks", func(t *testing.T) {
		// 4096 visible points spaced far beyond 4x the maximum radius; a
		// query next to each point must stay well below 16 checks on average.
		const n = 16
		const spacing = 8.0
		const maxR = 0.5
		rnd := rand.New(rand.NewSource(2))
		var points []sppmGatherPoint
		for i := 0; i < n; i++ {
			for j := 0; j < n; j++ {
				for k := 0; k < n; k++ {
					points = append(points, sppmGatherPoint{
						position: V(float64(i-n/2)*spacing, float64(j-n/2)*spacing, float64(k-n/2)*spacing),
						normal:   V(0, 0, 1),
						radius:   0.05 + rnd.Float64()*(maxR-0.05),
						pixel:    len(points),
					})
				}
			}
		}
		if len(points) != 4096 {
			t.Fatalf("points: %d", len(points))
		}
		index := newSPPMIndex(points)
		var checks uint64
		for _, p := range points {
			x := p.position.Add(V(rnd.Float64()*0.4-0.2, rnd.Float64()*0.4-0.2, rnd.Float64()*0.4-0.2))
			index.visitCandidates(x, func(i int) {
				checks++
				d := index.points[i].position.Sub(x)
				_ = d.Dot(d) <= index.points[i].radius*index.points[i].radius
			})
		}
		limit := uint64(16 * 4096)
		t.Logf("candidate checks: %d (limit %d)", checks, limit)
		ok := checks <= limit
		if !ok {
			t.Fatalf("too many distance checks: %d > %d", checks, limit)
		}
		targetCheck(t, ok, "candidate checks bounded by index selectivity")
	})
}

func TestPhotonProgressive(t *testing.T) {
	t.Run("InitialStateAndValidation", func(t *testing.T) {
		scene := sppmLightScene()
		camera := LookAt(V(4, 0, 10), V(0, 0, 0), V(0, 1, 0), 30)
		good := sppmTestOptions()
		bads := []SPPMOptions{}
		b := good
		b.PhotonsPerIteration = 0
		bads = append(bads, b)
		b = good
		b.MaxDepth = 0
		bads = append(bads, b)
		b = good
		b.InitialRadius = 0
		bads = append(bads, b)
		b = good
		b.InitialRadius = -1
		bads = append(bads, b)
		b = good
		b.InitialRadius = math.NaN()
		bads = append(bads, b)
		b = good
		b.InitialRadius = math.Inf(1)
		bads = append(bads, b)
		b = good
		b.Alpha = 0
		bads = append(bads, b)
		b = good
		b.Alpha = 1.5
		bads = append(bads, b)
		b = good
		b.Alpha = math.NaN()
		bads = append(bads, b)
		for i, options := range bads {
			if _, err := NewSPPM(scene, camera, 4, 4, options); err == nil {
				t.Fatalf("bad options %d accepted: %+v", i, options)
			}
		}
		if _, err := NewSPPM(scene, camera, 0, 4, good); err == nil {
			t.Fatal("zero width accepted")
		}
		if _, err := NewSPPM(scene, camera, 4, 0, good); err == nil {
			t.Fatal("zero height accepted")
		}
		empty := &Scene{}
		empty.Add(NewPlane(V(0, 0, 0), V(0, 0, 1), DiffuseMaterial(White)))
		empty.Compile()
		if _, err := NewSPPM(empty, camera, 4, 4, good); err == nil {
			t.Fatal("scene without sphere light accepted")
		}
		targetCheck(t, true, "invalid configurations rejected")

		r, err := NewSPPM(scene, camera, 4, 3, good)
		if err != nil {
			t.Fatal(err)
		}
		st := r.PixelState(2, 1)
		ok := st.Radius == good.InitialRadius && st.N == 0 && st.M == 0 &&
			st.Tau == Black && st.Phi == Black && st.EyeBeta == Black && !st.Visible
		if !ok {
			t.Fatalf("initial state: %+v", st)
		}
		stats := r.Stats()
		ok = ok && stats.Iterations == 0 && stats.Emitted == 0 && stats.CandidateChecks == 0
		ok = ok && r.Radiance(2, 1) == Black
		if !ok {
			t.Fatalf("initial counters: %+v radiance %v", stats, r.Radiance(2, 1))
		}
		targetCheck(t, ok, "initial pixel state and counters are zero")
	})

	t.Run("Recurrence", func(t *testing.T) {
		scene := sppmLightScene()
		camera := LookAt(V(4, 0, 10), V(0, 0, 0), V(0, 1, 0), 30)
		options := sppmTestOptions()
		options.PhotonsPerIteration = 20000
		options.InitialRadius = 0.5
		options.Alpha = 0.7
		options.Seed = 21
		const w, h = 4, 3
		r, err := NewSPPM(scene, camera, w, h, options)
		if err != nil {
			t.Fatal(err)
		}
		prev := make([]SPPMPixelState, w*h)
		for i := range prev {
			prev[i] = SPPMPixelState{Radius: options.InitialRadius}
		}
		accepted := 0
		visible := 0
		for step := 0; step < 3; step++ {
			if err := r.Step(); err != nil {
				t.Fatal(err)
			}
			for y := 0; y < h; y++ {
				for x := 0; x < w; x++ {
					i := y*w + x
					cur := r.PixelState(x, y)
					p := prev[i]
					if cur.Visible {
						visible++
					}
					if cur.Visible && cur.M > 0 {
						accepted++
						m := float64(cur.M)
						n1 := p.N + options.Alpha*m
						r1 := p.Radius * math.Sqrt(n1/(p.N+m))
						scale := r1 / p.Radius
						scale *= scale
						tau := p.Tau.Add(cur.EyeBeta.Mul(cur.Phi)).MulScalar(scale)
						if cur.N != n1 || cur.Radius != r1 || cur.Tau != tau {
							t.Fatalf("step %d pixel (%d,%d): got N=%v R=%v Tau=%v want N=%v R=%v Tau=%v",
								step, x, y, cur.N, cur.Radius, cur.Tau, n1, r1, tau)
						}
					} else {
						if cur.N != p.N || cur.Radius != p.Radius || cur.Tau != p.Tau {
							t.Fatalf("step %d pixel (%d,%d): state changed without photons: %+v vs %+v",
								step, x, y, cur, p)
						}
					}
					prev[i] = cur
				}
			}
		}
		ok := accepted > 0 && visible > 0
		if !ok {
			t.Fatalf("no accepted photons in recurrence test")
		}
		targetCheck(t, ok, "progressive recurrence matches N/R/tau formulas across iterations")
	})

	t.Run("InvisibleAndEmptyRounds", func(t *testing.T) {
		// pixel looking straight at the light: direct contribution only
		scene := &Scene{}
		scene.Add(NewSphere(V(0, 0, 0), 1, LightMaterial(Color{1, 0.5, 0.25}, 2)))
		scene.Compile()
		camera := LookAt(V(0, 0, 5), V(0, 0, 0), V(0, 1, 0), 40)
		options := sppmTestOptions()
		options.Seed = 31
		r, err := NewSPPM(scene, camera, 1, 1, options)
		if err != nil {
			t.Fatal(err)
		}
		for i := 0; i < 3; i++ {
			if err := r.Step(); err != nil {
				t.Fatal(err)
			}
		}
		st := r.PixelState(0, 0)
		ok := !st.Visible && st.N == 0 && st.Radius == options.InitialRadius && st.Tau == Black
		if !ok {
			t.Fatalf("light pixel state: %+v", st)
		}
		want := Color{1, 0.5, 0.25}.MulScalar(2)
		ok = ok && r.Radiance(0, 0) == want
		if !ok {
			t.Fatalf("direct light radiance: %v vs %v", r.Radiance(0, 0), want)
		}
		targetCheck(t, ok, "invisible pixel keeps history and averages direct light")

		// visible pixel whose photons are all absorbed: M stays zero
		scene2 := &Scene{}
		scene2.Add(NewSphere(V(0, 0, 2), 0.5, LightMaterial(White, 1)))
		scene2.Add(NewSphere(V(0, 0, 2), 1, DiffuseMaterial(Black)))
		scene2.Add(NewPlane(V(0, 0, 0), V(0, 0, 1), DiffuseMaterial(White)))
		scene2.Compile()
		camera2 := LookAt(V(5, 0, 5), V(5, 0, 0), V(0, 1, 0), 40)
		r2, err := NewSPPM(scene2, camera2, 1, 1, options)
		if err != nil {
			t.Fatal(err)
		}
		for i := 0; i < 2; i++ {
			if err := r2.Step(); err != nil {
				t.Fatal(err)
			}
		}
		st = r2.PixelState(0, 0)
		ok = st.Visible && st.M == 0 && st.N == 0 &&
			st.Radius == options.InitialRadius && st.Tau == Black
		if !ok {
			t.Fatalf("absorbed pixel state: %+v", st)
		}
		targetCheck(t, ok, "M=0 round leaves N, R and tau unchanged")

		// the Radiance denominator counts every emitted photon, even lost ones
		scene3 := sppmLightScene()
		camera3 := LookAt(V(4, 0, 10), V(0, 0, 0), V(0, 1, 0), 30)
		options3 := sppmTestOptions()
		options3.PhotonsPerIteration = 20000
		options3.Seed = 33
		r3, err := NewSPPM(scene3, camera3, 3, 3, options3)
		if err != nil {
			t.Fatal(err)
		}
		for i := 0; i < 3; i++ {
			if err := r3.Step(); err != nil {
				t.Fatal(err)
			}
		}
		stats := r3.Stats()
		ok = stats.Iterations == 3 && stats.Emitted == 3*uint64(options3.PhotonsPerIteration)
		if !ok {
			t.Fatalf("stats: %+v", stats)
		}
		found := false
		for y := 0; y < 3 && !found; y++ {
			for x := 0; x < 3 && !found; x++ {
				st = r3.PixelState(x, y)
				if st.Tau == Black {
					continue
				}
				found = true
				area := math.Pi * st.Radius * st.Radius
				want := st.Tau.DivScalar(area * float64(stats.Emitted))
				if r3.Radiance(x, y) != want {
					t.Fatalf("radiance %v vs %v", r3.Radiance(x, y), want)
				}
			}
		}
		ok = ok && found
		if !ok {
			t.Fatalf("no pixel with tau found")
		}
		targetCheck(t, ok, "radiance denominator includes lost photons")
	})

	t.Run("VisibilityLoss", func(t *testing.T) {
		// A tiny sphere on the edge of the center pixel: jitter makes the
		// eye path miss it on some iterations. State must survive those.
		scene := &Scene{}
		scene.Add(NewSphere(V(0, 0, -5), 0.0016, DiffuseMaterial(White)))
		scene.Add(NewSphere(V(0.05, 0, -4.85), 0.02, LightMaterial(White, 1)))
		scene.Compile()
		camera := LookAt(V(0, 0, 0), V(0, 0, -1), V(0, 1, 0), 0.0917)
		options := sppmTestOptions()
		options.PhotonsPerIteration = 100000
		options.MaxDepth = 4
		options.InitialRadius = 0.004
		options.Alpha = 0.7
		options.Seed = 41
		r, err := NewSPPM(scene, camera, 3, 3, options)
		if err != nil {
			t.Fatal(err)
		}
		prev := r.PixelState(1, 1)
		seenVisible, seenInvisible := false, false
		preserved := true
		for step := 0; step < 8; step++ {
			if err := r.Step(); err != nil {
				t.Fatal(err)
			}
			cur := r.PixelState(1, 1)
			if cur.Visible {
				seenVisible = true
			} else {
				seenInvisible = true
				if cur.N != prev.N || cur.Radius != prev.Radius || cur.Tau != prev.Tau {
					preserved = false
					t.Fatalf("step %d: invisible round changed state: %+v vs %+v", step, cur, prev)
				}
			}
			prev = cur
		}
		ok := seenVisible && seenInvisible && preserved
		if !ok {
			t.Fatalf("visible=%v invisible=%v preserved=%v", seenVisible, seenInvisible, preserved)
		}
		targetCheck(t, ok, "lost visible point preserves N, R and tau")
	})

	t.Run("Reproducible", func(t *testing.T) {
		build := func() *SPPMRenderer {
			scene := sppmLightScene()
			camera := LookAt(V(4, 0, 10), V(0, 0, 0), V(0, 1, 0), 30)
			options := sppmTestOptions()
			options.PhotonsPerIteration = 5000
			options.Seed = 51
			r, err := NewSPPM(scene, camera, 3, 2, options)
			if err != nil {
				t.Fatal(err)
			}
			return r
		}
		a := build()
		b := build()
		same := true
		for step := 0; step < 3; step++ {
			if err := a.Step(); err != nil {
				t.Fatal(err)
			}
			if err := b.Step(); err != nil {
				t.Fatal(err)
			}
			for y := 0; y < 2; y++ {
				for x := 0; x < 3; x++ {
					pa := a.PixelState(x, y)
					pb := b.PixelState(x, y)
					if pa != pb {
						same = false
						t.Fatalf("step %d pixel (%d,%d): %+v vs %+v", step, x, y, pa, pb)
					}
					if a.Radiance(x, y) != b.Radiance(x, y) {
						same = false
						t.Fatalf("step %d radiance (%d,%d) differs", step, x, y)
					}
				}
			}
			if a.Stats() != b.Stats() {
				same = false
				t.Fatalf("step %d stats: %+v vs %+v", step, a.Stats(), b.Stats())
			}
		}
		targetCheck(t, same, "two identical instances reproduce exactly over three steps")
	})
}
