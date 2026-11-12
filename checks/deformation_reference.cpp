// checks/deformation_reference.cpp
//
// Independent numerical reference for geometry deformation:
//   - Linear blend skinning (LBS), rigid dual quaternion skinning (DQ) and the
//     per-vertex DQ/LBS blend, including antipodal quaternion handling
//   - Blend shape (morph) offsets applied *before* skinning, including
//     in-between keyframe interval interpolation
//   - Deformed face normals accumulated by triangle area weighting inside
//     explicit smoothing groups
//
// The reference deliberately does NOT call any ufbx math, skinning, blend
// offset or normal generation helper. ufbx is only used to load scenes and to
// expose raw bind/pose/topology data. Matrix multiplication, quaternion
// algebra, dual quaternion transforms, weight handling and normal accumulation
// are all implemented below from first principles (in double precision, while
// the library uses float).
//
// Exit codes:
//   0 = all assertions passed
//   2 = numerical deformation mismatch (MUTANT_CAPTURE lines emitted)
//   3 = scene loading failed
//   4 = any other fatal failure

#include "ufbx.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

const double PI = 3.14159265358979323846;
const double DEG2RAD = PI / 180.0;
// The library's float epsilon is ~1.08e-19; valid weight sums must dwarf it.
const double WEIGHT_SUM_FLOOR = 1.0e-6;
const double POS_EPS = 1.0e-4;
const double NORMAL_EPS = 2.0e-3;

void fail_fatal(const char* fmt, ...);

// ---------------------------------------------------------------------------
// Independent math: everything here is written without ufbx helpers.
// Matrices are affine 4x3 in column-major order, same memory layout as
// ufbx_matrix: cols[0..2] are basis vectors, cols[3] the translation.
// ---------------------------------------------------------------------------

struct V3 { double x, y, z; };
struct Q4 { double x, y, z, w; };
struct M4 { double c[12]; }; // c[0..2]=col0, c[3..5]=col1, ...

V3 add(V3 a, V3 b) { return V3{a.x+b.x, a.y+b.y, a.z+b.z}; }
V3 sub(V3 a, V3 b) { return V3{a.x-b.x, a.y-b.y, a.z-b.z}; }
V3 mul(V3 a, double s) { return V3{a.x*s, a.y*s, a.z*s}; }
double dot(V3 a, V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
V3 cross(V3 a, V3 b) {
	return V3{a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}
double length(V3 a) { return std::sqrt(dot(a, a)); }
V3 normalize(V3 a) {
	double l = length(a);
	return l > 0.0 ? mul(a, 1.0/l) : V3{0,0,0};
}
bool finite_v(V3 a) {
	return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}

M4 identity() {
	M4 m{{1,0,0, 0,1,0, 0,0,1, 0,0,0}};
	return m;
}
M4 translation(V3 t) {
	M4 m = identity();
	m.c[9]=t.x; m.c[10]=t.y; m.c[11]=t.z;
	return m;
}
// Affine matrix multiplication r = a * b.
M4 mul(const M4& a, const M4& b) {
	M4 r;
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 3; row++) {
			r.c[col*3+row] =
				a.c[0*3+row]*b.c[col*3+0] +
				a.c[1*3+row]*b.c[col*3+1] +
				a.c[2*3+row]*b.c[col*3+2] +
				(col == 3 ? a.c[3*3+row] : 0.0);
		}
	}
	return r;
}
V3 transform(const M4& m, V3 v) {
	return V3{
		m.c[0]*v.x + m.c[3]*v.y + m.c[6]*v.z + m.c[9],
		m.c[1]*v.x + m.c[4]*v.y + m.c[7]*v.z + m.c[10],
		m.c[2]*v.x + m.c[5]*v.y + m.c[8]*v.z + m.c[11]};
}

Q4 q_identity() { return Q4{0,0,0,1}; }
double dot(Q4 a, Q4 b) { return a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w; }
Q4 scale(Q4 a, double s) { return Q4{a.x*s,a.y*s,a.z*s,a.w*s}; }
Q4 add(Q4 a, Q4 b) { return Q4{a.x+b.x,a.y+b.y,a.z+b.z,a.w+b.w}; }
Q4 normalize(Q4 a) {
	double l = std::sqrt(dot(a, a));
	return l > 0.0 ? scale(a, 1.0/l) : q_identity();
}
Q4 conjugate(Q4 a) { return Q4{-a.x,-a.y,-a.z,a.w}; }
Q4 quat_mul(Q4 a, Q4 b) {
	return Q4{
		a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
		a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
		a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
		a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z};
}
Q4 quat_from_rot_z(double angle) {
	return Q4{0.0, 0.0, std::sin(angle*0.5), std::cos(angle*0.5)};
}
// Rotation matrix -> unit quaternion through the trace branch method.
// Column convention matches ufbx: x=cols[0], y=cols[1], z=cols[2].
Q4 quat_from_matrix(const M4& m) {
	V3 x{m.c[0],m.c[1],m.c[2]};
	V3 y{m.c[3],m.c[4],m.c[5]};
	V3 z{m.c[6],m.c[7],m.c[8]};
	double trace = x.x + y.y + z.z;
	Q4 q;
	if (trace > 0.0) {
		double a = std::sqrt(trace + 1.0);
		double b = 0.5 / a;
		q.x = (y.z - z.y) * b;
		q.y = (z.x - x.z) * b;
		q.z = (x.y - y.x) * b;
		q.w = 0.5 * a;
	} else if (x.x > y.y && x.x > z.z) {
		double a = std::sqrt(1.0 + x.x - y.y - z.z);
		double b = 0.5 / a;
		q.x = 0.5 * a;
		q.y = (y.x + x.y) * b;
		q.z = (z.x + x.z) * b;
		q.w = (y.z - z.y) * b;
	} else if (y.y > z.z) {
		double a = std::sqrt(1.0 - x.x + y.y - z.z);
		double b = 0.5 / a;
		q.x = (y.x + x.y) * b;
		q.y = 0.5 * a;
		q.z = (z.y + y.z) * b;
		q.w = (z.x - x.z) * b;
	} else {
		double a = std::sqrt(1.0 - x.x - y.y + z.z);
		double b = 0.5 / a;
		q.x = (z.x + x.z) * b;
		q.y = (z.y + y.z) * b;
		q.z = 0.5 * a;
		q.w = (x.y - y.x) * b;
	}
	return normalize(q);
}
// Compose T * R * S (matches the FBX local transform convention).
M4 compose(V3 t, Q4 r, V3 s) {
	double x=r.x, y=r.y, z=r.z, w=r.w;
	double xx=x*x, xy=x*y, xz=x*z, xw=x*w;
	double yy=y*y, yz=y*z, yw=y*w, zz=z*z, zw=z*w;
	M4 m;
	double sx=2.0*s.x, sy=2.0*s.y, sz=2.0*s.z;
	m.c[0]=sx*(-yy-zz+0.5); m.c[1]=sx*(xy+zw);      m.c[2]=sx*(-yw+xz);
	m.c[3]=sy*(-zw+xy);     m.c[4]=sy*(-xx-zz+0.5); m.c[5]=sy*(xw+yz);
	m.c[6]=sz*(xz+yw);      m.c[7]=sz*(-xw+yz);     m.c[8]=sz*(-xx-yy+0.5);
	m.c[9]=t.x; m.c[10]=t.y; m.c[11]=t.z;
	return m;
}

struct TRS { V3 t; Q4 r; V3 s; double ortho_residual; };

// Independent matrix decomposition. Handles rigid transforms and positive,
// non-uniform scale. Sheared matrices are flagged via ortho_residual and the
// caller decides what to do (no new rules are invented for them).
TRS decompose(const M4& m) {
	V3 c0{m.c[0],m.c[1],m.c[2]};
	V3 c1{m.c[3],m.c[4],m.c[5]};
	V3 c2{m.c[6],m.c[7],m.c[8]};
	V3 s{length(c0), length(c1), length(c2)};
	V3 e0 = mul(c0, 1.0/s.x);
	V3 u1 = sub(c1, mul(e0, dot(c1, e0)));
	V3 e1 = normalize(u1);
	V3 u2 = sub(sub(c2, mul(e0, dot(c2, e0))), mul(e1, dot(c2, e1)));
	V3 e2 = normalize(u2);
	M4 rot{{e0.x,e0.y,e0.z, e1.x,e1.y,e1.z, e2.x,e2.y,e2.z, 0,0,0}};
	// Orthonormality residual: a rotation matrix must be orthogonal.
	V3 check0 = cross(e1, e2);
	double residual = length(sub(check0, e0));
	TRS out;
	out.t = V3{m.c[9],m.c[10],m.c[11]};
	out.r = quat_from_matrix(rot);
	out.s = s;
	out.ortho_residual = residual;
	return out;
}

