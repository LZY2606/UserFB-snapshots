package fauxgl

import (
	"errors"
	"image"
	"math"
)

// HDRSettings configures a tone-mapped bloom preview of an HDR target.
type HDRSettings struct {
	Background     Color
	Exposure       float64
	BloomThreshold float64
	BloomStrength  float64
	BloomLevels    int
}

// hdrColor is a premultiplied linear RGBA fragment stored by the HDR target.
// RGB channels are finite and non-negative and may exceed one; A is in [0, 1].
type hdrColor struct {
	R, G, B, A float64
}

// EnableHDR switches the context to a floating point render target. The
// target is initialized from ClearColor the first time it is created. Calling
// EnableHDR again without an intervening DisableHDR keeps the existing
// contents untouched.
func (dc *Context) EnableHDR() {
	if dc.hdr {
		return
	}
	dc.hdr = true
	if dc.hdrBuffer == nil {
		dc.initHDRBuffer()
	}
}

// DisableHDR returns the context to the byte color buffer. The floating point
// target is released, so a later EnableHDR starts from ClearColor again.
func (dc *Context) DisableHDR() {
	dc.hdr = false
	dc.hdrBuffer = nil
}

func (dc *Context) initHDRBuffer() {
	n := dc.Width * dc.Height
	dc.hdrBuffer = make([]hdrColor, n)
	c := dc.ClearColor
	p := hdrColor{c.R * c.A, c.G * c.A, c.B * c.A, c.A}
	for i := range dc.hdrBuffer {
		dc.hdrBuffer[i] = p
	}
}

// HDRSnapshot returns a row-major copy of the HDR target as premultiplied
// linear RGBA colors. It must only be called while HDR is enabled.
func (dc *Context) HDRSnapshot() []Color {
	if !dc.hdr {
		return nil
	}
	out := make([]Color, len(dc.hdrBuffer))
	for i, p := range dc.hdrBuffer {
		out[i] = Color{p.R, p.G, p.B, p.A}
	}
	return out
}

func (dc *Context) fragmentColor(v Vertex) Color {
	if hdr, ok := dc.Shader.(HDRShader); ok {
		return hdr.FragmentHDR(v)
	}
	return dc.Shader.Fragment(v)
}

// commitFragment applies one accepted fragment to the active color target,
// using floating point linear math while HDR is enabled and the original
// byte path otherwise. It must be called with the per-pixel lock held and
// after the depth re-check has passed.
func (dc *Context) commitFragment(x, y int, c Color) {
	if !dc.WriteColor {
		return
	}
	if !dc.hdr {
		if dc.AlphaBlend && c.A < 1 {
			sr, sg, sb, sa := c.NRGBA().RGBA()
			a := (0xffff - sa) * 0x101
			j := dc.ColorBuffer.PixOffset(x, y)
			dr := &dc.ColorBuffer.Pix[j+0]
			dg := &dc.ColorBuffer.Pix[j+1]
			db := &dc.ColorBuffer.Pix[j+2]
			da := &dc.ColorBuffer.Pix[j+3]
			*dr = uint8((uint32(*dr)*a/0xffff + sr) >> 8)
			*dg = uint8((uint32(*dg)*a/0xffff + sg) >> 8)
			*db = uint8((uint32(*db)*a/0xffff + sb) >> 8)
			*da = uint8((uint32(*da)*a/0xffff + sa) >> 8)
		} else {
			dc.ColorBuffer.SetNRGBA(x, y, c.NRGBA())
		}
		return
	}
	i := y*dc.Width + x
	dst := &dc.hdrBuffer[i]
	as := c.A
	if as < 0 {
		as = 0
	} else if as > 1 {
		as = 1
	}
	cs := hdrColor{c.R * as, c.G * as, c.B * as, as}
	if dc.AlphaBlend {
		da := 1 - as
		dst.R = cs.R + dst.R*da
		dst.G = cs.G + dst.G*da
		dst.B = cs.B + dst.B*da
		dst.A = as + dst.A*da
	} else {
		*dst = cs
	}
}

func (dc *Context) clearHDRBufferWith(c Color) {
	p := hdrColor{c.R * c.A, c.G * c.A, c.B * c.A, c.A}
	for i := range dc.hdrBuffer {
		dc.hdrBuffer[i] = p
	}
}

func finiteNonNegative(x float64) bool {
	return !math.IsNaN(x) && !math.IsInf(x, 0) && x >= 0
}

