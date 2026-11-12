// tests/task_sampled_normals.cpp
//
// Tests for the sample-position driven BC5 normal map optimizer.
//
// Everything in here is intentionally independent of the production
// implementation: the BC4 bit layout is parsed again from scratch, and a
// small full-scan sampling reference recomputes every sample from block
// bytes. Expected values never come from the production sampler itself.
#include "registry.hpp"

#include "bc5_normal_optimizer.h"
#include "rdo_bc_encoder.h"
#include "rgbcx.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace
{
	using utils::color_quad_u8;
	using utils::image_u8;

	const double kEps = 1e-12;

	// -------------------------------------------------------------------
	// Independent BC4 block parsing (test-only reference).
	// -------------------------------------------------------------------

	struct RefBC4
	{
		static uint32_t selector_of(const uint8_t* sel6, uint32_t pixel)
		{
			const uint32_t bit = 3 * pixel;
			const uint32_t bi = bit >> 3;
			const uint32_t sh = bit & 7;
			const uint32_t x = sel6[bi] | (sel6[bi + 1] << 8);
			return (x >> sh) & 7;
		}

		static void set_selector(uint8_t* sel6, uint32_t pixel, uint32_t value)
		{
			const uint32_t bit = 3 * pixel;
			const uint32_t bi = bit >> 3;
			const uint32_t sh = bit & 7;
			uint32_t x = sel6[bi] | (sel6[bi + 1] << 8);
			x = (x & ~(7u << sh)) | ((value & 7u) << sh);
			sel6[bi] = static_cast<uint8_t>(x & 0xFF);
			sel6[bi + 1] = static_cast<uint8_t>(x >> 8);
		}

		static void palette(uint8_t out[8], uint8_t a, uint8_t b)
		{
			if (a > b)
			{
				// 8 value interpolation mode.
				out[0] = a;
				out[1] = b;
				for (uint32_t i = 2; i < 8; i++)
					out[i] = static_cast<uint8_t>(((8 - i) * a + (i - 1) * b) / 7);
			}
			else
			{
				// 6 value mode; selectors 6 and 7 are hardwired.
				out[0] = a;
				out[1] = b;
				for (uint32_t i = 2; i < 6; i++)
					out[i] = static_cast<uint8_t>(((6 - i) * a + (i - 1) * b) / 5);
				out[6] = 0;
				out[7] = 255;
			}
		}

		static uint8_t decode_channel(const uint8_t* block8, uint32_t pixel)
		{
			uint8_t values[8];
			palette(values, block8[0], block8[1]);
			return values[selector_of(block8 + 2, pixel)];
		}
	};

	struct Vec3 { double x, y, z; };

	Vec3 normal_from_rg(double r, double g)
	{
		const double x = 2.0 * r - 1.0;
		const double y = 2.0 * g - 1.0;
		double z = 1.0 - x * x - y * y;
		if (z < 0.0) z = 0.0;
		z = std::sqrt(z);
		const double inv = 1.0 / std::sqrt(x * x + y * y + z * z);
		Vec3 n = { x * inv, y * inv, z * inv };
		return n;
	}

	// -------------------------------------------------------------------
	// Full-scan sampling reference over BC5 block bytes.
	// -------------------------------------------------------------------

	struct RefCorner
	{
		uint32_t texel;
		double weight;
	};

	struct RefFoot
	{
		std::vector<RefCorner> corners;
	};

	int wrap_idx(int x, uint32_t dim)
	{
		int r = x % static_cast<int>(dim);
		if (r < 0) r += dim;
		return r;
	}

	RefFoot ref_footprint(double u, double v, uint32_t w, uint32_t h, bool repeat)
	{
		const double gx = u * static_cast<double>(w) - 0.5;
		const double gy = v * static_cast<double>(h) - 0.5;
		const double fxd = std::floor(gx);
		const double fyd = std::floor(gy);
		const int x0 = static_cast<int>(fxd);
		const int y0 = static_cast<int>(fyd);
		const double tx = gx - fxd;
		const double ty = gy - fyd;

		RefFoot foot;
		for (int j = 0; j < 2; j++)
		{
			for (int i = 0; i < 2; i++)
			{
				int xi = x0 + i;
				int yj = y0 + j;
				if (repeat)
				{
					xi = wrap_idx(xi, w);
					yj = wrap_idx(yj, h);
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
				for (size_t k = 0; k < foot.corners.size(); k++)
				{
					if (foot.corners[k].texel == texel)
					{
						foot.corners[k].weight += cw;
						merged = true;
						break;
					}
				}
				if (!merged)
					foot.corners.push_back(RefCorner{ texel, cw });
			}
		}
		return foot;
	}

	void ref_filtered(const image_u8* source, const std::vector<uint8_t>* blocks,
		uint32_t w, const RefFoot& foot, double out_rg[2])
	{
		const uint32_t blocks_x = (w + 3) / 4;
		double r = 0.0, g = 0.0;
		for (size_t k = 0; k < foot.corners.size(); k++)
		{
			const uint32_t texel = foot.corners[k].texel;
			const uint32_t tx = texel % w;
			const uint32_t ty = texel / w;
			const double cw = foot.corners[k].weight;

			double rv, gv;
			if (source)
			{
				rv = (*source)(tx, ty).r / 255.0;
				gv = (*source)(tx, ty).g / 255.0;
			}
			else
			{
				const uint32_t local = (ty % 4) * 4 + (tx % 4);
				const uint32_t block = (ty / 4) * blocks_x + (tx / 4);
				const uint8_t* p = blocks->data() + block * 16;
				rv = RefBC4::decode_channel(p, local) / 255.0;
				gv = RefBC4::decode_channel(p + 8, local) / 255.0;
			}
			r += cw * rv;
			g += cw * gv;
		}
		out_rg[0] = r;
		out_rg[1] = g;
	}

	double ref_sample_loss(const std::vector<image_u8>& levels,
		const std::vector<std::vector<uint8_t> >& block_levels,
		const bc5_normal::Sample& sample, bool repeat)
	{
		double u = sample.u;
		double v = sample.v;
		if (repeat)
		{
			u -= std::floor(u);
			v -= std::floor(v);
		}
		else
		{
			if (u < 0.0) u = 0.0; else if (u > 1.0) u = 1.0;
			if (v < 0.0) v = 0.0; else if (v > 1.0) v = 1.0;
		}

		double lod = sample.lod;
		if (lod < 0.0) lod = 0.0;
		const double max_lod = static_cast<double>(levels.size() - 1);
		if (lod > max_lod) lod = max_lod;
		const uint32_t l0 = static_cast<uint32_t>(std::floor(lod));
		uint32_t l1 = l0 + 1;
		double frac = lod - static_cast<double>(l0);
		if (l1 >= levels.size())
		{
			l1 = static_cast<uint32_t>(levels.size() - 1);
			frac = 0.0;
		}

		double src_rg[2], cmp_rg[2];
		{
			const image_u8& im0 = levels[l0];
			const RefFoot foot0 = ref_footprint(u, v, im0.width(), im0.height(), repeat);
			double s0[2], c0[2];
			ref_filtered(&im0, nullptr, im0.width(), foot0, s0);
			ref_filtered(nullptr, &block_levels[l0], im0.width(), foot0, c0);
			src_rg[0] = s0[0]; src_rg[1] = s0[1];
			cmp_rg[0] = c0[0]; cmp_rg[1] = c0[1];
		}

		if (l1 != l0 && frac > 0.0)
		{
			const image_u8& im1 = levels[l1];
			const RefFoot foot1 = ref_footprint(u, v, im1.width(), im1.height(), repeat);
			double s1[2], c1[2];
			ref_filtered(&im1, nullptr, im1.width(), foot1, s1);
			ref_filtered(nullptr, &block_levels[l1], im1.width(), foot1, c1);
			src_rg[0] = src_rg[0] * (1.0 - frac) + s1[0] * frac;
			src_rg[1] = src_rg[1] * (1.0 - frac) + s1[1] * frac;
			cmp_rg[0] = cmp_rg[0] * (1.0 - frac) + c1[0] * frac;
			cmp_rg[1] = cmp_rg[1] * (1.0 - frac) + c1[1] * frac;
		}

		// Rebuild Z and normalize exactly once, after all RG filtering.
		const Vec3 ns = normal_from_rg(src_rg[0], src_rg[1]);
		const Vec3 nc = normal_from_rg(cmp_rg[0], cmp_rg[1]);

		double dot = ns.x * nc.x + ns.y * nc.y + ns.z * nc.z;
		double err = 1.0 - dot;
		if (err < 0.0) err = 0.0;
		return sample.weight * err;
	}

	double ref_total_loss(const std::vector<image_u8>& levels,
		const std::vector<std::vector<uint8_t> >& block_levels,
		const std::vector<bc5_normal::Sample>& samples, bool repeat)
	{
		double total = 0.0;
		for (size_t i = 0; i < samples.size(); i++)
			total += ref_sample_loss(levels, block_levels, samples[i], repeat);
		return total;
	}

	bool close_enough(double a, double b)
	{
		const double scale = std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
		return std::fabs(a - b) <= 1e-10 * scale;
	}

	// -------------------------------------------------------------------
	// Deterministic test images and fixed default HQ starting point.
	// -------------------------------------------------------------------

	image_u8 make_image(uint32_t w, uint32_t h, uint32_t seed)
	{
		image_u8 im(w, h);
		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w; x++)
			{
				const uint8_t r = static_cast<uint8_t>(25 + (x * 53 + y * 31 + seed * 17) % 200);
				const uint8_t g = static_cast<uint8_t>(35 + (x * 29 + y * 47 + seed * 13) % 190);
				const uint8_t b = static_cast<uint8_t>(10 + (x * 11 + y * 7 + seed * 5) % 120);
				im(x, y) = color_quad_u8(r, g, b, 255);
			}
		return im;
	}

	image_u8 make_flat_image(uint32_t w, uint32_t h, uint8_t r, uint8_t g)
	{
		image_u8 im(w, h);
		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w; x++)
				im(x, y) = color_quad_u8(r, g, 99, 255);
		return im;
	}

	std::vector<uint8_t> hq_start_bytes(const std::vector<image_u8>& levels)
	{
		std::vector<uint8_t> all;
		rgbcx::init();
		for (size_t l = 0; l < levels.size(); l++)
		{
			const image_u8& im = levels[l];
			const uint32_t bw = (im.width() + 3) / 4;
			const uint32_t bh = (im.height() + 3) / 4;
			image_u8 padded(bw * 4, bh * 4);
			for (uint32_t y = 0; y < padded.height(); y++)
				for (uint32_t x = 0; x < padded.width(); x++)
					padded(x, y) = im(std::min(x, im.width() - 1), std::min(y, im.height() - 1));

			for (uint32_t by = 0; by < bh; by++)
				for (uint32_t bx = 0; bx < bw; bx++)
				{
					color_quad_u8 pix[16];
					padded.get_block(bx, by, 4, 4, pix);
					uint8_t block[16];
					rgbcx::encode_bc5_hq(block, &pix[0].m_c[0]);
					all.insert(all.end(), block, block + 16);
				}
		}
		return all;
	}

	std::vector<std::vector<uint8_t> > split_level_bytes(const std::vector<image_u8>& levels,
		const std::vector<uint8_t>& flat)
	{
		std::vector<std::vector<uint8_t> > out(levels.size());
		size_t off = 0;
		for (size_t l = 0; l < levels.size(); l++)
		{
			const uint32_t bw = (levels[l].width() + 3) / 4;
			const uint32_t bh = (levels[l].height() + 3) / 4;
			const size_t n = static_cast<size_t>(bw) * bh * 16;
			out[l].assign(flat.begin() + off, flat.begin() + off + n);
			off += n;
		}
		return out;
	}

	// ===================================================================
	// sampling group
	// ===================================================================

	void test_sampling_calibration(Registry& r, const char* name,
		const std::vector<image_u8>& levels, const std::vector<bc5_normal::Sample>& samples, bool repeat)
	{
		bc5_normal::Options opt;
		opt.repeat = repeat;
		opt.max_sweeps = 2;
		bc5_normal::Result res;
		r.check(bc5_normal::optimize(levels, samples, opt, res), name);

		const std::vector<uint8_t> initial_flat = hq_start_bytes(levels);
		const std::vector<std::vector<uint8_t> > initial_levels = split_level_bytes(levels, initial_flat);

		const double ref_initial = ref_total_loss(levels, initial_levels, samples, repeat);
		const double ref_final = ref_total_loss(levels, res.blocks, samples, repeat);

		r.check(close_enough(ref_initial, res.initial_error), "initial error matches independent reference");
		r.check(close_enough(ref_final, res.final_error), "final error matches independent reference");
		r.check(res.final_error <= res.initial_error + 1e-10 * std::max(1.0, res.initial_error),
			"optimization never raises the sampled error");

		for (size_t l = 0; l < levels.size(); l++)
		{
			const uint32_t bw = (levels[l].width() + 3) / 4;
			const uint32_t bh = (levels[l].height() + 3) / 4;
			r.check(res.blocks[l].size() == static_cast<size_t>(bw) * bh * 16, "level block bytes are full 16-byte blocks");
		}
	}

	void sampling_cross_block(Registry& r)
	{
		const image_u8 im = make_image(8, 4, 1);
		std::vector<image_u8> levels;
		levels.push_back(im);

		std::vector<bc5_normal::Sample> samples;
		// gx = u*8 - 0.5 = 3.75 crosses the x=3/4 block boundary.
		samples.push_back(bc5_normal::Sample(4.25 / 8.0, 2.25 / 4.0, 0.0, 1.0));
		samples.push_back(bc5_normal::Sample(3.75 / 8.0, 3.75 / 4.0, 0.0, 0.75));
		test_sampling_calibration(r, "sampling/cross_block", levels, samples, false);
	}

	void sampling_repeat_seam(Registry& r)
	{
		const image_u8 im = make_image(5, 5, 2);
		std::vector<image_u8> levels;
		levels.push_back(im);

		std::vector<bc5_normal::Sample> samples;
		// gx = 0.98*5 - 0.5 = 4.4: the second corner wraps from column 4 to 0.
		samples.push_back(bc5_normal::Sample(0.98, 0.96, 0.0, 1.3));
		samples.push_back(bc5_normal::Sample(0.02, 0.04, 0.0, 0.4));
		test_sampling_calibration(r, "sampling/repeat_seam", levels, samples, true);

		// Repeat and clamp must disagree at this seam position.
		const std::vector<uint8_t> flat = hq_start_bytes(levels);
		const std::vector<std::vector<uint8_t> > blocks = split_level_bytes(levels, flat);
		const bc5_normal::Sample seam(0.98, 0.96, 0.0, 1.0);
		const double clamped = ref_sample_loss(levels, blocks, seam, false);
		const double wrapped = ref_sample_loss(levels, blocks, seam, true);
		r.check(clamped != wrapped, "repeat seam addresses a different texel than clamping");
	}

	void sampling_clamp_edge(Registry& r)
	{
		const image_u8 im = make_image(6, 6, 3);
		std::vector<image_u8> levels;
		levels.push_back(im);

		std::vector<bc5_normal::Sample> samples;
		// u=0 -> gx=-0.5: both corners collapse onto texel column 0.
		samples.push_back(bc5_normal::Sample(0.0, 0.0, 0.0, 1.0));
		samples.push_back(bc5_normal::Sample(1.0, 1.0, 0.0, 1.0));
		samples.push_back(bc5_normal::Sample(-0.25, 1.4, 0.0, 0.5));
		test_sampling_calibration(r, "sampling/clamp_edge", levels, samples, false);

		const RefFoot foot = ref_footprint(0.0, 0.0, 6, 6, false);
		r.check(foot.corners.size() == 1, "clamped edge footprint merges to one texel");
		r.check(std::fabs(foot.corners[0].weight - 1.0) < 1e-15, "merged edge weight is 1");
	}

	void sampling_odd_size(Registry& r)
	{
		const image_u8 im = make_image(7, 5, 4);
		std::vector<image_u8> levels;
		levels.push_back(im);

		std::vector<bc5_normal::Sample> samples;
		samples.push_back(bc5_normal::Sample(6.5 / 7.0, 4.5 / 5.0, 0.0, 1.0));
		samples.push_back(bc5_normal::Sample(1.0, 1.0, 0.0, 1.0));
		test_sampling_calibration(r, "sampling/odd_size", levels, samples, false);

		// No footprint may ever address the padded column/row 7/4.
		for (double u = 0.0; u <= 1.0; u += 0.03125)
			for (double v = 0.0; v <= 1.0; v += 0.0625)
			{
				const RefFoot foot = ref_footprint(u, v, 7, 5, false);
				for (size_t k = 0; k < foot.corners.size(); k++)
				{
					const uint32_t tx = foot.corners[k].texel % 7;
					const uint32_t ty = foot.corners[k].texel / 7;
					r.check(tx < 7 && ty < 5, "padded texels are never addressed");
				}
			}
	}

	void sampling_trilinear(Registry& r)
	{
		std::vector<image_u8> levels;
		levels.push_back(make_image(8, 8, 5));
		levels.push_back(make_image(4, 4, 6));
		levels.push_back(make_image(2, 2, 7));
		levels.push_back(make_image(1, 1, 8));

		std::vector<bc5_normal::Sample> samples;
		samples.push_back(bc5_normal::Sample(0.37, 0.61, 2.5, 1.0));   // blends 2x2 and the 1x1 tail
		samples.push_back(bc5_normal::Sample(0.81, 0.22, 1.25, 0.6));
		samples.push_back(bc5_normal::Sample(0.13, 0.91, 99.0, 1.0));   // lod clamps to last level
		samples.push_back(bc5_normal::Sample(0.55, 0.44, -3.0, 1.0));   // lod clamps to level 0
		test_sampling_calibration(r, "sampling/trilinear", levels, samples, false);

		const RefFoot foot = ref_footprint(0.37, 0.61, 1, 1, false);
		r.check(foot.corners.size() == 1 && foot.corners[0].texel == 0, "1x1 footprint collapses to its only texel");
		r.check(std::fabs(foot.corners[0].weight - 1.0) < 1e-15, "1x1 footprint weight is 1");

		// The inter-level blend must happen in filtered RG space, not on
		// rebuilt normals: build the same fractional-LOD source both ways and
		// show the orders diverge. Use strongly opposing per-level normals so
		// the distinction cannot be swallowed by rounding.
		image_u8 strong0(4, 4), strong1(2, 2);
		for (uint32_t y = 0; y < 4; y++)
			for (uint32_t x = 0; x < 4; x++)
				strong0(x, y) = color_quad_u8(230, 40, 0, 255);
		for (uint32_t y = 0; y < 2; y++)
			for (uint32_t x = 0; x < 2; x++)
				strong1(x, y) = color_quad_u8(40, 220, 0, 255);
		const bc5_normal::Sample tri(0.5, 0.5, 0.5, 1.0); // equal weight on the two levels
		{
			const RefFoot f0 = ref_footprint(tri.u, tri.v, 4, 4, false);
			const RefFoot f1 = ref_footprint(tri.u, tri.v, 2, 2, false);
			double rg0[2], rg1[2];
			ref_filtered(&strong0, nullptr, 4, f0, rg0);
			ref_filtered(&strong1, nullptr, 2, f1, rg1);

			// Correct: blend RG first, rebuild once.
			const Vec3 right = normal_from_rg(0.5 * rg0[0] + 0.5 * rg1[0], 0.5 * rg0[1] + 0.5 * rg1[1]);
			// Wrong: rebuild a normal per level, then blend normals.
			const Vec3 n0 = normal_from_rg(rg0[0], rg0[1]);
			const Vec3 n1 = normal_from_rg(rg1[0], rg1[1]);
			const double diff = std::fabs(0.5 * (n0.x + n1.x) - right.x)
				+ std::fabs(0.5 * (n0.y + n1.y) - right.y);
			r.check(diff > 1e-6, "trilinear RG-space blend differs from normal-space blending");
		}
	}

	void sampling_filter_order(Registry& r)
	{
		// Two strongly asymmetric texels so that "filter then rebuild" and
		// "rebuild then filter" observably diverge.
		image_u8 im(4, 4);
		for (uint32_t y = 0; y < 4; y++)
			for (uint32_t x = 0; x < 4; x++)
				im(x, y) = (x < 2) ? color_quad_u8(230, 40, 0, 255) : color_quad_u8(30, 210, 0, 255);

		std::vector<image_u8> levels;
		levels.push_back(im);
		const std::vector<bc5_normal::Sample> samples(1, bc5_normal::Sample(0.5, 0.5, 0.0, 1.0));

		bc5_normal::Options opt;
		opt.max_sweeps = 1;
		bc5_normal::Result res;
		r.check(bc5_normal::optimize(levels, samples, opt, res), "sampling/filter_order");

		const std::vector<uint8_t> flat = hq_start_bytes(levels);
		const std::vector<std::vector<uint8_t> > blocks = split_level_bytes(levels, flat);
		const double spec_order = ref_total_loss(levels, blocks, samples, false);

		// Wrong-order reference for the same footprint.
		const RefFoot foot = ref_footprint(0.5, 0.5, 4, 4, false);
		Vec3 wrong_src = { 0.0, 0.0, 0.0 };
		for (size_t k = 0; k < foot.corners.size(); k++)
		{
			const uint32_t tx = foot.corners[k].texel % 4;
			const uint32_t ty = foot.corners[k].texel / 4;
			const Vec3 n = normal_from_rg(im(tx, ty).r / 255.0, im(tx, ty).g / 255.0);
			wrong_src.x += foot.corners[k].weight * n.x;
			wrong_src.y += foot.corners[k].weight * n.y;
			wrong_src.z += foot.corners[k].weight * n.z;
		}
		double srg[2];
		ref_filtered(&im, nullptr, 4, foot, srg);
		const Vec3 right_src = normal_from_rg(srg[0], srg[1]);
		r.check(std::fabs(wrong_src.x - right_src.x) + std::fabs(wrong_src.y - right_src.y) > 1e-9,
			"filter-first vs rebuild-first produce different source normals");

		r.check(close_enough(spec_order, res.initial_error), "production follows the filter-first order");
	}

	void sampling_bc4_layout(Registry& r)
	{
		// Explicit numeric calibration of both BC4 palettes and selector 15.
		uint8_t values8[8], values6[8];
		RefBC4::palette(values8, 200, 40);   // 8 value mode
		RefBC4::palette(values6, 40, 200);   // 6 value mode
		r.check(values8[0] == 200 && values8[1] == 40, "8-value mode endpoints");
		r.check(values8[2] == static_cast<uint8_t>((6 * 200 + 40) / 7), "8-value interpolation entry 2");
		r.check(values8[7] == static_cast<uint8_t>((200 + 6 * 40) / 7), "8-value interpolation entry 7");
		r.check(values6[0] == 40 && values6[1] == 200, "6-value mode endpoints");
		r.check(values6[6] == 0 && values6[7] == 255, "6-value mode hardwired selectors 6/7");

		uint8_t sel[6];
		std::memset(sel, 0, sizeof(sel));
		RefBC4::set_selector(sel, 15, 7);
		r.check(RefBC4::selector_of(sel, 15) == 7, "pixel 15 selector round trips through 3-bit packing");
		r.check(RefBC4::selector_of(sel, 14) == 0, "neighbor selector untouched by pixel 15 write");

		// A full sample landing exactly on texel 15 must decode selector 15.
		const image_u8 im = make_flat_image(4, 4, 128, 128);
		std::vector<image_u8> levels;
		levels.push_back(im);
		std::vector<uint8_t> flat = hq_start_bytes(levels);
		RefBC4::set_selector(flat.data() + 2, 15, 7);
		RefBC4::set_selector(flat.data() + 8 + 2, 15, 7);
		const std::vector<std::vector<uint8_t> > blocks = split_level_bytes(levels, flat);

		const bc5_normal::Sample at15(3.5 / 4.0, 3.5 / 4.0, 0.0, 1.0);
		const RefFoot foot = ref_footprint(at15.u, at15.v, 4, 4, false);
		r.check(foot.corners.size() == 1 && foot.corners[0].texel == 15, "texel-center sample isolates pixel 15");
		const double loss8 = ref_sample_loss(levels, blocks, at15, false);
		r.check(std::isfinite(loss8), "selector-15 sample evaluates through the reference");
	}

	// ===================================================================
	// transactions group
	// ===================================================================

	// Slow, independent reimplementation of one production sweep.
	struct SlowSweep
	{
		const std::vector<image_u8>& levels;
		const std::vector<bc5_normal::Sample>& samples;
		std::vector<std::vector<uint8_t> > blocks;
		uint64_t expected_evals;
		uint64_t accepted;
		bool saw_mode_switch_candidate;

		SlowSweep(const std::vector<image_u8>& lv, const std::vector<bc5_normal::Sample>& sm,
			const std::vector<uint8_t>& start) :
			levels(lv), samples(sm), expected_evals(0), accepted(0), saw_mode_switch_candidate(false)
		{
			blocks = split_level_bytes(lv, start);
		}

		double list_error(const std::vector<uint32_t>& ids) const
		{
			double total = 0.0;
			for (size_t i = 0; i < ids.size(); i++)
				total += ref_sample_loss(levels, blocks, samples[ids[i]], false);
			return total;
		}

		std::vector<uint32_t> texel_assoc(uint32_t level, uint32_t texel)
		{
			std::set<uint32_t> ids;
			for (uint32_t s = 0; s < samples.size(); s++)
			{
				bc5_normal::Sample sm = samples[s];
				if (sm.lod < 0.0) sm.lod = 0.0;
				if (sm.lod > static_cast<double>(levels.size() - 1))
					sm.lod = static_cast<double>(levels.size() - 1);
				const uint32_t l0 = static_cast<uint32_t>(std::floor(sm.lod));
				uint32_t l1 = l0 + 1;
				if (l1 >= levels.size()) l1 = l0;
				for (uint32_t q = 0; q < 2; q++)
				{
					const uint32_t ll = (q == 0) ? l0 : l1;
					if (ll != level || (q == 1 && l1 == l0)) continue;
					const RefFoot foot = ref_footprint(std::min(std::max(sm.u, 0.0), 1.0),
						std::min(std::max(sm.v, 0.0), 1.0), levels[ll].width(), levels[ll].height(), false);
					for (size_t k = 0; k < foot.corners.size(); k++)
						if (foot.corners[k].texel == texel) { ids.insert(s); break; }
				}
			}
			return std::vector<uint32_t>(ids.begin(), ids.end());
		}

		std::vector<uint32_t> block_assoc(uint32_t level, uint32_t block)
		{
			const image_u8& im = levels[level];
			const uint32_t bw = (im.width() + 3) / 4;
			std::set<uint32_t> ids;
			const uint32_t x0 = (block % bw) * 4;
			const uint32_t y0 = (block / bw) * 4;
			for (uint32_t ty = 0; ty < std::min(4u, im.height() - y0); ty++)
				for (uint32_t tx = 0; tx < std::min(4u, im.width() - x0); tx++)
				{
					const uint32_t texel = (y0 + ty) * im.width() + (x0 + tx);
					const std::vector<uint32_t> a = texel_assoc(level, texel);
					ids.insert(a.begin(), a.end());
				}
			return std::vector<uint32_t>(ids.begin(), ids.end());
		}

		void run()
		{
			expected_evals += static_cast<uint64_t>(samples.size()); // initial full pass

			for (size_t l = 0; l < levels.size(); l++)
			{
				const image_u8& im = levels[l];
				const uint32_t bw = (im.width() + 3) / 4;
				const uint32_t bh = (im.height() + 3) / 4;

				// Selector coordinates, texel row order.
				for (uint32_t ty = 0; ty < im.height(); ty++)
					for (uint32_t tx = 0; tx < im.width(); tx++)
					{
						const uint32_t texel = ty * im.width() + tx;
						const std::vector<uint32_t> assoc = texel_assoc(static_cast<uint32_t>(l), texel);
						if (assoc.empty()) continue;

						const uint32_t block = (ty / 4) * bw + (tx / 4);
						const uint32_t local = (ty % 4) * 4 + (tx % 4);
						uint8_t* p = blocks[l].data() + block * 16;
						uint8_t saved[12];
						std::memcpy(saved, p + 2, 6);
						std::memcpy(saved + 6, p + 10, 6);
						const double orig = list_error(assoc);

						double best = std::numeric_limits<double>::infinity();
						uint32_t best_key = 64;
						for (uint32_t sy = 0; sy < 8; sy++)
							for (uint32_t sx = 0; sx < 8; sx++)
							{
								RefBC4::set_selector(p + 2, local, sx);
								RefBC4::set_selector(p + 10, local, sy);
								expected_evals += assoc.size();
								const double e = list_error(assoc);
								const uint32_t key = 8 * sx + sy;
								if (e < best - kEps || (std::fabs(e - best) <= kEps && key < best_key))
								{
									best = e;
									best_key = key;
								}
							}
						std::memcpy(p + 2, saved, 6);
						std::memcpy(p + 10, saved + 6, 6);
						if (orig <= best + kEps || !(orig - best > kEps)) continue;
						RefBC4::set_selector(p + 2, local, best_key / 8);
						RefBC4::set_selector(p + 10, local, best_key % 8);
						accepted++;
					}

				// Endpoint coordinates, block row order.
				static const uint32_t kOff[4] = { 0, 1, 8, 9 };
				for (uint32_t by = 0; by < bh; by++)
					for (uint32_t bx = 0; bx < bw; bx++)
					{
						const uint32_t block = by * bw + bx;
						const std::vector<uint32_t> assoc = block_assoc(static_cast<uint32_t>(l), block);
						if (assoc.empty()) continue;
						uint8_t* p = blocks[l].data() + block * 16;
						for (uint32_t oi = 0; oi < 4; oi++)
						{
							const uint32_t off = kOff[oi];
							const uint8_t orig_byte = p[off];
							const bool was_six_mode = p[0] <= p[1];
							const double orig = list_error(assoc);

							std::vector<uint8_t> cand;
							for (int d = -1; d <= 1; d++)
							{
								const int v = static_cast<int>(orig_byte) + d;
								if (v >= 0 && v <= 255) cand.push_back(static_cast<uint8_t>(v));
							}
							double best = std::numeric_limits<double>::infinity();
							uint32_t best_byte = 256;
							for (size_t q = 0; q < cand.size(); q++)
							{
								p[off] = cand[q];
								const bool now_six_mode = p[0] <= p[1];
								if (now_six_mode != was_six_mode)
									saw_mode_switch_candidate = true;
								expected_evals += assoc.size();
								const double e = list_error(assoc);
								if (e < best - kEps || (std::fabs(e - best) <= kEps && cand[q] < best_byte))
								{
									best = e;
									best_byte = cand[q];
								}
							}
							p[off] = orig_byte;
							if (orig <= best + kEps || !(orig - best > kEps)) continue;
							p[off] = static_cast<uint8_t>(best_byte);
							accepted++;
						}
					}
			}

			expected_evals += static_cast<uint64_t>(samples.size()); // final full verification
		}
	};

	void transactions_one_sweep(Registry& r)
	{
		std::vector<image_u8> levels;
		levels.push_back(make_image(5, 5, 9));

		std::vector<bc5_normal::Sample> samples;
		for (uint32_t i = 0; i < 5; i++)
			for (uint32_t j = 0; j < 5; j++)
				samples.push_back(bc5_normal::Sample((i + 0.5) / 5.0, (j + 0.5) / 5.0, 0.0, 1.0));
		samples.push_back(bc5_normal::Sample(4.25 / 5.0, 3.75 / 5.0, 0.0, 2.0));

		bc5_normal::Options opt;
		opt.max_sweeps = 1;
		bc5_normal::Result res;
		r.check(bc5_normal::optimize(levels, samples, opt, res), "transactions/one_sweep optimize");

		const std::vector<uint8_t> start = hq_start_bytes(levels);
		SlowSweep slow(levels, samples, start);
		slow.run();

		r.check(slow.blocks[0] == res.blocks[0], "one sweep matches the slow candidate traversal byte for byte");
		r.check(slow.accepted == res.accepted_moves, "accepted move count matches slow traversal");
		r.check(slow.expected_evals == res.loss_evaluations, "loss evaluations only touch intersecting footprints");
		r.check(res.sweeps == 1, "exactly one sweep completed");

		// A naive implementation would re-evaluate every sample for every candidate.
		const uint32_t coords = levels[0].width() * levels[0].height()
			+ 4 * ((levels[0].width() + 3) / 4) * ((levels[0].height() + 3) / 4);
		const uint64_t naive = 2 * static_cast<uint64_t>(samples.size())
			+ static_cast<uint64_t>(coords) * 64 * samples.size();
		r.check(res.loss_evaluations < naive, "unrelated samples are not recomputed per candidate");
	}

	void transactions_rollback_and_modes(Registry& r)
	{
		// A flat region forces 6-value BC4 blocks with equal endpoints, so
		// endpoint +/-1 candidates cross into the 8-value interpretation.
		std::vector<image_u8> levels;
		levels.push_back(make_flat_image(8, 4, 130, 110));

		std::vector<bc5_normal::Sample> samples;
		for (uint32_t i = 0; i < 8; i++)
			for (uint32_t j = 0; j < 4; j++)
				samples.push_back(bc5_normal::Sample((i + 0.5) / 8.0, (j + 0.5) / 4.0, 0.0, 1.0));

		const std::vector<uint8_t> start = hq_start_bytes(levels);
		r.check(start[0] == start[1], "flat R block starts in 6-value mode with equal endpoints");

		SlowSweep slow(levels, samples, start);
		slow.run();
		r.check(slow.saw_mode_switch_candidate, "endpoint candidates cross the 6/8 palette boundary");

		bc5_normal::Options opt;
		opt.max_sweeps = 1;
		bc5_normal::Result res;
		r.check(bc5_normal::optimize(levels, samples, opt, res), "transactions/rollback optimize");
		r.check(res.blocks[0] == slow.blocks[0], "production and slow traversal agree after mode-switch candidates");

		const double ref_final = ref_total_loss(levels, res.blocks, samples, false);
		r.check(close_enough(ref_final, res.final_error), "rolled-back candidates leave a coherent cache/stream pair");

		bc5_normal::Result again;
		r.check(bc5_normal::optimize(levels, samples, opt, again), "rerun");
		r.check(again.blocks == res.blocks && again.loss_evaluations == res.loss_evaluations
			&& again.accepted_moves == res.accepted_moves, "repeated runs are byte and statistic identical");
	}

	// ===================================================================
	// convergence group
	// ===================================================================

	void convergence_local_optimum(Registry& r)
	{
		std::vector<image_u8> levels;
		levels.push_back(make_image(4, 4, 11));

		std::vector<bc5_normal::Sample> samples;
		for (uint32_t i = 0; i < 4; i++)
			for (uint32_t j = 0; j < 4; j++)
				samples.push_back(bc5_normal::Sample((i + 0.5) / 4.0, (j + 0.5) / 4.0, 0.0, 1.0));

		bc5_normal::Options opt;
		opt.max_sweeps = 64;
		bc5_normal::Result res;
		r.check(bc5_normal::optimize(levels, samples, opt, res), "convergence/local_optimum");
		r.check(res.converged, "optimizer reports convergence");
		r.check(res.sweeps <= 64, "sweep count stays within the cap");

		std::vector<std::vector<uint8_t> > work = res.blocks;
		const double current = ref_total_loss(levels, work, samples, false);
		r.check(close_enough(current, res.final_error), "reference recomputes the converged error");

		uint8_t* p = work[0].data();
		for (uint32_t pixel = 0; pixel < 16; pixel++)
		{
			uint8_t saved[12];
			std::memcpy(saved, p + 2, 6);
			std::memcpy(saved + 6, p + 10, 6);
			for (uint32_t sy = 0; sy < 8; sy++)
				for (uint32_t sx = 0; sx < 8; sx++)
				{
					RefBC4::set_selector(p + 2, pixel, sx);
					RefBC4::set_selector(p + 10, pixel, sy);
					const double e = ref_total_loss(levels, work, samples, false);
					r.check(!(current - e > kEps), "no selector pair improves the converged state");
				}
			std::memcpy(p + 2, saved, 6);
			std::memcpy(p + 10, saved + 6, 6);
		}

		static const uint32_t kOff[4] = { 0, 1, 8, 9 };
		for (uint32_t oi = 0; oi < 4; oi++)
		{
			const uint8_t orig = p[kOff[oi]];
			for (int d = -1; d <= 1; d++)
			{
				const int v = static_cast<int>(orig) + d;
				if (v < 0 || v > 255) continue;
				p[kOff[oi]] = static_cast<uint8_t>(v);
				const double e = ref_total_loss(levels, work, samples, false);
				r.check(!(current - e > kEps), "no endpoint +/-1 move improves the converged state");
			}
			p[kOff[oi]] = orig;
		}
	}

	// Builds the shared-footprint counterexample. Texel A=(0,0) is shared by a
	// horizontal sample S1 (A+B) and a vertical sample S2 (A+C). A deterministic
	// seed search finds an image where some selector move at A lowers S1 alone
	// but does not strictly lower the merged S1+S2 loss. Both judgments and the
	// exact inputs are printed so the example stands on its own.
	struct SharedFootprintCase
	{
		bool found;
		uint32_t seed;
		uint32_t hq_key;
		uint32_t candidate_key;
		double s1_base, s1_move, combined_base, combined_move;
	};

	SharedFootprintCase build_shared_footprint_case(Registry& r)
	{
		SharedFootprintCase sc;
		sc.found = false;
		sc.seed = 0;
		sc.hq_key = 64;
		sc.candidate_key = 64;

		const bc5_normal::Sample s1(0.25, 0.125, 0.0, 1.0); // gx=0.5, gy=0   -> (0,0)+(1,0)
		const bc5_normal::Sample s2(0.125, 0.25, 0.0, 1.0); // gx=0,   gy=0.5 -> (0,0)+(0,1)
		std::vector<bc5_normal::Sample> two;
		two.push_back(s1);
		two.push_back(s2);

		for (uint32_t seed = 1; seed <= 256 && !sc.found; seed++)
		{
			std::vector<image_u8> levels;
			levels.push_back(make_image(4, 4, seed));

			const std::vector<uint8_t> start = hq_start_bytes(levels);
			std::vector<std::vector<uint8_t> > work = split_level_bytes(levels, start);

			const double base1 = ref_sample_loss(levels, work, s1, false);
			const double base2 = ref_sample_loss(levels, work, s2, false);
			const double base_combined = base1 + base2;

			uint8_t* p = work[0].data();
			const uint32_t sx0 = RefBC4::selector_of(p + 2, 0);
			const uint32_t sy0 = RefBC4::selector_of(p + 10, 0);
			const uint32_t hq_key = 8 * sx0 + sy0;

			// First pass: the HQ state must itself be a combined local optimum,
			// i.e. no selector pair strictly improves the merged footprint.
			bool hq_is_combined_optimum = true;
			for (uint32_t sy = 0; sy < 8; sy++)
				for (uint32_t sx = 0; sx < 8; sx++)
				{
					const uint32_t key = 8 * sx + sy;
					if (key == hq_key) continue;
					RefBC4::set_selector(p + 2, 0, sx);
					RefBC4::set_selector(p + 10, 0, sy);
					const double e = ref_sample_loss(levels, work, s1, false)
						+ ref_sample_loss(levels, work, s2, false);
					if (base_combined - e > kEps)
						hq_is_combined_optimum = false;
				}
			if (!hq_is_combined_optimum)
				continue;

			// Second pass: find a pair that S1 alone would accept.
			for (uint32_t sy = 0; sy < 8 && !sc.found; sy++)
				for (uint32_t sx = 0; sx < 8 && !sc.found; sx++)
				{
					const uint32_t key = 8 * sx + sy;
					if (key == hq_key) continue;

					RefBC4::set_selector(p + 2, 0, sx);
					RefBC4::set_selector(p + 10, 0, sy);
					const double e1 = ref_sample_loss(levels, work, s1, false);
					const double e2 = ref_sample_loss(levels, work, s2, false);
					const double s1_gain = base1 - e1;
					const double combined_delta = base_combined - (e1 + e2);

					if (s1_gain > kEps && !(combined_delta > kEps))
					{
						sc.found = true;
						sc.seed = seed;
						sc.hq_key = hq_key;
						sc.candidate_key = key;
						sc.s1_base = base1;
						sc.s1_move = e1;
						sc.combined_base = base_combined;
						sc.combined_move = e1 + e2;
					}
				}
		}

		r.check(sc.found, "deterministic seed search finds a privately beneficial but jointly rejected selector move");
		if (sc.found)
		{
			std::printf("[convergence/shared_footprint] make_image seed=%u hq 8*sx+sy=%u candidate=%u\n",
				sc.seed, sc.hq_key, sc.candidate_key);
		}
		return sc;
	}

	void convergence_shared_footprint(Registry& r)
	{
		const SharedFootprintCase sc = build_shared_footprint_case(r);
		r.check(sc.found, "shared footprint case available");

		std::vector<image_u8> levels;
		levels.push_back(make_image(4, 4, sc.seed));

		std::vector<bc5_normal::Sample> two;
		two.push_back(bc5_normal::Sample(0.25, 0.125, 0.0, 1.0));
		two.push_back(bc5_normal::Sample(0.125, 0.25, 0.0, 1.0));

		const std::vector<uint8_t> start = hq_start_bytes(levels);
		const std::vector<std::vector<uint8_t> > start_blocks = split_level_bytes(levels, start);
		const uint32_t sx0 = RefBC4::selector_of(start_blocks[0].data() + 2, 0);
		const uint32_t sy0 = RefBC4::selector_of(start_blocks[0].data() + 10, 0);
		r.check(8 * sx0 + sy0 == sc.hq_key, "recorded HQ selector matches the rebuilt image");

		bc5_normal::Options opt;
		opt.max_sweeps = 1;
		bc5_normal::Result res;
		r.check(bc5_normal::optimize(levels, two, opt, res), "shared footprint optimize");
		const uint32_t sx_after = RefBC4::selector_of(res.blocks[0].data() + 2, 0);
		const uint32_t sy_after = RefBC4::selector_of(res.blocks[0].data() + 10, 0);
		r.check(8 * sx_after + sy_after == sc.hq_key,
			"the privately beneficial but jointly harmful selector move is rejected; texel A keeps its HQ selectors");

		// Independent confirmation of the two competing judgments.
		std::vector<std::vector<uint8_t> > probe = start_blocks;
		RefBC4::set_selector(probe[0].data() + 2, 0, sc.candidate_key / 8);
		RefBC4::set_selector(probe[0].data() + 10, 0, sc.candidate_key % 8);
		const double e1_base = ref_sample_loss(levels, start_blocks, two[0], false);
		const double e1_move = ref_sample_loss(levels, probe, two[0], false);
		const double combined_base = ref_total_loss(levels, start_blocks, two, false);
		const double combined_move = ref_total_loss(levels, probe, two, false);
		std::printf("[convergence/shared_footprint] single-sample judgment: %.6g -> %.6g (gain %.6g); "
			"merged judgment: %.6g -> %.6g (delta %.6g)\n",
			e1_base, e1_move, e1_base - e1_move, combined_base, combined_move, combined_base - combined_move);
		r.check(std::fabs(e1_base - sc.s1_base) < 1e-18 && std::fabs(combined_base - sc.combined_base) < 1e-18,
			"reported judgments are reproducible from the recorded inputs");
		r.check(e1_base - e1_move > kEps, "single affected sample would accept the move");
		r.check(!(combined_base - combined_move > kEps), "merged footprint rejects the move");
	}

	// ===================================================================
	// integration group
	// ===================================================================

	void integration_encoder(Registry& r)
	{
		image_u8 src(7, 5);
		for (uint32_t y = 0; y < 5; y++)
			for (uint32_t x = 0; x < 7; x++)
				src(x, y) = color_quad_u8(
					static_cast<uint8_t>(30 + x * 19 + y * 7),
					static_cast<uint8_t>(40 + x * 11 + y * 23),
					static_cast<uint8_t>(20 + x * 29 + y * 13),
					255);

		std::vector<bc5_normal::Sample> samples;
		for (uint32_t i = 0; i < 6; i++)
			for (uint32_t j = 0; j < 4; j++)
				samples.push_back(bc5_normal::Sample((i + 0.5) / 6.0, (j + 0.5) / 4.0, 0.0, 1.0));

		rdo_bc::rdo_bc_params params;
		params.m_dxgi_format = DXGI_FORMAT_BC5_UNORM;
		params.m_rdo_lambda = 0.0f;
		params.m_status_output = false;
		params.m_y_flip = true;
		params.m_bc45_channel0 = 2; // B -> R slot
		params.m_bc45_channel1 = 0; // R -> G slot
		params.m_bc5_filtered_normals = true;
		params.m_bc5_normal_repeat = false;
		params.m_bc5_normal_sweeps = 3;
		params.m_bc5_normal_samples = samples;

		rdo_bc::rdo_bc_encoder enc;
		r.check(enc.init(src, params) && enc.encode(), "integration encoder runs with flip, swizzle, odd size");

		const uint32_t total_blocks = enc.get_total_blocks();
		r.check(total_blocks == 2 * 2, "7x5 occupies a 2x2 block grid");
		const uint8_t* prerdo = static_cast<const uint8_t*>(enc.get_prerdo_blocks());
		const uint8_t* final_blocks = static_cast<const uint8_t*>(enc.get_blocks());

		// Recreate the logical image the optimizer should have seen:
		// y_flip first, then the channel swizzle, with no padding.
		image_u8 logical(7, 5);
		for (uint32_t y = 0; y < 5; y++)
			for (uint32_t x = 0; x < 7; x++)
			{
				const color_quad_u8& c = src(x, 4 - y);
				logical(x, y) = color_quad_u8(c.m_c[2], c.m_c[0], c.m_c[2], c.m_c[3]);
			}

		std::vector<image_u8> levels;
		levels.push_back(logical);
		const std::vector<uint8_t> expected_hq = hq_start_bytes(levels);
		r.check(std::memcmp(prerdo, expected_hq.data(), expected_hq.size()) == 0,
			"get_prerdo_blocks returns the pre-optimization HQ bytes");

		bc5_normal::Options opt;
		opt.max_sweeps = 3;
		bc5_normal::Result core;
		r.check(bc5_normal::optimize(levels, samples, opt, core), "independent core");
		r.check(core.blocks.size() == 1 && core.blocks[0].size() == expected_hq.size(), "core one-level sizes");
		r.check(std::memcmp(final_blocks, core.blocks[0].data(), core.blocks[0].size()) == 0,
			"get_blocks returns the shared optimizer core output");

		// Repeated encoder runs are byte identical.
		rdo_bc::rdo_bc_encoder enc2;
		r.check(enc2.init(src, params) && enc2.encode(), "second encoder run");
		r.check(std::memcmp(enc2.get_blocks(), final_blocks, expected_hq.size()) == 0,
			"encoder output is deterministic");

		// Default-off path stays byte compatible with the legacy HQ encoder.
		rdo_bc::rdo_bc_params legacy = params;
		legacy.m_bc5_filtered_normals = false;
		legacy.m_bc345_search_rad = rgbcx::BC4_DEFAULT_SEARCH_RAD;
		rdo_bc::rdo_bc_encoder enc3;
		r.check(enc3.init(src, legacy) && enc3.encode(), "legacy path still runs");

		image_u8 flipped(7, 5);
		for (uint32_t y = 0; y < 5; y++)
			for (uint32_t x = 0; x < 7; x++)
				flipped(x, y) = src(x, 4 - y);
		image_u8 legacy_padded(8, 8);
		for (uint32_t y = 0; y < 8; y++)
			for (uint32_t x = 0; x < 8; x++)
				legacy_padded(x, y) = flipped(std::min(x, 6u), std::min(y, 4u));
		std::vector<uint8_t> expected_legacy;
		for (uint32_t b = 0; b < 4; b++)
		{
			color_quad_u8 pix[16];
			legacy_padded.get_block(b % 2, b / 2, 4, 4, pix);
			uint8_t block[16];
			// The legacy path honors the configured channel selection but not
			// the filtered-normal swizzle path.
			rgbcx::encode_bc5_hq(block, &pix[0].m_c[0], 2, 0, 4, rgbcx::BC4_DEFAULT_SEARCH_RAD, rgbcx::BC4_USE_ALL_MODES);
			expected_legacy.insert(expected_legacy.end(), block, block + 16);
		}
		r.check(std::memcmp(enc3.get_blocks(), expected_legacy.data(), expected_legacy.size()) == 0,
			"default-off output remains byte compatible with legacy HQ");

		// Incompatible modes are rejected.
		rdo_bc::rdo_bc_params bad_fmt = params;
		bad_fmt.m_dxgi_format = DXGI_FORMAT_BC4_UNORM;
		rdo_bc::rdo_bc_encoder e_bad;
		r.check(e_bad.init(src, bad_fmt) && !e_bad.encode(), "non-BC5 format rejected");

		rdo_bc::rdo_bc_params bad_lambda = params;
		bad_lambda.m_rdo_lambda = 1.0f;
		rdo_bc::rdo_bc_encoder e_lambda;
		r.check(e_lambda.init(src, bad_lambda) && !e_lambda.encode(), "nonzero lambda rejected");

		rdo_bc::rdo_bc_params bad_samples = params;
		bad_samples.m_bc5_normal_samples.clear();
		rdo_bc::rdo_bc_encoder e_samples;
		r.check(e_samples.init(src, bad_samples) && !e_samples.encode(), "empty sample list rejected");
	}

	void integration_core_validation(Registry& r)
	{
		std::vector<image_u8> levels;
		levels.push_back(make_image(4, 4, 13));
		const std::vector<bc5_normal::Sample> samples(1, bc5_normal::Sample(0.5, 0.5, 0.0, 1.0));
		bc5_normal::Options opt;

		bc5_normal::Result sentinel;
		sentinel.initial_error = 7.5;
		sentinel.sweeps = 42;
		sentinel.converged = true;
		const bc5_normal::Result snapshot = sentinel;

		std::vector<image_u8> empty;
		r.check(!bc5_normal::optimize(empty, samples, opt, sentinel), "empty level list rejected");
		std::vector<bc5_normal::Sample> bad_weight = samples;
		bad_weight[0].weight = -1.0;
		r.check(!bc5_normal::optimize(levels, bad_weight, opt, sentinel), "non-positive weight rejected");
		std::vector<bc5_normal::Sample> bad_nan = samples;
		bad_nan[0].lod = std::nan("");
		r.check(!bc5_normal::optimize(levels, bad_nan, opt, sentinel), "non-finite sample rejected");
		bc5_normal::Options bad_sweeps;
		bad_sweeps.max_sweeps = 0;
		r.check(!bc5_normal::optimize(levels, samples, bad_sweeps, sentinel), "max_sweeps 0 rejected");
		bad_sweeps.max_sweeps = 65;
		r.check(!bc5_normal::optimize(levels, samples, bad_sweeps, sentinel), "max_sweeps 65 rejected");

		std::vector<image_u8> illegal;
		illegal.push_back(make_image(4, 4, 1));
		illegal.push_back(make_image(1, 1, 2));
		illegal.push_back(make_image(1, 1, 3)); // appended after a 1x1 level
		r.check(!bc5_normal::optimize(illegal, samples, opt, sentinel), "level after 1x1 rejected");

		std::vector<image_u8> wrong;
		wrong.push_back(make_image(4, 4, 1));
		wrong.push_back(make_image(3, 3, 2)); // legal mip would be 2x2
		r.check(!bc5_normal::optimize(wrong, samples, opt, sentinel), "illegal mip dimensions rejected");

		r.check(sentinel.initial_error == snapshot.initial_error && sentinel.sweeps == snapshot.sweeps
			&& sentinel.converged == snapshot.converged && sentinel.blocks == snapshot.blocks,
			"failed calls leave the result untouched");
	}

} // anonymous namespace

