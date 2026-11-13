package pt

import (
	"errors"
	"math"
	"math/rand"
)

// SPPMOptions configures a SPPMRenderer.
type SPPMOptions struct {
	PhotonsPerIteration int     // photons emitted from the lights each iteration
	MaxDepth            int     // maximum surface intersections per eye or photon path
	InitialRadius       float64 // initial gather radius of every pixel
	Alpha               float64 // progressive radius reduction factor in (0, 1]
	Seed                int64   // seed for the renderer's private random source
}

// SPPMPixelState is a value copy of a single pixel's progressive state.
// M, Phi, EyeBeta and the visible point describe the most recent iteration;
// N, Tau and Radius are the accumulated progressive values.
type SPPMPixelState struct {
	Radius   float64
	N        float64
	Tau      Color
	Phi      Color
	EyeBeta  Color
	M        uint64
	Visible  bool
	Position Vector
	Normal   Vector
}

// SPPMStats reports cumulative counters of a SPPMRenderer.
type SPPMStats struct {
	Iterations      uint64
	Emitted         uint64 // total photons emitted, including lost ones
	CandidateChecks uint64 // visible points that reached the distance check
}

// sppmGatherPoint is a visible point as inserted into the gather index.
type sppmGatherPoint struct {
	position Vector
	normal   Vector
	albedo   Color
	radius   float64
	pixel    int
}

type sppmCell struct {
	x, y, z int
}

func sppmCellOf(p Vector, size float64) sppmCell {
	return sppmCell{
		int(math.Floor(p.X / size)),
		int(math.Floor(p.Y / size)),
		int(math.Floor(p.Z / size)),
	}
}

// sppmIndex is a uniform grid hash over the visible points of one iteration.
// Each point is inserted into every cell its radius sphere touches, so a
// photon only has to visit the single cell containing its hit position.
type sppmIndex struct {
	points   []sppmGatherPoint
	cellSize float64
	cells    map[sppmCell][]int
}

func newSPPMIndex(points []sppmGatherPoint) *sppmIndex {
	maxR := 0.0
	for _, p := range points {
		if p.radius > maxR {
			maxR = p.radius
		}
	}
	if maxR <= 0 {
		maxR = 1
	}
	ix := &sppmIndex{points: points, cellSize: maxR, cells: make(map[sppmCell][]int)}
	for i, p := range points {
		r := p.radius
		lo := sppmCellOf(Vector{p.position.X - r, p.position.Y - r, p.position.Z - r}, maxR)
		hi := sppmCellOf(Vector{p.position.X + r, p.position.Y + r, p.position.Z + r}, maxR)
		for x := lo.x; x <= hi.x; x++ {
			for y := lo.y; y <= hi.y; y++ {
				for z := lo.z; z <= hi.z; z++ {
					c := sppmCell{x, y, z}
					ix.cells[c] = append(ix.cells[c], i)
				}
			}
		}
	}
	return ix
}

// visitCandidates calls fn once for every point whose radius sphere overlaps
// the cell containing x. A point is reported at most once per call.
func (ix *sppmIndex) visitCandidates(x Vector, fn func(int)) {
	if ix == nil {
		return
	}
	c := sppmCellOf(x, ix.cellSize)
	for _, i := range ix.cells[c] {
		fn(i)
	}
}

// sppmGatherAccept implements the local kernel rules: the photon hit must be
// within the pixel's old radius, the two normals must roughly agree, and the
// incoming light must arrive in the visible point's positive hemisphere.
func sppmGatherAccept(p *sppmGatherPoint, x, n, dir Vector) bool {
	d := p.position.Sub(x)
	if d.Dot(d) > p.radius*p.radius {
		return false
	}
	if p.normal.Dot(n) <= 0.9 {
		return false
	}
	if p.normal.Dot(dir) >= 0 {
		return false
	}
	return true
}

type sppmPixel struct {
	// progressive state
	n      float64
	radius float64
	tau    Color
	direct Color // running sum of per-iteration direct contributions
	// most recent iteration
	visible  bool
	position Vector
	normal   Vector
	beta     Color
	albedo   Color
	m        uint64
	phi      Color
}

