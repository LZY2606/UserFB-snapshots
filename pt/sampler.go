package pt

import (
	"math"
	"math/rand"
)

type LightMode int

const (
	LightModeRandom = iota
	LightModeAll
)

type SpecularMode int

const (
	SpecularModeNaive = iota
	SpecularModeFirst
	SpecularModeAll
)

type BounceType int

const (
	BounceTypeAny = iota
	BounceTypeDiffuse
	BounceTypeSpecular
)

type Sampler interface {
	Sample(scene *Scene, ray Ray, rnd *rand.Rand) Color
}

func NewSampler(firstHitSamples, maxBounces int) *DefaultSampler {
	return &DefaultSampler{firstHitSamples, maxBounces, true, true, LightModeRandom, SpecularModeNaive, false}
}

func NewDirectSampler() *DefaultSampler {
	return &DefaultSampler{1, 0, true, false, LightModeAll, SpecularModeAll, false}
}

type DefaultSampler struct {
	FirstHitSamples int
	MaxBounces      int
	DirectLighting  bool
	SoftShadows     bool
	LightMode       LightMode
	SpecularMode    SpecularMode
	EnvironmentMIS  bool
}

func (s *DefaultSampler) Sample(scene *Scene, ray Ray, rnd *rand.Rand) Color {
	return s.sample(scene, ray, true, s.FirstHitSamples, 0, rnd)
}

func (s *DefaultSampler) sample(scene *Scene, ray Ray, emission bool, samples, depth int, rnd *rand.Rand) Color {
	if depth > s.MaxBounces {
		return Black
	}
	hit := scene.Intersect(ray)
	if !hit.Ok() {
		return s.sampleEnvironment(scene, ray)
	}
	return s.shade(scene, ray, hit, emission, samples, depth, rnd)
}

func (s *DefaultSampler) shade(scene *Scene, ray Ray, hit Hit, emission bool, samples, depth int, rnd *rand.Rand) Color {
	info := hit.Info(ray)
	material := info.Material
	result := Black
	if material.Emittance > 0 {
		if s.DirectLighting && !emission {
			return Black
		}
		result = result.Add(material.Color.MulScalar(material.Emittance * float64(samples)))
	}
	n := int(math.Sqrt(float64(samples)))
	var ma, mb BounceType
	if s.SpecularMode == SpecularModeAll || (depth == 0 && s.SpecularMode == SpecularModeFirst) {
		ma = BounceTypeDiffuse
		mb = BounceTypeSpecular
	} else {
		ma = BounceTypeAny
		mb = BounceTypeAny
	}
	for u := 0; u < n; u++ {
		for v := 0; v < n; v++ {
			for mode := ma; mode <= mb; mode++ {
				fu := (float64(u) + rnd.Float64()) / float64(n)
				fv := (float64(v) + rnd.Float64()) / float64(n)
				newRay, reflected, p := ray.Bounce(&info, fu, fv, mode, rnd)
				if mode == BounceTypeAny {
					p = 1
				}
				if p > 0 && reflected {
					// specular
					indirect := s.sample(scene, newRay, reflected, 1, depth+1, rnd)
					tinted := indirect.Mix(material.Color.Mul(indirect), material.Tint)
					result = result.Add(tinted.MulScalar(p))
				}
				if p > 0 && !reflected {
					// diffuse
					var indirect, envDirect Color
					if s.EnvironmentMIS && s.DirectLighting && scene.Environment != nil {
						envDirect, indirect = s.sampleEnvironmentMIS(scene, info, newRay, depth, rnd)
					} else {
						indirect = s.sample(scene, newRay, reflected, 1, depth+1, rnd)
					}
					direct := Black
					if s.DirectLighting {
						direct = s.sampleLights(scene, info.Ray, rnd)
					}
					direct = direct.Add(envDirect)
					result = result.Add(material.Color.Mul(direct.Add(indirect)).MulScalar(p))
				}
			}
		}
	}
	return result.DivScalar(float64(n * n))
}

func (s *DefaultSampler) sampleEnvironment(scene *Scene, ray Ray) Color {
	if scene.Environment != nil {
		return scene.Environment.Evaluate(ray.Direction, scene.TextureAngle)
	}
	if scene.Texture != nil {
		d := ray.Direction
		u := math.Atan2(d.Z, d.X) + scene.TextureAngle
		v := math.Atan2(d.Y, Vector{d.X, 0, d.Z}.Length())
		u = (u + math.Pi) / (2 * math.Pi)
		v = (v + math.Pi/2) / math.Pi
		return scene.Texture.Sample(u, v)
	}
	return scene.Color
}

