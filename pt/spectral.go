package pt

// Spectral transport extension.
//
// The classic RGB path tracer (DefaultSampler + Material) shares one
// refraction direction for all three color channels, so it cannot
// reproduce dispersive effects. This file adds a SpectralSampler that
// can be handed to the existing Renderer through the Sampler interface.
// Each Sample call traces a single continuous wavelength, follows the
// wavelength-dependent index of refraction through dielectric media and
// finally integrates against the sensor response curves.
//
// Interface organization:
//
//	Spectrum          piecewise-linear spectral distribution on strictly
//	                  increasing nanometer nodes inside [400, 700]; linear
//	                  between nodes, zero outside. The zero value is the
//	                  identically-zero spectrum.
//	Sensor            three non-negative response spectra (R, G, B).
//	SpectralMaterial  per-shape scattering: Lambert, Mirror or Dielectric,
//	                  plus an optional emission spectrum.
//	SpectralSampler   implements Sampler. Constructed with NewSpectralSampler
//	                  which binds a static Scene, a per-Shape material map,
//	                  an environment spectrum, a Sensor and MaxBounces.
//	Trace             public fixed-wavelength tracing entry point.
//	SampleWavelength  draws a wavelength with density proportional to the
//	WavelengthPDF     summed sensor responses (exact piecewise integration).
//
// The sampler never guesses a spectrum from RGB colors and never calls
// the RGB direct-lighting code; emission and environment are both
// collected by the same random walk.

import (
	"fmt"
	"math"
	"math/rand"
	"sort"
)

// Spectrum is a piecewise-linear spectral distribution. Wavelengths are
// nanometers, strictly increasing, inside [400, 700]; Values are finite
// and non-negative. Outside the node range the spectrum is zero. The
// zero value (no nodes) is the identically-zero spectrum.
type Spectrum struct {
	Wavelengths []float64
	Values      []float64
}

// NewSpectrum validates the nodes and returns a Spectrum holding its own
// copies of the input slices.
func NewSpectrum(wavelengths, values []float64) (Spectrum, error) {
	if len(wavelengths) != len(values) {
		return Spectrum{}, fmt.Errorf("spectral: %d wavelengths but %d values", len(wavelengths), len(values))
	}
	if err := validateSpectrumNodes(wavelengths, values); err != nil {
		return Spectrum{}, err
	}
	return Spectrum{cloneFloats(wavelengths), cloneFloats(values)}, nil
}

func validateSpectrumNodes(w, v []float64) error {
	if len(w) < 2 {
		return fmt.Errorf("spectral: need at least 2 nodes, got %d", len(w))
	}
	for i := range w {
		if math.IsNaN(w[i]) || math.IsInf(w[i], 0) {
			return fmt.Errorf("spectral: wavelength %d is not finite", i)
		}
		if w[i] < 400 || w[i] > 700 {
			return fmt.Errorf("spectral: wavelength %g outside [400, 700]", w[i])
		}
		if i > 0 && w[i] <= w[i-1] {
			return fmt.Errorf("spectral: wavelengths not strictly increasing (%g after %g)", w[i], w[i-1])
		}
		if math.IsNaN(v[i]) || math.IsInf(v[i], 0) || v[i] < 0 {
			return fmt.Errorf("spectral: value %d (%g) must be finite and non-negative", i, v[i])
		}
	}
	return nil
}

func cloneFloats(x []float64) []float64 {
	return append([]float64(nil), x...)
}

func (s Spectrum) isZero() bool {
	return len(s.Wavelengths) == 0
}

func (s Spectrum) clone() Spectrum {
	if s.isZero() {
		return Spectrum{}
	}
	return Spectrum{cloneFloats(s.Wavelengths), cloneFloats(s.Values)}
}

// Value evaluates the piecewise-linear spectrum at lambda (nanometers).
func (s Spectrum) Value(lambda float64) float64 {
	n := len(s.Wavelengths)
	if n == 0 || lambda < s.Wavelengths[0] || lambda > s.Wavelengths[n-1] {
		return 0
	}
	i := sort.Search(n, func(i int) bool { return s.Wavelengths[i] >= lambda })
	if s.Wavelengths[i] == lambda {
		return s.Values[i]
	}
	w0, w1 := s.Wavelengths[i-1], s.Wavelengths[i]
	v0, v1 := s.Values[i-1], s.Values[i]
	return v0 + (v1-v0)*(lambda-w0)/(w1-w0)
}