// SPPMRenderer renders a scene with progressive photon mapping. It coexists
// with the path tracing Renderer and reuses Scene intersection, HitInfo and
// the existing material direction math.
type SPPMRenderer struct {
	scene   *Scene
	camera  Camera
	width   int
	height  int
	options SPPMOptions
	rnd     *rand.Rand
	lights  []*Sphere
	pixels  []sppmPixel

	iterations      uint64
	emitted         uint64
	candidateChecks uint64
}

// NewSPPM creates a progressive photon mapping renderer for the given scene.
// The scene must contain at least one untextured Sphere light whose Color and
// Emittance describe a constant outgoing radiance. Supported shapes are
// Sphere, Plane, Triangle and Mesh; supported materials are DiffuseMaterial,
// MetallicMaterial with Gloss 0 and Tint 1, and ClearMaterial with Gloss 0.
func NewSPPM(scene *Scene, camera Camera, width, height int, options SPPMOptions) (*SPPMRenderer, error) {
	if width <= 0 || height <= 0 {
		return nil, errors.New("sppm: width and height must be positive")
	}
	if options.PhotonsPerIteration <= 0 {
		return nil, errors.New("sppm: PhotonsPerIteration must be positive")
	}
	if options.MaxDepth <= 0 {
		return nil, errors.New("sppm: MaxDepth must be positive")
	}
	if math.IsNaN(options.InitialRadius) || math.IsInf(options.InitialRadius, 0) || options.InitialRadius <= 0 {
		return nil, errors.New("sppm: InitialRadius must be finite and greater than zero")
	}
	if !(options.Alpha > 0) || options.Alpha > 1 {
		return nil, errors.New("sppm: Alpha must be in (0, 1]")
	}
	var lights []*Sphere
	for _, shape := range scene.Lights {
		s, ok := shape.(*Sphere)
		if !ok || s.Material.Texture != nil || s.Material.Emittance <= 0 {
			continue
		}
		lights = append(lights, s)
	}
	if len(lights) == 0 {
		return nil, errors.New("sppm: scene needs at least one untextured sphere light")
	}
	scene.Compile()
	r := &SPPMRenderer{
		scene:   scene,
		camera:  camera,
		width:   width,
		height:  height,
		options: options,
		rnd:     rand.New(rand.NewSource(options.Seed)),
		lights:  lights,
		pixels:  make([]sppmPixel, width*height),
	}
	for i := range r.pixels {
		r.pixels[i].radius = options.InitialRadius
	}
	return r, nil
}

// Stats returns the renderer's cumulative counters.
func (r *SPPMRenderer) Stats() SPPMStats {
	return SPPMStats{
		Iterations:      r.iterations,
		Emitted:         r.emitted,
		CandidateChecks: r.candidateChecks,
	}
}

// PixelState returns a copy of the pixel's current state.
func (r *SPPMRenderer) PixelState(x, y int) SPPMPixelState {
	p := &r.pixels[y*r.width+x]
	return SPPMPixelState{
		Radius:   p.radius,
		N:        p.n,
		Tau:      p.tau,
		Phi:      p.phi,
		EyeBeta:  p.beta,
		M:        p.m,
		Visible:  p.visible,
		Position: p.position,
		Normal:   p.normal,
	}
}

// Radiance returns the current linear RGB estimate of a pixel: the iteration
// average of the eye path's direct contribution plus the progressive photon
// estimate normalized by every photon emitted so far.
func (r *SPPMRenderer) Radiance(x, y int) Color {
	if r.iterations == 0 {
		return Black
	}
	p := &r.pixels[y*r.width+x]
	result := p.direct.DivScalar(float64(r.iterations))
	if r.emitted > 0 {
		area := math.Pi * p.radius * p.radius
		result = result.Add(p.tau.DivScalar(area * float64(r.emitted)))
	}
	return result
}

