// Device-free PeriodicPlan example (C++17, standard library only).
//
// Builds a short looping waveform, constructs a steady-stated 4x IIR
// upsampling plan and prints the first output period twice, demonstrating that
// the very first sample already belongs to the closed periodic response (no
// zero-state ramp-up). No audio hardware is used.

#include "pandaresampler.hh"

#include <cmath>
#include <cstdio>
#include <vector>

using PandaResampler::PeriodicPlan;
using PandaResampler::Resampler2;

int
main()
{
  constexpr unsigned N = 16;
  std::vector<float> loop (N);
  for (unsigned i = 0; i < N; i++) {
    const double ph = 6.283185307179586 * i / N;
    loop[i] = float (0.6 * std::sin (ph) + 0.2 * std::sin (3.0 * ph));
  }

  const float *src[1] = { loop.data() };
  PeriodicPlan plan (Resampler2::UP, 4, Resampler2::PREC_96DB, 1, src, N);

  const uint64_t P = plan.period();       // N * ratio = 64 output frames
  std::printf ("source frames N=%u  output period P=%llu\n",
               N, (unsigned long long) P);

  std::vector<float> out (static_cast<size_t> (P));
  float *dst[1] = { out.data() };

  for (int lap = 0; lap < 2; lap++) {
    plan.render (dst, size_t (P));
    std::printf ("lap %d (first 8 samples):", lap);
    for (unsigned i = 0; i < 8; i++) std::printf (" %+.5f", out[i]);
    std::printf ("\n");
  }

  // Seeking far ahead (well beyond 2^53) lands on the same in-period phase as
  // a short seek, because the cursor is reduced with integer arithmetic.
  plan.seek ((uint64_t (1) << 54) + 3);
  std::vector<float> far (4), near (4);
  float *far_p[1] = { far.data() };
  plan.render (far_p, 4);

  plan.reset();
  plan.seek (3);
  float *near_p[1] = { near.data() };
  plan.render (near_p, 4);

  float err = 0.0f;
  for (unsigned i = 0; i < 4; i++) err = std::max (err, std::fabs (far[i] - near[i]));
  std::printf ("seek(2^54+3) vs seek(3) max diff = %.3g\n", err);
  return 0;
}
