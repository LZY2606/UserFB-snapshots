// Microflake volume test suite: tensor (SGGX math), flight (sparse medium
// traversal) and transport (Radiance integration) groups. No main here;
// task_support/driver.c runs the registered cases.

#include "task_support/check.h"
#include "task_support/surface_access.hpp"
#include "volume/radiance_medium.hpp"

#include <cmath>
#include <vector>

using microflake::Vec3;
using microflake::Sggx;
using microflake::SparseMedium;
using smallpt::Vector3;
using smallpt::Ray;
using smallpt::RNG;

namespace {

	//-------------------------------------------------------------------------
	// Helpers
	//-------------------------------------------------------------------------

	void RotationMatrix(double ax, double ay, double az, double angle, double R[9]) {
		const double n = std::sqrt(ax * ax + ay * ay + az * az);
		ax /= n; ay /= n; az /= n;
		const double c = std::cos(angle), s = std::sin(angle), t = 1.0 - c;
		R[0] = t * ax * ax + c;      R[1] = t * ax * ay - s * az;  R[2] = t * ax * az + s * ay;
		R[3] = t * ax * ay + s * az; R[4] = t * ay * ay + c;       R[5] = t * ay * az - s * ax;
		R[6] = t * ax * az - s * ay; R[7] = t * ay * az + s * ax;  R[8] = t * az * az + c;
	}

	Vec3 ApplyRotation(const double R[9], const Vec3& v) {
		return {
			R[0] * v.x + R[1] * v.y + R[2] * v.z,
			R[3] * v.x + R[4] * v.y + R[5] * v.z,
			R[6] * v.x + R[7] * v.y + R[8] * v.z
		};
	}

	// S = R diag(diag) R^T, output as (xx, yy, zz, xy, xz, yz).
	void TensorFromDiagRotation(const double diag[3], const double R[9], double S[6]) {
		double M[9] = {};
		for (int i = 0; i < 3; ++i) {
			for (int j = 0; j < 3; ++j) {
				double acc = 0.0;
				for (int k = 0; k < 3; ++k) {
					acc += R[3 * i + k] * diag[k] * R[3 * j + k];
				}
				M[3 * i + j] = acc;
			}
		}
		S[0] = M[0]; S[1] = M[4]; S[2] = M[8];
		S[3] = M[1]; S[4] = M[2]; S[5] = M[5];
	}

	// S' = R S R^T for S given as (xx, yy, zz, xy, xz, yz).
	void RotateTensor(const double R[9], const double S[6], double out[6]) {
		const double M[9] = {
			S[0], S[3], S[4],
			S[3], S[1], S[5],
			S[4], S[5], S[2]
		};
		double T[9] = {};
		for (int i = 0; i < 3; ++i) {
			for (int j = 0; j < 3; ++j) {
				double acc = 0.0;
				for (int k = 0; k < 3; ++k) {
					for (int l = 0; l < 3; ++l) {
						acc += R[3 * i + k] * M[3 * k + l] * R[3 * j + l];
					}
				}
				T[3 * i + j] = acc;
			}
		}
		out[0] = T[0]; out[1] = T[4]; out[2] = T[8];
		out[3] = T[1]; out[4] = T[2]; out[5] = T[5];
	}

	// Independent latitude-longitude sphere quadrature of f over unit directions.
	template< typename F >
	double SphereQuadrature(int ntheta, int nphi, F&& f) {
		double sum = 0.0;
		for (int i = 0; i < ntheta; ++i) {
			const double theta = microflake::kPi * (i + 0.5) / ntheta;
			const double st = std::sin(theta), ct = std::cos(theta);
			const double w = st * (microflake::kPi / ntheta) * (2.0 * microflake::kPi / nphi);
			for (int j = 0; j < nphi; ++j) {
				const double phi = 2.0 * microflake::kPi * (j + 0.5) / nphi;
				const Vec3 m{ st * std::cos(phi), st * std::sin(phi), ct };
				sum += f(m) * w;
			}
		}
		return sum;
	}

