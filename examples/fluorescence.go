package main

import . "github.com/fogleman/pt/pt"

// Blue light in, green light out: a fluorescent floor converts short
// wavelengths to longer ones, which plain RGB path tracing cannot
// represent. Run: go run examples/fluorescence.go
func main() {
	// spectral curves are piecewise linear between nodes (nm) and zero
	// outside their node range
	zero, _ := NewSpectrum([]float64{400, 700}, []float64{0, 0})

	// environment emits only blue light around 450 nm
	env, _ := NewSpectrum(
		[]float64{400, 439, 440, 460, 461, 700},
		[]float64{0, 0, 3, 3, 0, 0})

	// fluorescent material: no elastic reflection, converts blue
	// excitation (rows) to green emission (columns)
	excitation := []float64{400, 460, 520}
	emission := []float64{500, 560, 620}
	table := [][]float64{
		{0.6, 0.6, 0.6},
		{0.6, 0.6, 0.6},
		{0.6, 0.6, 0.6},
	}
	floor, _ := NewFluorescentMaterial(zero, zero, excitation, emission, table)

	// a non-fluorescent sphere with elastic albedo only
	albedo, _ := NewSpectrum([]float64{400, 700}, []float64{0.7, 0.7})
	ball, _ := NewFluorescentMaterial(albedo, zero, excitation, emission,
		[][]float64{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}})

	// camera responses: three broad bands
	r, _ := NewSpectrum([]float64{400, 560, 700}, []float64{0, 0.2, 1})
	g, _ := NewSpectrum([]float64{400, 500, 620, 700}, []float64{0, 1, 0.6, 0})
	b, _ := NewSpectrum([]float64{400, 470, 700}, []float64{1, 0.4, 0})
	sensor, err := NewSensor(r, g, b)
	if err != nil {
		panic(err)
	}

	scene := Scene{}
	floorShape := NewPlane(V(0, 0, 0), V(0, 0, 1), DiffuseMaterial(White))
	ballShape := NewSphere(V(0, 0, 1), 1, DiffuseMaterial(White))
	scene.Add(floorShape)
	scene.Add(ballShape)

	sampler, err := NewFluorescentSampler(&scene, map[Shape]*FluorescentMaterial{
		floorShape: floor,
		ballShape:  ball,
	}, env, sensor, 4)
	if err != nil {
		panic(err)
	}

	camera := LookAt(V(3, 3, 3), V(0, 0, 0.5), V(0, 0, 1), 50)
	renderer := NewRenderer(&scene, &camera, sampler, 480, 270)
	renderer.SamplesPerPixel = 16
	renderer.IterativeRender("fluorescence%03d.png", 4)
}