// sampleEnvironmentMIS combines one environment-map direction sample with
// the cosine-weighted bounce ray using the power heuristic (exponent 2).
// The cosine strategy keeps its conditional density max(N.d,0)/pi. When
// the bounce would exceed the depth limit only the environment strategy
// remains and it gets weight 1.
func (s *DefaultSampler) sampleEnvironmentMIS(scene *Scene, info HitInfo, bounceRay Ray, depth int, rnd *rand.Rand) (direct, indirect Color) {
	env := scene.Environment
	normal := info.Ray.Direction
	paired := depth+1 <= s.MaxBounces
	// cosine-weighted bounce strategy
	if paired {
		hit := scene.Intersect(bounceRay)
		if !hit.Ok() {
			// the bounce escaped to the environment: apply the
			// complementary MIS weight for this direction
			d := bounceRay.Direction
			pCos := math.Max(d.Dot(normal), 0) / math.Pi
			pEnv := env.PDF(d, scene.TextureAngle)
			w := pCos * pCos / (pCos*pCos + pEnv*pEnv)
			indirect = s.sampleEnvironment(scene, bounceRay).MulScalar(w)
		} else {
			// the bounce hit another object: its lighting is
			// estimated recursively and must not be darkened by
			// the environment MIS weights
			indirect = s.shade(scene, bounceRay, hit, false, 1, depth+1, rnd)
		}
	}
	// environment direction sampling strategy
	d, radiance, pEnv := env.Sample(rnd, scene.TextureAngle)
	cos := d.Dot(normal)
	if cos > 0 && radiance.MaxComponent() > 0 {
		shadowRay := Ray{info.Ray.Origin, d}
		if shadowHit := scene.Intersect(shadowRay); !shadowHit.Ok() {
			w := 1.0
			if paired {
				pCos := cos / math.Pi
				w = pEnv * pEnv / (pEnv*pEnv + pCos*pCos)
			}
			// Lambert BRDF (albedo/pi) times cosine; the albedo is
			// applied by the caller with the material color
			direct = radiance.MulScalar(cos * w / (math.Pi * pEnv))
		}
	}
	return direct, indirect
}

func (s *DefaultSampler) sampleLights(scene *Scene, n Ray, rnd *rand.Rand) Color {
	nLights := len(scene.Lights)
	if nLights == 0 {
		return Black
	}

	if s.LightMode == LightModeAll {
		var result Color
		for _, light := range scene.Lights {
			result = result.Add(s.sampleLight(scene, n, rnd, light))
		}
		return result
	} else {
		// pick a random light
		light := scene.Lights[rand.Intn(nLights)]
		return s.sampleLight(scene, n, rnd, light).MulScalar(float64(nLights))
	}
}

func (s *DefaultSampler) sampleLight(scene *Scene, n Ray, rnd *rand.Rand, light Shape) Color {
	// get bounding sphere center and radius
	var center Vector
	var radius float64
	switch t := light.(type) {
	case *Sphere:
		radius = t.Radius
		center = t.Center
	default:
		// get bounding sphere from bounding box
		box := t.BoundingBox()
		radius = box.OuterRadius()
		center = box.Center()
	}

	// get random point in disk
	point := center
	if s.SoftShadows {
		for {
			x := rnd.Float64()*2 - 1
			y := rnd.Float64()*2 - 1
			if x*x+y*y <= 1 {
				l := center.Sub(n.Origin).Normalize()
				u := l.Cross(RandomUnitVector(rnd)).Normalize()
				v := l.Cross(u)
				point = Vector{}
				point = point.Add(u.MulScalar(x * radius))
				point = point.Add(v.MulScalar(y * radius))
				point = point.Add(center)
				break
			}
		}
	}

	// construct ray toward light point
	ray := Ray{n.Origin, point.Sub(n.Origin).Normalize()}

	// get cosine term
	diffuse := ray.Direction.Dot(n.Direction)
	if diffuse <= 0 {
		return Black
	}

	// check for light visibility
	hit := scene.Intersect(ray)
	if !hit.Ok() || hit.Shape != light {
		return Black
	}

	// compute solid angle (hemisphere coverage)
	hyp := center.Sub(n.Origin).Length()
	opp := radius
	theta := math.Asin(opp / hyp)
	adj := opp / math.Tan(theta)
	d := math.Cos(theta) * adj
	r := math.Sin(theta) * adj
	coverage := (r * r) / (d * d)

	// TODO: fix issue where hyp < opp (point inside sphere)
	if hyp < opp {
		coverage = 1
	}
	coverage = math.Min(coverage, 1)

	// get material properties from light
	material := MaterialAt(light, point)

	// combine factors
	m := material.Emittance * diffuse * coverage
	return material.Color.MulScalar(m)
}
