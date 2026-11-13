// Example for run_constrained(): synthesize a level-surface-like texture from a
// procedurally generated sample while pinning two pixels and bounding the
// amount of "ink" inside two overlapping regions. Writes the original-size
// result as a PPM and re-checks the color counts. Usage:
//   constrained <output.ppm>

#define main wfc_upstream_main
#include "main.cpp"
#undef main

#include <cassert>
#include <cstdio>
#include <iostream>

namespace
{

const RGBA kInk{36, 36, 48, 255};
const RGBA kPaper{228, 228, 220, 255};
const RGBA kAccent{200, 70, 60, 255};

const size_t kSampleSize = 8;
const size_t kOutSize    = 16;

PalettedImage make_sample()
{
	PalettedImage sample{kSampleSize, kSampleSize, {}, {kInk, kPaper, kAccent}};
	for (size_t y = 0; y < kSampleSize; ++y) {
		for (size_t x = 0; x < kSampleSize; ++x) {
			size_t v = ((x / 2) + (y / 2)) % 2; // 2x2 block checkerboard
			if ((x == y && x > 0) || (x + y == kSampleSize - 1 && x > 0 && y > 0)) {
				v = 2; // red diagonals
			}
			sample.data.push_back(static_cast<ColorIndex>(v));
		}
	}
	return sample;
}

} // namespace

int main(int argc, char* argv[])
{
	if (argc != 2) {
		std::fprintf(stderr, "usage: %s <output.ppm>\n", argv[0]);
		return 1;
	}

	const auto sample = make_sample();
	const auto patterns = extract_patterns(sample, 2, true, 4, nullptr);
	assert(patterns.size() > 1);
	OverlappingModel model(patterns, sample.palette, 2, true, kOutSize, kOutSize, kInvalidHash);

	// The sample tiled 2x2 is a valid torus configuration; pins and budgets are
	// derived from it (with slack) so the request is satisfiable by construction.
	const auto ref = [&](size_t x, size_t y) {
		return sample.data[(y % kSampleSize) * kSampleSize + (x % kSampleSize)];
	};
	const ColorIndex ink = 0, accent = 2;
	assert(ref(0, 0) == ink);
	assert(ref(kOutSize - 1, kOutSize - 1) == accent);

	const auto count_in = [&](size_t x0, size_t y0, size_t x1, size_t y1, ColorIndex c) {
		size_t n = 0;
		for (size_t y = y0; y < y1; ++y) {
			for (size_t x = x0; x < x1; ++x) {
				n += ref(x, y) == c;
			}
		}
		return n;
	};

	// Two overlapping half-open regions on the 16x16 output.
	const size_t ax1 = 16, ay1 = 12;          // A = [0,16) x [0,12)
	const size_t bx0 = 8, by0 = 4;            // B = [8,16) x [4,16)
	const size_t area_a = 16 * 12, area_b = 8 * 12;
	const size_t ca = count_in(0, 0, ax1, ay1, ink);
	const size_t cb = count_in(bx0, by0, 16, 16, ink);
	const size_t min_a = ca * 3 / 4, max_a = ca + (area_a - ca) / 2;
	const size_t min_b = cb * 3 / 4, max_b = cb + (area_b - cb) / 2;
	assert(0 < min_a && min_a <= ca && ca <= max_a && max_a < area_a);
	assert(0 < min_b && min_b <= cb && cb <= max_b && max_b < area_b);

	TextureConstraints constraints;
	constraints.pins.push_back({0, 0, kInk});
	constraints.pins.push_back({kOutSize - 1, kOutSize - 1, kAccent});
	constraints.budgets.push_back({0, 0, ax1, ay1, kInk, min_a, max_a});
	constraints.budgets.push_back({bx0, by0, 16, 16, kInk, min_b, max_b});

	auto output = create_output(model);
	const auto result = run_constrained(&output, model, constraints, 7);
	assert(result == Result::kSuccess);

	// Verify the original-size result: uniform contributions per pixel, pins,
	// and both budgets on physical pixels.
	const auto graphics = model.graphics(output);
	std::vector<RGBA> pixels(kOutSize * kOutSize);
	for (size_t y = 0; y < kOutSize; ++y) {
		for (size_t x = 0; x < kOutSize; ++x) {
			const auto& contributors = graphics.ref(x, y);
			assert(!contributors.empty());
			for (const auto c : contributors) {
				assert(c == contributors.front());
			}
			pixels[y * kOutSize + x] = sample.palette[contributors.front()];
		}
	}
	assert(pixels[0] == kInk);
	assert(pixels[kOutSize * kOutSize - 1] == kAccent);
	const auto count_pixels = [&](size_t x0, size_t y0, size_t x1, size_t y1) {
		size_t n = 0;
		for (size_t y = y0; y < y1; ++y) {
			for (size_t x = x0; x < x1; ++x) {
				n += pixels[y * kOutSize + x] == kInk;
			}
		}
		return n;
	};
	const size_t got_a = count_pixels(0, 0, ax1, ay1);
	const size_t got_b = count_pixels(bx0, by0, 16, 16);
	assert(min_a <= got_a && got_a <= max_a);
	assert(min_b <= got_b && got_b <= max_b);

	FILE* f = std::fopen(argv[1], "wb");
	assert(f != nullptr);
	std::fprintf(f, "P6\n%zu %zu\n255\n", kOutSize, kOutSize);
	for (const auto& p : pixels) {
		std::fputc(p.r, f);
		std::fputc(p.g, f);
		std::fputc(p.b, f);
	}
	std::fclose(f);

	std::cout << "WFC_CONSTRAINED_EXAMPLE_OK ink_a=" << got_a << " ink_b=" << got_b << "\n";
	return 0;
}
