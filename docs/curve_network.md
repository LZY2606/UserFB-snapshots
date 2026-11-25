# Bidirectional curve networks

Nanospline can build a single native tensor-product
`BSplinePatch<double,3,-1,-1>` from two intersecting families of clamped,
non-periodic, non-rational 3D B-spline curves.

The entry points are declared in
[`include/nanospline/CurveNetwork.h`](../include/nanospline/CurveNetwork.h):

```cpp
nanospline::CurveNetwork network;
std::string error;
if (!network.build(specification, &error)) {
    // network.surface() and the previous successful result are unchanged.
}
const nanospline::BSplinePatch<double,3,-1,-1>& surface = network.surface();
```

There is also a direct output overload and a throwing overload:

```cpp
bool nanospline::buildCurveNetwork(specification, patch, &error);
auto patch = nanospline::createCurveNetwork(specification);
```

`CurveNetwork` additionally owns the `ReparameterizedBSpline` objects used for
the horizontal and vertical families.

## Intersection tables and site convention

Let there be `m` horizontal curves `C_j` and `n` vertical curves `D_i`, where
`2 <= m,n <= 6`.

- `C_j` corresponds to the common site `V_j`, and `spec.vSites.size() == m`.
- `D_i` corresponds to the common site `U_i`, and `spec.uSites.size() == n`.
- `spec.a[j][i]` is the parameter on `C_j` of its intersection with `D_i`.
- `spec.b[i][j]` is the parameter on `D_i` of the same intersection.

Thus,

```text
C_j(a[j][i]) = D_i(b[i][j]) = P_ij
```

within the required spatial tolerance of `1e-10`.

Both `uSites` and `vSites` start at `0`, end at `1`, are strictly increasing,
and have adjacent spacing at least `0.05`. Every row of `a` and `b` is
strictly monotone. Its first and last entries are the corresponding curve's
two domain endpoints, so a row may run in either parameter direction. A
decreasing row denotes a curve whose orientation is reversed relative to the
common axis.

The two evaluations are averaged for the Gordon intersection net:

```text
P_ij = (C_j(a[j][i]) + D_i(b[i][j])) / 2
```

This avoids modifying either input curve when the two evaluations differ only
by floating-point-scale noise.

Malformed structures, non-finite values, bad/duplicate sites, non-monotone
tables, invalid/non-C1 curves, and incompatible intersections are rejected
before assembly. `CurveNetwork::build` builds into local storage and publishes
the result only after all numerical stages succeed; therefore a failed retry
does not overwrite an existing surface. Numerical solve failures are reported
with `CurveNetworkErrorCode::NumericalSolveFailure` and an explanatory message.

## Fixed monotone Hermite reparameterization

For every curve, `MonotoneHermiteMap` constructs the map from common sites to
the curve's own parameter. The maps and their first derivatives are public:

```cpp
double phi = network.horizontalReparameterizations()[j].map().evaluate(u);
double dphi = network.horizontalReparameterizations()[j].map().derivative(u);
```

For a decreasing parameter row, all parameter values are first negated. This
makes the interpolation data increasing. Let the common sites be `x_k` and the
canonical increasing values be `y_k`. For each interval,

```text
h_k = x_{k+1} - x_k
delta_k = (y_{k+1} - y_k) / h_k
```

Initial slopes are:

```text
m_0       = delta_0
m_{N-1}   = delta_{N-2}
m_k       = 2 / (1/delta_{k-1} + 1/delta_k)   for interior k
```

Then intervals are processed from left to right. For interval `k`, define

```text
alpha = m_k / delta_k
beta  = m_{k+1} / delta_k
r     = sqrt(alpha^2 + beta^2)
```

If `r > 9`, both endpoint slopes are multiplied by `3/r`. The resulting values
and slopes define the standard cubic Hermite segment. For an originally
decreasing row, the stored map and derivative are given the restored negative
sign.

`ReparameterizedBSpline` composes the source B-spline polynomial pieces with
these cubic maps. If a source internal knot is `s`, the unique common
parameter `u = phi^{-1}(s)` is added as a composition breakpoint. Consequently
no output polynomial interval crosses an original source knot, and the
implementation does not merely rename or replace source knots.

On each resulting interval:

1. the source Bézier polynomial is expressed in power form;
2. the cubic Hermite map is shifted to that interval's left endpoint;
3. the two polynomials are composed exactly;
4. the degree `3*p` polynomial is converted to Bézier control points.

The pieces are stored as one native dynamic-degree
`BSpline<double,3,-1>` with repeated Bézier-style knots. The curve owns its
control points and knot vector; no `std::function`, curve pointer, evaluation
closure, or sampled fit is retained.

## Gordon surface and common spline space

The returned surface is

```text
S(u,v) = L_U(u,v) + L_V(u,v) - L_UV(u,v)
```

where:

- `L_U` interpolates the complete reparameterized horizontal curves through
  the `V_j` sites with a natural cubic spline in `v`;
- `L_V` interpolates the complete reparameterized vertical curves through the
  `U_i` sites with a natural cubic spline in `u`;
- `L_UV` is the tensor-product natural cubic interpolation of the averaged
  intersection net `P_ij`.

A two-site axis is linear. For three or more sites, “natural cubic” means
site interpolation, C2 continuity at internal sites, and zero second
derivative at both ends. The implementation solves the endpoint-second-
derivative equations directly.

All three terms are refined into one exact common tensor-product space:

- common degree `p_u` is at least every horizontal composition degree and at
  least `3` when U uses natural cubic interpolation;
- common degree `p_v` is defined analogously;
- the U break vector is the union of U sites and all horizontal composition
  breakpoints;
- the V break vector is the union of V sites and all vertical composition
  breakpoints.

Each axis uses a clamped Bézier-redundant knot vector. If an axis has `K`
intervals and degree `p`, it has

```text
N = K*p + 1
```

control points. Its knot vector contains `p+1` copies of each endpoint and `p`
copies of every internal break.

The final control grid is an `(N_u*N_v) × 3` Eigen matrix in the same
V-major layout as the rest of nanospline:

```text
linear_index(i,j) = i * N_v + j
```

where `i` is the U control index and `j` is the V control index. The three
Gordon terms are added control-by-control in this identical degree/knot space,
then one `BSplinePatch<double,3,-1,-1>` is initialized. Evaluation, iso curves,
derivatives, splitting, and ownership therefore all use the original
`BSplinePatch` implementation.

## Example and OBJ output

Build the example through the task script or directly:

```bash
c++ -std=c++17 -O2 -Iinclude -Ivendor/eigen \
  src/CurveNetwork.cpp src/ReparameterizedBSpline.cpp \
  examples/curve_network.cpp -o curve_network_example

./curve_network_example network.obj
```

The example constructs a non-planar `4 × 4` network with non-unit parameter
domains and one reversed table row, builds the native patch, and samples only
that patch. The OBJ contains:

- `v` vertex positions from `BSplinePatch::evaluate`;
- `vn` normals from `evaluate_derivative_u().cross(evaluate_derivative_v())`;
- quadrilateral `f` records referencing matching position/normal indices.

The verification target is:

```bash
bash task_support/run.sh target
```

It compiles the production files and `tests/curve_network.cpp`, runs the
`synchronization`, `surface`, and `compatibility` case groups, builds and runs
the OBJ example, and then runs the original baseline production path.
