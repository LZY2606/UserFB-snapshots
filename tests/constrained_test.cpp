// Tests for the constrained overlapping-model solver.
//
// Included the same way as support/baseline.cpp: the upstream translation unit
// is pulled in with its main() renamed, and this file provides its own main().
//
// Expected results come from an INDEPENDENT exhaustive solver: it enumerates
// pattern assignments at every valid anchor and checks overlapping-pixel
// consistency, pins and physical-pixel budgets directly. It never calls any
// function of the constrained solver under test.

#define main wfc_upstream_main
#include "main.cpp"
#undef main

#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {

using emilib::irange;

const RGBA kBlack{0, 0, 0, 255};
const RGBA kWhite{255, 255, 255, 255};
const RGBA kRed{255, 0, 0, 255};

int color_index_of(const Palette& palette, RGBA color)
{
	for (auto i : irange(palette.size())) {
		if (palette[i] == color) { return static_cast<int>(i); }
	}
	return -1;
}

Pattern solid_pattern(int n, ColorIndex color)
{
	return Pattern(n * n, color);
}

Pattern pattern_from_rows(int n, std::initializer_list<ColorIndex> rows)
{
	assert(rows.size() == static_cast<size_t>(n * n));
	(void)n;
	return Pattern(rows.begin(), rows.end());
}

PatternHash hash_of(const Pattern& pattern, size_t palette_size)
{
	return hash_from_pattern(pattern, palette_size);
}

PatternPrevalence prevalence_of(std::initializer_list<std::pair<Pattern, size_t>> items,
                                size_t palette_size)
{
	PatternPrevalence result;
	for (const auto& item : items) {
		result[hash_of(item.first, palette_size)] = item.second;
	}
	return result;
}

int find_pattern_index(const OverlappingModel& model, const Pattern& wanted)
{
	const auto& patterns = model.patterns();
	for (auto i : irange(patterns.size())) {
		if (patterns[i] == wanted) { return static_cast<int>(i); }
	}
	return -1;
}

// ----------------------------------------------------------------------------
// Independent exhaustive oracle
// ----------------------------------------------------------------------------

struct OracleBudget
{
	int                 color;
	size_t              lo, hi;
	std::vector<size_t> pixels;
};

struct ExhaustiveOracle
{
	const OverlappingModel&       model;
	int                           n, T, W, H;
	bool                          periodic;
	const std::vector<Pattern>*   patterns;
	std::vector<std::pair<int,int>> anchors;
	std::vector<std::vector<char>> root_allowed; // [anchor][pattern]
	std::vector<std::pair<size_t, int>> pins;    // pixel -> color
	std::vector<OracleBudget>     budgets;

	ExhaustiveOracle(const OverlappingModel& m, const Output& root,
	                 const TextureConstraints& constraints)
		: model(m)
	{
		n  = m.n();
		T  = static_cast<int>(m._num_patterns);
		W  = static_cast<int>(m._width);
		H  = static_cast<int>(m._height);
		periodic = m.periodic();
		patterns = &m.patterns();

		for (int y = 0; y < H; ++y) {
			for (int x = 0; x < W; ++x) {
				if (m.on_boundary(x, y)) { continue; }
				std::vector<char> allowed(T);
				for (int t = 0; t < T; ++t) {
					allowed[t] = root._wave.get(x, y, t) ? 1 : 0;
				}
				root_allowed.push_back(std::move(allowed));
				anchors.emplace_back(x, y);
			}
		}

		const Palette& palette = m.palette();
		for (const auto& pin : constraints.pins) {
			pins.emplace_back(pin.y * W + pin.x, color_index_of(palette, pin.color));
		}
		for (const auto& budget : constraints.budgets) {
			OracleBudget ob;
			ob.color = color_index_of(palette, budget.color);
			ob.lo = budget.min_count;
			ob.hi = budget.max_count;
			for (size_t y = budget.y0; y < budget.y1; ++y) {
				for (size_t x = budget.x0; x < budget.x1; ++x) {
					ob.pixels.push_back(y * W + x);
				}
			}
			budgets.push_back(std::move(ob));
		}
	}

