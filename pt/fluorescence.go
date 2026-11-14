package pt

import (
	"errors"
	"fmt"
	"math"
	"math/rand"
	"sort"
)

// Spectral fluorescence transport.
//
// This file adds a wavelength-aware path tracing mode on top of the
// existing RGB renderer. A FluorescentSampler binds a static Scene, one
// spectral material per Shape, an environment spectrum and a three
// channel Sensor. The camera samples a single sensor wavelength per
// path; surface events may convert the current wavelength to a shorter
// excitation wavelength according to a radiative energy kernel
// K(li, lo) = T(li, lo) * li / lo that is only non-zero for li <= lo.
// The final scalar radiance is weighted by the original sensor response
// vector Sensor(ls) / p(ls), never by the last excitation wavelength.

const (
	fluoroMinLambda = 400.0 // nanometers
	fluoroMaxLambda = 700.0 // nanometers
)

// checkFluoroNodes validates a spectral node grid: at least two finite
// nodes inside [400, 700] nm, strictly increasing.
func checkFluoroNodes(nodes []float64) error {
	if len(nodes) < 2 {
		return fmt.Errorf("fluorescence: need at least 2 nodes, got %d", len(nodes))
	}
	for i, x := range nodes {
		if math.IsNaN(x) || math.IsInf(x, 0) {
			return fmt.Errorf("fluorescence: non-finite node %v at index %d", x, i)
		}
		if x < fluoroMinLambda || x > fluoroMaxLambda {
			return fmt.Errorf("fluorescence: node %v outside [%v, %v] nm", x, fluoroMinLambda, fluoroMaxLambda)
		}
		if i > 0 && x <= nodes[i-1] {
			return fmt.Errorf("fluorescence: nodes not strictly increasing at index %d", i)
		}
	}
	return nil
}

// Spectrum is a piecewise-linear spectral curve defined by nodes in
// nanometers. It is linear between nodes and zero outside the node
// range. Values are finite and non-negative.
type Spectrum struct {
	nodes []float64
	vals  []float64
}

// NewSpectrum builds a Spectrum from node wavelengths and values,
// copying both slices.
func NewSpectrum(nodes, vals []float64) (Spectrum, error) {
	if len(nodes) != len(vals) {
		return Spectrum{}, fmt.Errorf("fluorescence: %d nodes but %d values", len(nodes), len(vals))
	}
	if err := checkFluoroNodes(nodes); err != nil {
		return Spectrum{}, err
	}
	for i, v := range vals {
		if math.IsNaN(v) || math.IsInf(v, 0) {
			return Spectrum{}, fmt.Errorf("fluorescence: non-finite value at index %d", i)
		}
		if v < 0 {
			return Spectrum{}, fmt.Errorf("fluorescence: negative value %v at index %d", v, i)
		}
	}
	return Spectrum{
		nodes: append([]float64(nil), nodes...),
		vals:  append([]float64(nil), vals...),
	}, nil
}

func (s Spectrum) clone() Spectrum {
	return Spectrum{
		nodes: append([]float64(nil), s.nodes...),
		vals:  append([]float64(nil), s.vals...),
	}
}

// Nodes returns a copy of the node wavelengths.
func (s Spectrum) Nodes() []float64 {
	return append([]float64(nil), s.nodes...)
}

// Values returns a copy of the node values.
func (s Spectrum) Values() []float64 {
	return append([]float64(nil), s.vals...)
}

// Value evaluates the spectrum at lambda (nanometers). Zero outside
// the node range.
func (s Spectrum) Value(lambda float64) float64 {
	n := len(s.nodes)
	if n == 0 || lambda < s.nodes[0] || lambda > s.nodes[n-1] {
		return 0
	}
	i := sort.Search(n, func(i int) bool { return s.nodes[i] >= lambda })
	if i < n && s.nodes[i] == lambda {
		return s.vals[i]
	}
	// nodes[i-1] < lambda < nodes[i]
	x0, x1 := s.nodes[i-1], s.nodes[i]
	t := (lambda - x0) / (x1 - x0)
	return s.vals[i-1]*(1-t) + s.vals[i]*t
}

