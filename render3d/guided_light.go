package render3d

import (
	"fmt"
	"math"
	"math/rand"
	"sort"

	"github.com/unixpickle/model3d/model3d"
)

// GuidedMeshAreaLight is an AreaLight made of flat
// triangles where each triangle has a constant,
// double-sided emission color and a separate,
// non-negative sampling guide.
//
// The guide is independent of the emission: triangles are
// selected with probability proportional to area*guide,
// and points are then sampled uniformly inside the chosen
// triangle. Changing the guide never changes the emitted
// radiance, only the proposal used by bidirectional
// rendering. When every guide is zero, triangles are
// selected proportionally to their area instead.
//
// Triangles are assumed not to overlap in their interiors,
// although sharing edges is allowed. The caller is
// responsible for guaranteeing this.
type GuidedMeshAreaLight struct {
	// Copied constructor data, so later mutations of the
	// caller's slices cannot affect the light.
	triangles []model3d.Triangle
	emissions []Color
	guides    []float64

	normals   []model3d.Coord3D
	areas     []float64
	materials []*LambertMaterial

	// masses stores area*guide for every triangle, or the
	// area when the all-zero fallback is active. cumuMass
	// holds cumulative masses for face selection.
	masses    []float64
	cumuMass  []float64
	totalMass float64

	totalEmission float64

	collider      model3d.Collider
	triangleIndex map[*model3d.Triangle]int
}

// NewGuidedMeshAreaLight creates a GuidedMeshAreaLight.
//
// The three slices must be non-empty and equally long.
// Every triangle must be non-degenerate with finite
// coordinates and parameters. Colors must be finite and
// non-negative, and guides must be finite and
// non-negative. Unless every guide is zero (in which case
// the light falls back to area-proportional sampling),
// every triangle with non-zero emission must have a
// positive guide.
//
// All input data is copied.
func NewGuidedMeshAreaLight(triangles []*model3d.Triangle, emission []Color,
	guide []float64) (AreaLight, error) {
	n := len(triangles)
	if n == 0 {
		return nil, fmt.Errorf("NewGuidedMeshAreaLight: need at least one triangle")
	}
	if len(emission) != n {
		return nil, fmt.Errorf("NewGuidedMeshAreaLight: got %d triangles but %d colors",
			n, len(emission))
	}
	if len(guide) != n {
		return nil, fmt.Errorf("NewGuidedMeshAreaLight: got %d triangles but %d guides",
			n, len(guide))
	}

	g := &GuidedMeshAreaLight{
		triangles:     make([]model3d.Triangle, n),
		emissions:     make([]Color, n),
		guides:        make([]float64, n),
		normals:       make([]model3d.Coord3D, n),
		areas:         make([]float64, n),
		materials:     make([]*LambertMaterial, n),
		masses:        make([]float64, n),
		cumuMass:      make([]float64, n),
		triangleIndex: map[*model3d.Triangle]int{},
	}

	allZeroGuide := true
	for i, t := range triangles {
		if t == nil {
			return nil, fmt.Errorf("NewGuidedMeshAreaLight: triangle %d is nil", i)
		}
		if !isFiniteCoord(t[0]) || !isFiniteCoord(t[1]) || !isFiniteCoord(t[2]) {
			return nil, fmt.Errorf("NewGuidedMeshAreaLight: triangle %d has non-finite coords", i)
		}
		area := t.Area()
		if !(area > 0) || math.IsNaN(area) || math.IsInf(area, 0) {
			return nil, fmt.Errorf("NewGuidedMeshAreaLight: triangle %d is degenerate", i)
		}
		c := emission[i]
		if !isFiniteColor(c) || c.X < 0 || c.Y < 0 || c.Z < 0 {
			return nil, fmt.Errorf("NewGuidedMeshAreaLight: emission %d must be finite and "+
				"non-negative, got %v", i, c)
		}
		v := guide[i]
		if math.IsNaN(v) || math.IsInf(v, 0) || v < 0 {
			return nil, fmt.Errorf("NewGuidedMeshAreaLight: guide %d must be finite and "+
				"non-negative, got %v", i, v)
		}
		if v > 0 {
			allZeroGuide = false
		}

		g.triangles[i] = *t
		g.emissions[i] = c
		g.guides[i] = v
		g.areas[i] = area
		g.normals[i] = t.Normal()
		g.materials[i] = &LambertMaterial{EmissionColor: c}
		g.totalEmission += area * c.Sum()
	}

	if !allZeroGuide {
		for i := range triangles {
			if g.emissions[i].Sum() > 0 && g.guides[i] <= 0 {
				return nil, fmt.Errorf("NewGuidedMeshAreaLight: emitting triangle %d has a "+
					"zero guide; give it a positive guide or set every guide to zero for "+
					"area-proportional sampling", i)
			}
		}
	}
	for i := range g.triangles {
		mass := g.areas[i] * g.guides[i]
		if allZeroGuide {
			mass = g.areas[i]
		}
		g.masses[i] = mass
		g.totalMass += mass
		g.cumuMass[i] = g.totalMass
	}

	// Own collider made from private triangle copies, so
	// Cast can report the per-triangle emission material.
	colliderTris := make([]*model3d.Triangle, n)
	for i := range g.triangles {
		t := g.triangles[i]
		colliderTris[i] = &t
	}
	model3d.GroupTriangles(colliderTris)
	for i, t := range colliderTris {
		g.triangleIndex[t] = i
	}
	g.collider = model3d.GroupedTrianglesToCollider(colliderTris)

	return g, nil
}