// Raw ufbx matrix -> our matrix (plain scalar copy, no helper calls).
M4 from_ufbx(const ufbx_matrix& m) {
	M4 r;
	for (int i = 0; i < 12; i++) r.c[i] = (double)m.v[i];
	return r;
}
V3 from_ufbx(ufbx_vec3 v) { return V3{(double)v.x,(double)v.y,(double)v.z}; }

// ---------------------------------------------------------------------------
// Skinning data extracted from a loaded scene (raw scalars only).
// ---------------------------------------------------------------------------

struct BoneInfluence { size_t cluster; double weight; };

struct SkinData {
	std::vector<std::vector<BoneInfluence>> influences;
	std::vector<double> dq_weight;
	std::vector<M4> geometry_to_world; // per cluster
	M4 fallback;
	bool has_fallback;
};

struct MorphKey {
	double target_weight;
	std::vector<std::pair<uint32_t, V3>> offsets; // vertex -> shape offset
};
struct MorphChannel {
	std::vector<MorphKey> keys; // sorted by target weight
};

// ---------------------------------------------------------------------------
// Independent skinning implementation.
// ---------------------------------------------------------------------------

struct SkinResult { M4 lbs; M4 dq; double total_weight; bool antipode_flipped; };

// LBS matrix: normalized convex blend of the per-bone geometry-to-world maps.
M4 lbs_matrix(const SkinData& skin, size_t vertex) {
	M4 acc{{0,0,0,0,0,0,0,0,0,0,0,0}};
	double total = 0.0;
	for (const BoneInfluence& inf : skin.influences[vertex]) {
		for (int i = 0; i < 12; i++)
			acc.c[i] += inf.weight * skin.geometry_to_world[inf.cluster].c[i];
		total += inf.weight;
	}
	if (total > 0.0) {
		for (int i = 0; i < 12; i++) acc.c[i] /= total;
		return acc;
	}
	return skin.has_fallback ? skin.fallback : identity();
}

// Rigid dual quaternion transform. Two independent formulations are computed:
//  (a) blended unit dual quaternion, translation from 2*qe*conj(q0)/|q0|^2
//  (b) the real/dual component reconstruction, cross-checked against (a)
//
// The core takes explicit rigid (quaternion, translation, scale, weight)
// inputs: quaternion sign is only observable at this level, exactly where the
// library performs its antipodal correction (matrix round-trips erase it).
struct DQInput { Q4 q; V3 t; V3 s; double weight; };
struct DQCore { M4 matrix; bool antipode_flipped; double total_weight; };

DQCore dq_core(const std::vector<DQInput>& inputs, const M4& fallback)
{
	DQCore out;
	out.antipode_flipped = false;
	out.total_weight = 0.0;
	Q4 q0_first = q_identity();
	Q4 acc_q{0,0,0,0}, acc_e{0,0,0,0};
	V3 acc_s{0,0,0};
	for (size_t i = 0; i < inputs.size(); i++) {
		const DQInput& in = inputs[i];
		Q4 q = in.q;
		if (i == 0) q0_first = q;
		if (dot(q0_first, q) < 0.0) {
			q = scale(q, -1.0);
			out.antipode_flipped = true;
		}
		// Dual part qe = 0.5 * t * q with t treated as a pure quaternion.
		Q4 t_pure{in.t.x*0.5, in.t.y*0.5, in.t.z*0.5, 0.0};
		Q4 qe = quat_mul(t_pure, q);
		acc_q = add(acc_q, scale(q, in.weight));
		acc_e = add(acc_e, scale(qe, in.weight));
		acc_s = add(acc_s, mul(in.s, in.weight));
		out.total_weight += in.weight;
	}
	if (out.total_weight <= 0.0) {
		out.matrix = fallback;
		return out;
	}
	double inv = 1.0 / out.total_weight;
	acc_q = scale(acc_q, inv);
	acc_e = scale(acc_e, inv);
	acc_s = mul(acc_s, inv);

	double len2 = dot(acc_q, acc_q);
	double len = std::sqrt(len2);
	Q4 qn = scale(acc_q, 1.0/len);

	// Route (a): translation from the dual quaternion conjugate identity.
	Q4 tq = scale(quat_mul(acc_e, conjugate(acc_q)), 2.0 / len2);
	V3 ta{tq.x, tq.y, tq.z};

	// Route (b): scalar component form of the same identity.
	Q4 q0 = acc_q, qe = acc_e;
	double rcp2 = 2.0 / len2;
	V3 tb{
		rcp2*(-qe.w*q0.x + qe.x*q0.w - qe.y*q0.z + qe.z*q0.y),
		rcp2*(-qe.w*q0.y + qe.x*q0.z + qe.y*q0.w - qe.z*q0.x),
		rcp2*(-qe.w*q0.z - qe.x*q0.y + qe.y*q0.x + qe.z*q0.w)};
	// The two routes are algebraically identical; enforce it independently.
	if (length(sub(ta, tb)) > 1.0e-10)
		fail_fatal("internal DQ route mismatch");

	out.matrix = compose(ta, qn, acc_s);
	return out;
}

SkinResult dq_transform(const SkinData& skin, size_t vertex) {
	SkinResult res;
	res.total_weight = 0.0;
	res.antipode_flipped = false;
	const auto& infs = skin.influences[vertex];
	std::vector<DQInput> inputs;
	inputs.reserve(infs.size());
	for (const BoneInfluence& inf : infs) {
		TRS trs = decompose(skin.geometry_to_world[inf.cluster]);
		inputs.push_back(DQInput{trs.r, trs.t, trs.s, inf.weight});
		res.total_weight += inf.weight;
	}
	if (res.total_weight <= 0.0) {
		res.lbs = res.dq = skin.has_fallback ? skin.fallback : identity();
		return res;
	}
	DQCore core = dq_core(inputs, identity());
	res.antipode_flipped = core.antipode_flipped;
	res.dq = core.matrix;
	// LBS matrix for the same vertex.
	res.lbs = lbs_matrix(skin, vertex);
	return res;
}

// Full per-vertex skin matrix honoring the DQ/LBS blend weight.
M4 skin_vertex_matrix(const SkinData& skin, size_t vertex) {
	double total = 0.0;
	for (const BoneInfluence& inf : skin.influences[vertex]) total += inf.weight;
	if (total <= 0.0) return skin.has_fallback ? skin.fallback : identity();
	SkinResult dq = dq_transform(skin, vertex);
	double blend = skin.dq_weight[vertex];
	if (blend <= 0.0) return dq.lbs;
	if (blend >= 1.0) return dq.dq;
	M4 out;
	for (int i = 0; i < 12; i++)
		out.c[i] = (1.0-blend)*dq.lbs.c[i] + blend*dq.dq.c[i];
	return out;
}

// Independent in-between interpolation. An implicit zero key at target weight
// 0 anchors extrapolation outside the authored range (matching FBX semantics):
// weights below the first/above the last target are extrapolated against it,
// and negative target weights branch the search direction.
void effective_weights(const MorphChannel& ch, double weight,
	std::vector<double>& effective)
{
	effective.assign(ch.keys.size(), 0.0);
	if (ch.keys.empty()) return;
	const size_t nkeys = ch.keys.size();
	ssize_t last_negative = -1;
	for (size_t i = 0; i < nkeys; i++)
		if (ch.keys[i].target_weight < 0.0) last_negative = (ssize_t)i;

	ssize_t prev = -1, next = -1; // -1 stands for the implicit zero key
	if (weight > 0.0) {
		if (last_negative >= 0) prev = last_negative;
		for (size_t i = (size_t)(last_negative + 1); i < nkeys; i++) {
			prev = next;
			next = (ssize_t)i;
			if (ch.keys[i].target_weight > weight) break;
		}
	} else {
		if (last_negative + 1 < (ssize_t)nkeys) prev = last_negative + 1;
		for (ssize_t i = last_negative; i >= 0; i--) {
			prev = next;
			next = i;
			if (ch.keys[(size_t)i].target_weight < weight) break;
		}
	}
	double prev_target = prev >= 0 ? ch.keys[(size_t)prev].target_weight : 0.0;
	double next_target = next >= 0 ? ch.keys[(size_t)next].target_weight : 0.0;
	double delta = next_target - prev_target;
	if (delta != 0.0) {
		double t = (weight - prev_target) / delta;
		if (prev >= 0) effective[(size_t)prev] = 1.0 - t;
		if (next >= 0) effective[(size_t)next] = t;
	}
}

V3 morph_offset(const MorphChannel& ch, size_t vertex, double weight) {
	std::vector<double> eff;
	effective_weights(ch, weight, eff);
	V3 off{0,0,0};
	for (size_t k = 0; k < ch.keys.size(); k++) {
		if (eff[k] == 0.0) continue;
		for (const auto& entry : ch.keys[k].offsets) {
			if (entry.first == vertex) off = add(off, mul(entry.second, eff[k]));
		}
	}
	return off;
}

// ---------------------------------------------------------------------------
// Reporting helpers
// ---------------------------------------------------------------------------

int g_failures = 0;

