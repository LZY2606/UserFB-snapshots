package main

import (
	"fmt"

	. "github.com/fogleman/fauxgl"
)

// HDR rendering with bloom. Renders the same geometry three ways:
//
//	hdr_without_bloom.png - linear HDR target resolved with exposure + sRGB
//	hdr_with_bloom.png    - the same plus thresholded, multi-level bloom
//	ldr.png               - the classic byte pipeline for comparison
//
// The Phong specular highlight stores values well above 1.0 in the floating
// point target. The LDR byte path clamps that highlight to white before any
// exposure can recover it, which is why the bloom frame sees detail the byte
// frame cannot.

const (
	width  = 800
	height = 600
	fovy   = 30
	near   = 1
	far    = 10
)

var (
	eye    = V(-3, 1, -2)
	center = V(0, 0, 0)
	up     = V(0, 1, 0)
	light  = V(-1, 0.35, 0.8).Normalize()
)

func setup() (*Mesh, *Context, *PhongShader) {
	mesh, err := LoadSTL("examples/sphere.stl")
	if err != nil {
		panic(err)
	}
	mesh.BiUnitCube()
	mesh.SmoothNormals()

	context := NewContext(width, height)
	context.ClearColor = HexColor("#101820")

	aspect := float64(width) / float64(height)
	matrix := LookAt(eye, center, up).Perspective(fovy, aspect, near, far)

	shader := NewPhongShader(matrix, light, eye)
	shader.ObjectColor = HexColor("#468966")
	shader.SpecularColor = Color{6, 6, 6, 1} // deliberately hot highlight
	shader.SpecularPower = 48
	context.Shader = shader
	return mesh, context, shader
}

func renderHDR(bloom bool) {
	mesh, context, _ := setup()
	context.EnableHDR()
	context.DrawMesh(mesh)

	settings := HDRSettings{
		Background:     context.ClearColor,
		Exposure:       0.9,
		BloomThreshold: 0.8,
		BloomStrength:  0.5,
		BloomLevels:    5,
	}
	if !bloom {
		settings.BloomStrength = 0
	}
	image, err := context.ResolveHDR(settings)
	if err != nil {
		panic(err)
	}
	name := "hdr_without_bloom.png"
	if bloom {
		name = "hdr_with_bloom.png"
	}
	if err := SavePNG(name, image); err != nil {
		panic(err)
	}
	fmt.Println("wrote", name)
}

func renderLDR() {
	mesh, context, _ := setup()
	context.ClearColorBuffer()
	context.DrawMesh(mesh)
	if err := SavePNG("ldr.png", context.Image()); err != nil {
		panic(err)
	}
	fmt.Println("wrote ldr.png")
}

func main() {
	renderHDR(false)
	renderHDR(true)
	renderLDR()
}