	SparseMedium::CellInput Cell(std::int32_t kx, std::int32_t ky, std::int32_t kz,
	                             double rho, double albedo,
	                             double sxx = 1.0, double syy = 1.0, double szz = 1.0,
	                             double sxy = 0.0, double sxz = 0.0, double syz = 0.0) {
		return SparseMedium::CellInput{
			kx, ky, kz, rho, albedo, albedo, albedo,
			sxx, syy, szz, sxy, sxz, syz
		};
	}

	//-------------------------------------------------------------------------
	// tensor group
	//-------------------------------------------------------------------------

	int TensorPhaseNormalization() {
		double R[9];
		RotationMatrix(1.0, 2.0, 3.0, 0.9, R);
		double Srot[6];
		TensorFromDiagRotation({ 4.0, 0.5, 0.09 }, R, Srot); // rotated, off-diagonal
		const Sggx iso(1.0, 1.0, 1.0, 0.0, 0.0, 0.0);
		const Sggx ani(4.0, 1.0, 0.25, 0.0, 0.0, 0.0);
		const Sggx rot(Srot[0], Srot[1], Srot[2], Srot[3], Srot[4], Srot[5]);
		const Sggx* tensors[3] = { &iso, &ani, &rot };
		const Vec3 wis[3] = {
			Vec3{ 0.0, 0.0, 1.0 },
			microflake::Normalize(Vec3{ 1.0, 2.0, 3.0 }),
			microflake::Normalize(Vec3{ -0.5, 0.25, 1.0 })
		};
		// Independent sphere quadrature of the directional pdf: integral = 1.
		for (const Sggx* t : tensors) {
			for (const Vec3& wi : wis) {
				const double norm = SphereQuadrature(240, 480, [&](const Vec3& wo) {
					return t->Phase(wo, wi);
				});
				TASK_CHECK(std::fabs(norm - 1.0) < 3e-3);
			}
		}
		// Reciprocity: sigma(wi) p(wo|wi) == sigma(wo) p(wi|wo) == D(h)/4.
		const Vec3 wos[3] = {
			microflake::Normalize(Vec3{ 1.0, -2.0, 0.5 }),
			microflake::Normalize(Vec3{ -1.0, -1.0, -1.0 }),
			Vec3{ 0.0, 0.0, 1.0 }
		};
		for (const Sggx* t : tensors) {
			for (const Vec3& wi : wis) {
				for (const Vec3& wo : wos) {
					const double lhs = t->Sigma(wi) * t->Phase(wo, wi);
					const double rhs = t->Sigma(wo) * t->Phase(wi, wo);
					TASK_CHECK(std::fabs(lhs - rhs) <= 1e-12 * (1.0 + std::fabs(lhs)));
				}
			}
		}
		// Zero-measure antipodal set wi + wo == 0 evaluates to 0.
		TASK_CHECK(iso.Phase(Vec3{ 0.0, 0.0, -1.0 }, Vec3{ 0.0, 0.0, 1.0 }) == 0.0);
		TASK_CHECK(rot.Phase(Vec3{ 0.0, 0.0, -1.0 }, Vec3{ 0.0, 0.0, 1.0 }) == 0.0);
		return 0;
	}

	int TensorProjectedArea() {
		double R[9];
		RotationMatrix(1.0, 2.0, 3.0, 0.9, R);
		double Srot[6];
		TensorFromDiagRotation({ 4.0, 0.5, 0.09 }, R, Srot);
		const Sggx ani(4.0, 1.0, 0.25, 0.0, 0.0, 0.0);
		const Sggx rot(Srot[0], Srot[1], Srot[2], Srot[3], Srot[4], Srot[5]);
		const Sggx* tensors[2] = { &ani, &rot };
		const Vec3 ws[4] = {
			Vec3{ 0.0, 0.0, 1.0 },
			Vec3{ 1.0, 0.0, 0.0 },
			microflake::Normalize(Vec3{ 1.0, 2.0, 3.0 }),
			microflake::Normalize(Vec3{ -0.5, 0.25, 1.0 })
		};
		// Projected area: integral_{m.w > 0} D(m) (m.w) dm == sigma(w).
		for (const Sggx* t : tensors) {
			for (const Vec3& w : ws) {
				const double ref = SphereQuadrature(240, 480, [&](const Vec3& m) {
					const double c = microflake::Dot(m, w);
					return c > 0.0 ? t->D(m) * c : 0.0;
				});
				const double expect = t->ProjectedArea(w);
				TASK_CHECK(std::fabs(ref - expect) < 3e-3 * expect + 1e-4);
			}
		}
		return 0;
	}

