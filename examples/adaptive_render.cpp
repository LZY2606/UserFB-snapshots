// ==================================================================================
// Adaptive crosstalk cancellation rendering example
//
// Self-contained: generates two deterministic "clean backing" reference tracks,
// colors each with a short unknown reverb-like response to build the crosstalk
// that leaks into the vocal mic, and adds an independent synthetic vocal. The
// AdaptiveConvolver learns the leakage purely from the references and the mixed
// target, then renders the predicted crosstalk and the cleaned residual.
//
// No devices or external libraries are required; the result is written as raw
// 32-bit float PCM (mono interleaved per file).
//
// Build:
//   c++ -std=c++17 -O2 -I. examples/adaptive_render.cpp -L.task-build \
//       -lproduction -o adaptive_render
// or simply link the produced object archive directly.
// ==================================================================================

#include "AdaptiveConvolver.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>


namespace
{

const double kPi = 3.14159265358979323846;

uint32_t xorshift(uint32_t& state)
{
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}


float noiseSample(uint32_t& state)
{
  return static_cast<float>(static_cast<int32_t>(xorshift(state) & 0xFFFFu) - 32768) / 32768.0f;
}


// Deterministic synthetic "vocal": harmonically related partials with slow
// vibrato and a gentle amplitude envelope, independent of the references.
float vocalSample(size_t frame, double sampleRate)
{
  const double t = static_cast<double>(frame) / sampleRate;
  const double vibrato = 1.0 + 0.012 * std::sin(2.0 * kPi * 5.0 * t);
  const double fundamental = 180.0 * vibrato;
  double sample = 0.0;
  static const double harmonics[] = {1.0, 0.55, 0.32, 0.18, 0.1};
  for (size_t h=0; h<5; ++h)
  {
    const double freq = fundamental * static_cast<double>(h + 1);
    sample += harmonics[h] * std::sin(2.0 * kPi * freq * t);
  }
  const double envelope = 0.35 + 0.3 * std::sin(2.0 * kPi * 0.7 * t);
  return static_cast<float>(0.22 * envelope * sample);
}


// Short exponentially decaying response standing in for the unknown reverb
void buildReverb(size_t channel, size_t length, std::vector<float>& response, uint32_t& state)
{
  response.assign(length, 0.0f);
  const size_t stride = 3 + channel;
  for (size_t i=0; i<length; ++i)
  {
    if (i % stride == 0)
    {
      const float sign = (xorshift(state) & 1u) ? 1.0f : -1.0f;
      response[i] = sign * std::exp(-static_cast<float>(i) / (0.35f * static_cast<float>(length)));
    }
  }
}


float fir(const std::vector<float>& taps, const std::vector<float>& signal, size_t frame)
{
  double accumulator = 0.0;
  const size_t limit = std::min(taps.size(), frame + 1);
  for (size_t i=0; i<limit; ++i)
  {
    accumulator += static_cast<double>(taps[i]) * signal[frame - i];
  }
  return static_cast<float>(accumulator);
}


bool writeRawF32(const std::string& path, const std::vector<float>& samples)
{
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (!file)
  {
    return false;
  }
  const size_t written = std::fwrite(samples.data(), sizeof(float), samples.size(), file);
  std::fclose(file);
  return written == samples.size();
}


double energy(const std::vector<float>& samples, size_t begin, size_t end)
{
  double total = 0.0;
  for (size_t i=begin; i<end && i<samples.size(); ++i)
  {
    const double value = static_cast<double>(samples[i]);
    total += value * value;
  }
  return total;
}

} // anonymous namespace