	// If forced[slot] >= 0, that anchor must take exactly that pattern.
	bool satisfiable(const std::vector<int>* forced = nullptr) const
	{
		const size_t A = anchors.size();
		std::vector<int> assignment(A, 0);

		while (true) {
			if (assignment_valid(assignment, forced)) { return true; }

			// Odometer increment.
			size_t i = 0;
			while (i < A) {
				assignment[i] += 1;
				if (assignment[i] < T) { break; }
				assignment[i] = 0;
				++i;
			}
			if (i == A) { return false; }
		}
	}

	bool assignment_valid(const std::vector<int>& assignment,
	                      const std::vector<int>* forced) const
	{
		for (size_t a = 0; a < anchors.size(); ++a) {
			const int t = assignment[a];
			if (!root_allowed[a][t]) { return false; }
			if (forced && (*forced)[static_cast<size_t>(a)] >= 0 &&
			    (*forced)[static_cast<size_t>(a)] != t) {
				return false;
			}
		}

		std::vector<int> pixel_color(static_cast<size_t>(W) * H, -1);

		for (size_t a = 0; a < anchors.size(); ++a) {
			const int ax = anchors[a].first;
			const int ay = anchors[a].second;
			const Pattern& pattern = (*patterns)[assignment[a]];
			for (int dy = 0; dy < n; ++dy) {
				for (int dx = 0; dx < n; ++dx) {
					int px = ax + dx;
					int py = ay + dy;
					if (periodic) {
						px %= W;
						py %= H;
					}
					assert(px >= 0 && px < W && py >= 0 && py < H);
					const int color = pattern[dy * n + dx];
					const int idx = py * W + px;
					if (pixel_color[idx] >= 0 && pixel_color[idx] != color) {
						return false; // overlapping patterns disagree
					}
					pixel_color[idx] = color;
				}
			}
		}

		for (int color : pixel_color) {
			if (color < 0) { return false; } // uncovered physical pixel
		}
		for (const auto& pin : pins) {
			if (pixel_color[pin.first] != pin.second) { return false; }
		}
		for (const OracleBudget& budget : budgets) {
			size_t count = 0;
			for (size_t pixel : budget.pixels) {
				if (pixel_color[pixel] == budget.color) { ++count; }
			}
			if (count < budget.lo || count > budget.hi) { return false; }
		}
		return true;
	}
};

// ----------------------------------------------------------------------------
// Verification of a successful solver result on the physical output pixels
// ----------------------------------------------------------------------------

void verify_solution(const Output& output, const OverlappingModel& model,
                     const TextureConstraints& constraints)
{
	const Graphics graphics = model.graphics(output);
	const Palette& palette = model.palette();
	const int W = static_cast<int>(model._width);
	const int H = static_cast<int>(model._height);

	std::vector<RGBA> actual(static_cast<size_t>(W) * H);

	for (int y = 0; y < H; ++y) {
		for (int x = 0; x < W; ++x) {
			const auto& contributors = graphics.ref(x, y);
			assert(!contributors.empty());
			// Every covering pattern must agree on this physical pixel.
			for (ColorIndex c : contributors) {
				assert(c == contributors.front());
				(void)c;
			}
			actual[y * W + x] = palette[contributors.front()];
		}
	}

	for (const auto& pin : constraints.pins) {
		assert(actual[pin.y * W + pin.x] == pin.color);
		(void)pin;
	}
	for (const auto& budget : constraints.budgets) {
		size_t count = 0;
		for (size_t y = budget.y0; y < budget.y1; ++y) {
			for (size_t x = budget.x0; x < budget.x1; ++x) {
				if (actual[y * W + x] == budget.color) { ++count; }
			}
		}
		assert(count >= budget.min_count);
		assert(count <= budget.max_count);
		(void)count;
	}
}