	int TensorDegenerateScalingCovariance() {
		// S = s^2 I: extinction rho * s and uniform sphere phase 1/(4 pi).
		const double s = 2.5;
		const Sggx iso(s * s, s * s, s * s, 0.0, 0.0, 0.0);
		const Vec3 dirs[4] = {
			Vec3{ 0.0, 0.0, 1.0 },
			microflake::Normalize(Vec3{ 1.0, 2.0, 3.0 }),
			microflake::Normalize(Vec3{ -0.5, 0.25, 1.0 }),
			microflake::Normalize(Vec3{ 1.0, -2.0, 0.5 })
		};
		for (const Vec3& w : dirs) {
			TASK_CHECK(std::fabs(iso.Sigma(w) - s) < 1e-12);
		}
		for (const Vec3& wi : dirs) {
			for (const Vec3& wo : dirs) {
				TASK_CHECK(std::fabs(iso.Phase(wo, wi) - 1.0 / (4.0 * microflake::kPi)) < 1e-12);
			}
		}

		// S -> c^2 S: extinction scales by c, phase function unchanged.
		const Sggx base(2.0, 1.0, 0.5, 0.3, -0.2, 0.1);
		const double c = 3.0, c2 = c * c;
		const Sggx scaled(c2 * 2.0, c2 * 1.0, c2 * 0.5, c2 * 0.3, c2 * -0.2, c2 * 0.1);
		for (const Vec3& wi : dirs) {
			TASK_CHECK(std::fabs(scaled.Sigma(wi) - c * base.Sigma(wi)) < 1e-12);
			for (const Vec3& wo : dirs) {
				TASK_CHECK(std::fabs(scaled.Phase(wo, wi) - base.Phase(wo, wi)) < 1e-12);
			}
		}

		// Rotating S and all directions together is covariant.
		double R[9];
		RotationMatrix(0.3, 1.0, -0.6, 1.1, R);
		const double Sb[6] = { 2.0, 1.0, 0.5, 0.3, -0.2, 0.1 };
		double Sr[6];
		RotateTensor(R, Sb, Sr);
		const Sggx rot(Sr[0], Sr[1], Sr[2], Sr[3], Sr[4], Sr[5]);
		for (const Vec3& wi : dirs) {
			const Vec3 rwi = ApplyRotation(R, wi);
			TASK_CHECK(std::fabs(rot.Sigma(rwi) - base.Sigma(wi)) < 1e-12);
			for (const Vec3& wo : dirs) {
				const Vec3 rwo = ApplyRotation(R, wo);
				TASK_CHECK(std::fabs(rot.Phase(rwo, rwi) - base.Phase(wo, wi)) < 1e-11);
			}
		}
		return 0;
	}

