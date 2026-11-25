package pt

import (
	"errors"
	"math"
	"math/rand"
	"sort"
)

// Spectral rendering
//
// A SpectralSampler traces one wavelength per Sample call. At that wavelength
// every dielectric interface has its own refractive index, so different
// wavelengths follow genuinely different optical paths (real dispersion).
// The wavelength is drawn with density p(lambda), proportional to the sum of
// the three sensor response spectra; the scalar path radiance L is converted
// to a sensor color with L * (Sr, Sg, Sb) / p(lambda). This is a sensor
// response integral: it is neither three separate RGB-filter renders nor an
// RGB-to-spectrum guess.
//
// The public surface is organized as:
//
//   - Spectrum / NewSpectrum: validated piecewise linear spectral functions.
//   - Sensor / NewSensor: three non-negative response spectra.
//   - SpectralMaterial / SpectralScatter: wavelength-dependent surfaces;
//     dielectrics carry their Cauchy coefficients A, B on the material.
//   - NewSpectralSampler: binds one static Scene, a per-Shape material map,
//     an environment spectrum, a Sensor and MaxBounces, and returns an
//     object implementing the existing Sampler interface.
//   - SampleWavelength / WavelengthPDF: exact wavelength measure queries.
//   - TraceWavelength / TraceWavelengthPath: fixed-wavelength transport,
//     the latter returning every surface event, so optical paths can be
//     checked independently of the sensor weighting.
//
// Everything in this file uses only the Go standard library. The sampler
// stores no random state; every random number comes from the *rand.Rand
// passed to the trace entry.

const (
	spectralMinWavelength = 400.0
	spectralMaxWavelength = 700.0
	// spectralShift moves a ray off a surface after an event. It only prevents
	// self-intersection; the medium stack and sphere identity are tracked
	// explicitly and never inferred from the shifted origin.
	spectralShift = 1e-7
)

// SpectralScatter selects how a SpectralMaterial interacts at a surface.
type SpectralScatter int

const (
	// SpectralLambert is a perfect Lambertian diffuse surface.
	SpectralLambert SpectralScatter = iota
	// SpectralMirror is an ideal specular reflector.
	SpectralMirror
	// SpectralDielectric is an ideal, non-absorbing dielectric (spheres only).
	SpectralDielectric
)

// Spectrum is a piecewise linear spectral function over nanometer
// wavelengths. It is linear between nodes and zero outside the node range.
// Node wavelengths must be strictly increasing and lie in [400, 700].
type Spectrum struct {
	wavelengths []float64
	values      []float64
}

// NewSpectrum validates and stores a piecewise linear spectrum. It copies the
// input slices, so later mutation by the caller cannot affect the result.
func NewSpectrum(wavelengths, values []float64) (Spectrum, error) {
	if len(wavelengths) < 2 || len(wavelengths) != len(values) {
		return Spectrum{}, errors.New("pt: spectrum needs at least two matching wavelength/value points")
	}
	ws := append([]float64(nil), wavelengths...)
	vs := append([]float64(nil), values...)
	for i, w := range ws {
		if math.IsNaN(w) || math.IsInf(w, 0) || w < spectralMinWavelength || w > spectralMaxWavelength {
			return Spectrum{}, errors.New("pt: spectrum wavelength must be finite and within [400, 700] nm")
		}
		if i > 0 && w <= ws[i-1] {
			return Spectrum{}, errors.New("pt: spectrum wavelengths must be strictly increasing")
		}
		if math.IsNaN(vs[i]) || math.IsInf(vs[i], 0) || vs[i] < 0 {
			return Spectrum{}, errors.New("pt: spectrum values must be finite and non-negative")
		}
	}
	return Spectrum{ws, vs}, nil
}

// Wavelengths returns a copy of the node wavelengths in nanometers.
func (s Spectrum) Wavelengths() []float64 {
	return append([]float64(nil), s.wavelengths...)
}

// Values returns a copy of the node values.
func (s Spectrum) Values() []float64 {
	return append([]float64(nil), s.values...)
}

// Len reports the number of nodes.
func (s Spectrum) Len() int {
	return len(s.wavelengths)
}