int main(int argc, char** argv)
{
  const std::string outDir = (argc > 1) ? argv[1] : ".";

  const double sampleRate = 32000.0;
  const size_t seconds = 4;
  const size_t frames = static_cast<size_t>(sampleRate) * seconds;
  const size_t channels = 2;
  const size_t B = 32;
  const size_t L = 257;
  const float mu = 0.25f;
  const float beta = 0.2f;
  const float delta = 1e-4f;

  // Two uncorrelated clean references
  std::vector<std::vector<float> > references(channels, std::vector<float>(frames, 0.0f));
  for (size_t c=0; c<channels; ++c)
  {
    uint32_t state = 0x51ed270bu + static_cast<uint32_t>(c) * 2246822519u;
    for (size_t i=0; i<frames; ++i)
    {
      references[c][i] = 0.6f * noiseSample(state);
    }
  }

  // Unknown short reverbs coloring the leakage
  std::vector<std::vector<float> > reverbs(channels);
  for (size_t c=0; c<channels; ++c)
  {
    uint32_t state = 0x9e3779b9u + static_cast<uint32_t>(c) * 40503u;
    buildReverb(c, L, reverbs[c], state);
  }

  // Vocal mic signal = independent vocal + leaked reverb tails
  std::vector<float> vocal(frames, 0.0f);
  std::vector<float> leakage(frames, 0.0f);
  std::vector<float> mixture(frames, 0.0f);
  for (size_t i=0; i<frames; ++i)
  {
    vocal[i] = vocalSample(i, sampleRate);
    float leaked = 0.0f;
    for (size_t c=0; c<channels; ++c)
    {
      leaked += fir(reverbs[c], references[c], i);
    }
    leakage[i] = leaked;
    mixture[i] = vocal[i] + leaked;
  }

  fftconvolver::AdaptiveConvolver convolver;
  if (!convolver.init(B, L, channels, mu, beta, delta))
  {
    std::fprintf(stderr, "AdaptiveConvolver initialization failed\n");
    return 1;
  }

  std::vector<float> predicted(frames, 0.0f);
  std::vector<float> residual(frames, 0.0f);
  std::vector<const float*> plane(channels, nullptr);

  // Arbitrary fragmented call sizes, as a real streaming client would use
  const size_t chunkSizes[] = {1, 7, 31};
  size_t pos = 0;
  size_t call = 0;
  while (pos < frames)
  {
    const size_t chunk = std::min(frames - pos, chunkSizes[call++ % 3]);
    for (size_t c=0; c<channels; ++c)
    {
      plane[c] = references[c].data() + pos;
    }
    convolver.process(plane.data(),
                      mixture.data() + pos,
                      predicted.data() + pos,
                      residual.data() + pos,
                      chunk);
    pos += chunk;
  }

  // Energy report over the final second where the filter has converged
  const size_t reportBegin = frames - static_cast<size_t>(sampleRate);
  const double mixEnergy = energy(mixture, reportBegin, frames);
  const double leakEnergy = energy(leakage, reportBegin, frames);
  const double vocalEnergy = energy(vocal, reportBegin, frames);
  const double predictedEnergy = energy(predicted, reportBegin, frames);
  const double residualEnergy = energy(residual, reportBegin, frames);

  std::printf("Adaptive crosstalk cancellation render (%zu frames, B=%zu L=%zu, %zu refs)\n",
              frames, B, L, channels);
  std::printf("  mixture energy    : %.6f\n", mixEnergy);
  std::printf("  vocal energy      : %.6f (kept in the residual)\n", vocalEnergy);
  std::printf("  leakage energy    : %.6f (target of the cancellation)\n", leakEnergy);
  std::printf("  predicted energy  : %.6f\n", predictedEnergy);
  std::printf("  residual energy   : %.6f\n", residualEnergy);
  if (residualEnergy > 0.0)
  {
    const double reductionDb = 10.0 * std::log10(mixEnergy / residualEnergy);
    std::printf("  mixture->residual: %.2f dB\n", reductionDb);
  }

  const std::string mixPath = outDir + "/adaptive_mix.f32";
  const std::string predPath = outDir + "/adaptive_predicted.f32";
  const std::string resPath = outDir + "/adaptive_residual.f32";
  const bool ok = writeRawF32(mixPath, mixture)
                  && writeRawF32(predPath, predicted)
                  && writeRawF32(resPath, residual);
  if (!ok)
  {
    std::fprintf(stderr, "Failed to write output files into %s\n", outDir.c_str());
    return 2;
  }
  std::printf("Wrote raw float32 PCM:\n  %s\n  %s\n  %s\n",
              mixPath.c_str(), predPath.c_str(), resPath.c_str());
  return 0;
}
