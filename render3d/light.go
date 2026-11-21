package render3d

import (
	"math"
	"math/rand"
	"sort"

	"github.com/unixpickle/model3d/model3d"
)

// Color is a linear RGB color, where X, Y, and Z store R,
// G, and B respectively.
//
// Note that these colors are NOT sRGB (the standard),
// since sRGB values do not represent linear brightness.
//
// Colors should be positive, but they are not bounded on
// the positive side, since light isn't in the real world.
type Color = model3d.Coord3D

// ClampColor clamps the color into the range [0, 1].
func ClampColor(c Color) Color {
	return c.Max(Color{}).Min(Color{X: 1, Y: 1, Z: 1})
}

// NewColor creates a Color with a given brightness.
func NewColor(b float64) Color {
	return model3d.Ones(b)
}

// NewColorRGB creates a Color from sRGB values.
func NewColorRGB(r, g, b float64) Color {
	return Color{X: gammaExpand(r), Y: gammaExpand(g), Z: gammaExpand(b)}
}

// RGB gets sRGB values for a Color.
func RGB(c Color) (float64, float64, float64) {
	return gammaCompress(c.X), gammaCompress(c.Y), gammaCompress(c.Z)
}

func gammaCompress(u float64) float64 {
	if u <= 0.0031308 {
		return 12.92 * u
	} else {
		return 1.055*math.Pow(u, 1/2.4) - 0.055
	}
}

func gammaExpand(u float64) float64 {
	if u <= 0.04045 {
		return u / 12.92
	} else {
		return math.Pow((u+0.055)/1.055, 2.4)
	}
}

// A PointLight is a light eminating from a point and
// going in all directions equally.
type PointLight struct {
	Origin model3d.Coord3D
	Color  Color

	// If true, the ray tracer should use an inverse
	// square relation to dim this light as it gets
	// farther from an object.
	QuadDropoff bool
}

// ColorAtDistance gets the Color produced by this light
// at some distance.
func (p *PointLight) ColorAtDistance(distance float64) Color {
	if !p.QuadDropoff {
		return p.Color
	}
	return p.Color.Scale(1 / (distance * distance))
}

// ShadeCollision determines a scaled color for a surface
// light collision.
func (p *PointLight) ShadeCollision(normal, pointToLight model3d.Coord3D) Color {
	dist := pointToLight.Norm()
	color := p.ColorAtDistance(dist)

	// Multiply by a density correction that comes from
	// lambertian shading.
	// In essence, when doing simple ray tracing, we want
	// the brightest part of a lambertian surface to have
	// the same brightness as the point light.
	density := 0.25 * math.Max(0, normal.Dot(pointToLight.Scale(1/dist)))

	return color.Scale(density)
}

// AreaLight is a surface that emits light.
//
// For regular path tracing, AreaLight is not needed,
// since lights are just regular objects.
// For global illumination methods that trace paths from
// lights to the scene, AreaLights are needed to sample
// from the light sources.
//
// An AreaLight should not be reflective in any way;
// its BSDF should be zero everywhere.
type AreaLight interface {
	Object

	// SampleLight samples a point on the surface of the
	// light with a probability density proportional to
	// the sum of the emission R, G, and B.
	//
	// Returns both the normal to the light, and the
	// emission at the sampled point.
	SampleLight(gen *rand.Rand) (point, normal model3d.Coord3D, emission Color)

	// TotalEmission gets the integral of the emission
	// over the area of the light, where the red, green,
	// and blue components are summed together.
	TotalEmission() float64
}

// AreaDensityLight is an AreaLight whose sampling proposal
// can be evaluated directly as a probability density with
// respect to world surface area.
//
// This decouples the sampling guide (the proposal used by
// bidirectional rendering) from the actual emission: the
// density returned here describes how SampleLight chooses
// points, and must not be derived from the emitted color.
// Lights without emission at a point may still carry
// positive proposal density.
//
// All densities are per unit of world surface area (not
// per solid angle). To convert an area density p_A(x) at a
// surface point x into the directional density measured
// relative to a uniform unit sphere at a point y at
// distance r = |x-y|, use the solid-angle Jacobian
//
//	p_w(omega) = p_A(x) * r^2 / |cos(theta_x)|
//
// where theta_x is the angle between the surface normal at
// x and the direction from x to y. The library's material
// densities (e.g. LambertMaterial.SourceDensity) are
// measured relative to a uniform unit sphere, so a
// cosine-weighted hemisphere sample with density
// 4*cos(theta) per solid angle corresponds to
// p_A(x) = 4*cos(theta)*|cos(theta_x)|/r^2 per unit area.
//
// LightDensity returns 0 for points that are not on the
// light's surface or whose normal does not match the
// surface normal at the point. A point counts as on the
// surface when its distance to the supporting surface is
// at most areaDensityEpsilon and its projection lies
// inside the surface (expanded by areaDensityEpsilon);
// normals match when their dot product is at least
// 1-areaDensityNormalSlack. Points on edges shared by
// multiple faces are assigned uniquely to the first face
// in the light's fixed face ordering.
type AreaDensityLight interface {
	AreaLight

	// LightDensity returns the world-area probability
	// density of sampling (point, normal) from this
	// light's proposal.
	LightDensity(point, normal model3d.Coord3D) float64
}

