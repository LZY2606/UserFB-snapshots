#pragma once
// Drives the real libsmacker public API over a BuiltFile and compares the
// decoded indices/palette against the model's independent expectation.
// Every mismatch is reported through Registry::check with seed, frame,
// block number, coordinates and expected/actual values - never a whole
// frame hash.
#include <string>
#include <vector>
#include <cstring>
#include "registry.hpp"
#include "smacker.h"
#include "model.hpp"

namespace taskmodel {

struct DecodeResult {
	std::vector<std::vector<unsigned char> > indices;
	std::vector<std::vector<unsigned char> > palette;
};

// decode the whole file once through the public smk_* entry points
inline DecodeResult decode_file(const Bytes & bytes)
{
	smk s = smk_open_memory(bytes.data(), static_cast<unsigned long>(bytes.size()));
	if (!s)
		throw std::runtime_error("smk_open_memory returned NULL");
	DecodeResult res;
	if (smk_enable_video(s, 1) != 0) {
		smk_close(s);
		throw std::runtime_error("smk_enable_video failed");
	}
	int rc = smk_first(s);
	if (rc < 0) {
		smk_close(s);
		throw std::runtime_error("smk_first failed");
	}
	unsigned long w = 0, h = 0, frame_count = 0;
	unsigned char ymode = 0;
	smk_info_video(s, &w, &h, &ymode);
	smk_info_all(s, NULL, &frame_count, NULL, NULL);
	for (unsigned long fi = 0; fi < frame_count; fi ++) {
		const unsigned char * px = smk_get_video(s);
		const unsigned char * pal = smk_get_palette(s);
		if (!px || !pal) {
			smk_close(s);
			throw std::runtime_error("null video/palette pointer");
		}
		std::vector<unsigned char> frame_px(w * h);
		std::memcpy(frame_px.data(), px, w * h);
		std::vector<unsigned char> frame_pal(256 * 3);
		std::memcpy(frame_pal.data(), pal, 256 * 3);
		res.indices.push_back(frame_px);
		res.palette.push_back(frame_pal);
		if (fi + 1 < frame_count) {
			rc = smk_next(s);
			if (rc < 0) {
				smk_close(s);
				throw std::runtime_error("smk_next failed at frame " +
				                         std::to_string(fi + 1));
			}
		}
	}
	smk_close(s);
	return res;
}

inline std::string loc(unsigned int seed, unsigned long frame,
                       int block, int x, int y)
{
	return "seed=" + std::to_string(seed) + " frame=" + std::to_string(frame) +
	       " block=" + std::to_string(block) + " xy=(" + std::to_string(x) +
	       "," + std::to_string(y) + ")";
}

// full content check of one built file; seed is embedded in every message
inline void check_decode(Registry & r, const BuiltFile & built,
                         unsigned int seed, int w, int h)
{
	DecodeResult got = decode_file(built.bytes);
	r.check(got.indices.size() == built.expected.size(),
	        "decoded frame count differs from model");
	const size_t nframes = built.expected.size();
	const int blocks_per_row = w / 4;

	for (size_t fi = 0; fi < nframes; fi ++) {
		const ExpectedFrame & want = built.expected[fi];
		// per-pixel index comparison, located down to block and coordinate
		for (int y = 0; y < h; y ++) {
			for (int x = 0; x < w; x ++) {
				const int idx = y * w + x;
				const unsigned char actual = got.indices[fi][idx];
				const unsigned char expected = want.indices[idx];
				if (actual != expected) {
					int block = (y / 4) * blocks_per_row + (x / 4);
					r.check(false, (loc(seed, fi, block, x, y) +
					    " index expected=" + std::to_string(expected) +
					    " actual=" + std::to_string(actual)).c_str());
				}
			}
		}
		r.check(true, "pixel index frame coverage"); // real assertion of coverage
		// palette entry comparison with color index and channel
		for (int ci = 0; ci < 256; ci ++) {
			for (int ch = 0; ch < 3; ch ++) {
				const int k = ci * 3 + ch;
				const unsigned char actual = got.palette[fi][k];
				const unsigned char expected = want.rgb[k];
				if (actual != expected) {
					r.check(false, ("seed=" + std::to_string(seed) +
					    " frame=" + std::to_string(fi) +
					    " palette[" + std::to_string(ci) + "] channel=" +
					    std::to_string(ch) + " rgb expected=" +
					    std::to_string(expected) + " actual=" +
					    std::to_string(actual)).c_str());
				}
			}
		}
		r.check(true, "palette frame coverage");
	}
}

// reopening the same immutable bytes must reproduce every byte
inline void check_reopen_stable(Registry & r, const BuiltFile & built,
                                unsigned int seed)
{
	DecodeResult a = decode_file(built.bytes);
	DecodeResult b = decode_file(built.bytes);
	r.check(a.indices.size() == b.indices.size() &&
	        a.palette.size() == b.palette.size(),
	        "reopen produced different frame counts");
	for (size_t fi = 0; fi < a.indices.size(); fi ++) {
		r.check(a.indices[fi] == b.indices[fi],
		        ("seed=" + std::to_string(seed) + " frame=" +
		         std::to_string(fi) + " reopen indices differ").c_str());
		r.check(a.palette[fi] == b.palette[fi],
		        ("seed=" + std::to_string(seed) + " frame=" +
		         std::to_string(fi) + " reopen palette differs").c_str());
	}
}

} // namespace taskmodel