	int TensorVisibleNormalSampling() {
		double R[9];
		RotationMatrix(0.3, 1.0, -0.6, 1.1, R);
		double S[6];
		TensorFromDiagRotation({ 3.0, 0.8, 0.2 }, R, S); // rotated, off-diagonal
		const Sggx g(S[0], S[1], S[2], S[3], S[4], S[5]);
		const Vec3 wi = microflake::Normalize(Vec3{ 0.2, 0.5, -0.84 });
		const double sigma = g.Sigma(wi);

		// Quadrature reference for the visible-normal distribution
		// p_v(m) = D(m) max(0, m.wi) / sigma(wi).
		const double ref_norm = SphereQuadrature(300, 600, [&](const Vec3& m) {
			const double c = microflake::Dot(m, wi);
			return c > 0.0 ? g.D(m) * c / sigma : 0.0;
		});
		TASK_CHECK(std::fabs(ref_norm - 1.0) < 2e-3);
		double ref[3];
		for (int k = 0; k < 3; ++k) {
			ref[k] = SphereQuadrature(300, 600, [&](const Vec3& m) {
				const double c = microflake::Dot(m, wi);
				return c > 0.0 ? g.D(m) * c / sigma * m[k] : 0.0;
			});
		}

		// Fixed-seed statistical moment comparison (not a pdf-vs-eval check):
		// sample size N = 200000; the per-component standard error
		// SE_k = sqrt(Var[m_k] / N) is estimated from the same samples and the
		// sample mean must agree with the quadrature reference within 5 SE
		// (plus a small absolute floor for the quadrature error).
		RNG rng(987654321u);
		const int N = 200000;
		double sum[3] = {}, sq[3] = {};
		double min_dot = 1.0;
		for (int i = 0; i < N; ++i) {
			const Vec3 m = g.SampleVisibleNormal(wi, rng.Uniform(), rng.Uniform());
			const double c = microflake::Dot(m, wi);
			min_dot = std::min(min_dot, c);
			for (int k = 0; k < 3; ++k) {
				sum[k] += m[k];
				sq[k]  += m[k] * m[k];
			}
		}
		TASK_CHECK(min_dot > 0.0); // every sampled normal is visible
		for (int k = 0; k < 3; ++k) {
			const double mean = sum[k] / N;
			const double var  = sq[k] / N - mean * mean;
			const double se   = std::sqrt(var / N);
			TASK_CHECK(std::fabs(mean - ref[k]) < 5.0 * se + 2e-4);
		}

		// Sampled normals carry positive visible-normal pdf.
		const Vec3 m = g.SampleVisibleNormal(wi, 0.31, 0.77);
		TASK_CHECK(g.VisibleNormalPdf(m, wi) > 0.0);
		return 0;
	}

	//-------------------------------------------------------------------------
	// flight group
	//-------------------------------------------------------------------------

