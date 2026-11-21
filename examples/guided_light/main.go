// Command guided_light renders a small scene whose area
// light uses a sampling guide independent of its actual
// emission, and writes the result to a PNG file.
package main

import (
	"flag"
	"math"
	"math/rand"
	"os"

	"github.com/unixpickle/model3d/model3d"
	"github.com/unixpickle/model3d/render3d"
)

func main() {
	var output string
	flag.StringVar(&output, "output", "guided.png", "path of the PNG to write")
	flag.Parse()

	rand.Seed(1234)

	// A ceiling light made of four triangles meeting at a
	// center vertex. Emission and guide are deliberately
	// mismatched: the brightest triangle is proposed rarely
	// while a dim triangle is proposed often. The linear
	// radiance in the output does not depend on the guides.
	corners := [4]model3d.Coord3D{
		model3d.XYZ(-2, -2, 3),
		model3d.XYZ(2, -2, 3),
		model3d.XYZ(2, 2, 3),
		model3d.XYZ(-2, 2, 3),
	}
	center := model3d.XYZ(0, 0, 3)
	triangles := make([]*model3d.Triangle, 4)
	for i := range corners {
		// Winding chosen so the flat normals face downward
		// into the room rather than toward the ceiling.
		triangles[i] = &model3d.Triangle{corners[(i+1)%4], corners[i], center}
	}
	emission := []render3d.Color{
		render3d.NewColorRGB(0.9, 0.1, 0.1),
		render3d.NewColorRGB(0.1, 0.9, 0.1),
		render3d.NewColorRGB(0.1, 0.1, 0.9),
		render3d.NewColorRGB(0.8, 0.8, 0.2),
	}
	guide := []float64{100, 0.01, 5, 0.2}

	light, err := render3d.NewGuidedMeshAreaLight(triangles, emission, guide)
	if err != nil {
		panic(err)
	}

	// An inside-out box provides diffuse walls and a floor.
	ground := &render3d.ColliderObject{
		Collider: model3d.MeshToCollider(
			model3d.NewMeshRect(
				model3d.XYZ(-8, -8, -8),
				model3d.XYZ(8, 8, 0),
			).Scale(-1),
		),
		Material: &render3d.LambertMaterial{DiffuseColor: render3d.NewColor(0.5)},
	}
	scene := render3d.JoinedObject{ground, light}

	camera := render3d.NewCameraAt(model3d.XYZ(0, -6, 1.5), model3d.XYZ(0, 0, 1.5),
		math.Pi/3)
	tracer := &render3d.BidirPathTracer{
		Camera:        camera,
		Light:         light,
		MaxDepth:      4,
		MaxLightDepth: 3,
		NumSamples:    400,
	}

	img := render3d.NewImage(32, 32)
	tracer.Render(img, scene)

	total := 0.0
	nonBlack := 0
	for _, c := range img.Data {
		v := c.Sum()
		if math.IsNaN(v) || math.IsInf(v, 0) || v < 0 {
			panic("render produced an invalid pixel value")
		}
		if v > 0 {
			nonBlack++
		}
		total += v
	}
	if total <= 0 || nonBlack == 0 {
		panic("render produced a black image")
	}
	if err := img.Save(output); err != nil {
		panic(err)
	}
	if info, err := os.Stat(output); err != nil || info.Size() == 0 {
		panic("output PNG is missing or empty")
	}
}
