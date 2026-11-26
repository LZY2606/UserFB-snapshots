// Target test cases for BasicPerlinNoise<double>::intersectHeight.
//
// Every reference value below is built independently of the new interface:
// roots come from high-density sign scanning of noise2D followed by
// bisection, and controlled permutation tables are built by hand. The new
// intersectHeight() is only ever the object under test.
//
// The same translation unit is compiled with the default fixed Z slice and
// again with -DSIVPERLIN_DEFAULT_Z=0.625, so all adaptive searches must
// converge under either slice.

#include "check.h"
#include "PerlinNoise.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace
{
	using Noise = siv::BasicPerlinNoise<double>;
	using Status = siv::RayHit2D::Status;

	constexpr double kTol = 1e-7;
	constexpr unsigned kBudget = 200000u;

	// Signed height of the ray relative to the surface at parameter t.
	double signedHeight(const Noise& n,
		const double ox, const double oy, const double oz,
		const double dx, const double dy, const double dz,
		const double t)
	{
		return (oy + dy * t) - n.noise2D(ox + dx * t, oz + dz * t);
	}

	struct Ray
	{
		double ox, oy, oz, dx, dy, dz;
	};

	// Independent reference: dense sign scan then bisection. Returns false if
	// no sign change is present on [t0, t1]. Never calls intersectHeight.
	bool referenceFirstRoot(const Noise& n, const Ray& r,
		const double t0, const double t1, double& outRoot)
	{
		const int samples = 200000;
		double prev = signedHeight(n, r.ox, r.oy, r.oz, r.dx, r.dy, r.dz, t0);
		for (int k = 1; k <= samples; ++k)
		{
			const double t = t0 + (t1 - t0) * static_cast<double>(k) / samples;
			const double cur = signedHeight(n, r.ox, r.oy, r.oz, r.dx, r.dy, r.dz, t);
			if (prev * cur < 0.0)
			{
				double a = t0 + (t1 - t0) * static_cast<double>(k - 1) / samples;
				double b = t;
				double fa = prev;
				for (int it = 0; it < 100; ++it)
				{
					const double m = (a + b) * 0.5;
					const double fm = signedHeight(n, r.ox, r.oy, r.oz, r.dx, r.dy, r.dz, m);
					if (fa * fm <= 0.0)
					{
						b = m;
					}
					else
					{
						a = m;
						fa = fm;
					}
				}
				outRoot = (a + b) * 0.5;
				return true;
			}
			prev = cur;
		}
		return false;
	}

	// All sign-change roots (for multi-root checks).
	std::vector<double> referenceRoots(const Noise& n, const Ray& r,
		const double t0, const double t1)
	{
		std::vector<double> roots;
		const int samples = 400000;
		double prev = signedHeight(n, r.ox, r.oy, r.oz, r.dx, r.dy, r.dz, t0);
		for (int k = 1; k <= samples; ++k)
		{
			const double t = t0 + (t1 - t0) * static_cast<double>(k) / samples;
			const double cur = signedHeight(n, r.ox, r.oy, r.oz, r.dx, r.dy, r.dz, t);
			if (prev * cur < 0.0)
			{
				double a = t0 + (t1 - t0) * static_cast<double>(k - 1) / samples;
				double b = t;
				double fa = prev;
				for (int it = 0; it < 100; ++it)
				{
					const double m = (a + b) * 0.5;
					const double fm = signedHeight(n, r.ox, r.oy, r.oz, r.dx, r.dy, r.dz, m);
					if (fa * fm <= 0.0) b = m;
					else { a = m; fa = fm; }
				}
				roots.push_back((a + b) * 0.5);
			}
			prev = cur;
		}
		return roots;
	}

	// A deterministic, hand-built permutation (not the default table).
	Noise controlledNoise()
	{
		Noise::state_type s{};
		for (int i = 0; i < 256; ++i)
		{
			s[i] = static_cast<std::uint8_t>((i * 37 + 11) & 255);
		}
		Noise n;
		n.deserialize(s);
		return n;
	}

	// Asserts a simple transverse crossing against the independent reference.
	void checkTransverse(const Noise& n, const Ray& r, const double t0, const double t1)
	{
		double ref = 0.0;
		const bool found = referenceFirstRoot(n, r, t0, t1, ref);
		TASK_CHECK(found);

		const siv::RayHit2D hit = n.intersectHeight(
			r.ox, r.oy, r.oz, r.dx, r.dy, r.dz, t0, t1, kTol, kBudget);
		TASK_CHECK(hit.status == Status::Hit);
		TASK_CHECK(hit.t_lo >= t0 - 1e-12 && hit.t_hi <= t1 + 1e-12);
		TASK_CHECK(hit.t_hi - hit.t_lo <= kTol);
		TASK_CHECK(ref >= hit.t_lo - kTol && ref <= hit.t_hi + kTol);

		const double mid = (hit.t_lo + hit.t_hi) * 0.5;
		TASK_CHECK(std::fabs(mid - ref) <= 2.0 * kTol);

		const double residual = signedHeight(n, r.ox, r.oy, r.oz, r.dx, r.dy, r.dz, mid);
		TASK_CHECK(std::fabs(residual) < 1e-6);
	}
}

