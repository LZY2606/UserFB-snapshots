// Tests for run_constrained(): pixel pins and regional color budgets on top of
// the overlapping WFC model, with constraint propagation and backtracking.
// Includes an independent brute-force oracle that enumerates pattern
// assignments on the valid anchors and checks overlap agreement, pins and
// physical-pixel budgets directly - it never calls the code under test.

#define main wfc_upstream_main
#include "main.cpp"
#undef main

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace
{

const RGBA kBlack{0, 0, 0, 255};
const RGBA kWhite{255, 255, 255, 255};
const RGBA kForeign{1, 2, 3, 4}; // never in any palette used here

Palette two_color_palette() { return {kBlack, kWhite}; }

PalettedImage checker_sample()
{
	PalettedImage sample{4, 4, {}, two_color_palette()};
	for (int y = 0; y < 4; ++y) {
		for (int x = 0; x < 4; ++x) {
			sample.data.push_back((x + y) % 2);
		}
	}
	return sample;
}

OverlappingModel checker_model(bool periodic, size_t width, size_t height)
{
	auto patterns = extract_patterns(checker_sample(), 2, true, 1, nullptr);
	assert(patterns.size() == 2);
	return OverlappingModel(patterns, two_color_palette(), 2, periodic, width, height, kInvalidHash);
}

std::vector<int> snapshot_wave(const Output& output, const OverlappingModel& model)
{
	std::vector<int> snap;
	for (size_t x = 0; x < model._width; ++x) {
		for (size_t y = 0; y < model._height; ++y) {
			for (size_t t = 0; t < model._num_patterns; ++t) {
				snap.push_back(output._wave.get(x, y, t));
			}
			snap.push_back(output._changes.get(x, y));
		}
	}
	return snap;
}

size_t palette_index_of(const Palette& palette, RGBA color)
{
	for (size_t i = 0; i < palette.size(); ++i) {
		if (palette[i] == color) { return i; }
	}
	assert(false && "color not in palette");
	return 0;
}

// Independent exhaustive reference: enumerate all pattern assignments on the
// valid anchors (respecting the bans already present in root), keep those where
// overlapping pixels agree, then check pins and budgets on the actual physical
// pixels. Returns true iff at least one assignment satisfies everything.
bool oracle_satisfiable(const OverlappingModel& model, const Output& root,
                        const TextureConstraints& constraints)
{
	const size_t w = model._width, h = model._height;
	const int n = model.n();
	const auto& palette = model.palette();

	std::vector<std::pair<size_t, size_t>> anchors;
	std::vector<std::vector<PatternIndex>> allowed;
	for (size_t y = 0; y < h; ++y) {
		for (size_t x = 0; x < w; ++x) {
			if (model.on_boundary((int)x, (int)y)) { continue; }
			anchors.push_back({x, y});
			std::vector<PatternIndex> a;
			for (size_t t = 0; t < model._num_patterns; ++t) {
				if (root._wave.get(x, y, t)) { a.push_back((PatternIndex)t); }
			}
			allowed.push_back(a);
		}
	}

	std::vector<std::pair<size_t, size_t>> pins;
	for (const auto& pin : constraints.pins) {
		pins.push_back({pin.y * w + pin.x, palette_index_of(palette, pin.color)});
	}
	struct OBudget { size_t x0, y0, x1, y1, color, lo, hi; };
	std::vector<OBudget> budgets;
	for (const auto& b : constraints.budgets) {
		budgets.push_back(OBudget{b.x0, b.y0, b.x1, b.y1,
		                          palette_index_of(palette, b.color), b.min_count, b.max_count});
	}

	std::vector<size_t> choice(anchors.size(), 0);
	for (;;) {
		// Evaluate this assignment: every pixel's contributions must agree.
		std::vector<int> pixel(w * h, -1);
		bool ok = true;
		for (size_t i = 0; i < anchors.size() && ok; ++i) {
			const auto t = allowed[i][choice[i]];
			const size_t sx = anchors[i].first, sy = anchors[i].second;
			for (int dy = 0; dy < n && ok; ++dy) {
				for (int dx = 0; dx < n && ok; ++dx) {
					size_t px = sx + dx, py = sy + dy;
					if (model._periodic_out) { px %= w; py %= h; }
					const int c = model.patterns()[t][dx + dy * n];
					auto& slot = pixel[py * w + px];
					if (slot < 0) { slot = c; }
					else if (slot != c) { ok = false; }
				}
			}
		}
		if (ok) {
			for (const auto& pin : pins) {
				if (pixel[pin.first] != (int)pin.second) { ok = false; }
			}
			for (const auto& b : budgets) {
				size_t count = 0;
				for (size_t y = b.y0; y < b.y1; ++y) {
					for (size_t x = b.x0; x < b.x1; ++x) {
						count += pixel[y * w + x] == (int)b.color;
					}
				}
				if (count < b.lo || count > b.hi) { ok = false; }
			}
			if (ok) { return true; }
		}
		// Next assignment (odometer).
		size_t i = anchors.size();
		bool advanced = false;
		while (i > 0) {
			--i;
			if (++choice[i] < allowed[i].size()) { advanced = true; break; }
			choice[i] = 0;
		}
		if (!advanced) { return false; }
	}
}

// Verify a successful wave independently: contributors agree per pixel, pins
// hold, budgets hold on physical pixels.
void check_solution(const OverlappingModel& model, const Output& output,
                    const TextureConstraints& constraints)
{
	const auto graphics = model.graphics(output);
	const auto& palette = model.palette();
	const size_t w = model._width, h = model._height;

	std::vector<RGBA> img(w * h);
	for (size_t y = 0; y < h; ++y) {
		for (size_t x = 0; x < w; ++x) {
			const auto& contributors = graphics.ref(x, y);
			assert(!contributors.empty());
			for (const auto c : contributors) {
				assert(c == contributors.front() && "pixel contributions disagree");
			}
			img[y * w + x] = palette[contributors.front()];
		}
	}
	for (const auto& pin : constraints.pins) {
		assert(img[pin.y * w + pin.x] == pin.color && "pin violated");
	}
	for (const auto& b : constraints.budgets) {
		size_t count = 0;
		for (size_t y = b.y0; y < b.y1; ++y) {
			for (size_t x = b.x0; x < b.x1; ++x) {
				count += img[y * w + x] == b.color;
			}
		}
		assert(count >= b.min_count && count <= b.max_count && "budget violated");
	}
}

void run_against_oracle(const OverlappingModel& model, const Output& root,
                        const TextureConstraints& constraints, size_t seed)
{
	const bool expected = oracle_satisfiable(model, root, constraints);
	Output output = root;
	const auto result = run_constrained(&output, model, constraints, seed);
	assert((result == Result::kSuccess) == expected);
	if (result == Result::kSuccess) {
		check_solution(model, output, constraints);
	} else {
		assert(snapshot_wave(output, model) == snapshot_wave(root, model));
	}
}

// Periodic output, N = 2, a pin and two budgets, cross-checked with the oracle.
void test_periodic_with_pin_and_budget()
{
	auto model = checker_model(true, 4, 4);
	auto root = create_output(model);

	TextureConstraints tc;
	tc.pins.push_back({2, 1, kWhite});
	tc.budgets.push_back({0, 0, 4, 4, kWhite, 8, 8});
	tc.budgets.push_back({1, 1, 3, 3, kBlack, 2, 2});

	run_against_oracle(model, root, tc, 42);
}

// Non-periodic output, N = 2: pin the bottom-right corner pixel, which is only
// covered by the single anchor (width-n, height-n).
void test_planar_corner_pin()
{
	auto model = checker_model(false, 5, 4);
	auto root = create_output(model);

	TextureConstraints tc;
	tc.pins.push_back({4, 3, kWhite});

	run_against_oracle(model, root, tc, 42);

	Output output = root;
	assert(run_constrained(&output, model, tc, 42) == Result::kSuccess);
	const auto graphics = model.graphics(output);
	const auto& contributors = graphics.ref(4, 3);
	assert(!contributors.empty() && contributors.front() == 1); // white
}

// N = 1 cells are independent; two intersecting budgets plus a pin must trigger
// each other in a chain: pin cell0=white saturates budget A (so cell1!=white),
// which leaves cell2 as the only possible white for budget B.
void test_intersecting_budgets_trigger()
{
	PalettedImage sample{2, 2, {}, two_color_palette()};
	for (int y = 0; y < 2; ++y) {
		for (int x = 0; x < 2; ++x) {
			sample.data.push_back(x % 2);
		}
	}
	auto patterns = extract_patterns(sample, 1, true, 1, nullptr);
	assert(patterns.size() == 2);
	OverlappingModel model(patterns, two_color_palette(), 1, false, 4, 1, kInvalidHash);
	auto root = create_output(model);

	TextureConstraints tc;
	tc.pins.push_back({0, 0, kWhite});
	tc.budgets.push_back({0, 0, 2, 1, kWhite, 1, 1});
	tc.budgets.push_back({1, 0, 3, 1, kWhite, 1, 1});
	// A duplicate same-color/same-rect budget must not be merged into a looser one.
	tc.budgets.push_back({1, 0, 3, 1, kWhite, 1, 1});

	run_against_oracle(model, root, tc, 5);

	Output output = root;
	assert(run_constrained(&output, model, tc, 5) == Result::kSuccess);
	const auto graphics = model.graphics(output);
	assert(graphics.ref(0, 0).front() == 1); // pinned white
	assert(graphics.ref(1, 0).front() == 0); // budget A saturated -> black
	assert(graphics.ref(2, 0).front() == 1); // budget B forces white
}

// Bans already present in the incoming wave are root constraints.
void test_root_preban()
{
	auto model = checker_model(true, 4, 4);
	auto root = create_output(model);

	size_t banned = kInvalidIndex;
	for (size_t t = 0; t < model._num_patterns; ++t) {
		if (model.patterns()[t][0] == 1) { banned = t; } // white top-left
	}
	assert(banned != kInvalidIndex);
	root._wave.set(0, 0, banned, false); // caller does not flag _changes

	TextureConstraints tc; // no extra constraints
	run_against_oracle(model, root, tc, 11);

	Output output = root;
	assert(run_constrained(&output, model, tc, 11) == Result::kSuccess);
	assert(!output._wave.get(0, 0, banned) && "root ban was lost");
	check_solution(model, output, tc);
}

// Every invalid input must throw std::invalid_argument before touching output.
void test_invalid_inputs()
{
	auto model = checker_model(true, 4, 4);
	auto root = create_output(model);
	const auto root_snap = snapshot_wave(root, model);

	const auto expect_throw = [&](const TextureConstraints& tc) {
		Output output = root;
		bool threw = false;
		try {
			run_constrained(&output, model, tc, 1);
		} catch (const std::invalid_argument&) {
			threw = true;
		}
		assert(threw);
		assert(snapshot_wave(output, model) == root_snap && "invalid input modified output");
	};

	{ TextureConstraints tc; tc.pins.push_back({4, 0, kWhite});   expect_throw(tc); } // x out of bounds
	{ TextureConstraints tc; tc.pins.push_back({0, 4, kWhite});   expect_throw(tc); } // y out of bounds
	{ TextureConstraints tc; tc.pins.push_back({0, 0, kForeign}); expect_throw(tc); } // color not in palette
	{ TextureConstraints tc; tc.budgets.push_back({1, 0, 1, 2, kWhite, 0, 0}); expect_throw(tc); } // empty rect
	{ TextureConstraints tc; tc.budgets.push_back({0, 1, 2, 1, kWhite, 0, 0}); expect_throw(tc); } // empty rect
	{ TextureConstraints tc; tc.budgets.push_back({0, 0, 5, 1, kWhite, 0, 1}); expect_throw(tc); } // rect out of bounds
	{ TextureConstraints tc; tc.budgets.push_back({0, 0, 1, 5, kWhite, 0, 1}); expect_throw(tc); } // rect out of bounds
	{ TextureConstraints tc; tc.budgets.push_back({0, 0, 2, 2, kWhite, 3, 2}); expect_throw(tc); } // min > max
	{ TextureConstraints tc; tc.budgets.push_back({0, 0, 2, 2, kWhite, 0, 5}); expect_throw(tc); } // max > area
	{ TextureConstraints tc; tc.budgets.push_back({0, 0, 2, 2, kForeign, 0, 1}); expect_throw(tc); } // bad color

	// Wave dimensions not matching the model.
	Output bad;
	bad._wave    = Array3D<Bool>(5, 4, model._num_patterns, true);
	bad._changes = Array2D<Bool>(5, 4, false);
	bool threw = false;
	try {
		run_constrained(&bad, model, TextureConstraints{}, 1);
	} catch (const std::invalid_argument&) {
		threw = true;
	}
	assert(threw);
}

// Legal but contradictory requests return kFail and restore the wave.
void test_contradiction_restores()
{
	auto model = checker_model(true, 4, 4);
	auto root = create_output(model);
	const auto root_snap = snapshot_wave(root, model);

	// Two pins on the same pixel with different colors.
	{
		Output output = root;
		TextureConstraints tc;
		tc.pins.push_back({1, 1, kWhite});
		tc.pins.push_back({1, 1, kBlack});
		assert(run_constrained(&output, model, tc, 3) == Result::kFail);
		assert(snapshot_wave(output, model) == root_snap);
	}
	// A pin contradicting a budget.
	{
		Output output = root;
		TextureConstraints tc;
		tc.pins.push_back({0, 0, kBlack});
		tc.budgets.push_back({0, 0, 1, 1, kWhite, 1, 1});
		assert(run_constrained(&output, model, tc, 3) == Result::kFail);
		assert(snapshot_wave(output, model) == root_snap);
	}
	// Globally unsatisfiable: odd checkerboard torus with a pin.
	{
		auto odd = checker_model(true, 5, 5);
		auto output = create_output(odd);
		const auto snap = snapshot_wave(output, odd);
		TextureConstraints tc;
		tc.pins.push_back({2, 2, kWhite});
		assert(run_constrained(&output, odd, tc, 7) == Result::kFail);
		assert(snapshot_wave(output, odd) == snap);
	}
}

// Same model, root wave, constraints and seed -> identical result.
void test_seed_reproducible()
{
	auto model = checker_model(true, 4, 4);
	TextureConstraints tc;
	tc.budgets.push_back({0, 0, 4, 4, kWhite, 6, 10}); // loose: real choices remain

	const auto run_once = [&](size_t seed) {
		auto output = create_output(model);
		assert(run_constrained(&output, model, tc, seed) == Result::kSuccess);
		check_solution(model, output, tc);
		return snapshot_wave(output, model);
	};

	assert(run_once(99) == run_once(99));
	run_once(100); // a different seed still yields a valid solution
}

// Backtracking witness. Patterns: two checker phases with heavy weights plus an
// all-black pattern with a tiny weight. On the 5x5 torus both checker phases
// are globally impossible, but propagation only finds out after a decision has
// been pushed around the odd cycle. The old non-backtracking run() commits to
// its first (heavy) branch and fails; run_constrained() must backtrack out of
// the doomed heavy branches and succeed with the light all-black pattern.
void test_backtracking_recovers()
{
	PatternPrevalence prevalence;
	prevalence[hash_from_pattern({0, 1, 1, 0}, 2)] = 1000; // checker A, heaviest
	prevalence[hash_from_pattern({1, 0, 0, 1}, 2)] =  100; // checker B
	prevalence[hash_from_pattern({0, 0, 0, 0}, 2)] =    1; // all black, lightest
	OverlappingModel model(prevalence, two_color_palette(), 2, true, 5, 5, kInvalidHash);
	assert(model._num_patterns == 3);

	const size_t seed = 5; // with this seed the first branch tried is checker A

	// Branch evidence, part 1: without backtracking the heavy first choice is
	// committed and the whole run fails.
	{
		auto output = create_output(model);
		assert(run(&output, model, seed, 0, nullptr) == Result::kFail);
	}

	// Branch evidence, part 2: the constrained solver recovers and the only
	// surviving branch is the lightest pattern - every pixel ends up black.
	TextureConstraints tc;
	{
		auto output = create_output(model);
		assert(run_constrained(&output, model, tc, seed) == Result::kSuccess);
		check_solution(model, output, tc);
		const auto graphics = model.graphics(output);
		for (size_t y = 0; y < 5; ++y) {
			for (size_t x = 0; x < 5; ++x) {
				assert(graphics.ref(x, y).front() == 0);
			}
		}
	}

	// Branch evidence, part 3: with the light pattern banned at the root, the
	// heavy branches are all infeasible and the search must exhaust to kFail.
	{
		size_t all_black = kInvalidIndex;
		for (size_t t = 0; t < model._num_patterns; ++t) {
			bool uniform = true;
			for (const auto c : model.patterns()[t]) { uniform &= c == 0; }
			if (uniform) { all_black = t; }
		}
		assert(all_black != kInvalidIndex);
		auto output = create_output(model);
		for (size_t y = 0; y < 5; ++y) {
			for (size_t x = 0; x < 5; ++x) {
				output._wave.set(x, y, all_black, false);
			}
		}
		assert(run_constrained(&output, model, tc, seed) == Result::kFail);
	}
}

// Alternate satisfiable and unsatisfiable requests; failures must restore the
// wave and must not disturb subsequent runs.
void test_alternating_requests()
{
	auto model = checker_model(true, 4, 4);
	for (size_t i = 0; i < 3; ++i) {
		TextureConstraints ok_tc;
		ok_tc.pins.push_back({i, 0, kWhite});

		auto out_ok = create_output(model);
		assert(run_constrained(&out_ok, model, ok_tc, 40 + i) == Result::kSuccess);
		check_solution(model, out_ok, ok_tc);

		auto out_bad = create_output(model);
		const auto snap = snapshot_wave(out_bad, model);
		TextureConstraints bad_tc;
		bad_tc.pins.push_back({0, 0, kWhite});
		bad_tc.pins.push_back({0, 0, kBlack});
		assert(run_constrained(&out_bad, model, bad_tc, 40 + i) == Result::kFail);
		assert(snapshot_wave(out_bad, model) == snap);

		auto out_ok2 = create_output(model);
		assert(run_constrained(&out_ok2, model, ok_tc, 40 + i) == Result::kSuccess);
		check_solution(model, out_ok2, ok_tc);
	}
}

} // namespace

int main()
{
	test_periodic_with_pin_and_budget();
	test_planar_corner_pin();
	test_intersecting_budgets_trigger();
	test_root_preban();
	test_invalid_inputs();
	test_contradiction_restores();
	test_seed_reproducible();
	test_backtracking_recovers();
	test_alternating_requests();
	std::cout << "WFC_CONSTRAINED_OK\n";
	return 0;
}
