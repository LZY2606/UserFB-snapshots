package pt

import (
	"errors"
	"math"
	"math/rand"
	"sort"
)

// EnvironmentMap is an equirectangular environment (sky) radiance map.
// Pixels are linear RGB radiance, row-major, with the first row at the
// north pole. Each texel is constant (no interpolation). Sampling is
// proportional to texel luminance times true spherical area; all PDFs
// are with respect to solid angle.
type EnvironmentMap struct {
	width   int
	height  int
	pixels  []Color
	pdf     []float64 // per-texel solid-angle PDF
	cdf     []float64 // normalized cumulative texel probabilities, len n+1
	cosRow  []float64 // cos(pi*r/height) for r in 0..height
	uniform bool      // all-black fallback: uniform spherical distribution
}

// NewEnvironmentMap builds an environment map from width*height linear RGB
// radiance values in row-major order (first row at the north pole). The
// input slice is copied. It returns an error for non-positive or
// overflowing dimensions, a length mismatch, or negative/non-finite
// components.
func NewEnvironmentMap(width, height int, pixels []Color) (*EnvironmentMap, error) {
	if width <= 0 || height <= 0 {
		return nil, errors.New("pt: environment map dimensions must be positive")
	}
	if width > math.MaxInt/height {
		return nil, errors.New("pt: environment map dimensions overflow")
	}
	n := width * height
	if len(pixels) != n {
		return nil, errors.New("pt: environment map pixel count does not match dimensions")
	}
	cp := make([]Color, n)
	for i, c := range pixels {
		if c.R < 0 || c.G < 0 || c.B < 0 ||
			math.IsNaN(c.R) || math.IsNaN(c.G) || math.IsNaN(c.B) ||
			math.IsInf(c.R, 0) || math.IsInf(c.G, 0) || math.IsInf(c.B, 0) {
			return nil, errors.New("pt: environment map pixels must be finite and non-negative")
		}
		cp[i] = c
	}
	m := &EnvironmentMap{
		width:  width,
		height: height,
		pixels: cp,
		pdf:    make([]float64, n),
		cdf:    make([]float64, n+1),
		cosRow: make([]float64, height+1),
	}
	for r := 0; r <= height; r++ {
		m.cosRow[r] = math.Cos(math.Pi * float64(r) / float64(height))
	}
	// texel weights: luminance * true spherical area, scaled by 1/max
	// so extreme dynamic ranges cannot overflow the cumulative sums
	w := make([]float64, n)
	maxW := 0.0
	for r := 0; r < height; r++ {
		area := 2 * math.Pi / float64(width) * (m.cosRow[r] - m.cosRow[r+1])
		for c := 0; c < width; c++ {
			i := r*width + c
			p := cp[i]
			lum := 0.2126*p.R + 0.7152*p.G + 0.0722*p.B
			w[i] = lum * area
			if w[i] > maxW {
				maxW = w[i]
			}
		}
	}
	if maxW == 0 {
		m.uniform = true
		return m, nil
	}
	total := 0.0
	for i := 0; i < n; i++ {
		p := w[i] / maxW
		total += p
		m.cdf[i+1] = total
	}
	// per-texel solid-angle PDF: (w_i/total) / area_i = lum_i / sum(lum*area)
	for r := 0; r < height; r++ {
		area := 2 * math.Pi / float64(width) * (m.cosRow[r] - m.cosRow[r+1])
		for c := 0; c < width; c++ {
			i := r*width + c
			m.pdf[i] = (w[i] / maxW) / total / area
		}
	}
	return m, nil
}

// index returns the texel index for a unit direction, applying the
// rotation angle (radians) the same way Scene.TextureAngle does.
func (m *EnvironmentMap) index(d Vector, angle float64) int {
	u := (math.Atan2(d.Z, d.X) + angle + math.Pi) / (2 * math.Pi)
	u -= math.Floor(u)
	v := math.Acos(Clamp(d.Y, -1, 1)) / math.Pi
	col := int(math.Floor(u * float64(m.width)))
	if col >= m.width {
		col = m.width - 1
	}
	row := int(math.Floor(v * float64(m.height)))
	if row >= m.height {
		row = m.height - 1
	}
	return row*m.width + col
}

// Evaluate returns the radiance of the texel containing the direction.
func (m *EnvironmentMap) Evaluate(direction Vector, angle float64) Color {
	return m.pixels[m.index(direction, angle)]
}

// PDF returns the solid-angle probability density of the direction under
// the sampling distribution. It is consistent with the pdf returned by
// Sample and consumes no randomness.
func (m *EnvironmentMap) PDF(direction Vector, angle float64) float64 {
	if m.uniform {
		return 1 / (4 * math.Pi)
	}
	return m.pdf[m.index(direction, angle)]
}

// Sample draws a direction proportional to texel luminance times
// spherical area, uniform in solid angle within the chosen texel. It
// returns the direction, its radiance, and its solid-angle PDF. An
// all-black map falls back to a uniform spherical distribution (the
// radiance is still black).
func (m *EnvironmentMap) Sample(rnd *rand.Rand, angle float64) (direction Vector, radiance Color, pdf float64) {
	if m.uniform {
		y := rnd.Float64()*2 - 1
		a := rnd.Float64() * 2 * math.Pi
		s := math.Sqrt(math.Max(0, 1-y*y))
		return Vector{s * math.Cos(a), y, s * math.Sin(a)}, Black, 1 / (4 * math.Pi)
	}
	target := rnd.Float64() * m.cdf[len(m.cdf)-1]
	i := sort.Search(len(m.cdf)-1, func(i int) bool { return m.cdf[i+1] > target })
	row := i / m.width
	col := i % m.width
	// uniform in solid angle within the texel: cos(theta) uniform in the
	// row band, longitude uniform in the column band
	y := m.cosRow[row] + (m.cosRow[row+1]-m.cosRow[row])*rnd.Float64()
	u := (float64(col) + rnd.Float64()) / float64(m.width)
	a := 2*math.Pi*u - math.Pi - angle
	s := math.Sqrt(math.Max(0, 1-y*y))
	d := Vector{s * math.Cos(a), y, s * math.Sin(a)}
	return d, m.pixels[i], m.pdf[i]
}
