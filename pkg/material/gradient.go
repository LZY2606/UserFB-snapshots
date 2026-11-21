package material

import (
	"fmt"
	"math"
	"math/rand"

	"github.com/hunterloftis/pbr/pkg/render"
	"github.com/hunterloftis/pbr/pkg/rgb"
)

// Metalness declares a gradient channel tracking un's Metalness: the
// probability of the discrete metal/non-metal branch taken in At.
// The derivative is estimated with the score (likelihood-ratio) method
// along each traced path, so the material must satisfy Transmission == 0,
// Roughness > 0, and 0 < Metalness < 1.
func Metalness(un *Uniform) render.GradientParam {
	return render.GradientParam{Material: un, Kind: render.Metalness}
}

// Emission declares a gradient channel tracking un's Emission, the scale
// of its emitted light. The derivative keeps the explicit term
// d Light() / d Emission = Color; the emitter's identity is unchanged.
// The material must satisfy Emission > 0.
func Emission(un *Uniform) render.GradientParam {
	return render.GradientParam{Material: un, Kind: render.Emission}
}

// GradientValid implements render.GradientMaterial.
func (un *Uniform) GradientValid(kind render.GradientKind) error {
	switch kind {
	case render.Metalness:
		if !finite(un.Metalness) || un.Metalness <= 0 || un.Metalness >= 1 {
			return fmt.Errorf("metalness %v: need 0 < metalness < 1", un.Metalness)
		}
		if !finite(un.Roughness) || un.Roughness <= 0 {
			return fmt.Errorf("roughness %v: need roughness > 0", un.Roughness)
		}
		if un.Transmission != 0 {
			return fmt.Errorf("transmission %v: metalness gradients require transmission == 0", un.Transmission)
		}
	case render.Emission:
		if !finite(un.Emission) || un.Emission <= 0 {
			return fmt.Errorf("emission %v: need emission > 0", un.Emission)
		}
	default:
		return fmt.Errorf("unsupported gradient kind %s", kind)
	}
	return nil
}

// EmissionDeriv implements render.EmissionDeriver:
// Light() = Color * Emission, so d Light() / d Emission = Color.
func (un *Uniform) EmissionDeriv() rgb.Energy {
	return un.Color
}

// recordMetalness reports the realized metal/non-metal branch choice to the
// active gradient trace, if any: d log p / d Metalness of the outcome.
// It consumes no random numbers, so recording never alters the path.
func (un *Uniform) recordMetalness(rnd *rand.Rand, metal bool) {
	if metal {
		render.RecordChoice(rnd, un, render.Metalness, 1/un.Metalness)
		return
	}
	render.RecordChoice(rnd, un, render.Metalness, -1/(1-un.Metalness))
}

func finite(f float64) bool {
	return !math.IsNaN(f) && !math.IsInf(f, 0)
}
