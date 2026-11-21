#pragma once

//-----------------------------------------------------------------------------
// Integration of the sparse SGGX microflake medium with the unmodified
// sequential cpp-smallpt production translation unit.
//
// The original Radiance(ray, rng) entry point and the original command keep
// using the original surface-only branch. The new overload
// Radiance(ray, rng, medium, stats) lets every camera, diffuse, specular and
// refractive successor ray compete a real volume event against the nearest
// surface:
//   - A volume event happens only where the sampled exponential optical depth
//     is consumed; only then is the throughput multiplied by the cell albedo
//     a, and the path continues along a visible-microflake reflection
//     direction. No Beer transmittance is multiplied when the ray reaches a
//     surface without a collision.
//   - A volume event increments the path depth and participates in Russian
//     roulette; crossing cell boundaries does not increment the depth.
//   - In non-zero medium mode roulette uses the maximum channel of the
//     actual accumulated throughput, clamped to [0,1], with the usual
//     compensation.
//   - If the medium is null, empty, or has only rho == 0 cells, the call
//     forwards to the original Radiance: colors, the RNG stream and sphere
//     intersection behavior are preserved. Rays whose segment crosses only
//     vacuum also consume no random numbers.
//-----------------------------------------------------------------------------

#include "task_support/surface_access.hpp"
#include "volume/sggx.hpp"
#include "volume/sparse_medium.hpp"

#include <algorithm>
#include <cstdint>

namespace smallpt {

	struct MediumRenderStats {

		std::uint64_t mediumQueries = 0u;     // segments that competed a volume event
		std::uint64_t volumeCollisions = 0u;  // real volume scattering events
		std::uint64_t nodeTests = 0u;         // coarse block tests
		std::uint64_t cellTests = 0u;         // fine cell tests
		std::uint32_t maxQueryDepth = 0u;     // deepest ray depth that queried the medium
		std::uint32_t maxCollisionDepth = 0u; // deepest ray depth that collided
	};

	[[nodiscard]]
	inline microflake::Vec3 ToMicroflake(const Vector3& v) noexcept {
		return { v.m_x, v.m_y, v.m_z };
	}

	[[nodiscard]]
	inline Vector3 ToSmallpt(const microflake::Vec3& v) noexcept {
		return { v.x, v.y, v.z };
	}

	[[nodiscard]]
	static const Vector3 Radiance(const Ray& ray, RNG& rng,
	                              const microflake::SparseMedium* medium,
	                              MediumRenderStats* stats) {
		if (nullptr == medium || !medium->HasExtinction()) {
			// Empty or zero-extinction medium: identical colors, RNG stream
			// and sphere intersection behavior.
			return Radiance(ray, rng);
		}

		constexpr double kMediumFar = 1e30; // finite window when no surface is hit

		Ray r = ray;
		Vector3 L;
		Vector3 F(1.0);

		while (true) {
			const auto hit = Intersect(r);
			const double t_surface = hit ? r.m_tmax : kMediumFar;

			// Compete a real volume event against the nearest surface.
			if (stats) {
				++stats->mediumQueries;
				stats->maxQueryDepth = std::max(stats->maxQueryDepth, r.m_depth);
			}
			microflake::TraversalStats ts;
			const double tau = medium->OpticalDepth(ToMicroflake(r.m_o), ToMicroflake(r.m_d),
			                                        r.m_tmin, t_surface,
			                                        stats ? &ts : nullptr);
			if (stats) {
				stats->nodeTests += ts.nodeTests;
				stats->cellTests += ts.cellTests;
			}
			if (tau > 0.0) {
				// The segment crosses some extinction: draw one exponential
				// optical depth for the whole segment.
				const double u = rng.Uniform();
				microflake::SparseMedium::Collision collision;
				microflake::TraversalStats ts2;
				const bool volume_hit =
					medium->FirstCollision(ToMicroflake(r.m_o), ToMicroflake(r.m_d),
					                       r.m_tmin, t_surface, u, collision,
					                       stats ? &ts2 : nullptr);
				if (stats) {
					stats->nodeTests += ts2.nodeTests;
					stats->cellTests += ts2.cellTests;
				}
				if (volume_hit) {
					const auto& cell = medium->CellAt(collision.cell);
					if (stats) {
						++stats->volumeCollisions;
						stats->maxCollisionDepth = std::max(stats->maxCollisionDepth, r.m_depth);
					}

					// The albedo acts only at a real collision.
					F *= Vector3(cell.albedo.x, cell.albedo.y, cell.albedo.z);

					// Russian roulette on the actual accumulated throughput.
					if (4u < r.m_depth) {
						const double continue_probability = std::clamp(F.Max(), 0.0, 1.0);
						if (rng.Uniform() >= continue_probability) {
							return L;
						}
						F /= continue_probability;
					}

					// Continue along a visible-microflake reflection direction.
					double pdf;
					const microflake::Vec3 wo =
						cell.sggx.SampleReflection(ToMicroflake(-r.m_d),
						                           rng.Uniform(), rng.Uniform(), pdf);
					r = Ray(r(collision.t), ToSmallpt(wo), EPSILON_SPHERE,
					        INFINITY, r.m_depth + 1u);
					continue;
				}
			}

			if (!hit) {
				return L;
			}

			// Original surface branch (no Beer transmittance is multiplied
			// when the ray reaches the surface without a collision).
			const Sphere& shape = g_spheres[hit.value()];
			const Vector3 p = r(r.m_tmax);
			const Vector3 n = Normalize(p - shape.m_p);

			L += F * shape.m_e;
			F *= shape.m_f;

			// Russian roulette on the actual accumulated throughput.
			if (4u < r.m_depth) {
				const double continue_probability = std::clamp(F.Max(), 0.0, 1.0);
				if (rng.Uniform() >= continue_probability) {
					return L;
				}
				F /= continue_probability;
			}

			// Next path segment
			switch (shape.m_reflection_t) {

			case Reflection_t::Specular: {
				const Vector3 d = IdealSpecularReflect(r.m_d, n);
				r = Ray(p, d, EPSILON_SPHERE, INFINITY, r.m_depth + 1u);
				break;
			}

			case Reflection_t::Refractive: {
				double pr;
				const Vector3 d = IdealSpecularTransmit(r.m_d, n, REFRACTIVE_INDEX_OUT, REFRACTIVE_INDEX_IN, pr, rng);
				F *= pr;
				r = Ray(p, d, EPSILON_SPHERE, INFINITY, r.m_depth + 1u);
				break;
			}

			default: {
				const Vector3 w = (0.0 > n.Dot(r.m_d)) ? n : -n;
				const Vector3 u = Normalize((std::abs(w.m_x) > 0.1 ? Vector3(0.0, 1.0, 0.0) : Vector3(1.0, 0.0, 0.0)).Cross(w));
				const Vector3 v = w.Cross(u);

				const Vector3 sample_d = CosineWeightedSampleOnHemisphere(rng.Uniform(), rng.Uniform());
				const Vector3 d = Normalize(sample_d.m_x * u + sample_d.m_y * v + sample_d.m_z * w);
				r = Ray(p, d, EPSILON_SPHERE, INFINITY, r.m_depth + 1u);
				break;
			}

			}
		}
	}
}
