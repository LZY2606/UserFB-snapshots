package ln

import (
	"errors"
	"fmt"
	"math"
	"sort"
)

// ClipVisiblePaths splits the given world-space polylines into their visible
// pieces as seen from eye. A point on a path is hidden when the open line
// segment from eye to that point passes through the interior of a sphere in
// the scene. Hidden parameter ranges are found analytically for every source
// segment, so no sampling or step size is involved.
//
// Only *Sphere occluders are accepted. An empty scene is valid. The input
// paths, the scene and the spheres are never modified; on error no partial
// result is published.
func (s *Scene) ClipVisiblePaths(paths Paths, eye Vector) (Paths, error) {
	spheres, err := validateClipInputs(s, paths, eye)
	if err != nil {
		return nil, err
	}
	var result Paths
	for _, path := range paths {
		result = append(result, clipOnePath(path, spheres, eye)...)
	}
	return result, nil
}

func validateClipInputs(s *Scene, paths Paths, eye Vector) ([]*Sphere, error) {
	if !vectorIsFinite(eye) {
		return nil, errors.New("ClipVisiblePaths: eye contains a non-finite coordinate")
	}
	for pi, path := range paths {
		for vi, v := range path {
			if !vectorIsFinite(v) {
				return nil, fmt.Errorf("ClipVisiblePaths: path %d vertex %d contains a non-finite coordinate", pi, vi)
			}
		}
	}
	spheres := make([]*Sphere, 0, len(s.Shapes))
	for i, shape := range s.Shapes {
		sphere, ok := shape.(*Sphere)
		if !ok {
			return nil, fmt.Errorf("ClipVisiblePaths: shape %d is %T, only *Sphere occluders are supported", i, shape)
		}
		if !vectorIsFinite(sphere.Center) {
			return nil, fmt.Errorf("ClipVisiblePaths: sphere %d has a non-finite center", i)
		}
		if math.IsNaN(sphere.Radius) || math.IsInf(sphere.Radius, 0) {
			return nil, fmt.Errorf("ClipVisiblePaths: sphere %d has a non-finite radius", i)
		}
		if sphere.Radius <= 0 {
			return nil, fmt.Errorf("ClipVisiblePaths: sphere %d has non-positive radius %g", i, sphere.Radius)
		}
		// Eye on the surface is rejected as well as inside the sphere.
		if d2 := eye.Sub(sphere.Center).LengthSquared(); d2 <= sphere.Radius*sphere.Radius {
			return nil, fmt.Errorf("ClipVisiblePaths: eye is inside or on the surface of sphere %d", i)
		}
		spheres = append(spheres, sphere)
	}
	return spheres, nil
}

func vectorIsFinite(v Vector) bool {
	return !math.IsNaN(v.X) && !math.IsInf(v.X, 0) &&
		!math.IsNaN(v.Y) && !math.IsInf(v.Y, 0) &&
		!math.IsNaN(v.Z) && !math.IsInf(v.Z, 0)
}

// ---------------------------------------------------------------------------
// Interval solving on the segment parameter u in [0, 1].
//
// For a segment a -> b and a sphere centered at c with radius r, the view ray
// is A(t) = eye + t*(P(u) - eye), t in (0, 1), with P(u) = a + u*(b - a).
// Writing w = eye - c and m(u) = P(u) - eye gives
//
//	|A(t) - c|^2 - r^2 = alpha*t^2 + 2*beta*t + gamma
//	alpha(u) = |m|^2, beta(u) = m·w, gamma = |w|^2 - r^2 > 0
//
// The open segment crosses the interior exactly when the quadratic has a
// positive discriminant and its roots straddle the open interval (0, 1):
//
//	q(u) = beta^2 - alpha*gamma > 0          (strict: tangency is no crossing)
//	beta(u) < 0                              (far root t+ > 0)
//	h(u) = r^2 - |P(u) - c|^2 > 0            (P inside the sphere), or
//	alpha(u) + beta(u) > 0                   (near root t- < 1)
//
// Every condition is quadratic in u, so the hidden parameter ranges are
// obtained by intersecting sign intervals of polynomials.
// ---------------------------------------------------------------------------