// Sensor holds the three camera response spectra. A path samples one
// sensor wavelength ls with density proportional to R+G+B and weights
// the scalar result by Sensor(ls) / p(ls).
type Sensor struct {
	R, G, B Spectrum
	breaks  []float64 // merged nodes of the three spectra
	cdf     []float64 // exact cumulative integral of R+G+B over breaks
	total   float64
}

// NewSensor builds a Sensor from three response spectra. It returns an
// error if the summed response is identically zero.
func NewSensor(r, g, b Spectrum) (Sensor, error) {
	s := Sensor{R: r.clone(), G: g.clone(), B: b.clone()}
	seen := make(map[float64]bool)
	for _, sp := range []Spectrum{s.R, s.G, s.B} {
		for _, x := range sp.nodes {
			if !seen[x] {
				seen[x] = true
				s.breaks = append(s.breaks, x)
			}
		}
	}
	sort.Float64s(s.breaks)
	s.cdf = make([]float64, len(s.breaks))
	prev := s.Density(s.breaks[0])
	acc := 0.0
	for i := 1; i < len(s.breaks); i++ {
		cur := s.Density(s.breaks[i])
		acc += 0.5 * (prev + cur) * (s.breaks[i] - s.breaks[i-1])
		s.cdf[i] = acc
		prev = cur
	}
	s.total = acc
	if !(acc > 0) {
		return Sensor{}, errors.New("fluorescence: sensor response is identically zero")
	}
	return s, nil
}

func (s Sensor) clone() Sensor {
	c := Sensor{
		R:      s.R.clone(),
		G:      s.G.clone(),
		B:      s.B.clone(),
		breaks: append([]float64(nil), s.breaks...),
		cdf:    append([]float64(nil), s.cdf...),
		total:  s.total,
	}
	return c
}

// Density is the unnormalized sampling density (R+G+B)(lambda).
func (s Sensor) Density(lambda float64) float64 {
	return s.R.Value(lambda) + s.G.Value(lambda) + s.B.Value(lambda)
}

// Total is the exact integral of Density over [400, 700] nm.
func (s Sensor) Total() float64 {
	return s.total
}

// Response returns the vector (R, G, B)(lambda).
func (s Sensor) Response(lambda float64) Color {
	return Color{s.R.Value(lambda), s.G.Value(lambda), s.B.Value(lambda)}
}

// SampleWavelength inverts the exact piecewise-quadratic CDF of the
// summed response. u must be in [0, 1). It returns the wavelength and
// its probability density p(ls) = Density(ls) / Total().
func (s Sensor) SampleWavelength(u float64) (lambda, pdf float64) {
	if u < 0 {
		u = 0
	}
	if u >= 1 {
		u = 1 - 1e-15
	}
	target := u * s.total
	i := sort.Search(len(s.cdf), func(i int) bool { return s.cdf[i] >= target })
	if i == 0 {
		i = 1
	}
	if i >= len(s.cdf) {
		i = len(s.cdf) - 1
	}
	x0, x1 := s.breaks[i-1], s.breaks[i]
	s0, s1 := s.Density(x0), s.Density(x1)
	d := target - s.cdf[i-1]
	dx := x1 - x0
	var t float64
	if s1 == s0 {
		if s0 > 0 {
			t = d / s0
		}
	} else {
		// solve s0*t + (s1-s0)*t^2/(2*dx) = d for t in [0, dx]
		a := (s1 - s0) / (2 * dx)
		t = (math.Sqrt(s0*s0+4*a*d) - s0) / (2 * a)
	}
	if t < 0 {
		t = 0
	}
	if t > dx {
		t = dx
	}
	lambda = x0 + t
	return lambda, s.Density(lambda) / s.total
}

