// Constrained overlapping-model example.
//
// Builds a small input texture in memory, extracts N x N overlapping patterns
// from it (N > 1), runs one constrained synthesis with a pixel pin and two
// intersecting color budgets, writes the ORIGINAL-SIZE result as a binary PPM
// and verifies the physical pixel counts against the constraints.
//
// Usage: constrained <output.ppm>
//
// Included the same way as support/baseline.cpp: the upstream translation unit
// is pulled in with its main() renamed and this file supplies the real main().

#define main wfc_upstream_main
#include "main.cpp"
#undef main

#include <cassert>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using emilib::irange;

const RGBA kBlack{0, 0, 0, 255};
const RGBA kWhite{255, 255, 255, 255};

// A generated 4x4 two-color checkerboard sample texture.
PalettedImage make_sample_texture()
{
	const Palette palette{kBlack, kWhite};
	PalettedImage sample;
	sample.width  = 4;
	sample.height = 4;
	sample.palette = palette;
	sample.data.reserve(16);
	for (size_t y = 0; y < 4; ++y) {
		for (size_t x = 0; x < 4; ++x) {
			sample.data.push_back(static_cast<ColorIndex>((x + y) % 2));
		}
	}
	return sample;
}

void write_ppm(const std::string& path, const Image& image)
{
	std::ofstream file(path, std::ios::binary);
	if (!file) { throw std::runtime_error("cannot open output PPM: " + path); }
	file << "P6\n" << image.width() << ' ' << image.height() << "\n255\n";
	for (size_t y = 0; y < image.height(); ++y) {
		for (size_t x = 0; x < image.width(); ++x) {
			const RGBA pixel = image.get(x, y);
			const unsigned char rgb[3] = {pixel.r, pixel.g, pixel.b};
			file.write(reinterpret_cast<const char*>(rgb), 3);
		}
	}
	if (!file) { throw std::runtime_error("failed writing PPM: " + path); }
}

size_t count_color(const Image& image, size_t x0, size_t y0, size_t x1, size_t y1, RGBA color)
{
	size_t count = 0;
	for (size_t y = y0; y < y1; ++y) {
		for (size_t x = x0; x < x1; ++x) {
			if (image.get(x, y) == color) { ++count; }
		}
	}
	return count;
}

} // namespace

int main(int argc, char* argv[])
{
	if (argc != 2) {
		std::cerr << "usage: " << argv[0] << " <output.ppm>\n";
		return 2;
	}
	const std::string out_path = argv[1];

	constexpr int    N = 2; // deliberately not N = 1
	constexpr size_t W = 8;
	constexpr size_t H = 6;

	PalettedImage sample = make_sample_texture();
	const PatternPrevalence patterns =
		extract_patterns(sample, N, /*periodic_in=*/true, /*symmetry=*/1, nullptr);

	OverlappingModel model(patterns, sample.palette, N, /*periodic_out=*/false,
	                       W, H, kInvalidHash);

	TextureConstraints constraints;

	// Pin the bottom-right corner of the non-periodic output (the pixel covered
	// only by the last valid anchor, so the right/bottom edges are exercised).
	constraints.pins.push_back({W - 1, H - 1, kWhite});

	// Two intersecting half-open regions, both in force at once.
	const ColorBudget budget_a{0, 0, 4, 3, kWhite, 4, 8};   // [0,4) x [0,3)
	const ColorBudget budget_b{3, 1, 7, 6, kWhite, 8, 12};  // [3,7) x [1,6)
	constraints.budgets.push_back(budget_a);
	constraints.budgets.push_back(budget_b);

	Output output = create_output(model);
	const Result result = run_constrained(&output, model, constraints, 20261128);
	if (result != Result::kSuccess) {
		std::cerr << "constrained synthesis failed\n";
		return 1;
	}

	// Original-size image: image_from_graphics does NOT apply kUpscale.
	const Image image = image_from_graphics(model.graphics(output), model.palette());
	assert(image.width() == W && image.height() == H);

	// Every physical pixel must have a single, agreed color.
	const Graphics graphics = model.graphics(output);
	for (int y : irange(static_cast<int>(H))) {
		for (int x : irange(static_cast<int>(W))) {
			const auto& contributors = graphics.ref(x, y);
			assert(!contributors.empty());
			for (ColorIndex c : contributors) { assert(c == contributors.front()); (void)c; }
		}
	}

	// Verify the pin on physical pixels.
	assert(image.get(W - 1, H - 1) == kWhite);

	// Verify both budgets on the physical output pixels.
	const size_t count_a = count_color(image, budget_a.x0, budget_a.y0,
	                                   budget_a.x1, budget_a.y1, budget_a.color);
	const size_t count_b = count_color(image, budget_b.x0, budget_b.y0,
	                                   budget_b.x1, budget_b.y1, budget_b.color);
	assert(count_a >= budget_a.min_count && count_a <= budget_a.max_count);
	assert(count_b >= budget_b.min_count && count_b <= budget_b.max_count);

	write_ppm(out_path, image);

	std::cout << "pinned pixel (" << (W - 1) << "," << (H - 1) << ") = white\n";
	std::cout << "budget A white count = " << count_a
	          << " in [" << budget_a.min_count << "," << budget_a.max_count << "]\n";
	std::cout << "budget B white count = " << count_b
	          << " in [" << budget_b.min_count << "," << budget_b.max_count << "]\n";
	std::cout << "wrote " << out_path << " (" << W << "x" << H << ")\n";
	std::cout << "WFC_CONSTRAINED_EXAMPLE_OK\n";
	return 0;
}