// Step advances the render by one iteration: trace one jittered eye sample
// per pixel, emit a fresh batch of photons, gather them onto the current
// visible points, and apply the progressive radius/flux update.
func (r *SPPMRenderer) Step() error {
	for y := 0; y < r.height; y++ {
		for x := 0; x < r.width; x++ {
			r.traceEyePixel(x, y)
		}
	}
	points := make([]sppmGatherPoint, 0, len(r.pixels))
	for i := range r.pixels {
		p := &r.pixels[i]
		if p.visible {
			points = append(points, sppmGatherPoint{
				position: p.position,
				normal:   p.normal,
				albedo:   p.albedo,
				radius:   p.radius,
				pixel:    i,
			})
		}
	}
	index := newSPPMIndex(points)
	for i := 0; i < r.options.PhotonsPerIteration; i++ {
		r.emitPhoton(index)
		r.emitted++
	}
	alpha := r.options.Alpha
	for i := range r.pixels {
		p := &r.pixels[i]
		if !p.visible || p.m == 0 {
			continue
		}
		m := float64(p.m)
		n1 := p.n + alpha*m
		r1 := p.radius * math.Sqrt(n1/(p.n+m))
		scale := r1 / p.radius
		scale *= scale
		p.tau = p.tau.Add(p.beta.Mul(p.phi)).MulScalar(scale)
		p.n = n1
		p.radius = r1
	}
	r.iterations++
	return nil
}

// sppmCameraRay mirrors Camera.CastRay but takes the axis center for
// single-pixel axes instead of dividing by width/height minus one.
func sppmCameraRay(c *Camera, x, y, w, h int, u, v float64, rnd *rand.Rand) Ray {
	aspect := float64(w) / float64(h)
	var px, py float64
	if w > 1 {
		px = ((float64(x)+u-0.5)/(float64(w)-1))*2 - 1
	}
	if h > 1 {
		py = ((float64(y)+v-0.5)/(float64(h)-1))*2 - 1
	}
	d := Vector{}
	d = d.Add(c.u.MulScalar(-px * aspect))
	d = d.Add(c.v.MulScalar(-py))
	d = d.Add(c.w.MulScalar(c.m))
	d = d.Normalize()
	p := c.p
	if c.apertureRadius > 0 {
		focalPoint := c.p.Add(d.MulScalar(c.focalDistance))
		angle := rnd.Float64() * 2 * math.Pi
		radius := rnd.Float64() * c.apertureRadius
		p = p.Add(c.u.MulScalar(math.Cos(angle) * radius))
		p = p.Add(c.v.MulScalar(math.Sin(angle) * radius))
		d = focalPoint.Sub(p).Normalize()
	}
	return Ray{p, d}
}

// traceEyePixel traces one jittered eye path for pixel (x, y), recording the
// direct light/environment contribution and, if the path ends on a diffuse
// surface, the visible point used by this iteration's gather.
func (r *SPPMRenderer) traceEyePixel(x, y int) {
	p := &r.pixels[y*r.width+x]
	p.m = 0
	p.phi = Black
	p.visible = false
	p.beta = Black
	p.position = Vector{}
	p.normal = Vector{}
	u := r.rnd.Float64()
	v := r.rnd.Float64()
	ray := sppmCameraRay(&r.camera, x, y, r.width, r.height, u, v, r.rnd)
	beta := White
	direct := Black
	for depth := 0; depth < r.options.MaxDepth; depth++ {
		hit := r.scene.Intersect(ray)
		if !hit.Ok() {
			direct = direct.Add(beta.Mul(r.scene.Color))
			break
		}
		info := hit.Info(ray)
		material := info.Material
		if material.Emittance > 0 {
			emission := material.Color.MulScalar(material.Emittance)
			direct = direct.Add(beta.Mul(emission))
			break
		}
		if material.Transparent {
			n1, n2 := 1.0, material.Index
			if info.Inside {
				n1, n2 = n2, n1
			}
			reflectance := info.Ray.Reflectance(ray, n1, n2)
			if r.rnd.Float64() < reflectance {
				ray = info.Ray.Reflect(ray)
			} else {
				ray = info.Ray.Refract(ray, n1, n2)
				s := n1 / n2
				beta = beta.MulScalar(s * s)
			}
			ray.Origin = ray.Origin.Add(ray.Direction.MulScalar(1e-4))
			continue
		}
		if material.Reflectivity >= 0 {
			beta = beta.Mul(material.Color)
			ray = info.Ray.Reflect(ray)
			ray.Origin = ray.Origin.Add(ray.Direction.MulScalar(1e-4))
			continue
		}
		p.visible = true
		p.position = info.Position
		p.normal = info.Normal
		p.beta = beta
		p.albedo = material.Color
		break
	}
	p.direct = p.direct.Add(direct)
}

