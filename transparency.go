package fauxgl

import (
	"errors"
	"image/color"
	"math"
	"sort"
	"sync"
)

// transparencyFragment is a single fragment collected during a transparency
// pass. The color is stored as straight (non-premultiplied) RGBA with each
// channel clamped to [0, 1].
type transparencyFragment struct {
	pixel int     // index into the color/depth buffers (y*Width + x)
	z     float64 // screen-space depth, without DepthBias applied
	order uint64  // submission order of the source primitive
	color Color
}

// transparencyState holds the pending fragments of an active transparency
// pass along with the premultiplied background captured when the pass began.
type transparencyState struct {
	background []float64 // premultiplied RGBA, 4 floats per pixel
	fragments  []transparencyFragment
	nextOrder  uint64
	mutex      sync.Mutex
}

// BeginTransparency starts an order-independent transparency pass. It
// requires ReadDepth=true, WriteDepth=false, WriteColor=true,
// AlphaBlend=true and Wireframe=false. While the pass is active, the Draw*
// entry points collect fragments instead of touching the color and depth
// buffers; the existing depth buffer is used as an opaque occluder. Call
// ResolveTransparency to composite the collected fragments back to front,
// or CancelTransparency to discard them.
func (dc *Context) BeginTransparency() error {
	if dc.transparency != nil {
		return errors.New("fauxgl: a transparency pass is already active")
	}
	if !dc.ReadDepth || dc.WriteDepth || !dc.WriteColor || !dc.AlphaBlend || dc.Wireframe {
		return errors.New("fauxgl: transparency requires ReadDepth=true, " +
			"WriteDepth=false, WriteColor=true, AlphaBlend=true, Wireframe=false")
	}
	w, h := dc.Width, dc.Height
	background := make([]float64, w*h*4)
	for y := 0; y < h; y++ {
		i := dc.ColorBuffer.PixOffset(0, y)
		j := y * w * 4
		for x := 0; x < w; x++ {
			a := float64(dc.ColorBuffer.Pix[i+3]) / 0xff
			background[j+0] = float64(dc.ColorBuffer.Pix[i+0]) / 0xff * a
			background[j+1] = float64(dc.ColorBuffer.Pix[i+1]) / 0xff * a
			background[j+2] = float64(dc.ColorBuffer.Pix[i+2]) / 0xff * a
			background[j+3] = a
			i += 4
			j += 4
		}
	}
	dc.transparency = &transparencyState{background: background}
	return nil
}

// ResolveTransparency sorts the fragments collected during the active
// transparency pass by per-pixel depth (far to near, ties broken by
// submission order) and composites them over the background captured by
// BeginTransparency. The pass ends. The depth buffer is never modified.
func (dc *Context) ResolveTransparency() error {
	ts := dc.transparency
	if ts == nil {
		return errors.New("fauxgl: no active transparency pass")
	}
	dc.transparency = nil
	ts.resolve(dc)
	return nil
}

// CancelTransparency discards any fragments collected during the active
// transparency pass without modifying the color or depth buffers. It is
// safe to call when no pass is active.
func (dc *Context) CancelTransparency() {
	dc.transparency = nil
}

// transparencyOrder reserves a block of n submission-order slots and
// returns the base of the block. It returns 0 when no pass is active.
func (dc *Context) transparencyOrder(n uint64) uint64 {
	ts := dc.transparency
	if ts == nil {
		return 0
	}
	ts.mutex.Lock()
	defer ts.mutex.Unlock()
	base := ts.nextOrder
	ts.nextOrder += n
	return base
}

func (ts *transparencyState) add(f transparencyFragment) {
	ts.mutex.Lock()
	ts.fragments = append(ts.fragments, f)
	ts.mutex.Unlock()
}

func (ts *transparencyState) resolve(dc *Context) {
	// far to near per pixel; equal depths keep submission order so that
	// later submissions land on top
	sort.Slice(ts.fragments, func(i, j int) bool {
		a, b := ts.fragments[i], ts.fragments[j]
		if a.pixel != b.pixel {
			return a.pixel < b.pixel
		}
		if a.z != b.z {
			return a.z > b.z
		}
		return a.order < b.order
	})
	w := dc.Width
	var acc [4]float64
	current := -1
	flush := func() {
		if current < 0 {
			return
		}
		var r, g, b float64
		if acc[3] > 0 {
			r = acc[0] / acc[3]
			g = acc[1] / acc[3]
			b = acc[2] / acc[3]
		}
		x := current % w
		y := current / w
		dc.ColorBuffer.SetNRGBA(x, y, color.NRGBA{
			quantize8(r),
			quantize8(g),
			quantize8(b),
			quantize8(acc[3]),
		})
	}
	for _, f := range ts.fragments {
		if f.pixel != current {
			flush()
			current = f.pixel
			copy(acc[:], ts.background[f.pixel*4:])
		}
		c := f.color
		inv := 1 - c.A
		acc[0] = c.R*c.A + inv*acc[0]
		acc[1] = c.G*c.A + inv*acc[1]
		acc[2] = c.B*c.A + inv*acc[2]
		acc[3] = c.A + inv*acc[3]
	}
	flush()
}

