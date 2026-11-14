package fauxgl

import (
	"errors"
	"fmt"
	"image"
	"math"
)

// HDRSettings controls how a linear HDR target is resolved to an LDR image.
type HDRSettings struct {
	Background     Color
	Exposure       float64
	BloomThreshold float64
	BloomStrength  float64
	BloomLevels    int
}

// EnableHDR allocates an independent, linear floating-point render target
// initialized with ClearColor. Enabling more than once leaves the existing
// contents untouched. Disabling and enabling again starts from a fresh target.
func (dc *Context) EnableHDR() {
	dc.hdrMu.Lock()
	defer dc.hdrMu.Unlock()
	dc.enableHDR()
}

func (dc *Context) enableHDR() {
	if dc.hdrEnabled {
		return
	}
	dc.hdrBuffer = make([]Color, dc.Width*dc.Height)
	dc.clearHDRBufferWith(dc.ClearColor)
	dc.hdrEnabled = true
}

// DisableHDR stops rendering into the HDR target. The byte color buffer is
// left untouched.
func (dc *Context) DisableHDR() {
	dc.hdrMu.Lock()
	defer dc.hdrMu.Unlock()
	dc.hdrEnabled = false
	dc.hdrBuffer = nil
}

// HDRSnapshot returns a copy of the HDR target as row-major premultiplied
// linear RGBA colors. It returns nil when HDR is not enabled.
func (dc *Context) HDRSnapshot() []Color {
	dc.hdrMu.Lock()
	defer dc.hdrMu.Unlock()
	if !dc.hdrEnabled {
		return nil
	}
	snapshot := make([]Color, len(dc.hdrBuffer))
	copy(snapshot, dc.hdrBuffer)
	return snapshot
}

func (dc *Context) clearHDRBufferWith(c Color) {
	p := Color{c.R * c.A, c.G * c.A, c.B * c.A, c.A}
	for i := range dc.hdrBuffer {
		dc.hdrBuffer[i] = p
	}
}

func finiteNonNegative(x float64) bool {
	return !math.IsNaN(x) && !math.IsInf(x, 0) && x >= 0
}

func validateHDRSettings(settings HDRSettings) error {
	if settings.Background.A != 1 {
		return fmt.Errorf("hdr: background alpha must be 1")
	}
	if !finiteNonNegative(settings.Background.R) ||
		!finiteNonNegative(settings.Background.G) ||
		!finiteNonNegative(settings.Background.B) {
		return fmt.Errorf("hdr: background rgb must be finite and non-negative")
	}
	if !(settings.Exposure > 0) || math.IsInf(settings.Exposure, 0) || math.IsNaN(settings.Exposure) {
		return fmt.Errorf("hdr: exposure must be finite and positive")
	}
	if !finiteNonNegative(settings.BloomThreshold) {
		return fmt.Errorf("hdr: bloom threshold must be finite and non-negative")
	}
	if !finiteNonNegative(settings.BloomStrength) {
		return fmt.Errorf("hdr: bloom strength must be finite and non-negative")
	}
	if settings.BloomLevels < 1 {
		return fmt.Errorf("hdr: bloom levels must be at least 1")
	}
	return nil
}

// ResolveHDR composites the premultiplied HDR target over the background,
// applies thresholded bloom, tone maps with the given exposure and encodes to
// sRGB bytes. It does not mutate the HDR target, the depth buffer or any
// previously returned image.
func (dc *Context) ResolveHDR(settings HDRSettings) (*image.NRGBA, error) {
	dc.hdrMu.Lock()
	defer dc.hdrMu.Unlock()
	if !dc.hdrEnabled {
		return nil, errors.New("hdr: not enabled")
	}
	if err := validateHDRSettings(settings); err != nil {
		return nil, err
	}

	im := image.NewNRGBA(image.Rect(0, 0, dc.Width, dc.Height))

	// Composite the premultiplied snapshot over an opaque background.
	c := make([]Color, len(dc.hdrBuffer))
	bg := settings.Background
	for i, src := range dc.hdrBuffer {
		inv := 1 - src.A
		c[i] = Color{
			src.R + bg.R*inv,
			src.G + bg.G*inv,
			src.B + bg.B*inv,
			1,
		}
	}

	// Thresholded highlight image D0.
	d0 := make([]Color, len(c))
	for i, col := range c {
		d0[i] = hdrHighlight(col, settings.BloomThreshold)
	}

	// Build the downsample pyramid and reconstruct the bloom image.
	levels := hdrBuildPyramid(d0, dc.Width, dc.Height, settings.BloomLevels)
	u := hdrReconstructBloom(levels)

	// Add bloom, tone map and encode.
	for y := 0; y < dc.Height; y++ {
		for x := 0; x < dc.Width; x++ {
			i := y*dc.Width + x
			h := Color{
				c[i].R + settings.BloomStrength*u[i].R,
				c[i].G + settings.BloomStrength*u[i].G,
				c[i].B + settings.BloomStrength*u[i].B,
				1,
			}
			m := Color{
				1 - math.Exp(-settings.Exposure*h.R),
				1 - math.Exp(-settings.Exposure*h.G),
				1 - math.Exp(-settings.Exposure*h.B),
				1,
			}
			j := im.PixOffset(x, y)
			im.Pix[j+0] = uint8(Round(linearToSRGB(m.R) * 255))
			im.Pix[j+1] = uint8(Round(linearToSRGB(m.G) * 255))
			im.Pix[j+2] = uint8(Round(linearToSRGB(m.B) * 255))
			im.Pix[j+3] = 255
		}
	}
	return im, nil
}

type hdrLevel struct {
	width, height int
	pixels        []Color
}