void emit_capture(const char* kind, const char* id, uint32_t index,
	double time_or_weight, V3 expected, V3 actual)
{
	std::printf("MUTANT_CAPTURE kind=%s id=%s index=%u time/weight=%.9g "
		"expected=(%.9g,%.9g,%.9g) actual=(%.9g,%.9g,%.9g) diff=%.9g\n",
		kind, id, index, time_or_weight,
		expected.x, expected.y, expected.z,
		actual.x, actual.y, actual.z,
		length(sub(expected, actual)));
}

bool check_pos(const char* id, uint32_t vertex, double time_weight,
	V3 expected, V3 actual)
{
	if (!finite_v(actual)) {
		std::printf("FAIL non-finite position id=%s vertex=%u actual=(%.9g,%.9g,%.9g)\n",
			id, vertex, actual.x, actual.y, actual.z);
		emit_capture("position", id, vertex, time_weight, expected, actual);
		g_failures++;
		return false;
	}
	double d = length(sub(expected, actual));
	if (!(d <= POS_EPS)) {
		std::printf("FAIL position id=%s vertex=%u time/weight=%.9g diff=%.9g\n",
			id, vertex, time_weight, d);
		emit_capture("position", id, vertex, time_weight, expected, actual);
		g_failures++;
		return false;
	}
	return true;
}

bool check_normal(const char* id, uint32_t corner, double time_weight,
	V3 expected, V3 actual)
{
	if (!finite_v(actual) || !finite_v(expected)) {
		std::printf("FAIL non-finite normal id=%s corner=%u\n", id, corner);
		emit_capture("normal", id, corner, time_weight, expected, actual);
		g_failures++;
		return false;
	}
	double d = length(sub(expected, actual));
	if (!(d <= NORMAL_EPS)) {
		std::printf("FAIL normal id=%s corner=%u time/weight=%.9g diff=%.9g\n",
			id, corner, time_weight, d);
		emit_capture("normal", id, corner, time_weight, expected, actual);
		g_failures++;
	}
	return d <= NORMAL_EPS;
}

void fail_fatal(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	std::vprintf(fmt, ap);
	va_end(ap);
	std::printf("\n");
	std::exit(4);
}

// ---------------------------------------------------------------------------
// Part 1: hand-computed control cases, no ufbx involvement at all.
// ---------------------------------------------------------------------------

void control_case_equal_weights()
{
	// Two bones: Rz(+60), Rz(-60), zero translation, unit scale.
	// Point (1,0,0), equal weights.
	Q4 qp = quat_from_rot_z( 60.0*DEG2RAD);
	Q4 qn = quat_from_rot_z(-60.0*DEG2RAD);
	V3 p{1,0,0};

	// Build geometry-to-world matrices directly (bind offsets are identity).
	M4 gp = compose(V3{0,0,0}, qp, V3{1,1,1});
	M4 gn = compose(V3{0,0,0}, qn, V3{1,1,1});

	SkinData skin;
	skin.influences.resize(1);
	skin.dq_weight.push_back(0.0);
	skin.geometry_to_world.push_back(gp);
	skin.geometry_to_world.push_back(gn);
	skin.influences[0].push_back(BoneInfluence{0, 0.5});
	skin.influences[0].push_back(BoneInfluence{1, 0.5});
	skin.has_fallback = false;

	// LBS must give (0.5, 0, 0).
	M4 m_lbs = lbs_matrix(skin, 0);
	V3 r_lbs = transform(m_lbs, p);
	V3 e_lbs{0.5, 0.0, 0.0};
	if (length(sub(r_lbs, e_lbs)) > 1e-12)
		fail_fatal("control LBS broken: (%.12g,%.12g,%.12g)", r_lbs.x, r_lbs.y, r_lbs.z);

	// Rigid DQ must give (1, 0, 0).
	SkinResult dq = dq_transform(skin, 0);
	V3 r_dq = transform(dq.dq, p);
	V3 e_dq{1.0, 0.0, 0.0};
	if (length(sub(r_dq, e_dq)) > 1e-12)
		fail_fatal("control DQ broken: (%.12g,%.12g,%.12g)", r_dq.x, r_dq.y, r_dq.z);

	// dq_weight = 0.25 must give (0.625, 0, 0).
	skin.dq_weight[0] = 0.25;
	M4 m_mix = skin_vertex_matrix(skin, 0);
	V3 r_mix = transform(m_mix, p);
	V3 e_mix{0.625, 0.0, 0.0};
	if (length(sub(r_mix, e_mix)) > 1e-12)
		fail_fatal("control DQ/LBS blend broken: (%.12g,%.12g,%.12g)",
			r_mix.x, r_mix.y, r_mix.z);

	// The two raw bone quaternions must be well away from the 180 degree
	// shortest-arc ambiguity (|dot| = 0.5).
	double d = dot(qp, qn);
	if (std::fabs(std::fabs(d) - 0.5) > 1e-12 || std::fabs(d) < 0.25)
		fail_fatal("control quaternion dot product unexpected: %.12g", d);
}

void control_case_antipode_and_weight_scale()
{
	Q4 qp = quat_from_rot_z( 60.0*DEG2RAD);
	Q4 qn = quat_from_rot_z(-60.0*DEG2RAD);
	V3 p{1,0,0};
	V3 zero_t{0,0,0}, unit_s{1,1,1};

	// Baseline: equal rigid influences expressed directly as quaternions.
	std::vector<DQInput> inputs;
	inputs.push_back(DQInput{qp, zero_t, unit_s, 0.5});
	inputs.push_back(DQInput{qn, zero_t, unit_s, 0.5});
	V3 reference = transform(dq_core(inputs, identity()).matrix, p);

	// Negate one whole bone quaternion: the antipode correction must engage
	// and the physical result must not change.
	std::vector<DQInput> flipped = inputs;
	flipped[1].q = scale(qn, -1.0);
	DQCore flipped_core = dq_core(flipped, identity());
	if (!flipped_core.antipode_flipped)
		fail_fatal("antipode case did not engage the sign correction");
	V3 flipped_pos = transform(flipped_core.matrix, p);
	if (length(sub(flipped_pos, reference)) > 1e-12)
		fail_fatal("quaternion negation changed physical result");

	// Same with the first bone negated: the reference quaternion changes sign
	// too so no per-influence flip is needed, but the physical result must be
	// identical regardless (the 180 degree shortest-arc ambiguity itself is
	// not treated as a unique numerical answer elsewhere).
	std::vector<DQInput> flipped0 = inputs;
	flipped0[0].q = scale(qp, -1.0);
	DQCore flipped0_core = dq_core(flipped0, identity());
	if (length(sub(transform(flipped0_core.matrix, p), reference)) > 1e-12)
		fail_fatal("first-bone negation changed physical result");

	// Uniformly inflate every weight: normalization must hide it.
	for (DQInput& in : inputs) {
		in.weight *= 3.7;
		if (!(in.weight >= 0.0 && std::isfinite(in.weight)))
			fail_fatal("weights must stay non-negative and finite");
	}
	double total = 0.0;
	for (const DQInput& in : inputs) {
		if (!(in.weight >= 0.0 && std::isfinite(in.weight)))
			fail_fatal("weights must stay non-negative and finite");
		total += in.weight;
	}
	if (!(total > WEIGHT_SUM_FLOOR))
		fail_fatal("weight sum must stay well above the library threshold");
	V3 scaled_pos = transform(dq_core(inputs, identity()).matrix, p);
	if (length(sub(scaled_pos, reference)) > 1e-12)
		fail_fatal("uniform weight scaling changed physical result");
}

