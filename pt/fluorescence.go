package pt

// Spectral, wavelength-domain path tracing with fluorescence support.
//
// The FluorescentSampler implements the existing Sampler interface so it can
// be handed to a Renderer unchanged. Wavelengths are expressed in nanometres
// and every spectral quantity is a piecewise-linear Spectrum over strictly
// increasing nodes inside [400, 700] nm, zero outside its node range.
//
// A fluorescent material carries an elastic (wavelength-preserving) albedo
// plus a two-dimensional conversion table T(li, lo): excitation wavelength
// li as rows, emission wavelength lo as columns. When the eye path walks
// backwards from the sensor, the currently known wavelength is lo and the
// next event samples a shorter excitation wavelength li from the radiance
// kernel K(li, lo) = T(li, lo) * li / lo, which is zero for li > lo.

import (
	"errors"
	"fmt"
	"math"
	"math/rand"
)

const (
	// SpectralMin and SpectralMax bound every spectral node in nanometres.
	SpectralMin = 400.0
	SpectralMax = 700.0
	// spectralTol guards comparisons at shared grid nodes.
	spectralTol = 1e-9
	// rootIterations bounds the bisection used for exact CDF inversion.
	rootIterations = 64
)

// ---------------------------------------------------------------------------
// Spectra
// ---------------------------------------------------------------------------

// Spectrum is a piecewise-linear spectral function: Wavelengths hold the
// strictly increasing node positions (in nanometres) and Values the matching
// samples. Evaluation is linear between nodes and zero outside the range.
type Spectrum struct {
	Wavelengths []float64
	Values      []float64
}

// Eval returns the spectral value at wavelength lambda (nanometres).
func (s *Spectrum) Eval(lambda float64) float64 {
	n := len(s.Wavelengths)
	if n == 0 || lambda < s.Wavelengths[0] || lambda > s.Wavelengths[n-1] {
		return 0
	}
	// binary search for the cell containing lambda
	lo, hi := 0, n-1
	if lambda >= s.Wavelengths[hi] {
		return s.Values[hi]
	}
	for hi-lo > 1 {
		mid := (lo + hi) >> 1
		if s.Wavelengths[mid] <= lambda {
			lo = mid
		} else {
			hi = mid
		}
	}
	x0, x1 := s.Wavelengths[lo], s.Wavelengths[hi]
	if x1 == x0 {
		return s.Values[lo]
	}
	t := (lambda - x0) / (x1 - x0)
	return s.Values[lo] + t*(s.Values[hi]-s.Values[lo])
}

// Integral returns the exact integral over the node range, i.e. the sum of
// trapezoid cell areas. Restricted sub-ranges are computed by IntegralTo.
func (s *Spectrum) Integral() float64 {
	total := 0.0
	for i := 0; i+1 < len(s.Wavelengths); i++ {
		total += 0.5 * (s.Values[i] + s.Values[i+1]) * (s.Wavelengths[i+1] - s.Wavelengths[i])
	}
	return total
}

// IntegralTo returns the exact integral of the spectrum from its leftmost
// node up to lambda. Values outside the node range are zero.
func (s *Spectrum) IntegralTo(lambda float64) float64 {
	n := len(s.Wavelengths)
	if n == 0 || lambda <= s.Wavelengths[0] {
		return 0
	}
	total := 0.0
	for i := 0; i+1 < n; i++ {
		x0, x1 := s.Wavelengths[i], s.Wavelengths[i+1]
		y0, y1 := s.Values[i], s.Values[i+1]
		if lambda <= x0 {
			break
		}
		if lambda >= x1 {
			total += 0.5 * (y0 + y1) * (x1 - x0)
			continue
		}
		t := (lambda - x0) / (x1 - x0)
		y := y0 + t*(y1-y0)
		total += 0.5 * (y0 + y) * (lambda - x0)
	}
	return total
}

// copy returns a deep copy of the spectrum's backing slices.
func (s *Spectrum) copy() Spectrum {
	wl := make([]float64, len(s.Wavelengths))
	v := make([]float64, len(s.Values))
	copy(wl, s.Wavelengths)
	copy(v, s.Values)
	return Spectrum{wl, v}
}

