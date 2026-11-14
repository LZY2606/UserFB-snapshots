#include "task_support/check.h"

#include "overlapping_wfc.hpp"
#include "wfc.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

namespace {

using Rules = Propagator::PropagatorState;

constexpr double kNegInf = -std::numeric_limits<double>::infinity();

Rules checker_rules() {
  Rules rules(2);
  for (unsigned p = 0; p < 2; ++p) {
    for (unsigned d = 0; d < 4; ++d) {
      rules[p][d].push_back(1 - p);
    }
  }
  return rules;
}

Rules all_compatible_rules(unsigned P) {
  Rules rules(P);
  for (unsigned a = 0; a < P; ++a) {
    for (unsigned d = 0; d < 4; ++d) {
      for (unsigned b = 0; b < P; ++b) {
        rules[a][d].push_back(b);
      }
    }
  }
  return rules;
}

// b is compatible with a in every direction iff (a + b) is even.
Rules parity_rules(unsigned P) {
  Rules rules(P);
  for (unsigned a = 0; a < P; ++a) {
    for (unsigned d = 0; d < 4; ++d) {
      for (unsigned b = 0; b < P; ++b) {
        if ((a + b) % 2 == 0) {
          rules[a][d].push_back(b);
        }
      }
    }
  }
  return rules;
}

std::vector<double> normalized(std::vector<double> weights) {
  double sum = 0.0;
  for (double w : weights) {
    sum += w;
  }
  for (double &w : weights) {
    w /= sum;
  }
  return weights;
}

struct Ban {
  unsigned y, x, p;
};

struct Model {
  unsigned height = 1;
  unsigned width = 1;
  unsigned P = 1;
  bool periodic = false;
  std::vector<double> weights; // normalized
  Rules rules;
  std::vector<Ban> bans;