std::vector<unsigned char> snapshot_bytes(const Output& output)
{
	const size_t slots = output._changes.width() * output._changes.height();
	const size_t H = output._changes.height();
	const size_t T = output._wave.size() / slots;
	std::vector<unsigned char> bytes;
	for (size_t i = 0; i < output._wave.size(); ++i) {
		const size_t t = i % T;
		const size_t y = (i / T) % H;
		const size_t x = i / (T * H);
		bytes.push_back(output._wave.get(x, y, t));
	}
	for (size_t y = 0; y < output._changes.height(); ++y) {
		for (size_t x = 0; x < output._changes.width(); ++x) {
			bytes.push_back(output._changes.get(x, y));
		}
	}
	return bytes;
}

// ----------------------------------------------------------------------------
// Reusable model setups
// ----------------------------------------------------------------------------

// Two checkerboard 2x2 patterns on a black/white palette.
OverlappingModel make_checker(int W, int H, bool periodic)
{
	const Palette palette{kBlack, kWhite};
	const Pattern p0 = pattern_from_rows(2, {0, 1, 1, 0});
	const Pattern p1 = pattern_from_rows(2, {1, 0, 0, 1});
	auto prevalence = prevalence_of({{p0, 1}, {p1, 1}}, palette.size());
	return OverlappingModel(prevalence, palette, 2, periodic,
	                        static_cast<size_t>(W), static_cast<size_t>(H), kInvalidHash);
}

// Row-stripe 2x2 patterns: any vertical sequence of row colors is feasible.
// A = both rows black, B = both rows white, H0 = black over white,
// H1 = white over black.
struct StripeSetup
{
	Pattern A, B, H0, H1;
	OverlappingModel model;
};

StripeSetup make_stripe(int W, int H, bool periodic,
                        size_t wA, size_t wB, size_t wH0, size_t wH1)
{
	const Pattern A   = solid_pattern(2, 0);
	const Pattern B   = solid_pattern(2, 1);
	const Pattern H0  = pattern_from_rows(2, {0, 0, 1, 1});
	const Pattern H1  = pattern_from_rows(2, {1, 1, 0, 0});
	const auto prevalence = prevalence_of({{A, wA}, {B, wB}, {H0, wH0}, {H1, wH1}}, 2);
	OverlappingModel model(prevalence, Palette{kBlack, kWhite}, 2, periodic,
	                       static_cast<size_t>(W), static_cast<size_t>(H), kInvalidHash);
	return StripeSetup{A, B, H0, H1, std::move(model)};
}

// ----------------------------------------------------------------------------

void test_periodic_checker_pins_and_budget()
{
	auto model = make_checker(4, 4, true);

	TextureConstraints tc;
	tc.pins.push_back({0, 0, kWhite});
	tc.pins.push_back({3, 3, kWhite}); // bottom-right corner of the torus
	tc.budgets.push_back({0, 0, 4, 4, kWhite, 8, 8});

	auto output = create_output(model);
	ExhaustiveOracle oracle(model, output, tc);
	assert(oracle.satisfiable());

	assert(run_constrained(&output, model, tc, 123) == Result::kSuccess);
	verify_solution(output, model, tc);
}

void test_periodic_wrapped_pixel_counted_once()
{
	// Tight min==max budget on the whole torus; the wrap-around coverage of
	// corner pixels must not inflate the physical pixel count.
	auto model = make_checker(4, 4, true);

	TextureConstraints tc;
	tc.budgets.push_back({0, 0, 4, 4, kBlack, 8, 8});
	tc.budgets.push_back({1, 1, 3, 3, kWhite, 2, 2}); // inner 2x2 of a checker

	auto output = create_output(model);
	ExhaustiveOracle oracle(model, output, tc);
	assert(oracle.satisfiable());

	assert(run_constrained(&output, model, tc, 7) == Result::kSuccess);
	verify_solution(output, model, tc);
}