// validateSpectrum enforces the node and value rules shared by every
// one-dimensional spectral input.
func validateSpectrum(name string, s *Spectrum, atLeastTwo bool) error {
	if s == nil {
		return fmt.Errorf("%s: nil spectrum", name)
	}
	if len(s.Wavelengths) != len(s.Values) {
		return fmt.Errorf("%s: %d wavelengths but %d values", name, len(s.Wavelengths), len(s.Values))
	}
	if atLeastTwo && len(s.Wavelengths) < 2 {
		return fmt.Errorf("%s: need at least two nodes, got %d", name, len(s.Wavelengths))
	}
	if len(s.Wavelengths) == 0 {
		return fmt.Errorf("%s: empty spectrum", name)
	}
	for i, x := range s.Wavelengths {
		if math.IsNaN(x) || math.IsInf(x, 0) {
			return fmt.Errorf("%s: non-finite wavelength at node %d", name, i)
		}
		if x < SpectralMin-spectralTol || x > SpectralMax+spectralTol {
			return fmt.Errorf("%s: wavelength %g outside [%g, %g] nm", name, x, SpectralMin, SpectralMax)
		}
		if i > 0 && x <= s.Wavelengths[i-1] {
			return fmt.Errorf("%s: wavelengths must be strictly increasing (node %d: %g after %g)",
				name, i, x, s.Wavelengths[i-1])
		}
	}
	for i, y := range s.Values {
		if math.IsNaN(y) || math.IsInf(y, 0) {
			return fmt.Errorf("%s: non-finite value at node %d", name, i)
		}
		if y < -spectralTol {
			return fmt.Errorf("%s: negative value %g at node %d", name, y, i)
		}
	}
	return nil
}

// Sensor holds the three response spectra (e.g. red, green, blue channels)
// used to turn a scalar spectral radiance into an output color.
type Sensor struct {
	Responses [3]Spectrum
}

// Eval returns the three sensor responses at wavelength lambda.
func (z *Sensor) Eval(lambda float64) [3]float64 {
	return [3]float64{
		z.Responses[0].Eval(lambda),
		z.Responses[1].Eval(lambda),
		z.Responses[2].Eval(lambda),
	}
}

// SumIntegral returns the exact integral of the sum of the three responses.
func (z *Sensor) SumIntegral() float64 {
	total := 0.0
	for i := range z.Responses {
		total += z.Responses[i].Integral()
	}
	return total
}

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------

// SpectralMaterial describes one shape in the wavelength domain. Emission is
// an emissive spectrum (self-luminance per unit wavelength) and Albedo the
// elastic, wavelength-preserving diffuse reflectance in [0, 1]. A nil table
// makes the material purely elastic.
type SpectralMaterial struct {
	Emission Spectrum
	Albedo   Spectrum
	Table    *FluorescenceTable
}

// FluorescenceTable is the conversion density T(li, lo), in units of per
// nanometre (photon-number conversion density). Excitation holds the row
// axis (li), Emission the column axis (lo), and Data is laid out row major:
// Data[i*len(Emission)+j] = T(Excitation[i], Emission[j]).
type FluorescenceTable struct {
	Excitation Spectrum // row axis, li
	Emission   Spectrum // column axis, lo
	Data       [][]float64
}

// Eval performs bilinear interpolation inside the grid rectangle and returns
// zero outside it.
func (t *FluorescenceTable) Eval(li, lo float64) float64 {
	nr := len(t.Excitation.Wavelengths)
	nc := len(t.Emission.Wavelengths)
	if nr == 0 || nc == 0 {
		return 0
	}
	if li < t.Excitation.Wavelengths[0] || li > t.Excitation.Wavelengths[nr-1] {
		return 0
	}
	if lo < t.Emission.Wavelengths[0] || lo > t.Emission.Wavelengths[nc-1] {
		return 0
	}
	i, fi := locate(t.Excitation.Wavelengths, li)
	j, fj := locate(t.Emission.Wavelengths, lo)
	v00 := t.Data[i][j]
	v10 := t.Data[i+1][j]
	v01 := t.Data[i][j+1]
	v11 := t.Data[i+1][j+1]
	return (1-fi)*(1-fj)*v00 + fi*(1-fj)*v10 + (1-fi)*fj*v01 + fi*fj*v11
}