void control_case_unequal_weights_offsets_and_endpoints()
{
	// Non-zero bind offsets: G0 = T(0,2,0) * Rz(90), G1 = T(3,0,0).
	Q4 q0 = quat_from_rot_z(90.0*DEG2RAD);
	M4 g0 = mul(translation(V3{0,2,0}), compose(V3{0,0,0}, q0, V3{1,1,1}));
	M4 g1 = translation(V3{3,0,0});
	V3 p{1,0,0};

	// Hand-computed transformed points.
	V3 g0p = transform(g0, p); // (0,3,0)
	V3 g1p = transform(g1, p); // (4,0,0)
	if (length(sub(g0p, V3{0,3,0})) > 1e-12 || length(sub(g1p, V3{4,0,0})) > 1e-12)
		fail_fatal("bind offset control matrices are wrong");

	SkinData skin;
	skin.influences.resize(2);
	skin.dq_weight.assign(2, 0.0);
	skin.geometry_to_world.push_back(g0);
	skin.geometry_to_world.push_back(g1);
	skin.influences[0].push_back(BoneInfluence{0, 0.75});
	skin.influences[0].push_back(BoneInfluence{1, 0.25});
	// Second vertex has no influences at all: it must keep its position.
	skin.has_fallback = false;

	// Weight sanity contract.
	double total = 0.0;
	for (const BoneInfluence& inf : skin.influences[0]) {
		if (!(inf.weight >= 0.0 && std::isfinite(inf.weight)))
			fail_fatal("unequal weights must be non-negative and finite");
		total += inf.weight;
	}
	if (!(total > WEIGHT_SUM_FLOOR)) fail_fatal("weight sum too small");

	// LBS hand value: 0.75*(0,3,0) + 0.25*(4,0,0) = (1, 2.25, 0).
	V3 lbs_p = transform(lbs_matrix(skin, 0), p);
	if (length(sub(lbs_p, V3{1, 2.25, 0})) > 1e-12)
		fail_fatal("unequal LBS hand value broken");

	// Unweighted vertex stays fixed under every blend weight.
	for (size_t v = 0; v < 2; v++) {
		double blends[] = {0.0, 0.25, 0.75, 1.0};
		for (double b : blends) {
			skin.dq_weight[v] = b;
			M4 m = skin_vertex_matrix(skin, v);
			V3 got = transform(m, p);
			if (v == 1 && length(sub(got, p)) > 1e-12)
				fail_fatal("unweighted vertex moved at dq_weight=%g", b);
		}
	}

	// For the weighted vertex every interior blend weight must equal the exact
	// convex combination of its LBS and rigid DQ points.
	SkinResult dq = dq_transform(skin, 0);
	V3 dq_p = transform(dq.dq, p);
	V3 lbs_point = lbs_p;
	double blends[] = {0.0, 0.25, 0.75, 1.0};
	for (double b : blends) {
		skin.dq_weight[0] = b;
		V3 got = transform(skin_vertex_matrix(skin, 0), p);
		V3 expect = add(mul(lbs_point, 1.0-b), mul(dq_p, b));
		if (length(sub(got, expect)) > 1e-10)
			fail_fatal("interior blend weight %g is inconsistent", b);
	}

	// Positive uniform scale must survive the DQ decomposition path.
	M4 scaled = mul(translation(V3{1,-2,0.5}),
		mul(compose(V3{0,0,0}, quat_from_rot_z(35.0*DEG2RAD), V3{2,2,2}),
			identity()));
	TRS trs = decompose(scaled);
	if (trs.ortho_residual > 1e-10)
		fail_fatal("uniform scale decomposition not rigid");
	if (length(sub(trs.s, V3{2,2,2})) > 1e-10)
		fail_fatal("positive scale lost in decomposition");
	M4 rebuilt = compose(trs.t, trs.r, trs.s);
	V3 dpt = sub(transform(rebuilt, V3{0.7,-0.3,1.2}),
	             transform(scaled,  V3{0.7,-0.3,1.2}));
	if (length(dpt) > 1e-10)
		fail_fatal("scaled rigid transform does not round-trip (residual %.3g, diff %.6g)",
			trs.ortho_residual, length(dpt));
}

void run_control_cases()
{
	control_case_equal_weights();
	control_case_antipode_and_weight_scale();
	control_case_unequal_weights_offsets_and_endpoints();
	std::printf("UFBX_DQ_REFERENCE_OK\n");
}

// ---------------------------------------------------------------------------
// Part 2: tiny hand-authored ASCII FBX with non-commutative bone transforms,
// a sparse in-between morph channel and one hard normal edge.
// ---------------------------------------------------------------------------

// Vertex layout (identity is asserted by the tests below):
//   v0 (-1,0,0)  v1 (1,0,0)  v2 (-1,1,0)  v3 (1,1,0)
//   v4 (-1,2,0)  v5 (1,2,0)
// Triangles: (0,1,3) (0,3,2) (2,3,5) (2,5,4), winding gives +Z normals.
// Hard edge between triangles 0 and 1 (both marked flat).

const uint32_t MINI_VERTICES = 6;
const uint32_t MINI_FACES = 4;
const uint32_t MINI_CORNERS = 12;
const double MINI_STATIC_WEIGHT = 0.75; // static DeformPercent 75
const double MINI_SAMPLE_WEIGHTS[] = {0.25, 0.75};

