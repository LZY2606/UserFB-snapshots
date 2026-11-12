package main

import (
	"fmt"
	"math"
	"math/rand"
	"os"

	. "github.com/fogleman/pt/pt"
)

// Renders a diffuse scene lit by a procedural environment map with a small
// bright "sun" patch, using EnvironmentMap importance sampling combined with
// cosine bounces via MIS (DefaultSampler.EnvironmentMIS).
func main() {
	// build a 64x32 equirectangular sky: dim blue gradient plus a small
	// bright sun patch (linear RGB radiance, first row at the north pole)
	const w, h = 64, 32
	pixels := make([]Color, w*h)
	for r := 0; r < h; r++ {
		for c := 0; c < w; c++ {
			base := 0.03 + 0.08*float64(r)/float64(h-1)
			pixels[r*w+c] = Color{base * 0.8, base, base * 1.4}
		}
	}
	sun := Color{60, 55, 45}
	for _, cell := range [][2]int{{10, 40}, {10, 41}, {11, 40}, {11, 41}} {
		pixels[cell[0]*w+cell[1]] = sun
	}
	env, err := NewEnvironmentMap(w, h, pixels)
	if err != nil {
		panic(err)
	}

	// scene: floor, a diffuse ball and a mirror ball, sky from the env map
	scene := Scene{}
	scene.Environment = env
	scene.Add(NewPlane(V(0, 0, 0), V(0, 1, 0), DiffuseMaterial(Color{0.7, 0.7, 0.7})))
	scene.Add(NewSphere(V(-1.2, 0.6, 0), 0.6, DiffuseMaterial(Color{0.8, 0.3, 0.2})))
	scene.Add(NewSphere(V(1.2, 0.6, 0.5), 0.6, MetallicMaterial(White, 0, 0)))
	scene.Compile()

	camera := LookAt(V(0, 2.2, 4.5), V(0, 0.6, 0), V(0, 1, 0), 40)

	sampler := NewSampler(4, 4)
	sampler.EnvironmentMIS = true

	const width, height = 320, 180
	const samples = 16
	if err := os.MkdirAll(".cache", 0755); err != nil {
		panic(err)
	}
	file, err := os.Create(".cache/environment.ppm")
	if err != nil {
		panic(err)
	}
	defer file.Close()
	fmt.Fprintf(file, "P3\n%d %d\n255\n", width, height)
	for y := 0; y < height; y++ {
		for x := 0; x < width; x++ {
			rnd := rand.New(rand.NewSource(int64(y*width + x)))
			sum := Black
			for s := 0; s < samples; s++ {
				fu := (float64(x) + rnd.Float64()) / width
				fv := (float64(y) + rnd.Float64()) / height
				ray := camera.CastRay(x, y, width, height, fu, fv, rnd)
				sum = sum.Add(sampler.Sample(&scene, ray, rnd))
			}
			c := sum.DivScalar(samples).Pow(1 / 2.2)
			r := int(math.Max(0, math.Min(255, c.R*255)))
			g := int(math.Max(0, math.Min(255, c.G*255)))
			b := int(math.Max(0, math.Min(255, c.B*255)))
			fmt.Fprintf(file, "%d %d %d\n", r, g, b)
		}
	}
	fmt.Println("wrote .cache/environment.ppm")
}