// hdrHighlight computes the thresholded highlight color for a linear color.
func hdrHighlight(c Color, threshold float64) Color {
	l := 0.2126*c.R + 0.7152*c.G + 0.0722*c.B
	t := 0.0
	if l > 0 {
		t = math.Max(l-threshold, 0) / l
	}
	return Color{c.R * t, c.G * t, c.B * t, 1}
}

// hdrBuildPyramid downsamples base repeatedly. Each level's pixels cover the
// corresponding equal fraction of the previous level's rectangle. Halving
// stops at the requested level count or once the image is 1x1.
func hdrBuildPyramid(base []Color, width, height, maxLevels int) []hdrLevel {
	levels := []hdrLevel{{width, height, base}}
	for len(levels) < maxLevels {
		w, h := levels[len(levels)-1].width, levels[len(levels)-1].height
		if w <= 1 && h <= 1 {
			break
		}
		nw := (w + 1) / 2
		nh := (h + 1) / 2
		if nw == w && nh == h {
			break
		}
		src := levels[len(levels)-1].pixels
		dst := make([]Color, nw*nh)
		hdrDownsample(src, w, h, dst, nw, nh)
		levels = append(levels, hdrLevel{nw, nh, dst})
	}
	return levels
}

// hdrReconstructBloom combines the pyramid from the smallest level upward:
// Ulast = Dlast, Ui = (Di + bilinear(Ui+1)) / 2.
func hdrReconstructBloom(levels []hdrLevel) []Color {
	last := len(levels) - 1
	u := make([]Color, len(levels[last].pixels))
	copy(u, levels[last].pixels)
	for i := last - 1; i >= 0; i-- {
		up := hdrBilinearUpsample(u, levels[i+1].width, levels[i+1].height,
			levels[i].width, levels[i].height)
		d := levels[i].pixels
		next := make([]Color, len(up))
		for j := range up {
			next[j] = Color{
				(d[j].R + up[j].R) / 2,
				(d[j].G + up[j].G) / 2,
				(d[j].B + up[j].B) / 2,
				(d[j].A + up[j].A) / 2,
			}
		}
		u = next
	}
	return u
}

// hdrDownsample area-averages src into dst. Each destination pixel maps to the
// corresponding rectangle of the source, and source pixels contribute by their
// true overlap area. This handles odd sizes without padding or duplicating
// the last row or column.
func hdrDownsample(src []Color, sw, sh int, dst []Color, dw, dh int) {
	sx := float64(sw) / float64(dw)
	sy := float64(sh) / float64(dh)
	for y := 0; y < dh; y++ {
		y0 := float64(y) * sy
		y1 := float64(y+1) * sy
		for x := 0; x < dw; x++ {
			x0 := float64(x) * sx
			x1 := float64(x+1) * sx
			var r, g, b, a, total float64
			for yy := int(math.Floor(y0)); yy < int(math.Ceil(y1)); yy++ {
				if yy < 0 || yy >= sh {
					continue
				}
				wy := math.Min(y1, float64(yy+1)) - math.Max(y0, float64(yy))
				if wy <= 0 {
					continue
				}
				for xx := int(math.Floor(x0)); xx < int(math.Ceil(x1)); xx++ {
					if xx < 0 || xx >= sw {
						continue
					}
					wx := math.Min(x1, float64(xx+1)) - math.Max(x0, float64(xx))
					if wx <= 0 {
						continue
					}
					w := wx * wy
					col := src[yy*sw+xx]
					r += col.R * w
					g += col.G * w
					b += col.B * w
					a += col.A * w
					total += w
				}
			}
			if total > 0 {
				dst[y*dw+x] = Color{r / total, g / total, b / total, a / total}
			}
		}
	}
}

// hdrBilinearUpsample resamples src to dst using normalized texel-center
// alignment with clamped edges.
func hdrBilinearUpsample(src []Color, sw, sh int, dw, dh int) []Color {
	dst := make([]Color, dw*dh)
	for y := 0; y < dh; y++ {
		fy := (float64(y)+0.5)*float64(sh)/float64(dh) - 0.5
		y0, y1, ty := hdrBilinearCoords(fy, sh)
		for x := 0; x < dw; x++ {
			fx := (float64(x)+0.5)*float64(sw)/float64(dw) - 0.5
			x0, x1, tx := hdrBilinearCoords(fx, sw)
			c00 := src[y0*sw+x0]
			c10 := src[y0*sw+x1]
			c01 := src[y1*sw+x0]
			c11 := src[y1*sw+x1]
			top := hdrLerpColor(c00, c10, tx)
			bottom := hdrLerpColor(c01, c11, tx)
			dst[y*dw+x] = hdrLerpColor(top, bottom, ty)
		}
	}
	return dst
}

func hdrBilinearCoords(f float64, n int) (int, int, float64) {
	if f < 0 {
		f = 0
	}
	if f > float64(n-1) {
		f = float64(n - 1)
	}
	i0 := int(math.Floor(f))
	i1 := i0 + 1
	if i1 > n-1 {
		i1 = n - 1
	}
	return i0, i1, f - float64(i0)
}

func hdrLerpColor(a, b Color, t float64) Color {
	return Color{
		a.R + (b.R-a.R)*t,
		a.G + (b.G-a.G)*t,
		a.B + (b.B-a.B)*t,
		a.A + (b.A-a.A)*t,
	}
}

// linearToSRGB applies the standard piecewise sRGB transfer function.
func linearToSRGB(c float64) float64 {
	if c <= 0.0031308 {
		return 12.92 * c
	}
	return 1.055*math.Pow(c, 1/2.4) - 0.055
}