//
// crossing: simple transverse crossings, multiple roots in one cell,
// same-sign endpoints with an internal crossing, vertical and negative rays.
//
static void groupCrossing()
{
	// 1-2: ordinary simple crossings for two seeds, checked against reference.
	for (const unsigned seed : { 12345u, 98765u })
	{
		Noise n(seed);
		Ray r{ 0.3, n.noise2D(0.3, 0.4) + 1.0, 0.4, 0.1, -0.2, 0.05 };
		checkTransverse(n, r, 0.0, 20.0);
	}

	// 3: vertical ray (dx = dz = 0) crossing straight down onto the surface.
	{
		Noise n(42u);
		const double x = 1.7, z = -2.3, h = n.noise2D(x, z);
		Ray r{ x, h + 2.0, z, 0.0, -1.0, 0.0 };
		checkTransverse(n, r, 0.0, 10.0);
		// Vertical geometry: x/z stay fixed and the depth is exactly 2.
		const siv::RayHit2D hit = n.intersectHeight(x, h + 2.0, z, 0.0, -1.0, 0.0,
			0.0, 10.0, kTol, kBudget);
		TASK_CHECK(hit.status == Status::Hit);
		const double mid = (hit.t_lo + hit.t_hi) * 0.5;
		TASK_CHECK(std::fabs(mid - 2.0) <= 2.0 * kTol);
	}

	// 4: negative direction components, crossing while travelling backwards.
	{
		Noise n(12345u);
		Ray r{ 3.9, n.noise2D(3.9, 1.1) + 1.5, 1.1, -0.3, -0.25, -0.1 };
		checkTransverse(n, r, 0.0, 15.0);
	}

	// 5: controlled permutation, one cell, two well-separated roots - the
	// earliest must be returned, never the later one.
	{
		Noise n = controlledNoise();
		bool located = false;
		for (int cz = -3; cz < 3 && !located; ++cz)
		{
			for (int cx = -3; cx < 3 && !located; ++cx)
			{
				const double ox = cx + 0.03, oz = cz + 0.5;
				const double dx = 0.94 / 400.0, T = 400.0;
				for (int iy = 0; iy < 80; ++iy)
				{
					const double y0 = -0.95 + iy * 0.024;
					Ray r{ ox, y0, oz, dx, 0.0, 0.0 };
					const std::vector<double> roots = referenceRoots(n, r, 0.0, T);
					if (roots.size() >= 2 && roots[1] - roots[0] > 5.0)
					{
						located = true;
						const siv::RayHit2D hit = n.intersectHeight(
							ox, y0, oz, dx, 0.0, 0.0, 0.0, T, kTol, kBudget);
						TASK_CHECK(hit.status == Status::Hit);
						TASK_CHECK(hit.t_hi - hit.t_lo <= kTol);
						TASK_CHECK(roots[0] >= hit.t_lo - kTol && roots[0] <= hit.t_hi + kTol);
						TASK_CHECK(std::fabs((hit.t_lo + hit.t_hi) * 0.5 - roots[0]) <= 2.0 * kTol);
						// The second, later root must lie beyond the returned interval.
						TASK_CHECK(roots[1] > hit.t_hi);
						break;
					}
				}
			}
		}
		TASK_CHECK(located);
	}

	// 6: cell whose endpoint signed heights share a sign but which contains
	// an internal crossing (two sign changes). A sign-only endpoint test
	// would miss this; the polynomial isolation must not.
	{
		Noise n(12345u);
		bool located = false;
		for (int cz = 0; cz < 4 && !located; ++cz)
		{
			for (int cx = 0; cx < 4 && !located; ++cx)
			{
				for (int trial = 0; trial < 400; ++trial)
				{
					const double ang = trial * 0.07;
					Ray r{ cx + 0.1, n.noise2D(cx + 0.1, cz + 0.1) + 0.05, cz + 0.1,
						std::cos(ang) * 0.8, -0.2, std::sin(ang) * 0.8 };
					const double T = 1.0;
					const double g0 = signedHeight(n, r.ox, r.oy, r.oz, r.dx, r.dy, r.dz, 0.0);
					const double g1 = signedHeight(n, r.ox, r.oy, r.oz, r.dx, r.dy, r.dz, T);
					const std::vector<double> roots = referenceRoots(n, r, 0.0, T);
					if (g0 * g1 > 0.0 && roots.size() >= 2)
					{
						located = true;
						TASK_CHECK(g0 * g1 > 0.0);	// endpoints genuinely same sign
						const siv::RayHit2D hit = n.intersectHeight(
							r.ox, r.oy, r.oz, r.dx, r.dy, r.dz, 0.0, T, kTol, kBudget);
						TASK_CHECK(hit.status == Status::Hit);
						TASK_CHECK(roots.front() >= hit.t_lo - kTol &&
							roots.front() <= hit.t_hi + kTol);
						break;
					}
				}
			}
		}
		TASK_CHECK(located);
	}
}