// At evaluates the piecewise linear function. It is zero outside the node
// range, including outside [400, 700].
func (s Spectrum) At(wavelength float64) float64 {
	if len(s.wavelengths) == 0 || wavelength < s.wavelengths[0] || wavelength > s.wavelengths[len(s.wavelengths)-1] {
		return 0
	}
	i := sort.SearchFloat64s(s.wavelengths, wavelength)
	if i >= len(s.wavelengths) {
		return s.values[len(s.values)-1]
	}
	if s.wavelengths[i] == wavelength {
		return s.values[i]
	}
	// i is the first node strictly above the wavelength.
	w0, w1 := s.wavelengths[i-1], s.wavelengths[i]
	v0, v1 := s.values[i-1], s.values[i]
	t := (wavelength - w0) / (w1 - w0)
	return v0 + t*(v1-v0)
}

// Integral returns the exact integral over [400, 700], summing trapezoid
// areas node interval by node interval. Parts outside the node range are zero.
func (s Spectrum) Integral() float64 {
	total := 0.0
	for i := 1; i < len(s.wavelengths); i++ {
		total += 0.5 * (s.values[i-1] + s.values[i]) * (s.wavelengths[i] - s.wavelengths[i-1])
	}
	return total
}

func (s Spectrum) copy() Spectrum {
	if len(s.wavelengths) == 0 {
		return Spectrum{}
	}
	return Spectrum{
		wavelengths: append([]float64(nil), s.wavelengths...),
		values:      append([]float64(nil), s.values...),
	}
}

// Sensor holds three non-negative spectral response functions, one per
// channel. The sum of their integrals must be positive.
type Sensor struct {
	red, green, blue Spectrum
}

// NewSensor copies and validates the three response spectra and requires the
// sum of their integrals to be positive.
func NewSensor(red, green, blue Spectrum) (Sensor, error) {
	if len(red.wavelengths) == 0 || len(green.wavelengths) == 0 || len(blue.wavelengths) == 0 {
		return Sensor{}, errors.New("pt: sensor channels must be valid spectra")
	}
	sensor := Sensor{red.copy(), green.copy(), blue.copy()}
	if sensor.responseIntegral() <= 0 {
		return Sensor{}, errors.New("pt: sensor response integrals must sum to a positive value")
	}
	return sensor, nil
}

// Red, Green and Blue return copies of the channel response spectra.
func (s Sensor) Red() Spectrum   { return s.red.copy() }
func (s Sensor) Green() Spectrum { return s.green.copy() }
func (s Sensor) Blue() Spectrum  { return s.blue.copy() }

// responseAt returns (Sr, Sg, Sb) at one wavelength.
func (s Sensor) responseAt(wavelength float64) (float64, float64, float64) {
	return s.red.At(wavelength), s.green.At(wavelength), s.blue.At(wavelength)
}

// responseIntegral is the exact integral over [400, 700] of the summed
// response, i.e. the normalization constant of the wavelength PDF.
func (s Sensor) responseIntegral() float64 {
	return s.red.Integral() + s.green.Integral() + s.blue.Integral()
}

func (s Sensor) copy() Sensor {
	return Sensor{s.red.copy(), s.green.copy(), s.blue.copy()}
}

// SpectralMaterial describes wavelength-dependent surface behavior.
//
// Albedo is used by SpectralLambert, Reflectance by SpectralMirror, and
// Emission may be attached to any scatter type (the zero spectrum means no
// self-emission). SpectralDielectric has no absorption and no tunable
// reflectance; IndexA/IndexB are its Cauchy coefficients
// n(lambda) = A + B/(lambda/1000)^2 and are only read in that mode.
type SpectralMaterial struct {
	Scatter     SpectralScatter
	Albedo      Spectrum // diffuse reflectance, node values in [0, 1]
	Reflectance Spectrum // specular reflectance, node values in [0, 1]
	Emission    Spectrum // self-emission, any finite non-negative values
	IndexA      float64  // Cauchy A, dielectric only; A >= 1
	IndexB      float64  // Cauchy B, dielectric only; B >= 0
}

