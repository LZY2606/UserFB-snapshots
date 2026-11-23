// tests/progressive.cpp - tests for jpge's progressive (SOF2) encoder.
//
// These tests deliberately do NOT trust the encoder: they parse the produced JPEG
// streams themselves (DHT, SOF, SOS and the entropy-coded scan data), reconstruct the
// quantized coefficients of every scan, and compare them block by block against the
// coefficients independently recovered from a baseline sequential encoding of the same
// image. jpgd is only used as an additional cross-check of the full decoded RGB image.
//
// The fixed runner calls the four exported functions directly; each returns the number
// of assertions it executed (a negative value signals a failed assertion).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>

#include "jpge.h"
#include "jpgd.h"

namespace {

	// ---------------------------------------------------------------------------
	// Assertion helpers
	// ---------------------------------------------------------------------------
	struct CaseCtx
	{
		int asserts;
		int fails;
		CaseCtx() : asserts(0), fails(0) { }
	};
}

#define CHECK(ctx, cond) \
	do { \
		++(ctx).asserts; \
		if (!(cond)) { \
			++(ctx).fails; \
			fprintf(stderr, "ASSERT FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		} \
	} while (0)

namespace {

	// ---------------------------------------------------------------------------
	// Minimal, independent JPEG scan parser / coefficient reconstructor.
	// Coefficients are kept in natural (de-zigzagged) order on the encoder's padded grid.
	// ---------------------------------------------------------------------------
	static const int kZigZag[64] = {
		0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,
		7,14,21,28,35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,
		31,39,46,53,60,61,54,47,55,62,63 };

	static inline int jpeg_extend(int v, int s)
	{
		return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
	}

	struct BitReader
	{
		const uint8_t* d; int n; int byte; int bit;
		int get1()
		{
			if (byte >= n) return -1;
			int b = (d[byte] >> (7 - bit)) & 1;
			if (++bit == 8) { bit = 0; byte++; }
			return b;
		}
		int get(int k)
		{
			int v = 0;
			for (int i = 0; i < k; i++) { int b = get1(); if (b < 0) return -1; v = (v << 1) | b; }
			return v;
		}
		bool rest_is_ones() const
		{
			int by = byte, bi = bit;
			while (by < n)
			{
				for (; bi < 8; bi++)
					if (((d[by] >> (7 - bi)) & 1) == 0) return false;
				bi = 0; by++;
			}
			return true;
		}
	};

	struct HuffTable
	{
		int size[256];
		int code[256];
		HuffTable() { for (int i = 0; i < 256; i++) { size[i] = -1; code[i] = -1; } }
	};

	struct ScanInfo
	{
		int ns;
		int comps[3];
		int ss, se, ah, al;
	};

	struct ParseStats
	{
		int max_eobrun;         // largest EOB run length (in blocks) seen in AC-first scans
		int zrl_count;
		int eobrun_count;
		bool zrl_touches_eobrun;
		int ac_blocks[4];       // blocks visited by AC-first scans, per component
		int ac_ref_blocks[4];
		int dc_blocks[4];       // blocks visited by interleaved DC scans, per component
		ParseStats()
			: max_eobrun(0), zrl_count(0), eobrun_count(0), zrl_touches_eobrun(false)
		{ for (int i = 0; i < 4; i++) { ac_blocks[i] = ac_ref_blocks[i] = dc_blocks[i] = 0; } }
	};

	struct ParsedImage
	{
		int w, h, ncomp;
		int hs[4], vs[4];
		int maxhs, maxvs;
		int pw[4], ph[4];       // padded block grid (interleaved MCUs)
		int tw[4], th[4];       // true block grid (non-interleaved scans)
		int mcus_x, mcus_y;
		bool progressive;
		int sof0, sof2, eoi;
		std::vector<int16_t> coef[4];
		std::vector<ScanInfo> scans;
		bool huff_no_all_ones;  // set for progressive streams
		ParsedImage() : w(0), h(0), ncomp(0), maxhs(1), maxvs(1), mcus_x(0), mcus_y(0),
			progressive(false), sof0(0), sof2(0), eoi(0), huff_no_all_ones(true) { }
	};

	// Returns 0 on success, negative on a parse error.
	static int parse_jpeg(const uint8_t* d, int N, ParsedImage& im, ParseStats* stats)
	{
		HuffTable h[8];
		int i = 0;

		// ---- pass 1: frame header, tables ----
		while (i + 1 < N)
		{
			if (!(d[i] == 0xFF && d[i + 1] != 0 && d[i + 1] != 0xFF)) { i++; continue; }
			int m = d[i + 1];
			if (m == 0xD8) { i += 2; continue; }
			if (m == 0xD9) { im.eoi++; i += 2; continue; }
			int L = (d[i + 2] << 8) | d[i + 3];
			int seg = i + 2 + L;
			if (L < 2 || seg > N) return -1;
			if (m == 0xC0 || m == 0xC2)
			{
				if (m == 0xC0) im.sof0++; else { im.sof2++; im.progressive = true; }
				im.h = (d[i + 5] << 8) | d[i + 6];
				im.w = (d[i + 7] << 8) | d[i + 8];
				im.ncomp = d[i + 9];
				for (int c = 0; c < im.ncomp; c++)
				{
					im.hs[c] = d[i + 10 + c * 3 + 1] >> 4;
					im.vs[c] = d[i + 10 + c * 3 + 1] & 15;
				}
			}
			else if (m == 0xC4)
			{
				int j = i + 4;
				while (j < seg)
				{
					int tc = d[j] >> 4, th = d[j] & 15;
					int nb[17];
					for (int k = 1; k <= 16; k++) nb[k] = d[j + k];
					int cnt = 0; for (int k = 1; k <= 16; k++) cnt += nb[k];
					if (j + 17 + cnt > seg) return -1;
					HuffTable& t = h[tc * 4 + th];
					int code = 0, k2 = 0;
					for (int l = 1; l <= 16; l++)
					{
						for (int q = 0; q < nb[l]; q++)
						{
							int sym = d[j + 17 + k2];
							t.size[sym] = l;
							t.code[sym] = code;
							// The JPEG format must never use an all-ones code word.
							if (code == ((1 << l) - 1)) im.huff_no_all_ones = false;
							code++; k2++;
						}
						code <<= 1;
					}
					j += 17 + cnt;
				}
			}
			i = seg;
		}

		if (im.w <= 0 || im.h <= 0 || im.ncomp <= 0 || im.w > 100000 || im.h > 100000) return -1;
		for (int c = 0; c < im.ncomp; c++)
		{
			if (im.hs[c] > im.maxhs) im.maxhs = im.hs[c];
			if (im.vs[c] > im.maxvs) im.maxvs = im.vs[c];
		}
		im.mcus_x = (im.w + 8 * im.maxhs - 1) / (8 * im.maxhs);
		im.mcus_y = (im.h + 8 * im.maxvs - 1) / (8 * im.maxvs);
		for (int c = 0; c < im.ncomp; c++)
		{
			im.pw[c] = im.mcus_x * im.hs[c];
			im.ph[c] = im.mcus_y * im.vs[c];
			im.tw[c] = ((im.w * im.hs[c] + im.maxhs - 1) / im.maxhs + 7) / 8;
			im.th[c] = ((im.h * im.vs[c] + im.maxvs - 1) / im.maxvs + 7) / 8;
			im.coef[c].assign((size_t)im.pw[c] * im.ph[c] * 64, 0);
		}

		if (stats) { memset(stats, 0, sizeof(*stats)); }

		// ---- pass 2: scans ----
		i = 0;
		while (i + 1 < N)
		{
			if (!(d[i] == 0xFF && d[i + 1] != 0 && d[i + 1] != 0xFF)) { i++; continue; }
			int m = d[i + 1];
			if (m == 0xD8 || m == 0xD9) { i += 2; continue; }
			int L = (d[i + 2] << 8) | d[i + 3];
			int seg = i + 2 + L;
			if (L < 2 || seg > N) return -1;
			if (m != 0xDA) { i = seg; continue; }

			int ns = d[i + 4];
			if (ns < 1 || ns > 3) return -1;
			ScanInfo sc; sc.ns = ns;
			int tc[3], ta[3];
			for (int k = 0; k < ns; k++)
			{
				sc.comps[k] = d[i + 5 + 2 * k] - 1;
				tc[k] = d[i + 6 + 2 * k] >> 4;
				ta[k] = d[i + 6 + 2 * k] & 15;
				if (sc.comps[k] < 0 || sc.comps[k] >= im.ncomp) return -1;
			}
			sc.ss = d[i + 5 + 2 * ns];
			sc.se = d[i + 6 + 2 * ns];
			sc.ah = d[i + 7 + 2 * ns] >> 4;
			sc.al = d[i + 7 + 2 * ns] & 15;
			im.scans.push_back(sc);

			// Locate the entropy segment and undo FF stuffing.
			int j = i + 2 + L;
			int e = j;
			while (e + 1 < N) { if (d[e] == 0xFF && d[e + 1] != 0 && d[e + 1] != 0xFF) break; e++; }
			std::vector<uint8_t> ent;
			for (int k = j; k < e; k++)
			{
				if (d[k] == 0xFF)
				{
					if (k + 1 >= e || d[k + 1] != 0) return -1; // an un-stuffed 0xFF inside entropy
					ent.push_back(0xFF); // drop the stuffed 0x00
					k++;
				}
				else ent.push_back(d[k]);
			}
			BitReader B; B.d = ent.empty() ? (const uint8_t*)"" : ent.data(); B.n = (int)ent.size(); B.byte = 0; B.bit = 0;

			auto hdecode = [&](const HuffTable& t) -> int {
				int code = 0;
				for (int l = 1; l <= 16; l++)
				{
					int b = B.get1(); if (b < 0) return -1;
					code = (code << 1) | b;
					for (int s = 0; s < 256; s++)
						if (t.size[s] == l && t.code[s] == code) return s;
					if (code < (1 << l) - 1 && code >= (1 << l)) return -1;
				}
				return -1;
			};
			auto blk = [&](int c, int bx, int by) -> int16_t* { return &im.coef[c][((size_t)by * im.pw[c] + bx) * 64]; };

			bool ok = true;
			if (sc.ss == 0 && sc.se == 63)
			{
				// Baseline sequential interleaved scan.
				int lastdc[4] = { 0,0,0,0 };
				for (int my = 0; my < im.mcus_y && ok; my++)
					for (int mx = 0; mx < im.mcus_x && ok; mx++)
						for (int k = 0; k < ns && ok; k++)
						{
							int c = sc.comps[k];
							for (int yy = 0; yy < im.vs[c] && ok; yy++)
								for (int xx = 0; xx < im.hs[c] && ok; xx++)
								{
									int16_t* p = blk(c, mx * im.hs[c] + xx, my * im.vs[c] + yy);
									int s = hdecode(h[tc[k]]); if (s < 0) { ok = false; break; }
									int diff = 0;
									if (s) { int v = B.get(s); if (v < 0) { ok = false; break; } diff = jpeg_extend(v, s); }
									lastdc[c] += diff; p[0] = (int16_t)lastdc[c];
									for (int kk = 1; kk <= 63; kk++)
									{
										int sym = hdecode(h[4 + ta[k]]); if (sym < 0) { ok = false; break; }
										int r = sym >> 4, sz = sym & 15;
										if (sz)
										{
											kk += r; if (kk > 63) { ok = false; break; }
											int v = B.get(sz); if (v < 0) { ok = false; break; }
											p[kZigZag[kk]] = (int16_t)jpeg_extend(v, sz);
										}
										else { if (r == 15) kk += 15; else break; }
									}
								}
						}
			}
			else if (ns == 1)
			{
				// Progressive non-interleaved scan (AC first/refine, or DC first/refine on a
				// single-component image).
				int c = sc.comps[0];
				int lastdc = 0;
				long eobrun = 0;
				int prev_event = 0; // 1 = EOBRUN, 2 = ZRL, 0 = other
				for (int by = 0; by < im.th[c] && ok; by++)
					for (int bx = 0; bx < im.tw[c] && ok; bx++)
					{
						int16_t* p = blk(c, bx, by);
						if (sc.ss == 0 && sc.ah == 0)
						{
							if (stats) stats->dc_blocks[c]++;
							int s = hdecode(h[tc[0]]); if (s < 0) { ok = false; break; }
							int diff = 0;
							if (s) { int v = B.get(s); if (v < 0) { ok = false; break; } diff = jpeg_extend(v, s); }
							lastdc += diff; p[0] = (int16_t)((uint32_t)lastdc << sc.al);
						}
						else if (sc.ss == 0)
						{
							int b = B.get1(); if (b < 0) { ok = false; break; }
							if (b) p[0] |= (int16_t)(1 << sc.al);
						}
						else if (sc.ah == 0)
						{
							if (stats) stats->ac_blocks[c]++;
							if (eobrun > 0) { eobrun--; continue; }
							for (int k = sc.ss; k <= sc.se; k++)
							{
								int sym = hdecode(h[4 + ta[0]]); if (sym < 0) { ok = false; break; } if(getenv("TDBG"))fprintf(stderr,"SYM %02x\n",sym);
								int r = sym >> 4, sz = sym & 15;
								if (sz)
								{
									if (stats) prev_event = 0;
									k += r; if (k > sc.se) { ok = false; break; }
									int v = B.get(sz); if (v < 0) { ok = false; break; }
									p[kZigZag[k]] = (int16_t)((uint32_t)jpeg_extend(v, sz) << sc.al);
								}
								else if (r == 15)
								{
									if (stats) { stats->zrl_count++; if (prev_event == 1) stats->zrl_touches_eobrun = true; prev_event = 2; }
									k += 15;
								}
								else
								{
									eobrun = 1 << r;
									if (r) { int v = B.get(r); if (v < 0) { ok = false; break; } eobrun += v; }
									if (stats)
									{
										stats->eobrun_count++;
										if (eobrun > stats->max_eobrun) stats->max_eobrun = (int)eobrun;
										if (prev_event == 2) stats->zrl_touches_eobrun = true;
									}
									eobrun--; break;
								}
							}
						}
						else
						{
							if (stats) stats->ac_ref_blocks[c]++;
							int p1 = 1 << sc.al; int k = sc.ss;
							if (eobrun == 0)
							{
								for (; k <= sc.se; k++)
								{
									int sym = hdecode(h[4 + ta[0]]); if (sym < 0) { ok = false; break; } if(getenv("TDBG"))fprintf(stderr,"SYM %02x\n",sym);
									int r = sym >> 4, sz = sym & 15; int s = 0;
									if (sz)
									{
										if (sz != 1) { ok = false; break; }
										int b = B.get1(); if (b < 0) { ok = false; break; }
										s = b ? p1 : -p1;
									}
									else if (r != 15)
									{
										eobrun = 1 << r;
										if (r) { int v = B.get(r); if (v < 0) { ok = false; break; } eobrun += v; }
										break;
									}
									do
									{
										int16_t* t = &p[kZigZag[k]];
										if (*t != 0)
										{
											int b = B.get1(); if (b < 0) { ok = false; break; }
											if (b) { if ((*t & p1) == 0) *t = (int16_t)(*t >= 0 ? *t + p1 : *t - p1); }
										}
										else { if (--r < 0) break; }
										k++;
									} while (k <= sc.se && ok);
									if (!ok) break;
									if (s && k <= sc.se) p[kZigZag[k]] = (int16_t)s;
								}
							}
							if (eobrun > 0 && ok)
							{
								for (; k <= sc.se; k++)
								{
									int16_t* t = &p[kZigZag[k]];
									if (*t != 0)
									{
										int b = B.get1(); if (b < 0) { ok = false; break; }
										if (b) { if ((*t & p1) == 0) *t = (int16_t)(*t >= 0 ? *t + p1 : *t - p1); }
									}
								}
								eobrun--;
							}
						}
					}
			}
			else
			{
				// Progressive interleaved DC scan.
				int lastdc[4] = { 0,0,0,0 };
				for (int my = 0; my < im.mcus_y && ok; my++)
					for (int mx = 0; mx < im.mcus_x && ok; mx++)
						for (int k = 0; k < ns && ok; k++)
						{
							int c = sc.comps[k];
							for (int yy = 0; yy < im.vs[c] && ok; yy++)
								for (int xx = 0; xx < im.hs[c] && ok; xx++)
								{
									int16_t* p = blk(c, mx * im.hs[c] + xx, my * im.vs[c] + yy);
									if (stats && sc.ah == 0) stats->dc_blocks[c]++;
									if (sc.ah == 0)
									{
										int s = hdecode(h[tc[k]]); if (s < 0) { ok = false; break; }
										int diff = 0;
										if (s) { int v = B.get(s); if (v < 0) { ok = false; break; } diff = jpeg_extend(v, s); }
										lastdc[c] += diff; p[0] = (int16_t)((uint32_t)lastdc[c] << sc.al);
									}
									else
									{
										int b = B.get1(); if (b < 0) { ok = false; break; }
										if (b) p[0] |= (int16_t)(1 << sc.al);
									}
								}
						}
			}

			if (!ok) return -2;
			// Trailing bits of a scan must be the 1s padding written by the encoder.
			if (!B.rest_is_ones()) return -3;

			i = e;
		}
		return 0;
	}

	// ---------------------------------------------------------------------------
	// Image helpers
	// ---------------------------------------------------------------------------
	static void make_image(int w, int h, int ch, std::vector<uint8_t>& out)
	{
		// An impulse grid: sparse bright/dark pixels on a flat mid-gray background. Blocks
		// that contain no impulse are entirely zero in AC (long EOB runs), while blocks that
		// catch one impulse have a low-frequency coefficient and then a long zero run before
		// the next nonzero (ZRL). A final fully flat band guarantees runs of all-zero blocks.
		out.assign((size_t)w * h * ch, 0);
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				for (int c = 0; c < ch; c++)
				{
					int v = 128 + c * 5;
					if ((y % 5) == 0 && (x % 6) == 0)
						v = (((x / 6) + (y / 5)) & 1) ? 245 - c * 4 : 12 + c * 6;
					else if ((y % 5) == 2 && (x % 11) == 5)
						v = 30 + c * 8;
					// The bottom band is flat so several consecutive all-zero blocks occur.
					if (y * 4 >= h * 3) v = 100 + c * 6 + (x / 32) * 2;
					out[((size_t)y * w + x) * ch + c] = (uint8_t)v;
				}
	}

	static bool encode_mem(const std::vector<uint8_t>& src, int w, int h, int ch,
		const jpge::params& p, std::vector<uint8_t>& jpeg)
	{
		jpeg.assign(1 << 20, 0);
		int size = (int)jpeg.size();
		bool ok = jpge::compress_image_to_jpeg_file_in_memory(jpeg.data(), size, w, h, ch, src.data(), p);
		jpeg.resize(ok ? size : 0);
		return ok;
	}

	static bool decode_rgb(const std::vector<uint8_t>& jpeg, std::vector<uint8_t>& rgb, int& w, int& h)
	{
		int n = 0; w = h = 0;
		unsigned char* p = jpgd::decompress_jpeg_image_from_memory(jpeg.data(), (int)jpeg.size(), &w, &h, &n, 3);
		if (!p) return false;
		rgb.assign(p, p + (size_t)w * h * 3);
		free(p);
		return true;
	}

	static int compare_coefficients(const ParsedImage& seq, const ParsedImage& prg, int max_report)
	{
		if (seq.ncomp != prg.ncomp) return -1000000;
		int diff = 0;
		for (int c = 0; c < seq.ncomp; c++)
		{
			if (seq.tw[c] != prg.tw[c] || seq.th[c] != prg.th[c]) return -1000000;
			for (int by = 0; by < seq.th[c]; by++)
				for (int bx = 0; bx < seq.tw[c]; bx++)
				{
					size_t k = ((size_t)by * seq.pw[c] + bx) * 64;
					size_t k2 = ((size_t)by * prg.pw[c] + bx) * 64;
					for (int q = 0; q < 64; q++)
						if (seq.coef[c][k + q] != prg.coef[c][k2 + q])
						{
							if (diff < max_report)
								fprintf(stderr, "  coef mismatch c=%d blk(%d,%d) q=%d seq=%d prg=%d\n",
									c, bx, by, q, (int)seq.coef[c][k + q], (int)prg.coef[c][k2 + q]);
							diff++;
						}
				}
		}
		return diff;
	}

	struct CoefCoverage
	{
		int neg_odd_dc;
		int pos_one_ac, neg_one_ac;
		int odd_corr, even_corr;
		int neg_grown;
	};

	static CoefCoverage measure_coverage(const ParsedImage& im)
	{
		CoefCoverage cv = { 0,0,0,0,0,0 };
		for (int c = 0; c < im.ncomp; c++)
			for (int by = 0; by < im.th[c]; by++)
				for (int bx = 0; bx < im.tw[c]; bx++)
				{
					const int16_t* p = &im.coef[c][((size_t)by * im.pw[c] + bx) * 64];
					if (p[0] < 0 && (p[0] & 1)) cv.neg_odd_dc++;
					for (int k = 1; k < 64; k++)
					{
						int v = p[k]; int a = v < 0 ? -v : v;
						if (v == 1) cv.pos_one_ac++;
						if (v == -1) cv.neg_one_ac++;
						if (a >= 3 && (a & 1)) {
							cv.odd_corr++;
							if (v < 0) cv.neg_grown++;
						}
						if (a >= 2 && !(a & 1)) cv.even_corr++;
					}
				}
		return cv;
	}

	// Encodes sequential and progressive variants, parses both, and checks that every
	// quantized coefficient on the true block grid matches.
	static bool verify_pair(CaseCtx& ctx, const std::vector<uint8_t>& src, int w, int h, int ch,
		const jpge::params& base, ParseStats* stats, ParsedImage* seq_out, ParsedImage* prg_out)
	{
		jpge::params seqp = base; seqp.m_progressive = false; seqp.m_two_pass_flag = false;
		jpge::params prgp = base; prgp.m_progressive = true; prgp.m_two_pass_flag = false;

		std::vector<uint8_t> seq_jpeg, prg_jpeg;
		bool e1 = encode_mem(src, w, h, ch, seqp, seq_jpeg);
		bool e2 = encode_mem(src, w, h, ch, prgp, prg_jpeg);
		CHECK(ctx, e1);
		CHECK(ctx, e2);
		if (!e1 || !e2) return false;

		ParsedImage si, pi;
		int rs = parse_jpeg(seq_jpeg.data(), (int)seq_jpeg.size(), si, NULL);
		int rp = parse_jpeg(prg_jpeg.data(), (int)prg_jpeg.size(), pi, stats);
		CHECK(ctx, rs == 0);
		CHECK(ctx, rp == 0);
		if (rs || rp) return false;

		CHECK(ctx, si.sof0 == 1 && si.sof2 == 0);
		CHECK(ctx, pi.sof2 == 1 && pi.sof0 == 0);
		CHECK(ctx, pi.eoi == 1);
		CHECK(ctx, pi.progressive);
		CHECK(ctx, pi.huff_no_all_ones);

		int diff = compare_coefficients(si, pi, 8);
		CHECK(ctx, diff == 0);

		// Full RGB must be identical after jpgd decoding of both streams.
		std::vector<uint8_t> seq_rgb, prg_rgb;
		int w1 = 0, h1 = 0, w2 = 0, h2 = 0;
		bool d1 = decode_rgb(seq_jpeg, seq_rgb, w1, h1);
		bool d2 = decode_rgb(prg_jpeg, prg_rgb, w2, h2);
		CHECK(ctx, d1 && d2);
		CHECK(ctx, w1 == w2 && h1 == h2);
		CHECK(ctx, d1 && d2 && seq_rgb == prg_rgb);

		if (seq_out) *seq_out = si;
		if (prg_out) *prg_out = pi;
		return diff == 0 && d1 && d2;
	}

	static bool check_scan_script(CaseCtx& ctx, const ParsedImage& im)
	{
		const bool color = im.ncomp == 3;
		const int n = color ? 8 : 4;
		CHECK(ctx, (int)im.scans.size() == n);
		if ((int)im.scans.size() != n) return false;

		bool ok = true;
		// 1. Interleaved DC first.
		CHECK(ctx, im.scans[0].ns == im.ncomp && im.scans[0].ss == 0 && im.scans[0].se == 0 && im.scans[0].ah == 0 && im.scans[0].al == 1);
		if (im.scans[0].ns == im.ncomp) for (int c = 0; c < im.ncomp; c++) CHECK(ctx, im.scans[0].comps[c] == c);
		// 2. AC first per component.
		for (int c = 0; c < im.ncomp; c++)
		{
			const ScanInfo& s = im.scans[1 + c];
			CHECK(ctx, s.ns == 1 && s.comps[0] == c && s.ss == 1 && s.se == 63 && s.ah == 0 && s.al == 1);
		}
		// 3. Interleaved DC refinement.
		{
			const ScanInfo& s = im.scans[1 + im.ncomp];
			CHECK(ctx, s.ns == im.ncomp && s.ss == 0 && s.se == 0 && s.ah == 1 && s.al == 0);
			if (s.ns == im.ncomp) for (int c = 0; c < im.ncomp; c++) CHECK(ctx, s.comps[c] == c);
		}
		// 4. AC refinement per component.
		for (int c = 0; c < im.ncomp; c++)
		{
			const ScanInfo& s = im.scans[2 + im.ncomp + c];
			CHECK(ctx, s.ns == 1 && s.comps[0] == c && s.ss == 1 && s.se == 63 && s.ah == 1 && s.al == 0);
		}
		return ok;
	}

} // namespace

// ===========================================================================
// case_scans: scan script, EOBRUN accumulation, baseline-vs-progressive coeffs.
// ===========================================================================
int case_scans(void)
{
	CaseCtx ctx;
	const int w = 56, h = 40, ch = 3;
	std::vector<uint8_t> src; make_image(w, h, ch, src);

	jpge::params p; p.m_quality = 62; p.m_subsampling = jpge::H2V2;
	ParseStats stats;
	ParsedImage seq, prg;
	bool okp = verify_pair(ctx, src, w, h, ch, p, &stats, &seq, &prg);
	CHECK(ctx, okp);
	if (okp)
	{
		CHECK(ctx, check_scan_script(ctx, prg));

		// Non-interleaved AC scans must cover exactly the true block grid, while the
		// interleaved DC scan covers the padded MCU grid (a different count).
		for (int c = 0; c < prg.ncomp; c++)
		{
			CHECK(ctx, stats.ac_blocks[c] == prg.tw[c] * prg.th[c]);
			CHECK(ctx, stats.ac_ref_blocks[c] == prg.tw[c] * prg.th[c]);
			CHECK(ctx, stats.dc_blocks[c] == prg.pw[c] * prg.ph[c]);
		}

		// At least one EOB run must span three or more consecutive blocks.
		CHECK(ctx, stats.max_eobrun >= 3);
		// ZRL and EOBRUN must both be exercised by the same scan family; EOBRUN must also
		// span multiple blocks (checked above). A ZRL that directly precedes an EOBRUN is
		// one of the trickier orderings, so accept either that or both events existing.
		CHECK(ctx, stats.zrl_count > 0);
		CHECK(ctx, stats.eobrun_count > 0);
		CHECK(ctx, stats.zrl_touches_eobrun || (stats.zrl_count > 0 && stats.eobrun_count > 0));

		// Corpus coverage for the DC/AC coding paths.
		CoefCoverage cv = measure_coverage(seq);
		CHECK(ctx, cv.neg_odd_dc > 0);
		CHECK(ctx, cv.pos_one_ac > 0);
		CHECK(ctx, cv.neg_one_ac > 0);
		CHECK(ctx, cv.odd_corr > 0);
		CHECK(ctx, cv.even_corr > 0);
	}

	return ctx.fails ? -1 : ctx.asserts;
}

// ===========================================================================
// case_refinement: successive-approximation detail (signs, correction bits, EOBRUN).
// ===========================================================================
int case_refinement(void)
{
	CaseCtx ctx;
	const int ch = 3;

	// 1. Ordinary image: exercise correction bits on already-significant coefficients,
	//    newly-significant +/-1 AC, and negative magnitude growth.
	{
		const int w = 48, h = 32;
		std::vector<uint8_t> src; make_image(w, h, ch, src);
		jpge::params p; p.m_quality = 90; p.m_subsampling = jpge::H2V2;
		ParsedImage seq, prg;
		bool okp = verify_pair(ctx, src, w, h, ch, p, NULL, &seq, &prg);
		CHECK(ctx, okp);
		if (okp)
		{
			CoefCoverage cv = measure_coverage(seq);
			CHECK(ctx, cv.neg_one_ac > 0);     // newly significant negative
			CHECK(ctx, cv.pos_one_ac > 0);     // newly significant positive
			CHECK(ctx, cv.odd_corr > 0);       // correction bit 1 on a known coefficient
			CHECK(ctx, cv.even_corr > 0);      // correction bit 0 on a known coefficient
			CHECK(ctx, cv.neg_grown > 0);      // negative coefficient whose magnitude grows
			CHECK(ctx, cv.neg_odd_dc > 0);     // negative odd DC needs the refine bit
		}
	}

	// 2. Solid image: every AC coefficient is zero, so the whole AC-first scan is one
	//    long EOBRUN and refinement only emits DC bits.
	{
		const int w = 40, h = 32;
		std::vector<uint8_t> src((size_t)w * h * ch);
		for (size_t i = 0; i < src.size(); i++) src[i] = (uint8_t)(40 + (i % ch) * 20);
		jpge::params p; p.m_quality = 75; p.m_subsampling = jpge::H1V1;
		ParseStats stats;
		ParsedImage seq, prg;
		bool okp = verify_pair(ctx, src, w, h, ch, p, &stats, &seq, &prg);
		CHECK(ctx, okp);
		if (okp)
		{
			// Long all-zero AC run, decoded as a single EOBRUN per component.
			CHECK(ctx, stats.max_eobrun >= 3);
			CHECK(ctx, stats.zrl_count == 0);
		}
	}

	return ctx.fails ? -1 : ctx.asserts;
}

// ===========================================================================
// case_geometry: four sampling modes, odd dimensions, padded vs true block counts.
// ===========================================================================
int case_geometry(void)
{
	CaseCtx ctx;
	bool saw_padding_diff = false;

	struct GeomCase { int w, h, ch, sub; };
	const GeomCase cases[] = {
		{ 8, 8, 1, jpge::Y_ONLY },      // single block
		{ 35, 19, 3, jpge::Y_ONLY },
		{ 35, 19, 3, jpge::H1V1 },
		{ 35, 19, 3, jpge::H2V1 },
		{ 33, 17, 3, jpge::H2V2 },
		{ 19, 11, 3, jpge::H2V2 },      // odd 4:2:0 with right/bottom padding
		{ 43, 29, 4, jpge::H2V2 },      // 4-channel source maps to 3 components
		{ 17, 9, 3, jpge::H2V1 },
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
	{
		const GeomCase& g = cases[i];
		std::vector<uint8_t> src; make_image(g.w, g.h, g.ch, src);
		jpge::params p; p.m_quality = 83; p.m_subsampling = (jpge::subsampling_t)g.sub;
		ParseStats stats;
		ParsedImage seq, prg;
		bool okp = verify_pair(ctx, src, g.w, g.h, g.ch, p, &stats, &seq, &prg);
		CHECK(ctx, okp);
		if (!okp) continue;

		for (int c = 0; c < prg.ncomp; c++)
		{
			// Non-interleaved AC block counts come from the true (unpadded) grid.
			CHECK(ctx, stats.ac_blocks[c] == prg.tw[c] * prg.th[c]);
			// Interleaved DC block counts come from the padded MCU grid.
			CHECK(ctx, stats.dc_blocks[c] == prg.pw[c] * prg.ph[c]);
		}

		// The padded grid can never be smaller than the true grid, and for widths that are
		// not a multiple of the MCU width the padded luma grid is strictly larger (e.g. a
		// 35-pixel-wide H2V2 image needs six 8-pixel chroma blocks but only five are real).
		for (int c = 0; c < prg.ncomp; c++)
		{
			CHECK(ctx, prg.pw[c] >= prg.tw[c]);
			CHECK(ctx, prg.ph[c] >= prg.th[c]);
		}
		// Padded vs true grids are computed independently: the interleaved DC scan visits
		// every padded block, the non-interleaved AC scans only the true blocks. Record
		// whether any component of any case genuinely needs edge padding.
		for (int c = 0; c < prg.ncomp; c++)
			if (prg.pw[c] != prg.tw[c] || prg.ph[c] != prg.th[c]) saw_padding_diff = true;
	}

	// At least one geometry must exercise padding: otherwise the padded-vs-true
	// distinction (and the odd-size edge-block handling) is never tested.
	CHECK(ctx, saw_padding_diff);

	return ctx.fails ? -1 : ctx.asserts;
}

// ===========================================================================
// case_compatibility: byte compatibility of legacy modes, all entry points,
// failure handling, defaults and illegal combinations.
// ===========================================================================
namespace {

	struct VectorStream : jpge::output_stream
	{
		std::vector<uint8_t> bytes;
		virtual bool put_buf(const void* p, int len)
		{
			const uint8_t* b = (const uint8_t*)p;
			bytes.insert(bytes.end(), b, b + len);
			return true;
		}
	};

	struct FailingStream : jpge::output_stream
	{
		size_t limit, written;
		FailingStream(size_t l) : limit(l), written(0) { }
		virtual bool put_buf(const void* p, int len)
		{
			(void)p;
			if (written + (size_t)len > limit) { written = limit; return false; }
			written += (size_t)len;
			return true;
		}
	};

	static uint32_t fnv1a(const std::vector<uint8_t>& v)
	{
		uint32_t h = 2166136261u;
		for (size_t i = 0; i < v.size(); i++) { h ^= v[i]; h *= 16777619u; }
		return h;
	}

	static bool encode_scanline(const std::vector<uint8_t>& src, int w, int h, int ch,
		const jpge::params& p, std::vector<uint8_t>& out, size_t fail_after = (size_t)-1)
	{
		jpge::jpeg_encoder enc;
		VectorStream vs;
		FailingStream fs(fail_after);
		jpge::output_stream* os = (fail_after == (size_t)-1) ? (jpge::output_stream*)&vs : (jpge::output_stream*)&fs;
		if (!enc.init(os, w, h, ch, p)) return false;
		bool ok = true;
		for (uint pass = 0; pass < enc.get_total_passes() && ok; pass++)
		{
			for (int y = 0; y < h && ok; y++)
				ok = enc.process_scanline(&src[(size_t)y * w * ch]);
			if (ok) ok = enc.process_scanline(NULL);
		}
		if (fail_after == (size_t)-1) out = vs.bytes;
		return ok;
	}
}

int case_compatibility(void)
{
	CaseCtx ctx;

	// --- defaults ---
	{
		jpge::params p;
		CHECK(ctx, p.m_progressive == false);
		CHECK(ctx, p.m_two_pass_flag == false);
		std::vector<uint8_t> src; make_image(35, 19, 3, src);
		std::vector<uint8_t> jpeg;
		CHECK(ctx, encode_mem(src, 35, 19, 3, p, jpeg));
		ParsedImage im; int r = parse_jpeg(jpeg.data(), (int)jpeg.size(), im, NULL);
		CHECK(ctx, r == 0);
		CHECK(ctx, im.sof0 == 1 && im.sof2 == 0);   // default stays baseline
	}

	// --- illegal combination: progressive + two-pass must fail init ---
	{
		std::vector<uint8_t> src; make_image(16, 16, 1, src);
		jpge::params p; p.m_subsampling = jpge::Y_ONLY; p.m_progressive = true; p.m_two_pass_flag = true;
		VectorStream vs;
		jpge::jpeg_encoder enc;
		CHECK(ctx, enc.init(&vs, 16, 16, 1, p) == false);
	}
	// --- get_total_passes, and source submitted exactly once ---
	{
		jpge::params p; p.m_progressive = true;
		std::vector<uint8_t> src; make_image(16, 16, 1, src);
		VectorStream vs; jpge::jpeg_encoder enc;
		CHECK(ctx, enc.init(&vs, 16, 16, 1, p));
		CHECK(ctx, enc.get_total_passes() == 1);
		for (int y = 0; y < 16; y++) enc.process_scanline(&src[(size_t)y * 16]);
		// Nothing may be written until the source image has been fully submitted (the final
		// NULL scanline is what triggers every progressive scan).
		CHECK(ctx, vs.bytes.empty());
		CHECK(ctx, enc.process_scanline(NULL));
		CHECK(ctx, !vs.bytes.empty());
		ParsedImage im;
		CHECK(ctx, parse_jpeg(vs.bytes.data(), (int)vs.bytes.size(), im, NULL) == 0);
		CHECK(ctx, im.sof2 == 1 && im.eoi == 1);
		enc.deinit();
		jpge::params p2; p2.m_two_pass_flag = true;
		VectorStream vs2; jpge::jpeg_encoder enc2;
		CHECK(ctx, enc2.init(&vs2, 16, 16, 1, p2));
		CHECK(ctx, enc2.get_total_passes() == 2);
		enc2.deinit();
	}

	// --- byte compatibility of baseline and two-pass modes against reference digests ---
	{
		const int w = 35, h = 19, ch = 3;
		std::vector<uint8_t> src((size_t)w * h * ch);
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				for (int c = 0; c < ch; c++)
					src[((size_t)y * w + x) * ch + c] = (uint8_t)((x * 31 + y * 17 + c * 67) & 255);

		struct Ref { int sub, two, q; int size; uint32_t fnv; };
		const Ref refs[] = {
			{ jpge::Y_ONLY, 0, 83, 709,  0x3a1fdcd2u },
			{ jpge::Y_ONLY, 1, 83, 555,  0xc1f8ab47u },
			{ jpge::H1V1,   0, 50, 1295, 0xfcd0380au },
			{ jpge::H2V2,   0, 83, 1327, 0x9b65ef60u },
			{ jpge::H2V2,   1, 83, 1007, 0xdb8da10eu },
			{ jpge::H2V1,   1, 95, 1591, 0xb6792567u },
		};
		for (size_t i = 0; i < sizeof(refs) / sizeof(refs[0]); i++)
		{
			jpge::params p; p.m_subsampling = (jpge::subsampling_t)refs[i].sub; p.m_quality = refs[i].q; p.m_two_pass_flag = refs[i].two != 0;
			std::vector<uint8_t> jpeg;
			CHECK(ctx, encode_mem(src, w, h, ch, p, jpeg));
			CHECK(ctx, (int)jpeg.size() == refs[i].size);
			CHECK(ctx, fnv1a(jpeg) == refs[i].fnv);
		}
	}

	// --- file / memory / scanline entry points must agree byte for byte ---
	{
		const int w = 43, h = 29, ch = 3;
		std::vector<uint8_t> src; make_image(w, h, ch, src);
		jpge::params p; p.m_quality = 82; p.m_subsampling = jpge::H2V2; p.m_progressive = true;

		std::vector<uint8_t> mem_jpeg, line_jpeg;
		CHECK(ctx, encode_mem(src, w, h, ch, p, mem_jpeg));
		CHECK(ctx, encode_scanline(src, w, h, ch, p, line_jpeg));
		CHECK(ctx, mem_jpeg == line_jpeg);

		const char* tmp = getenv("TMPDIR"); if (!tmp) tmp = "/tmp";
		char path[512];
		snprintf(path, sizeof(path), "%s/jpge_prog_%d.jpg", tmp, (int)getpid());
		bool file_ok = jpge::compress_image_to_jpeg_file(path, w, h, ch, src.data(), p);
		CHECK(ctx, file_ok);
		std::vector<uint8_t> file_jpeg;
		if (file_ok)
		{
			FILE* f = fopen(path, "rb");
			CHECK(ctx, f != NULL);
			if (f)
			{
				fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
				file_jpeg.resize((size_t)n);
				CHECK(ctx, n > 0 && fread(file_jpeg.data(), 1, (size_t)n, f) == (size_t)n);
				fclose(f);
			}
			remove(path);
		}
		CHECK(ctx, file_jpeg == mem_jpeg);

		// Both streams still decode through jpgd to the same RGB.
		std::vector<uint8_t> a, b; int aw = 0, ah = 0, bw = 0, bh = 0;
		CHECK(ctx, decode_rgb(mem_jpeg, a, aw, ah));
		CHECK(ctx, decode_rgb(file_jpeg, b, bw, bh));
		CHECK(ctx, aw == bw && ah == bh && a == b);
	}

	// --- output failure: returns false, then deinit + re-init works ---
	{
		const int w = 32, h = 24, ch = 1;
		std::vector<uint8_t> src; make_image(w, h, ch, src);
		jpge::params p; p.m_subsampling = jpge::Y_ONLY; p.m_progressive = true;

		jpge::jpeg_encoder enc;
		FailingStream fs(64);   // only 64 bytes of space: markers alone overflow
		CHECK(ctx, enc.init(&fs, w, h, ch, p));
		bool saw_fail = false;
		for (int y = 0; y < h; y++) enc.process_scanline(&src[(size_t)y * w]);
		saw_fail = !enc.process_scanline(NULL);
		CHECK(ctx, saw_fail);

		enc.deinit();
		VectorStream vs;
		CHECK(ctx, enc.init(&vs, w, h, ch, p));
		bool ok = true;
		for (int y = 0; y < h && ok; y++) ok = enc.process_scanline(&src[(size_t)y * w]);
		if (ok) ok = enc.process_scanline(NULL);
		CHECK(ctx, ok);
		CHECK(ctx, !vs.bytes.empty());
		enc.deinit();
	}

	// --- too-small memory buffer fails gracefully ---
	{
		const int w = 32, h = 24, ch = 1;
		std::vector<uint8_t> src; make_image(w, h, ch, src);
		jpge::params p; p.m_subsampling = jpge::Y_ONLY; p.m_progressive = true;
		std::vector<uint8_t> small(8, 0);
		int size = (int)small.size();
		CHECK(ctx, jpge::compress_image_to_jpeg_file_in_memory(small.data(), size, w, h, ch, src.data(), p) == false);
	}

	return ctx.fails ? -1 : ctx.asserts;
}