void write_mini_fbx(const std::string& path)
{
	std::ofstream f(path.c_str());
	if (!f) fail_fatal("cannot write %s", path.c_str());
	f << "; FBX 7.5.0 generated by checks/deformation_reference.cpp\n";
	f << "FBXHeaderExtension:  {\n";
	f << "\tFBXHeaderVersion: 1003\n\tFBXVersion: 7500\n";
	f << "\tSceneInfo: \"SceneInfo::GlobalInfo\", \"UserData\" {\n";
	f << "\t\tType: \"UserData\"\n\t\tVersion: 100\n\t}\n}\n";
	f << "GlobalSettings:  {\n\tVersion: 1000\n\tProperties70:  {\n";
	f << "\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",1\n";
	f << "\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n";
	f << "\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n";
	f << "\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n";
	f << "\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n";
	f << "\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n";
	f << "\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n";
	f << "\t\tP: \"TimeMode\", \"enum\", \"\", \"\",11\n\t}\n}\n";
	f << "Documents:  {\n\tCount: 1\n";
	f << "\tDocument: 1000, \"\", \"Scene\" {\n\t\tRootNode: 0\n\t}\n}\n";
	f << "References:  {\n}\n";
	f << "Definitions:  {\n\tVersion: 100\n\tCount: 11\n";
	const char* types[] = {
		"GlobalSettings", "AnimationStack", "AnimationLayer", "AnimationCurveNode",
		"AnimationCurve", "Geometry", "Model", "NodeAttribute", "Material",
		"Deformer", "Pose"};
	for (size_t i = 0; i < sizeof(types)/sizeof(types[0]); i++)
		f << "\tObjectType: \"" << types[i] << "\" {\n\t\tCount: 1\n\t}\n";
	f << "}\n";
	f << "Objects:  {\n";

	// Mesh geometry
	f << "\tGeometry: 2000, \"Geometry::MiniMesh\", \"Mesh\" {\n";
	f << "\t\tVertices: *18 {\n";
	f << "\t\t\ta: -1,0,0, 1,0,0, -1,1,0, 1,1,0, -1,2,0, 1,2,0\n";
	f << "\t\t} \n";
	f << "\t\tPolygonVertexIndex: *12 {\n";
	f << "\t\t\ta: 0,1,-4, 0,3,-3, 2,3,-6, 2,5,-5\n"; // 0,1,3 0,3,2 2,3,5 2,5,4
	f << "\t\t} \n";
	f << "\t\tGeometryVersion: 124\n";
	// Both sides of the v2-v3 edge are hard; remaining faces smooth.
	f << "\t\tLayerElementSmoothing: 0 {\n";
	f << "\t\t\tVersion: 102\n\t\t\tName: \"\"\n";
	f << "\t\t\tMappingInformationType: \"ByPolygon\"\n";
	f << "\t\t\tReferenceInformationType: \"Direct\"\n";
	f << "\t\t\tSmoothing: 0,0,1,1\n";
	f << "\t\t}\n";
	f << "\t\tLayer: 0 {\n";
	f << "\t\t\tVersion: 100\n";
	f << "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementSmoothing\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n";
	f << "\t\t}\n";
	f << "\t}\n";

	// In-between shape at target weight 0.5: sparse offsets on v1 and v3.
	f << "\tGeometry: 2001, \"Geometry::MorphHalf\", \"Shape\" {\n";
	f << "\t\tVersion: 100\n";
	f << "\t\tIndexes: *2 {\n\t\t\ta: 1,3\n\t\t} \n";
	f << "\t\tVertices: *6 {\n";
	f << "\t\t\ta: 0,0.1,0, 0,-0.1,0.2\n";
	f << "\t\t} \n";
	f << "\t\tNormals: *6 {\n\t\t\ta: 0,0,0, 0,0,0\n\t\t} \n";
	f << "\t}\n";
	// Full shape at target weight 1.0: sparse offsets on v1 and v4.
	f << "\tGeometry: 2002, \"Geometry::MorphFull\", \"Shape\" {\n";
	f << "\t\tVersion: 100\n";
	f << "\t\tIndexes: *2 {\n\t\t\ta: 1,4\n\t\t} \n";
	f << "\t\tVertices: *6 {\n";
	f << "\t\t\ta: 0,0.4,0, 0.3,0,0\n";
	f << "\t\t} \n";
	f << "\t\tNormals: *6 {\n\t\t\ta: 0,0,0, 0,0,0\n\t\t} \n";
	f << "\t}\n";

	// Mesh model, identity transform.
	f << "\tModel: 3000, \"Model::MiniMesh\", \"Mesh\" {\n";
	f << "\t\tVersion: 232\n\t\tProperties70:  {\n";
	f << "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,0,0\n";
	f << "\t\t\tP: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\",0,0,0\n";
	f << "\t\t\tP: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",1,1,1\n";
	f << "\t\t}\n\t\tShading: T\n\t\tCulling: \"CullingOff\"\n\t}\n";

	// Bones. Non-commutative rotation+translation pairs, rotations chosen so
	// the quaternion dot product between the two bone frames is negative
	// (Rz(120) vs Rz(-120): dot = cos(120 deg) = -0.5) while staying away
	// from the exact 180 degree shortest-arc singularity.
	f << "\tNodeAttribute: 4001, \"NodeAttribute::BoneA\", \"LimbNode\" {\n";
	f << "\t\tTypeFlags: \"Skeleton\"\n\t}\n";
	f << "\tNodeAttribute: 4002, \"NodeAttribute::BoneB\", \"LimbNode\" {\n";
	f << "\t\tTypeFlags: \"Skeleton\"\n\t}\n";
	f << "\tModel: 3001, \"Model::BoneA\", \"LimbNode\" {\n";
	f << "\t\tVersion: 232\n\t\tProperties70:  {\n";
	f << "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0.5,-0.5,0.25\n";
	f << "\t\t\tP: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\",0,0,120\n";
	f << "\t\t\tP: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",1,1,1\n";
	f << "\t\t}\n\t\tShading: Y\n\t\tCulling: \"CullingOff\"\n\t}\n";
	f << "\tModel: 3002, \"Model::BoneB\", \"LimbNode\" {\n";
	f << "\t\tVersion: 232\n\t\tProperties70:  {\n";
	f << "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",-0.5,0.75,-0.25\n";
	f << "\t\t\tP: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\",0,0,-120\n";
	f << "\t\t\tP: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",1,1,1\n";
	f << "\t\t}\n\t\tShading: Y\n\t\tCulling: \"CullingOff\"\n\t}\n";

	// Bind pose: both bones and the mesh at identity.
	f << "\tPose: 5000, \"Pose::BindPose\", \"BindPose\" {\n";
	f << "\t\tType: \"BindPose\"\n\t\tVersion: 100\n\t\tNbPoseNodes: 3\n";
	uint32_t pose_nodes[] = {3000u, 3001u, 3002u};
	for (size_t i = 0; i < 3; i++) {
		f << "\t\tPoseNode:  {\n\t\t\tNode: " << pose_nodes[i] << "\n";
		f << "\t\t\tMatrix: *16 {\n";
		f << "\t\t\t\ta: 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1\n";
		f << "\t\t\t} \n\t\t}\n";
	}
	f << "\t}\n";

	// Skin deformer: blended DQ/LBS with per-vertex blend weights.
	f << "\tDeformer: 6000, \"Deformer::Skin\", \"Skin\" {\n";
	f << "\t\tVersion: 101\n\t\tLink_DeformAcuracy: 50\n";
	f << "\t\tSkinningType: \"Blend\"\n";
	f << "\t\tIndexes: *6 {\n\t\t\ta: 0,1,2,3,4,5\n\t\t} \n";
	f << "\t\tBlendWeights: *6 {\n\t\t\ta: 0,0.25,0.75,1,0.5,0\n\t\t} \n";
	f << "\t}\n";
	// Cluster A: weights for v0..v4, v5 is left unweighted.
	f << "\tDeformer: 6001, \"SubDeformer::ClusterA\", \"Cluster\" {\n";
	f << "\t\tVersion: 100\n";
	f << "\t\tIndexes: *5 {\n\t\t\ta: 0,1,2,3,4\n\t\t} \n";
	f << "\t\tWeights: *5 {\n\t\t\ta: 0.8,0.6,0.3,0.1,0.4\n\t\t} \n";
	f << "\t\tTransform: *16 {\n";
	f << "\t\t\ta: 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1\n\t\t} \n";
	f << "\t\tTransformLink: *16 {\n";
	f << "\t\t\ta: 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1\n\t\t} \n";
	f << "\t}\n";
	f << "\tDeformer: 6002, \"SubDeformer::ClusterB\", \"Cluster\" {\n";
	f << "\t\tVersion: 100\n";
	f << "\t\tIndexes: *5 {\n\t\t\ta: 0,1,2,3,4\n\t\t} \n";
	f << "\t\tWeights: *5 {\n\t\t\ta: 0.2,0.4,0.7,0.9,0.6\n\t\t} \n";
	f << "\t\tTransform: *16 {\n";
	f << "\t\t\ta: 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1\n\t\t} \n";
	f << "\t\tTransformLink: *16 {\n";
	f << "\t\t\ta: 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1\n\t\t} \n";
	f << "\t}\n";

	// Blend deformer with one channel and an in-between.
	f << "\tDeformer: 6100, \"Deformer::Blend\", \"BlendShape\" {\n";
	f << "\t\tVersion: 100\n\t}\n";
	f << "\tDeformer: 6101, \"SubDeformer::MorphChannel\", \"BlendShapeChannel\" {\n";
	f << "\t\tVersion: 100\n\t\tProperties70:  {\n";
	f << "\t\t\tP: \"DeformPercent\", \"Number\", \"\", \"A+\",75\n\t\t}\n";
	f << "\t\tDeformPercent: 75\n";
	f << "\t\tFullWeights: *2 {\n\t\t\ta: 50,100\n\t\t} \n";
	f << "\t}\n";

	// Animation: DeformPercent 25 at t=0s, 75 at t=1s.
	f << "\tAnimationStack: 7000, \"AnimStack::Take 001\", \"\" {\n";
	f << "\t\tProperties70:  {\n";
	f << "\t\t\tP: \"LocalStart\", \"KTime\", \"Time\", \"\",0\n";
	f << "\t\t\tP: \"LocalStop\", \"KTime\", \"Time\", \"\",46186158000\n";
	f << "\t\t}\n\t}\n";
	f << "\tAnimationLayer: 7001, \"AnimLayer::BaseLayer\", \"\" {\n\t}\n";
	f << "\tAnimationCurveNode: 7002, \"AnimCurveNode::DeformPercent\", \"\" {\n";
	f << "\t\tProperties70:  {\n";
	f << "\t\t\tP: \"d|DeformPercent\", \"Number\", \"\", \"A\",75\n\t\t}\n\t}\n";
	f << "\tAnimationCurve: 7003, \"AnimCurve::Morph\", \"\" {\n";
	f << "\t\tDefault: 0\n\t\tKeyVer: 4009\n";
	f << "\t\tKeyTime: *2 {\n\t\t\ta: 0,46186158000\n\t\t} \n";
	f << "\t\tKeyValueFloat: *2 {\n\t\t\ta: 25,75\n\t\t} \n";
	f << "\t\tKeyAttrFlags: *2 {\n\t\t\ta: 24840,24840\n\t\t} \n";
	f << "\t\tKeyAttrDataFloat: *8 {\n";
	f << "\t\t\ta: 0,0,0,0, 0,0,0,0\n\t\t} \n";
	f << "\t\tKeyAttrRefCount: *2 {\n\t\t\ta: 1,1\n\t\t} \n";
	f << "\t}\n";

	f << "}\n"; // Objects

	f << "Connections:  {\n";
	f << "\tC: \"OO\",3000,0\n";
	f << "\tC: \"OO\",3001,0\n";
	f << "\tC: \"OO\",3002,0\n";
	f << "\tC: \"OO\",4001,3001\n";
	f << "\tC: \"OO\",4002,3002\n";
	f << "\tC: \"OO\",2000,3000\n";
	f << "\tC: \"OO\",6000,2000\n";
	f << "\tC: \"OO\",6001,6000\n";
	f << "\tC: \"OO\",6002,6000\n";
	f << "\tC: \"OO\",3001,6001\n";
	f << "\tC: \"OO\",3002,6002\n";
	f << "\tC: \"OO\",6100,2000\n";
	f << "\tC: \"OO\",6101,6100\n";
	f << "\tC: \"OO\",2001,6101\n";
	f << "\tC: \"OO\",2002,6101\n";
	f << "\tC: \"OO\",7001,7000\n";
	f << "\tC: \"OO\",7002,7001\n";
	f << "\tC: \"OP\",7002,6101, \"DeformPercent\"\n";
	f << "\tC: \"OP\",7003,7002, \"d|DeformPercent\"\n";
	f << "}\n";
	f << "Takes:  {\n\tCurrent: \"Take 001\"\n";
	f << "\tTake: \"Take 001\" {\n\t\tLocalTime: 0,46186158000\n\t\tReferenceTime: 0,46186158000\n\t}\n";
	f << "}\n";
	f.close();
}

// ---------------------------------------------------------------------------
// Loading helpers: translate ufbx raw data into our own structures.
// ---------------------------------------------------------------------------

SkinData extract_skin(const ufbx_mesh* mesh)
{
	SkinData skin;
	skin.influences.assign(mesh->num_vertices, std::vector<BoneInfluence>());
	skin.dq_weight.assign(mesh->num_vertices, 0.0);
	skin.has_fallback = false;
	if (mesh->skin_deformers.count == 0) return skin;

	const ufbx_skin_deformer* sd = mesh->skin_deformers.data[0];
	skin.geometry_to_world.resize(sd->clusters.count);
	for (size_t c = 0; c < sd->clusters.count; c++) {
		// Raw bind/pose matrix produced by the loaded scene; all subsequent
		// math happens in our own implementation.
		skin.geometry_to_world[c] = from_ufbx(sd->clusters.data[c]->geometry_to_world);
	}
	for (size_t v = 0; v < sd->vertices.count; v++) {
		ufbx_skin_vertex sv = sd->vertices.data[v];
		skin.dq_weight[v] = (double)sv.dq_weight;
		for (uint32_t i = 0; i < sv.num_weights; i++) {
			ufbx_skin_weight w = sd->weights.data[sv.weight_begin + i];
			double wv = (double)w.weight;
			if (wv < 0.0 || !std::isfinite(wv))
				fail_fatal("library exposed invalid skin weight at vertex %zu", v);
			skin.influences[v].push_back(BoneInfluence{(size_t)w.cluster_index, wv});
		}
	}
	if (mesh->instances.count > 0) {
		skin.fallback = from_ufbx(mesh->instances.data[0]->geometry_to_world);
		skin.has_fallback = true;
	}
	return skin;
}