  std::vector<uint8_t> allowed() const {
    std::vector<uint8_t> a((size_t)height * width * P, 1);
    for (const Ban &b : bans) {
      a[((size_t)b.y * width + b.x) * P + b.p] = 0;
    }
    return a;
  }
};

WFC make_wfc(const Model &m, int seed) {
  WFC wfc(m.periodic, seed, m.weights, m.rules, m.height, m.width);
  for (const Ban &b : m.bans) {
    // No propagate(): the removal alone is already a hard constraint.
    wfc.remove_wave_pattern(b.y, b.x, b.p);
  }
  return wfc;
}

bool compatible(const Rules &rules, unsigned from, unsigned dir, unsigned to) {
  const std::vector<unsigned> &v = rules[from][dir];
  return std::find(v.begin(), v.end(), to) != v.end();
}

// Check every real adjacency of a complete assignment, including the
// periodic seams and the self-adjacency of width-1 or height-1 strips.
bool assignment_valid(const Model &m, const std::vector<unsigned> &a) {
  const unsigned W = m.width, H = m.height;
  for (unsigned y = 0; y < H; ++y) {
    for (unsigned x = 0; x < W; ++x) {
      const unsigned i = y * W + x;
      if (x + 1 < W && !compatible(m.rules, a[i], 2, a[i + 1])) {
        return false;
      }
      if (m.periodic && x == W - 1 && !compatible(m.rules, a[i], 2, a[y * W])) {
        return false;
      }
      if (y + 1 < H && !compatible(m.rules, a[i], 3, a[i + W])) {
        return false;
      }
      if (m.periodic && y == H - 1 && !compatible(m.rules, a[i], 3, a[x])) {
        return false;
      }
    }
  }
  return true;
}

struct BruteResult {
  bool possible;
  double log_partition;
  std::vector<double> marginals;
};

// Independent enumeration of every complete assignment.
BruteResult brute_force(const Model &m) {
  const size_t N = (size_t)m.height * m.width;
  const size_t P = m.P;
  const std::vector<uint8_t> allowed = m.allowed();
  std::vector<double> marginals(N * P, 0.0);
  std::vector<unsigned> assignment(N, 0);
  double partition = 0.0;
  std::function<void(size_t, double)> dfs = [&](size_t t, double w) {
    if (t == N) {
      if (!assignment_valid(m, assignment)) {
        return;
      }
      partition += w;
      for (size_t i = 0; i < N; ++i) {
        marginals[i * P + assignment[i]] += w;
      }
      return;
    }
    for (size_t p = 0; p < P; ++p) {
      if (!allowed[t * P + p]) {
        continue;
      }
      assignment[t] = (unsigned)p;
      dfs(t + 1, w * m.weights[p]);
    }
  };
  dfs(0, 1.0);
  BruteResult r;
  r.possible = partition > 0.0;
  r.log_partition = r.possible ? std::log(partition) : kNegInf;
  if (r.possible) {
    for (double &v : marginals) {
      v /= partition;
    }
  }
  r.marginals = std::move(marginals);
  return r;
}

int check_against_brute(const Model &m, const StripResult &r) {
  const BruteResult b = brute_force(m);
  const size_t N = (size_t)m.height * m.width;
  if (!b.possible) {
    TASK_CHECK(r.status == StripStatus::impossible);
    TASK_CHECK(r.log_partition == kNegInf);
    TASK_CHECK(!r.sample.has_value());
    TASK_CHECK(r.marginals.size() == N * m.P);
    for (double v : r.marginals) {
      TASK_CHECK(v == 0.0);
    }
    return 0;
  }
  TASK_CHECK(r.status == StripStatus::success);
  TASK_CHECK(std::isfinite(r.log_partition));
  TASK_CHECK(std::fabs(r.log_partition - b.log_partition) <=
             1e-9 * (1.0 + std::fabs(b.log_partition)));
  TASK_CHECK(r.marginals.size() == N * m.P);
  for (size_t i = 0; i < N * m.P; ++i) {
    TASK_CHECK(std::fabs(r.marginals[i] - b.marginals[i]) <= 1e-9);
  }
  return 0;
}

int check_sample_legal(const Model &m, const StripResult &r) {
  TASK_CHECK(r.status == StripStatus::success);
  TASK_CHECK(r.sample.has_value());
  const Array2D<unsigned> &s = *r.sample;
  TASK_CHECK(s.height == m.height && s.width == m.width);
  const std::vector<uint8_t> allowed = m.allowed();
  std::vector<unsigned> a(s.data.begin(), s.data.end());
  for (size_t i = 0; i < a.size(); ++i) {
    TASK_CHECK(a[i] < m.P);
    TASK_CHECK(allowed[i * m.P + a[i]]);
  }
  TASK_CHECK(assignment_valid(m, a));
  return 0;
}

Model checker_model(unsigned height, unsigned width, bool periodic,
                    std::vector<double> weights) {
  Model m;
  m.height = height;
  m.width = width;
  m.P = 2;
  m.periodic = periodic;
  m.weights = normalized(std::move(weights));
  m.rules = checker_rules();
  return m;
}

// ---------------------------------------------------------------------------
// probability group
// ---------------------------------------------------------------------------

int case_checker_open_bruteforce() {
  Model m = checker_model(3, 4, false, {1.0, 3.0});
  m.bans = {{0, 0, 1}};
  WFC wfc = make_wfc(m, 11);
  StripResult r = wfc.analyze_strip(100000, 5);
  if (check_against_brute(m, r)) {
    return 1;
  }
  if (check_sample_legal(m, r)) {
    return 1;
  }
  TASK_CHECK(r.peak_states >= 1 && r.peak_states <= 100000);
  for (size_t t = 0; t < 12; ++t) {
    const double s = r.marginals[t * 2] + r.marginals[t * 2 + 1];
    TASK_CHECK(std::fabs(s - 1.0) <= 1e-9);
  }
  TASK_CHECK(r.marginals[1] == 0.0); // banned pattern at (0,0)
  return 0;
}

int case_checker_torus_bruteforce() {
  Model m = checker_model(4, 4, true, {1.0, 3.0});
  WFC wfc = make_wfc(m, 11);
  StripResult r = wfc.analyze_strip(100000, 5);
  if (check_against_brute(m, r)) {
    return 1;
  }
  return check_sample_legal(m, r);
}

int case_three_pattern_parity_torus() {
  Model m;
  m.height = 2;
  m.width = 3;
  m.P = 3;
  m.periodic = true;
  m.weights = normalized({2.0, 1.0, 3.0});
  m.rules = parity_rules(3);
  WFC wfc = make_wfc(m, 11);
  StripResult r = wfc.analyze_strip(100000, 5);
  if (check_against_brute(m, r)) {
    return 1;
  }
  return check_sample_legal(m, r);
}

int case_single_cell_self_loop() {
  Model m;
  m.height = 1;
  m.width = 1;
  m.P = 3;
  m.periodic = true;
  m.weights = normalized({5.0, 1.0, 2.0});
  // Pattern 2 has no vertical self-loop, so it cannot tile a 1x1 torus.
  Rules rules(3);
  for (unsigned d = 0; d < 4; ++d) {
    rules[0][d] = {0};
    rules[1][d] = {1};
  }
  rules[2][1] = {2};
  rules[2][2] = {2};
  m.rules = rules;
  WFC wfc = make_wfc(m, 11);
  StripResult r = wfc.analyze_strip(100000, 5);
  if (check_against_brute(m, r)) {
    return 1;
  }
  if (check_sample_legal(m, r)) {
    return 1;
  }
  // Only patterns 0 and 1 can appear, proportionally to their weights.
  TASK_CHECK(std::fabs(r.marginals[0] - 5.0 / 6.0) <= 1e-9);
  TASK_CHECK(std::fabs(r.marginals[1] - 1.0 / 6.0) <= 1e-9);
  TASK_CHECK(r.marginals[2] == 0.0);
  TASK_CHECK(std::fabs(r.log_partition - std::log(0.75)) <= 1e-9);
  return 0;
}

int case_self_loop_column() {
  // Width-1 periodic column: every row self-adjoins horizontally, and the
  // vertical seam closes the column.
  Rules rules(2);
  rules[0][0] = {1};
  rules[0][3] = {1};
  rules[1][0] = {0};
  rules[1][3] = {0};
  rules[0][1] = {0};
  rules[0][2] = {0};
  rules[1][1] = {1};
  rules[1][2] = {1};
  Model even;
  even.height = 4;
  even.width = 1;
  even.P = 2;
  even.periodic = true;
  even.weights = normalized({1.0, 1.0});
  even.rules = rules;
  WFC wfc_even = make_wfc(even, 11);
  StripResult r_even = wfc_even.analyze_strip(100000, 5);
  if (check_against_brute(even, r_even)) {
    return 1;
  }
  if (check_sample_legal(even, r_even)) {
    return 1;
  }
  // Odd height: the alternation cannot close the seam.
  Model odd = even;
  odd.height = 5;
  WFC wfc_odd = make_wfc(odd, 11);
  StripResult r_odd = wfc_odd.analyze_strip(100000, 5);
  return check_against_brute(odd, r_odd);
}

int case_unpropagated_removal() {
  Model m = checker_model(3, 3, false, {1.0, 1.0});
  m.bans = {{1, 1, 0}, {0, 1, 1}};
  WFC wfc = make_wfc(m, 11); // removals are not propagated
  StripResult r = wfc.analyze_strip(100000, 5);
  if (check_against_brute(m, r)) {
    return 1;
  }
  if (check_sample_legal(m, r)) {
    return 1;
  }
  TASK_CHECK(r.marginals[(1 * 3 + 1) * 2 + 0] == 0.0);
  TASK_CHECK(r.marginals[(0 * 3 + 1) * 2 + 1] == 0.0);
  return 0;
}

int case_degenerate_rows() {
  Model row = checker_model(1, 6, false, {1.0, 1.0});
  WFC wfc_row = make_wfc(row, 11);
  StripResult r_row = wfc_row.analyze_strip(100000, 5);
  if (check_against_brute(row, r_row)) {
    return 1;
  }
  if (check_sample_legal(row, r_row)) {
    return 1;
  }
  Model column = checker_model(6, 1, false, {2.0, 1.0});
  WFC wfc_column = make_wfc(column, 11);
  StripResult r_column = wfc_column.analyze_strip(100000, 5);
  if (check_against_brute(column, r_column)) {
    return 1;
  }
  return check_sample_legal(column, r_column);
}

int case_extreme_weights_long_strip() {
  Model m;
  m.height = 64;
  m.width = 1;
  m.P = 2;
  m.periodic = false;
  m.weights = normalized({1.0, 1e-200});
  m.rules = all_compatible_rules(2);
  m.bans = {{0, 0, 0}, {1, 0, 0}};
  WFC wfc = make_wfc(m, 11);
  StripResult r = wfc.analyze_strip(16, 5);
  TASK_CHECK(r.status == StripStatus::success);
  // The partition is about 1e-400: it must not underflow to zero.
  TASK_CHECK(std::isfinite(r.log_partition));
  const double expected = 2.0 * std::log(1e-200);
  TASK_CHECK(std::fabs(r.log_partition - expected) <= 1e-6);
  TASK_CHECK(r.marginals.size() == 64 * 2);
  TASK_CHECK(r.marginals[0] == 0.0);
  TASK_CHECK(std::fabs(r.marginals[1] - 1.0) <= 1e-12);
  TASK_CHECK(std::fabs(r.marginals[40 * 2 + 0] - 1.0) <= 1e-12);
  TASK_CHECK(r.peak_states <= 16);
  TASK_CHECK(r.sample.has_value());
  TASK_CHECK(r.sample->data[0] == 1 && r.sample->data[1] == 1);
  return 0;
}

int case_limit_and_retry() {
  Model m = checker_model(3, 4, false, {1.0, 3.0});
  WFC wfc = make_wfc(m, 3);
  StripResult r0 = wfc.analyze_strip(0, 5);
  TASK_CHECK(r0.status == StripStatus::limit);
  TASK_CHECK(r0.peak_states == 0);
  StripResult r1 = wfc.analyze_strip(1, 5);
  TASK_CHECK(r1.status == StripStatus::limit);
  TASK_CHECK(r1.peak_states <= 1);
  TASK_CHECK(r1.log_partition == kNegInf);
  TASK_CHECK(r1.marginals.empty());
  TASK_CHECK(!r1.sample.has_value());
  StripResult full = wfc.analyze_strip(100000, 5);
  TASK_CHECK(full.status == StripStatus::success);
  const size_t peak = full.peak_states;
  TASK_CHECK(peak >= 2);
  // A limit equal to the observed peak is enough.
  StripResult exact = wfc.analyze_strip(peak, 5);
  TASK_CHECK(exact.status == StripStatus::success);
  TASK_CHECK(exact.log_partition == full.log_partition);
  TASK_CHECK(exact.marginals == full.marginals);
  TASK_CHECK(exact.sample.has_value() && full.sample.has_value() &&
             exact.sample->data == full.sample->data);
  // One less is not.
  StripResult below = wfc.analyze_strip(peak - 1, 5);
  TASK_CHECK(below.status == StripStatus::limit);
  TASK_CHECK(below.log_partition == kNegInf);
  TASK_CHECK(below.marginals.empty());
  TASK_CHECK(!below.sample.has_value());
  // After the failures, the same object gives the same results as a fresh
  // one that never failed.
  WFC fresh = make_wfc(m, 3);
  StripResult reference = fresh.analyze_strip(100000, 5);
  TASK_CHECK(reference.status == StripStatus::success);
  TASK_CHECK(reference.log_partition == full.log_partition);
  TASK_CHECK(reference.marginals == full.marginals);
  TASK_CHECK(reference.sample.has_value() &&
             reference.sample->data == full.sample->data);
  return 0;
}

int case_out_of_range() {
  Rules checker = checker_rules();
  WFC wide(false, 1, {1.0, 1.0}, checker, 2, 7);
  TASK_CHECK(wide.analyze_strip(100000, 0).status == StripStatus::limit);
  WFC tall(false, 1, {1.0, 1.0}, checker, 65, 1);
  TASK_CHECK(tall.analyze_strip(100000, 0).status == StripStatus::limit);
  WFC many(false, 1, std::vector<double>(9, 1.0), all_compatible_rules(9), 2,
           2);
  StripResult r = many.analyze_strip(100000, 0);
  TASK_CHECK(r.status == StripStatus::limit);
  TASK_CHECK(r.log_partition == kNegInf);
  TASK_CHECK(r.marginals.empty());
  TASK_CHECK(!r.sample.has_value());
  TASK_CHECK(r.peak_states == 0);
  WFC ok(false, 1, {1.0, 1.0}, checker, 2, 2);
  TASK_CHECK(ok.analyze_strip(0, 0).status == StripStatus::limit);
  return 0;
}

int case_impossible_odd_torus() {
  Model m = checker_model(3, 3, true, {1.0, 3.0});
  WFC wfc = make_wfc(m, 11);
  StripResult r = wfc.analyze_strip(100000, 5);
  return check_against_brute(m, r);
}

// ---------------------------------------------------------------------------
// sampling group
// ---------------------------------------------------------------------------

int case_sample_repeatable_and_legal() {
  Model m = checker_model(3, 4, false, {1.0, 3.0});
  WFC wfc = make_wfc(m, 7);
  StripResult a = wfc.analyze_strip(1000, 42);
  StripResult b = wfc.analyze_strip(1000, 42);
  TASK_CHECK(a.sample.has_value() && b.sample.has_value());
  TASK_CHECK(a.sample->data == b.sample->data);
  WFC fresh = make_wfc(m, 7);
  StripResult c = fresh.analyze_strip(1000, 42);
  TASK_CHECK(c.sample.has_value() && c.sample->data == a.sample->data);
  if (check_sample_legal(m, a)) {
    return 1;
  }
  // Different seeds eventually give different samples.
  bool differ = false;
  for (int s = 0; s < 30; ++s) {
    StripResult r = wfc.analyze_strip(1000, s);
    TASK_CHECK(r.sample.has_value());
    if (r.sample->data != a.sample->data) {
      differ = true;
    }
  }
  TASK_CHECK(differ);
  // analyze_strip leaves the object and its generator untouched.
  std::optional<Array2D<unsigned>> out = wfc.run();
  std::optional<Array2D<unsigned>> out_fresh = fresh.run();
  TASK_CHECK(out.has_value() && out_fresh.has_value());
  TASK_CHECK(out->data == out_fresh->data);
  return 0;
}

int case_sample_forced_by_bans() {
  // Banning pattern 1 at (0,0) forces a single checkerboard.
  Model m = checker_model(3, 3, false, {1.0, 3.0});
  m.bans = {{0, 0, 1}};
  WFC wfc = make_wfc(m, 17);
  std::vector<unsigned> expected(9);
  for (unsigned y = 0; y < 3; ++y) {
    for (unsigned x = 0; x < 3; ++x) {
      expected[y * 3 + x] = (x + y) % 2;
    }
  }
  for (int s = 0; s < 20; ++s) {
    StripResult r = wfc.analyze_strip(1000, s);
    TASK_CHECK(r.status == StripStatus::success);
    TASK_CHECK(r.sample.has_value());
    TASK_CHECK(r.sample->data == expected);
  }
  StripResult r = wfc.analyze_strip(1000, 0);
  for (size_t t = 0; t < 9; ++t) {
    TASK_CHECK(r.marginals[t * 2 + expected[t]] == 1.0);
    TASK_CHECK(r.marginals[t * 2 + 1 - expected[t]] == 0.0);
  }
  return 0;
}

int case_sample_joint_frequencies() {
  // 3x3 open checkerboard with weights (1/4, 3/4): exactly two valid
  // assignments, with probability 1/4 and 3/4.
  Model m = checker_model(3, 3, false, {1.0, 3.0});
  WFC wfc = make_wfc(m, 17);
  const int K = 4000;
  int count_origin_zero = 0;
  int count_right_zero = 0;
  for (int s = 0; s < K; ++s) {
    StripResult r = wfc.analyze_strip(1000, s);
    TASK_CHECK(r.status == StripStatus::success);
    TASK_CHECK(r.sample.has_value());
    const std::vector<unsigned> &a = r.sample->data;
    // The sample is drawn from the joint distribution: neighbouring cells
    // stay anti-correlated, which independent per-cell sampling would break.
    TASK_CHECK(a[0] != a[1]);
    if (a[0] == 0) {
      ++count_origin_zero;
    }
    if (a[1] == 0) {
      ++count_right_zero;
    }
  }
  const double freq_origin = (double)count_origin_zero / K;
  const double freq_right = (double)count_right_zero / K;
  TASK_CHECK(std::fabs(freq_origin - 0.25) <= 0.04);
  TASK_CHECK(std::fabs(freq_right - 0.75) <= 0.04);
  return 0;
}

// ---------------------------------------------------------------------------
// image group
// ---------------------------------------------------------------------------

Array2D<unsigned> checker_input() {
  Array2D<unsigned> input(2, 2);
  input.data = {0, 1, 1, 0};
  return input;
}

int check_image_checkerboard(const Array2D<unsigned> &img, bool periodic) {
  for (size_t y = 0; y < img.height; ++y) {
    for (size_t x = 0; x < img.width; ++x) {
      TASK_CHECK(img.get(y, x) <= 1);
      if (x + 1 < img.width) {
        TASK_CHECK(img.get(y, x) != img.get(y, x + 1));
      }
      if (y + 1 < img.height) {
        TASK_CHECK(img.get(y, x) != img.get(y + 1, x));
      }
      if (periodic) {
        TASK_CHECK(img.get(y, x) != img.get(y, (x + 1) % img.width));
        TASK_CHECK(img.get(y, x) != img.get((y + 1) % img.height, x));
      }
    }
  }
  return 0;
}

int case_image_open_nonsquare_n2() {
  Array2D<unsigned> input = checker_input();
  OverlappingWFCOptions options{true, false, 5, 7, 1, false, 2};
  OverlappingWFC<unsigned> owfc(input, options, 99);
  StripImageResult<unsigned> r = owfc.analyze_strip(100000, 1234);
  TASK_CHECK(r.status == StripStatus::success);
  TASK_CHECK(r.peak_states >= 1 && r.peak_states <= 100000);
  // The 4x6 wave has exactly two valid assignments of weight 2^-24 each.
  TASK_CHECK(std::fabs(r.log_partition - (-23.0 * std::log(2.0))) <= 1e-9);
  // The marginals refer to the wave anchors, not to the output pixels.
  TASK_CHECK(r.marginals.size() == 4 * 6 * 2);
  for (double v : r.marginals) {
    TASK_CHECK(std::fabs(v - 0.5) <= 1e-9);
  }
  TASK_CHECK(r.sample.has_value());
  const Array2D<unsigned> &img = *r.sample;
  TASK_CHECK(img.height == 5 && img.width == 7);
  if (check_image_checkerboard(img, false)) {
    return 1;
  }
  StripImageResult<unsigned> r2 = owfc.analyze_strip(100000, 1234);
  TASK_CHECK(r2.sample.has_value() && r2.sample->data == img.data);
  return 0;
}

int case_image_periodic_n2() {
  Array2D<unsigned> input = checker_input();
  OverlappingWFCOptions options{true, true, 4, 6, 1, false, 2};
  OverlappingWFC<unsigned> owfc(input, options, 99);
  StripImageResult<unsigned> r = owfc.analyze_strip(100000, 77);
  TASK_CHECK(r.status == StripStatus::success);
  TASK_CHECK(std::fabs(r.log_partition - (-23.0 * std::log(2.0))) <= 1e-9);
  TASK_CHECK(r.marginals.size() == 4 * 6 * 2);
  for (double v : r.marginals) {
    TASK_CHECK(std::fabs(v - 0.5) <= 1e-9);
  }
  TASK_CHECK(r.sample.has_value());
  const Array2D<unsigned> &img = *r.sample;
  TASK_CHECK(img.height == 4 && img.width == 6);
  // The periodic seam is checked as well.
  return check_image_checkerboard(img, true);
}

int case_image_ground() {
  // Uniform columns: the ground pattern tiles the ground row with itself.
  Array2D<unsigned> input(3, 2);
  input.data = {0, 0, 0, 0, 2, 2};
  OverlappingWFCOptions options{true, false, 4, 6, 1, true, 2};
  OverlappingWFC<unsigned> owfc(input, options, 99);
  StripImageResult<unsigned> r = owfc.analyze_strip(100000, 7);
  TASK_CHECK(r.status == StripStatus::success);
  // The ground forces a single assignment of the 3x5 wave.
  TASK_CHECK(std::fabs(r.log_partition - 15.0 * std::log(1.0 / 3.0)) <= 1e-9);
  TASK_CHECK(r.marginals.size() == 3 * 5 * 3);
  for (unsigned y = 0; y < 3; ++y) {
    for (unsigned x = 0; x < 5; ++x) {
      for (unsigned p = 0; p < 3; ++p) {
        const double expected = (p == y) ? 1.0 : 0.0;
        TASK_CHECK(
            std::fabs(r.marginals[((size_t)y * 5 + x) * 3 + p] - expected) <=
            1e-9);
      }
    }
  }
  TASK_CHECK(r.sample.has_value());
  const Array2D<unsigned> &img = *r.sample;
  TASK_CHECK(img.height == 4 && img.width == 6);
  for (size_t x = 0; x < 6; ++x) {
    TASK_CHECK(img.get(0, x) == 0);
    TASK_CHECK(img.get(1, x) == 0);
    TASK_CHECK(img.get(2, x) == 2);
    TASK_CHECK(img.get(3, x) == 0);
  }
  return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"probability", "checker_open_bruteforce", &case_checker_open_bruteforce},
    {"probability", "checker_torus_bruteforce", &case_checker_torus_bruteforce},
    {"probability", "three_pattern_parity_torus",
     &case_three_pattern_parity_torus},
    {"probability", "single_cell_self_loop", &case_single_cell_self_loop},
    {"probability", "self_loop_column", &case_self_loop_column},
    {"probability", "unpropagated_removal", &case_unpropagated_removal},
    {"probability", "degenerate_rows", &case_degenerate_rows},
    {"probability", "extreme_weights_long_strip",
     &case_extreme_weights_long_strip},
    {"probability", "limit_and_retry", &case_limit_and_retry},
    {"probability", "out_of_range", &case_out_of_range},
    {"probability", "impossible_odd_torus", &case_impossible_odd_torus},
    {"sampling", "sample_repeatable_and_legal",
     &case_sample_repeatable_and_legal},
    {"sampling", "sample_forced_by_bans", &case_sample_forced_by_bans},
    {"sampling", "sample_joint_frequencies", &case_sample_joint_frequencies},
    {"image", "image_open_nonsquare_n2", &case_image_open_nonsquare_n2},
    {"image", "image_periodic_n2", &case_image_periodic_n2},
    {"image", "image_ground", &case_image_ground},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
