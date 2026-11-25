package main

// Fluorescence example: blue light from the environment hits a fluorescent
// sphere whose conversion table only turns short (blue) excitation
// wavelengths into longer green emission wavelengths. The walk changes the
// current wavelength along the real geometric path while the sensor
// wavelength drawn at the camera is retained until output.
//
// Run it directly with:
//
//	go run examples/fluorescence.go [out.png]
//
// Only the Go standard library and the local pt package are used.

import (
	"fmt"
	"math/rand"
	"os"

	. "github.com/fogleman/pt/pt"
)

func main() {
	// blue-only environment spectrum
	blueEnv := Spectrum{
		Wavelengths: []float64{400, 440, 480, 500},
		Values:      []float64{1, 1, 1, 0},
	}

	scene := &Scene{}
	floor := NewPlane(V(0, 0, 0), V(0, 0, 1), DiffuseMaterial(Black))
	ball := NewSphere(V(0, 0, 1.2), 1, DiffuseMaterial(Black))
	scene.Add(floor)
	scene.Add(ball)
	scene.Compile()

	// conversion table: excitation (rows) in the blue band, emission
	// (columns) in the green band, constant photon-number density
	table := &FluorescenceTable{
		Excitation: Spectrum{
			Wavelengths: []float64{400, 440, 480},
			Values:      []float64{0, 0, 0},
		},
		Emission: Spectrum{
			Wavelengths: []float64{500, 550, 600},
			Values:      []float64{0, 0, 0},
		},
		Data: [][]float64{
			{0.004, 0.004, 0.004},
			{0.004, 0.004, 0.004},
			{0.004, 0.004, 0.004},
		},
	}
	materials := map[Shape]SpectralMaterial{
		floor: {
			Emission: Spectrum{Wavelengths: []float64{400, 700}, Values: []float64{0, 0}},
			Albedo:   Spectrum{Wavelengths: []float64{400, 700}, Values: []float64{0.6, 0.6}},
		},
		ball: {
			Emission: Spectrum{Wavelengths: []float64{400, 700}, Values: []float64{0, 0}},
			Albedo:   Spectrum{Wavelengths: []float64{400, 700}, Values: []float64{0, 0}},
			Table:    table,
		},
	}

	// three non-overlapping response bands: red, green, blue
	sensor := Sensor{Responses: [3]Spectrum{
		{Wavelengths: []float64{600, 650, 700}, Values: []float64{0, 1, 0}}, // R: red band
		{Wavelengths: []float64{500, 550, 600}, Values: []float64{1, 1, 1}}, // G: green band
		{Wavelengths: []float64{400, 450, 500}, Values: []float64{0, 1, 0}}, // B: blue band
	}}

	sampler, err := NewFluorescentSampler(scene, materials, blueEnv, sensor, 4)
	if err != nil {
		panic(err)
	}

	// inspect one real path before rendering: start in the green band so
	// the sphere must convert it down into a blue excitation wavelength
	rnd := rand.New(rand.NewSource(7))
	tracedRay := Ray{Origin: V(0, 0, 3), Direction: V(0, 0, -1)}
	L, events := sampler.Trace(scene, tracedRay, 560, rnd)
	fmt.Printf("scalar radiance of traced path: %.6f\n", L)
	for _, e := range events {
		fmt.Printf("  depth=%d branch=%-12s lambda %7.2f -> %7.2f throughput=%.6f\n",
			e.Depth, e.Branch, e.WavelengthIn, e.WavelengthOut, e.Throughput)
	}

	// render a small image with the existing Renderer
	camera := LookAt(V(0, -4, 1.5), V(0, 0, 1), V(0, 0, 1), 45)
	const size = 64
	renderer := NewRenderer(scene, &camera, sampler, size, size)
	renderer.SamplesPerPixel = 32
	renderer.NumCPU = 1
	renderer.Verbose = false
	image := renderer.Render()

	var sumR, sumG, sumB float64
	for y := 0; y < size; y++ {
		for x := 0; x < size; x++ {
			c := renderer.Buffer.Color(x, y)
			sumR += c.R
			sumG += c.G
			sumB += c.B
		}
	}
	n := float64(size * size)
	fmt.Printf("image mean (linear): R=%.6f G=%.6f B=%.6f\n", sumR/n, sumG/n, sumB/n)

	if len(os.Args) > 1 {
		if err := SavePNG(os.Args[1], image); err != nil {
			panic(err)
		}
		fmt.Printf("wrote %s\n", os.Args[1])
	}
}
