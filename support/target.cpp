#include "check.h"
#include "PerlinNoise.hpp"

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace
{
	using siv::PerlinNoise;
	using siv::RayHit2D;
	using Status = RayHit2D::Status;

	constexpr double kTol = 1e-7;
	constexpr unsigned kBudget = 1'000'000u;

	// Independent reference: a dense sign scan followed by bisection, built
	// only on the scalar noise2D surface. It never calls intersectHeight, so
	// the new interface cannot manufacture its own expected answer.
	std::vector<double> referenceRoots(const PerlinNoise& n,
		double ox, double oy, double oz,
		double dx, double dy, double dz,
		double t0, double t1, int samples = 60000)
	{
		auto f = [&](double t) {
			return (oy + dy * t) - n.noise2D(ox + dx * t, oz + dz * t);
		};
		std::vector<double> roots;
		double prev = f(t0);
		for (int k = 1; k <= samples; ++k)
		{
			const double a = t0 + (t1 - t0) * (k - 1) / samples;
			const double b = t0 + (t1 - t0) * k / samples;
			const double cur = f(b);
			if ((prev < 0.0 && cur > 0.0) || (prev > 0.0 && cur < 0.0))
			{
				double lo = a, hi = b;
				for (int it = 0; it < 120; ++it)
				{
					const double m = 0.5 * (lo + hi);
					const double fm = f(m);
					if ((f(lo) < 0.0 && fm > 0.0) || (f(lo) > 0.0 && fm < 0.0))
						hi = m;
					else
						lo = m;
				}
				roots.push_back(0.5 * (lo + hi));
			}
			prev = cur;
		}
		return roots;
	}

	double heightResidual(const PerlinNoise& n, const RayHit2D& hit,
		double ox, double oy, double oz,
		double dx, double dy, double dz)
	{
		const double t = 0.5 * (hit.t_lo + hit.t_hi);
		return std::abs((oy + dy * t) - n.noise2D(ox + dx * t, oz + dz * t));
	}

	// A simple transverse crossing must land within 2*tolerance of the
	// independent earliest root, with a small height residual and a
	// contract-conformant interval.
	void checkSimpleCrossing(const PerlinNoise& n,
		double ox, double oy, double oz,
		double dx, double dy, double dz,
		double t0, double t1, double tolerance)
	{
		const std::vector<double> roots = referenceRoots(n, ox, oy, oz, dx, dy, dz, t0, t1);
		TASK_CHECK(!roots.empty());
		const RayHit2D hit = n.intersectHeight(ox, oy, oz, dx, dy, dz, t0, t1, tolerance, kBudget);
		TASK_CHECK(hit.status == Status::Hit);
		TASK_CHECK(hit.t_lo >= t0 - tolerance && hit.t_hi <= t1 + tolerance);
		TASK_CHECK(hit.t_hi - hit.t_lo <= tolerance);
		const double tm = 0.5 * (hit.t_lo + hit.t_hi);
		TASK_CHECK(std::abs(tm - roots.front()) <= 2.0 * tolerance);
		TASK_CHECK(heightResidual(n, hit, ox, oy, oz, dx, dy, dz) <= 1e-5);
	}
}

