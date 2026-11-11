package pt

import (
	"math"
	"math/rand"
)

// sampleSphereLight estimates direct lighting from a spherical light by
// sampling its visible solid angle uniformly. For a Lambertian surface each
// sample contributes Emittance * color * max(N.d, 0) / (pi * pdf); samples
// shadowed by other geometry (or falling on the back side) contribute black.
func sampleSphereLight(scene *Scene, n Ray, rnd *rand.Rand, light *Sphere) Color {
	direction, _, pdf, ok := SampleSphereLight(n.Origin, light, rnd.Float64(), rnd.Float64())
	if !ok {
		// e.g. shading point inside the light: no solid-angle sample
		return Black
	}
	diffuse := direction.Dot(n.Direction)
	if diffuse <= 0 {
		return Black
	}
	ray := Ray{n.Origin, direction}
	hit := scene.Intersect(ray)
	if !hit.Ok() || hit.Shape != light {
		return Black
	}
	material := MaterialAt(light, ray.Position(hit.T))
	m := material.Emittance * diffuse / (math.Pi * pdf)
	return material.Color.MulScalar(m)
}

// SampleSphereLight uniformly samples, by solid angle, the cone of directions
// from origin that intersect the given sphere. It returns a unit direction,
// the distance along that direction to the first intersection with the
// sphere, and the pdf of the sample per unit solid angle. u and v must be in
// [0,1): u = 0 points at the sphere center, larger u moves toward the cone
// edge, and v revolves once around the cone axis. ok is false (with zero
// results) for invalid radii, non-finite inputs, origins inside or on the
// sphere, or out-of-range u/v.
func SampleSphereLight(origin Vector, light *Sphere, u, v float64) (direction Vector, distance, pdf float64, ok bool) {
	if light == nil {
		return Vector{}, 0, 0, false
	}
	radius := light.Radius
	if !(radius > 0) || math.IsInf(radius, 0) {
		return Vector{}, 0, 0, false
	}
	center := light.Center
	if !finiteVector(origin) || !finiteVector(center) {
		return Vector{}, 0, 0, false
	}
	if math.IsNaN(u) || math.IsNaN(v) || u < 0 || u >= 1 || v < 0 || v >= 1 {
		return Vector{}, 0, 0, false
	}
	toCenter := center.Sub(origin)
	dist2 := toCenter.Dot(toCenter)
	if math.IsInf(dist2, 0) || math.IsNaN(dist2) || !(dist2 > radius*radius) {
		// origin is inside or on the sphere
		return Vector{}, 0, 0, false
	}
	dist := math.Sqrt(dist2)
	w := toCenter.DivScalar(dist)

	// Cone half-angle: sinThetaMax = radius / dist. Compute 1 - cosThetaMax
	// as sin^2 / (1 + cos) so distant small lights don't lose it to
	// cancellation.
	sinThetaMax := radius / dist
	cosThetaMax := math.Sqrt(1 - sinThetaMax*sinThetaMax)
	oneMinusCosMax := (sinThetaMax * sinThetaMax) / (1 + cosThetaMax)

	// cosTheta is uniform in [cosThetaMax, 1]; u = 0 gives the cone axis.
	a := u * oneMinusCosMax
	cosTheta := 1 - a
	sinTheta := math.Sqrt(a * (2 - a))
	phi := 2 * math.Pi * v

	t1, t2 := coneBasis(w)
	direction = w.MulScalar(cosTheta).
		Add(t1.MulScalar(sinTheta * math.Cos(phi))).
		Add(t2.MulScalar(sinTheta * math.Sin(phi))).
		Normalize()

	// Distance to the first sphere intersection along direction:
	// t = dist*cosTheta - sqrt(r^2 - dist^2*sin^2Theta). Written this way
	// (rather than as a quadratic in t) to avoid subtracting two ~dist^2
	// numbers for far-away lights.
	perp2 := radius*radius - dist2*sinTheta*sinTheta
	if perp2 < 0 {
		perp2 = 0 // tangent within floating-point error
	}
	distance = dist*cosTheta - math.Sqrt(perp2)
	pdf = 1 / (2 * math.Pi * oneMinusCosMax)
	return direction, distance, pdf, true
}

// coneBasis builds a deterministic orthonormal basis around unit vector w.
// The reference axis is the coordinate axis least aligned with w, so the
// cross product stays well conditioned even when w hugs an axis.
func coneBasis(w Vector) (t1, t2 Vector) {
	t1 = w.Cross(w.MinAxis()).Normalize()
	t2 = w.Cross(t1)
	return t1, t2
}

func finiteVector(v Vector) bool {
	for _, x := range []float64{v.X, v.Y, v.Z} {
		if math.IsNaN(x) || math.IsInf(x, 0) {
			return false
		}
	}
	return true
}