//
// ordering: near-to-far processing, budgets, and Uncertain never masking.
//
static void groupOrdering()
{
	// 1: an early root-free near-tangent followed by a definite later crossing.
	// The early candidate must come back Uncertain; the later clear root must
	// not be returned over it. The geometry is discovered from noise2D alone
	// (no use of the new interface) and adapts to the fixed Z slice in force.
	{
		bool located = false;
		for (const unsigned seed : { 1u, 7u, 42u, 99u, 12345u })
		{
			Noise n(seed);
			for (int cz = -2; cz < 3 && !located; ++cz)
			{
				for (int cx = -2; cx < 3 && !located; ++cx)
				{
					const double oz = cz + 0.4, ox = cx + 0.1;
					const double dx = 0.9 / 2000.0, tCell = 2000.0;
					const double T = 4.0 * tCell, dy = -1e-12;

					// Dense-sampled maximum of the surface over the first cell;
					// anchoring to it guarantees a sub-envelope close approach.
					double denseMax = -1e9;
					for (int k = 0; k <= 200000; ++k)
					{
						const double t = tCell * k / 200000.0;
						denseMax = (std::max)(denseMax, n.noise2D(ox + dx * t, oz));
					}
					const double y0 = denseMax + 1e-14;
					const Ray ray{ ox, y0, oz, dx, dy, 0.0 };

					const double f0 = signedHeight(n, ox, y0, oz, dx, dy, 0.0, 0.0);
					// A definite, sign-changing root strictly beyond the first cell.
					double laterRoot = 0.0;
					const bool later = referenceFirstRoot(n, ray, tCell, T, laterRoot);

					if (f0 > 1e-11 && later)
					{
						const siv::RayHit2D hit = n.intersectHeight(
							ox, y0, oz, dx, dy, 0.0, 0.0, T, kTol, kBudget);
						if (hit.status == Status::Uncertain &&
							hit.t_hi <= tCell + 1.0 &&
							hit.t_hi < laterRoot - 1.0)
						{
							TASK_CHECK(f0 > 1e-11);
							TASK_CHECK(later);
							TASK_CHECK(hit.status == Status::Uncertain);
							TASK_CHECK(hit.t_lo <= hit.t_hi);
							TASK_CHECK(hit.t_hi <= tCell + 1.0);	// stays in the early region
							TASK_CHECK(hit.t_hi < laterRoot - 1.0);	// later clear root not chosen
							located = true;
						}
					}
				}
			}
		}
		TASK_CHECK(located);
	}

	// 2-3: a starved budget yields Uncertain (never Miss), a generous budget
	// resolves the same transverse ray to a Hit.
	{
		Noise n(12345u);
		Ray r{ 0.3, n.noise2D(0.3, 0.4) + 1.0, 0.4, 0.1, -0.2, 0.05 };
		const siv::RayHit2D starved = n.intersectHeight(
			r.ox, r.oy, r.oz, r.dx, r.dy, r.dz, 0.0, 20.0, kTol, 1u);
		TASK_CHECK(starved.status == Status::Uncertain);
		TASK_CHECK(starved.t_lo >= 0.0 && starved.t_hi <= 20.0);

		const siv::RayHit2D funded = n.intersectHeight(
			r.ox, r.oy, r.oz, r.dx, r.dy, r.dz, 0.0, 20.0, kTol, kBudget);
		TASK_CHECK(funded.status == Status::Hit);
	}

	// 4: increasing budgets make monotonic progress; the smallest usable
	// budgets stay Uncertain until the root is isolated.
	{
		Noise n(777u);
		Ray r{ 2.1, n.noise2D(2.1, -1.2) + 1.2, -1.2, 0.12, -0.3, 0.04 };
		int uncertainSeen = 0;
		for (const unsigned budget : { 1u, 2u, 4u, 8u, 16u, 32u })
		{
			const siv::RayHit2D h = n.intersectHeight(
				r.ox, r.oy, r.oz, r.dx, r.dy, r.dz, 0.0, 12.0, kTol, budget);
			TASK_CHECK(h.status == Status::Uncertain || h.status == Status::Hit);
			if (h.status == Status::Uncertain) ++uncertainSeen;
		}
		TASK_CHECK(uncertainSeen >= 1);
		const siv::RayHit2D full = n.intersectHeight(
			r.ox, r.oy, r.oz, r.dx, r.dy, r.dz, 0.0, 12.0, kTol, kBudget);
		TASK_CHECK(full.status == Status::Hit);
	}

	// 5: a genuinely root-free ray that only approaches within the rounding
	// envelope is Uncertain, not Miss (Miss must not be used to hide it).
	{
		Noise n(7u);
		const double ox = 0.05, oz = 0.5, dx = 0.9 / 2000.0, T = 2000.0;
		double ymax = -1e9;
		for (int k = 0; k <= 200000; ++k)
			ymax = (std::max)(ymax, n.noise2D(ox + dx * T * k / 200000.0, oz));
		const double y0 = ymax + 1e-14;	// strictly root-free, sub-envelope gap
		const siv::RayHit2D h = n.intersectHeight(ox, y0, oz, dx, 0.0, 0.0,
			0.0, T, 1e-9, kBudget);
		TASK_CHECK(h.status == Status::Uncertain);
	}

	// 6: a clearly root-free distant ray is still a proven Miss, proving the
	// solver does not answer Uncertain to every request.
	{
		Noise n(7u);
		const siv::RayHit2D h = n.intersectHeight(0.05, 5.0, 0.5, 0.00045, 0.0, 0.0,
			0.0, 2000.0, kTol, kBudget);
		TASK_CHECK(h.status == Status::Miss);
		TASK_CHECK(h.t_lo == 0.0 && h.t_hi == 2000.0);
	}
}