//
// crossing: basic transverse crossings against the independent reference,
// including a deterministic one-cell / two-root sample whose cell endpoints
// have the same sign.
//
void task_cases_crossing()
{
	{
		// Vertical ray.
		const PerlinNoise n(12345u);
		const double x = 1.3, z = 2.7;
		checkSimpleCrossing(n, x, 2.0, z, 0.0, -1.0, 0.0, 0.0, 4.0, kTol);
	}
	{
		// Oblique ray through negative coordinates and several grid lines.
		const PerlinNoise n(12345u);
		checkSimpleCrossing(n, -3.7, 1.5, -1.2, 0.4, -0.8, 0.3, 0.0, 4.0, kTol);
	}
	{
		// Both horizontal directions negative.
		const PerlinNoise n(7u);
		checkSimpleCrossing(n, 4.2, 1.2, 3.1, -0.9, -0.7, -0.6, 0.0, 3.0, kTol);
	}
	{
		// A different seed still agrees with the reference.
		const PerlinNoise n(98765u);
		checkSimpleCrossing(n, 2.1, 1.4, -1.7, 0.5, -0.9, -0.2, 0.0, 3.5, kTol);
	}
	{
		// Deterministic one-cell, two-root sample: with a fixed permutation
		// and a fixed RNG, search for a ray confined to one cell whose
		// independent scan finds two sign changes while the cell endpoint
		// values share one sign. Endpoint signs alone therefore cannot prove
		// a Miss, and the interface must still return the earliest root.
		// The search adapts to whichever fixed Z slice is compiled in.
		const PerlinNoise n(1u);
		std::mt19937 rng(123);
		bool found = false;
		for (int trial = 0; trial < 400000 && !found; ++trial)
		{
			const std::int64_t bx = static_cast<std::int64_t>(-20 + 40.0 * rng() / rng.max());
			const std::int64_t bz = static_cast<std::int64_t>(-20 + 40.0 * rng() / rng.max());
			const double ox = bx + 0.15 + 0.7 * rng() / rng.max();
			const double oz = bz + 0.15 + 0.7 * rng() / rng.max();
			const double oy = (-1.0 + 2.0 * rng() / rng.max()) * 0.8;
			const double dx = (-1.0 + 2.0 * rng() / rng.max()) * 0.8;
			const double dy = (-1.0 + 2.0 * rng() / rng.max()) * 0.8;
			const double dz = (-1.0 + 2.0 * rng() / rng.max()) * 0.8;
			const double T = 0.9;
			if (ox + dx * T < bx + 0.02 || ox + dx * T > bx + 0.98) continue;
			if (oz + dz * T < bz + 0.02 || oz + dz * T > bz + 0.98) continue;
			auto f = [&](double t) {
				return (oy + dy * t) - n.noise2D(ox + dx * t, oz + dz * t);
			};
			const std::vector<double> roots = referenceRoots(n, ox, oy, oz, dx, dy, dz, 0.0, T, 200000);
			if (roots.size() < 2) continue;
			if (f(0.0) * f(T) <= 0.0) continue; // require same-sign endpoints
			found = true;
			TASK_CHECK(f(0.0) * f(T) > 0.0);
			const RayHit2D hit = n.intersectHeight(ox, oy, oz, dx, dy, dz, 0.0, T, 1e-8, kBudget);
			TASK_CHECK(hit.status == Status::Hit);
			const double tm = 0.5 * (hit.t_lo + hit.t_hi);
			TASK_CHECK(std::abs(tm - roots.front()) <= 2e-8);
			TASK_CHECK(tm < roots[1] - 0.05); // earliest, not the second root
			TASK_CHECK(heightResidual(n, hit, ox, oy, oz, dx, dy, dz) <= 1e-7);
		}
		TASK_CHECK(found);
	}
	{
		// A ray launched exactly on a grid line.
		const PerlinNoise n(2024u);
		checkSimpleCrossing(n, 3.0, 1.6, -2.0, 0.3, -1.0, 0.5, 0.0, 3.0, kTol);
	}
}