// FluorescentMaterial is the spectral material bound to one Shape.
// Elastic scattering keeps the wavelength with albedo rho(lambda);
// conversion absorbs a photon at the current emission wavelength lo and
// re-emits at a shorter excitation wavelength li with density
// proportional to the kernel K(li, lo) = T(li, lo) * li / lo.
type FluorescentMaterial struct {
	albedo   Spectrum    // elastic albedo rho, values in [0, 1]
	emission Spectrum    // self emission (radiance per nm)
	exc      []float64   // excitation (li) grid, rows of T
	emi      []float64   // emission (lo) grid, columns of T
	table    [][]float64 // T in photons per nm, [excitation row][emission column]
}

// NewFluorescentMaterial validates and copies the spectral material
// description. The table is indexed as table[excitation][emission] and
// its dimensions must match the two grids.
func NewFluorescentMaterial(albedo, emission Spectrum, excitation, emissionGrid []float64, table [][]float64) (*FluorescentMaterial, error) {
	if err := checkFluoroNodes(excitation); err != nil {
		return nil, fmt.Errorf("fluorescence: excitation grid: %v", err)
	}
	if err := checkFluoroNodes(emissionGrid); err != nil {
		return nil, fmt.Errorf("fluorescence: emission grid: %v", err)
	}
	for i, v := range albedo.vals {
		if v > 1 {
			return nil, fmt.Errorf("fluorescence: elastic albedo %v > 1 at node %d", v, i)
		}
	}
	if len(table) != len(excitation) {
		return nil, fmt.Errorf("fluorescence: table has %d rows, excitation grid has %d nodes", len(table), len(excitation))
	}
	for i, row := range table {
		if len(row) != len(emissionGrid) {
			return nil, fmt.Errorf("fluorescence: table row %d has %d columns, emission grid has %d nodes", i, len(row), len(emissionGrid))
		}
		for j, v := range row {
			if math.IsNaN(v) || math.IsInf(v, 0) {
				return nil, fmt.Errorf("fluorescence: non-finite table value at [%d][%d]", i, j)
			}
			if v < 0 {
				return nil, fmt.Errorf("fluorescence: negative table value %v at [%d][%d]", v, i, j)
			}
		}
	}
	m := &FluorescentMaterial{
		albedo:   albedo.clone(),
		emission: emission.clone(),
		exc:      append([]float64(nil), excitation...),
		emi:      append([]float64(nil), emissionGrid...),
		table:    make([][]float64, len(table)),
	}
	for i, row := range table {
		m.table[i] = append([]float64(nil), row...)
	}
	return m, nil
}

// Albedo returns the elastic albedo rho(lambda).
func (m *FluorescentMaterial) Albedo(lambda float64) float64 {
	return m.albedo.Value(lambda)
}

// Emission returns the self emission at lambda.
func (m *FluorescentMaterial) Emission(lambda float64) float64 {
	return m.emission.Value(lambda)
}

// Table evaluates T(li, lo) by bilinear interpolation on the grid
// rectangles. Zero outside the grids.
func (m *FluorescentMaterial) Table(li, lo float64) float64 {
	i, ti, ok := fluoroGridCell(m.exc, li)
	if !ok {
		return 0
	}
	j, tj, ok := fluoroGridCell(m.emi, lo)
	if !ok {
		return 0
	}
	a := m.table[i][j]*(1-ti) + m.table[i+1][j]*ti
	b := m.table[i][j+1]*(1-ti) + m.table[i+1][j+1]*ti
	return a*(1-tj) + b*tj
}

func fluoroGridCell(grid []float64, x float64) (int, float64, bool) {
	n := len(grid)
	if x < grid[0] || x > grid[n-1] {
		return 0, 0, false
	}
	i := sort.Search(n, func(i int) bool { return grid[i] >= x })
	if i < n && grid[i] == x {
		if i == n-1 {
			return n - 2, 1, true
		}
		return i, 0, true
	}
	return i - 1, (x - grid[i-1]) / (grid[i] - grid[i-1]), true
}