type realInterval struct {
	lo, hi float64
}

const (
	intervalEps = 1e-12
	rootEps     = 1e-14
)

// quadRoots returns the sorted real roots of p2*u^2 + p1*u + p0 = 0.
func quadRoots(p0, p1, p2 float64) []float64 {
	scale := math.Max(math.Max(math.Abs(p0), math.Abs(p1)), math.Abs(p2))
	if scale == 0 {
		return nil
	}
	eps := rootEps * scale
	if math.Abs(p2) <= eps {
		if math.Abs(p1) <= eps {
			return nil
		}
		return []float64{-p0 / p1}
	}
	disc := p1*p1 - 4*p2*p0
	discEps := rootEps * math.Max(p1*p1, math.Abs(4*p2*p0))
	if disc <= discEps {
		// No two distinct roots: a tangency must not open a strict-sign gap.
		if disc >= -discEps && math.Abs(p1) > rootEps*scale {
			return []float64{-p1 / (2 * p2)}
		}
		return nil
	}
	s := math.Sqrt(math.Max(0, disc))
	q := -0.5 * (p1 + math.Copysign(s, p1))
	roots := []float64{q / p2, 0}
	if q == 0 {
		roots[1] = -p1 / p2
	} else {
		roots[1] = p0 / q
	}
	if roots[0] > roots[1] {
		roots[0], roots[1] = roots[1], roots[0]
	}
	if roots[0] == roots[1] {
		return roots[:1]
	}
	return roots
}

// solveQuadSign returns the closed sub-intervals of [lo, hi] on which the
// quadratic p(u) is strictly positive (wantPositive == true) or strictly
// negative. Roots themselves do not satisfy a strict sign, but they are kept
// as interval boundaries so that surface points become output endpoints.
func solveQuadSign(p0, p1, p2, lo, hi float64, wantPositive bool) []realInterval {
	bounds := []float64{lo}
	for _, r := range quadRoots(p0, p1, p2) {
		if r > lo && r < hi {
			bounds = append(bounds, r)
		}
	}
	bounds = append(bounds, hi)
	var out []realInterval
	for i := 0; i < len(bounds)-1; i++ {
		x0, x1 := bounds[i], bounds[i+1]
		mid := p0 + (p1+p2*((x0+x1)/2))*((x0+x1)/2)
		positive := mid > 0
		if positive == wantPositive && mid != 0 {
			out = append(out, realInterval{x0, x1})
		}
	}
	return out
}

func intersectIntervals(a, b []realInterval) []realInterval {
	var out []realInterval
	i, j := 0, 0
	for i < len(a) && j < len(b) {
		lo := math.Max(a[i].lo, b[j].lo)
		hi := math.Min(a[i].hi, b[j].hi)
		if lo <= hi {
			out = append(out, realInterval{lo, hi})
		}
		if a[i].hi < b[j].hi {
			i++
		} else {
			j++
		}
	}
	return out
}

func unionIntervals(groups ...[]realInterval) []realInterval {
	var all []realInterval
	for _, g := range groups {
		all = append(all, g...)
	}
	if len(all) == 0 {
		return nil
	}
	sort.Slice(all, func(i, j int) bool { return all[i].lo < all[j].lo })
	merged := []realInterval{all[0]}
	for _, in := range all[1:] {
		last := &merged[len(merged)-1]
		if in.lo <= last.hi+intervalEps {
			if in.hi > last.hi {
				last.hi = in.hi
			}
		} else {
			merged = append(merged, in)
		}
	}
	return merged
}