void register_task_tests(Registry& r)
{
	r.add("sampling", "cross_block", std::bind(&sampling_cross_block, std::ref(r)));
	r.add("sampling", "repeat_seam", std::bind(&sampling_repeat_seam, std::ref(r)));
	r.add("sampling", "clamp_edge", std::bind(&sampling_clamp_edge, std::ref(r)));
	r.add("sampling", "odd_size", std::bind(&sampling_odd_size, std::ref(r)));
	r.add("sampling", "trilinear", std::bind(&sampling_trilinear, std::ref(r)));
	r.add("sampling", "filter_order", std::bind(&sampling_filter_order, std::ref(r)));
	r.add("sampling", "trilinear_layout", std::bind(&sampling_bc4_layout, std::ref(r)));

	r.add("transactions", "one_sweep", std::bind(&transactions_one_sweep, std::ref(r)));
	r.add("transactions", "rollback_and_modes", std::bind(&transactions_rollback_and_modes, std::ref(r)));

	r.add("convergence", "local_optimum", std::bind(&convergence_local_optimum, std::ref(r)));
	r.add("convergence", "shared_footprint", std::bind(&convergence_shared_footprint, std::ref(r)));

	r.add("integration", "encoder", std::bind(&integration_encoder, std::ref(r)));
	r.add("integration", "core_validation", std::bind(&integration_core_validation, std::ref(r)));
}