void test_non_periodic_corner_pin_and_budget()
{
	StripeSetup sm = make_stripe(2, 4, false, 1, 1, 1, 1);
	const OverlappingModel& model = sm.model;

	TextureConstraints tc;
	tc.pins.push_back({1, 3, kWhite}); // bottom-right corner pixel
	tc.budgets.push_back({0, 0, 2, 4, kWhite, 2, 6});

	auto output = create_output(model);
	ExhaustiveOracle oracle(model, output, tc);
	assert(oracle.satisfiable());

	assert(run_constrained(&output, model, tc, 99) == Result::kSuccess);
	verify_solution(output, model, tc);
}

void test_intersecting_budgets_trigger_each_other()
{
	// Two overlapping rectangles with tight quotas; each restriction must feed
	// the other one through the shared pixels.
	auto model = make_checker(4, 4, true);

	TextureConstraints tc;
	tc.budgets.push_back({0, 0, 4, 2, kWhite, 4, 4}); // top two rows: half white
	tc.budgets.push_back({0, 0, 2, 4, kBlack, 4, 4}); // left two cols: half black
	tc.budgets.push_back({0, 0, 4, 4, kWhite, 8, 8}); // whole board, overlaps both

	auto output = create_output(model);
	ExhaustiveOracle oracle(model, output, tc);
	assert(oracle.satisfiable());

	assert(run_constrained(&output, model, tc, 55) == Result::kSuccess);
	verify_solution(output, model, tc);

	// Same shape but an impossible extra quota on the overlapping region.
	TextureConstraints impossible = tc;
	impossible.budgets.push_back({0, 0, 2, 2, kWhite, 4, 4});
	auto bad_output = create_output(model);
	ExhaustiveOracle bad_oracle(model, bad_output, impossible);
	assert(!bad_oracle.satisfiable());
	assert(run_constrained(&bad_output, model, impossible, 55) == Result::kFail);
}

void test_root_pre_bans_are_respected()
{
	auto model = make_checker(4, 4, true);
	auto output = create_output(model);

	const Pattern p1 = pattern_from_rows(2, {1, 0, 0, 1}); // white at local (0, 0)
	const int white_corner = find_pattern_index(model, p1);
	assert(white_corner >= 0);

	// Root constraint: forbid the white-corner pattern at one anchor and pin
	// that corner black. The other pattern supplies the black pixel, so this
	// is consistent and must survive.
	output._wave.set(0, 0, white_corner, false);
	output._changes.set(0, 0, true);
	while (model.propagate(&output));

	TextureConstraints tc;
	tc.pins.push_back({0, 0, kBlack});

	ExhaustiveOracle oracle(model, output, tc);
	assert(oracle.satisfiable());
	assert(run_constrained(&output, model, tc, 3) == Result::kSuccess);
	verify_solution(output, model, tc);

	// A root ban that contradicts the pin must make the whole thing fail:
	// forbid the only pattern whose local (0, 0) is black, then pin it black.
	auto dead = create_output(model);
	const Pattern p0 = pattern_from_rows(2, {0, 1, 1, 0});
	const int black_corner = find_pattern_index(model, p0);
	dead._wave.set(0, 0, black_corner, false);
	dead._changes.set(0, 0, true);
	while (model.propagate(&dead));
	TextureConstraints pin_black;
	pin_black.pins.push_back({0, 0, kBlack});
	ExhaustiveOracle dead_oracle(model, dead, pin_black);
	assert(!dead_oracle.satisfiable());
	assert(run_constrained(&dead, model, pin_black, 3) == Result::kFail);
}

void test_odd_torus_budget_contradiction()
{
	auto model = make_checker(5, 5, true);
	TextureConstraints tc;
	tc.budgets.push_back({0, 0, 5, 5, kWhite, 1, 25});

	auto output = create_output(model);
	ExhaustiveOracle oracle(model, output, tc);
	assert(!oracle.satisfiable()); // no checker assignment exists on an odd torus
	assert(run_constrained(&output, model, tc, 11) == Result::kFail);
}