std::vector<MorphChannel> extract_morphs(const ufbx_mesh* mesh)
{
	std::vector<MorphChannel> channels;
	for (size_t d = 0; d < mesh->blend_deformers.count; d++) {
		const ufbx_blend_deformer* bd = mesh->blend_deformers.data[d];
		for (size_t ci = 0; ci < bd->channels.count; ci++) {
			const ufbx_blend_channel* ch = bd->channels.data[ci];
			MorphChannel mc;
			for (size_t k = 0; k < ch->keyframes.count; k++) {
				const ufbx_blend_keyframe& kf = ch->keyframes.data[k];
				MorphKey key;
				key.target_weight = (double)kf.target_weight;
				const ufbx_blend_shape* shape = kf.shape;
				for (size_t o = 0; o < shape->num_offsets; o++) {
					key.offsets.push_back(std::make_pair(
						shape->offset_vertices.data[o],
						from_ufbx(shape->position_offsets.data[o])));
				}
				mc.keys.push_back(key);
			}
			// Sort by target weight so interval lookup is well defined.
			for (size_t i = 1; i < mc.keys.size(); i++)
				for (size_t j = i; j > 0 && mc.keys[j-1].target_weight > mc.keys[j].target_weight; j--)
					std::swap(mc.keys[j-1], mc.keys[j]);
			channels.push_back(mc);
		}
	}
	return channels;
}

std::vector<double> channel_weights(const ufbx_mesh* mesh)
{
	std::vector<double> weights;
	for (size_t d = 0; d < mesh->blend_deformers.count; d++) {
		const ufbx_blend_deformer* bd = mesh->blend_deformers.data[d];
		for (size_t ci = 0; ci < bd->channels.count; ci++)
			weights.push_back((double)bd->channels.data[ci]->weight);
	}
	return weights;
}

// Independent expected deformed position for one vertex.
V3 expected_position(const ufbx_mesh* mesh, const SkinData& skin,
	const std::vector<MorphChannel>& channels,
	const std::vector<double>& morph_weights, size_t v)
{
	V3 p = from_ufbx(mesh->vertices.data[v]);
	for (size_t c = 0; c < channels.size(); c++)
		p = add(p, morph_offset(channels[c], v, morph_weights[c]));
	M4 m = skin_vertex_matrix(skin, v);
	return transform(m, p);
}

// ---------------------------------------------------------------------------
// Independent normal generation over explicit smoothing groups.
// ---------------------------------------------------------------------------

struct NormalGroups {
	std::vector<uint32_t> corner_group; // group id per face corner
	std::vector<V3> group_normal;       // normalized area-weighted normal
};

struct DSU {
	std::vector<uint32_t> parent;
	explicit DSU(uint32_t n) : parent(n) { for (uint32_t i=0;i<n;i++) parent[i]=i; }
	uint32_t find(uint32_t x) { while (parent[x]!=x) { parent[x]=parent[parent[x]]; x=parent[x]; } return x; }
	void unite(uint32_t a, uint32_t b) { parent[find(a)] = find(b); }
};

NormalGroups compute_normals(const ufbx_mesh* mesh, const std::vector<V3>& positions)
{
	const uint32_t corners = (uint32_t)mesh->num_indices;
	DSU dsu(corners);

	// Map each (vertex, vertex) edge to the two corners touching it.
	// A corner entry represents the half-edge ending at its vertex; the
	// previous half-edge in the same face starts there, so pair corners whose
	// start vertices coincide across faces.
	struct HalfEdge {
		uint32_t from, to;       // directed half-edge endpoints
		uint32_t end_corner;    // face corner at `to`
		uint32_t start_corner;  // face corner at `from` (previous in face)
		uint32_t face;
	};
	std::vector<HalfEdge> edges;
	edges.reserve(corners);
	for (size_t f = 0; f < mesh->num_faces; f++) {
		ufbx_face face = mesh->faces.data[f];
		for (uint32_t k = 0; k < face.num_indices; k++) {
			uint32_t cur_corner = face.index_begin + k;
			uint32_t prev_corner = face.index_begin + (k + face.num_indices - 1) % face.num_indices;
			uint32_t from = mesh->vertex_indices.data[prev_corner];
			uint32_t to = mesh->vertex_indices.data[cur_corner];
			edges.push_back(HalfEdge{from, to, cur_corner, prev_corner, (uint32_t)f});
		}
	}
	// Pair the two opposing half-edges of every manifold edge. The corners at
	// each shared endpoint are merged into one group only when the edge is
	// smooth; a hard edge keeps the two sides as distinct normal groups.
	for (size_t i = 0; i < edges.size(); i++) {
		for (size_t j = i+1; j < edges.size(); j++) {
			if (edges[i].from != edges[j].to || edges[i].to != edges[j].from) continue;
			bool smooth = false;
			if (mesh->edge_smoothing.count) {
				// Explicit per-edge smoothing wins when present.
				uint32_t edge_id = UINT32_MAX;
				for (uint32_t e = 0; e < mesh->num_edges; e++) {
					ufbx_edge ed = mesh->edges.data[e];
					if ((ed.indices[0] == edges[i].from && ed.indices[1] == edges[i].to) ||
					    (ed.indices[0] == edges[i].to && ed.indices[1] == edges[i].from)) {
						edge_id = e;
						break;
					}
				}
				if (edge_id != UINT32_MAX) smooth = mesh->edge_smoothing.data[edge_id];
			}
			if (mesh->face_smoothing.count) {
				// Match the library rule: smooth if either adjacent face is
				// smooth, hard only when both sides are flat.
				smooth = mesh->face_smoothing.data[edges[i].face] ||
				         mesh->face_smoothing.data[edges[j].face];
			}
			if (!smooth) continue;
			// Corner at vertex `to`: end corner of i, start corner of j.
			dsu.unite(edges[i].end_corner, edges[j].start_corner);
			// Corner at vertex `from`: start corner of i, end corner of j.
			dsu.unite(edges[i].start_corner, edges[j].end_corner);
		}
	}

	std::vector<uint32_t> remap(corners, UINT32_MAX);
	NormalGroups ng;
	ng.corner_group.assign(corners, UINT32_MAX);
	uint32_t group_count = 0;
	for (uint32_t c = 0; c < corners; c++) {
		uint32_t root = dsu.find(c);
		if (remap[root] == UINT32_MAX) remap[root] = group_count++;
		ng.corner_group[c] = remap[root];
	}
	std::vector<V3> accum(group_count, V3{0,0,0});
	for (size_t f = 0; f < mesh->num_faces; f++) {
		ufbx_face face = mesh->faces.data[f];
		if (face.num_indices < 3) continue;
		V3 a = positions[mesh->vertex_indices.data[face.index_begin+0]];
		V3 b = positions[mesh->vertex_indices.data[face.index_begin+1]];
		V3 c = positions[mesh->vertex_indices.data[face.index_begin+2]];
		V3 fn = cross(sub(b, a), sub(c, a)); // triangle area-weighted normal
		for (uint32_t k = 0; k < face.num_indices; k++) {
			uint32_t corner = face.index_begin + k;
			accum[ng.corner_group[corner]] = add(accum[ng.corner_group[corner]], fn);
		}
	}
	ng.group_normal.resize(group_count);
	for (uint32_t g = 0; g < group_count; g++)
		ng.group_normal[g] = normalize(accum[g]);
	return ng;
}

// ---------------------------------------------------------------------------
// OBJ export of the deformed fixture
// ---------------------------------------------------------------------------

void export_obj(const std::string& path, const ufbx_mesh* mesh,
	const std::vector<V3>& positions, const NormalGroups& ng)
{
	std::ofstream f(path.c_str());
	if (!f) fail_fatal("cannot write %s", path.c_str());
	f << "# deformation.obj generated by checks/deformation_reference.cpp\n";
	for (size_t v = 0; v < mesh->num_vertices; v++)
		f << "v " << positions[v].x << ' ' << positions[v].y << ' ' << positions[v].z << '\n';
	for (const V3& n : ng.group_normal)
		f << "vn " << n.x << ' ' << n.y << ' ' << n.z << '\n';
	for (size_t face = 0; face < mesh->num_faces; face++) {
		ufbx_face fc = mesh->faces.data[face];
		f << "f";
		for (uint32_t k = 0; k < fc.num_indices; k++) {
			uint32_t corner = fc.index_begin + k;
			uint32_t vert = mesh->vertex_indices.data[corner] + 1;
			uint32_t norm = ng.corner_group[corner] + 1;
			f << ' ' << vert << "//" << norm;
		}
		f << '\n';
	}
}