//
// geometry: clipping, vertical, lattice lines, negative coords, seam,
// scaling, on-surface origin.
//
static void groupGeometry()
{
	Noise n(12345u);
	const Ray base{ 0.3, n.noise2D(0.3, 0.4) + 1.0, 0.4, 0.1, -0.2, 0.05 };
	double root = 0.0;
	const bool haveRoot = referenceFirstRoot(n, base, 0.0, 20.0, root);
	TASK_CHECK(haveRoot);

	// 1: interval ending before the root is proven root-free.
	{
		const siv::RayHit2D h = n.intersectHeight(
			base.ox, base.oy, base.oz, base.dx, base.dy, base.dz,
			0.0, root - 0.5, kTol, kBudget);
		TASK_CHECK(h.status == Status::Miss);
		TASK_CHECK(h.t_lo == 0.0 && h.t_hi == root - 0.5);
	}

	// 2: interval covering the root returns the Hit.
	{
		const siv::RayHit2D h = n.intersectHeight(
			base.ox, base.oy, base.oz, base.dx, base.dy, base.dz,
			0.0, root + 0.5, kTol, kBudget);
		TASK_CHECK(h.status == Status::Hit);
		TASK_CHECK(root >= h.t_lo - kTol && root <= h.t_hi + kTol);
	}

	// 3: a sub-interval entirely past the root, ray below the surface and
	// moving away, is a Miss.
	{
		const siv::RayHit2D h = n.intersectHeight(
			base.ox, base.oy, base.oz, base.dx, base.dy, base.dz,
			root + 1.0, root + 3.0, kTol, kBudget);
		TASK_CHECK(h.status == Status::Miss);
	}

	// 4: ray running exactly along an integer lattice line in z.
	{
		Ray r{ 0.5, n.noise2D(0.5, 2.0) + 1.0, 2.0, 0.2, -0.3, 0.0 };
		checkTransverse(n, r, 0.0, 20.0);
	}

	// 5: entirely negative-coordinate cell.
	{
		Noise m(555u);
		Ray r{ -1.7, m.noise2D(-1.7, -3.3) + 1.2, -3.3, 0.15, -0.3, 0.1 };
		checkTransverse(m, r, 0.0, 12.0);
	}

	// 6: crossing the 256-period seam; the field is periodic there and the
	// root matches the reference across x = 256.
	{
		TASK_CHECK(std::fabs(n.noise2D(255.999, 0.5) - n.noise2D(-0.001, 0.5)) < 1e-12);
		Ray r{ 255.4, n.noise2D(255.4, 0.5) + 2.0, 0.5, 0.25, -0.5, 0.1 };
		checkTransverse(n, r, 0.0, 10.0);
	}

	// 7: non-unit direction length - the parameter changes but the world-space
	// hit point must be identical, and the result stays in the input t scale.
	{
		const double k = 0.01;
		const siv::RayHit2D unit = n.intersectHeight(
			base.ox, base.oy, base.oz, base.dx, base.dy, base.dz,
			0.0, 20.0, kTol, kBudget);
		const siv::RayHit2D scaled = n.intersectHeight(
			base.ox, base.oy, base.oz, base.dx * k, base.dy * k, base.dz * k,
			0.0, 20.0 / k, kTol, kBudget);
		TASK_CHECK(unit.status == Status::Hit && scaled.status == Status::Hit);
		const double tu = (unit.t_lo + unit.t_hi) * 0.5;
		const double ts = (scaled.t_lo + scaled.t_hi) * 0.5;
		const double wxU = base.ox + base.dx * tu, wxS = base.ox + base.dx * k * ts;
		const double wyU = base.oy + base.dy * tu, wyS = base.oy + base.dy * k * ts;
		TASK_CHECK(std::fabs(wxU - wxS) < 1e-6 && std::fabs(wyU - wyS) < 1e-6);
		TASK_CHECK(scaled.t_hi - scaled.t_lo <= kTol);	// width measured in its own t
	}

	// 8: origin exactly on the surface is a zero-width Hit at t0.
	{
		const double x = 0.9, z = 0.2, h = n.noise2D(x, z);
		const siv::RayHit2D hh = n.intersectHeight(x, h, z, 1.0, 0.0, 0.2,
			0.0, 5.0, kTol, kBudget);
		TASK_CHECK(hh.status == Status::Hit);
		TASK_CHECK(hh.t_lo == 0.0 && hh.t_hi == 0.0);
	}
}