// Kernel evaluates the radiative energy kernel K(li, lo) =
// T(li, lo) * li / lo. It is zero for li > lo and outside the grids.
func (m *FluorescentMaterial) Kernel(li, lo float64) float64 {
	if li > lo || lo <= 0 {
		return 0
	}
	return m.Table(li, lo) * li / lo
}

// columnAt interpolates T(., lo) onto the excitation nodes for a fixed
// emission wavelength lo.
func (m *FluorescentMaterial) columnAt(lo float64) ([]float64, bool) {
	j, tj, ok := fluoroGridCell(m.emi, lo)
	if !ok {
		return nil, false
	}
	col := make([]float64, len(m.exc))
	for i := range m.exc {
		col[i] = m.table[i][j]*(1-tj) + m.table[i][j+1]*tj
	}
	return col, true
}

// excitationCells returns, for a fixed lo, T interpolated onto the
// excitation nodes and the exact integral of K(li, lo) over each
// excitation cell clipped to li <= lo. On each cell T is linear in li,
// so K = T * li / lo is a quadratic with an analytic cubic integral.
func (m *FluorescentMaterial) excitationCells(lo float64) (col, integrals []float64, total float64) {
	col, ok := m.columnAt(lo)
	if !ok || lo <= 0 {
		return nil, nil, 0
	}
	integrals = make([]float64, len(m.exc)-1)
	for i := 0; i < len(m.exc)-1; i++ {
		x0, x1 := m.exc[i], m.exc[i+1]
		up := math.Min(x1, lo)
		if up <= x0 {
			continue
		}
		b := (col[i+1] - col[i]) / (x1 - x0)
		a := col[i] - b*x0 // T(li) = a + b*li on this cell
		// integral of (a + b*l) * l / lo dl = (a*l^2/2 + b*l^3/3) / lo
		F := func(l float64) float64 { return a*l*l/2 + b*l*l*l/3 }
		integrals[i] = (F(up) - F(x0)) / lo
		total += integrals[i]
	}
	return col, integrals, total
}

// ConversionIntegral returns I(lo) = integral of K(li, lo) dli over
// all excitation wavelengths li <= lo, computed exactly piece by piece.
func (m *FluorescentMaterial) ConversionIntegral(lo float64) float64 {
	_, _, total := m.excitationCells(lo)
	return total
}

// ExcitationPDF returns the density of the excitation wavelength li
// sampled for a fixed emission wavelength lo: K(li, lo) / I(lo).
func (m *FluorescentMaterial) ExcitationPDF(li, lo float64) float64 {
	total := m.ConversionIntegral(lo)
	if total <= 0 {
		return 0
	}
	return m.Kernel(li, lo) / total
}

// SampleExcitation draws an excitation wavelength li for a fixed
// emission wavelength lo with density K(li, lo) / I(lo) by exact
// piecewise CDF construction and bracketed inversion of the cubic CDF
// inside the selected cell. u must be in [0, 1). It returns the
// wavelength and its density.
func (m *FluorescentMaterial) SampleExcitation(lo, u float64) (li, pdf float64) {
	col, integrals, total := m.excitationCells(lo)
	if total <= 0 {
		return 0, 0
	}
	if u < 0 {
		u = 0
	}
	if u >= 1 {
		u = 1 - 1e-15
	}
	target := u * total
	cum := 0.0
	for i, cell := range integrals {
		if cell <= 0 || target > cum+cell {
			cum += cell
			continue
		}
		x0, x1 := m.exc[i], m.exc[i+1]
		up := math.Min(x1, lo)
		b := (col[i+1] - col[i]) / (x1 - x0)
		a := col[i] - b*x0
		F := func(l float64) float64 { return a*l*l/2 + b*l*l*l/3 }
		// solve F(l) = F(x0) + (target - cum) * lo on [x0, up];
		// F is non-decreasing there since T >= 0 and l > 0
		rhs := F(x0) + (target-cum)*lo
		loB, hiB := x0, up
		for iter := 0; iter < 80; iter++ {
			mid := 0.5 * (loB + hiB)
			if F(mid) < rhs {
				loB = mid
			} else {
				hiB = mid
			}
		}
		li = 0.5 * (loB + hiB)
		return li, m.Kernel(li, lo) / total
	}
	// numerical fallback: right edge of the last non-zero cell
	for i := len(integrals) - 1; i >= 0; i-- {
		if integrals[i] > 0 {
			li = math.Min(m.exc[i+1], lo)
			return li, m.Kernel(li, lo) / total
		}
	}
	return 0, 0
}