// locate returns the lower cell index and the fractional position inside
// that cell for lambda on the strictly increasing grid w. Callers must have
// already verified lambda lies inside the grid.
func locate(w []float64, lambda float64) (int, float64) {
	hi := len(w) - 1
	if lambda >= w[hi] {
		return hi - 1, 1.0
	}
	lo := 0
	for hi-lo > 1 {
		mid := (lo + hi) >> 1
		if w[mid] <= lambda {
			lo = mid
		} else {
			hi = mid
		}
	}
	d := w[lo+1] - w[lo]
	if d == 0 {
		return lo, 0
	}
	return lo, (lambda - w[lo]) / d
}

// Kernel evaluates K(li, lo) = T(li, lo) * li / lo, the radiance transfer
// kernel used while walking the eye path backwards. It is zero for li > lo.
func (t *FluorescenceTable) Kernel(li, lo float64) float64 {
	if lo <= 0 || li > lo+spectralTol {
		return 0
	}
	return t.Eval(li, lo) * li / lo
}

// validate checks both axes and the rectangular, non-negative data layout.
func (t *FluorescenceTable) validate() error {
	if t == nil {
		return errors.New("fluorescence: nil conversion table")
	}
	if err := validateSpectrum("fluorescence excitation grid", &t.Excitation, true); err != nil {
		return err
	}
	if err := validateSpectrum("fluorescence emission grid", &t.Emission, true); err != nil {
		return err
	}
	nr := len(t.Excitation.Wavelengths)
	nc := len(t.Emission.Wavelengths)
	if len(t.Data) != nr {
		return fmt.Errorf("fluorescence table: %d rows for %d excitation nodes", len(t.Data), nr)
	}
	for i, row := range t.Data {
		if len(row) != nc {
			return fmt.Errorf("fluorescence table: row %d has %d entries, expected %d", i, len(row), nc)
		}
		for j, v := range row {
			if math.IsNaN(v) || math.IsInf(v, 0) {
				return fmt.Errorf("fluorescence table: non-finite entry at (%d,%d)", i, j)
			}
			if v < -spectralTol {
				return fmt.Errorf("fluorescence table: negative entry %g at (%d,%d)", v, i, j)
			}
		}
	}
	return nil
}

// ---------------------------------------------------------------------------
// Fixed-emission-wavelength kernel view K(li) = T(li, lo) * li / lo
// ---------------------------------------------------------------------------

// kernelSlice is the excitation-axis slice of the kernel at a fixed current
// (emission) wavelength lo. T(li, lo) is piecewise linear in li; multiplying
// by li makes the kernel piecewise quadratic with an exactly invertible
// cubic cumulative distribution function.
type kernelSlice struct {
	table    *FluorescenceTable
	lo       float64
	segments []kernelSegment
}

// kernelSegment covers one excitation cell [a, b] whose kernel values are
// K(li) = alpha*li^2 + beta*li (the constant term is zero because T is
// linear in li and K = T*li/lo).
type kernelSegment struct {
	a, b        float64
	alpha, beta float64
	offset      float64 // CDF value at a (integral over earlier cells)
	integral    float64 // exact integral over [a, b]
}

