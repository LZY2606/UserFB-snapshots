package render

import (
	"errors"
	"fmt"
	"math/rand"
	"reflect"
	"sync"

	"github.com/hunterloftis/pbr/pkg/geom"
	"github.com/hunterloftis/pbr/pkg/rgb"
)

// GradientKind identifies which material parameter a gradient channel tracks.
type GradientKind int

const (
	// Metalness tracks a Uniform material's Metalness, the probability of
	// the discrete metal/non-metal branch taken in its At method.
	Metalness GradientKind = iota
	// Emission tracks a Uniform material's Emission, its emitted light scale.
	Emission
)

func (k GradientKind) String() string {
	switch k {
	case Metalness:
		return "metalness"
	case Emission:
		return "emission"
	}
	return fmt.Sprintf("GradientKind(%d)", int(k))
}

// GradientParam declares one gradient channel: the derivative of the
// rendered image with respect to one parameter of one shared material.
// Materials are matched by identity, so every object sharing a material
// (and every visit along a path) accumulates into the same channel.
// Construct params with material.Metalness / material.Emission.
type GradientParam struct {
	Material interface{}
	Kind     GradientKind
}

// GradientMaterial is implemented by materials that support gradient
// tracking (e.g. *material.Uniform). GradientValid reports whether the
// material's current values may be differentiated for the given kind.
type GradientMaterial interface {
	GradientValid(kind GradientKind) error
}

// EmissionDeriver is implemented by materials whose emitted light can be
// differentiated with respect to their emission parameter:
// it returns d Light() / d Emission.
type EmissionDeriver interface {
	EmissionDeriv() rgb.Energy
}

// EmissionGradientSource is optionally implemented by Objects to expose the
// derivative of their emitted light with respect to a material's emission
// parameter. The surface package's Sphere, Cube, and Triangle implement it.
type EmissionGradientSource interface {
	EmissionGradient() (material interface{}, grad rgb.Energy, ok bool)
}

// GradientResult holds a synchronously rendered beauty image plus one
// signed gradient image per declared parameter. Beauty, Channels, and
// Samples share a single normalization: each pixel holds the average of
// Samples path traces. Beauty is linear radiance; gradients are signed,
// linear, and neither clamped nor gamma-mapped.
type GradientResult struct {
	Width    int
	Height   int
	Samples  int            // path samples per pixel
	Beauty   []rgb.Energy   // row-major, Width*Height averaged linear radiance
	Channels [][]rgb.Energy // one row-major image per GradientParam
}

// BeautyAt returns the averaged linear radiance of pixel (x, y).
func (r *GradientResult) BeautyAt(x, y int) rgb.Energy {
	return r.Beauty[y*r.Width+x]
}

// GradientAt returns the averaged gradient of channel ch at pixel (x, y).
func (r *GradientResult) GradientAt(ch, x, y int) rgb.Energy {
	return r.Channels[ch][y*r.Width+x]
}

type gradKey struct {
	mat  interface{}
	kind GradientKind
}

// gradState accumulates score-function gradients for a single path.
type gradState struct {
	channels map[gradKey]int // parameter -> channel index (shared, read-only)
	score    []float64       // running log-probability derivative per channel
	sum      []rgb.Energy    // accumulated gradient per channel
}

// fresh returns a zeroed accumulator sharing the channel layout.
func (g *gradState) fresh() *gradState {
	return &gradState{
		channels: g.channels,
		score:    make([]float64, len(g.score)),
		sum:      make([]rgb.Energy, len(g.sum)),
	}
}

// contribute adds path contribution c to every channel, weighted by the
// score of the discrete choices made before this contribution.
func (g *gradState) contribute(c rgb.Energy) {
	for ch, s := range g.score {
		if s != 0 {
			g.sum[ch] = g.sum[ch].Plus(c.Scaled(s))
		}
	}
}