// FluoroEventKind identifies what happened at one step of a path.
type FluoroEventKind int

const (
	FluoroEventEmission FluoroEventKind = iota // surface self emission collected
	FluoroEventElastic                         // wavelength kept
	FluoroEventConvert                         // wavelength converted to excitation
	FluoroEventAbsorb                          // path absorbed (A == 0)
	FluoroEventEscape                          // ray escaped to the environment
	FluoroEventTruncate                        // path hit MaxBounces limit
)

func (k FluoroEventKind) String() string {
	switch k {
	case FluoroEventEmission:
		return "emission"
	case FluoroEventElastic:
		return "elastic"
	case FluoroEventConvert:
		return "convert"
	case FluoroEventAbsorb:
		return "absorb"
	case FluoroEventEscape:
		return "escape"
	case FluoroEventTruncate:
		return "truncate"
	}
	return "unknown"
}

// FluoroEvent records one step of a real path: the wavelength before
// and after the step, the branch taken, and the cumulative path weight
// (throughput) after the step.
type FluoroEvent struct {
	Depth     int
	Kind      FluoroEventKind
	LambdaIn  float64
	LambdaOut float64
	Weight    float64
}

// FluorescentSampler is a Sampler that traces wavelength-changing
// paths through a static scene. It supports untextured Sphere, Plane
// and Triangle shapes with double-sided ideal diffuse continuation.
// Refraction, participating media and explicit light sampling are
// intentionally not implemented.
type FluorescentSampler struct {
	scene      *Scene
	materials  map[Shape]*FluorescentMaterial
	env        Spectrum
	sensor     Sensor
	maxBounces int
}

// NewFluorescentSampler binds a static scene, one spectral material
// per shape, an environment spectrum, a sensor and the maximum number
// of surface events per path. All slices and maps are copied. Every
// shape in the scene must have a bound material and must be an
// untextured Sphere, Plane or Triangle, otherwise an error is
// returned. MaxBounces must be non-negative; 0 allows only environment
// escapes.
func NewFluorescentSampler(scene *Scene, materials map[Shape]*FluorescentMaterial, env Spectrum, sensor Sensor, maxBounces int) (*FluorescentSampler, error) {
	if scene == nil {
		return nil, errors.New("fluorescence: nil scene")
	}
	if maxBounces < 0 {
		return nil, fmt.Errorf("fluorescence: negative MaxBounces %d", maxBounces)
	}
	s := &FluorescentSampler{
		scene:      scene,
		materials:  make(map[Shape]*FluorescentMaterial, len(scene.Shapes)),
		env:        env.clone(),
		sensor:     sensor.clone(),
		maxBounces: maxBounces,
	}
	for _, shape := range scene.Shapes {
		m, ok := materials[shape]
		if !ok || m == nil {
			return nil, fmt.Errorf("fluorescence: no spectral material bound for shape %T", shape)
		}
		switch shape.(type) {
		case *Sphere, *Plane, *Triangle:
		default:
			return nil, fmt.Errorf("fluorescence: unsupported geometry %T", shape)
		}
		rm := shape.MaterialAt(Vector{})
		if rm.Texture != nil || rm.NormalTexture != nil || rm.BumpTexture != nil || rm.GlossTexture != nil {
			return nil, fmt.Errorf("fluorescence: textured shape %T not supported", shape)
		}
		s.materials[shape] = m
	}
	scene.Compile()
	return s, nil
}