//
// ordering: unresolved earlier candidates must shadow later definite roots;
// budgets and multiple roots must never be reordered or hidden as Miss.
//
void task_cases_ordering()
{
	const PerlinNoise n(12345u);

	// Deterministically find a multi-cell ray with at least two roots in
	// distinct cells. Adapts to the compiled fixed Z slice.
	std::mt19937 rng(99);
	double ox = 0, oy = 1.5, oz = 0, dx = 0, dy = -0.6, dz = 0, T = 4.0;
	std::vector<double> roots;
	bool found = false;
	for (int trial = 0; trial < 200000 && !found; ++trial)
	{
		ox = -8.0 + 16.0 * rng() / rng.max();
		oz = -8.0 + 16.0 * rng() / rng.max();
		dx = (-1.0 + 2.0 * rng() / rng.max()) * 3.0;
		dz = (-1.0 + 2.0 * rng() / rng.max()) * 3.0;
		oy = 1.5;
		if (std::abs(ox + dx * T) > 9999.0 || std::abs(oz + dz * T) > 9999.0) continue;
		roots = referenceRoots(n, ox, oy, oz, dx, dy, dz, 0.0, T, 40000);
		if (roots.size() < 2) continue;
		const long c0x = static_cast<long>(std::floor(ox + dx * roots[0]));
		const long c1x = static_cast<long>(std::floor(ox + dx * roots[1]));
		const long c0z = static_cast<long>(std::floor(oz + dz * roots[0]));
		const long c1z = static_cast<long>(std::floor(oz + dz * roots[1]));
		if (c0x != c1x || c0z != c1z) found = true;
	}
	TASK_CHECK(found);

	{
		// Full budget: earliest definite root.
		const RayHit2D full = n.intersectHeight(ox, oy, oz, dx, dy, dz, 0.0, T, 1e-8, kBudget);
		TASK_CHECK(full.status == Status::Hit);
		TASK_CHECK(std::abs(0.5 * (full.t_lo + full.t_hi) - roots.front()) <= 2e-8);
	}
	{
		// One-node budget: an earlier unresolved candidate is reported as
		// Uncertain and must not be skipped for the later definite root.
		const RayHit2D tight = n.intersectHeight(ox, oy, oz, dx, dy, dz, 0.0, T, 1e-8, 1u);
		TASK_CHECK(tight.status == Status::Uncertain);
		TASK_CHECK(tight.t_lo <= tight.t_hi);
		TASK_CHECK(tight.t_lo >= 0.0 && tight.t_hi <= T);
		TASK_CHECK(tight.t_hi < roots.front()); // shadows, never jumps past it
	}
	{
		// A growing budget must resolve the same earliest root; the answer
		// never regresses into a Miss.
		Status previous = Status::Uncertain;
		bool sawHit = false;
		for (unsigned budget : {2u, 8u, 64u, 4096u, 100000u})
		{
			const RayHit2D r = n.intersectHeight(ox, oy, oz, dx, dy, dz, 0.0, T, 1e-8, budget);
			TASK_CHECK(r.status != Status::Miss);
			if (r.status == Status::Hit)
			{
				sawHit = true;
				TASK_CHECK(std::abs(0.5 * (r.t_lo + r.t_hi) - roots.front()) <= 2e-8);
			}
			previous = r.status;
		}
		TASK_CHECK(sawHit);
		TASK_CHECK(previous == Status::Hit);
	}
	{
		// Root-free leading cells are skipped, then the later real root is
		// returned (vertical ray only meets the surface once).
		const double x = 1.3, z = 2.7;
		const RayHit2D r = n.intersectHeight(x, 2.0, z, 0.0, -1.0, 0.0, 0.0, 4.0, kTol, kBudget);
		TASK_CHECK(r.status == Status::Hit);
		TASK_CHECK(r.t_lo > 1.0);
	}
	{
		// An Uncertain interval stays on the original (unnormalized) t scale.
		const RayHit2D tight = n.intersectHeight(ox, oy, oz, dx, dy, dz, 0.0, T, 1e-8, 4u);
		TASK_CHECK(tight.status == Status::Uncertain);
		TASK_CHECK(tight.t_hi - tight.t_lo > 1e-8);
		TASK_CHECK(tight.t_hi <= T);
	}
}