// sliceKernel builds the exact piecewise-quadratic representation of
// K(li, lo) over li <= lo, splitting excitation cells at lo when the
// truncation lands inside a cell.
func sliceKernel(table *FluorescenceTable, lo float64) kernelSlice {
	ks := kernelSlice{table: table, lo: lo}
	g := table.Excitation.Wavelengths
	eg := table.Emission.Wavelengths
	// T is zero outside the grid rectangle: a current wavelength that the
	// emission axis does not cover carries no conversion mass.
	if lo < eg[0] || lo > eg[len(eg)-1] || lo <= g[0] {
		return ks
	}
	upper := math.Min(lo, g[len(g)-1])
	offset := 0.0
	for i := 0; i+1 < len(g); i++ {
		a, b := g[i], g[i+1]
		if b <= a || a >= upper {
			continue
		}
		hi := b
		if hi > upper {
			hi = upper
		}
		// T(li, lo) is linear in li inside this excitation cell:
		// interpolate the lo-dependent endpoint column values first.
		t0 := interpColumn(table, i, lo)
		t1 := interpColumn(table, i+1, lo)
		dx := b - a
		slope := (t1 - t0) / dx
		// T(li,lo) = t0 + slope*(li-a) = slope*li + (t0 - slope*a)
		// K(li) = T*li/lo => alpha = slope/lo, beta = (t0-slope*a)/lo
		alpha := slope / lo
		beta := (t0 - slope*a) / lo
		seg := kernelSegment{a: a, b: hi, alpha: alpha, beta: beta, offset: offset}
		seg.integral = seg.antiderivative(hi) - seg.antiderivative(a)
		ks.segments = append(ks.segments, seg)
		offset += seg.integral
		if hi >= upper-spectralTol {
			break
		}
	}
	return ks
}

// interpColumn returns T(Excitation[row], lo), interpolating along the
// emission (column) axis only.
func interpColumn(table *FluorescenceTable, row int, lo float64) float64 {
	w := table.Emission.Wavelengths
	v := table.Data[row]
	n := len(w)
	if lo <= w[0] {
		return v[0]
	}
	if lo >= w[n-1] {
		return v[n-1]
	}
	lo2, hi2 := 0, n-1
	for hi2-lo2 > 1 {
		mid := (lo2 + hi2) >> 1
		if w[mid] <= lo {
			lo2 = mid
		} else {
			hi2 = mid
		}
	}
	t := (lo - w[lo2]) / (w[hi2] - w[lo2])
	return v[lo2] + t*(v[hi2]-v[lo2])
}

// antiderivative is the exact cubic primitive of the quadratic kernel.
func (s kernelSegment) antiderivative(x float64) float64 {
	return s.alpha*x*x*x/3 + s.beta*x*x/2
}

// value returns the kernel density K(x) at excitation wavelength x.
func (s kernelSegment) value(x float64) float64 {
	return (s.alpha*x + s.beta) * x
}

// integral is I(lo) = integral K(li, lo) dli over li <= lo.
func (k kernelSlice) integral() float64 {
	if len(k.segments) == 0 {
		return 0
	}
	last := k.segments[len(k.segments)-1]
	return last.offset + last.integral
}

// pdf returns the kernel density K(li, lo) at an excitation wavelength.
func (k kernelSlice) pdf(li float64) float64 {
	for _, s := range k.segments {
		if li >= s.a-spectralTol && li <= s.b+spectralTol {
			return s.value(li)
		}
	}
	return 0
}

// sample draws an excitation wavelength from K(li, lo)/I(lo) using exact
// piecewise CDF inversion (bisection on the cubic primitive inside a cell).
func (k kernelSlice) sample(u float64) float64 {
	total := k.integral()
	if total <= 0 || len(k.segments) == 0 {
		return 0
	}
	target := u * total
	idx := 0
	// advance while the target is at or past this cell's cumulative end,
	// which also skips any zero-integral cells inside the support
	for idx < len(k.segments)-1 &&
		target >= k.segments[idx].offset+k.segments[idx].integral {
		idx++
	}
	seg := k.segments[idx]
	if seg.integral <= 0 {
		return seg.b
	}
	// invert F(x) = offset + (A(x)-A(a)) = target within [a, b]
	goal := target - seg.offset + seg.antiderivative(seg.a)
	lo, hi := seg.a, seg.b
	for i := 0; i < rootIterations; i++ {
		mid := (lo + hi) / 2
		if seg.antiderivative(mid) < goal {
			lo = mid
		} else {
			hi = mid
		}
	}
	return (lo + hi) / 2
}

// ---------------------------------------------------------------------------
// FluorescentSampler
// ---------------------------------------------------------------------------