func (m SpectralMaterial) copy() SpectralMaterial {
	return SpectralMaterial{
		Scatter:     m.Scatter,
		Albedo:      m.Albedo.copy(),
		Reflectance: m.Reflectance.copy(),
		Emission:    m.Emission.copy(),
		IndexA:      m.IndexA,
		IndexB:      m.IndexB,
	}
}

// SpectralEvent records one processed surface event of a fixed-wavelength
// walk, allowing optical path tests to inspect geometry and media without
// involving the sensor measure.
type SpectralEvent struct {
	Shape        Shape
	Wavelength   float64
	Position     Vector
	Normal       Vector
	Direction    Vector // outgoing direction after the event
	Entering     bool   // true when crossing into the hit sphere, false when leaving
	Reflection   bool
	Transmission bool
	StackDepth   int // dielectric stack depth after the event
	EtaI         float64
	EtaT         float64
	Fresnel      float64
	Emission     float64 // emission added at this hit, already scaled by throughput
}

// SpectralSampler implements the Sampler interface with single-wavelength
// spectral path tracing. A sampler is bound at construction to one static
// scene, one material per shape, one environment spectrum, one sensor and a
// maximum number of surface events.
type SpectralSampler struct {
	scene       *Scene
	materials   map[Shape]SpectralMaterial
	environment Spectrum
	sensor      Sensor
	maxBounces  int
	glass       map[*Sphere]cauchyCoeffs
	glassOrder  []*Sphere
	// Wavelength sampling: a piecewise linear CDF of p over the merged nodes
	// of the three sensor responses. Each segment stores its cumulative
	// probability and the coefficients of the linear density on it. The CDF
	// is inverted analytically per segment.
	cdfNodes []float64
	cdfValue []float64 // cumulative probability at each node, in [0, 1]
	cdfSlope []float64 // slope of the linear density on the segment to the right
	cdfNorm  float64   // integral of the summed response
}

type cauchyCoeffs struct{ a, b float64 }

// Compile-time check that the existing Sampler interface keeps working.
var _ Sampler = (*SpectralSampler)(nil)

// NewSpectralSampler binds a static (compiled) scene, a per-shape spectral
// material map, an environment spectrum, a sensor and a maximum number of
// surface events. The map and every spectrum are copied, so the returned
// sampler never aliases input slices or maps. Invalid data produces an error.
func NewSpectralSampler(scene *Scene, materials map[Shape]SpectralMaterial, environment Spectrum, sensor Sensor, maxBounces int) (*SpectralSampler, error) {
	if scene == nil {
		return nil, errors.New("pt: spectral sampler needs a scene")
	}
	if scene.tree == nil {
		return nil, errors.New("pt: spectral sampler scene must be compiled")
	}
	if maxBounces < 0 {
		return nil, errors.New("pt: MaxBounces must be non-negative")
	}
	if len(environment.wavelengths) == 0 {
		return nil, errors.New("pt: spectral sampler needs an environment spectrum")
	}
	if len(materials) != len(scene.Shapes) {
		return nil, errors.New("pt: every scene shape needs exactly one spectral material")
	}
	sampler := &SpectralSampler{
		scene:       scene,
		materials:   make(map[Shape]SpectralMaterial, len(materials)),
		environment: environment.copy(),
		sensor:      sensor.copy(),
		maxBounces:  maxBounces,
		glass:       make(map[*Sphere]cauchyCoeffs),
	}
	for _, shape := range scene.Shapes {
		switch shape.(type) {
		case *Sphere, *Plane, *Triangle:
		default:
			return nil, errors.New("pt: spectral rendering only supports Sphere, Plane and Triangle")
		}
		material, ok := materials[shape]
		if !ok {
			return nil, errors.New("pt: every scene shape needs a spectral material")
		}
		if err := validateSpectralMaterial(material); err != nil {
			return nil, err
		}
		sampler.materials[shape] = material.copy()
		if material.Scatter == SpectralDielectric {
			sphere, ok := shape.(*Sphere)
			if !ok {
				return nil, errors.New("pt: dielectric spectral materials are only supported on spheres")
			}
			sampler.glassOrder = append(sampler.glassOrder, sphere)
			sampler.glass[sphere] = cauchyCoeffs{material.IndexA, material.IndexB}
		}
	}
	if err := sampler.validateGlassGeometry(); err != nil {
		return nil, err
	}
	if err := sampler.buildWavelengthCDF(); err != nil {
		return nil, err
	}
	return sampler, nil
}