// MaxBounces returns the maximum number of surface events per path.
func (s *FluorescentSampler) MaxBounces() int {
	return s.maxBounces
}

// Sensor returns the bound sensor.
func (s *FluorescentSampler) Sensor() Sensor {
	return s.sensor
}

// Environment returns the bound environment spectrum.
func (s *FluorescentSampler) Environment() Spectrum {
	return s.env
}

// Material returns the spectral material bound to a shape, or nil.
func (s *FluorescentSampler) Material(shape Shape) *FluorescentMaterial {
	return s.materials[shape]
}

// Sample implements the Sampler interface. It samples one sensor
// wavelength ls, traces a scalar path starting at ls, and weights the
// result by the original sensor response Sensor(ls) / p(ls). The scene
// argument must be the scene bound at construction; the bound scene is
// the one that is traced. rnd must be non-nil and is the only source
// of randomness.
func (s *FluorescentSampler) Sample(scene *Scene, ray Ray, rnd *rand.Rand) Color {
	c, _ := s.SampleWithEvents(scene, ray, rnd)
	return c
}

// SampleWithEvents is Sample plus the list of path events. The event
// slice belongs to the caller; no history is kept across calls.
func (s *FluorescentSampler) SampleWithEvents(scene *Scene, ray Ray, rnd *rand.Rand) (Color, []FluoroEvent) {
	ls, pdf := s.sensor.SampleWavelength(rnd.Float64())
	l, events := s.Trace(ray, ls, rnd)
	if l == 0 {
		return Black, events
	}
	return s.sensor.Response(ls).MulScalar(l / pdf), events
}

// Trace follows one scalar path starting at the given initial
// wavelength and returns the collected radiance along with the path
// events. Each surface event adds the self emission at the current
// wavelength, then either keeps the wavelength (probability rho/A) or
// converts it to a sampled excitation wavelength (probability I/A),
// multiplies the throughput by A = rho + I, and continues with a
// cosine-weighted direction around the normal facing the incoming ray.
// Hitting a light and escaping to the environment are both accounted
// by the same random walk; there is no Russian roulette.
func (s *FluorescentSampler) Trace(ray Ray, lambda float64, rnd *rand.Rand) (float64, []FluoroEvent) {
	var events []FluoroEvent
	radiance := 0.0
	throughput := 1.0
	for depth := 0; ; depth++ {
		hit := s.scene.Intersect(ray)
		if !hit.Ok() {
			radiance += throughput * s.env.Value(lambda)
			events = append(events, FluoroEvent{depth, FluoroEventEscape, lambda, lambda, throughput})
			break
		}
		if depth >= s.maxBounces {
			events = append(events, FluoroEvent{depth, FluoroEventTruncate, lambda, lambda, throughput})
			break
		}
		material := s.materials[hit.Shape]
		info := hit.Info(ray)
		if e := material.Emission(lambda); e > 0 {
			radiance += throughput * e
			events = append(events, FluoroEvent{depth, FluoroEventEmission, lambda, lambda, throughput})
		}
		rho := material.Albedo(lambda)
		conversion := material.ConversionIntegral(lambda)
		a := rho + conversion
		if a <= 0 {
			events = append(events, FluoroEvent{depth, FluoroEventAbsorb, lambda, lambda, throughput})
			break
		}
		kind := FluoroEventElastic
		next := lambda
		if rnd.Float64() >= rho/a {
			kind = FluoroEventConvert
			next, _ = material.SampleExcitation(lambda, rnd.Float64())
		}
		throughput *= a
		events = append(events, FluoroEvent{depth, kind, lambda, next, throughput})
		lambda = next
		// cosine-weighted continuation around the normal that faces
		// the incoming ray (double-sided diffuse)
		n := info.Normal
		if n.Dot(ray.Direction) > 0 {
			n = n.Negate()
		}
		ray = Ray{info.Position, n}.WeightedBounce(rnd.Float64(), rnd.Float64(), rnd)
	}
	return radiance, events
}