// hiddenIntervals returns the parameter ranges of segment a -> b whose view
// rays from eye pass through the interior of this sphere.
func (sphere *Sphere) hiddenIntervals(a, b, eye Vector) []realInterval {
	v := b.Sub(a) // segment direction
	w := eye.Sub(sphere.Center)
	m0 := a.Sub(eye)          // m(u) = m0 + u*v
	k := a.Sub(sphere.Center) // P(u) - c = k + u*v

	dd := m0.Dot(m0)
	dv := m0.Dot(v)
	vv := v.Dot(v)
	dw := m0.Dot(w)
	vw := v.Dot(w)
	ww := w.Dot(w)
	kv := k.Dot(v)
	kk := k.Dot(k)
	r2 := sphere.Radius * sphere.Radius
	gamma := ww - r2 // strictly positive: eye was validated outside every sphere

	// q(u) = beta^2 - alpha*gamma
	q0 := dw*dw - dd*gamma
	q1 := 2*dw*vw - 2*dv*gamma
	q2 := vw*vw - vv*gamma
	// beta(u) = dw + vw*u, hidden requires beta < 0
	// h(u) = r^2 - |P(u) - c|^2, hidden may have h > 0
	h0 := r2 - kk
	h1 := -2 * kv
	h2 := -vv
	// alpha + beta, hidden may have alpha + beta > 0
	m0c := dd + dw
	m1c := 2*dv + vw
	m2c := vv

	qPos := solveQuadSign(q0, q1, q2, 0, 1, true)
	betaNeg := solveQuadSign(dw, vw, 0, 0, 1, false)
	hPos := solveQuadSign(h0, h1, h2, 0, 1, true)
	mPos := solveQuadSign(m0c, m1c, m2c, 0, 1, true)

	hidden := intersectIntervals(qPos, betaNeg)
	hidden = intersectIntervals(hidden, unionIntervals(hPos, mPos))
	return clampIntervals(hidden)
}

func clampIntervals(in []realInterval) []realInterval {
	var out []realInterval
	for _, iv := range in {
		lo := math.Max(0, iv.lo)
		hi := math.Min(1, iv.hi)
		if lo <= hi {
			out = append(out, realInterval{lo, hi})
		}
	}
	return out
}

// visibleIntervals returns the complement, within [0, 1], of the union of the
// hidden ranges produced by every sphere. Touching or overlapping hidden
// ranges are merged first, so no phantom visible slivers can appear.
func visibleIntervals(a, b, eye Vector, spheres []*Sphere) []realInterval {
	var hidden []realInterval
	for _, sphere := range spheres {
		hidden = unionIntervals(hidden, sphere.hiddenIntervals(a, b, eye))
	}
	var visible []realInterval
	cur := 0.0
	for _, iv := range hidden {
		if iv.lo > cur+intervalEps {
			visible = append(visible, realInterval{cur, iv.lo})
		}
		if iv.hi > cur {
			cur = iv.hi
		}
	}
	if cur < 1-intervalEps {
		visible = append(visible, realInterval{cur, 1})
	}
	return visible
}

// ---------------------------------------------------------------------------
// Polyline assembly: source ordering and connectivity are preserved. Segments
// sharing a visible vertex stay in one Path; an occluded break always starts a
// new Path; distinct source Paths are never merged.
// ---------------------------------------------------------------------------

func clipOnePath(path Path, spheres []*Sphere, eye Vector) Paths {
	var result Paths
	var current Path
	flush := func() {
		if len(current) >= 2 {
			result = append(result, current)
		}
		current = nil
	}
	for i := 0; i+1 < len(path); i++ {
		a, b := path[i], path[i+1]
		if a == b {
			continue // degenerate segment, contributes no geometry
		}
		v := b.Sub(a)
		visible := visibleIntervals(a, b, eye, spheres)
		for j, iv := range visible {
			start := a
			if iv.lo != 0 {
				start = a.Add(v.MulScalar(iv.lo))
			}
			end := b
			if iv.hi != 1 {
				end = a.Add(v.MulScalar(iv.hi))
			}
			joins := j == 0 && iv.lo == 0 && len(current) > 0 && current[len(current)-1] == a
			if !joins {
				flush()
				current = Path{start}
			}
			if end != current[len(current)-1] {
				current = append(current, end)
			}
		}
	}
	flush()
	return result
}