func validateSpectralMaterial(m SpectralMaterial) error {
	switch m.Scatter {
	case SpectralLambert:
		if err := validateUnitSpectrum(m.Albedo, "albedo"); err != nil {
			return err
		}
	case SpectralMirror:
		if err := validateUnitSpectrum(m.Reflectance, "reflectance"); err != nil {
			return err
		}
	case SpectralDielectric:
		if math.IsNaN(m.IndexA) || math.IsInf(m.IndexA, 0) || m.IndexA < 1 {
			return errors.New("pt: dielectric Cauchy A must be finite and at least 1")
		}
		if math.IsNaN(m.IndexB) || math.IsInf(m.IndexB, 0) || m.IndexB < 0 {
			return errors.New("pt: dielectric Cauchy B must be finite and non-negative")
		}
	default:
		return errors.New("pt: unknown spectral scatter type")
	}
	if len(m.Emission.wavelengths) != 0 {
		if err := validateFiniteSpectrum(m.Emission, "emission"); err != nil {
			return err
		}
	}
	return nil
}

func validateUnitSpectrum(s Spectrum, label string) error {
	if len(s.wavelengths) == 0 {
		return errors.New("pt: " + label + " spectrum is required")
	}
	for _, v := range s.values {
		if v > 1 {
			return errors.New("pt: " + label + " values must not exceed 1")
		}
	}
	return nil
}

func validateFiniteSpectrum(s Spectrum, label string) error {
	if len(s.wavelengths) == 0 {
		return errors.New("pt: " + label + " spectrum is required")
	}
	return nil
}

// validateGlassGeometry enforces that dielectric spheres are pairwise either
// strictly separate or strictly nested: no surface intersections and no
// tangency.
func (s *SpectralSampler) validateGlassGeometry() error {
	spheres := s.glassOrder
	for i := 0; i < len(spheres); i++ {
		for j := i + 1; j < len(spheres); j++ {
			a, b := spheres[i], spheres[j]
			d := a.Center.Sub(b.Center).Length()
			sum := a.Radius + b.Radius
			largest := math.Max(a.Radius, b.Radius)
			smallest := math.Min(a.Radius, b.Radius)
			tol := 1e-9 * math.Max(math.Max(1.0, d), largest)
			// Strictly separate: d > r1+r2. Strictly nested: d+min(r) < max(r).
			if !(d > sum+tol || d+smallest < largest-tol) {
				return errors.New("pt: dielectric spheres must be strictly separate or strictly nested")
			}
		}
	}
	return nil
}

// buildWavelengthCDF merges the sensor response node sets and integrates the
// summed piecewise linear density exactly on each common segment. The CDF is
// inverted analytically per segment when sampling; there is no node-index
// sampling and no dense grid.
func (s *SpectralSampler) buildWavelengthCDF() error {
	norm := s.sensor.responseIntegral()
	if !(norm > 0) || math.IsInf(norm, 0) {
		return errors.New("pt: sensor response must have a finite positive integral")
	}
	nodeSet := map[float64]struct{}{}
	for _, ch := range []Spectrum{s.sensor.red, s.sensor.green, s.sensor.blue} {
		for _, w := range ch.wavelengths {
			nodeSet[w] = struct{}{}
		}
	}
	nodes := make([]float64, 0, len(nodeSet))
	for w := range nodeSet {
		nodes = append(nodes, w)
	}
	sort.Float64s(nodes)
	s.cdfNodes = nodes
	s.cdfValue = make([]float64, len(nodes))
	s.cdfSlope = make([]float64, len(nodes)-1)
	cumulative := 0.0
	for i := 1; i < len(nodes); i++ {
		w0, w1 := nodes[i-1], nodes[i]
		r0 := s.summedResponse(w0)
		r1 := s.summedResponse(w1)
		slope := (r1 - r0) / (w1 - w0)
		s.cdfSlope[i-1] = slope
		// Exact trapezoid integral of the linear density on this segment,
		// which accounts for the physical wavelength interval length.
		cumulative += 0.5 * (r0 + r1) * (w1 - w0) / norm
		s.cdfValue[i] = cumulative
	}
	s.cdfValue[len(s.cdfValue)-1] = 1
	s.cdfNorm = norm
	return nil
}

