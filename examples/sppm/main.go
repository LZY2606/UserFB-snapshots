// Command sppm renders a glass sphere focusing light onto a table with
// progressive photon mapping and writes the result as a binary PPM image.
//
// Usage:
//
//	go run examples/sppm/main.go [out.ppm]
package main

import (
	"fmt"
	"os"

	"github.com/fogleman/pt/pt"
)

func main() {
	path := "sppm.ppm"
	if len(os.Args) > 1 {
		path = os.Args[1]
	}

	// scene: a glass sphere on a diffuse table, lit by a small sphere light
	scene := pt.Scene{}
	scene.Add(pt.NewPlane(pt.V(0, 0, 0), pt.V(0, 0, 1), pt.DiffuseMaterial(pt.Color{R: 0.85, G: 0.85, B: 0.85})))
	scene.Add(pt.NewSphere(pt.V(0, 0, 1), 1, pt.ClearMaterial(1.5, 0)))
	scene.Add(pt.NewSphere(pt.V(0, 0, 6), 0.5, pt.LightMaterial(pt.White, 8)))
	scene.Compile()

	camera := pt.LookAt(pt.V(3.5, -4.5, 2.8), pt.V(0, 0, 0.6), pt.V(0, 0, 1), 38)

	const width, height = 320, 240
	renderer, err := pt.NewSPPM(&scene, camera, width, height, pt.SPPMOptions{
		PhotonsPerIteration: 100000,
		MaxDepth:            8,
		InitialRadius:       0.2,
		Alpha:               0.7,
		Seed:                1,
	})
	if err != nil {
		panic(err)
	}
	for i := 0; i < 100; i++ {
		if err := renderer.Step(); err != nil {
			panic(err)
		}
	}
	stats := renderer.Stats()
	fmt.Fprintf(os.Stderr, "iterations=%d emitted=%d candidateChecks=%d\n",
		stats.Iterations, stats.Emitted, stats.CandidateChecks)

	file, err := os.Create(path)
	if err != nil {
		panic(err)
	}
	defer file.Close()
	fmt.Fprintf(file, "P6\n%d %d\n255\n", width, height)
	buf := make([]byte, 0, width*height*3)
	for y := 0; y < height; y++ {
		for x := 0; x < width; x++ {
			c := renderer.Radiance(x, y).Pow(1 / 2.2)
			buf = append(buf, ppmByte(c.R), ppmByte(c.G), ppmByte(c.B))
		}
	}
	if _, err := file.Write(buf); err != nil {
		panic(err)
	}
	fmt.Fprintln(os.Stderr, "wrote", path)
}

func ppmByte(x float64) byte {
	if x < 0 {
		x = 0
	}
	if x > 1 {
		x = 1
	}
	return byte(x*255 + 0.5)
}