// Integral returns the exact integral of the spectrum over wavelength.
func (s Spectrum) Integral() float64 {
	total := 0.0
	for i := 0; i+1 < len(s.Wavelengths); i++ {
		total += 0.5 * (s.Values[i] + s.Values[i+1]) * (s.Wavelengths[i+1] - s.Wavelengths[i])
	}
	return total
}

// Sensor holds the three non-negative response spectra used to integrate
// scalar spectral radiance into output channels.
type Sensor struct {
	R, G, B Spectrum
}

// NewSensor validates the response spectra and returns a Sensor holding
// its own copies. The sum of the three response integrals must be > 0.
func NewSensor(r, g, b Spectrum) (Sensor, error) {
	s := Sensor{r.clone(), g.clone(), b.clone()}
	if err := validateSensor(s); err != nil {
		return Sensor{}, err
	}
	return s, nil
}

func validateSensor(s Sensor) error {
	for _, sp := range []Spectrum{s.R, s.G, s.B} {
		if sp.isZero() {
			continue
		}
		if err := validateSpectrumNodes(sp.Wavelengths, sp.Values); err != nil {
			return fmt.Errorf("spectral: sensor: %v", err)
		}
	}
	if s.R.Integral()+s.G.Integral()+s.B.Integral() <= 0 {
		return fmt.Errorf("spectral: sensor response integrals sum to zero")
	}
	return nil
}

func (s Sensor) clone() Sensor {
	return Sensor{s.R.clone(), s.G.clone(), s.B.clone()}
}

// ScatterType selects the scattering model of a SpectralMaterial.
type ScatterType int

const (
	// Lambert scatters cosine-weighted and scales throughput by the albedo.
	Lambert ScatterType = iota
	// Mirror reflects geometrically and scales throughput by the reflectance.
	Mirror
	// Dielectric refracts without absorption using the exact unpolarized
	// Fresnel equations; only spheres may be dielectric.
	Dielectric
)

// SpectralMaterial describes how one Shape scatters a single wavelength.
// Albedo is the Lambert albedo or mirror reflectance (values <= 1).
// Emission is an optional emission spectrum added at every hit.
// A and B are the Cauchy coefficients of a dielectric:
// n(lambda) = A + B / (lambda/1000)^2, with A >= 1, B >= 0, both finite.
type SpectralMaterial struct {
	Scatter  ScatterType
	Albedo   Spectrum
	Emission Spectrum
	A, B     float64
}

// LambertMaterial returns a diffuse spectral material.
func LambertMaterial(albedo, emission Spectrum) SpectralMaterial {
	return SpectralMaterial{Scatter: Lambert, Albedo: albedo, Emission: emission}
}

// MirrorMaterial returns an ideal specular spectral material.
func MirrorMaterial(reflectance, emission Spectrum) SpectralMaterial {
	return SpectralMaterial{Scatter: Mirror, Albedo: reflectance, Emission: emission}
}

// DielectricMaterial returns an absorption-free dielectric with
// n(lambda) = a + b/(lambda/1000)^2.
func DielectricMaterial(a, b float64, emission Spectrum) SpectralMaterial {
	return SpectralMaterial{Scatter: Dielectric, A: a, B: b, Emission: emission}
}

// Index returns the refractive index of a dielectric at lambda (nm).
// Non-dielectric materials have index 1.
func (m SpectralMaterial) Index(lambda float64) float64 {
	if m.Scatter != Dielectric {
		return 1
	}
	x := lambda / 1000
	return m.A + m.B/(x*x)
}

func (m SpectralMaterial) clone() SpectralMaterial {
	m.Albedo = m.Albedo.clone()
	m.Emission = m.Emission.clone()
	return m
}