func (s *SpectralSampler) summedResponse(wavelength float64) float64 {
	r, g, b := s.sensor.responseAt(wavelength)
	return r + g + b
}

// SampleWavelength draws one wavelength (nanometers) with density
// proportional to the summed sensor response. u must be a uniform random
// number in [0, 1). The inversion solves the linear CDF segment exactly.
func (s *SpectralSampler) SampleWavelength(u float64) float64 {
	if u <= 0 {
		return s.cdfNodes[0]
	}
	if u >= 1 {
		return s.cdfNodes[len(s.cdfNodes)-1]
	}
	// First segment whose right endpoint reaches u.
	i := sort.Search(len(s.cdfValue), func(i int) bool { return s.cdfValue[i] >= u })
	if i <= 0 {
		i = 1
	}
	if i >= len(s.cdfNodes) {
		return s.cdfNodes[len(s.cdfNodes)-1]
	}
	left := 0.0
	if i > 0 {
		left = s.cdfValue[i-1]
	}
	// Unnormalized probability mass still needed inside this segment.
	target := (u - left) * s.cdfNorm
	w0 := s.cdfNodes[i-1]
	r0 := s.summedResponse(w0)
	slope := s.cdfSlope[i-1]
	// In local coordinate t = x - w0 the density reads r0 + slope*t, so its
	// integral is r0*t + 0.5*slope*t^2 = target, solved in a stable form.
	if math.Abs(slope) < 1e-18 {
		if r0 <= 0 {
			return s.cdfNodes[i]
		}
		return w0 + target/r0
	}
	disc := r0*r0 + 2*slope*target
	if disc < 0 {
		disc = 0
	}
	t := 2 * target / (r0 + math.Sqrt(disc))
	x := w0 + t
	if x < w0 {
		x = w0
	}
	if x > s.cdfNodes[i] {
		x = s.cdfNodes[i]
	}
	return x
}

// WavelengthPDF returns p(lambda) in inverse nanometers. It is zero wherever
// the summed sensor response is zero, including outside [400, 700].
func (s *SpectralSampler) WavelengthPDF(wavelength float64) float64 {
	if wavelength < spectralMinWavelength || wavelength > spectralMaxWavelength || s.cdfNorm <= 0 {
		return 0
	}
	return s.summedResponse(wavelength) / s.cdfNorm
}

// Sample implements the existing Sampler interface. The caller must pass the
// bound scene, a unit-length ray and a non-nil random source. One wavelength
// is sampled per call and exactly one scalar random walk is traced.
func (s *SpectralSampler) Sample(scene *Scene, ray Ray, rnd *rand.Rand) Color {
	wavelength := s.SampleWavelength(rnd.Float64())
	pdf := s.WavelengthPDF(wavelength)
	radiance := s.TraceWavelength(wavelength, ray, rnd)
	if pdf <= 0 {
		return Black
	}
	red, green, blue := s.sensor.responseAt(wavelength)
	weight := radiance / pdf
	return Color{weight * red, weight * green, weight * blue}
}

// TraceWavelength runs the scalar random walk at a fixed wavelength and
// returns the spectral radiance along the ray.
func (s *SpectralSampler) TraceWavelength(wavelength float64, ray Ray, rnd *rand.Rand) float64 {
	radiance, _ := s.trace(wavelength, ray, rnd, false)
	return radiance
}

// TraceWavelengthPath behaves like TraceWavelength but also returns every
// processed surface event, so the optical path can be verified independently
// of the sensor weighting.
func (s *SpectralSampler) TraceWavelengthPath(wavelength float64, ray Ray, rnd *rand.Rand) (float64, []SpectralEvent) {
	return s.trace(wavelength, ray, rnd, true)
}