	int FlightFirstCollisionCdf() {
		// Cells along +x (including a negative key) separated by vacuum gaps.
		// Ray o + t d crosses:
		//   cell -3: t in [0.5, 1.5), rate 2.0  (cumulative tau 2.0)
		//   cell  0: t in [3.5, 4.5), rate 0.5  (cumulative tau 2.5)
		//   cell  2: t in [5.5, 6.5), rate 1.0  (cumulative tau 3.5)
		//   cell  5: t in [8.5, 9.5), rate 0.25 (cumulative tau 3.75)
		const std::vector< SparseMedium::CellInput > cells = {
			Cell(-3, 0, 0, 2.0,  0.5),
			Cell( 0, 0, 0, 0.5,  0.5),
			Cell( 2, 0, 0, 1.0,  0.5),
			Cell( 5, 0, 0, 0.25, 0.5)
		};
		const SparseMedium med(Vec3{ 0.0, 0.0, 0.0 }, 1.0, cells);
		const Vec3 o{ -3.5, 0.5, 0.5 };
		const Vec3 d{ 1.0, 0.0, 0.0 };

		// Analytic transmittance from the per-cell accumulated rates.
		TASK_CHECK(std::fabs(med.Transmittance(o, d, 0.0, 10.0) - std::exp(-3.75)) < 1e-12);

		// First-collision CDF: u maps to tau = -log(1-u); the hit must consume
		// exactly that depth along the whole segment without resampling.
		const struct { double tau, t; int key; } rows[] = {
			{ 0.5,      0.75,     -3 }, // 0.5 + 0.5/2
			{ 1.999999, 1.4999995,-3 },
			{ 2.000001, 3.500002,  0 }, // past the interval end: next occupied cell decides
			{ 2.25,     4.0,       0 }, // 3.5 + 0.25/0.5
			{ 3.0,      6.0,       2 }, // 5.5 + 0.5/1
			{ 3.6,      8.9,       5 }  // 8.5 + 0.1/0.25
		};
		for (const auto& row : rows) {
			const double u = 1.0 - std::exp(-row.tau);
			SparseMedium::Collision c;
			TASK_CHECK(med.FirstCollision(o, d, 0.0, 10.0, u, c));
			TASK_CHECK(std::fabs(c.t - row.t) < 1e-6);
			TASK_CHECK(med.CellAt(c.cell).kx == row.key);
		}
		// Escape: depth beyond the total 3.75 leaves through the window end.
		{
			const double u = 1.0 - std::exp(-3.750001);
			SparseMedium::Collision c;
			TASK_CHECK(!med.FirstCollision(o, d, 0.0, 10.0, u, c));
		}

		// Truncated window clips cells mid-interval: tau = 2*0.75 + 0.5*0.75.
		TASK_CHECK(std::fabs(med.Transmittance(o, d, 0.75, 4.25) - std::exp(-1.875)) < 1e-12);
		{
			SparseMedium::Collision c;
			TASK_CHECK(med.FirstCollision(o, d, 0.75, 4.25, 1.0 - std::exp(-1.0), c));
			TASK_CHECK(std::fabs(c.t - 1.25) < 1e-6); // 0.75 + 1.0/2
			TASK_CHECK(med.CellAt(c.cell).kx == -3);
			TASK_CHECK(med.FirstCollision(o, d, 0.75, 4.25, 1.0 - std::exp(-1.6), c));
			TASK_CHECK(std::fabs(c.t - 3.7) < 1e-6); // 3.5 + (1.6-1.5)/0.5
			TASK_CHECK(med.CellAt(c.cell).kx == 0);
		}

		// Direction scaling: a non-unit direction covers the same world span
		// in half the parameter; the collision distance stays in ray parameter.
		const Vec3 d2{ 2.0, 0.0, 0.0 };
		TASK_CHECK(std::fabs(med.Transmittance(o, d2, 0.0, 5.0) - std::exp(-3.75)) < 1e-12);
		{
			SparseMedium::Collision c;
			TASK_CHECK(med.FirstCollision(o, d2, 0.0, 5.0, 1.0 - std::exp(-3.0), c));
			TASK_CHECK(std::fabs(c.t - 3.0) < 1e-6); // world distance 6.0 at |d| = 2
			TASK_CHECK(med.CellAt(c.cell).kx == 2);
		}

		// Negative direction crosses the same cells in reverse order.
		const Vec3 o2{ 6.5, 0.5, 0.5 };
		const Vec3 d3{ -1.0, 0.0, 0.0 };
		TASK_CHECK(std::fabs(med.Transmittance(o2, d3, 0.0, 10.0) - std::exp(-3.75)) < 1e-12);
		{
			SparseMedium::Collision c;
			TASK_CHECK(med.FirstCollision(o2, d3, 0.0, 10.0, 1.0 - std::exp(-0.5), c));
			TASK_CHECK(std::fabs(c.t - 3.75) < 1e-6); // cell 5 gives 0.25, then 3.5 + 0.25/1
			TASK_CHECK(med.CellAt(c.cell).kx == 2);
		}
		return 0;
	}