func validateSpectralMaterial(shape Shape, m SpectralMaterial) error {
	switch shape.(type) {
	case *Sphere, *Plane, *Triangle:
	default:
		return fmt.Errorf("spectral: unsupported shape type %T (want *Sphere, *Plane or *Triangle)", shape)
	}
	switch m.Scatter {
	case Lambert, Mirror:
		if !m.Albedo.isZero() {
			if err := validateSpectrumNodes(m.Albedo.Wavelengths, m.Albedo.Values); err != nil {
				return fmt.Errorf("spectral: albedo: %v", err)
			}
			for _, v := range m.Albedo.Values {
				if v > 1 {
					return fmt.Errorf("spectral: reflectance %g exceeds 1", v)
				}
			}
		}
	case Dielectric:
		if _, ok := shape.(*Sphere); !ok {
			return fmt.Errorf("spectral: dielectric material requires a *Sphere, got %T", shape)
		}
		if math.IsNaN(m.A) || math.IsInf(m.A, 0) || m.A < 1 {
			return fmt.Errorf("spectral: dielectric A must be finite and >= 1, got %g", m.A)
		}
		if math.IsNaN(m.B) || math.IsInf(m.B, 0) || m.B < 0 {
			return fmt.Errorf("spectral: dielectric B must be finite and >= 0, got %g", m.B)
		}
	default:
		return fmt.Errorf("spectral: unknown scatter type %d", m.Scatter)
	}
	if !m.Emission.isZero() {
		if err := validateSpectrumNodes(m.Emission.Wavelengths, m.Emission.Values); err != nil {
			return fmt.Errorf("spectral: emission: %v", err)
		}
	}
	return nil
}

// dielectricSphere is a dielectric *Sphere plus its Cauchy coefficients.
type dielectricSphere struct {
	sphere *Sphere
	a, b   float64
}

func (d dielectricSphere) index(lambda float64) float64 {
	x := lambda / 1000
	return d.a + d.b/(x*x)
}

// SpectralSampler traces one wavelength per sample through a static scene.
// It implements the Sampler interface and keeps no random state of its
// own; every random number comes from the rnd passed by the caller.
type SpectralSampler struct {
	scene           *Scene
	materials       map[Shape]SpectralMaterial
	env             Spectrum
	sensor          Sensor
	maxBounces      int
	dielectrics     []dielectricSphere
	dielectricIndex map[Shape]int
	nodes           []float64 // union of sensor node wavelengths
	sum             []float64 // summed sensor response at nodes
	cum             []float64 // cumulative integral of sum up to nodes[i]
	total           float64   // cum[len-1], > 0
}

// NewSpectralSampler binds a static Scene, a per-Shape spectral material
// map, an environment spectrum, a Sensor and MaxBounces (the maximum
// number of surface events per path; 0 allows only environment escape).
// All inputs are validated and copied; invalid data returns an error.
// Dielectric spheres must be pairwise strictly separate or strictly
// nested; intersecting or tangent configurations are rejected.
func NewSpectralSampler(scene *Scene, materials map[Shape]SpectralMaterial, env Spectrum, sensor Sensor, maxBounces int) (*SpectralSampler, error) {
	if scene == nil {
		return nil, fmt.Errorf("spectral: nil scene")
	}
	if maxBounces < 0 {
		return nil, fmt.Errorf("spectral: negative MaxBounces %d", maxBounces)
	}
	if !env.isZero() {
		if err := validateSpectrumNodes(env.Wavelengths, env.Values); err != nil {
			return nil, fmt.Errorf("spectral: environment: %v", err)
		}
	}
	if err := validateSensor(sensor); err != nil {
		return nil, err
	}
	s := &SpectralSampler{
		scene:           scene,
		materials:       make(map[Shape]SpectralMaterial, len(materials)),
		env:             env.clone(),
		sensor:          sensor.clone(),
		maxBounces:      maxBounces,
		dielectricIndex: make(map[Shape]int),
	}
	for _, shape := range scene.Shapes {
		m, ok := materials[shape]
		if !ok {
			return nil, fmt.Errorf("spectral: no spectral material for shape %T", shape)
		}
		if err := validateSpectralMaterial(shape, m); err != nil {
			return nil, err
		}
		s.materials[shape] = m.clone()
		if m.Scatter == Dielectric {
			s.dielectricIndex[shape] = len(s.dielectrics)
			s.dielectrics = append(s.dielectrics, dielectricSphere{shape.(*Sphere), m.A, m.B})
		}
	}
	for i := 0; i < len(s.dielectrics); i++ {
		for j := i + 1; j < len(s.dielectrics); j++ {
			a, b := s.dielectrics[i].sphere, s.dielectrics[j].sphere
			d := a.Center.Sub(b.Center).Length()
			if d <= a.Radius+b.Radius && d >= math.Abs(a.Radius-b.Radius) {
				return nil, fmt.Errorf("spectral: dielectric spheres %d and %d intersect or touch (d=%g, r=%g, %g)", i, j, d, a.Radius, b.Radius)
			}
		}
	}
	// Exact wavelength distribution: p(lambda) proportional to the summed
	// sensor responses, integrated segment by segment over the union of
	// the sensor nodes.
	set := make(map[float64]bool)
	for _, sp := range []Spectrum{s.sensor.R, s.sensor.G, s.sensor.B} {
		for _, w := range sp.Wavelengths {
			set[w] = true
		}
	}
	nodes := make([]float64, 0, len(set))
	for w := range set {
		nodes = append(nodes, w)
	}
	sort.Float64s(nodes)
	sum := make([]float64, len(nodes))
	for i, w := range nodes {
		sum[i] = s.sensor.R.Value(w) + s.sensor.G.Value(w) + s.sensor.B.Value(w)
	}
	cum := make([]float64, len(nodes))
	for i := 1; i < len(nodes); i++ {
		cum[i] = cum[i-1] + 0.5*(sum[i-1]+sum[i])*(nodes[i]-nodes[i-1])
	}
	s.nodes, s.sum, s.cum = nodes, sum, cum
	s.total = cum[len(cum)-1]
	if s.total <= 0 {
		return nil, fmt.Errorf("spectral: sensor response integrals sum to zero")
	}
	scene.Compile()
	return s, nil
}