func (s *SpectralSampler) trace(wavelength float64, ray Ray, rnd *rand.Rand, record bool) (float64, []SpectralEvent) {
	stack := s.initialMediumStack(ray.Origin)
	throughput := 1.0
	radiance := 0.0
	var events []SpectralEvent
	current := ray
	for eventCount := 0; ; eventCount++ {
		hit := s.scene.Intersect(current)
		if !hit.Ok() {
			// Escape: the environment is handled by this same random walk.
			radiance += throughput * s.environment.At(wavelength)
			return radiance, events
		}
		if eventCount >= s.maxBounces {
			// MaxBounces = 0 therefore allows environment escapes only.
			return radiance, events
		}
		info := hit.Info(current)
		material := s.materials[info.Shape]
		event := SpectralEvent{
			Shape:      info.Shape,
			Wavelength: wavelength,
			Position:   info.Position,
			Normal:     info.Normal,
			Entering:   !info.Inside,
			StackDepth: len(stack),
		}
		// Every hit adds self-emission first; directly hit emitters and
		// escapes share this single walk, with no explicit light sampling.
		if len(material.Emission.wavelengths) != 0 {
			event.Emission = throughput * material.Emission.At(wavelength)
			radiance += event.Emission
		}
		switch material.Scatter {
		case SpectralLambert:
			throughput *= material.Albedo.At(wavelength)
			direction := cosineHemisphere(info.Normal, rnd)
			event.Direction = direction
			current = Ray{info.Position, direction}
			current.Origin = current.Origin.Add(direction.MulScalar(spectralShift))
		case SpectralMirror:
			throughput *= material.Reflectance.At(wavelength)
			direction := info.Normal.Reflect(current.Direction)
			event.Direction = direction
			event.Reflection = true
			current = Ray{info.Position, direction}
			current.Origin = current.Origin.Add(direction.MulScalar(spectralShift))
		case SpectralDielectric:
			sphere := info.Shape.(*Sphere)
			etaI := s.mediumIndex(stack, wavelength)
			etaT := 1.0
			if event.Entering {
				etaT = s.glassIndex(sphere, wavelength)
			} else {
				// Leaving this sphere: the destination is the enclosing
				// medium, possibly another glass sphere, not necessarily air.
				etaT = s.enclosingMediumIndex(stack, sphere, wavelength)
			}
			fresnel := dielectricFresnel(current.Direction, info.Normal, etaI, etaT)
			event.EtaI, event.EtaT, event.Fresnel = etaI, etaT, fresnel
			if rnd.Float64() < fresnel {
				// Reflection does not change the medium stack.
				direction := info.Normal.Reflect(current.Direction)
				event.Direction = direction
				event.Reflection = true
				current = Ray{info.Position, direction}
				current.Origin = current.Origin.Add(direction.MulScalar(spectralShift))
			} else {
				refracted, tir := refractDirection(current.Direction, info.Normal, etaI, etaT)
				if tir {
					// Total internal reflection: reflection only, stack kept.
					direction := info.Normal.Reflect(current.Direction)
					event.Direction = direction
					event.Reflection = true
					event.Fresnel = 1
					current = Ray{info.Position, direction}
					current.Origin = current.Origin.Add(direction.MulScalar(spectralShift))
					if record {
						event.StackDepth = len(stack)
						events = append(events, event)
					}
					continue
				}
				// Radiance-form transmission factor. The Fresnel branch
				// probability already canceled, so Fresnel is not reapplied.
				throughput *= (etaI / etaT) * (etaI / etaT)
				if event.Entering {
					stack = append(stack, sphere)
				} else {
					stack = popSphere(stack, sphere)
				}
				event.Direction = refracted
				event.Transmission = true
				event.StackDepth = len(stack)
				current = Ray{info.Position, refracted}
				current.Origin = current.Origin.Add(refracted.MulScalar(spectralShift))
			}
		}
		event.StackDepth = len(stack)
		if record {
			events = append(events, event)
		}
	}
}

