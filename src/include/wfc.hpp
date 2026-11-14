#ifndef FAST_WFC_WFC_HPP_
#define FAST_WFC_WFC_HPP_

#include <optional>
#include <random>
#include <vector>

#include "utils/array2D.hpp"
#include "propagator.hpp"
#include "wave.hpp"

/**
 * Status of the exact strip inference, see WFC::analyze_strip.
 */
enum class StripStatus {
  success,    // The inference succeeded, every field is filled.
  impossible, // No complete assignment satisfies the current constraints.
  limit       // Out of range input, or the state limit was exceeded.
};

/**
 * Result of the exact inference on a narrow strip.
 */
struct StripResult {
  StripStatus status;
  /**
   * Natural logarithm of the partition function: the sum, over every
   * complete assignment satisfying the current constraints, of the product
   * of the normalized pattern weights of every cell.
   */
  double log_partition;
  /**
   * marginals[(y * width + x) * nb_patterns + p] is the probability of
   * pattern p in cell (x, y) under the distribution conditioned on the
   * current hard constraints. Forbidden patterns have probability 0.
   */
  std::vector<double> marginals;
  /**
   * A full assignment sampled from the joint distribution, if any.
   */
  std::optional<Array2D<unsigned>> sample;
  /**
   * Maximum number of distinct frontier keys kept in a single layer.
   */
  size_t peak_states;
};

/**
 * Class containing the generic WFC algorithm.
 */
class WFC {
private:
  /**
   * The random number generator.
   */
  std::minstd_rand gen;

  /**
   * The distribution of the patterns as given in input.
   */
  const std::vector<double> patterns_frequencies;

  /**
   * The wave, indicating which patterns can be put in which cell.
   */
  Wave wave;

  /**
   * The number of distinct patterns.
   */
  const size_t nb_patterns;

  /**
   * The propagator, used to propagate the information in the wave.
   */
  Propagator propagator;

  /**
   * Transform the wave to a valid output (a 2d array of patterns that aren't in
   * contradiction). This function should be used only when all cell of the wave
   * are defined.
   */
  Array2D<unsigned> wave_to_output() const noexcept;

public:
  /**
   * Basic constructor initializing the algorithm.
   */
  WFC(bool periodic_output, int seed, std::vector<double> patterns_frequencies,
      Propagator::PropagatorState propagator, unsigned wave_height,
      unsigned wave_width)
    noexcept;

  /**
   * Run the algorithm, and return a result if it succeeded.
   */
  std::optional<Array2D<unsigned>> run() noexcept;

  /**
   * Return value of observe.
   */
  enum ObserveStatus {
    success,    // WFC has finished and has succeeded.
    failure,    // WFC has finished and failed.
    to_continue // WFC isn't finished.
  };

  /**
   * Define the value of the cell with lowest entropy.
   */
  ObserveStatus observe() noexcept;

  /**
   * Propagate the information of the wave.
   */
  void propagate() noexcept { propagator.propagate(wave); }

  /**
   * Remove pattern from cell (i,j).
   */
  void remove_wave_pattern(unsigned i, unsigned j, unsigned pattern) noexcept {
    if (wave.get(i, j, pattern)) {
      wave.set(i, j, pattern, false);
      propagator.add_to_propagator(i, j, pattern);
    }
  }

  /**
   * Exact inference on a narrow strip (wave width 1 to 6, height 1 to 64,
   * 1 to 8 patterns), under the current wave constraints taken as hard
   * constraints. Every complete assignment is weighted by the product of
   * the normalized pattern weights of its cells, and checked against the
   * real adjacencies (including the periodic seam, and self-adjacency when
   * the width or the height is 1).
   *
   * state_limit bounds the number of distinct frontier keys kept in any
   * layer of the dynamic programming. The object is not modified, so the
   * call can be retried with a larger limit.
   */
  StripResult analyze_strip(size_t state_limit, int sample_seed) const noexcept;
};

#endif // FAST_WFC_WFC_HPP_