func (s HDRSettings) validate() error {
	if !finiteNonNegative(s.Background.R) || !finiteNonNegative(s.Background.G) ||
		!finiteNonNegative(s.Background.B) || s.Background.A != 1 {
		return errors.New("fauxgl: HDRSettings.Background must have finite non-negative RGB and alpha 1")
	}
	if !(s.Exposure > 0) || math.IsInf(s.Exposure, 1) {
		return errors.New("fauxgl: HDRSettings.Exposure must be finite and positive")
	}
	if !finiteNonNegative(s.BloomThreshold) || !finiteNonNegative(s.BloomStrength) {
		return errors.New("fauxgl: HDRSettings bloom parameters must be finite and non-negative")
	}
	if s.BloomLevels < 1 {
		return errors.New("fauxgl: HDRSettings.BloomLevels must be at least 1")
	}
	return nil
}

// ResolveHDR composites the HDR target over the settings background, builds a
// bloom pyramid, applies exposure and the sRGB transfer function and returns
// the resulting byte image. The floating point target, depth buffer and any
// previously returned images are never modified.
func (dc *Context) ResolveHDR(settings HDRSettings) (*image.NRGBA, error) {
	if !dc.hdr {
		return nil, errors.New("fauxgl: ResolveHDR called without EnableHDR")
	}
	if err := settings.validate(); err != nil {
		return nil, err
	}

	w, h := dc.Width, dc.Height
	// Composite the premultiplied snapshot over the opaque background,
	// yielding opaque premultiplied (hence straight) linear color C.
	C := make([]hdrColor, w*h)
	bg := settings.Background
	for i, p := range dc.hdrBuffer {
		da := 1 - p.A
		C[i] = hdrColor{
			p.R + bg.R*da,
			p.G + bg.G*da,
			p.B + bg.B*da,
			1,
		}
	}

	// Build the bloom pyramid and reconstruct the full-resolution glow.
	U := resolveBloom(C, w, h, settings.BloomThreshold, settings.BloomLevels)

	im := image.NewNRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			i := y*w + x
			p := C[i]
			u := U[i]
			hr := p.R + settings.BloomStrength*u.R
			hg := p.G + settings.BloomStrength*u.G
			hb := p.B + settings.BloomStrength*u.B
			// Exposure via one minus exponential, then the standard sRGB
			// piecewise transfer function with round-to-nearest bytes.
			j := im.PixOffset(x, y)
			im.Pix[j+0] = sRGBByte(1 - math.Exp(-settings.Exposure*hr))
			im.Pix[j+1] = sRGBByte(1 - math.Exp(-settings.Exposure*hg))
			im.Pix[j+2] = sRGBByte(1 - math.Exp(-settings.Exposure*hb))
			im.Pix[j+3] = 255
		}
	}
	return im, nil
}

// resolveBloom runs the bright-pass extraction, area-weighted downsampling
// pyramid and bilinear reconstruction, returning the full-resolution glow.
func resolveBloom(C []hdrColor, w, h int, threshold float64, levels int) []hdrColor {
	// Bright-pass extractor: D0 = C * max(L-threshold, 0) / L.
	D0 := make([]hdrColor, len(C))
	for i, p := range C {
		l := 0.2126*p.R + 0.7152*p.G + 0.0722*p.B
		if l > 0 && l > threshold {
			k := (l - threshold) / l
			D0[i] = hdrColor{p.R * k, p.G * k, p.B * k, 1}
		}
	}

	// Downsample by true overlap area until the requested number of levels
	// or a 1x1 pyramid is reached.
	pyramid := [][]hdrColor{D0}
	widths, heights := []int{w}, []int{h}
	for level := 1; level < levels; level++ {
		sw, sh := widths[level-1], heights[level-1]
		dw, dh := (sw+1)/2, (sh+1)/2
		if dw == sw && dh == sh {
			break
		}
		pyramid = append(pyramid, downsampleHDR(pyramid[level-1], sw, sh, dw, dh))
		widths = append(widths, dw)
		heights = append(heights, dh)
	}

	// Reconstruct from the smallest level. Each upsampled level is averaged
	// with the bright-pass data at its own resolution.
	U := pyramid[len(pyramid)-1]
	uw, uh := widths[len(pyramid)-1], heights[len(pyramid)-1]
	for level := len(pyramid) - 2; level >= 0; level-- {
		D := pyramid[level]
		dw, dh := widths[level], heights[level]
		up := upsampleHDR(U, uw, uh, dw, dh)
		U = make([]hdrColor, dw*dh)
		for i := range U {
			U[i] = hdrColor{
				(D[i].R + up[i].R) / 2,
				(D[i].G + up[i].G) / 2,
				(D[i].B + up[i].B) / 2,
				1,
			}
		}
		uw, uh = dw, dh
	}
	return U
}

