package surface

import (
	"math"
	"math/rand"

	"github.com/hunterloftis/pbr/pkg/geom"
	"github.com/hunterloftis/pbr/pkg/render"
	"github.com/hunterloftis/pbr/pkg/rgb"
)

type Material interface {
	At(u, v float64, in, norm geom.Dir, rnd *rand.Rand) (normal geom.Dir, bsdf render.BSDF)
	Light() rgb.Energy
	Transmit() rgb.Energy
}

type DefaultMaterial struct {
}

func (d *DefaultMaterial) At(u, v float64, in, norm geom.Dir, rnd *rand.Rand) (normal geom.Dir, bsdf render.BSDF) {
	return norm, Lambert{}
}

func (d *DefaultMaterial) Light() rgb.Energy {
	return rgb.Black
}

func (d *DefaultMaterial) Transmit() rgb.Energy {
	return rgb.Black
}

type Lambert struct {
}

func (l Lambert) Sample(wo geom.Dir, rnd *rand.Rand) (geom.Dir, float64, bool) {
	wi, _ := geom.Up.RandHemiCos(rnd)
	return wi, l.PDF(wi, wo), wo.Dot(geom.Up) > 0
}

func (l Lambert) PDF(wi, wo geom.Dir) float64 {
	return wi.Dot(geom.Up) * math.Pi
}

func (l Lambert) Eval(wi, wo geom.Dir) rgb.Energy {
	return rgb.White
}

func (l Lambert) Emit() rgb.Energy {
	return rgb.Black
}

// emissionGradient exposes a material's emission derivative, if the
// material supports it, keyed by the material's identity.
func emissionGradient(m Material) (interface{}, rgb.Energy, bool) {
	d, ok := m.(render.EmissionDeriver)
	if !ok {
		return nil, rgb.Black, false
	}
	return m, d.EmissionDeriv(), true
}