//
// geometry: vertical rays, grid-line alignment, negative coordinates, the
// 256-period seam, origin-on-surface, horizontal crossings and clipping.
//
void task_cases_geometry()
{
	{
		// Pure vertical ray (no horizontal component at all).
		const PerlinNoise n(11u);
		const double x = -7.4, z = 9.1;
		const RayHit2D r = n.intersectHeight(x, 2.0, z, 0.0, -1.0, 0.0, 0.0, 4.0, kTol, kBudget);
		TASK_CHECK(r.status == Status::Hit);
		TASK_CHECK(heightResidual(n, r, x, 2.0, z, 0.0, -1.0, 0.0) <= 1e-6);
	}
	{
		// Origin exactly on the surface: zero-width Hit is allowed.
		const PerlinNoise n(11u);
		const double x = 0.4, z = 0.6, ys = n.noise2D(x, z);
		const RayHit2D r = n.intersectHeight(x, ys, z, 1.0, 0.0, 0.0, 0.0, 3.0, kTol, kBudget);
		TASK_CHECK(r.status == Status::Hit);
		TASK_CHECK(r.t_lo == 0.0 && r.t_hi == 0.0);
	}
	{
		// Negative-z direction starting on an integer z grid line.
		const PerlinNoise n(33u);
		checkSimpleCrossing(n, -2.5, 1.5, 4.0, 0.2, -0.9, -1.0, 0.0, 3.0, kTol);
	}
	{
		// 256-period seam: the field is periodic, so a translated ray hits at
		// the same parameter and leaves the same residual.
		const PerlinNoise n(55u);
		const double ox = 3.3, oy = 1.4, oz = -2.2;
		const double dx = 0.4, dy = -0.8, dz = 0.3;
		TASK_CHECK(std::abs(n.noise2D(ox, oz) - n.noise2D(ox + 256.0, oz - 256.0)) < 1e-12);
		const RayHit2D a = n.intersectHeight(ox, oy, oz, dx, dy, dz, 0.0, 4.0, kTol, kBudget);
		const RayHit2D b = n.intersectHeight(ox + 256.0, oy, oz - 256.0, dx, dy, dz, 0.0, 4.0, kTol, kBudget);
		TASK_CHECK(a.status == Status::Hit && b.status == Status::Hit);
		TASK_CHECK(std::abs(0.5 * (a.t_lo + a.t_hi) - 0.5 * (b.t_lo + b.t_hi)) <= 2.0 * kTol);
	}
	{
		// Horizontal ray (dy == 0): search for a genuine level crossing.
		const PerlinNoise n(77u);
		bool found = false;
		std::mt19937 rng(5);
		for (int trial = 0; trial < 4000 && !found; ++trial)
		{
			const double ox = -6.0 + 12.0 * rng() / rng.max();
			const double oz = -6.0 + 12.0 * rng() / rng.max();
			const double oy = -0.6 + 1.2 * rng() / rng.max();
			const double dx = -1.0 + 2.0 * rng() / rng.max();
			const double dz = -1.0 + 2.0 * rng() / rng.max();
			if (dx == 0.0 && dz == 0.0) continue;
			const std::vector<double> roots = referenceRoots(n, ox, oy, oz, dx, 0.0, dz, 0.0, 3.0, 20000);
			if (roots.empty()) continue;
			found = true;
			const RayHit2D r = n.intersectHeight(ox, oy, oz, dx, 0.0, dz, 0.0, 3.0, kTol, kBudget);
			TASK_CHECK(r.status == Status::Hit);
			TASK_CHECK(std::abs(0.5 * (r.t_lo + r.t_hi) - roots.front()) <= 2.0 * kTol);
			TASK_CHECK(heightResidual(n, r, ox, oy, oz, dx, 0.0, dz) <= 1e-6);
		}
		TASK_CHECK(found);
	}
	{
		// Interval clipping: above and below the unique vertical root are
		// Miss; a window containing the root is Hit.
		const PerlinNoise n(12345u);
		const double x = 1.3, z = 2.7;
		const RayHit2D above = n.intersectHeight(x, 2.0, z, 0.0, -1.0, 0.0, 0.0, 1.0, kTol, kBudget);
		const RayHit2D below = n.intersectHeight(x, 2.0, z, 0.0, -1.0, 0.0, 3.0, 4.0, kTol, kBudget);
		const RayHit2D inside = n.intersectHeight(x, 2.0, z, 0.0, -1.0, 0.0, 2.0, 2.1, kTol, kBudget);
		TASK_CHECK(above.status == Status::Miss);
		TASK_CHECK(below.status == Status::Miss);
		TASK_CHECK(inside.status == Status::Hit);
		TASK_CHECK(inside.t_lo >= 2.0 && inside.t_hi <= 2.1);
	}
}