void test_invalid_arguments_leave_output_untouched()
{
	auto model = make_checker(4, 4, true);
	auto output = create_output(model);
	const auto before = snapshot_bytes(output);

	auto expect_throw = [&](const TextureConstraints& tc) {
		auto probe = create_output(model);
		const auto probe_before = snapshot_bytes(probe);
		bool threw = false;
		try {
			run_constrained(&probe, model, tc, 1);
		} catch (const std::invalid_argument&) {
			threw = true;
		}
		assert(threw);
		assert(snapshot_bytes(probe) == probe_before);
		(void)threw;
	};

	TextureConstraints tc;

	tc = {};
	tc.pins.push_back({4, 0, kWhite}); // x out of bounds
	expect_throw(tc);

	tc = {};
	tc.pins.push_back({0, 4, kWhite}); // y out of bounds
	expect_throw(tc);

	tc = {};
	tc.pins.push_back({0, 0, kRed}); // color not in palette
	expect_throw(tc);

	tc = {};
	tc.budgets.push_back({2, 0, 2, 4, kWhite, 0, 0}); // empty rectangle
	expect_throw(tc);

	tc = {};
	tc.budgets.push_back({0, 0, 5, 4, kWhite, 0, 4}); // x1 out of bounds
	expect_throw(tc);

	tc = {};
	tc.budgets.push_back({0, 0, 4, 4, kWhite, 9, 8}); // min > max
	expect_throw(tc);

	tc = {};
	tc.budgets.push_back({0, 0, 2, 2, kWhite, 0, 5}); // max > area
	expect_throw(tc);

	tc = {};
	tc.budgets.push_back({0, 0, 4, 4, kRed, 0, 4}); // unknown budget color
	expect_throw(tc);

	// Wave dimensions do not match the model.
	{
		auto other = make_checker(6, 6, true);
		auto wrong = create_output(other);
		bool threw = false;
		try {
			run_constrained(&wrong, model, TextureConstraints{}, 1);
		} catch (const std::invalid_argument&) {
			threw = true;
		}
		assert(threw);
		(void)threw;
	}

	assert(snapshot_bytes(output) == before);
}

void test_failure_restores_output()
{
	auto model = make_checker(4, 4, true);
	auto output = create_output(model);
	const auto before = snapshot_bytes(output);

	TextureConstraints impossible;
	impossible.pins.push_back({0, 0, kWhite});
	impossible.pins.push_back({1, 0, kWhite}); // checker neighbors must differ
	assert(run_constrained(&output, model, impossible, 4) == Result::kFail);
	assert(snapshot_bytes(output) == before);
}

void test_fixed_seed_is_reproducible()
{
	auto model = make_checker(6, 6, true);
	TextureConstraints tc;
	tc.budgets.push_back({0, 0, 6, 6, kWhite, 18, 18});

	auto a = create_output(model);
	auto b = create_output(model);
	assert(run_constrained(&a, model, tc, 2026) == Result::kSuccess);
	assert(run_constrained(&b, model, tc, 2026) == Result::kSuccess);

	const Graphics ga = model.graphics(a);
	const Graphics gb = model.graphics(b);
	for (int y = 0; y < 6; ++y) {
		for (int x = 0; x < 6; ++x) {
			assert(ga.ref(x, y) == gb.ref(x, y));
		}
	}
	verify_solution(a, model, tc);
}