// downsampleHDR averages src into a dw x dh grid, weighting every source
// pixel by its true overlap area with the destination pixel's footprint.
// Footprints are defined by normalized texel centers, so odd dimensions keep
// their edge row or column half-sized instead of padding with zeros or
// duplicating pixels.
func downsampleHDR(src []hdrColor, sw, sh, dw, dh int) []hdrColor {
	dst := make([]hdrColor, dw*dh)
	for dy := 0; dy < dh; dy++ {
		// Destination pixel dy covers source rows [sy0, sy1).
		yLo := float64(dy) * float64(sh) / float64(dh)
		yHi := float64(dy+1) * float64(sh) / float64(dh)
		sy0 := int(math.Floor(yLo))
		sy1 := int(math.Ceil(yHi))
		for dx := 0; dx < dw; dx++ {
			xLo := float64(dx) * float64(sw) / float64(dw)
			xHi := float64(dx+1) * float64(sw) / float64(dw)
			sx0 := int(math.Floor(xLo))
			sx1 := int(math.Ceil(xHi))
			var r, g, b, weight float64
			for sy := sy0; sy < sy1; sy++ {
				oy := math.Min(yHi, float64(sy)+1) - math.Max(yLo, float64(sy))
				if oy <= 0 {
					continue
				}
				for sx := sx0; sx < sx1; sx++ {
					ox := math.Min(xHi, float64(sx)+1) - math.Max(xLo, float64(sx))
					if ox <= 0 {
						continue
					}
					area := ox * oy
					p := src[sy*sw+sx]
					r += p.R * area
					g += p.G * area
					b += p.B * area
					weight += area
				}
			}
			if weight > 0 {
				inv := 1 / weight
				dst[dy*dw+dx] = hdrColor{r * inv, g * inv, b * inv, 1}
			}
		}
	}
	return dst
}

// upsampleHDR bilinearly samples src into a dw x dh grid, aligning normalized
// texel centers and clamping at the edges.
func upsampleHDR(src []hdrColor, sw, sh, dw, dh int) []hdrColor {
	dst := make([]hdrColor, dw*dh)
	for dy := 0; dy < dh; dy++ {
		fy := (float64(dy)+0.5)*float64(sh)/float64(dh) - 0.5
		y0 := int(math.Floor(fy))
		ty := fy - float64(y0)
		cy0 := clampInt(y0, 0, sh-1)
		cy1 := clampInt(y0+1, 0, sh-1)
		for dx := 0; dx < dw; dx++ {
			fx := (float64(dx)+0.5)*float64(sw)/float64(dw) - 0.5
			x0 := int(math.Floor(fx))
			tx := fx - float64(x0)
			cx0 := clampInt(x0, 0, sw-1)
			cx1 := clampInt(x0+1, 0, sw-1)
			p00 := src[cy0*sw+cx0]
			p10 := src[cy0*sw+cx1]
			p01 := src[cy1*sw+cx0]
			p11 := src[cy1*sw+cx1]
			w00 := (1 - tx) * (1 - ty)
			w10 := tx * (1 - ty)
			w01 := (1 - tx) * ty
			w11 := tx * ty
			dst[dy*dw+dx] = hdrColor{
				p00.R*w00 + p10.R*w10 + p01.R*w01 + p11.R*w11,
				p00.G*w00 + p10.G*w10 + p01.G*w01 + p11.G*w11,
				p00.B*w00 + p10.B*w10 + p01.B*w01 + p11.B*w11,
				1,
			}
		}
	}
	return dst
}

func clampInt(x, lo, hi int) int {
	if x < lo {
		return lo
	}
	if x > hi {
		return hi
	}
	return x
}

// sRGB OETF (IEC 61966-2-1) with round-to-nearest byte output.
func sRGBByte(linear float64) uint8 {
	if linear < 0 {
		linear = 0
	}
	var v float64
	if linear <= 0.0031308 {
		v = 12.92 * linear
	} else {
		v = 1.055*math.Pow(linear, 1/2.4) - 0.055
	}
	byteValue := Round(v * 255)
	if byteValue < 0 {
		byteValue = 0
	} else if byteValue > 255 {
		byteValue = 255
	}
	return uint8(byteValue)
}