// SampleWavelength draws one wavelength with density proportional to the
// summed sensor responses and returns it together with its PDF value
// (per nanometer). The CDF is inverted analytically: inside each node
// segment the summed response is linear, so the cumulative integral is
// quadratic and solved in closed form.
func (s *SpectralSampler) SampleWavelength(rnd *rand.Rand) (float64, float64) {
	u := rnd.Float64() * s.total
	i := sort.Search(len(s.cum), func(j int) bool { return s.cum[j] > u }) - 1
	if i < 0 {
		i = 0
	}
	if i > len(s.nodes)-2 {
		i = len(s.nodes) - 2
	}
	w0, w1 := s.nodes[i], s.nodes[i+1]
	s0, s1 := s.sum[i], s.sum[i+1]
	t := u - s.cum[i]
	k := (s1 - s0) / (w1 - w0)
	var x float64
	if k == 0 {
		x = t / s0
	} else {
		// solve s0*x + k*x^2/2 = t with the numerically stable form
		disc := s0*s0 + 2*k*t
		if disc < 0 {
			disc = 0
		}
		x = 2 * t / (s0 + math.Sqrt(disc))
	}
	lambda := w0 + x
	return lambda, s.WavelengthPDF(lambda)
}

// WavelengthPDF returns the sampling density (per nanometer) of
// SampleWavelength at lambda.
func (s *SpectralSampler) WavelengthPDF(lambda float64) float64 {
	if lambda < s.nodes[0] || lambda > s.nodes[len(s.nodes)-1] {
		return 0
	}
	r := s.sensor.R.Value(lambda) + s.sensor.G.Value(lambda) + s.sensor.B.Value(lambda)
	return r / s.total
}

// Sample implements the Sampler interface: one wavelength is drawn with
// density p, the path is traced at that wavelength, and the scalar
// radiance L is returned as L*(Sr,Sg,Sb)/p with no extra normalization.
// The caller guarantees that scene is the bound scene, ray.Direction is
// unit length and rnd is non-nil.
func (s *SpectralSampler) Sample(scene *Scene, ray Ray, rnd *rand.Rand) Color {
	lambda, pdf := s.SampleWavelength(rnd)
	l := s.Trace(ray, lambda, rnd)
	scale := l / pdf
	return Color{
		R: scale * s.sensor.R.Value(lambda),
		G: scale * s.sensor.G.Value(lambda),
		B: scale * s.sensor.B.Value(lambda),
	}
}

