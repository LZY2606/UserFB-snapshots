package pt

import (
	"math"
	"math/rand"
	"testing"
)

func seedClose(t *testing.T, got, want Color) {
	t.Helper()
	if math.Abs(got.R-want.R) > 1e-12 || math.Abs(got.G-want.G) > 1e-12 || math.Abs(got.B-want.B) > 1e-12 {
		t.Fatalf("got=%+v want=%+v", got, want)
	}
}

func TestEnvironmentSeedSky(t *testing.T) {
	scene := &Scene{Color: Color{.25, .5, .75}}
	scene.Compile()
	got := NewSampler(1, 1).Sample(scene, Ray{Vector{0, 1, 0}, Vector{0, 1, 0}}, rand.New(rand.NewSource(81)))
	seedClose(t, got, scene.Color)
	t.Logf("LEGACY_SKY_OK rgb=%.6f,%.6f,%.6f", got.R, got.G, got.B)
}

func TestEnvironmentSeedDiffuse(t *testing.T) {
	scene := &Scene{Color: Color{.25, .5, .75}}
	scene.Add(NewPlane(Vector{}, Vector{0, 1, 0}, DiffuseMaterial(Color{.4, .6, .8})))
	scene.Compile()
	ray := Ray{Vector{0, 1, 0}, Vector{0, -1, 0}}
	sampler := NewSampler(1, 1)
	for i := int64(0); i < 32; i++ {
		got := sampler.Sample(scene, ray, rand.New(rand.NewSource(i)))
		seedClose(t, got, Color{.1, .3, .6})
	}
	t.Log("LEGACY_DIFFUSE_OK seeds=32 rgb=0.100000,0.300000,0.600000")
	sampler.MaxBounces = 0
	got := sampler.Sample(scene, ray, rand.New(rand.NewSource(81)))
	seedClose(t, got, Black)
	t.Log("LEGACY_NO_ENV_NEE rgb=0.000000,0.000000,0.000000")
}

func TestEnvironmentSeedMirror(t *testing.T) {
	scene := &Scene{Color: Color{.25, .5, .75}}
	scene.Add(NewPlane(Vector{}, Vector{0, 1, 0}, MetallicMaterial(White, 0, 0)))
	scene.Compile()
	sampler := NewSampler(1, 1)
	got := sampler.Sample(scene, Ray{Vector{0, 1, 0}, Vector{0, -1, 0}}, rand.New(rand.NewSource(81)))
	seedClose(t, got, scene.Color)
	t.Log("LEGACY_MIRROR_OK rgb=0.250000,0.500000,0.750000")
}