void test_n1_model()
{
	const Palette palette{kBlack, kWhite};
	// 1x1 patterns: each anchor is simply a pixel color.
	const Pattern black{0};
	const Pattern white{1};
	PatternPrevalence prevalence;
	prevalence[hash_of(black, 2)] = 1;
	prevalence[hash_of(white, 2)] = 1;
	OverlappingModel model(prevalence, palette, 1, false, 4, 3, kInvalidHash);

	TextureConstraints tc;
	tc.pins.push_back({0, 0, kBlack});
	tc.pins.push_back({3, 2, kWhite}); // bottom-right corner
	tc.budgets.push_back({0, 0, 4, 3, kWhite, 4, 4});

	auto output = create_output(model);
	ExhaustiveOracle oracle(model, output, tc);
	assert(oracle.satisfiable());
	assert(run_constrained(&output, model, tc, 17) == Result::kSuccess);
	verify_solution(output, model, tc);
}

// Explicit backtracking evidence: the heavyweight first branch is unsolvable,
// the alternative branch is solvable, and the solver must switch between them.
void test_backtracking_is_real()
{
	// A is given a dominating weight so it is always observed before H1.
	StripeSetup sm = make_stripe(2, 4, false, 1000, 1, 1, 1);
	const OverlappingModel& model = sm.model;

	const int idx_A  = find_pattern_index(model, sm.A);
	const int idx_H1 = find_pattern_index(model, sm.H1);
	assert(idx_A >= 0 && idx_H1 >= 0);

	auto make_root = [&]() {
		Output root = create_output(model);
		// Non-periodic 2x4 model with n=2: anchors are (0,0), (0,1), (0,2).
		// Freeze the two lower anchors to A (black rows); the top anchor keeps
		// {A, H1} as its only candidates after overlap propagation.
		for (int t = 0; t < static_cast<int>(model._num_patterns); ++t) {
			if (t != idx_A) {
				root._wave.set(0, 1, t, false);
				root._wave.set(0, 2, t, false);
			}
		}
		root._changes.set(0, 1, true);
		root._changes.set(0, 2, true);
		while (model.propagate(&root));
		return root;
	};

	TextureConstraints tc;
	// Rows 1..3 are forced black by the root; only row 0 can be white. A quota
	// of at least one white pixel rules out the all-black A branch but accepts
	// the H1 branch (two white pixels), and it does not trigger at the root
	// because two white pixels are still merely possible there.
	tc.budgets.push_back({0, 0, 2, 4, kWhite, 1, 2});

	Output root = make_root();

	// Deterministic branch-order evidence. Replicate the solver's observation
	// rule (the same spin_the_bottle helper and 0.5*uniform noise bias used by
	// the existing observe()) to prove that, with seed 42, the very first
	// decision on the only open slot is the dead A branch.
	{
		std::mt19937 gen(42);
		std::uniform_real_distribution<double> dis(0.0, 1.0);
		dis(gen); // entropy tie-break noise consumed first
		std::vector<double> distribution(model._num_patterns, 0.0);
		distribution[idx_A]  = model._pattern_weight[idx_A];
		distribution[idx_H1] = model._pattern_weight[idx_H1];
		const int first_choice = static_cast<int>(
			spin_the_bottle(std::move(distribution), dis(gen)));
		assert(first_choice == idx_A); // heavyweight branch is tried first
		(void)first_choice;
	}

	// Anchor (0,0) is the only slot with more than one candidate.
	std::vector<int> forced_A(model._width * model._height, -1);
	forced_A[0] = idx_A;
	std::vector<int> forced_H1 = forced_A;
	forced_H1[0] = idx_H1;

	ExhaustiveOracle oracle(model, root, tc);
	assert(!oracle.satisfiable(&forced_A));  // first branch provably dead
	assert(oracle.satisfiable(&forced_H1));  // alternative branch provably alive

	// With H1 banned up front, only the dead branch remains: must fail.
	Output dead = make_root();
	dead._wave.set(0, 0, idx_H1, false);
	dead._changes.set(0, 0, true);
	assert(run_constrained(&dead, model, tc, 42) == Result::kFail);

	// With both branches available the weighted observation picks A first,
	// hits the budget contradiction, restores the snapshot and succeeds on H1.
	Output search = make_root();
	assert(run_constrained(&search, model, tc, 42) == Result::kSuccess);
	verify_solution(search, model, tc);

	// Reproducibility across the full backtracking search.
	Output search2 = make_root();
	assert(run_constrained(&search2, model, tc, 42) == Result::kSuccess);
	assert(model.graphics(search).ref(0, 0) == model.graphics(search2).ref(0, 0));
}