// FluorescentSampler is a Sampler that traces a single wavelength along the
// real geometric path. The sensor wavelength is drawn once per Sample call
// and retained until output; fluorescence events may change the current
// path wavelength any number of times.
type FluorescentSampler struct {
	scene          *Scene
	materials      map[Shape]SpectralMaterial
	environment    Spectrum
	sensor         Sensor
	maxBounces     int
	sensorSegments []sensorSegment
}

// sensorSegment is one cell of the merged sensor grid on which the sum of
// the three responses is linear, so its CDF is exactly quadratic.
type sensorSegment struct {
	a, b             float64
	sa, sb           [3]float64 // responses at the endpoints
	offset, integral float64    // CDF bookkeeping for the summed response
}

// FluoroEvent is one recorded surface or termination event of a path.
type FluoroEvent struct {
	Depth         int     // surface event index, starting at 1
	Shape         Shape   // surface hit, nil for environment/absorption
	WavelengthIn  float64 // current wavelength before the event
	WavelengthOut float64 // wavelength after branching (equals In on an elastic branch)
	Branch        string  // "emission", "elastic", "fluorescence", "absorb", "environment", "depth"
	Emission      float64 // self-emission added at this event
	Throughput    float64 // cumulative scalar throughput after the event
}

// NewFluorescentSampler binds a static scene, per-shape spectral materials,
// an environment spectrum, a three-channel sensor and a bounce budget. All
// slices and maps are copied, and every input is validated up front; the
// returned error is non-nil for unsupported geometry, bad nodes, negative
// values or an all-black sensor.
func NewFluorescentSampler(scene *Scene, materials map[Shape]SpectralMaterial,
	environment Spectrum, sensor Sensor, maxBounces int) (*FluorescentSampler, error) {

	if scene == nil {
		return nil, errors.New("fluorescence: nil scene")
	}
	if maxBounces < 0 {
		return nil, fmt.Errorf("fluorescence: MaxBounces must be non-negative, got %d", maxBounces)
	}
	if err := validateSpectrum("environment", &environment, true); err != nil {
		return nil, err
	}
	// every shape needs a binding and only raw Sphere, Plane, Triangle are
	// supported (no transforms, meshes or other wrappers)
	for _, shape := range scene.Shapes {
		switch shape.(type) {
		case *Sphere, *Plane, *Triangle:
		default:
			return nil, fmt.Errorf("fluorescence: unsupported geometry %T", shape)
		}
		if _, ok := materials[shape]; !ok {
			return nil, fmt.Errorf("fluorescence: shape %T has no spectral material binding", shape)
		}
	}
	if len(materials) != len(scene.Shapes) {
		return nil, fmt.Errorf("fluorescence: %d material bindings for %d shapes",
			len(materials), len(scene.Shapes))
	}
	// validate a copied set of materials
	copiedMaterials := make(map[Shape]SpectralMaterial, len(materials))
	for shape, m := range materials {
		if err := validateSpectrum("material emission", &m.Emission, true); err != nil {
			return nil, err
		}
		if err := validateSpectrum("material albedo", &m.Albedo, true); err != nil {
			return nil, err
		}
		for i, y := range m.Albedo.Values {
			if y > 1+spectralTol {
				return nil, fmt.Errorf("fluorescence: elastic albedo %g > 1 at node %d", y, i)
			}
		}
		if m.Table != nil {
			if err := m.Table.validate(); err != nil {
				return nil, err
			}
		}
		copiedMaterials[shape] = copySpectralMaterial(m)
	}
	// sensor: three valid, non-collectively-black responses
	totalResponse := 0.0
	for i := range sensor.Responses {
		if err := validateSpectrum(fmt.Sprintf("sensor response %d", i), &sensor.Responses[i], true); err != nil {
			return nil, err
		}
		totalResponse += sensor.Responses[i].Integral()
	}
	if totalResponse <= spectralTol {
		return nil, errors.New("fluorescence: all-black sensor")
	}

	s := &FluorescentSampler{
		scene:       scene,
		materials:   copiedMaterials,
		environment: environment.copy(),
		maxBounces:  maxBounces,
	}
	for i := range sensor.Responses {
		s.sensor.Responses[i] = sensor.Responses[i].copy()
	}
	s.buildSensorCDF()
	return s, nil
}

