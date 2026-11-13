package pt

import (
	"math"
	"math/rand"
	"testing"
)

func TestSeedGeometry(t *testing.T) {
	scene := &Scene{}
	sphere := NewSphere(V(0, 0, 0), 1, DiffuseMaterial(White))
	scene.Add(sphere)
	scene.Compile()
	ray := Ray{Origin: V(0, 0, 3), Direction: V(0, 0, -1)}
	hit := scene.Intersect(ray)
	if !hit.Ok() || math.Abs(hit.T-2) > 1e-12 {
		t.Fatalf("sphere hit: %+v", hit)
	}
	info := hit.Info(ray)
	if info.Position != V(0, 0, 1) || info.Normal != V(0, 0, 1) {
		t.Fatalf("surface: %+v", info)
	}
	t.Log("SEED_GEOMETRY t=2 position=(0,0,1)")
}

func TestSeedTransport(t *testing.T) {
	scene := &Scene{}
	scene.Add(NewSphere(V(0, 0, 0), 1, LightMaterial(Color{.25, .5, 1}, 2)))
	scene.Compile()
	ray := Ray{Origin: V(0, 0, 3), Direction: V(0, 0, -1)}
	sampler := NewSampler(1, 0)
	got := sampler.Sample(scene, ray, rand.New(rand.NewSource(11)))
	if got != (Color{.5, 1, 2}) {
		t.Fatalf("emission: %+v", got)
	}
	scene2 := &Scene{}
	light := NewSphere(V(0, 0, 5), 1, LightMaterial(White, 1))
	scene2.Add(light)
	scene2.Compile()
	sampler.SoftShadows = false
	got = sampler.sampleLight(scene2, Ray{Origin: V(0, 0, 0), Direction: V(0, 0, 1)}, rand.New(rand.NewSource(12)), light)
	if math.Abs(got.R-1.0/24) > 1e-12 || got.R != got.G || got.R != got.B {
		t.Fatalf("old light: %+v", got)
	}
	t.Logf("SEED_TRANSPORT emission=(0.5,1,2) old_direct=%.12f", got.R)
}

func TestSeedCameraAndBounce(t *testing.T) {
	c := LookAt(V(0, 0, 3), V(0, 0, 0), V(0, 1, 0), 45)
	r := c.CastRay(1, 1, 3, 3, .5, .5, rand.New(rand.NewSource(3)))
	if r.Origin != V(0, 0, 3) || r.Direction != V(0, 0, -1) {
		t.Fatalf("camera: %+v", r)
	}
	s := NewSphere(V(0, 0, 0), 1, MetallicMaterial(White, 0, 1))
	h := s.Intersect(r)
	info := h.Info(r)
	bounce, spec, p := r.Bounce(&info, .25, .75, BounceTypeAny, rand.New(rand.NewSource(3)))
	if !spec || p != 1 || bounce.Direction != V(0, 0, 1) {
		t.Fatalf("mirror: %+v %v %v", bounce, spec, p)
	}
	t.Log("SEED_CAMERA_BOUNCE mirror=(0,0,1)")
}
