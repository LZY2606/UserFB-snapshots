#include "sparse_medium.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace microflake {

	namespace {

		constexpr int kBlockShift = 4;
		constexpr int kBlockSize  = 1 << kBlockShift; // cells per block axis

		[[nodiscard]]
		bool IsFinite(const Vec3& v) noexcept {
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
		}

		// Floor division of a cell key to its block key; exact for int32.
		[[nodiscard]]
		std::int32_t BlockOf(std::int32_t k) noexcept {
			return (std::int32_t)std::floor((double)k / (double)kBlockSize);
		}

		void CheckQuery(const Vec3& o, const Vec3& d, double t0, double t1) {
			if (!IsFinite(o)) {
				throw std::invalid_argument("SparseMedium: non-finite ray origin");
			}
			if (!IsFinite(d) || !(Norm2(d) > 0.0)) {
				throw std::invalid_argument("SparseMedium: ray direction must be finite and non-zero");
			}
			if (!std::isfinite(t0) || !std::isfinite(t1) || !(t0 >= 0.0) || !(t0 <= t1)) {
				throw std::invalid_argument("SparseMedium: window must be finite with 0 <= t0 <= t1");
			}
		}

		//---------------------------------------------------------------------
		// Exact 3D DDA over a regular grid (origin O, spacing h), clipped to
		// the ray parameter window [t0, t1]. Cells own their half-open box
		// [k, k+1) * h + O. visit(ix, iy, iz, ta, tb) is called in parameter
		// order for every positive-length cell interval and stops the
		// traversal by returning false. Zero-length intervals (exact face
		// starts, exact corner ties) are stepped over without epsilon; every
		// axis whose plane is crossed at the same parameter advances together.
		//---------------------------------------------------------------------
		template< typename F >
		void Dda3(const double O[3], double h,
		          const Vec3& o, const Vec3& d, double t0, double t1, F&& visit) {

			constexpr double kInf = std::numeric_limits< double >::infinity();

			const double oo[3] = { o.x, o.y, o.z };
			const double dd[3] = { d.x, d.y, d.z };
			const double pp[3] = { o.x + t0 * d.x, o.y + t0 * d.y, o.z + t0 * d.z };

			std::int64_t cell[3];
			int step[3];
			double tNext[3];
			for (int a = 0; a < 3; ++a) {
				if (0.0 == dd[a]) {
					cell[a]  = (std::int64_t)std::floor((pp[a] - O[a]) / h);
					step[a]  = 0;
					tNext[a] = kInf;
				}
				else {
					const std::int64_t c = (std::int64_t)std::floor((pp[a] - O[a]) / h);
					cell[a]  = c;
					step[a]  = dd[a] > 0.0 ? 1 : -1;
					const std::int64_t plane = step[a] > 0 ? c + 1 : c;
					tNext[a] = (O[a] + (double)plane * h - oo[a]) / dd[a];
				}
			}

			double t = t0;
			while (t < t1) {
				double tn = tNext[0];
				if (tNext[1] < tn) tn = tNext[1];
				if (tNext[2] < tn) tn = tNext[2];
				const double te = tn < t1 ? tn : t1;
				if (te > t) {
					if (!visit(cell[0], cell[1], cell[2], t, te)) {
						return;
					}
				}
				if (!(tn < t1)) {
					return;
				}
				// Advance every axis tied at the crossing parameter.
				for (int a = 0; a < 3; ++a) {
					if (0 != step[a] && tNext[a] <= tn) {
						cell[a] += step[a];
						const std::int64_t plane = step[a] > 0 ? cell[a] + 1 : cell[a];
						double nt = (O[a] + (double)plane * h - oo[a]) / dd[a];
						if (!(nt > tn)) {
							// Never step backwards in parameter space.
							nt = std::nextafter(tn, kInf);
						}
						tNext[a] = nt;
					}
				}
				t = tn;
			}
		}
	}

	SparseMedium::SparseMedium(const Vec3& origin, double cellSize,
	                           const std::vector< CellInput >& cells)
		: m_origin(origin),
		  m_cellSize(cellSize),
		  m_bmin{ 0.0, 0.0, 0.0 },
		  m_bmax{ 0.0, 0.0, 0.0 },
		  m_has_extinction(false) {

		if (!IsFinite(origin)) {
			throw std::invalid_argument("SparseMedium: origin must be finite");
		}
		if (!(cellSize > 0.0) || !std::isfinite(cellSize)) {
			throw std::invalid_argument("SparseMedium: cell size must be positive and finite");
		}

		// Validate and build into local storage first; the medium publishes
		// its data only after every input has been fully validated.
		std::vector< Cell > local_cells;
		std::unordered_map< GridKey, std::uint32_t, GridKeyHash > local_index;
		std::unordered_set< GridKey, GridKeyHash > local_blocks;
		local_cells.reserve(cells.size());

		std::int64_t bmin[3] = { INT64_MAX, INT64_MAX, INT64_MAX };
		std::int64_t bmax[3] = { INT64_MIN, INT64_MIN, INT64_MIN };
		bool has_extinction = false;

		for (const CellInput& in : cells) {
			if (!std::isfinite(in.rho) || !(in.rho >= 0.0)) {
				throw std::invalid_argument("SparseMedium: rho must be finite and non-negative");
			}
			const double albedo[3] = { in.ax, in.ay, in.az };
			for (const double a : albedo) {
				if (!std::isfinite(a) || a < 0.0 || a > 1.0) {
					throw std::invalid_argument("SparseMedium: albedo must be in [0,1]");
				}
			}
			const double s[6] = { in.sxx, in.syy, in.szz, in.sxy, in.sxz, in.syz };
			for (const double v : s) {
				if (!std::isfinite(v)) {
					throw std::invalid_argument("SparseMedium: non-finite tensor component");
				}
			}
			double eigen[3];
			SymmetricEigenvalues3(s, eigen);
			const double emin = std::min(eigen[0], std::min(eigen[1], eigen[2]));
			const double emax = std::max(eigen[0], std::max(eigen[1], eigen[2]));
			if (!(emin >= 0.01 * (1.0 - 1e-9)) || !(emax <= 100.0 * (1.0 + 1e-9))) {
				throw std::invalid_argument("SparseMedium: tensor eigenvalues outside [0.01, 100]");
			}
			Sggx sggx(in.sxx, in.syy, in.szz, in.sxy, in.sxz, in.syz); // throws unless SPD

			const GridKey key{ in.kx, in.ky, in.kz };
			const auto index = (std::uint32_t)local_cells.size();
			if (!local_index.emplace(key, index).second) {
				throw std::invalid_argument("SparseMedium: duplicate cell key");
			}
			local_cells.push_back(Cell{ in.kx, in.ky, in.kz, in.rho,
			                            Vec3{ in.ax, in.ay, in.az }, sggx });

			const GridKey block{ BlockOf(in.kx), BlockOf(in.ky), BlockOf(in.kz) };
			local_blocks.insert(block);
			const std::int64_t bb[3] = { block.x, block.y, block.z };
			for (int a = 0; a < 3; ++a) {
				bmin[a] = std::min(bmin[a], bb[a]);
				bmax[a] = std::max(bmax[a], bb[a]);
			}
			if (in.rho > 0.0) {
				has_extinction = true;
			}
		}

		if (!local_cells.empty()) {
			constexpr double kInf = std::numeric_limits< double >::infinity();
			const double block_h = cellSize * (double)kBlockSize;
			const double O[3] = { origin.x, origin.y, origin.z };
			for (int a = 0; a < 3; ++a) {
				m_bmin[a] = std::nextafter(O[a] + (double)bmin[a] * block_h, -kInf);
				m_bmax[a] = std::nextafter(O[a] + (double)(bmax[a] + 1) * block_h, kInf);
			}
		}

		m_cells          = std::move(local_cells);
		m_cell_index     = std::move(local_index);
		m_blocks         = std::move(local_blocks);
		m_has_extinction = has_extinction;
	}

	template< typename F >
	void SparseMedium::Traverse(const Vec3& o, const Vec3& d, double t0, double t1,
	                            TraversalStats* stats, F&& visit) const {
		if (m_cells.empty() || !(t0 < t1)) {
			return;
		}

		// Clip the window to the world bounds of the occupied blocks.
		double ta = t0, tb = t1;
		const double oo[3] = { o.x, o.y, o.z };
		const double dd[3] = { d.x, d.y, d.z };
		for (int a = 0; a < 3; ++a) {
			if (0.0 == dd[a]) {
				if (!(oo[a] >= m_bmin[a] && oo[a] < m_bmax[a])) {
					return;
				}
			}
			else {
				double u0 = (m_bmin[a] - oo[a]) / dd[a];
				double u1 = (m_bmax[a] - oo[a]) / dd[a];
				if (u0 > u1) {
					const double tmp = u0; u0 = u1; u1 = tmp;
				}
				if (u0 > ta) ta = u0;
				if (u1 < tb) tb = u1;
			}
		}
		if (!(ta < tb)) {
			return;
		}

		const double block_h = m_cellSize * (double)kBlockSize;
		const double O[3] = { m_origin.x, m_origin.y, m_origin.z };

		// Coarse DDA over 16^3-cell blocks skips empty space; the fine DDA
		// only runs inside occupied blocks. No array sized to the key
		// bounding box is ever allocated.
		Dda3(O, block_h, o, d, ta, tb,
		     [&](std::int64_t bx, std::int64_t by, std::int64_t bz,
		         double ba, double bb) -> bool {

			if (stats) {
				++stats->nodeTests;
			}
			if (bx < INT32_MIN || bx > INT32_MAX ||
			    by < INT32_MIN || by > INT32_MAX ||
			    bz < INT32_MIN || bz > INT32_MAX) {
				return true;
			}
			const GridKey block{ (std::int32_t)bx, (std::int32_t)by, (std::int32_t)bz };
			if (m_blocks.end() == m_blocks.find(block)) {
				return true;
			}
			bool stop = false;
			Dda3(O, m_cellSize, o, d, ba, bb,
			     [&](std::int64_t cx, std::int64_t cy, std::int64_t cz,
			         double ca, double cb) -> bool {

				if (stats) {
					++stats->cellTests;
				}
				if (cx < INT32_MIN || cx > INT32_MAX ||
				    cy < INT32_MIN || cy > INT32_MAX ||
				    cz < INT32_MIN || cz > INT32_MAX) {
					return true;
				}
				const GridKey key{ (std::int32_t)cx, (std::int32_t)cy, (std::int32_t)cz };
				const auto it = m_cell_index.find(key);
				if (m_cell_index.end() == it) {
					return true;
				}
				if (!visit((std::size_t)it->second, ca, cb)) {
					stop = true;
					return false;
				}
				return true;
			});
			return !stop;
		});
	}

	double SparseMedium::OpticalDepth(const Vec3& o, const Vec3& d,
	                                  double t0, double t1,
	                                  TraversalStats* stats) const {
		CheckQuery(o, d, t0, t1);
		const double dn = Norm(d);
		const Vec3  w  = d / dn;
		double tau = 0.0;
		Traverse(o, d, t0, t1, stats,
		         [&](std::size_t index, double a, double b) {
			const Cell& cell = m_cells[index];
			if (cell.rho > 0.0) {
				tau += cell.rho * cell.sggx.Sigma(w) * dn * (b - a);
			}
			return true;
		});
		return tau;
	}

	double SparseMedium::Transmittance(const Vec3& o, const Vec3& d,
	                                   double t0, double t1,
	                                   TraversalStats* stats) const {
		return std::exp(-OpticalDepth(o, d, t0, t1, stats));
	}

	bool SparseMedium::FirstCollision(const Vec3& o, const Vec3& d,
	                                  double t0, double t1, double u,
	                                  Collision& out,
	                                  TraversalStats* stats) const {
		CheckQuery(o, d, t0, t1);
		if (!(u >= 0.0 && u < 1.0)) {
			throw std::invalid_argument("SparseMedium: u must be in [0,1)");
		}
		const double dn = Norm(d);
		const Vec3  w  = d / dn;
		// One exponential optical depth for the whole valid segment.
		const double tau_target = -std::log(1.0 - u);

		double accum = 0.0;
		bool found = false;
		double t_hit = 0.0;
		std::size_t cell_hit = 0u;
		Traverse(o, d, t0, t1, stats,
		         [&](std::size_t index, double a, double b) {
			const Cell& cell = m_cells[index];
			if (cell.rho > 0.0) {
				const double mu  = cell.rho * cell.sggx.Sigma(w) * dn;
				const double len = b - a;
				const double need = tau_target - accum;
				if (need < mu * len) {
					// Consumed strictly inside this interval; a depth that
					// lands exactly on an interval end is decided by the
					// next positive-length interval (need == 0 hits at its
					// start), and by escape at the end of the whole window.
					t_hit    = a + need / mu;
					cell_hit = index;
					found    = true;
					return false;
				}
				accum += mu * len;
			}
			return true;
		});
		if (found) {
			out.t    = t_hit;
			out.cell = cell_hit;
		}
		return found;
	}

	std::vector< SparseMedium::Interval >
	SparseMedium::OccupiedIntervals(const Vec3& o, const Vec3& d,
	                                double t0, double t1,
	                                TraversalStats* stats) const {
		CheckQuery(o, d, t0, t1);
		std::vector< Interval > out;
		Traverse(o, d, t0, t1, stats,
		         [&](std::size_t index, double a, double b) {
			const Cell& cell = m_cells[index];
			out.push_back(Interval{ a, b, cell.kx, cell.ky, cell.kz, index });
			return true;
		});
		return out;
	}
}
