#include "wfc.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>

namespace {
  /**
   * Normalize a vector so the sum of its elements is equal to 1.0f
   */
  std::vector<double>& normalize(std::vector<double>& v) {
    double sum_weights = 0.0;
    for(double weight: v) {
      sum_weights += weight;
    }

    double inv_sum_weights = 1.0/sum_weights;
    for(double& weight: v) {
      weight *= inv_sum_weights;
    }

    return v;
  }
}


Array2D<unsigned> WFC::wave_to_output() const noexcept {
  Array2D<unsigned> output_patterns(wave.height, wave.width);
  for (unsigned i = 0; i < wave.size; i++) {
    for (unsigned k = 0; k < nb_patterns; k++) {
      if (wave.get(i, k)) {
        output_patterns.data[i] = k;
      }
    }
  }
  return output_patterns;
}

WFC::WFC(bool periodic_output, int seed,
         std::vector<double> patterns_frequencies,
         Propagator::PropagatorState propagator, unsigned wave_height,
         unsigned wave_width)
  noexcept
  : gen(seed), patterns_frequencies(normalize(patterns_frequencies)),
    wave(wave_height, wave_width, patterns_frequencies),
    nb_patterns(propagator.size()),
    propagator(wave.height, wave.width, periodic_output, propagator) {}

std::optional<Array2D<unsigned>> WFC::run() noexcept {
  while (true) {

    // Define the value of an undefined cell.
    ObserveStatus result = observe();

    // Check if the algorithm has terminated.
    if (result == failure) {
      return std::nullopt;
    } else if (result == success) {
      return wave_to_output();
    }

    // Propagate the information.
    propagator.propagate(wave);
  }
}


WFC::ObserveStatus WFC::observe() noexcept {
    // Get the cell with lowest entropy.
    int argmin = wave.get_min_entropy(gen);

    // If there is a contradiction, the algorithm has failed.
    if (argmin == -2) {
      return failure;
    }

    // If the lowest entropy is 0, then the algorithm has succeeded and
    // finished.
    if (argmin == -1) {
      wave_to_output();
      return success;
    }

    // Choose an element according to the pattern distribution
    double s = 0;
    for (unsigned k = 0; k < nb_patterns; k++) {
      s += wave.get(argmin, k) ? patterns_frequencies[k] : 0;
    }

    std::uniform_real_distribution<> dis(0, s);
    double random_value = dis(gen);
    size_t chosen_value = nb_patterns - 1;

    for (unsigned k = 0; k < nb_patterns; k++) {
      random_value -= wave.get(argmin, k) ? patterns_frequencies[k] : 0;
      if (random_value <= 0) {
        chosen_value = k;
        break;
      }
    }

    // And define the cell with the pattern.
    for (unsigned k = 0; k < nb_patterns; k++) {
      if (wave.get(argmin, k) != (k == chosen_value)) {
        propagator.add_to_propagator(argmin / wave.width, argmin % wave.width,
                                     k);
        wave.set(argmin, k, false);
      }
    }

    return to_continue;
  }