	int FlightGridOwnership() {
		const std::vector< SparseMedium::CellInput > cells = {
			Cell( 0, 0, 0, 1.0, 0.5),
			Cell( 1, 1, 0, 1.0, 0.5),
			Cell( 1, 0, 0, 1.0, 0.5),
			Cell( 0, 1, 0, 1.0, 0.5),
			Cell( 2, 0, 0, 1.0, 0.5),
			Cell(-1, 0, 0, 1.0, 0.5),
			Cell( 0, 0,-2, 1.0, 0.5)
		};
		const SparseMedium med(Vec3{ 0.0, 0.0, 0.0 }, 1.0, cells);
		const double s2 = std::sqrt(2.0);

		// Equal-distance two-axis crossing: the diagonal corner at (1,1) is
		// crossed by advancing both axes together; (1,0) and (0,1) own nothing.
		{
			const Vec3 o{ 0.0, 0.0, 0.5 };
			const Vec3 d = microflake::Normalize(Vec3{ 1.0, 1.0, 0.0 });
			const auto iv = med.OccupiedIntervals(o, d, 0.0, 3.0);
			TASK_CHECK(iv.size() == 2u);
			TASK_CHECK(iv[0].kx == 0 && iv[0].ky == 0 && iv[0].kz == 0);
			TASK_CHECK(std::fabs(iv[0].t0 - 0.0) < 1e-12 && std::fabs(iv[0].t1 - s2) < 1e-12);
			TASK_CHECK(iv[1].kx == 1 && iv[1].ky == 1 && iv[1].kz == 0);
			TASK_CHECK(std::fabs(iv[1].t0 - s2) < 1e-12 && std::fabs(iv[1].t1 - 2.0 * s2) < 1e-12);
		}

		// Face start, positive direction: the point x = 1 belongs to cell 1.
		{
			const Vec3 o{ 1.0, 0.5, 0.5 };
			const Vec3 d{ 1.0, 0.0, 0.0 };
			const auto iv = med.OccupiedIntervals(o, d, 0.0, 2.0);
			TASK_CHECK(iv.size() == 2u);
			TASK_CHECK(iv[0].kx == 1 && std::fabs(iv[0].t0 - 0.0) < 1e-12 && std::fabs(iv[0].t1 - 1.0) < 1e-12);
			TASK_CHECK(iv[1].kx == 2 && std::fabs(iv[1].t0 - 1.0) < 1e-12 && std::fabs(iv[1].t1 - 2.0) < 1e-12);
		}

		// Face start, negative direction: cell 1 owns only the zero-length
		// instant; cell 0 owns [0,1) and cell -1 owns [1,2).
		{
			const Vec3 o{ 1.0, 0.5, 0.5 };
			const Vec3 d{ -1.0, 0.0, 0.0 };
			const auto iv = med.OccupiedIntervals(o, d, 0.0, 2.0);
			TASK_CHECK(iv.size() == 2u);
			TASK_CHECK(iv[0].kx == 0 && std::fabs(iv[0].t0 - 0.0) < 1e-12 && std::fabs(iv[0].t1 - 1.0) < 1e-12);
			TASK_CHECK(iv[1].kx == -1 && std::fabs(iv[1].t0 - 1.0) < 1e-12 && std::fabs(iv[1].t1 - 2.0) < 1e-12);
		}

		// Axis-parallel ray towards negative keys.
		{
			const Vec3 o{ 0.5, 0.5, -2.5 };
			const Vec3 d{ 0.0, 0.0, 1.0 };
			const auto iv = med.OccupiedIntervals(o, d, 0.0, 4.0);
			TASK_CHECK(iv.size() == 1u);
			TASK_CHECK(iv[0].kz == -2 && std::fabs(iv[0].t0 - 0.5) < 1e-12 && std::fabs(iv[0].t1 - 1.5) < 1e-12);
		}
		return 0;
	}

	int FlightHierarchySkipsEmptySpace() {
		// Two occupied cells a million cells apart, plus 20000 irrelevant
		// occupied cells far off the ray.
		std::vector< SparseMedium::CellInput > cells;
		cells.push_back(Cell(0, 0, 0, 1.0, 0.5));
		cells.push_back(Cell(1000000, 0, 0, 1.0, 0.5));
		for (std::int32_t i = 0; i < 20000; ++i) {
			cells.push_back(Cell(i, 500, 0, 1.0, 0.5));
		}
		const SparseMedium med(Vec3{ 0.0, 0.0, 0.0 }, 1.0, cells);

		const Vec3 o{ -0.5, 0.5, 0.5 };
		const Vec3 d{ 1.0, 0.0, 0.0 };
		microflake::TraversalStats st;
		const double T = med.Transmittance(o, d, 0.0, 1000002.0, &st);
		TASK_CHECK(std::fabs(T - std::exp(-2.0)) < 1e-9);
		// The million-cell gap is skipped at block granularity: no enumeration
		// of empty cells, and far fewer cell tests than a full scan.
		TASK_CHECK(st.cellTests < 200u);
		TASK_CHECK(st.nodeTests > 0u && st.nodeTests < 200000u);
		TASK_CHECK(st.cellTests * 100u < med.CellCount());

		// First collision across the gap, in the original ray parameter.
		microflake::TraversalStats st2;
		SparseMedium::Collision c;
		TASK_CHECK(med.FirstCollision(o, d, 0.0, 1000002.0, 1.0 - std::exp(-1.5), c, &st2));
		TASK_CHECK(std::fabs(c.t - 1000001.0) < 1e-6); // 1000000.5 + 0.5/1
		TASK_CHECK(med.CellAt(c.cell).kx == 1000000);
		TASK_CHECK(st2.cellTests < 200u);
		return 0;
	}