func quantize8(x float64) uint8 {
	return uint8(math.Round(Clamp(x, 0, 1) * 0xff))
}

// rasterizeTransparency collects fragments for one triangle during a
// transparency pass. Coverage uses pixel centers with a top-left half-open
// rule in screen space (y down): after orienting the triangle to a positive
// signed area, edges walking upward and horizontal edges walking rightward
// include their boundary. This makes shared edges - between adjacent faces,
// inside clipping fans and inside thick lines - contribute exactly once.
func (dc *Context) rasterizeTransparency(ts *transparencyState, v0, v1, v2 Vertex, s0, s1, s2 Vector, order uint64) RasterizeInfo {
	var info RasterizeInfo
	if dc.Width <= 0 || dc.Height <= 0 {
		return info
	}

	// orient so the signed screen-space area is positive
	a2 := (s1.X-s0.X)*(s2.Y-s0.Y) - (s2.X-s0.X)*(s1.Y-s0.Y)
	if a2 < 0 {
		v1, v2 = v2, v1
		s1, s2 = s2, s1
		a2 = -a2
	}
	if !(a2 > 0) {
		// zero-area or degenerate geometry
		return info
	}

	// edge inclusion flags for the top-left rule
	inc0 := s2.Y < s1.Y || (s2.Y == s1.Y && s2.X > s1.X) // edge s1 -> s2
	inc1 := s0.Y < s2.Y || (s0.Y == s2.Y && s0.X > s2.X) // edge s2 -> s0
	inc2 := s1.Y < s0.Y || (s1.Y == s0.Y && s1.X > s0.X) // edge s0 -> s1

	// bounding box of pixel centers, clamped to the buffer extents
	min := s0.Min(s1.Min(s2))
	max := s0.Max(s1.Max(s2))
	x0 := ClampInt(int(math.Ceil(min.X-0.5)), 0, dc.Width-1)
	x1 := ClampInt(int(math.Floor(max.X-0.5)), 0, dc.Width-1)
	y0 := ClampInt(int(math.Ceil(min.Y-0.5)), 0, dc.Height-1)
	y1 := ClampInt(int(math.Floor(max.Y-0.5)), 0, dc.Height-1)

	// reciprocals
	ra := 1 / a2
	r0 := 1 / v0.Output.W
	r1 := 1 / v1.Output.W
	r2 := 1 / v2.Output.W

	for y := y0; y <= y1; y++ {
		py := float64(y) + 0.5
		for x := x0; x <= x1; x++ {
			px := float64(x) + 0.5
			e0 := (s2.X-s1.X)*(py-s1.Y) - (s2.Y-s1.Y)*(px-s1.X)
			e1 := (s0.X-s2.X)*(py-s2.Y) - (s0.Y-s2.Y)*(px-s2.X)
			e2 := (s1.X-s0.X)*(py-s0.Y) - (s1.Y-s0.Y)*(px-s0.X)
			if !(e0 > 0 || (e0 == 0 && inc0)) {
				continue
			}
			if !(e1 > 0 || (e1 == 0 && inc1)) {
				continue
			}
			if !(e2 > 0 || (e2 == 0 && inc2)) {
				continue
			}
			i := y*dc.Width + x
			info.TotalPixels++
			b0 := e0 * ra
			b1 := e1 * ra
			b2 := e2 * ra
			z := b0*s0.Z + b1*s1.Z + b2*s2.Z
			bz := z + dc.DepthBias
			if dc.ReadDepth && bz > dc.DepthBuffer[i] {
				continue
			}
			// perspective-correct interpolation of vertex data
			b := VectorW{b0 * r0, b1 * r1, b2 * r2, 0}
			b.W = 1 / (b.X + b.Y + b.Z)
			v := InterpolateVertexes(v0, v1, v2, b)
			// invoke fragment shader
			c := dc.Shader.Fragment(v)
			if c == Discard {
				continue
			}
			c = Color{
				Clamp(c.R, 0, 1),
				Clamp(c.G, 0, 1),
				Clamp(c.B, 0, 1),
				Clamp(c.A, 0, 1),
			}
			if c.A == 0 {
				continue
			}
			ts.add(transparencyFragment{pixel: i, z: z, order: order, color: c})
			info.UpdatedPixels++
		}
	}

	return info
}
