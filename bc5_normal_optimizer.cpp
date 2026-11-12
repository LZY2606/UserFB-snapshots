// bc5_normal_optimizer.cpp
//
// Implementation of the sample-position driven BC5 normal map optimizer.
//
// The whole design is built around one fact: a GPU filters the reconstructed
// R and G channels first (bilinear within a level, linear between levels),
// and only afterwards rebuilds Z and renormalizes the normal. A footprint of
// a sample therefore touches up to 4 texels in each of 2 mip levels, which
// may live in several different BC5 blocks. The optimizer keeps explicit
// sample->texel/block association lists so that each discrete candidate move
// only re-evaluates the samples whose footprint intersects the changed
// coordinate, with the sample cache and the block byte stream committed or
// rolled back together.
#include "bc5_normal_optimizer.h"

#include "rgbcx.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace bc5_normal
{
	namespace
	{
		static const double kTol = 1e-12;

		// -------------------------------------------------------------------
		// Independent BC4 integer interpolation (both BC4 palette modes).
		// -------------------------------------------------------------------

		static uint8_t bc4_palette_value(const uint8_t* p, uint32_t pixel)
		{
			const uint32_t e0 = p[0];
			const uint32_t e1 = p[1];

			uint8_t values[8];
			if (e0 > e1)
			{
				// 8 value mode
				values[0] = static_cast<uint8_t>(e0);
				values[1] = static_cast<uint8_t>(e1);
				for (uint32_t i = 2; i < 8; i++)
					values[i] = static_cast<uint8_t>(((8 - i) * e0 + (i - 1) * e1) / 7);
			}
			else
			{
				// 6 value mode, selectors 6 and 7 are hardwired
				values[0] = static_cast<uint8_t>(e0);
				values[1] = static_cast<uint8_t>(e1);
				for (uint32_t i = 2; i < 6; i++)
					values[i] = static_cast<uint8_t>(((6 - i) * e0 + (i - 1) * e1) / 5);
				values[6] = 0;
				values[7] = 255;
			}

			uint64_t bits = 0;
			for (uint32_t i = 0; i < 6; i++)
				bits |= static_cast<uint64_t>(p[2 + i]) << (8 * i);

			const uint32_t selector = static_cast<uint32_t>((bits >> (3 * pixel)) & 7);
			return values[selector];
		}

		static void bc4_set_selector(uint8_t* sel6, uint32_t pixel, uint32_t value)
		{
			const uint32_t bit = 3 * pixel;
			const uint32_t bi = bit / 8;
			const uint32_t sh = bit % 8;
			uint32_t x = sel6[bi];
			if (bi + 1 < 6)
				x |= sel6[bi + 1] << 8;
			x = (x & ~(7u << sh)) | (value << sh);
			sel6[bi] = static_cast<uint8_t>(x & 0xFF);
			if (bi + 1 < 6)
				sel6[bi + 1] = static_cast<uint8_t>(x >> 8);
		}

		// Filter first, reconstruct Z second, normalize last.
		static void make_normal(double r01, double g01, double out[3])
		{
			const double x = 2.0 * r01 - 1.0;
			const double y = 2.0 * g01 - 1.0;
			double z = 1.0 - x * x - y * y;
			if (z < 0.0)
				z = 0.0;
			z = std::sqrt(z);

			const double inv_len = 1.0 / std::sqrt(x * x + y * y + z * z);
			out[0] = x * inv_len;
			out[1] = y * inv_len;
			out[2] = z * inv_len;
		}

		// -------------------------------------------------------------------
		// Sample footprints under the GPU texel-center convention.
		// -------------------------------------------------------------------

		struct FootTex
		{
			uint32_t texel;
			double weight;
		};

		struct FootLevel
		{
			// At most 4 corner entries; duplicate addresses (clamp edges,
			// repeat seams, 1x1 levels) are merged into one weight.
			std::vector<FootTex> tex;
		};

		struct Footprint
		{
			uint32_t level0;
			uint32_t level1;
			double lod_frac;
			FootLevel levels[2];
		};

		struct SampleDesc
		{
			double u, v, lod, weight;
		};

		static int wrap_index(int x, uint32_t dim)
		{
			const int m = static_cast<int>(dim);
			int r = x % m;
			if (r < 0)
				r += m;
			return r;
		}

		static FootLevel build_foot_level(double u, double v, uint32_t w, uint32_t h, bool repeat)
		{
			const double gx = u * static_cast<double>(w) - 0.5;
			const double gy = v * static_cast<double>(h) - 0.5;

			const double fxd = std::floor(gx);
			const double fyd = std::floor(gy);
			const int x0 = static_cast<int>(fxd);
			const int y0 = static_cast<int>(fyd);
			const double tx = gx - fxd;
			const double ty = gy - fyd;

			FootLevel fl;
			fl.tex.reserve(4);

			for (int j = 0; j < 2; j++)
			{
				for (int i = 0; i < 2; i++)
				{
					int xi = x0 + i;
					int yj = y0 + j;
					if (repeat)
					{
						xi = wrap_index(xi, w);
						yj = wrap_index(yj, h);
					}
					else
					{
						if (xi < 0) xi = 0;
						else if (xi >= static_cast<int>(w)) xi = static_cast<int>(w) - 1;
						if (yj < 0) yj = 0;
						else if (yj >= static_cast<int>(h)) yj = static_cast<int>(h) - 1;
					}

					const double cw = (i ? tx : 1.0 - tx) * (j ? ty : 1.0 - ty);
					const uint32_t texel = static_cast<uint32_t>(yj) * w + static_cast<uint32_t>(xi);

					bool merged = false;
					for (size_t k = 0; k < fl.tex.size(); k++)
					{
						if (fl.tex[k].texel == texel)
						{
							fl.tex[k].weight += cw;
							merged = true;
							break;
						}
					}
					if (!merged)
						fl.tex.push_back(FootTex{ texel, cw });
				}
			}

			return fl;
		}

		// -------------------------------------------------------------------
		// Per-level mutable state.
		// -------------------------------------------------------------------

		struct LevelState
		{
			uint32_t w, h;
			uint32_t blocks_x, blocks_y, num_blocks;

			// Row-major BC5_UNORM blocks, 16 bytes each.
			std::vector<uint8_t> bytes;

			// Source RG in [0,1], one pair per valid texel.
			std::vector<double> src_r;
			std::vector<double> src_g;

			// Association lists: samples whose footprint references a texel/block.
			// Every sample id appears at most once per list.
			std::vector<std::vector<uint32_t> > tex_assoc;
			std::vector<std::vector<uint32_t> > block_assoc;
		};

		class OptimizerEngine
		{
		public:
			OptimizerEngine(const std::vector<utils::image_u8>& images,
				const std::vector<SampleDesc>& sample_descs,
				const Options& options) :
				m_images(images),
				m_samples(sample_descs),
				m_options(options),
				m_loss_evaluations(0),
				m_accepted_moves(0)
			{
			}

			uint64_t loss_evaluations() const { return m_loss_evaluations; }
			uint64_t accepted_moves() const { return m_accepted_moves; }

			bool run(std::vector<std::vector<uint8_t> >& out_blocks,
				double& out_initial_error,
				double& out_final_error,
				unsigned& out_sweeps,
				bool& out_converged)
			{
				if (!build_levels())
					return false;
				if (!encode_initial_blocks())
					return false;
				if (!build_footprints())
					return false;
				if (!build_associations())
					return false;

				m_cache.assign(m_samples.size(), 0.0);

				if (!evaluate_all())
					return false;

				// m_cache already stores the full weighted term per sample.
				double cached_error = 0.0;
				for (size_t i = 0; i < m_samples.size(); i++)
					cached_error += m_cache[i];
				if (!std::isfinite(cached_error))
					return false;

				const double initial_error = cached_error;

				unsigned sweeps = 0;
				bool converged = false;
				while (sweeps < m_options.max_sweeps)
				{
					const uint64_t accepted_before = m_accepted_moves;

					if (!sweep_selectors())
						return false;
					if (!sweep_endpoints())
						return false;

					sweeps++;

					if (m_accepted_moves == accepted_before)
					{
						converged = true;
						break;
					}
				}

				// Re-sum the cache: every accepted move updated the affected
				// entries, so this is the cached post-sweep total.
				cached_error = 0.0;
				for (size_t i = 0; i < m_samples.size(); i++)
					cached_error += m_cache[i];
				if (!std::isfinite(cached_error))
					return false;

				// Independent full recomputation: the cached total must match.
				std::vector<double> verify(m_samples.size());
				for (size_t i = 0; i < m_samples.size(); i++)
				{
					verify[i] = evaluate_sample(static_cast<uint32_t>(i));
					m_loss_evaluations++;
					if (!std::isfinite(verify[i]))
						return false;
				}

				double final_error = 0.0;
				for (size_t i = 0; i < m_samples.size(); i++)
					final_error += verify[i];
				if (!std::isfinite(final_error))
					return false;

				const double tolerance = 1e-10 * std::max(1.0, initial_error);
				if (std::fabs(final_error - cached_error) > tolerance)
					return false;
				if (final_error > initial_error + tolerance)
					return false;

				out_blocks.resize(m_levels.size());
				for (size_t l = 0; l < m_levels.size(); l++)
					out_blocks[l] = m_levels[l].bytes;

				out_initial_error = initial_error;
				out_final_error = final_error;
				out_sweeps = sweeps;
				out_converged = converged;
				return true;
			}

		private:
			const std::vector<utils::image_u8>& m_images;
			const std::vector<SampleDesc>& m_samples;
			const Options& m_options;

			std::vector<LevelState> m_levels;
			std::vector<Footprint> m_footprints;
			std::vector<double> m_cache;

			uint64_t m_loss_evaluations;
			uint64_t m_accepted_moves;

			bool build_levels()
			{
				const size_t num_levels = m_images.size();
				m_levels.resize(num_levels);

				for (size_t l = 0; l < num_levels; l++)
				{
					const utils::image_u8& im = m_images[l];
					const uint64_t w64 = im.width();
					const uint64_t h64 = im.height();
					if (w64 == 0 || h64 == 0)
						return false;
					if (w64 > UINT32_MAX || h64 > UINT32_MAX)
						return false;

					if (l > 0)
					{
						const uint32_t pw = m_images[l - 1].width();
						const uint32_t ph = m_images[l - 1].height();
						if (pw == 1 && ph == 1)
							return false; // no levels may be appended after 1x1
						const uint32_t expected_w = std::max(1u, pw / 2);
						const uint32_t expected_h = std::max(1u, ph / 2);
						if (static_cast<uint32_t>(w64) != expected_w || static_cast<uint32_t>(h64) != expected_h)
							return false;
					}

					const uint64_t texels64 = w64 * h64;
					if (texels64 > 0x40000000ULL)
						return false;

					const uint64_t bw64 = (w64 + 3) / 4;
					const uint64_t bh64 = (h64 + 3) / 4;
					const uint64_t blocks64 = bw64 * bh64;
					if (blocks64 > 0x10000000ULL || blocks64 * 16 > static_cast<uint64_t>(UINT32_MAX))
						return false;

					LevelState& st = m_levels[l];
					st.w = static_cast<uint32_t>(w64);
					st.h = static_cast<uint32_t>(h64);
					st.blocks_x = static_cast<uint32_t>(bw64);
					st.blocks_y = static_cast<uint32_t>(bh64);
					st.num_blocks = static_cast<uint32_t>(blocks64);
					st.bytes.resize(static_cast<size_t>(blocks64) * 16, 0);
					st.src_r.resize(static_cast<size_t>(texels64));
					st.src_g.resize(static_cast<size_t>(texels64));
					st.tex_assoc.resize(static_cast<size_t>(texels64));
					st.block_assoc.resize(static_cast<size_t>(blocks64));

					for (uint32_t y = 0; y < st.h; y++)
					{
						for (uint32_t x = 0; x < st.w; x++)
						{
							const utils::color_quad_u8& p = im(x, y);
							const uint32_t idx = y * st.w + x;
							st.src_r[idx] = p.r / 255.0;
							st.src_g[idx] = p.g / 255.0;
						}
					}
				}

				return true;
			}

			bool encode_initial_blocks()
			{
				rgbcx::init();

				for (size_t l = 0; l < m_levels.size(); l++)
				{
					const LevelState& st = m_levels[l];
					const utils::image_u8& im = m_images[l];

					// Padded working copy: out-of-range texels duplicate the
					// last row/column of the level.
					utils::image_u8 padded;
					padded.init(st.blocks_x * 4, st.blocks_y * 4);
					for (uint32_t y = 0; y < padded.height(); y++)
					{
						const uint32_t sy = std::min(y, st.h - 1);
						for (uint32_t x = 0; x < padded.width(); x++)
						{
							const uint32_t sx = std::min(x, st.w - 1);
							padded(x, y) = im(sx, sy);
						}
					}

					for (uint32_t by = 0; by < st.blocks_y; by++)
					{
						for (uint32_t bx = 0; bx < st.blocks_x; bx++)
						{
							utils::color_quad_u8 block_pixels[16];
							padded.get_block(bx, by, 4, 4, block_pixels);

							uint8_t* dst = m_levels[l].bytes.data() + (by * st.blocks_x + bx) * 16;
							rgbcx::encode_bc5_hq(dst, &block_pixels[0].m_c[0]);
						}
					}
				}

				return true;
			}

			bool build_footprints()
			{
				const size_t num_levels = m_levels.size();
				m_footprints.resize(m_samples.size());

				for (size_t s = 0; s < m_samples.size(); s++)
				{
					const SampleDesc& sd = m_samples[s];
					Footprint& fp = m_footprints[s];

					double u = sd.u;
					double v = sd.v;
					if (m_options.repeat)
					{
						// Wrap finite UVs into [0,1) before the modular addressing.
						u -= std::floor(u);
						v -= std::floor(v);
					}
					else
					{
						if (u < 0.0) u = 0.0;
						else if (u > 1.0) u = 1.0;
						if (v < 0.0) v = 0.0;
						else if (v > 1.0) v = 1.0;
					}

					double lod = sd.lod;
					if (lod < 0.0) lod = 0.0;
					const double max_lod = static_cast<double>(num_levels - 1);
					if (lod > max_lod) lod = max_lod;

					const uint32_t l0 = static_cast<uint32_t>(std::floor(lod));
					double frac = lod - static_cast<double>(l0);
					uint32_t l1 = l0 + 1;
					if (l1 >= num_levels)
					{
						l1 = static_cast<uint32_t>(num_levels - 1);
						frac = 0.0;
					}

					fp.level0 = l0;
					fp.level1 = l1;
					fp.lod_frac = frac;
					fp.levels[0] = build_foot_level(u, v, m_levels[l0].w, m_levels[l0].h, m_options.repeat);
					if (l1 != l0)
						fp.levels[1] = build_foot_level(u, v, m_levels[l1].w, m_levels[l1].h, m_options.repeat);
					else
						fp.levels[1] = fp.levels[0];
				}

				return true;
			}

			bool build_associations()
			{
				for (size_t s = 0; s < m_samples.size(); s++)
				{
					const Footprint& fp = m_footprints[s];
					for (uint32_t k = 0; k < 2; k++)
					{
						const uint32_t level = (k == 0) ? fp.level0 : fp.level1;
						if (k == 1 && fp.level1 == fp.level0)
							continue;

						LevelState& st = m_levels[level];
						const FootLevel& fl = fp.levels[k];
						for (size_t q = 0; q < fl.tex.size(); q++)
						{
							const uint32_t texel = fl.tex[q].texel;
							const uint32_t tx = texel % st.w;
							const uint32_t ty = texel / st.w;
							const uint32_t block = (ty / 4) * st.blocks_x + (tx / 4);

							st.tex_assoc[texel].push_back(static_cast<uint32_t>(s));
							st.block_assoc[block].push_back(static_cast<uint32_t>(s));
						}
					}
				}

				for (size_t l = 0; l < m_levels.size(); l++)
				{
					LevelState& st = m_levels[l];
					for (size_t i = 0; i < st.tex_assoc.size(); i++)
					{
						std::sort(st.tex_assoc[i].begin(), st.tex_assoc[i].end());
						st.tex_assoc[i].erase(std::unique(st.tex_assoc[i].begin(), st.tex_assoc[i].end()), st.tex_assoc[i].end());
					}
					for (size_t i = 0; i < st.block_assoc.size(); i++)
					{
						std::sort(st.block_assoc[i].begin(), st.block_assoc[i].end());
						st.block_assoc[i].erase(std::unique(st.block_assoc[i].begin(), st.block_assoc[i].end()), st.block_assoc[i].end());
					}
				}

				return true;
			}

			// ---------------------------------------------------------------
			// Sample evaluation
			// ---------------------------------------------------------------

			void filtered_source(const LevelState& st, const FootLevel& fl, double out_rg[2]) const
			{
				double r = 0.0, g = 0.0;
				for (size_t q = 0; q < fl.tex.size(); q++)
				{
					const FootTex& ft = fl.tex[q];
					r += ft.weight * st.src_r[ft.texel];
					g += ft.weight * st.src_g[ft.texel];
				}
				out_rg[0] = r;
				out_rg[1] = g;
			}

			void filtered_compressed(const LevelState& st, const FootLevel& fl, double out_rg[2]) const
			{
				double r = 0.0, g = 0.0;
				for (size_t q = 0; q < fl.tex.size(); q++)
				{
					const FootTex& ft = fl.tex[q];
					const uint32_t texel = ft.texel;
					const uint32_t tx = texel % st.w;
					const uint32_t ty = texel / st.w;
					const uint32_t block = (ty / 4) * st.blocks_x + (tx / 4);
					const uint32_t local = (ty % 4) * 4 + (tx % 4);
					const uint8_t* p = st.bytes.data() + block * 16;

					r += ft.weight * bc4_palette_value(p, local) / 255.0;
					g += ft.weight * bc4_palette_value(p + 8, local) / 255.0;
				}
				out_rg[0] = r;
				out_rg[1] = g;
			}

			// One angular error term for one sample against the current bytes.
			double evaluate_sample(uint32_t sample_index) const
			{
				const SampleDesc& sd = m_samples[sample_index];
				const Footprint& fp = m_footprints[sample_index];

				// All filtering happens in RG space: bilinear within each
				// level, linear between levels. Z is rebuilt and the normal
				// normalized only once, on the final filtered RG.
				double src_rg[2], cmp_rg[2];
				filtered_source(m_levels[fp.level0], fp.levels[0], src_rg);
				filtered_compressed(m_levels[fp.level0], fp.levels[0], cmp_rg);

				if (fp.level1 != fp.level0 && fp.lod_frac > 0.0)
				{
					double src_rg1[2], cmp_rg1[2];
					filtered_source(m_levels[fp.level1], fp.levels[1], src_rg1);
					filtered_compressed(m_levels[fp.level1], fp.levels[1], cmp_rg1);

					const double t = fp.lod_frac;
					src_rg[0] = src_rg[0] * (1.0 - t) + src_rg1[0] * t;
					src_rg[1] = src_rg[1] * (1.0 - t) + src_rg1[1] * t;
					cmp_rg[0] = cmp_rg[0] * (1.0 - t) + cmp_rg1[0] * t;
					cmp_rg[1] = cmp_rg[1] * (1.0 - t) + cmp_rg1[1] * t;
				}

				double ns[3], nc[3];
				make_normal(src_rg[0], src_rg[1], ns);
				make_normal(cmp_rg[0], cmp_rg[1], nc);

				double dot = ns[0] * nc[0] + ns[1] * nc[1] + ns[2] * nc[2];
				double err = 1.0 - dot;
				if (err < 0.0)
					err = 0.0;
				return sd.weight * err;
			}

			bool evaluate_all()
			{
				double total = 0.0;
				for (size_t i = 0; i < m_samples.size(); i++)
				{
					const double v = evaluate_sample(static_cast<uint32_t>(i));
					m_loss_evaluations++;
					if (!std::isfinite(v))
						return false;
					m_cache[i] = v;
					total += v;
				}
				return std::isfinite(total);
			}

			// Evaluates one candidate state over its (deduplicated) affected
			// sample list. The results go into out_values, never into the
			// cache: the cache is only updated when a move is committed.
			bool evaluate_candidate(const std::vector<uint32_t>& list, std::vector<double>& out_values, double& out_total)
			{
				out_values.resize(list.size());
				out_total = 0.0;
				for (size_t i = 0; i < list.size(); i++)
				{
					const double v = evaluate_sample(list[i]);
					m_loss_evaluations++;
					if (!std::isfinite(v))
						return false;
					out_values[i] = v;
					out_total += v;
				}
				return std::isfinite(out_total);
			}

			double cached_list_total(const std::vector<uint32_t>& list) const
			{
				double total = 0.0;
				for (size_t i = 0; i < list.size(); i++)
					total += m_cache[list[i]];
				return total;
			}

			// ---------------------------------------------------------------
			// Selector coordinates: 64 (sx,sy) pairs per valid texel
			// ---------------------------------------------------------------

			bool sweep_selectors()
			{
				std::vector<double> candidate_values;
				std::vector<double> best_values;

				for (size_t l = 0; l < m_levels.size(); l++)
				{
					LevelState& st = m_levels[l];

					for (uint32_t ty = 0; ty < st.h; ty++)
					{
						for (uint32_t tx = 0; tx < st.w; tx++)
						{
							const uint32_t texel = ty * st.w + tx;
							const std::vector<uint32_t>& affected = st.tex_assoc[texel];
							if (affected.empty())
								continue;

							const uint32_t block = (ty / 4) * st.blocks_x + (tx / 4);
							const uint32_t local = (ty % 4) * 4 + (tx % 4);
							uint8_t* p = st.bytes.data() + block * 16;

							const double orig_error = cached_list_total(affected);

							// Cache and byte stream are saved together.
							uint8_t saved[12];
							std::memcpy(saved, p + 2, 6);
							std::memcpy(saved + 6, p + 10, 6);

							double min_error = std::numeric_limits<double>::infinity();
							uint32_t best_key = 64;

							for (uint32_t sy = 0; sy < 8; sy++)
							{
								for (uint32_t sx = 0; sx < 8; sx++)
								{
									bc4_set_selector(p + 2, local, sx);
									bc4_set_selector(p + 10, local, sy);

									double candidate_error = 0.0;
									if (!evaluate_candidate(affected, candidate_values, candidate_error))
										return false;

									const uint32_t key = 8 * sx + sy;
									if (candidate_error < min_error - kTol)
									{
										min_error = candidate_error;
										best_key = key;
										best_values.swap(candidate_values);
										std::vector<double>().swap(candidate_values);
									}
									else if (std::fabs(candidate_error - min_error) <= kTol && key < best_key)
									{
										// Ties at the minimum resolve to the smaller 8*sx+sy key.
										best_key = key;
										best_values.swap(candidate_values);
										std::vector<double>().swap(candidate_values);
									}
								}
							}

							// Roll the bytes back to the committed state first.
							std::memcpy(p + 2, saved, 6);
							std::memcpy(p + 10, saved + 6, 6);

							// Original value within tolerance of the minimum: keep it.
							if (orig_error <= min_error + kTol)
								continue;

							const double best_error = min_error;

							// Strict improvement gate relative to the committed state.
							if (!(orig_error - best_error > kTol))
								continue;

							// Commit bytes and cache together.
							const uint32_t chosen_sx = best_key / 8;
							const uint32_t chosen_sy = best_key % 8;
							bc4_set_selector(p + 2, local, chosen_sx);
							bc4_set_selector(p + 10, local, chosen_sy);
							for (size_t i = 0; i < affected.size(); i++)
								m_cache[affected[i]] = best_values[i];
							m_accepted_moves++;
						}
					}
				}

				return true;
			}

			// ---------------------------------------------------------------
			// Endpoint coordinates: offsets 0,1 (R) and 8,9 (G) per block
			// ---------------------------------------------------------------

			bool sweep_endpoints()
			{
				static const uint32_t kOffsets[4] = { 0, 1, 8, 9 };

				std::vector<double> candidate_values;
				std::vector<double> best_values;

				for (size_t l = 0; l < m_levels.size(); l++)
				{
					LevelState& st = m_levels[l];

					for (uint32_t by = 0; by < st.blocks_y; by++)
					{
						for (uint32_t bx = 0; bx < st.blocks_x; bx++)
						{
							const uint32_t block = by * st.blocks_x + bx;
							const std::vector<uint32_t>& affected = st.block_assoc[block];
							if (affected.empty())
								continue;

							uint8_t* p = st.bytes.data() + block * 16;

							for (uint32_t oi = 0; oi < 4; oi++)
							{
								const uint32_t off = kOffsets[oi];
								const uint8_t orig_value = p[off];
								const double orig_error = cached_list_total(affected);

								uint8_t candidates[3];
								uint32_t num_candidates = 0;
								for (int d = -1; d <= 1; d++)
								{
									const int v = static_cast<int>(orig_value) + d;
									if (v < 0 || v > 255)
										continue;
									const uint8_t cv = static_cast<uint8_t>(v);
									bool duplicate = false;
									for (uint32_t q = 0; q < num_candidates; q++)
									{
										if (candidates[q] == cv)
										{
											duplicate = true;
											break;
										}
									}
									if (!duplicate)
										candidates[num_candidates++] = cv;
								}

								double min_error = std::numeric_limits<double>::infinity();
								uint32_t best_value = 256;

								for (uint32_t q = 0; q < num_candidates; q++)
								{
									p[off] = candidates[q];

									double candidate_error = 0.0;
									if (!evaluate_candidate(affected, candidate_values, candidate_error))
										return false;

									const uint32_t cv = candidates[q];
									if (candidate_error < min_error - kTol)
									{
										min_error = candidate_error;
										best_value = cv;
										best_values.swap(candidate_values);
										std::vector<double>().swap(candidate_values);
									}
									else if (std::fabs(candidate_error - min_error) <= kTol && cv < best_value)
									{
										// Ties at the minimum resolve to the smaller byte.
										best_value = cv;
										best_values.swap(candidate_values);
										std::vector<double>().swap(candidate_values);
									}
								}

								// Roll the byte back to the committed state.
								p[off] = orig_value;

								// Original value within tolerance of the minimum: keep it.
								if (orig_error <= min_error + kTol)
									continue;

								const double best_error = min_error;

								// Strict improvement gate. Crossing the endpoint ordering
								// reinterprets the whole block via the 6/8 value palette,
								// which evaluate_sample() already handles per candidate.
								if (!(orig_error - best_error > kTol))
									continue;

								// Commit byte and cache together.
								p[off] = static_cast<uint8_t>(best_value);
								for (size_t i = 0; i < affected.size(); i++)
									m_cache[affected[i]] = best_values[i];
								m_accepted_moves++;
							}
						}
					}
				}

				return true;
			}
		};

	} // anonymous namespace

	bool optimize(const std::vector<utils::image_u8>& levels,
		const std::vector<Sample>& samples,
		const Options& options,
		Result& result)
	{
		// --- Explicit input validation. On failure the caller's result is untouched. ---
		if (levels.empty())
			return false;
		if (samples.empty())
			return false;
		if (options.max_sweeps < 1 || options.max_sweeps > 64)
			return false;

		for (size_t l = 0; l < levels.size(); l++)
		{
			if (levels[l].width() == 0 || levels[l].height() == 0)
				return false;
			if (l > 0)
			{
				const uint32_t pw = levels[l - 1].width();
				const uint32_t ph = levels[l - 1].height();
				if (pw == 1 && ph == 1)
					return false;
				if (levels[l].width() != std::max(1u, pw / 2) || levels[l].height() != std::max(1u, ph / 2))
					return false;
			}
		}

		std::vector<SampleDesc> descs;
		descs.reserve(samples.size());
		for (size_t i = 0; i < samples.size(); i++)
		{
			const Sample& s = samples[i];
			if (!std::isfinite(s.u) || !std::isfinite(s.v) || !std::isfinite(s.lod) || !std::isfinite(s.weight))
				return false;
			if (!(s.weight > 0.0))
				return false;
			descs.push_back(SampleDesc{ s.u, s.v, s.lod, s.weight });
		}

		Options actual_options = options;
		if (actual_options.max_sweeps < 1 || actual_options.max_sweeps > 64)
			return false;

		OptimizerEngine engine(levels, descs, actual_options);

		std::vector<std::vector<uint8_t> > out_blocks;
		double initial_error = 0.0;
		double final_error = 0.0;
		unsigned sweeps = 0;
		bool converged = false;

		if (!engine.run(out_blocks, initial_error, final_error, sweeps, converged))
			return false;

		if (!std::isfinite(initial_error) || !std::isfinite(final_error))
			return false;

		Result produced;
		produced.blocks = out_blocks;
		produced.initial_error = initial_error;
		produced.final_error = final_error;
		produced.loss_evaluations = engine.loss_evaluations();
		produced.accepted_moves = engine.accepted_moves();
		produced.sweeps = sweeps;
		produced.converged = converged;

		result = produced;
		return true;
	}

} // namespace bc5_normal