func copySpectralMaterial(m SpectralMaterial) SpectralMaterial {
	out := SpectralMaterial{
		Emission: m.Emission.copy(),
		Albedo:   m.Albedo.copy(),
	}
	if m.Table != nil {
		t := &FluorescenceTable{
			Excitation: m.Table.Excitation.copy(),
			Emission:   m.Table.Emission.copy(),
		}
		t.Data = make([][]float64, len(m.Table.Data))
		for i, row := range m.Table.Data {
			t.Data[i] = make([]float64, len(row))
			copy(t.Data[i], row)
		}
		out.Table = t
	}
	return out
}

// buildSensorCDF merges the three response grids into one piecewise-linear
// support grid and records exact integral offsets for sampling p(ls).
func (s *FluorescentSampler) buildSensorCDF() {
	merged := map[float64]struct{}{}
	for i := range s.sensor.Responses {
		for _, x := range s.sensor.Responses[i].Wavelengths {
			merged[x] = struct{}{}
		}
	}
	nodes := make([]float64, 0, len(merged))
	for x := range merged {
		nodes = append(nodes, x)
	}
	// small, so a simple insertion sort keeps the code dependency-free
	for i := 1; i < len(nodes); i++ {
		for j := i; j > 0 && nodes[j] < nodes[j-1]; j-- {
			nodes[j], nodes[j-1] = nodes[j-1], nodes[j]
		}
	}
	offset := 0.0
	for i := 0; i+1 < len(nodes); i++ {
		a, b := nodes[i], nodes[i+1]
		seg := sensorSegment{a: a, b: b}
		sumA, sumB := 0.0, 0.0
		for k := range s.sensor.Responses {
			// a response contributes nothing over a cell whose interior
			// lies outside its own node range; point values at the support
			// boundaries have measure zero and are clipped as well
			va, vb := clippedEndpoints(&s.sensor.Responses[k], a, b)
			seg.sa[k], seg.sb[k] = va, vb
			sumA += va
			sumB += vb
		}
		seg.integral = 0.5 * (sumA + sumB) * (b - a)
		seg.offset = offset
		offset += seg.integral
		s.sensorSegments = append(s.sensorSegments, seg)
	}
}

// clippedEndpoints returns the response values at the ends of merged cell
// [a, b], or (0, 0) when the cell interior is outside the spectrum support.
func clippedEndpoints(spec *Spectrum, a, b float64) (float64, float64) {
	w := spec.Wavelengths
	w0, w1 := w[0], w[len(w)-1]
	if a >= w1 || b <= w0 {
		return 0, 0
	}
	return spec.Eval(a), spec.Eval(b)
}

// SensorPDF returns the normalized sensor sampling density p(ls),
// proportional to the sum of the three response spectra.
func (s *FluorescentSampler) SensorPDF(lambda float64) float64 {
	total := s.sensorTotal()
	if total <= 0 {
		return 0
	}
	sum := 0.0
	for i := range s.sensor.Responses {
		sum += s.sensor.Responses[i].Eval(lambda)
	}
	return sum / total
}

func (s *FluorescentSampler) sensorTotal() float64 {
	if len(s.sensorSegments) == 0 {
		return 0
	}
	last := s.sensorSegments[len(s.sensorSegments)-1]
	return last.offset + last.integral
}

// sampleSensorWavelength draws ls exactly from the summed-response density
// by inverting the piecewise-quadratic CDF on the merged node grid.
func (s *FluorescentSampler) sampleSensorWavelength(u float64) float64 {
	total := s.sensorTotal()
	target := u * total
	idx := 0
	// skip every cell whose cumulative mass ends at or before the target;
	// this naturally jumps zero-integral gap cells between disjoint bands
	for idx < len(s.sensorSegments)-1 &&
		target >= s.sensorSegments[idx].offset+s.sensorSegments[idx].integral {
		idx++
	}
	seg := s.sensorSegments[idx]
	if seg.integral <= 0 {
		return seg.b
	}
	sumA := seg.sa[0] + seg.sa[1] + seg.sa[2]
	sumB := seg.sb[0] + seg.sb[1] + seg.sb[2]
	dx := seg.b - seg.a
	goal := target - seg.offset
	// integral of the linear sum over [a, x]: sumA*t*dx + 0.5*(sumB-sumA)*t^2*dx
	if math.Abs(sumB-sumA) <= spectralTol {
		t := goal / (sumA * dx)
		return seg.a + math.Max(0, math.Min(1, t))*dx
	}
	m := 0.5 * (sumB - sumA) * dx
	c := sumA * dx
	t := (-c + math.Sqrt(math.Max(0, c*c+4*m*goal))) / (2 * m)
	t = math.Max(0, math.Min(1, t))
	return seg.a + t*dx
}