// Trace follows one path at a single wavelength and returns the scalar
// spectral radiance. Emission is added at every surface hit and the
// environment spectrum is collected when the path escapes; both are
// handled by the same random walk (no separate direct-light estimate,
// no Russian roulette). At most MaxBounces surface events are processed.
func (s *SpectralSampler) Trace(ray Ray, lambda float64, rnd *rand.Rand) float64 {
	stack := s.mediumStack(ray.Origin)
	throughput := 1.0
	radiance := 0.0
	for bounce := 0; ; bounce++ {
		hit := s.scene.Intersect(ray)
		if !hit.Ok() {
			radiance += throughput * s.env.Value(lambda)
			break
		}
		if bounce >= s.maxBounces {
			break
		}
		m := s.materials[hit.Shape]
		radiance += throughput * m.Emission.Value(lambda)
		p := ray.Position(hit.T)
		switch m.Scatter {
		case Mirror:
			n := orientedNormal(hit.Shape, p, ray.Direction)
			throughput *= m.Albedo.Value(lambda)
			d := n.Reflect(ray.Direction)
			ray = Ray{p.Add(d.MulScalar(1e-4)), d}
		case Dielectric:
			idx := s.dielectricIndex[hit.Shape]
			n := orientedNormal(hit.Shape, p, ray.Direction)
			ni := 1.0
			if len(stack) > 0 {
				ni = s.dielectrics[stack[len(stack)-1]].index(lambda)
			}
			// The hit sphere is either the innermost medium (exit) or a
			// sphere outside the stack (entry); strict nesting forbids
			// any other case.
			entering := len(stack) == 0 || stack[len(stack)-1] != idx
			nt := 1.0
			if entering {
				nt = s.dielectrics[idx].index(lambda)
			} else if len(stack) > 1 {
				nt = s.dielectrics[stack[len(stack)-2]].index(lambda)
			}
			cosI := -n.Dot(ray.Direction)
			eta := ni / nt
			sinT2 := eta * eta * (1 - cosI*cosI)
			reflect := sinT2 >= 1 // total internal reflection
			if !reflect {
				cosT := math.Sqrt(1 - sinT2)
				rOrth := (ni*cosI - nt*cosT) / (ni*cosI + nt*cosT)
				rPar := (nt*cosI - ni*cosT) / (nt*cosI + ni*cosT)
				f := (rOrth*rOrth + rPar*rPar) / 2
				reflect = rnd.Float64() < f
			}
			if reflect {
				// Reflection never changes the medium stack.
				d := n.Reflect(ray.Direction)
				ray = Ray{p.Add(d.MulScalar(1e-4)), d}
			} else {
				// Transmission crosses the boundary; the Fresnel
				// selection probability cancels, only the radiance
				// scaling (ni/nt)^2 remains.
				cosT := math.Sqrt(1 - sinT2)
				d := ray.Direction.MulScalar(eta).Add(n.MulScalar(eta*cosI - cosT))
				throughput *= eta * eta
				if entering {
					stack = append(stack, idx)
				} else {
					stack = stack[:len(stack)-1]
				}
				ray = Ray{p.Add(d.MulScalar(1e-4)), d}
			}
		default: // Lambert
			n := orientedNormal(hit.Shape, p, ray.Direction)
			throughput *= m.Albedo.Value(lambda)
			d := Ray{p, n}.WeightedBounce(rnd.Float64(), rnd.Float64(), rnd).Direction
			ray = Ray{p.Add(d.MulScalar(1e-4)), d}
		}
	}
	return radiance
}

// mediumStack returns the indices of the dielectric spheres containing
// point p, outermost first, so the last element is the current medium.
func (s *SpectralSampler) mediumStack(p Vector) []int {
	var stack []int
	for i, d := range s.dielectrics {
		if p.Sub(d.sphere.Center).Length() < d.sphere.Radius {
			stack = append(stack, i)
		}
	}
	sort.Slice(stack, func(a, b int) bool {
		return s.dielectrics[stack[a]].sphere.Radius > s.dielectrics[stack[b]].sphere.Radius
	})
	return stack
}

// orientedNormal returns the shape normal at p flipped to oppose d.
func orientedNormal(shape Shape, p, d Vector) Vector {
	n := shape.NormalAt(p)
	if n.Dot(d) > 0 {
		n = n.Negate()
	}
	return n
}