// ---------------------------------------------------------------------------
// Fixture validation
// ---------------------------------------------------------------------------

ufbx_scene* load_scene(const char* path, bool skin, ufbx_error* error)
{
	ufbx_load_opts opts;
	std::memset(&opts, 0, sizeof(opts));
	opts.evaluate_skinning = skin;
	ufbx_scene* s = ufbx_load_file(path, &opts, error);
	if (!s) {
		std::printf("LOAD_FAILED %s: %s\n", path, error->description.data);
		std::exit(3);
	}
	return s;
}

void validate_fixture_shape(ufbx_scene* scene)
{
	ufbx_node* node = ufbx_find_node(scene, "MiniMesh");
	if (!node || !node->mesh) fail_fatal("fixture mesh node missing");
	ufbx_mesh* mesh = node->mesh;
	if (mesh->num_vertices != MINI_VERTICES)
		fail_fatal("fixture vertex count: got %zu expected %u",
			mesh->num_vertices, MINI_VERTICES);
	if (mesh->num_faces != MINI_FACES)
		fail_fatal("fixture face count: got %zu expected %u", mesh->num_faces, MINI_FACES);
	if (mesh->num_indices != MINI_CORNERS)
		fail_fatal("fixture corner count: got %zu expected %u",
			mesh->num_indices, MINI_CORNERS);
	if (mesh->skin_deformers.count != 1) fail_fatal("fixture skin deformer missing");
	if (mesh->blend_deformers.count != 1) fail_fatal("fixture blend deformer missing");
	ufbx_skin_deformer* skin = mesh->skin_deformers.data[0];
	if (skin->skinning_method != UFBX_SKINNING_METHOD_BLENDED_DQ_LINEAR)
		fail_fatal("fixture skinning method not blended DQ/LBS");
	ufbx_blend_deformer* bd = mesh->blend_deformers.data[0];
	if (bd->channels.count != 1 || bd->channels.data[0]->keyframes.count != 2)
		fail_fatal("fixture morph must have one channel with two keyframes");
	ufbx_blend_channel* ch = bd->channels.data[0];
	if (std::fabs((double)ch->keyframes.data[0].target_weight - 0.5) > 1e-9 ||
	    std::fabs((double)ch->keyframes.data[1].target_weight - 1.0) > 1e-9)
		fail_fatal("fixture in-between targets corrupted");
	if (mesh->face_smoothing.count != MINI_FACES)
		fail_fatal("fixture face smoothing missing");
}

// Verify the independently constructed bone matrices match the library's
// loaded node-to-world matrices (sanity check of scene authoring only; the
// skinning math stays ours).
void validate_fixture_bone_matrices(ufbx_scene* scene)
{
	ufbx_node* a = ufbx_find_node(scene, "BoneA");
	ufbx_node* b = ufbx_find_node(scene, "BoneB");
	if (!a || !b) fail_fatal("fixture bones missing");
	M4 expect_a = mul(translation(V3{0.5,-0.5,0.25}),
		compose(V3{0,0,0}, quat_from_rot_z(120.0*DEG2RAD), V3{1,1,1}));
	M4 expect_b = mul(translation(V3{-0.5,0.75,-0.25}),
		compose(V3{0,0,0}, quat_from_rot_z(-120.0*DEG2RAD), V3{1,1,1}));
	M4 got_a = from_ufbx(a->node_to_world);
	M4 got_b = from_ufbx(b->node_to_world);
	for (int i = 0; i < 12; i++) {
		if (std::fabs(expect_a.c[i]-got_a.c[i]) > 1e-6)
			fail_fatal("BoneA node_to_world mismatch at %d", i);
		if (std::fabs(expect_b.c[i]-got_b.c[i]) > 1e-6)
			fail_fatal("BoneB node_to_world mismatch at %d", i);
	}
	// The two bone rotations must have a quaternion dot well away from zero.
	Q4 qa = quat_from_rot_z(120.0*DEG2RAD);
	Q4 qb = quat_from_rot_z(-120.0*DEG2RAD);
	if (std::fabs(dot(qa, qb)) < 0.25 || std::fabs(std::fabs(dot(qa, qb))-0.5) > 1e-12)
		fail_fatal("fixture bone quaternion dot not safely antipodal");
}

void compare_scene_positions(const char* id, ufbx_scene* scene, double time_weight)
{
	for (size_t mi = 0; mi < scene->meshes.count; mi++) {
		ufbx_mesh* mesh = scene->meshes.data[mi];
		if (!mesh->skin_deformers.count && !mesh->blend_deformers.count) continue;
		SkinData skin = extract_skin(mesh);
		std::vector<MorphChannel> channels = extract_morphs(mesh);
		std::vector<double> weights = channel_weights(mesh);
		if (weights.size() != channels.size())
			fail_fatal("morph channel/weight count mismatch");
		for (size_t v = 0; v < mesh->num_vertices; v++) {
			V3 expect = expected_position(mesh, skin, channels, weights, v);
			ufbx_vec3 actual = mesh->skinned_position.values.data[v];
			check_pos(id, (uint32_t)v, time_weight, expect, from_ufbx(actual));
		}
	}
}