	//-------------------------------------------------------------------------
	// transport group
	//-------------------------------------------------------------------------

	int TransportPureAbsorptionDirectLight() {
		// Pure absorption slab (albedo 0) on the direct light ray of the
		// baseline: tau = 0.5 * 4 = 2, so the mean must be 12 * exp(-2).
		std::vector< SparseMedium::CellInput > cells;
		for (std::int32_t y = 71; y <= 74; ++y) {
			cells.push_back(Cell(50, y, 81, 0.5, 0.0));
		}
		const SparseMedium medium(Vec3{ 0.0, 0.0, 0.0 }, 1.0, cells);
		const double T = std::exp(-2.0);

		RNG rng(7u);
		const int N = 20000;
		double sum = 0.0;
		for (int i = 0; i < N; ++i) {
			const Vector3 c = Radiance(Ray(Vector3(50.0, 70.0, 81.6), Vector3(0.0, 1.0, 0.0), EPSILON_SPHERE),
			                           rng, &medium, nullptr);
			// Analog collision estimator: each sample is fully killed or fully
			// transmitted; no Beer factor is multiplied at the surface.
			TASK_CHECK(c.m_x == 0.0 || c.m_x == 12.0);
			sum += c.m_x;
		}
		// Binomial standard error of the mean at N = 20000 is about 0.029.
		TASK_CHECK(std::fabs(sum / N - 12.0 * T) < 0.15);

		// Vacuum behavior: an empty medium and an all-rho=0 medium preserve
		// colors and the RNG stream exactly, also over multiple bounces.
		const SparseMedium empty(Vec3{ 0.0, 0.0, 0.0 }, 1.0, {});
		const SparseMedium zero(Vec3{ 0.0, 0.0, 0.0 }, 1.0,
		                        { Cell(50, 72, 81, 0.0, 0.5) }); // on the direct ray
		const Ray probe(Vector3(50.0, 52.0, 295.6),
		                smallpt::Normalize(Vector3(0.05, -0.05, -1.0)), EPSILON_SPHERE);
		for (const SparseMedium* m : { &empty, &zero }) {
			RNG r1(424242u), r2(424242u);
			const Vector3 a = Radiance(probe, r1);
			const Vector3 b = Radiance(probe, r2, m, nullptr);
			TASK_CHECK(a == b);
			TASK_CHECK(r1.Uniform() == r2.Uniform());
			TASK_CHECK(r1.Uniform() == r2.Uniform());
		}
		{
			RNG r1(99u), r2(99u);
			const Vector3 a = Radiance(Ray(Vector3(50.0, 70.0, 81.6), Vector3(0.0, 1.0, 0.0), EPSILON_SPHERE), r1);
			const Vector3 b = Radiance(Ray(Vector3(50.0, 70.0, 81.6), Vector3(0.0, 1.0, 0.0), EPSILON_SPHERE), r2, &zero, nullptr);
			TASK_CHECK(a == b);
			TASK_CHECK(a == Vector3(12.0));
			TASK_CHECK(r1.Uniform() == r2.Uniform());
		}
		return 0;
	}

