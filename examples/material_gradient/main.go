// Command material_gradient renders a small frame synchronously and writes
// the linear beauty image (beauty.ppm) plus one signed RGB gradient image
// per declared material parameter (gradients.csv).
//
// Usage:
//
//	go run ./examples/material_gradient <output-dir>
package main

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/hunterloftis/pbr/pkg/camera"
	"github.com/hunterloftis/pbr/pkg/geom"
	"github.com/hunterloftis/pbr/pkg/material"
	"github.com/hunterloftis/pbr/pkg/render"
	"github.com/hunterloftis/pbr/pkg/rgb"
	"github.com/hunterloftis/pbr/pkg/surface"
)

// flat is a constant environment (pkg/env needs unavailable deps here).
type flat struct{ light rgb.Energy }

func (f flat) At(geom.Dir) rgb.Energy { return f.light }

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintf(os.Stderr, "usage: %s <output-dir>\n", filepath.Base(os.Args[0]))
		os.Exit(2)
	}
	dir := os.Args[1]
	if err := os.MkdirAll(dir, 0o755); err != nil {
		fatal(err)
	}

	// Two spheres share one metalness-tracked material; a sphere lamp
	// provides tracked emission.
	shared := &material.Uniform{
		Color:       rgb.Energy{X: 0.8, Y: 0.5, Z: 0.3},
		Metalness:   0.4,
		Roughness:   0.35,
		Specularity: 0.05,
	}
	lamp := &material.Uniform{
		Color:    rgb.Energy{X: 1, Y: 0.9, Z: 0.8},
		Emission: 40,
	}
	floorMat := &material.Uniform{
		Color:       rgb.Energy{X: 0.7, Y: 0.7, Z: 0.7},
		Roughness:   0.6,
		Specularity: 0.04,
	}
	ball1 := surface.UnitSphere(shared).Shift(geom.Vec{X: -0.7, Y: 0.2})
	ball2 := surface.UnitSphere(shared).Shift(geom.Vec{X: 0.7, Y: 0.2})
	light := surface.UnitSphere(lamp).Shift(geom.Vec{Y: 4, Z: 0.5}).Scale(geom.Vec{X: 4, Y: 4, Z: 4})
	floor := surface.UnitCube(floorMat).Shift(geom.Vec{Y: -0.35}).Scale(geom.Vec{X: 20, Y: 0.1, Z: 20})
	cam := camera.NewSLR().MoveTo(geom.Vec{Y: 1.0, Z: 3.5}).LookAt(geom.Vec{Y: 0.2})
	scene := render.NewScene(cam, surface.NewTree(ball1, ball2, light, floor), flat{rgb.Energy{X: 0.2, Y: 0.25, Z: 0.35}})

	params := []render.GradientParam{
		material.Metalness(shared),
		material.Emission(lamp),
	}
	res, err := render.RenderGradient(scene, 64, 48, 16, 6, true, 1, params)
	if err != nil {
		fatal(err)
	}

	if err := writeBeauty(filepath.Join(dir, "beauty.ppm"), res); err != nil {
		fatal(err)
	}
	if err := writeGradients(filepath.Join(dir, "gradients.csv"), res, params); err != nil {
		fatal(err)
	}
	fmt.Printf("wrote %s and %s (%dx%d, %d samples/pixel, %d gradient channels)\n",
		"beauty.ppm", "gradients.csv", res.Width, res.Height, res.Samples, len(res.Channels))
}

func fatal(err error) {
	fmt.Fprintln(os.Stderr, "error:", err)
	os.Exit(1)
}

// writeBeauty gamma-maps the linear beauty image into a P6 PPM.
func writeBeauty(path string, res *render.GradientResult) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	if _, err := fmt.Fprintf(f, "P6\n%d %d\n255\n", res.Width, res.Height); err != nil {
		return err
	}
	buf := make([]byte, 0, res.Width*res.Height*3)
	for _, e := range res.Beauty {
		c := e.ToRGBA()
		buf = append(buf, c.R, c.G, c.B)
	}
	_, err = f.Write(buf)
	return err
}

// writeGradients writes every gradient channel as signed linear floats.
func writeGradients(path string, res *render.GradientResult, params []render.GradientParam) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	if _, err := fmt.Fprintln(f, "channel,parameter,x,y,r,g,b"); err != nil {
		return err
	}
	for ch := range res.Channels {
		for y := 0; y < res.Height; y++ {
			for x := 0; x < res.Width; x++ {
				g := res.GradientAt(ch, x, y)
				if _, err := fmt.Fprintf(f, "%d,%s,%d,%d,%.9g,%.9g,%.9g\n",
					ch, params[ch].Kind, x, y, g.X, g.Y, g.Z); err != nil {
					return err
				}
			}
		}
	}
	return nil
}