// emitPhoton samples one photon from a uniformly chosen sphere light. The
// initial flux compensates the light selection probability, the surface area
// density and the cosine direction density, so each photon carries the
// selected light's full power and the per-iteration photon count (normalized
// away by the cumulative emission denominator) cannot change the exposure.
func (r *SPPMRenderer) emitPhoton(index *sppmIndex) {
	li := r.rnd.Intn(len(r.lights))
	light := r.lights[li]
	n := RandomUnitVector(r.rnd)
	position := light.Center.Add(n.MulScalar(light.Radius))
	u := r.rnd.Float64()
	v := r.rnd.Float64()
	ray := Ray{position, n}.WeightedBounce(u, v, r.rnd)
	ray.Origin = ray.Origin.Add(ray.Direction.MulScalar(1e-4))
	material := light.Material
	radiance := material.Color.MulScalar(material.Emittance)
	power := 4 * math.Pi * math.Pi * light.Radius * light.Radius
	flux := radiance.MulScalar(power * float64(len(r.lights)))
	r.tracePhoton(ray, flux, index)
}

// tracePhoton follows a photon through specular reflection and refraction.
// At each diffuse surface it first gathers into nearby visible points, then
// scales by the local albedo and continues on a cosine-sampled direction.
// Photons transport power, so no (n1/n2)^2 factor is applied on refraction.
func (r *SPPMRenderer) tracePhoton(ray Ray, flux Color, index *sppmIndex) {
	for depth := 0; depth < r.options.MaxDepth; depth++ {
		hit := r.scene.Intersect(ray)
		if !hit.Ok() {
			return
		}
		info := hit.Info(ray)
		material := info.Material
		if material.Emittance > 0 {
			return
		}
		if material.Transparent {
			n1, n2 := 1.0, material.Index
			if info.Inside {
				n1, n2 = n2, n1
			}
			reflectance := info.Ray.Reflectance(ray, n1, n2)
			if r.rnd.Float64() < reflectance {
				ray = info.Ray.Reflect(ray)
			} else {
				ray = info.Ray.Refract(ray, n1, n2)
			}
			ray.Origin = ray.Origin.Add(ray.Direction.MulScalar(1e-4))
			continue
		}
		if material.Reflectivity >= 0 {
			flux = flux.Mul(material.Color)
			ray = info.Ray.Reflect(ray)
			ray.Origin = ray.Origin.Add(ray.Direction.MulScalar(1e-4))
			continue
		}
		r.gather(index, info.Position, info.Normal, ray.Direction, flux)
		flux = flux.Mul(material.Color)
		u := r.rnd.Float64()
		v := r.rnd.Float64()
		ray = Ray{info.Position, info.Normal}.WeightedBounce(u, v, r.rnd)
		ray.Origin = ray.Origin.Add(ray.Direction.MulScalar(1e-4))
	}
}

// gather adds a photon's flux to every nearby visible point accepted by the
// local kernel rules, weighting by the visible point's albedo over pi.
func (r *SPPMRenderer) gather(index *sppmIndex, x, n, dir Vector, flux Color) {
	index.visitCandidates(x, func(i int) {
		r.candidateChecks++
		point := &index.points[i]
		if !sppmGatherAccept(point, x, n, dir) {
			return
		}
		p := &r.pixels[point.pixel]
		p.m++
		p.phi = p.phi.Add(flux.Mul(point.albedo).DivScalar(math.Pi))
	})
}