//
// limits: validation, the 512 grid-line cap, budget/tolerance behaviour,
// proven Miss, and state immutability across queries and (de)serialization.
//
void task_cases_limits()
{
	const PerlinNoise n(12345u);
	{
		// Invalid argument matrix.
		const RayHit2D zeroDir = n.intersectHeight(0, 0, 0, 0, 0, 0, 0, 1, kTol, 100u);
		const RayHit2D badOrder = n.intersectHeight(0, 0, 0, 1, 0, 0, 2, 1, kTol, 100u);
		const RayHit2D tolLow = n.intersectHeight(0, 0, 0, 1, 0, 0, 0, 1, 5e-11, 100u);
		const RayHit2D tolHigh = n.intersectHeight(0, 0, 0, 1, 0, 0, 0, 1, 2e-3, 100u);
		const RayHit2D nodesZero = n.intersectHeight(0, 0, 0, 1, 0, 0, 0, 1, kTol, 0u);
		const RayHit2D nodesBig = n.intersectHeight(0, 0, 0, 1, 0, 0, 0, 1, kTol, 1'000'001u);
		const RayHit2D farEndpoint = n.intersectHeight(20000.0, 0, 0, 1, 0, 0, 0, 1, kTol, 100u);
		const RayHit2D nonFinite = n.intersectHeight(std::numeric_limits<double>::infinity(), 0, 0, 1, 0, 0, 0, 1, kTol, 100u);
		TASK_CHECK(zeroDir.status == Status::Invalid);
		TASK_CHECK(badOrder.status == Status::Invalid);
		TASK_CHECK(tolLow.status == Status::Invalid);
		TASK_CHECK(tolHigh.status == Status::Invalid);
		TASK_CHECK(nodesZero.status == Status::Invalid);
		TASK_CHECK(nodesBig.status == Status::Invalid);
		TASK_CHECK(farEndpoint.status == Status::Invalid);
		TASK_CHECK(nonFinite.status == Status::Invalid);
	}
	{
		// Grid-line budget cap of 512.
		const RayHit2D tooMany = n.intersectHeight(0.0, 1.0, 0.0, 1.0, -1.0, 0.0, 0.0, 600.0, kTol, kBudget);
		const RayHit2D within = n.intersectHeight(0.0, 1.0, 0.0, 1.0, -1.0, 0.0, 0.0, 100.0, kTol, kBudget);
		TASK_CHECK(tooMany.status == Status::Invalid);
		TASK_CHECK(within.status != Status::Invalid);
	}
	{
		// A ray proven to stay entirely above the surface is a Miss, never
		// an Uncertain, even with a generous budget.
		const RayHit2D miss = n.intersectHeight(1.3, 5.0, 2.7, 0.0, -1.0, 0.0, 0.0, 1.0, kTol, kBudget);
		TASK_CHECK(miss.status == Status::Miss);
		TASK_CHECK(miss.t_lo == 0.0 && miss.t_hi == 1.0);
	}
	{
		// Tightening the tolerance keeps a Hit whose width shrinks and whose
		// center stays within 2*tolerance of the independent root.
		const double x = 1.3, z = 2.7;
		const std::vector<double> roots = referenceRoots(n, x, 2.0, z, 0.0, -1.0, 0.0, 0.0, 4.0);
		const RayHit2D coarse = n.intersectHeight(x, 2.0, z, 0.0, -1.0, 0.0, 0.0, 4.0, 1e-5, kBudget);
		const RayHit2D fine = n.intersectHeight(x, 2.0, z, 0.0, -1.0, 0.0, 0.0, 4.0, 1e-9, kBudget);
		TASK_CHECK(coarse.status == Status::Hit && fine.status == Status::Hit);
		TASK_CHECK(coarse.t_hi - coarse.t_lo <= 1e-5);
		TASK_CHECK(fine.t_hi - fine.t_lo <= 1e-9);
		TASK_CHECK(std::abs(0.5 * (fine.t_lo + fine.t_hi) - roots.front()) <= 2e-9);
	}
	{
		// Queries do not mutate the permutation state; repeated calls agree.
		const auto before = n.serialize();
		RayHit2D first{};
		for (int k = 0; k < 64; ++k)
		{
			const RayHit2D r = n.intersectHeight(1.3, 2.0, 2.7, 0.0, -1.0, 0.0, 0.0, 4.0, kTol, 1000u + k);
			if (k == 0) first = r;
		}
		const auto after = n.serialize();
		TASK_CHECK(before == after);
		const RayHit2D again = n.intersectHeight(1.3, 2.0, 2.7, 0.0, -1.0, 0.0, 0.0, 4.0, kTol, 1100u);
		TASK_CHECK(again.status == first.status && again.t_lo == first.t_lo && again.t_hi == first.t_hi);
	}
	{
		// reseed / deserialize expose the real permutation to later queries.
		PerlinNoise a(42u), b;
		const auto saved = a.serialize();
		b.deserialize(saved);
		TASK_CHECK(a.serialize() == b.serialize());
		TASK_CHECK(a.noise2D(-3.1, 2.7) == b.noise2D(-3.1, 2.7));
		a.reseed(777u);
		PerlinNoise c(777u);
		TASK_CHECK(a.serialize() == c.serialize());
		const RayHit2D ra = a.intersectHeight(1.3, 2.0, 2.7, 0.0, -1.0, 0.0, 0.0, 4.0, kTol, kBudget);
		const RayHit2D rc = c.intersectHeight(1.3, 2.0, 2.7, 0.0, -1.0, 0.0, 0.0, 4.0, kTol, kBudget);
		TASK_CHECK(ra.status == rc.status && ra.t_lo == rc.t_lo && ra.t_hi == rc.t_hi);
	}
	{
		// An exhausted budget is Uncertain with a valid in-range candidate
		// interval, not a Miss.
		const RayHit2D tight = n.intersectHeight(-3.7, 1.5, -1.2, 0.4, -0.8, 0.3, 0.0, 4.0, kTol, 1u);
		TASK_CHECK(tight.status == Status::Uncertain);
		TASK_CHECK(tight.t_lo <= tight.t_hi && tight.t_lo >= 0.0 && tight.t_hi <= 4.0);
	}
}

void task_cases(const char* group)
{
	const std::string g = group;
	if (g == "crossing") task_cases_crossing();
	else if (g == "ordering") task_cases_ordering();
	else if (g == "geometry") task_cases_geometry();
	else if (g == "limits") task_cases_limits();
	else
	{
		std::fprintf(stderr, "unknown group %s\n", group);
		std::exit(2);
	}
}