// emit adds the explicit emission derivative of obj, if its material's
// emission is tracked, scaled by the throughput reaching the object.
func (g *gradState) emit(obj Object, throughput rgb.Energy) {
	if obj == nil {
		return
	}
	src, ok := obj.(EmissionGradientSource)
	if !ok {
		return
	}
	mat, grad, ok := src.EmissionGradient()
	if !ok {
		return
	}
	ch, ok := g.channels[gradKey{mat, Emission}]
	if !ok {
		return
	}
	g.sum[ch] = g.sum[ch].Plus(grad.Times(throughput))
}

// gradContexts maps an active tracer's random source to its gradient state,
// letting materials report discrete choices without any interface changes.
var gradContexts sync.Map // map[*rand.Rand]*gradState

// RecordChoice reports the log-probability derivative of a discrete choice
// made by a material while shading. It is a no-op unless the trace using
// rnd is recording gradients, and it never consumes random numbers, so
// enabling or disabling recording leaves the path's random stream unchanged.
func RecordChoice(rnd *rand.Rand, mat interface{}, kind GradientKind, dlogp float64) {
	v, ok := gradContexts.Load(rnd)
	if !ok {
		return
	}
	g := v.(*gradState)
	ch, ok := g.channels[gradKey{mat, kind}]
	if !ok {
		return
	}
	g.score[ch] += dlogp
}

// newGradState validates params and builds the channel layout.
func newGradState(params []GradientParam) (*gradState, error) {
	g := &gradState{
		channels: make(map[gradKey]int, len(params)),
		score:    make([]float64, len(params)),
		sum:      make([]rgb.Energy, len(params)),
	}
	for i, p := range params {
		if p.Material == nil {
			return nil, fmt.Errorf("gradient param %d: nil material", i)
		}
		if !reflect.TypeOf(p.Material).Comparable() {
			return nil, fmt.Errorf("gradient param %d: material type %T cannot be used as a parameter identity", i, p.Material)
		}
		if p.Kind != Metalness && p.Kind != Emission {
			return nil, fmt.Errorf("gradient param %d: unsupported kind %s", i, p.Kind)
		}
		gm, ok := p.Material.(GradientMaterial)
		if !ok {
			return nil, fmt.Errorf("gradient param %d: material type %T does not support gradient tracking", i, p.Material)
		}
		if err := gm.GradientValid(p.Kind); err != nil {
			return nil, fmt.Errorf("gradient param %d: %v", i, err)
		}
		key := gradKey{p.Material, p.Kind}
		if _, dup := g.channels[key]; dup {
			return nil, fmt.Errorf("gradient param %d: duplicate %s parameter for the same material", i, p.Kind)
		}
		g.channels[key] = i
	}
	return g, nil
}

// traceGradient traces a single path. When tmpl declares channels, discrete
// material choices and emission derivatives are recorded along the path;
// otherwise it reduces to the plain trace. Recording never consumes random
// numbers, so the traced path is identical either way.
func (t *tracer) traceGradient(ray *geom.Ray, depth int, tmpl *gradState) (rgb.Energy, []rgb.Energy) {
	if tmpl == nil || len(tmpl.score) == 0 {
		return t.trace(ray, depth), nil
	}
	g := tmpl.fresh()
	t.grad = g
	gradContexts.Store(t.rnd, g)
	defer gradContexts.Delete(t.rnd)
	e := t.trace(ray, depth)
	t.grad = nil
	return e, g.sum
}

func checkGradientScene(s *Scene, needCamera bool) error {
	if s == nil {
		return errors.New("render: nil scene")
	}
	if s.Surface == nil {
		return errors.New("render: scene has no surface")
	}
	if s.Env == nil {
		return errors.New("render: scene has no environment")
	}
	if needCamera && s.Camera == nil {
		return errors.New("render: scene has no camera")
	}
	return nil
}