	int TransportScatteringAndContinuation() {
		// Scattering slab with a rotated off-diagonal tensor inside the box.
		double R[9];
		RotationMatrix(0.3, 1.0, -0.6, 1.1, R);
		double S[6];
		TensorFromDiagRotation({ 2.25, 0.5625, 1.0 }, R, S);
		std::vector< SparseMedium::CellInput > cells;
		for (std::int32_t ix = 5; ix < 20; ++ix) {
			for (std::int32_t iy = 2; iy < 18; ++iy) {
				for (std::int32_t iz = 5; iz < 35; ++iz) {
					cells.push_back(Cell(ix, iy, iz, 0.02, 0.75,
					                     S[0], S[1], S[2], S[3], S[4], S[5]));
				}
			}
		}
		const SparseMedium medium(Vec3{ 0.0, 0.0, 0.0 }, 4.0, cells);

		// Small render from the original camera through the original spheres.
		smallpt::MediumRenderStats stats;
		RNG rng(2024u);
		const Vector3 eye(50.0, 52.0, 295.6);
		const Vector3 gaze = smallpt::Normalize(Vector3(0.0, -0.042612, -1.0));
		double sum = 0.0;
		for (unsigned y = 0u; y < 12u; ++y) {
			for (unsigned x = 0u; x < 16u; ++x) {
				const Vector3 d = smallpt::Normalize(
					gaze + Vector3((double(x) + 0.5 - 8.0) * 0.09,
					               (double(y) + 0.5 - 6.0) * 0.09, 0.0));
				for (unsigned s = 0u; s < 4u; ++s) {
					const Vector3 c = Radiance(Ray(eye + d * 130.0, d, EPSILON_SPHERE),
					                           rng, &medium, &stats);
					TASK_CHECK(!c.HasNaNs() && std::isfinite(c.Max()) && c.Min() >= 0.0);
					sum += c.m_x + c.m_y + c.m_z;
				}
			}
		}
		TASK_CHECK(sum > 0.0);
		// More than two real volume scattering events happened.
		TASK_CHECK(stats.volumeCollisions >= 3u);
		TASK_CHECK(stats.mediumQueries > 0u);
		TASK_CHECK(stats.cellTests > 0u);
		TASK_CHECK(stats.maxQueryDepth >= 1u);

		// A deterministic ray into the mirror sphere: the specular successor
		// keeps competing volume events against the medium.
		{
			smallpt::MediumRenderStats ms;
			RNG mr(5u);
			Radiance(Ray(Vector3(27.0, 40.0, 47.0), Vector3(0.0, -1.0, 0.0), EPSILON_SPHERE),
			         mr, &medium, &ms);
			TASK_CHECK(ms.mediumQueries >= 2u);
			TASK_CHECK(ms.maxQueryDepth >= 1u);
		}
		// Same for a refractive successor through the glass sphere.
		{
			smallpt::MediumRenderStats ms;
			RNG mr(6u);
			Radiance(Ray(Vector3(73.0, 40.0, 78.0), Vector3(0.0, -1.0, 0.0), EPSILON_SPHERE),
			         mr, &medium, &ms);
			TASK_CHECK(ms.mediumQueries >= 2u);
			TASK_CHECK(ms.maxQueryDepth >= 1u);
		}
		return 0;
	}
}

const struct task_case task_cases[] = {
	{ "tensor", "phase_normalization",          &TensorPhaseNormalization },
	{ "tensor", "projected_area",               &TensorProjectedArea },
	{ "tensor", "degenerate_scaling_covariance",&TensorDegenerateScalingCovariance },
	{ "tensor", "visible_normal_sampling",      &TensorVisibleNormalSampling },
	{ "flight", "first_collision_cdf",          &FlightFirstCollisionCdf },
	{ "flight", "grid_ownership",               &FlightGridOwnership },
	{ "flight", "hierarchy_skips_empty_space",  &FlightHierarchySkipsEmptySpace },
	{ "transport", "pure_absorption_direct_light", &TransportPureAbsorptionDirectLight },
	{ "transport", "scattering_and_continuation",  &TransportScatteringAndContinuation }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