void run_fixture(const std::string& fbx_path, const std::string& obj_path)
{
	// ---- Static load path (morph weight fixed at 0.75) ----
	ufbx_error error;
	ufbx_scene* static_scene = load_scene(fbx_path.c_str(), true, &error);
	validate_fixture_shape(static_scene);
	validate_fixture_bone_matrices(static_scene);
	ufbx_mesh* mesh = ufbx_find_node(static_scene, "MiniMesh")->mesh;

	SkinData skin = extract_skin(mesh);
	std::vector<MorphChannel> channels = extract_morphs(mesh);

	// Fixture contract: weights non-negative, finite and with healthy sums.
	for (size_t v = 0; v < mesh->num_vertices; v++) {
		double sum = 0.0;
		for (const BoneInfluence& inf : skin.influences[v]) {
			if (!(inf.weight >= 0.0 && std::isfinite(inf.weight)))
				fail_fatal("fixture weight invalid at vertex %zu", v);
			sum += inf.weight;
		}
		if (v < 5 && sum <= WEIGHT_SUM_FLOOR)
			fail_fatal("fixture weighted vertex %zu has tiny sum", v);
	}
	// v5 must be the deliberately unweighted vertex.
	if (!skin.influences[5].empty()) fail_fatal("v5 should be unweighted");
	// All four interior/endpoint blend weights must be present across vertices.
	int seen_blend = 0;
	double wanted[] = {0.0, 0.25, 0.75, 1.0};
	for (double w : wanted) {
		for (size_t v = 0; v < skin.dq_weight.size(); v++)
			if (std::fabs(skin.dq_weight[v]-w) < 1e-9) { seen_blend++; break; }
	}
	if (seen_blend != 4) fail_fatal("fixture does not cover all dq blend weights");

	std::vector<double> static_weights = channel_weights(mesh);
	if (static_weights.size() != 1 ||
	    std::fabs(static_weights[0] - MINI_STATIC_WEIGHT) > 1e-9)
		fail_fatal("static morph weight mismatch");
	double static_w = static_weights[0];

	compare_scene_positions("mini-static", static_scene, static_w);

	// ---- Wrong implementation variants must produce concrete errors ----
	{
		// Variant A: add morph offsets in world space *after* skinning.
		size_t bad_vertices = 0;
		for (size_t v = 0; v < mesh->num_vertices; v++) {
			V3 correct = expected_position(mesh, skin, channels, static_weights, v);
			V3 base = from_ufbx(mesh->vertices.data[v]);
			V3 skinned = transform(skin_vertex_matrix(skin, v), base);
			V3 wrong = add(skinned, morph_offset(channels[0], v, static_w));
			if (length(sub(wrong, correct)) > 1e-9) bad_vertices++;
		}
		if (bad_vertices < 2)
			fail_fatal("world-space morph variant must differ on morphed vertices");

		// Variant B: ignore the in-between and drive the full shape directly.
		size_t bad_inbetween = 0;
		MorphChannel full_only;
		full_only.keys.push_back(channels[0].keys.back()); // target 1.0 only
		for (size_t v = 0; v < mesh->num_vertices; v++) {
			V3 correct = expected_position(mesh, skin, channels, static_weights, v);
			V3 p = add(from_ufbx(mesh->vertices.data[v]),
				morph_offset(full_only, v, static_w));
			V3 wrong = transform(skin_vertex_matrix(skin, v), p);
			if (length(sub(wrong, correct)) > 1e-9) bad_inbetween++;
		}
		if (bad_inbetween < 2)
			fail_fatal("ignoring the in-between must give concrete coordinate errors");
	}
	std::printf("UFBX_BLEND_SKIN_ORDER_OK\n");

	// ---- Evaluated sampling points (animation path) ----
	const size_t sample_count = sizeof(MINI_SAMPLE_WEIGHTS)/sizeof(MINI_SAMPLE_WEIGHTS[0]);
	if (sample_count != 2) fail_fatal("fixture sample point count mismatch");
	std::vector<std::vector<V3>> sample_positions;
	for (size_t s = 0; s < sample_count; s++) {
		double time = (double)s; // 0s -> 25%, 1s -> 75%
		ufbx_evaluate_opts eval;
		std::memset(&eval, 0, sizeof(eval));
		eval.evaluate_skinning = true;
		ufbx_scene* ev = ufbx_evaluate_scene(static_scene, static_scene->anim, time, &eval, &error);
		if (!ev) {
			std::printf("LOAD_FAILED evaluate_scene t=%g: %s\n", time, error.description.data);
			std::exit(3);
		}
		ufbx_mesh* emesh = ufbx_find_node(ev, "MiniMesh")->mesh;
		std::vector<double> wvec = channel_weights(emesh);
		double w = wvec.empty() ? 0.0 : wvec[0];
		if (std::fabs(w - MINI_SAMPLE_WEIGHTS[s]) > 1e-9)
			fail_fatal("sample %zu morph weight %g != %g", s, w, MINI_SAMPLE_WEIGHTS[s]);
		compare_scene_positions("mini-anim", ev, w);
		std::vector<V3> deformed(emesh->num_vertices);
		for (size_t v = 0; v < emesh->num_vertices; v++)
			deformed[v] = from_ufbx(emesh->skinned_position.values.data[v]);
		sample_positions.push_back(deformed);
		ufbx_free_scene(ev);
	}
	// The two sampled weights (25% and 75%) must produce visibly different
	// geometry, proving the animation sampling path actually drives the morph.
	size_t moved_by_animation = 0;
	for (size_t v = 0; v < sample_positions[0].size(); v++) {
		if (length(sub(sample_positions[0][v], sample_positions[1][v])) > 1e-6)
			moved_by_animation++;
	}
	if (moved_by_animation < 2)
		fail_fatal("animation samples did not move the morphed vertices");

	// ---- Deformed normals ----
	{
		ufbx_evaluate_opts eval;
		std::memset(&eval, 0, sizeof(eval));
		eval.evaluate_skinning = true;
		ufbx_scene* ev = ufbx_evaluate_scene(static_scene, static_scene->anim, 1.0, &eval, &error);
		if (!ev) fail_fatal("evaluate_scene for normals failed");
		ufbx_mesh* emesh = ufbx_find_node(ev, "MiniMesh")->mesh;
		std::vector<V3> deformed(emesh->num_vertices);
		for (size_t v = 0; v < emesh->num_vertices; v++)
			deformed[v] = from_ufbx(emesh->skinned_position.values.data[v]);

		NormalGroups ng = compute_normals(emesh, deformed);

		// Hard edge v0-v3 between tri0 (0,1,3) and tri1 (0,3,2): both
		// adjacent faces are flat so neither endpoint's corners may merge.
		if (ng.corner_group[0] == ng.corner_group[3] ||
		    ng.corner_group[2] == ng.corner_group[4])
			fail_fatal("hard edge was merged into one smoothing group");
		// Smooth edge v2-v3 between tri1 (0,3,2) and tri2 (2,3,5): corners at
		// v3 (4 vs 7) and at v2 (5 vs 6) must share one smoothing group.
		if (ng.corner_group[4] != ng.corner_group[7] ||
		    ng.corner_group[5] != ng.corner_group[6])
			fail_fatal("smooth edge split the smoothing groups");

		// Compare against every library generated skinned normal corner.
		if (!emesh->skinned_normal.exists || !emesh->generated_normals)
			fail_fatal("library did not generate deformed normals");
		for (uint32_t corner = 0; corner < emesh->num_indices; corner++) {
			uint32_t lib_index = emesh->skinned_normal.indices.data[corner];
			V3 lib_n = from_ufbx(emesh->skinned_normal.values.data[lib_index]);
			V3 my_n = ng.group_normal[ng.corner_group[corner]];
			check_normal("mini-normal", corner, 1.0, my_n, lib_n);
		}

		// End-to-end independent check: derive the deformed positions purely
		// from our own skin+morph pipeline on the evaluated pose, then generate
		// normals from them and compare to the library normals. This exercises
		// the whole deformation chain rather than reusing library positions.
		{
			SkinData eval_skin = extract_skin(emesh);
			std::vector<MorphChannel> eval_channels = extract_morphs(emesh);
			std::vector<double> eval_weights = channel_weights(emesh);
			std::vector<V3> expected(emesh->num_vertices);
			for (size_t v = 0; v < emesh->num_vertices; v++)
				expected[v] = expected_position(emesh, eval_skin, eval_channels, eval_weights, v);
			NormalGroups expected_ng = compute_normals(emesh, expected);
			for (uint32_t corner = 0; corner < emesh->num_indices; corner++) {
				uint32_t lib_index = emesh->skinned_normal.indices.data[corner];
				V3 lib_n = from_ufbx(emesh->skinned_normal.values.data[lib_index]);
				V3 exp_n = expected_ng.group_normal[expected_ng.corner_group[corner]];
				check_normal("mini-normal-expected", corner, 1.0, exp_n, lib_n);
			}
		}

		// Prove the deformed normals are not reused rest normals: the same
		// grouping on the undeformed mesh must disagree after deformation.
		std::vector<V3> rest(emesh->num_vertices);
		for (size_t v = 0; v < emesh->num_vertices; v++)
			rest[v] = from_ufbx(emesh->vertices.data[v]);
		NormalGroups rest_ng = compute_normals(emesh, rest);
		size_t changed = 0;
		for (uint32_t corner = 0; corner < emesh->num_indices; corner++) {
			uint32_t lib_index = emesh->skinned_normal.indices.data[corner];
			V3 lib_n = from_ufbx(emesh->skinned_normal.values.data[lib_index]);
			if (length(sub(rest_ng.group_normal[rest_ng.corner_group[corner]], lib_n)) > NORMAL_EPS)
				changed++;
		}
		if (changed < MINI_CORNERS/2)
			fail_fatal("deformed normals appear to reuse the static normals");

		export_obj(obj_path, emesh, deformed, ng);
		ufbx_free_scene(ev);
	}
	std::printf("UFBX_DEFORMED_NORMALS_OK\n");
	ufbx_free_scene(static_scene);
}

// ---------------------------------------------------------------------------
// Part 3: real FBX files through the formal load/evaluate_scene paths
// ---------------------------------------------------------------------------

void run_real_files()
{
	const char* files[] = {
		"data/maya_dual_quaternion_7500_ascii.fbx",
		"data/maya_dual_quaternion_scale_7500_ascii.fbx",
		"data/maya_dq_weights_7500_ascii.fbx",
		"data/maya_blend_inbetween_7500_ascii.fbx",
	};
	const double sample_times[] = {0.0, 0.5, 1.0};
	size_t total_vertices = 0;
	for (size_t fi = 0; fi < sizeof(files)/sizeof(files[0]); fi++) {
		ufbx_error error;
		ufbx_scene* scene = load_scene(files[fi], true, &error);
		// Static load path.
		compare_scene_positions(files[fi], scene, 0.0);
		// Formal evaluate path at several sampling times.
		if (scene->anim) {
			for (double t : sample_times) {
				ufbx_evaluate_opts eval;
				std::memset(&eval, 0, sizeof(eval));
				eval.evaluate_skinning = true;
				ufbx_scene* ev = ufbx_evaluate_scene(scene, scene->anim, t, &eval, &error);
				if (!ev) {
					std::printf("LOAD_FAILED evaluate %s t=%g: %s\n",
						files[fi], t, error.description.data);
					std::exit(3);
				}
				compare_scene_positions(files[fi], ev, t);
				ufbx_free_scene(ev);
			}
		}
		for (size_t mi = 0; mi < scene->meshes.count; mi++)
			total_vertices += scene->meshes.data[mi]->num_vertices;
		ufbx_free_scene(scene);
	}
	if (total_vertices < 100) fail_fatal("real file vertex coverage suspiciously small");
	std::printf("UFBX_SCENE_REFERENCE_OK\n");
	std::printf("scene_reference_vertices=%zu\n", total_vertices);
}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 2) fail_fatal("usage: %s <output-dir>", argv[0]);
	std::string dir = argv[1];
	std::string fbx_path = dir + "/deform_mini.fbx";
	std::string obj_path = dir + "/deformation.obj";

	run_control_cases();
	write_mini_fbx(fbx_path);
	run_fixture(fbx_path, obj_path);
	run_real_files();

	if (g_failures != 0) {
		std::printf("DEFORMATION_REFERENCE_FAILED failures=%d\n", g_failures);
		std::exit(2);
	}
	return 0;
}
