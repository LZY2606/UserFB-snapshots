// ==================================================================================
// Adaptive crosstalk-cancellation rendering example
//
// Renders a short mixing session entirely from deterministic, self-generated
// signals (no audio device and no external libraries):
//
//   - Two clean accompaniment reference channels (planar).
//   - Each reference leaks into the vocal track through an unknown short,
//     sparse reverberant response.
//   - An independent vocal line is added on top of the leaked crosstalk.
//
// AdaptiveConvolver learns the leakage filters from the references alone and
// renders the cleaned vocal track. A zero-frame tail flush retrieves the
// samples held back by the strict B-frame output delay, and the result is
// written as a mono 16-bit WAV file. The energies before and after the
// cancellation are reported on stdout.
//
// Build:
//   c++ -std=c++17 -O2 -I.. adaptive_render_example.cpp \
//       <path-to>/libproduction.a -o adaptive_render_example
// ==================================================================================

#include "../AdaptiveConvolver.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr size_t kSampleRate = 48000;
constexpr size_t kDurationSec = 2;
constexpr size_t kBlockSize = 64;
constexpr size_t kFilterLength = 129;
constexpr size_t kChannels = 2;

// Deterministic xorshift generator so every render is reproducible
uint32_t nextRand(uint32_t& state)
{
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

float randSigned(uint32_t& state)
{
  return static_cast<float>(static_cast<int32_t>(nextRand(state) & 0xFFFFu) - 32768) / 32768.0f;
}

// Causal time-domain convolution
float firAt(const std::vector<float>& x, const std::vector<float>& taps, long n)
{
  double acc = 0.0;
  for (size_t k = 0; k < taps.size(); ++k)
  {
    const long idx = n - static_cast<long>(k);
    if (idx >= 0)
    {
      acc += static_cast<double>(taps[k]) * static_cast<double>(x[static_cast<size_t>(idx)]);
    }
  }
  return static_cast<float>(acc);
}

// Short sparse decaying room response (the "unknown" leakage path)
std::vector<float> makeRoomResponse(uint32_t seed)
{
  std::vector<float> h(kFilterLength, 0.0f);
  uint32_t state = seed;
  const size_t reflections[] = {0, 5, 17, 41, 78, 128};
  for (size_t i = 0; i < sizeof(reflections) / sizeof(reflections[0]); ++i)
  {
    const size_t idx = reflections[i];
    const float sign = (nextRand(state) & 1u) ? 1.0f : -1.0f;
    h[idx] = sign * 0.55f * std::exp(-static_cast<float>(idx) / 45.0f);
  }
  return h;
}

// Writes a mono 16-bit PCM WAV file
bool writeWav(const std::string& path, const std::vector<float>& samples)
{
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) { return false; }

  float peak = 1e-9f;
  for (float v : samples) { peak = std::max(peak, std::fabs(v)); }
  const float gain = 0.9f / peak;

  const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * 2u);
  const uint32_t riffSize = 36u + dataBytes;
  const uint32_t sampleRate = static_cast<uint32_t>(kSampleRate);
  const uint32_t byteRate = sampleRate * 2u;

  std::fwrite("RIFF", 1, 4, file);
  std::fwrite(&riffSize, 4, 1, file);
  std::fwrite("WAVEfmt ", 1, 8, file);
  const uint32_t fmtChunkSize = 16u;
  const uint16_t audioFormat = 1u;       // PCM
  const uint16_t channels = 1u;
  const uint16_t bitsPerSample = 16u;
  const uint16_t blockAlign = channels * bitsPerSample / 8u;
  std::fwrite(&fmtChunkSize, 4, 1, file);
  std::fwrite(&audioFormat, 2, 1, file);
  std::fwrite(&channels, 2, 1, file);
  std::fwrite(&sampleRate, 4, 1, file);
  std::fwrite(&byteRate, 4, 1, file);
  std::fwrite(&blockAlign, 2, 1, file);
  std::fwrite(&bitsPerSample, 2, 1, file);
  std::fwrite("data", 1, 4, file);
  std::fwrite(&dataBytes, 4, 1, file);

  for (float v : samples)
  {
    float scaled = gain * v;
    scaled = std::max(-1.0f, std::min(1.0f, scaled));
    const int16_t s16 = static_cast<int16_t>(std::lround(scaled * 32767.0f));
    std::fwrite(&s16, 2, 1, file);
  }

  std::fclose(file);
  return true;
}

} // namespace


