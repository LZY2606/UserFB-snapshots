# fast-wfc

An implementation of [Wave Function Collapse](https://github.com/mxgmn/WaveFunctionCollapse) with a focus on performance.
It was called fast-wfc because at the time it introduced optimizations improving the execution time by an order of magnitude.

[Rust bindings](https://github.com/rickyhan/fastwfc-rs)

# Requirements

You need a C++-17 compatible compiler, and CMake installed.

# Install the library

```
git clone https://github.com/math-fehr/fast-wfc && cd fast-wfc/
cmake .
make install
```

will install the library `fastwfc` and `fastwfc_static` using CMake:

# Run the examples

```
cd example/
cmake .
make
./wfc_demo
```

will execute WFC on the examples defined in `example/samples.xml`, and will put the results in `example/results`.

# Exact inference on narrow strips

`WFC::analyze_strip(state_limit, sample_seed)` runs exact inference on a
narrow wave (width 1 to 6, height 1 to 64, 1 to 8 patterns) instead of the
usual greedy generation. The patterns currently forbidden in the wave are
taken as hard constraints, and every complete assignment is checked against
the real adjacencies, including the periodic seam. It returns a
`StripResult`:

* `log_partition`: the natural logarithm of the partition function (the
  sum, over every valid complete assignment, of the product of the
  normalized pattern weights of its cells),
* `marginals`: the conditional probability of every pattern at every wave
  anchor, laid out as `(y * width + x) * nb_patterns + p`,
* `sample`: a full assignment drawn from the joint distribution (not from
  independent per-cell marginals),
* `peak_states`: the maximum number of frontier states kept in one layer,
  bounded by `state_limit`.

`OverlappingWFC::analyze_strip` returns the same information in a
`StripImageResult`, with the sample rebuilt into an image. The following
program builds a sample in-program, analyzes a strip and writes the sampled
image as a small PPM:

```cpp
#include "overlapping_wfc.hpp"
#include <cstdio>

int main() {
  // A 2x2 checkerboard sample built in-program.
  Array2D<unsigned> input(2, 2);
  input.data = {0, 1, 1, 0};

  // The wave is (out - N + 1) per axis: here 7 high and 6 wide, which fits
  // the strip limits.
  OverlappingWFCOptions options{true, false, 8, 7, 1, false, 2};
  OverlappingWFC<unsigned> wfc(input, options, 7);

  StripImageResult<unsigned> result = wfc.analyze_strip(100000, 42);
  if (result.status != StripStatus::success || !result.sample.has_value()) {
    return 1;
  }
  std::fprintf(stderr, "log partition: %f\n", result.log_partition);

  // Write the sampled image as a small PPM on stdout.
  const Array2D<unsigned> &image = *result.sample;
  std::printf("P3\n%zu %zu\n255\n", image.width, image.height);
  for (size_t y = 0; y < image.height; ++y) {
    for (size_t x = 0; x < image.width; ++x) {
      const unsigned v = image.get(y, x) ? 255 : 0;
      std::printf("%u %u %u  ", v, v, v);
    }
    std::printf("\n");
  }
  return 0;
}
```

# Third-parties library

The files in `example/src/include/external/` come from:
* RapidXML [https://github.com/dwd/rapidxml](https://github.com/dwd/rapidxml)
* stb Library [https://github.com/nothings/stb](https://github.com/nothings/stb)

# Image samples

The image samples come from [https://github.com/mxgmn/WaveFunctionCollapse](https://github.com/mxgmn/WaveFunctionCollapse)

# Licence 

Copyright (c) 2018-2019 Mathieu Fehr and Nathanaëlle Courant.

MIT License, see `LICENSE` for further details.