// Geometric tolerances used by LightDensity implementations
// to decide whether a queried point lies on a surface and
// whether its normal matches. See the AreaDensityLight
// documentation for the exact rules.
const (
	// areaDensityEpsilon is the maximum distance a query
	// point may be from the supporting surface (or outside
	// its boundary) while still counting as on it.
	areaDensityEpsilon = 1e-7

	// areaDensityNormalSlack bounds 1 minus the dot product
	// between the queried normal and the surface normal.
	areaDensityNormalSlack = 1e-6
)

// SphereAreaLight is a perfect sphere implementing an
// area light.
type SphereAreaLight struct {
	Object
	sphere   *model3d.Sphere
	emission Color
}

// NewSphereAreaLight turns a sphere collider into an area
// light.
func NewSphereAreaLight(s *model3d.Sphere, emission Color) *SphereAreaLight {
	return &SphereAreaLight{
		Object: &ColliderObject{
			Collider: s,
			Material: &LambertMaterial{EmissionColor: emission},
		},
		sphere:   s,
		emission: emission,
	}
}

func (s *SphereAreaLight) SampleLight(gen *rand.Rand) (point, normal model3d.Coord3D,
	emission Color) {
	// Don't use model3d.NewCoord3DRandUnit() because it
	// won't use our per-thread RNG.
	for {
		normal = model3d.XYZ(gen.NormFloat64(), gen.NormFloat64(), gen.NormFloat64())
		n := normal.Norm()
		if n > 0.01 && n < 100.0 {
			normal = normal.Scale(1 / n)
			break
		}
	}
	point = s.sphere.Center.Add(normal.Scale(s.sphere.Radius))
	emission = s.emission
	return
}

func (s *SphereAreaLight) TotalEmission() float64 {
	return s.emission.Sum() * 4 * math.Pi * s.sphere.Radius * s.sphere.Radius
}

// CylinderAreaLight is a cylinder implementing an area
// light.
type CylinderAreaLight struct {
	Object
	cylinder *model3d.Cylinder
	emission Color

	sideArea  float64
	shaftArea float64
}

// NewCylinderAreaLight turns a cylinder collider into an
// area light.
func NewCylinderAreaLight(c *model3d.Cylinder, emission Color) *CylinderAreaLight {
	return &CylinderAreaLight{
		Object: &ColliderObject{
			Collider: c,
			Material: &LambertMaterial{EmissionColor: emission},
		},
		cylinder:  c,
		emission:  emission,
		sideArea:  c.Radius * c.Radius * math.Pi,
		shaftArea: 2 * c.Radius * math.Pi * c.P2.Dist(c.P1),
	}
}

func (c *CylinderAreaLight) SampleLight(gen *rand.Rand) (point, normal model3d.Coord3D,
	emission Color) {
	unscaledAxis := c.cylinder.P2.Sub(c.cylinder.P1)
	axis := unscaledAxis.Normalize()
	x, y := axis.OrthoBasis()

	totalArea := c.shaftArea + 2*c.sideArea

	theta := gen.Float64() * 2 * math.Pi
	radialPart := x.Scale(math.Cos(theta)).Add(y.Scale(math.Sin(theta)))

	part := gen.Float64() * totalArea
	if part < 2*c.sideArea {
		// Sample a face.
		center := c.cylinder.P2
		normal = axis
		if part < c.sideArea {
			center = c.cylinder.P1
			normal = axis.Scale(-1)
		}
		radius := c.cylinder.Radius * math.Sqrt(gen.Float64())
		point = center.Add(radialPart.Scale(radius))
	} else {
		// Sample the shaft.
		y := gen.Float64()
		point = c.cylinder.P1.Add(unscaledAxis.Scale(y)).Add(radialPart)
		normal = radialPart
	}
	emission = c.emission
	return
}

func (c *CylinderAreaLight) TotalEmission() float64 {
	return c.emission.Sum() * (c.shaftArea + 2*c.sideArea)
}

// MeshAreaLight is an AreaLight for the surface of a
// mesh.
type MeshAreaLight struct {
	Object
	emission  Color
	triangles []*model3d.Triangle
	cumuAreas []float64
	totalArea float64
}

// NewMeshAreaLight creates an efficient area light from
// the triangle mesh.
func NewMeshAreaLight(mesh *model3d.Mesh, emission Color) *MeshAreaLight {
	m := &MeshAreaLight{
		Object: &ColliderObject{
			Collider: model3d.MeshToCollider(mesh),
			Material: &LambertMaterial{EmissionColor: emission},
		},
		emission:  emission,
		triangles: mesh.TriangleSlice(),
	}
	m.cumuAreas = make([]float64, len(m.triangles))
	for i, t := range m.triangles {
		m.totalArea += t.Area()
		m.cumuAreas[i] = m.totalArea
	}
	return m
}