void test_alternating_solvable_and_unsolvable()
{
	auto model = make_checker(4, 4, true);

	TextureConstraints sat;
	sat.pins.push_back({0, 0, kWhite});
	sat.budgets.push_back({0, 0, 4, 4, kBlack, 8, 8});

	TextureConstraints unsat;
	unsat.pins.push_back({0, 0, kWhite});
	unsat.pins.push_back({0, 1, kWhite}); // adjacent checker pixels cannot match

	for (int round = 0; round < 4; ++round) {
		auto good = create_output(model);
		const auto good_before = snapshot_bytes(good);
		assert(run_constrained(&good, model, sat, 31) == Result::kSuccess);
		verify_solution(good, model, sat);

		auto bad = create_output(model);
		const auto bad_before = snapshot_bytes(bad);
		assert(run_constrained(&bad, model, unsat, 31) == Result::kFail);
		assert(snapshot_bytes(bad) == bad_before);
		(void)good_before;
	}
}

void test_duplicate_budgets_are_not_loosened()
{
	auto model = make_checker(4, 4, true);

	TextureConstraints tc;
	// Two identical, deliberately tight budgets must both be enforced.
	tc.budgets.push_back({0, 0, 2, 2, kWhite, 2, 2});
	tc.budgets.push_back({0, 0, 2, 2, kWhite, 2, 2});

	auto output = create_output(model);
	ExhaustiveOracle oracle(model, output, tc);
	assert(oracle.satisfiable());
	assert(run_constrained(&output, model, tc, 5) == Result::kSuccess);
	verify_solution(output, model, tc);

	// A quota no checker corner can meet, duplicated as well.
	TextureConstraints impossible;
	impossible.budgets.push_back({0, 0, 2, 2, kWhite, 4, 4});
	impossible.budgets.push_back({0, 0, 2, 2, kWhite, 4, 4});
	auto dead = create_output(model);
	ExhaustiveOracle dead_oracle(model, dead, impossible);
	assert(!dead_oracle.satisfiable());
	assert(run_constrained(&dead, model, impossible, 5) == Result::kFail);
}

} // namespace

int main()
{
	struct Case { const char* name; void (*fn)(); };
	const Case cases[] = {
		{"periodic_checker_pins_and_budget", test_periodic_checker_pins_and_budget},
		{"periodic_wrapped_pixel_counted_once", test_periodic_wrapped_pixel_counted_once},
		{"non_periodic_corner_pin_and_budget", test_non_periodic_corner_pin_and_budget},
		{"intersecting_budgets_trigger_each_other", test_intersecting_budgets_trigger_each_other},
		{"root_pre_bans_are_respected", test_root_pre_bans_are_respected},
		{"odd_torus_budget_contradiction", test_odd_torus_budget_contradiction},
		{"invalid_arguments_leave_output_untouched", test_invalid_arguments_leave_output_untouched},
		{"failure_restores_output", test_failure_restores_output},
		{"fixed_seed_is_reproducible", test_fixed_seed_is_reproducible},
		{"n1_model", test_n1_model},
		{"backtracking_is_real", test_backtracking_is_real},
		{"alternating_solvable_and_unsolvable", test_alternating_solvable_and_unsolvable},
		{"duplicate_budgets_are_not_loosened", test_duplicate_budgets_are_not_loosened},
	};
	for (const auto& c : cases) {
		try {
			c.fn();
			std::cout << "ok: " << c.name << "\n";
		} catch (const std::exception& e) {
			std::cout << "FAIL in " << c.name << ": " << e.what() << "\n";
			throw;
		}
	}

	std::cout << "WFC_CONSTRAINED_OK\n";
	return 0;
}