// RenderGradient synchronously renders a frame on the CPU with a fixed seed
// and a fixed number of samples per pixel. It returns the averaged linear
// beauty radiance plus one signed gradient image per declared parameter,
// all under the same normalization. Unlike the interactive Frame pipeline,
// no maxEnergy clamp is applied to traced paths. The result is freshly
// allocated on every call and shares no state with other renders.
//
// The result estimates the expectation of the underlying trace with the
// given bounce limit and direct-lighting switch; weight caps, light
// selection, absorption, and RandomGain behave exactly as in trace.
func RenderGradient(s *Scene, width, height, samples, depth int, direct bool, seed int64, params []GradientParam) (*GradientResult, error) {
	if err := checkGradientScene(s, true); err != nil {
		return nil, err
	}
	if width < 1 || height < 1 {
		return nil, fmt.Errorf("render: invalid frame size %dx%d", width, height)
	}
	if samples < 1 {
		return nil, fmt.Errorf("render: invalid sample count %d", samples)
	}
	if depth < 1 {
		return nil, fmt.Errorf("render: invalid bounce depth %d", depth)
	}
	tmpl, err := newGradState(params)
	if err != nil {
		return nil, err
	}

	rnd := rand.New(rand.NewSource(seed))
	t := &tracer{scene: s, rnd: rnd, bounce: depth, direct: direct}
	beauty := NewSample(width, height)
	nch := len(params)
	gsum := make([][]rgb.Energy, nch)
	for ch := range gsum {
		gsum[ch] = make([]rgb.Energy, width*height)
	}
	for y := 0; y < height; y++ {
		for x := 0; x < width; x++ {
			i := y*width + x
			for n := 0; n < samples; n++ {
				rx := float64(x) + rnd.Float64()
				ry := float64(y) + rnd.Float64()
				r := s.Camera.Ray(rx, ry, float64(width), float64(height), rnd)
				e, g := t.traceGradient(r, depth, tmpl)
				beauty.Add(x, y, e)
				for ch := 0; ch < nch; ch++ {
					gsum[ch][i] = gsum[ch][i].Plus(g[ch])
				}
			}
		}
	}

	res := &GradientResult{
		Width:    width,
		Height:   height,
		Samples:  samples,
		Beauty:   make([]rgb.Energy, width*height),
		Channels: make([][]rgb.Energy, nch),
	}
	norm := 1 / float64(samples)
	for y := 0; y < height; y++ {
		for x := 0; x < width; x++ {
			i := y*width + x
			e, _ := beauty.At(x, y) // Sample divides by the same sample count
			res.Beauty[i] = e
		}
	}
	for ch := 0; ch < nch; ch++ {
		res.Channels[ch] = make([]rgb.Energy, width*height)
		for i := range res.Channels[ch] {
			res.Channels[ch][i] = gsum[ch][i].Scaled(norm)
		}
	}
	return res, nil
}

// TraceGradient traces a single ray synchronously and returns its linear
// radiance along with one gradient per declared parameter. It is a
// diagnostic entry point: rnd drives the path, so callers can compare the
// random stream (e.g. the next Float64 after return) with and without
// gradient recording enabled.
func TraceGradient(s *Scene, ray *geom.Ray, rnd *rand.Rand, depth int, direct bool, params []GradientParam) (rgb.Energy, []rgb.Energy, error) {
	if err := checkGradientScene(s, false); err != nil {
		return rgb.Black, nil, err
	}
	if ray == nil {
		return rgb.Black, nil, errors.New("render: nil ray")
	}
	if rnd == nil {
		return rgb.Black, nil, errors.New("render: nil random source")
	}
	if depth < 1 {
		return rgb.Black, nil, fmt.Errorf("render: invalid bounce depth %d", depth)
	}
	tmpl, err := newGradState(params)
	if err != nil {
		return rgb.Black, nil, err
	}
	t := &tracer{scene: s, rnd: rnd, bounce: depth, direct: direct}
	e, g := t.traceGradient(ray, depth, tmpl)
	return e, g, nil
}