// Min gets the minimum of the bounding box.
func (g *GuidedMeshAreaLight) Min() model3d.Coord3D {
	return g.collider.Min()
}

// Max gets the maximum of the bounding box.
func (g *GuidedMeshAreaLight) Max() model3d.Coord3D {
	return g.collider.Max()
}

// Cast returns the first ray collision together with the
// constant emission material of the triangle that was
// actually hit. Emission is double-sided.
func (g *GuidedMeshAreaLight) Cast(r *model3d.Ray) (model3d.RayCollision, Material, bool) {
	coll, ok := g.collider.FirstRayCollision(r)
	if !ok {
		return coll, nil, false
	}
	tri := coll.Extra.(*model3d.TriangleCollision).Triangle
	return coll, g.materials[g.triangleIndex[tri]], true
}

// SampleLight picks a triangle with probability
// proportional to area*guide (area alone when all guides
// are zero) and then samples a point uniformly inside it.
// The returned emission is the triangle's true emission,
// independent of the guide.
func (g *GuidedMeshAreaLight) SampleLight(gen *rand.Rand) (point, normal model3d.Coord3D,
	emission Color) {
	triIdx := sort.SearchFloat64s(g.cumuMass, gen.Float64()*g.totalMass)
	if triIdx == len(g.cumuMass) {
		triIdx--
	}
	triangle := &g.triangles[triIdx]

	// Uniform point in a triangle; consumes the same two
	// random numbers as MeshAreaLight.SampleLight.
	r1 := math.Sqrt(gen.Float64())
	r2 := gen.Float64()
	point = triangle[0].Scale(1 - r1)
	point = point.Add(triangle[1].Scale(r1 * (1 - r2)))
	point = point.Add(triangle[2].Scale(r1 * r2))
	return point, g.normals[triIdx], g.emissions[triIdx]
}

// TotalEmission returns the area integral of the summed
// emission channels. It never involves the guides.
func (g *GuidedMeshAreaLight) TotalEmission() float64 {
	return g.totalEmission
}

// LightDensity returns the world-area proposal density at
// a surface point. See the AreaDensityLight documentation
// for the on-surface and normal tolerances and for the
// conversion to solid-angle densities.
//
// Points on edges shared by neighboring triangles are
// assigned to the first triangle in the fixed constructor
// ordering, so the result is unique. Points off the
// surface or with a mismatching normal return zero, as do
// points whose triangle has zero proposal mass.
func (g *GuidedMeshAreaLight) LightDensity(point, normal model3d.Coord3D) float64 {
	for i := range g.triangles {
		bary, dist := triangleBarycentric(&g.triangles[i], point)
		if dist > areaDensityEpsilon {
			continue
		}
		if bary[0] < -areaDensityEpsilon || bary[1] < -areaDensityEpsilon ||
			bary[2] < -areaDensityEpsilon {
			continue
		}
		if normal.Dot(g.normals[i]) < 1-areaDensityNormalSlack {
			return 0
		}
		if g.masses[i] <= 0 {
			return 0
		}
		return g.masses[i] / (g.totalMass * g.areas[i])
	}
	return 0
}

// triangleBarycentric projects p onto the plane of t and
// returns the barycentric coordinates plus the distance to
// the plane. The coordinates sum to one even when p is
// not exactly on the plane.
func triangleBarycentric(t *model3d.Triangle, p model3d.Coord3D) ([3]float64, float64) {
	v1 := t[1].Sub(t[0])
	v2 := t[2].Sub(t[0])
	d := p.Sub(t[0])
	n := v1.Cross(v2)
	nn := n.Dot(n)

	// Solve d = b1*v1 + b2*v2 + out-of-plane component in
	// the least-squares sense; the cross-product identity
	// makes this exact for the in-plane projection.
	b1 := d.Cross(v2).Dot(n) / nn
	b2 := v1.Cross(d).Dot(n) / nn
	b0 := 1 - b1 - b2
	dist := math.Abs(d.Dot(n.Normalize()))
	return [3]float64{b0, b1, b2}, dist
}

func isFiniteColor(c Color) bool {
	return !math.IsNaN(c.X) && !math.IsInf(c.X, 0) &&
		!math.IsNaN(c.Y) && !math.IsInf(c.Y, 0) &&
		!math.IsNaN(c.Z) && !math.IsInf(c.Z, 0)
}

func isFiniteCoord(c model3d.Coord3D) bool {
	return isFiniteColor(c)
}

var (
	_ AreaDensityLight = &GuidedMeshAreaLight{}
	_ AreaDensityLight = &joinedDensityAreaLight{}
)