// KernelValue exposes the fixed-lo kernel K(li, lo) of a shape's material.
func (s *FluorescentSampler) KernelValue(shape Shape, li, lo float64) float64 {
	m, ok := s.materials[shape]
	if !ok || m.Table == nil {
		return 0
	}
	return m.Table.Kernel(li, lo)
}

// KernelIntegral exposes I(lo) = integral K(li, lo) dli over li <= lo.
func (s *FluorescentSampler) KernelIntegral(shape Shape, lo float64) float64 {
	m, ok := s.materials[shape]
	if !ok || m.Table == nil {
		return 0
	}
	return sliceKernel(m.Table, lo).integral()
}

// KernelSample draws li from K(li, lo)/I(lo) and reports the matching PDF.
// It returns li == 0 when the kernel carries no mass at this lo.
func (s *FluorescentSampler) KernelSample(shape Shape, lo float64, u float64) (li, pdf float64) {
	m, ok := s.materials[shape]
	if !ok || m.Table == nil {
		return 0, 0
	}
	k := sliceKernel(m.Table, lo)
	total := k.integral()
	if total <= 0 {
		return 0, 0
	}
	li = k.sample(u)
	return li, k.pdf(li) / total
}

// Sample implements the Sampler interface. The sensor wavelength is drawn
// once and retained for the final Sensor(ls)/p(ls) weighting regardless of
// how many fluorescence conversions happen along the path.
func (s *FluorescentSampler) Sample(scene *Scene, ray Ray, rnd *rand.Rand) Color {
	if rnd == nil {
		panic("fluorescence: Sample requires a non-nil random source")
	}
	ls := s.sampleSensorWavelength(rnd.Float64())
	L := s.Transport(scene, ray, ls, rnd, nil)
	responses := s.sensor.Eval(ls)
	p := s.SensorPDF(ls)
	if p <= 0 {
		return Black
	}
	return Color{
		R: L * responses[0] / p,
		G: L * responses[1] / p,
		B: L * responses[2] / p,
	}
}

// Trace is a convenience wrapper around Transport that allocates the event
// record itself and returns it together with the scalar radiance. The
// sampler keeps no history: each call starts with an empty record.
func (s *FluorescentSampler) Trace(scene *Scene, ray Ray, initialWavelength float64,
	rnd *rand.Rand) (float64, []FluoroEvent) {

	var events []FluoroEvent
	L := s.Transport(scene, ray, initialWavelength, rnd, &events)
	return L, events
}