// initialMediumStack builds the sphere nesting at the ray origin from the
// actual containment geometry, so a ray may start at any nesting depth. The
// order is outermost first, innermost last, matching transmission pushes.
func (s *SpectralSampler) initialMediumStack(origin Vector) []*Sphere {
	var contained []*Sphere
	for _, sphere := range s.glassOrder {
		if origin.Sub(sphere.Center).Length() < sphere.Radius {
			contained = append(contained, sphere)
		}
	}
	sort.Slice(contained, func(i, j int) bool {
		return contained[i].Radius > contained[j].Radius
	})
	return contained
}

func (s *SpectralSampler) glassIndex(sphere *Sphere, wavelength float64) float64 {
	c := s.glass[sphere]
	u := wavelength / 1000.0
	return c.a + c.b/(u*u)
}

// mediumIndex returns the index of the innermost sphere in the stack, or 1
// for the exterior medium.
func (s *SpectralSampler) mediumIndex(stack []*Sphere, wavelength float64) float64 {
	if len(stack) == 0 {
		return 1
	}
	return s.glassIndex(stack[len(stack)-1], wavelength)
}

// enclosingMediumIndex returns the index entered when leaving sphere: the
// next sphere down the stack, or air when it was the outermost.
func (s *SpectralSampler) enclosingMediumIndex(stack []*Sphere, sphere *Sphere, wavelength float64) float64 {
	for i := len(stack) - 1; i >= 0; i-- {
		if stack[i] == sphere {
			if i == 0 {
				return 1
			}
			return s.glassIndex(stack[i-1], wavelength)
		}
	}
	return 1
}

func popSphere(stack []*Sphere, sphere *Sphere) []*Sphere {
	for i := len(stack) - 1; i >= 0; i-- {
		if stack[i] == sphere {
			return append(stack[:i], stack[i+1:]...)
		}
	}
	return stack
}

// dielectricFresnel computes the unpolarized Fresnel reflectance for the
// incident direction and the surface normal opposing it, including total
// internal reflection.
func dielectricFresnel(direction, normal Vector, etaI, etaT float64) float64 {
	cosI := -direction.Dot(normal)
	if cosI < 0 {
		cosI = -cosI
	}
	sinT2 := (etaI * etaI) / (etaT * etaT) * (1 - cosI*cosI)
	if sinT2 > 1 {
		return 1
	}
	cosT := math.Sqrt(1 - sinT2)
	rOrth := (etaI*cosI - etaT*cosT) / (etaI*cosI + etaT*cosT)
	rPar := (etaT*cosI - etaI*cosT) / (etaT*cosI + etaI*cosT)
	return 0.5 * (rOrth*rOrth + rPar*rPar)
}

// refractDirection applies Snell's law. The bool reports total internal
// reflection, in which case the direction is undefined.
func refractDirection(direction, normal Vector, etaI, etaT float64) (Vector, bool) {
	ratio := etaI / etaT
	cosI := -direction.Dot(normal)
	sinT2 := ratio * ratio * (1 - cosI*cosI)
	if sinT2 > 1 {
		return Vector{}, true
	}
	cosT := math.Sqrt(1 - sinT2)
	refracted := direction.MulScalar(ratio).Add(normal.MulScalar(ratio*cosI - cosT))
	return refracted.Normalize(), false
}

// cosineHemisphere samples a direction with a cosine-weighted distribution
// around the unit normal. The tangent frame is built deterministically and
// the two random numbers come from rnd.
func cosineHemisphere(normal Vector, rnd *rand.Rand) Vector {
	u1 := rnd.Float64()
	u2 := rnd.Float64()
	radius := math.Sqrt(u1)
	phi := 2 * math.Pi * u2
	sample := Vector{radius * math.Cos(phi), radius * math.Sin(phi), math.Sqrt(math.Max(0, 1-u1))}
	helper := Vector{0, 0, 1}
	if math.Abs(normal.Z) > 0.9 {
		helper = Vector{0, 1, 0}
	}
	tangent := normal.Cross(helper).Normalize()
	bitangent := normal.Cross(tangent).Normalize()
	return tangent.MulScalar(sample.X).
		Add(bitangent.MulScalar(sample.Y)).
		Add(normal.MulScalar(sample.Z)).
		Normalize()
}
