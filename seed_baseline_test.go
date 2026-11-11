package fauxgl

import (
	"bytes"
	"fmt"
	"image/color"
	"math"
	"testing"
)

func seedTriangle(z float64) *Triangle {
	return NewTriangleForPoints(V(-.8, -.7, z), V(.7, -.4, z), V(-.1, .8, z))
}

func seedContext() *Context {
	dc := NewContext(32, 32)
	dc.Cull = CullNone
	dc.ClearColorBufferWith(Black)
	return dc
}

func TestSeedOpaque(t *testing.T) {
	dc := seedContext()
	dc.Shader = NewSolidColorShader(Identity(), Color{1, 0, 0, 1})
	near := dc.DrawTriangle(seedTriangle(-.4))
	dc.Shader = NewSolidColorShader(Identity(), Color{0, 0, 1, 1})
	far := dc.DrawTriangle(seedTriangle(.4))
	p := dc.ColorBuffer.NRGBAAt(16, 16)
	z := dc.DepthBuffer[16*32+16]
	if near.UpdatedPixels == 0 || far.UpdatedPixels != 0 || p != (color.NRGBA{255, 0, 0, 255}) || math.Abs(z-.3) > 1e-12 {
		t.Fatalf("opaque near=%+v far=%+v pixel=%v depth=%g", near, far, p, z)
	}
	fmt.Printf("SEED_OPAQUE near_updates=%d far_updates=%d center=%v depth=%.3f\n", near.UpdatedPixels, far.UpdatedPixels, p, z)
}

func seedLayers(nearFirst bool) *Context {
	dc := seedContext()
	dc.WriteDepth = false
	red := func() {
		dc.Shader = NewSolidColorShader(Identity(), Color{1, 0, 0, .5})
		dc.DrawTriangle(seedTriangle(-.4))
	}
	blue := func() {
		dc.Shader = NewSolidColorShader(Identity(), Color{0, 0, 1, .5})
		dc.DrawTriangle(seedTriangle(.4))
	}
	if nearFirst {
		red()
		blue()
	} else {
		blue()
		red()
	}
	return dc
}

func TestSeedOrderDependence(t *testing.T) {
	a, b := seedLayers(true), seedLayers(false)
	pa, pb := a.ColorBuffer.NRGBAAt(16, 16), b.ColorBuffer.NRGBAAt(16, 16)
	if pa == pb || pa != (color.NRGBA{63, 0, 127, 255}) || pb != (color.NRGBA{127, 0, 63, 255}) {
		t.Fatalf("legacy order witness changed: near_first=%v far_first=%v", pa, pb)
	}
	for i, z := range a.DepthBuffer {
		if z != math.MaxFloat64 || b.DepthBuffer[i] != z {
			t.Fatal("WriteDepth=false changed depth")
		}
	}
	fmt.Printf("SEED_ORDER_DEPENDENT near_first=%v far_first=%v\n", pa, pb)
}

func TestSeedStraightAlpha(t *testing.T) {
	dc := seedContext()
	dc.ClearColorBufferWith(Transparent)
	dc.WriteDepth = false
	dc.Shader = NewSolidColorShader(Identity(), Color{1, 0, 0, .5})
	info := dc.DrawTriangle(seedTriangle(0))
	p := dc.ColorBuffer.NRGBAAt(16, 16)
	if info.UpdatedPixels == 0 || p != (color.NRGBA{127, 0, 0, 127}) {
		t.Fatalf("legacy alpha witness changed: updates=%d pixel=%v", info.UpdatedPixels, p)
	}
	fmt.Printf("SEED_STRAIGHT_ALPHA legacy_nrgba=%v\n", p)
}

func TestSeedCrossing(t *testing.T) {
	dc := seedContext()
	dc.WriteDepth = false
	red := NewTriangleForPoints(V(-.8, -.7, -.6), V(.7, -.4, .6), V(-.1, .8, .1))
	blue := NewTriangleForPoints(V(-.8, -.7, .6), V(.7, -.4, -.6), V(-.1, .8, -.1))
	dc.Shader = NewSolidColorShader(Identity(), Color{1, 0, 0, .5})
	dc.DrawTriangle(red)
	dc.Shader = NewSolidColorShader(Identity(), Color{0, 0, 1, .5})
	dc.DrawTriangle(blue)
	left, right := dc.ColorBuffer.NRGBAAt(9, 20), dc.ColorBuffer.NRGBAAt(21, 20)
	// Independent plane interpolation at two pixel centres, before screen-Z mapping.
	zAt := func(x, y float64) float64 {
		ax, ay, bx, by, cx, cy := 3.2, 27.2, 27.2, 22.4, 14.4, 3.2
		den := (by-cy)*(ax-cx) + (cx-bx)*(ay-cy)
		u := ((by-cy)*(x-cx) + (cx-bx)*(y-cy)) / den
		v := ((cy-ay)*(x-cx) + (ax-cx)*(y-cy)) / den
		return -.6*u + .6*v + .1*(1-u-v)
	}
	zLeft, zRight := zAt(9.5, 20.5), zAt(21.5, 20.5)
	if !(zLeft < 0 && zRight > 0) || left != right || left != (color.NRGBA{63, 0, 127, 255}) {
		t.Fatalf("crossing witness: left=%v right=%v redZ=(%g,%g)", left, right, zLeft, zRight)
	}
	fmt.Printf("SEED_CROSSING left=%v right=%v red_ndc_z=(%.6f,%.6f)\n", left, right, zLeft, zRight)
}

func TestSeedDiscard(t *testing.T) {
	dc := seedContext()
	before := append([]byte(nil), dc.ColorBuffer.Pix...)
	dc.Shader = NewSolidColorShader(Identity(), Discard)
	info := dc.DrawTriangle(seedTriangle(-.8))
	if info.TotalPixels == 0 || info.UpdatedPixels != 0 || !bytes.Equal(before, dc.ColorBuffer.Pix) {
		t.Fatalf("discard=%+v changed image=%v", info, !bytes.Equal(before, dc.ColorBuffer.Pix))
	}
	for _, z := range dc.DepthBuffer {
		if z != math.MaxFloat64 {
			t.Fatal("discard wrote depth")
		}
	}
	fmt.Printf("SEED_DISCARD total=%d updates=%d\n", info.TotalPixels, info.UpdatedPixels)
}
