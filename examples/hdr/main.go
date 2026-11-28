// HDR rendering with a bloom preview. The scene is rendered once into a
// floating point target and resolved twice: once without bloom and once
// with it, producing hdr_nobloom.png and hdr_bloom.png.
package main

import (
	"fmt"

	. "github.com/fogleman/fauxgl"
)

const (
	width  = 480
	height = 360
	fovy   = 40
	near   = 1
	far    = 20
)

func main() {
	// Build a small scene from real geometry: bright spheres on a floor.
	mesh := NewEmptyMesh()

	sphere := NewSphere(3)
	positions := []Vector{
		{-1.2, -0.3, 0.4},
		{0.2, -0.5, -0.3},
		{1.3, -0.1, 0.8},
	}
	colors := []Color{
		{2.8, 0.6, 0.3, 1}, // hot: channels above one
		{0.4, 1.8, 2.6, 1},
		{1.6, 2.4, 0.6, 1},
	}
	for i, pos := range positions {
		part := sphere.Copy()
		part.Transform(Translate(pos))
		part.SetColor(colors[i])
		mesh.Add(part)
	}

	floor := NewPlane()
	floor.Transform(Scale(Vector{8, 8, 1}))
	floor.Transform(Rotate(Vector{1, 0, 0}, Radians(90)))
	floor.Transform(Translate(Vector{0, 0, -1}))
	mesh.Add(floor)

	eye := V(2.2, -3.2, 1.8)
	center := V(0, 0, 0)
	up := V(0, 0, 1)
	aspect := float64(width) / float64(height)
	matrix := LookAt(eye, center, up).Perspective(fovy, aspect, near, far)
	light := V(-3, -2, 4).Normalize()

	shader := NewPhongShader(matrix, light, eye)
	shader.AmbientColor = Color{0.15, 0.15, 0.15, 1}
	shader.DiffuseColor = Color{0.7, 0.7, 0.7, 1}
	shader.SpecularColor = Color{1.5, 1.5, 1.5, 1}
	shader.SpecularPower = 24

	context := NewContext(width, height)
	context.ClearColor = Color{0.02, 0.02, 0.05, 1}
	context.EnableHDR()
	context.ClearColorBuffer()
	context.Shader = shader
	context.DrawMesh(mesh)

	settings := HDRSettings{
		Background:     Black,
		Exposure:       1.1,
		BloomThreshold: 1.0,
		BloomStrength:  0,
		BloomLevels:    5,
	}

	plain, err := context.ResolveHDR(settings)
	if err != nil {
		panic(err)
	}
	if err := SavePNG("hdr_nobloom.png", plain); err != nil {
		panic(err)
	}

	// A second resolve from the same floating point target adds the glow.
	// The target itself is untouched, so the two images share one render.
	settings.BloomStrength = 0.8
	bloomed, err := context.ResolveHDR(settings)
	if err != nil {
		panic(err)
	}
	if err := SavePNG("hdr_bloom.png", bloomed); err != nil {
		panic(err)
	}

	snap := context.HDRSnapshot()
	var brightest float64
	for _, c := range snap {
		if c.R > brightest {
			brightest = c.R
		}
	}
	fmt.Printf("wrote hdr_nobloom.png and hdr_bloom.png (%dx%d), peak linear R=%.3f\n",
		width, height, brightest)
}