func (m *MeshAreaLight) SampleLight(gen *rand.Rand) (point, normal model3d.Coord3D,
	emission Color) {
	triIdx := sort.SearchFloat64s(m.cumuAreas, gen.Float64()*m.totalArea)
	if triIdx == len(m.cumuAreas) {
		triIdx--
	}

	triangle := m.triangles[triIdx]

	// https://stackoverflow.com/questions/4778147/sample-random-point-in-triangle
	r1 := math.Sqrt(gen.Float64())
	r2 := gen.Float64()
	res := triangle[0].Scale(1 - r1)
	res = res.Add(triangle[1].Scale(r1 * (1 - r2)))
	res = res.Add(triangle[2].Scale(r1 * r2))
	return res, triangle.Normal(), m.emission
}

func (m *MeshAreaLight) TotalEmission() float64 {
	return m.totalArea * m.emission.Sum()
}

type joinedAreaLight struct {
	JoinedObject
	lights []AreaLight

	// active lists the indices into lights whose total
	// emission is positive. Completely black lights are
	// never selected by the aggregate proposal.
	active     []int
	cumuTotals []float64
	totalLight float64
}

// joinedDensityAreaLight is the AreaDensityLight variant
// of joinedAreaLight, used only when every active
// sub-light can evaluate an area density itself.
type joinedDensityAreaLight struct {
	*joinedAreaLight

	// densities[i] evaluates the proposal of active[i].
	densities []AreaDensityLight
}

// JoinAreaLights creates a larger AreaLight by combining
// smaller AreaLights.
//
// Sub-lights are selected with probability proportional to
// their TotalEmission, so completely black sub-lights are
// never selected. When every non-black sub-light
// implements AreaDensityLight, the aggregate implements it
// as well and LightDensity multiplies the sub-light's own
// area density by the actual sub-light selection
// probability. If any active sub-light only supports the
// legacy protocol, the aggregate is an ordinary AreaLight
// and bidirectional rendering falls back to the original
// emission-based proposal for the whole aggregate.
func JoinAreaLights(lights ...AreaLight) AreaLight {
	jo := make(JoinedObject, len(lights))
	for i, l := range lights {
		jo[i] = l
	}
	j := &joinedAreaLight{
		JoinedObject: jo,
		lights:       lights,
	}
	allDensity := true
	var densities []AreaDensityLight
	for i, l := range lights {
		total := l.TotalEmission()
		if total <= 0 {
			continue
		}
		j.active = append(j.active, i)
		j.totalLight += total
		j.cumuTotals = append(j.cumuTotals, j.totalLight)
		if dl, ok := l.(AreaDensityLight); ok {
			densities = append(densities, dl)
		} else {
			allDensity = false
		}
	}
	if allDensity {
		return &joinedDensityAreaLight{joinedAreaLight: j, densities: densities}
	}
	return j
}

func (j *joinedAreaLight) SampleLight(gen *rand.Rand) (point, normal model3d.Coord3D,
	emission Color) {
	if len(j.lights) == 0 {
		// An empty join has no surface to sample; return
		// finite zero values rather than indexing into an
		// empty slice.
		return model3d.Coord3D{}, model3d.Coord3D{}, Color{}
	}
	if len(j.active) == 0 {
		// Every sub-light is black. Sampling cannot happen
		// in a working render, but the call must stay
		// deterministic and free of out-of-bounds access.
		point, normal, emission = j.lights[0].SampleLight(gen)
		return
	}
	idx := sort.SearchFloat64s(j.cumuTotals, gen.Float64()*j.totalLight)
	if idx == len(j.cumuTotals) {
		idx = len(j.cumuTotals) - 1
	}
	point, normal, emission = j.lights[j.active[idx]].SampleLight(gen)
	return
}

func (j *joinedAreaLight) TotalEmission() float64 {
	return j.totalLight
}

// LightDensity evaluates the aggregate area proposal. The
// point is attributed to at most one sub-light: the
// active sub-lights are queried in fixed order, and the
// first sub-light whose surface contains the point owns it
// (shared edges are thus assigned deterministically). The
// sub-light selection probability
// TotalEmission/aggregateTotal is multiplied into the
// returned density. Points off every surface or with a
// mismatching normal return zero.
func (j *joinedDensityAreaLight) LightDensity(point, normal model3d.Coord3D) float64 {
	if len(j.active) == 0 || j.totalLight <= 0 {
		return 0
	}
	for k, idx := range j.active {
		if d := j.densities[k].LightDensity(point, normal); d > 0 {
			return j.lights[idx].TotalEmission() / j.totalLight * d
		}
	}
	return 0
}