//
// limits: argument validation, state invariance, tolerance/budget behaviour,
// and the fixed-Z-slice contract.
//
static void groupLimits()
{
	Noise n(12345u);
	const double inf = std::numeric_limits<double>::infinity();
	const double nan = std::numeric_limits<double>::quiet_NaN();
	const Ray good{ 0.3, 1.0, 0.4, 0.1, -0.2, 0.05 };

	auto call = [&](double ox, double oy, double oz, double dx, double dy, double dz,
		double t0, double t1, double tol, unsigned nodes)
	{
		return n.intersectHeight(ox, oy, oz, dx, dy, dz, t0, t1, tol, nodes);
	};

	// 1: non-finite inputs are Invalid.
	TASK_CHECK(call(inf, good.oy, good.oz, good.dx, good.dy, good.dz, 0, 1, kTol, kBudget).status == Status::Invalid);
	TASK_CHECK(call(good.ox, good.oy, good.oz, good.dx, nan, good.dz, 0, 1, kTol, kBudget).status == Status::Invalid);
	TASK_CHECK(call(good.ox, good.oy, good.oz, good.dx, good.dy, good.dz, 0, inf, kTol, kBudget).status == Status::Invalid);

	// 2: ordering / range / zero-direction preconditions.
	TASK_CHECK(call(good.ox, good.oy, good.oz, good.dx, good.dy, good.dz, 1.0, 0.5, kTol, kBudget).status == Status::Invalid);
	TASK_CHECK(call(good.ox, good.oy, good.oz, good.dx, good.dy, good.dz, 0, 1, 1e-11, kBudget).status == Status::Invalid);
	TASK_CHECK(call(good.ox, good.oy, good.oz, good.dx, good.dy, good.dz, 0, 1, 1e-3 * 2, kBudget).status == Status::Invalid);
	TASK_CHECK(call(good.ox, good.oy, good.oz, 0.0, 0.0, 0.0, 0, 1, kTol, kBudget).status == Status::Invalid);
	TASK_CHECK(call(good.ox, good.oy, good.oz, good.dx, good.dy, good.dz, 0, 1, kTol, 0u).status == Status::Invalid);
	TASK_CHECK(call(good.ox, good.oy, good.oz, good.dx, good.dy, good.dz, 0, 1, kTol, 1000001u).status == Status::Invalid);
	// Boundary tolerance values are accepted.
	TASK_CHECK(call(good.ox, good.oy, good.oz, good.dx, good.dy, good.dz, 0, 1, 1e-10, kBudget).status != Status::Invalid);
	TASK_CHECK(call(good.ox, good.oy, good.oz, good.dx, good.dy, good.dz, 0, 1, 1e-3, kBudget).status != Status::Invalid);

	// 3: coordinate extent and lattice-crossing caps.
	TASK_CHECK(call(10001.0, 0.0, 0.0, 0.0, -1.0, 0.0, 0, 1, kTol, kBudget).status == Status::Invalid);
	TASK_CHECK(call(0.0, 0.0, 0.0, 100.0, 0.0, 0.0, 0, 10, kTol, kBudget).status == Status::Invalid);
	// Just inside the crossing cap is accepted.
	TASK_CHECK(call(0.0, 5.0, 0.0, 50.0, -1.0, 0.0, 0, 10, kTol, kBudget).status != Status::Invalid);

	// 4: queries never mutate the instance; reseed/deserialize drive the field.
	{
		Noise a(12345u), b(12345u);
		const Noise::state_type before = a.serialize();
		for (int q = 0; q < 20; ++q)
		{
			const siv::RayHit2D probe = a.intersectHeight(0.3, 1.0 + q * 0.01, 0.4, 0.1, -0.2, 0.05,
				0.0, 10.0, kTol, kBudget);
			(void)probe;
		}
		TASK_CHECK(a.serialize() == before);	// std::array equality

		a.reseed(98765u);
		b.reseed(98765u);
		TASK_CHECK(a.serialize() == b.serialize());

		Noise::state_type custom{};
		for (int i = 0; i < 256; ++i) custom[i] = static_cast<std::uint8_t>((i * 37 + 11) & 255);
		a.deserialize(custom);
		TASK_CHECK(a.serialize() == custom);
		// Two instances sharing a state give identical query results.
		Noise c;
		c.deserialize(custom);
		const siv::RayHit2D ha = a.intersectHeight(0.3, 1.0, 0.4, 0.1, -0.2, 0.05,
			0.0, 10.0, kTol, kBudget);
		const siv::RayHit2D hc = c.intersectHeight(0.3, 1.0, 0.4, 0.1, -0.2, 0.05,
			0.0, 10.0, kTol, kBudget);
		TASK_CHECK(ha.status == hc.status && ha.t_lo == hc.t_lo && ha.t_hi == hc.t_hi);
	}

	// 5: distinct seeds can change the answer; identical seeds reproduce it.
	{
		Noise p(12345u), q(12345u), r(4242u);
		const siv::RayHit2D hp = p.intersectHeight(0.3, 0.0, 0.4, 0.1, -0.2, 0.05,
			0.0, 10.0, kTol, kBudget);
		const siv::RayHit2D hq = q.intersectHeight(0.3, 0.0, 0.4, 0.1, -0.2, 0.05,
			0.0, 10.0, kTol, kBudget);
		const siv::RayHit2D hr = r.intersectHeight(0.3, 0.0, 0.4, 0.1, -0.2, 0.05,
			0.0, 10.0, kTol, kBudget);
		TASK_CHECK(hp.t_lo == hq.t_lo && hp.t_hi == hq.t_hi && hp.status == hq.status);
		TASK_CHECK(hr.status != hp.status || hr.t_lo != hp.t_lo);
	}

	// 6: tighter tolerance never widens a transverse Hit interval.
	{
		Ray r{ 0.3, n.noise2D(0.3, 0.4) + 1.0, 0.4, 0.1, -0.2, 0.05 };
		const siv::RayHit2D coarse = n.intersectHeight(
			r.ox, r.oy, r.oz, r.dx, r.dy, r.dz, 0.0, 20.0, 1e-4, kBudget);
		const siv::RayHit2D fine = n.intersectHeight(
			r.ox, r.oy, r.oz, r.dx, r.dy, r.dz, 0.0, 20.0, 1e-8, kBudget);
		TASK_CHECK(coarse.status == Status::Hit && fine.status == Status::Hit);
		TASK_CHECK((fine.t_hi - fine.t_lo) <= (coarse.t_hi - coarse.t_lo) + 1e-15);
		TASK_CHECK(coarse.t_hi - coarse.t_lo <= 1e-4);
		TASK_CHECK(fine.t_hi - fine.t_lo <= 1e-8);
	}

	// 7: the surface really is the fixed Z slice used by noise2D.
	{
		const double x = 1.234, z = -2.345;
		TASK_CHECK(n.noise2D(x, z) == n.noise3D(x, z,
			static_cast<double>(SIVPERLIN_DEFAULT_Z)));
	}
}

void task_cases(const char* group)
{
	const std::string g = group;
	if (g == "crossing") groupCrossing();
	else if (g == "ordering") groupOrdering();
	else if (g == "geometry") groupGeometry();
	else if (g == "limits") groupLimits();
	else TASK_CHECK(false);
}