namespace {

StripResult strip_limit_result(size_t peak_states) noexcept {
  StripResult result;
  result.status = StripStatus::limit;
  result.log_partition = -std::numeric_limits<double>::infinity();
  result.marginals = std::vector<double>();
  result.sample = std::nullopt;
  result.peak_states = peak_states;
  return result;
}

StripResult strip_impossible_result(size_t nb_cells, size_t nb_patterns,
                                    size_t peak_states) {
  StripResult result;
  result.status = StripStatus::impossible;
  result.log_partition = -std::numeric_limits<double>::infinity();
  result.marginals = std::vector<double>(nb_cells * nb_patterns, 0.0);
  result.sample = std::nullopt;
  result.peak_states = peak_states;
  return result;
}

/**
 * Exact inference on a narrow strip.
 *
 * The cells are placed in row-major order. A frontier key packs, in 4-bit
 * slots, the patterns of the last W placed cells (0xF meaning "no cell"),
 * plus, when the output is periodic, the patterns of the first row so that
 * the periodic seam can be closed exactly. Histories landing on the same
 * key have their probability mass merged. The forward and backward passes
 * are rescaled layer by layer so that the accumulated weights never
 * underflow, and the marginals are obtained by combining both passes. A
 * sample is then drawn from the full joint distribution by conditional
 * backward sampling over the forward layers.
 */
StripResult strip_inference(unsigned height, unsigned width, bool periodic,
                            const std::vector<double> &weights,
                            const Propagator::PropagatorState &compat,
                            const std::vector<uint8_t> &allowed,
                            size_t state_limit, int sample_seed) {
  const unsigned W = width;
  const unsigned H = height;
  const unsigned P = (unsigned)weights.size();
  const size_t N = (size_t)W * (size_t)H;
  const uint64_t window_mask = (1ULL << (4 * W)) - 1;
  const uint64_t full_mask = periodic ? ((1ULL << (8 * W)) - 1) : window_mask;

  const auto slot = [](uint64_t key, unsigned i) -> unsigned {
    return (unsigned)((key >> (4 * i)) & 0xFULL);
  };
  const auto compatible = [&](unsigned pattern, unsigned direction,
                              unsigned other) -> bool {
    const std::vector<unsigned> &targets = compat[pattern][direction];
    return std::find(targets.begin(), targets.end(), other) != targets.end();
  };

  // Check the real adjacencies created when pattern p is placed at cell t,
  // given the frontier key describing the cells already placed.
  const auto transition_ok = [&](uint64_t key, size_t t, unsigned p) -> bool {
    if (!allowed[t * P + p]) {
      return false;
    }
    const unsigned y = (unsigned)(t / W);
    const unsigned x = (unsigned)(t % W);
    // Left neighbor inside the row.
    if (x > 0 && !compatible(slot(key, W - 1), 2, p)) {
      return false;
    }
    // Periodic seam closing the row (self-adjacency when W == 1). The
    // first cell of the row sits in window slot 1 when the last cell of
    // the row is placed.
    if (periodic && x == W - 1) {
      const unsigned first = (W == 1) ? p : slot(key, 1);
      if (!compatible(p, 2, first)) {
        return false;
      }
    }
    // Upper neighbor inside the column.
    if (y > 0 && !compatible(slot(key, 0), 3, p)) {
      return false;
    }
    // Periodic seam closing the column (self-adjacency when H == 1).
    if (periodic && y == H - 1) {
      const unsigned top = (H == 1) ? p : slot(key, W + x);
      if (!compatible(p, 3, top)) {
        return false;
      }
    }
    return true;
  };

  // Frontier key obtained after placing pattern p at cell t.
  const auto next_key = [&](uint64_t key, size_t t, unsigned p) -> uint64_t {
    uint64_t window =
        ((key & window_mask) >> 4) | ((uint64_t)p << (4 * (W - 1)));
    uint64_t rest = key & (full_mask & ~window_mask);
    if (periodic && t < W) {
      // The first row is part of the key: it is needed to close the
      // periodic seam when the last row is placed.
      rest &= ~(0xFULL << (4 * (W + t)));
      rest |= (uint64_t)p << (4 * (W + t));
    }
    return rest | window;
  };

  using Layer = std::unordered_map<uint64_t, double>;

  // Forward pass: forward[t][key] is the scaled probability mass of the
  // histories of the t first cells ending in the frontier key.
  std::vector<Layer> forward(N + 1);
  uint64_t initial_key = 0;
  for (unsigned i = 0; i < W; i++) {
    initial_key |= 0xFULL << (4 * i);
    if (periodic) {
      initial_key |= 0xFULL << (4 * (W + i));
    }
  }
  forward[0].emplace(initial_key, 1.0);
  std::vector<double> scale_log(N + 1, 0.0);
  size_t peak_states = 1;

  for (size_t t = 0; t < N; t++) {
    Layer &current = forward[t];
    Layer &next = forward[t + 1];
    for (const std::pair<const uint64_t, double> &entry : current) {
      const uint64_t key = entry.first;
      const double value = entry.second;
      if (value == 0.0) {
        continue;
      }
      for (unsigned p = 0; p < P; p++) {
        if (!transition_ok(key, t, p)) {
          continue;
        }
        const uint64_t nk = next_key(key, t, p);
        const auto it = next.find(nk);
        if (it == next.end()) {
          if (next.size() >= state_limit) {
            peak_states = std::max(peak_states, next.size());
            return strip_limit_result(peak_states);
          }
          next.emplace(nk, value * weights[p]);
        } else {
          it->second += value * weights[p];
        }
      }
    }
    if (next.empty()) {
      return strip_impossible_result(N, P, peak_states);
    }
    // Rescale the layer so that the accumulated mass cannot underflow.
    double max_value = 0.0;
    for (const std::pair<const uint64_t, double> &entry : next) {
      max_value = std::max(max_value, entry.second);
    }
    if (!(max_value > 0.0)) {
      return strip_impossible_result(N, P, peak_states);
    }
    scale_log[t + 1] = scale_log[t] + std::log(max_value);
    const double inv_max = 1.0 / max_value;
    for (auto it = next.begin(); it != next.end();) {
      it->second *= inv_max;
      it = (it->second == 0.0) ? next.erase(it) : std::next(it);
    }
    peak_states = std::max(peak_states, next.size());
  }

  double total = 0.0;
  for (const std::pair<const uint64_t, double> &entry : forward[N]) {
    total += entry.second;
  }
  if (!(total > 0.0)) {
    return strip_impossible_result(N, P, peak_states);
  }
  const double log_partition = std::log(total) + scale_log[N];

  // Backward pass: backward[key] is the scaled probability mass of the
  // completions of the frontier key. Combined with the forward pass, it
  // gives the exact marginal of every anchor.
  std::vector<double> marginals(N * P, 0.0);
  Layer backward;
  for (const std::pair<const uint64_t, double> &entry : forward[N]) {
    backward.emplace(entry.first, 1.0);
  }
  double backward_log = 0.0;
  for (size_t t = N; t-- > 0;) {
    Layer previous;
    previous.reserve(forward[t].size());
    double max_value = 0.0;
    for (const std::pair<const uint64_t, double> &entry : forward[t]) {
      const uint64_t key = entry.first;
      const double fvalue = entry.second;
      double bsum = 0.0;
      for (unsigned p = 0; p < P; p++) {
        if (!transition_ok(key, t, p)) {
          continue;
        }
        const uint64_t nk = next_key(key, t, p);
        const auto it = backward.find(nk);
        if (it == backward.end()) {
          continue;
        }
        const double bvalue = it->second;
        bsum += weights[p] * bvalue;
        if (fvalue > 0.0 && bvalue > 0.0) {
          const double log_term = std::log(fvalue) + scale_log[t] +
                                  std::log(weights[p]) + std::log(bvalue) +
                                  backward_log;
          marginals[t * P + p] += std::exp(log_term - log_partition);
        }
      }
      previous.emplace(key, bsum);
      max_value = std::max(max_value, bsum);
    }
    if (max_value > 0.0) {
      const double inv_max = 1.0 / max_value;
      for (std::pair<const uint64_t, double> &entry : previous) {
        entry.second *= inv_max;
      }
      backward_log += std::log(max_value);
    }
    backward = std::move(previous);
  }
  // Normalize the marginals of every anchor.
  for (size_t t = 0; t < N; t++) {
    double sum = 0.0;
    for (unsigned p = 0; p < P; p++) {
      sum += marginals[t * P + p];
    }
    if (sum > 0.0) {
      for (unsigned p = 0; p < P; p++) {
        marginals[t * P + p] /= sum;
      }
    }
  }

  // Conditional backward sampling of a complete assignment from the joint
  // distribution, reusing the forward layers.
  Array2D<unsigned> sample(H, W);
  {
    std::minstd_rand gen(sample_seed);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    std::vector<std::pair<uint64_t, double>> candidates(forward[N].begin(),
                                                        forward[N].end());
    std::sort(candidates.begin(), candidates.end());
    double mass = 0.0;
    for (const std::pair<uint64_t, double> &candidate : candidates) {
      mass += candidate.second;
    }
    double draw = uniform(gen) * mass;
    uint64_t key = candidates.back().first;
    for (const std::pair<uint64_t, double> &candidate : candidates) {
      draw -= candidate.second;
      if (draw <= 0.0) {
        key = candidate.first;
        break;
      }
    }
    for (size_t t = N; t-- > 0;) {
      const unsigned p = slot(key, W - 1);
      sample.data[t] = p;
      // Enumerate the predecessors: the slot shifted out of the window is
      // the only unknown of the previous frontier key.
      candidates.clear();
      const unsigned dropped_max = (t >= W) ? P : 1;
      for (unsigned qi = 0; qi < dropped_max; qi++) {
        const uint64_t q = (t >= W) ? qi : 0xFULL;
        uint64_t pre =
            ((((key & window_mask) << 4) & window_mask) | q) |
            (key & (full_mask & ~window_mask));
        if (periodic && t < W) {
          pre &= ~(0xFULL << (4 * (W + t)));
          pre |= 0xFULL << (4 * (W + t));
        }
        if (!transition_ok(pre, t, p)) {
          continue;
        }
        const auto it = forward[t].find(pre);
        if (it == forward[t].end() || it->second == 0.0) {
          continue;
        }
        candidates.emplace_back(pre, it->second * weights[p]);
      }
      if (candidates.empty()) {
        break; // Cannot happen when the partition is positive.
      }
      mass = 0.0;
      for (const std::pair<uint64_t, double> &candidate : candidates) {
        mass += candidate.second;
      }
      draw = uniform(gen) * mass;
      key = candidates.back().first;
      for (const std::pair<uint64_t, double> &candidate : candidates) {
        draw -= candidate.second;
        if (draw <= 0.0) {
          key = candidate.first;
          break;
        }
      }
    }
  }

  StripResult result;
  result.status = StripStatus::success;
  result.log_partition = log_partition;
  result.marginals = std::move(marginals);
  result.sample = std::move(sample);
  result.peak_states = peak_states;
  return result;
}

} // namespace

StripResult WFC::analyze_strip(size_t state_limit, int sample_seed) const
    noexcept {
  try {
    if (state_limit == 0 || wave.width < 1 || wave.width > 6 ||
        wave.height < 1 || wave.height > 64 || nb_patterns < 1 ||
        nb_patterns > 8) {
      return strip_limit_result(0);
    }
    // The current wave, including the removals that were not propagated
    // yet, is taken as the set of hard constraints.
    std::vector<uint8_t> allowed((size_t)wave.size * nb_patterns);
    for (unsigned i = 0; i < wave.size; i++) {
      for (size_t p = 0; p < nb_patterns; p++) {
        allowed[(size_t)i * nb_patterns + p] =
            wave.get(i, (unsigned)p) ? 1 : 0;
      }
    }
    return strip_inference(wave.height, wave.width, propagator.is_periodic(),
                           patterns_frequencies,
                           propagator.get_propagator_state(), allowed,
                           state_limit, sample_seed);
  } catch (...) {
    return strip_limit_result(0);
  }
}