int main(int argc, char** argv)
{
  const std::string outputPath = (argc > 1) ? argv[1] : "adaptive_render_output.wav";

  const size_t frames = kSampleRate * kDurationSec;

  // Two independent, broadband accompaniment references (tonal material over a
  // strong noise floor keeps them mutually uncorrelated, which is what lets the
  // multi-channel NLMS identify the leakage paths quickly)
  std::vector<std::vector<float> > refs(kChannels, std::vector<float>(frames));
  for (size_t c = 0; c < kChannels; ++c)
  {
    uint32_t state = 0xA0000000u + 0x1000u * static_cast<uint32_t>(c + 1);
    const double freq = (c == 0) ? 220.0 : 329.6;
    for (size_t n = 0; n < frames; ++n)
    {
      const double t = static_cast<double>(n) / static_cast<double>(kSampleRate);
      refs[c][n] = 0.25f * static_cast<float>(std::sin(2.0 * kPi * freq * t))
                 + 0.55f * randSigned(state);
    }
  }

  // Independent vocal line: vibrato tone plus breath noise, own random stream
  std::vector<float> vocal(frames);
  {
    uint32_t state = 0x5EED1234u;
    double phase = 0.0;
    for (size_t n = 0; n < frames; ++n)
    {
      const double t = static_cast<double>(n) / static_cast<double>(kSampleRate);
      const double f = 440.0 + 55.0 * std::sin(2.0 * kPi * 5.0 * t);
      phase += 2.0 * kPi * f / static_cast<double>(kSampleRate);
      vocal[n] = 0.5f * static_cast<float>(std::sin(phase))
               + 0.05f * randSigned(state);
    }
  }

  // Leakage through the (to the convolver unknown) short room responses
  const std::vector<float> h0 = makeRoomResponse(0x1111u);
  const std::vector<float> h1 = makeRoomResponse(0x2222u);
  std::vector<float> leakage(frames);
  std::vector<float> desired(frames);
  for (size_t n = 0; n < frames; ++n)
  {
    leakage[n] = firAt(refs[0], h0, static_cast<long>(n))
               + firAt(refs[1], h1, static_cast<long>(n));
    desired[n] = vocal[n] + leakage[n];
  }

  // Adaptive cancellation, fed in irregular fragments as a real caller would
  fftconvolver::AdaptiveConvolver convolver;
  if (!convolver.init(kBlockSize, kFilterLength, kChannels, 0.25f, 0.2f, 1e-4f))
  {
    std::fprintf(stderr, "AdaptiveConvolver::init failed\n");
    return 1;
  }

  const size_t totalOut = frames + kBlockSize; // B extra frames flush the delay
  std::vector<float> predicted(totalOut, 0.0f);
  std::vector<float> residual(totalOut, 0.0f);
  const size_t chunkSizes[] = {1, 7, 31};
  size_t pos = 0;
  size_t call = 0;
  std::vector<const float*> refPtrs(kChannels);
  std::vector<float> zeroRef(kBlockSize, 0.0f);

  while (pos < totalOut)
  {
    const size_t chunk = std::min(totalOut - pos, chunkSizes[call++ % 3]);
    const bool inSignal = (pos < frames);
    for (size_t c = 0; c < kChannels; ++c)
    {
      refPtrs[c] = inSignal ? refs[c].data() + pos : zeroRef.data();
    }
    const float* desiredPtr = inSignal ? desired.data() + pos : zeroRef.data();
    convolver.process(refPtrs.data(), desiredPtr,
                      predicted.data() + pos, residual.data() + pos, chunk);
    pos += chunk;
  }

  // Energies, aligning the B-frame-delayed output back to the input timeline
  double energyVocal = 0.0;
  double energyLeakBefore = 0.0;
  double energyMixBefore = 0.0;
  double energyLeakAfter = 0.0;
  double energyResidualAfter = 0.0;
  for (size_t n = 0; n < frames; ++n)
  {
    const float res = residual[n + kBlockSize];
    energyVocal += static_cast<double>(vocal[n]) * vocal[n];
    energyLeakBefore += static_cast<double>(leakage[n]) * leakage[n];
    energyMixBefore += static_cast<double>(desired[n]) * desired[n];
    energyResidualAfter += static_cast<double>(res) * res;
    const float remainingCrosstalk = res - vocal[n];
    energyLeakAfter += static_cast<double>(remainingCrosstalk) * remainingCrosstalk;
  }

  const double crosstalkDb = 10.0 * std::log10(energyLeakBefore / energyLeakAfter);
  const double mixToResidualDb = 10.0 * std::log10(energyMixBefore / energyResidualAfter);

  std::printf("Adaptive crosstalk cancellation render\n");
  std::printf("  frames              : %zu (%.2f s @ %zu Hz)\n",
              frames, static_cast<double>(frames) / kSampleRate, kSampleRate);
  std::printf("  references          : %zu channels, filter length L=%zu, block B=%zu\n",
              kChannels, kFilterLength, kBlockSize);
  std::printf("  energy before (mix) : %.6f\n", energyMixBefore);
  std::printf("  energy after  (res) : %.6f  (%.2f dB lower)\n",
              energyResidualAfter, mixToResidualDb);
  std::printf("  crosstalk energy    : %.6f -> %.6f  (%.2f dB reduction)\n",
              energyLeakBefore, energyLeakAfter, crosstalkDb);
  std::printf("  vocal energy        : %.6f (independent reference, preserved in residual)\n",
              energyVocal);

  // Render the cleaned vocal track (drop the B startup zeros and the flush tail)
  std::vector<float> cleaned(frames);
  for (size_t n = 0; n < frames; ++n)
  {
    cleaned[n] = residual[n + kBlockSize];
  }
  if (!writeWav(outputPath, cleaned))
  {
    std::fprintf(stderr, "Failed to write %s\n", outputPath.c_str());
    return 2;
  }
  std::printf("  rendered file       : %s (%zu frames, mono S16)\n",
              outputPath.c_str(), frames);

  return 0;
}
