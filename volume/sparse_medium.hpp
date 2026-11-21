#pragma once

//-----------------------------------------------------------------------------
// Read-only sparse microflake volume on a regular integer grid.
//
// The medium independently owns its cell data and is read-only after
// construction. Construction validates every input completely before the
// data is published; invalid data throws std::invalid_argument.
//
// Grid conventions:
//   - World cell of integer key k is the half-open box
//     origin + [k, k+1) * cellSize on every axis. A point exactly on a face
//     belongs to the cell on the positive side. Rays starting on a face,
//     axis-parallel rays and equal-distance multi-axis crossings are all
//     resolved by exact integer stepping; no epsilon is used to skip thin
//     segments (zero-length segments simply contribute nothing).
//   - Cells not listed at construction are vacuum and contribute no optical
//     depth.
//
// Query conventions:
//   - Rays are finite windows o + t d, t in [t0, t1]; d may be non-unit but
//     must be non-zero; the window must be finite with 0 <= t0 <= t1.
//   - Extinction per unit world length inside a cell is rho * sigma(d/|d|).
//   - Returned collision distances are expressed in the original ray
//     parameter t.
//   - A first-collision query consumes one exponential optical depth
//     -log(1-u) along the whole valid segment; crossing cells or vacuum
//     never resamples. If the depth lands exactly on the end of an interval
//     the next positive-length interval decides; if it lands on the end of
//     the whole valid window the ray escapes.
//-----------------------------------------------------------------------------

#include "sggx.hpp"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace microflake {

	// Actual counters of a traversal: coarse block tests and fine cell tests.
	struct TraversalStats {

		std::uint64_t nodeTests = 0u;
		std::uint64_t cellTests = 0u;
	};

	class SparseMedium {

	public:

		struct CellInput {

			std::int32_t kx, ky, kz;           // integer cell key
			double rho;                        // >= 0, finite
			double ax, ay, az;                 // RGB albedo in [0,1]
			double sxx, syy, szz, sxy, sxz, syz; // SPD, eigenvalues in [0.01, 100]
		};

		struct Cell {

			std::int32_t kx, ky, kz;
			double rho;
			Vec3 albedo;
			Sggx sggx;
		};

		// One occupied cell interval in ray parameter order.
		struct Interval {

			double t0, t1;              // clipped to the query window
			std::int32_t kx, ky, kz;
			std::size_t cell;           // index into CellAt()
		};

		struct Collision {

			double t;                   // ray parameter of the collision
			std::size_t cell;           // index into CellAt()
		};

		// Validates all inputs before publishing; throws std::invalid_argument
		// on non-finite values, non-positive cell size, negative rho, albedo
		// outside [0,1], non-SPD tensors, tensor eigenvalues outside
		// [0.01, 100], or duplicate keys.
		SparseMedium(const Vec3& origin, double cellSize, const std::vector<CellInput>& cells);
		SparseMedium(SparseMedium&&) noexcept = default;
		SparseMedium& operator=(SparseMedium&&) noexcept = default;
		SparseMedium(const SparseMedium&) = delete;
		SparseMedium& operator=(const SparseMedium&) = delete;
		~SparseMedium() = default;

		[[nodiscard]]
		std::size_t CellCount() const noexcept {
			return m_cells.size();
		}

		// True if any cell has rho > 0.
		[[nodiscard]]
		bool HasExtinction() const noexcept {
			return m_has_extinction;
		}

		[[nodiscard]]
		const Cell& CellAt(std::size_t i) const {
			return m_cells[i];
		}

		// Integrated extinction (optical depth) of the ray segment.
		[[nodiscard]]
		double OpticalDepth(const Vec3& o, const Vec3& d, double t0, double t1,
		                    TraversalStats* stats = nullptr) const;

		// Analytic transmittance exp(-optical depth) of the ray segment.
		[[nodiscard]]
		double Transmittance(const Vec3& o, const Vec3& d, double t0, double t1,
		                     TraversalStats* stats = nullptr) const;

		// First real collision for one exponential optical depth -log(1-u),
		// u in [0,1). Returns false on escape through the window end.
		[[nodiscard]]
		bool FirstCollision(const Vec3& o, const Vec3& d, double t0, double t1,
		                    double u, Collision& out,
		                    TraversalStats* stats = nullptr) const;

		// Diagnostic: occupied cell intervals in increasing ray parameter.
		[[nodiscard]]
		std::vector<Interval> OccupiedIntervals(const Vec3& o, const Vec3& d,
		                                        double t0, double t1,
		                                        TraversalStats* stats = nullptr) const;

	private:

		// visit(cellIndex, ta, tb) -> bool (false stops the traversal early).
		template< typename F >
		void Traverse(const Vec3& o, const Vec3& d, double t0, double t1,
		              TraversalStats* stats, F&& visit) const;

		Vec3 m_origin;
		double m_cellSize;
		std::vector<Cell> m_cells;

		struct GridKey {

			std::int32_t x, y, z;

			[[nodiscard]]
			bool operator==(const GridKey& rhs) const noexcept {
				return x == rhs.x && y == rhs.y && z == rhs.z;
			}
		};

		struct GridKeyHash {

			[[nodiscard]]
			std::size_t operator()(const GridKey& k) const noexcept {
				std::uint64_t h = 0xcbf29ce484222325ULL;
				const std::uint32_t v[3] = {
					(std::uint32_t)k.x, (std::uint32_t)k.y, (std::uint32_t)k.z
				};
				for (const std::uint32_t w : v) {
					h ^= w;
					h *= 0x100000001b3ULL;
					h ^= h >> 29U;
					h *= 0x9e3779b97f4a7c15ULL;
					h ^= h >> 32U;
				}
				return (std::size_t)h;
			}
		};

		std::unordered_map< GridKey, std::uint32_t, GridKeyHash > m_cell_index;
		std::unordered_set< GridKey, GridKeyHash > m_blocks; // 16^3 cell blocks
		double m_bmin[3]; // world bounds of occupied blocks (padded by 1 ulp)
		double m_bmax[3];
		bool m_has_extinction;
	};
}