// Transport runs the scalar wavelength-domain random walk for a caller
// chosen initial wavelength. The optional event slice records the real
// path; no history is retained across calls.
func (s *FluorescentSampler) Transport(scene *Scene, ray Ray, initialWavelength float64,
	rnd *rand.Rand, events *[]FluoroEvent) float64 {

	if rnd == nil {
		panic("fluorescence: Transport requires a non-nil random source")
	}
	lambda := initialWavelength
	throughput := 1.0
	radiance := 0.0
	ray.Direction = ray.Direction.Normalize()

	for depth := 0; ; depth++ {
		if depth >= s.maxBounces {
			// no more surface events may be processed: only escape counts
			hit := scene.Intersect(ray)
			if !hit.Ok() {
				env := s.environment.Eval(lambda)
				radiance += throughput * env
				if events != nil {
					*events = append(*events, FluoroEvent{
						Depth: depth + 1, WavelengthIn: lambda, WavelengthOut: lambda,
						Branch: "environment", Emission: env, Throughput: throughput})
				}
			} else if events != nil {
				*events = append(*events, FluoroEvent{
					Depth: depth + 1, Shape: hit.Shape, WavelengthIn: lambda, WavelengthOut: lambda,
					Branch: "depth", Throughput: throughput})
			}
			return radiance
		}

		hit := scene.Intersect(ray)
		if !hit.Ok() {
			env := s.environment.Eval(lambda)
			radiance += throughput * env
			if events != nil {
				*events = append(*events, FluoroEvent{
					Depth: depth + 1, WavelengthIn: lambda, WavelengthOut: lambda,
					Branch: "environment", Emission: env, Throughput: throughput})
			}
			return radiance
		}

		info := hit.Info(ray)
		material := s.materials[hit.Shape]
		// self-emission at the current wavelength first
		emission := material.Emission.Eval(lambda)
		if emission > 0 {
			radiance += throughput * emission
			if events != nil {
				*events = append(*events, FluoroEvent{
					Depth: depth + 1, Shape: hit.Shape, WavelengthIn: lambda, WavelengthOut: lambda,
					Branch: "emission", Emission: emission, Throughput: throughput})
			}
		}

		rho := material.Albedo.Eval(lambda)
		I := 0.0
		var k kernelSlice
		if material.Table != nil {
			k = sliceKernel(material.Table, lambda)
			I = k.integral()
		}
		A := rho + I
		if A <= spectralTol {
			if events != nil {
				*events = append(*events, FluoroEvent{
					Depth: depth + 1, Shape: hit.Shape, WavelengthIn: lambda, WavelengthOut: lambda,
					Branch: "absorb", Throughput: throughput})
			}
			return radiance
		}

		// branch: elastic with probability rho/A, otherwise convert
		if rnd.Float64() < rho/A {
			throughput *= A
			if events != nil {
				*events = append(*events, FluoroEvent{
					Depth: depth + 1, Shape: hit.Shape, WavelengthIn: lambda, WavelengthOut: lambda,
					Branch: "elastic", Throughput: throughput})
			}
		} else {
			throughput *= A
			newLambda := k.sample(rnd.Float64())
			if events != nil {
				*events = append(*events, FluoroEvent{
					Depth: depth + 1, Shape: hit.Shape, WavelengthIn: lambda, WavelengthOut: newLambda,
					Branch: "fluorescence", Throughput: throughput})
			}
			lambda = newLambda
		}

		// cosine continuation about the geometric normal facing the ray
		ray = cosineRay(info, rnd)
	}
}

// cosineRay builds a cosine-weighted ray on the hemisphere around the
// geometric normal facing the incoming ray. HitInfo.Normal is already
// oriented against the ray that produced the hit, which also gives
// double-sided diffuse surfaces the correct hemisphere.
func cosineRay(info HitInfo, rnd *rand.Rand) Ray {
	n := info.Normal
	u := rnd.Float64()
	v := rnd.Float64()
	radius := math.Sqrt(u)
	theta := 2 * math.Pi * v
	tangent := n.Cross(RandomUnitVector(rnd))
	if tangent.LengthSq() < spectralTol {
		axis := V(1, 0, 0)
		if math.Abs(n.Dot(axis)) > 0.9 {
			axis = V(0, 1, 0)
		}
		tangent = n.Cross(axis)
	}
	tangent = tangent.Normalize()
	bitangent := n.Cross(tangent)
	d := Vector{}
	d = d.Add(tangent.MulScalar(radius * math.Cos(theta)))
	d = d.Add(bitangent.MulScalar(radius * math.Sin(theta)))
	d = d.Add(n.MulScalar(math.Sqrt(math.Max(0, 1-u))))
	origin := info.Position.Add(n.MulScalar(1e-6))
	return Ray{origin, d.Normalize()}
}

// LengthSq reports the squared length; added here to keep vector math local
// to the fluorescence code without touching existing files.
func (v Vector) LengthSq() float64 {
	return v.X*v.X + v.Y*v.Y + v.Z*v.Z
}
