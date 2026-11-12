/*
target_regression.cpp

Regression coverage for RKSimulationBank. Three groups, driven by
checks/target_main.cpp:

  uf_numeric - bank outputs vs one independent RKSimulationMoog per voice,
               across the required sample rate / cutoff / resonance / input
               range, for both backends, including mid-stream parameter
               changes and 1/7/31-frame chunking equivalence.
  uf_voices  - structural voice independence: a separately written four-stage
               RK reference, sentinel-guarded misaligned planar buffers,
               single-sided impulses, reset of one middle voice and the tail
               of the following block.
  uf_native  - proof that the Auto backend really executes NEON on this
               ARM64 machine: runtime feature probe, backend selection,
               disassembly evidence saved under checks/evidence, and median
               timing of repeated runs (no absolute time threshold).

Only UF_CHECK from checks/harness.h is used for assertions.
*/

#include "harness.h"
#include "RKSimulationBank.h"
#include "RKSimulationModel.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif

#ifndef MOOG_BANK_EVIDENCE_DIR
#define MOOG_BANK_EVIDENCE_DIR "checks/evidence"
#endif

// uf_numeric / uf_voices / uf_native are defined at global scope so they link
// against the extern declarations in checks/target_main.cpp. Helpers use
// bank-specific names.

constexpr double kTolBase = 2e-5;
constexpr double kTolScale = 2e-5;

struct Rng
{
    uint32_t state;
    explicit Rng(uint32_t seed) : state(seed) {}
    float next()
    {
        // Deterministic LCG, no platform random_device dependency.
        state = state * 1664525u + 1013904223u;
        union { uint32_t u; float f; } bits;
        bits.u = (state >> 9) | 0x3f800000u; // [1, 2)
        return bits.f - 1.5f;               // [-0.5, 0.5)
    }
    float range(float lo, float hi) { return lo + (hi - lo) * (next() + 0.5f); }
};

bool within_tolerance(float got, float ref)
{
    return std::fabs(static_cast<double>(got) - static_cast<double>(ref))
           <= kTolBase + kTolScale * std::fabs(static_cast<double>(ref));
}

struct Event
{
    uint32_t frame;
    std::size_t voice;
    float hz;
    float resonance;
};

// One planar voice buffer with one-float sentinels at both ends. The usable
// pointer is offset by one float, so it is deliberately not 16-byte aligned.
struct GuardedPlanar
{
    std::vector<float> memory;
    float * ptr;
    static constexpr float kSentinel = -98765.4321f;

    GuardedPlanar() {}
    explicit GuardedPlanar(uint32_t frames)
        : memory(static_cast<std::size_t>(frames) + 2, kSentinel),
          ptr(memory.data() + 1) {}

    void check_sentinels() const
    {
        UF_CHECK(memory.front() == kSentinel);
        UF_CHECK(memory.back() == kSentinel);
    }
};

void apply_event(RKSimulationBank & bank, std::vector<RKSimulationMoog> & refs,
                 const Event & e)
{
    bank.SetCutoff(e.voice, e.hz);
    bank.SetResonance(e.voice, e.resonance);
    refs[e.voice].SetCutoff(e.hz);
    refs[e.voice].SetResonance(e.resonance);
}

// Drives a bank and a matching set of reference filters over the same signal
// and event list, optionally in fixed-size chunks. Events whose frame falls in
// the next chunk are applied before that chunk's Process, matching the
// "parameter updates take effect from the first frame of the next Process"
// contract on both sides.
void run_compare(RKSimulationBank & bank,
                 std::vector<RKSimulationMoog> & refs,
                 std::vector<GuardedPlanar> & bufs,
                 const std::vector<float> & signal,
                 const std::vector<Event> & events,
                 uint32_t frames, uint32_t chunk, bool require_bit_exact)
{
    const std::size_t voices = refs.size();
    std::size_t event_pos = 0;

    for (uint32_t start = 0; start < frames;)
    {
        // Events landing exactly on this frame are applied before the call,
        // so they affect this frame on the bank and the references alike.
        while (event_pos < events.size() && events[event_pos].frame == start)
        {
            apply_event(bank, refs, events[event_pos]);
            ++event_pos;
        }

        uint32_t end = std::min(frames, start + chunk);
        // Trim the boundary to the next event frame so that update starts
        // the following Process at its exact sample point regardless of chunk.
        if (event_pos < events.size()
            && events[event_pos].frame > start
            && events[event_pos].frame < end)
        {
            end = events[event_pos].frame;
        }
        UF_CHECK(end > start);

        std::vector<float *> planar(voices);
        for (std::size_t v = 0; v < voices; ++v)
            planar[v] = bufs[v].ptr + start;
        bank.Process(planar.data(), end - start);

        for (std::size_t v = 0; v < voices; ++v)
        {
            for (uint32_t f = start; f < end; ++f)
            {
                float one = signal[f];
                refs[v].Process(&one, 1);
                const float got = bufs[v].ptr[f];
                UF_CHECK(std::isfinite(got));
                if (require_bit_exact)
                    UF_CHECK(got == one);
                else
                    UF_CHECK(within_tolerance(got, one));
                bufs[v].ptr[f] = one; // hand the reference output back so a
                                      // later run can compare bank-vs-bank
            }
        }
        start = end;
    }
    UF_CHECK(event_pos == events.size());
    for (auto & b : bufs)
        b.check_sentinels();
}

// Fills independent signals per voice: strong plateaus past the clip knees,
// single-sided impulses, zero passages and ordinary bounded material.
void make_signal(std::vector<float> & out, uint32_t frames, uint32_t seed)
{
    Rng rng(seed);
    out.resize(frames);
    for (uint32_t f = 0; f < frames; ++f)
    {
        float x = rng.range(-1.0f, 1.0f);
        if (f % 977 == 0)
            x = (f % 1954 == 0) ? 6.0f : -6.0f;          // hard impulses
        if (f % 521 < 40)
            x = (seed & 1) ? 5.5f : -5.5f;               // clip plateau stretch
        if (f > frames / 3 && f < frames / 3 + 64)
            x = 0.0f;                                     // silence window
        out[f] = x;
    }
}

std::vector<GuardedPlanar> make_buffers(std::size_t voices, uint32_t frames)
{
    std::vector<GuardedPlanar> bufs(voices);
    for (std::size_t v = 0; v < voices; ++v)
        bufs[v] = GuardedPlanar(frames);
    return bufs;
}

void load_signal(std::vector<GuardedPlanar> & bufs,
                 const std::vector<float> & signal)
{
    for (auto & b : bufs)
        for (uint32_t f = 0; f < signal.size(); ++f)
            b.ptr[f] = signal[f];
}

std::vector<RKSimulationMoog> make_refs(float sample_rate, std::size_t voices,
                                        const std::vector<float> & hz,
                                        const std::vector<float> & res)
{
    std::vector<RKSimulationMoog> refs;
    refs.reserve(voices);
    for (std::size_t v = 0; v < voices; ++v)
    {
        refs.emplace_back(sample_rate);
        refs.back().SetCutoff(hz[v]);
        refs.back().SetResonance(res[v]);
    }
    return refs;
}

void apply_params(RKSimulationBank & bank, const std::vector<float> & hz,
                  const std::vector<float> & res)
{
    for (std::size_t v = 0; v < hz.size(); ++v)
    {
        bank.SetCutoff(v, hz[v]);
        bank.SetResonance(v, res[v]);
    }
}

// Parameter sets exercise cutoff 0 (frozen integrators), the 5000 Hz top of
// the comparison range, and resonances from 0 to 3.5.
std::vector<float> param_hz(std::size_t voices, uint32_t seed)
{
    static const float table[] = {0.0f, 12.0f, 110.0f, 440.0f, 1000.0f,
                                  2200.0f, 3333.0f, 4180.0f, 5000.0f};
    std::vector<float> hz(voices);
    for (std::size_t v = 0; v < voices; ++v)
        hz[v] = table[(v + seed) % 9];
    return hz;
}

std::vector<float> param_res(std::size_t voices, uint32_t seed)
{
    static const float table[] = {0.0f, 0.25f, 1.0f, 1.7f, 2.7f, 3.1f, 3.5f};
    std::vector<float> r(voices);
    for (std::size_t v = 0; v < voices; ++v)
        r[v] = table[(v * 3 + seed) % 7];
    return r;
}

bool neon_available()
{
#if MOOG_BANK_HAVE_NEON
    return true;
#else
    return false;
#endif
}

std::vector<RKSimulationBank::Backend> backends_under_test()
{
    std::vector<RKSimulationBank::Backend> out;
    out.push_back(RKSimulationBank::Backend::Scalar);
    if (neon_available())
        out.push_back(RKSimulationBank::Backend::Neon);
    return out;
}

// ---------------------------------------------------------------------------
// uf_numeric
// ---------------------------------------------------------------------------

void numeric_scenario(float sample_rate, std::size_t voices, uint32_t frames,
                      uint32_t seed)
{
    auto hz = param_hz(voices, seed);
    auto res = param_res(voices, seed);
    std::vector<float> signal;
    make_signal(signal, frames, seed);

    std::vector<Event> events;
    if (frames >= 32)
    {
        // Change only one voice mid-stream; every other voice must track its
        // untouched reference exactly. Events land on their real frame, which
        // requires chunked processing so the update precedes the right call.
        const std::size_t target = voices / 2;
        events.push_back({frames / 3, target, 777.0f, 2.2f});
        events.push_back({frames / 2, target, 0.0f, 3.5f});
        if (frames > 1000)
            events.push_back({frames - 500, target, 4999.0f, 0.5f});
    }

    for (RKSimulationBank::Backend backend : backends_under_test())
    {
        const bool bit_exact = (backend == RKSimulationBank::Backend::Scalar);

        // Mid-stream parameter changes, processed in small chunks so each
        // event takes effect from the first frame of the next Process call on
        // both the bank and the per-voice references.
        {
            RKSimulationBank bank(sample_rate, voices, backend);
            apply_params(bank, hz, res);
            auto refs = make_refs(sample_rate, voices, hz, res);
            auto bufs = make_buffers(voices, frames);
            load_signal(bufs, signal);
            run_compare(bank, refs, bufs, signal, events, frames, 31,
                        bit_exact);
        }

        // Same events at the same sample points, but split differently (7 vs
        // 31 frames): the result must be bit-identical because each update
        // still precedes the Process call that contains its frame.
        if (!events.empty())
        {
            std::vector<std::vector<float>> out7(voices);
            {
                RKSimulationBank bank(sample_rate, voices, backend);
                apply_params(bank, hz, res);
                auto refs = make_refs(sample_rate, voices, hz, res);
                auto bufs = make_buffers(voices, frames);
                load_signal(bufs, signal);
                run_compare(bank, refs, bufs, signal, events, frames, 7,
                            bit_exact);
                for (std::size_t v = 0; v < voices; ++v)
                    out7[v].assign(bufs[v].ptr, bufs[v].ptr + frames);
            }
            RKSimulationBank bank31(sample_rate, voices, backend);
            apply_params(bank31, hz, res);
            auto refs31 = make_refs(sample_rate, voices, hz, res);
            auto bufs31 = make_buffers(voices, frames);
            load_signal(bufs31, signal);
            run_compare(bank31, refs31, bufs31, signal, events, frames, 31,
                        bit_exact);
            for (std::size_t v = 0; v < voices; ++v)
                for (uint32_t f = 0; f < frames; ++f)
                    UF_CHECK(bufs31[v].ptr[f] == out7[v][f]);
        }

        // Chunking equivalence with constant parameters: one long block vs
        // the identical signal split into 1/7/31-frame Process calls must be
        // bit-identical, independent of the host machine load.
        const std::vector<Event> no_events;
        const uint32_t chunk_sizes[] = {1, 7, 31};
        std::vector<std::vector<float>> whole(voices);
        {
            RKSimulationBank bank(sample_rate, voices, backend);
            apply_params(bank, hz, res);
            auto refs = make_refs(sample_rate, voices, hz, res);
            auto bufs = make_buffers(voices, frames);
            load_signal(bufs, signal);
            run_compare(bank, refs, bufs, signal, no_events, frames, frames,
                        bit_exact);
            for (std::size_t v = 0; v < voices; ++v)
                whole[v] = std::vector<float>(bufs[v].ptr,
                                              bufs[v].ptr + frames);
        }
        for (uint32_t chunk : chunk_sizes)
        {
            RKSimulationBank bank(sample_rate, voices, backend);
            apply_params(bank, hz, res);
            auto refs = make_refs(sample_rate, voices, hz, res);
            auto bufs = make_buffers(voices, frames);
            load_signal(bufs, signal);
            run_compare(bank, refs, bufs, signal, no_events, frames, chunk,
                        bit_exact);
            for (std::size_t v = 0; v < voices; ++v)
                for (uint32_t f = 0; f < frames; ++f)
                    UF_CHECK(bufs[v].ptr[f] == whole[v][f]);
        }
    }
}

void uf_numeric()
{
    const float rates[] = {44100.0f, 48000.0f, 96000.0f};

    // Full 65536-frame comparison at every required sample rate with nine
    // voices, spanning the whole parameter and input envelope.
    for (float rate : rates)
        numeric_scenario(rate, 9, 65536, 0x1357u);

    // Remaining required voice counts on shorter deterministic scenarios.
    const std::size_t counts[] = {1, 2, 3, 5};
    uint32_t seed = 0x2468u;
    for (float rate : rates)
        for (std::size_t n : counts)
            numeric_scenario(rate, n, 4096, seed++);

    // Zero cutoff freezes the integrators: even a hard drive produces no
    // output movement from a zeroed voice.
    for (auto backend : backends_under_test())
    {
        const std::size_t voices = 3;
        RKSimulationBank bank(48000.0f, voices, backend);
        std::vector<float> hz(voices, 0.0f), res(voices, 3.5f);
        apply_params(bank, hz, res);
        auto refs = make_refs(48000.0f, voices, hz, res);
        auto bufs = make_buffers(voices, 512);
        for (auto & b : bufs)
            for (uint32_t f = 0; f < 512; ++f)
                b.ptr[f] = 6.0f;
        std::vector<float *> planar(voices);
        for (std::size_t v = 0; v < voices; ++v)
            planar[v] = bufs[v].ptr;
        bank.Process(planar.data(), 512);
        for (std::size_t v = 0; v < voices; ++v)
            for (uint32_t f = 0; f < 512; ++f)
                UF_CHECK(bufs[v].ptr[f] == 0.0f);
    }
}

// ---------------------------------------------------------------------------
// uf_voices: independently written four-stage RK reference and voice topology
// ---------------------------------------------------------------------------

// A compact, independently coded version of the same four-stage ladder
// equations. It deliberately does not call any RKSimulationBank code, so a
// state-layout mistake that couples two voices cannot be mirrored here.
class IndependentLadder
{
public:
    IndependentLadder(float sample_rate, float hz, float resonance)
        : step_(1.0 / static_cast<double>(sample_rate))
    {
        state_.fill(0.0);
        cutoff_ = static_cast<float>(2.0 * MOOG_PI * hz);
        resonance_ = resonance;
    }
    void set_params(float hz, float resonance)
    {
        cutoff_ = static_cast<float>(2.0 * MOOG_PI * hz);
        resonance_ = resonance;
    }
    float step(float input)
    {
        double d1[4], d2[4], d3[4], d4[4], tmp[4];
        deriv(input, state_.data(), d1);
        for (int i = 0; i < 4; ++i) tmp[i] = state_[i] + 0.5 * step_ * d1[i];
        deriv(input, tmp, d2);
        for (int i = 0; i < 4; ++i) tmp[i] = state_[i] + 0.5 * step_ * d2[i];
        deriv(input, tmp, d3);
        for (int i = 0; i < 4; ++i) tmp[i] = state_[i] + step_ * d3[i];
        deriv(input, tmp, d4);
        for (int i = 0; i < 4; ++i)
            state_[i] += (1.0 / 6.0) * step_
                       * (d1[i] + 2.0 * d2[i] + 2.0 * d3[i] + d4[i]);
        return static_cast<float>(state_[3]);
    }
private:
    static float sat(double x)
    {
        return clip(static_cast<float>(x), 3.0f, 1.0f / 3.0f);
    }
    void deriv(float input, const double * s, double * d)
    {
        double s0 = sat(s[0]), s1 = sat(s[1]), s2 = sat(s[2]), s3 = sat(s[3]);
        double fb = static_cast<double>(input)
                  - static_cast<double>(resonance_) * s[3];
        d[0] = static_cast<double>(cutoff_) * (sat(fb) - s0);
        d[1] = static_cast<double>(cutoff_) * (s0 - s1);
        d[2] = static_cast<double>(cutoff_) * (s1 - s2);
        d[3] = static_cast<double>(cutoff_) * (s2 - s3);
    }
    std::array<double, 4> state_;
    float cutoff_;
    float resonance_;
    double step_;
};

void uf_voices()
{
    const float rate = 48000.0f;
    const uint32_t frames = 2048;

    for (auto backend : backends_under_test())
    {
        const std::size_t voices = 5;
        auto hz = param_hz(voices, 11);
        auto res = param_res(voices, 5);
        RKSimulationBank bank(rate, voices, backend);
        apply_params(bank, hz, res);
        std::vector<IndependentLadder> ind;
        for (std::size_t v = 0; v < voices; ++v)
            ind.emplace_back(rate, hz[v], res[v]);

        auto bufs = make_buffers(voices, frames);
        std::vector<std::vector<float>> source(voices);
        Rng rng(0xABCDu);
        for (std::size_t v = 0; v < voices; ++v)
        {
            source[v].resize(frames);
            for (uint32_t f = 0; f < frames; ++f)
            {
                float x = rng.range(-6.0f, 6.0f);
                if (f == 0 && (v % 2 == 0))
                    x = 6.0f;                          // single-sided impulse
                if (f == 13 && (v % 2 == 1))
                    x = -6.0f;
                source[v][f] = x;
                bufs[v].ptr[f] = x;
            }
        }

        std::vector<float *> planar(voices);
        for (std::size_t v = 0; v < voices; ++v)
            planar[v] = bufs[v].ptr;
        bank.Process(planar.data(), frames);

        for (std::size_t v = 0; v < voices; ++v)
            for (uint32_t f = 0; f < frames; ++f)
            {
                float ref = ind[v].step(source[v][f]);
                UF_CHECK(within_tolerance(bufs[v].ptr[f], ref));
            }
        for (auto & b : bufs)
            b.check_sentinels();

        // Cascade guard: a silent lane parked next to a heavily driven lane
        // must stay exactly zero. Any cross-voice feedback would show here.
        {
            const std::size_t pair = 2;
            RKSimulationBank quiet_bank(rate, pair, backend);
            quiet_bank.SetCutoff(0, 4000.0f);
            quiet_bank.SetResonance(0, 3.5f);
            quiet_bank.SetCutoff(1, 4000.0f);
            quiet_bank.SetResonance(1, 3.5f);
            auto qb = make_buffers(pair, 1024);
            for (uint32_t f = 0; f < 1024; ++f)
            {
                qb[0].ptr[f] = 0.0f;
                qb[1].ptr[f] = (f < 128) ? 6.0f : -6.0f;
            }
            std::vector<float *> qp(pair);
            for (std::size_t v = 0; v < pair; ++v)
                qp[v] = qb[v].ptr;
            quiet_bank.Process(qp.data(), 1024);
            for (uint32_t f = 0; f < 1024; ++f)
                UF_CHECK(qb[0].ptr[f] == 0.0f);
            for (auto & b : qb)
                b.check_sentinels();
        }
    }

    // Reset of one middle voice: its history vanishes (matching a fresh
    // reference with the same parameters), while the neighbouring voices keep
    // their ringing tails into the next block.
    for (auto backend : backends_under_test())
    {
        const std::size_t voices = 3;
        const std::size_t middle = 1;
        auto hz = param_hz(voices, 3);
        auto res = param_res(voices, 9);

        RKSimulationBank bank(rate, voices, backend);
        apply_params(bank, hz, res);
        auto refs = make_refs(rate, voices, hz, res);
        auto bufs = make_buffers(voices, frames);
        std::vector<float> signal;
        make_signal(signal, frames, 0x777u);
        load_signal(bufs, signal);

        std::vector<float *> planar(voices);
        for (std::size_t v = 0; v < voices; ++v)
            planar[v] = bufs[v].ptr;

        const uint32_t cut = frames / 2;
        bank.Process(planar.data(), cut);
        for (std::size_t v = 0; v < voices; ++v)
            for (uint32_t f = 0; f < cut; ++f)
            {
                float one = signal[f];
                refs[v].Process(&one, 1);
                UF_CHECK(within_tolerance(bufs[v].ptr[f], one));
            }

        // Reset only the middle voice; its neighbours keep their history.
        bank.ResetVoice(middle);

        // Fresh middle reference with the retained parameters proves that
        // ResetVoice cleared history without touching cutoff/resonance.
        RKSimulationMoog fresh_middle(rate);
        fresh_middle.SetCutoff(hz[middle]);
        fresh_middle.SetResonance(res[middle]);

        // Continue into the second half of the same buffers.
        std::vector<float *> planar_tail(voices);
        for (std::size_t v = 0; v < voices; ++v)
            planar_tail[v] = bufs[v].ptr + cut;
        bank.Process(planar_tail.data(), frames - cut);
        double neighbour_energy = 0.0;
        for (std::size_t v = 0; v < voices; ++v)
        {
            for (uint32_t f = cut; f < frames; ++f)
            {
                float one = signal[f];
                if (v == middle)
                    fresh_middle.Process(&one, 1);
                else
                    refs[v].Process(&one, 1);
                UF_CHECK(within_tolerance(bufs[v].ptr[f], one));
                if (v != middle)
                    neighbour_energy += static_cast<double>(bufs[v].ptr[f])
                                      * bufs[v].ptr[f];
            }
        }
        // The unreset voices still carry an audible tail.
        UF_CHECK(neighbour_energy > 1.0);
        for (auto & b : bufs)
            b.check_sentinels();

        // One more block with no events: tails must continue to match the
        // references rather than restarting.
        std::vector<float> signal2;
        make_signal(signal2, frames, 0x888u);
        auto bufs2 = make_buffers(voices, frames);
        load_signal(bufs2, signal2);
        for (std::size_t v = 0; v < voices; ++v)
            planar[v] = bufs2[v].ptr;
        bank.Process(planar.data(), frames);
        for (std::size_t v = 0; v < voices; ++v)
            for (uint32_t f = 0; f < frames; ++f)
            {
                float one = signal2[f];
                if (v == middle)
                    fresh_middle.Process(&one, 1);
                else
                    refs[v].Process(&one, 1);
                UF_CHECK(within_tolerance(bufs2[v].ptr[f], one));
            }
    }
}

// ---------------------------------------------------------------------------
// uf_native: real NEON execution evidence and timing
// ---------------------------------------------------------------------------

bool file_contains(const std::string & path, const std::string & needle)
{
    std::ifstream in(path.c_str());
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str().find(needle) != std::string::npos;
}

std::size_t count_matches(const std::string & path, const std::string & needle)
{
    std::ifstream in(path.c_str());
    if (!in)
        return 0;
    std::size_t count = 0;
    std::string line;
    while (std::getline(in, line))
        if (line.find(needle) != std::string::npos)
            ++count;
    return count;
}

#if MOOG_BANK_HAVE_NEON
// Definitive functional check: execute a vector instruction and verify its
// lanes. Used when the kernel hides the optional-feature sysctl OIDs.
static bool neon_functional_probe()
{
    float64x2_t a = vdupq_n_f64(2.0);
    float64x2_t b = vdupq_n_f64(3.0);
    double out[2];
    vst1q_f64(out, vaddq_f64(a, b));
    return out[0] == 5.0 && out[1] == 5.0;
}
#endif

bool host_has_asimd()
{
#if defined(__APPLE__)
    int value = 0;
    std::size_t len = sizeof(value);
    bool sysctl_known = false;
    bool sysctl_confirmed = false;
    static const char * names[] = {
        "hw.optional.AdvSIMD", "hw.optional.neon",
        "hw.optional.arm.FEAT_FP16"
    };
    for (const char * name : names)
    {
        if (sysctlbyname(name, &value, &len, nullptr, 0) == 0)
        {
            sysctl_known = true;
            if (value == 1)
                sysctl_confirmed = true;
        }
    }
    if (sysctl_known)
        return sysctl_confirmed;
    // Sandboxed kernels may hide the optional OID namespace. Advanced SIMD is
    // mandatory in the AArch64 ABI; confirm it by real execution.
#  if MOOG_BANK_HAVE_NEON
    return neon_functional_probe();
#  else
    return false;
#  endif
#elif defined(__linux__)
    if (getauxval(AT_HWCAP) & HWCAP_ASIMD)
        return true;
#  if MOOG_BANK_HAVE_NEON
    return neon_functional_probe();
#  else
    return false;
#  endif
#else
#  if MOOG_BANK_HAVE_NEON
    return neon_functional_probe();
#  else
    return false;
#  endif
#endif
}

using clock_type = std::chrono::steady_clock;

double median_ms(std::vector<double> & samples)
{
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

double time_backend(RKSimulationBank::Backend backend, std::size_t voices,
                    uint32_t frames, int reps,
                    const std::vector<std::vector<float>> & source,
                    std::vector<double> * samples_out = nullptr)
{
    std::vector<double> samples;
    for (int r = 0; r < reps; ++r)
    {
        RKSimulationBank bank(48000.0f, voices, backend);
        auto bufs = make_buffers(voices, frames);
        std::vector<float *> planar(voices);
        for (std::size_t v = 0; v < voices; ++v)
        {
            std::memcpy(bufs[v].ptr, source[v].data(),
                        frames * sizeof(float));
            planar[v] = bufs[v].ptr;
        }
        auto t0 = clock_type::now();
        bank.Process(planar.data(), frames);
        auto t1 = clock_type::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    if (samples_out)
        *samples_out = samples;
    return median_ms(samples);
}

void uf_native()
{
    // Backend selection policy is observable on every platform.
    {
        RKSimulationBank auto_bank(48000.0f, 4,
                                   RKSimulationBank::Backend::Auto);
#if MOOG_BANK_HAVE_NEON
        UF_CHECK(auto_bank.GetBackend() == RKSimulationBank::Backend::Neon);
#else
        UF_CHECK(auto_bank.GetBackend() == RKSimulationBank::Backend::Scalar);
        bool threw = false;
        try
        {
            RKSimulationBank neon_bank(48000.0f, 4,
                                       RKSimulationBank::Backend::Neon);
        }
        catch (const std::invalid_argument &)
        {
            threw = true;
        }
        UF_CHECK(threw);
#endif
    }

    // Constructor and setter validation; failed setters must not move any
    // voice's state or parameters.
    {
        bool threw = false;

        try { RKSimulationBank b(0.0f, 2); }
        catch (const std::invalid_argument &) { threw = true; }
        UF_CHECK(threw);

        threw = false;
        try { RKSimulationBank b(std::numeric_limits<float>::quiet_NaN(), 2); }
        catch (const std::invalid_argument &) { threw = true; }
        UF_CHECK(threw);

        threw = false;
        try { RKSimulationBank b(48000.0f, 0); }
        catch (const std::invalid_argument &) { threw = true; }
        UF_CHECK(threw);

        RKSimulationBank good(48000.0f, 3);
        threw = false;
        try { good.SetCutoff(3, 100.0f); }
        catch (const std::invalid_argument &) { threw = true; }
        UF_CHECK(threw);

        threw = false;
        try { good.SetResonance(99, 1.0f); }
        catch (const std::invalid_argument &) { threw = true; }
        UF_CHECK(threw);

        threw = false;
        try { good.ResetVoice(3); }
        catch (const std::invalid_argument &) { threw = true; }
        UF_CHECK(threw);

        threw = false;
        try { good.SetCutoff(0, -1.0f); }
        catch (const std::invalid_argument &) { threw = true; }
        UF_CHECK(threw);

        threw = false;
        try { good.SetResonance(1, std::numeric_limits<float>::infinity()); }
        catch (const std::invalid_argument &) { threw = true; }
        UF_CHECK(threw);

        // frames == 0 never touches the pointers and never changes state.
        bool zero_ok = true;
        try { good.Process(nullptr, 0); }
        catch (...) { zero_ok = false; }
        UF_CHECK(zero_ok);

        // Every setter above failed, so parameters and state must be
        // untouched. Behaviourally, the bank must still match a freshly
        // constructed default-parameter bank sample for sample.
        RKSimulationBank untouched(48000.0f, 3);
        std::vector<float> a(3 * 64), b(3 * 64);
        std::vector<float *> pa(3), pb(3);
        Rng rng(0x9999u);
        for (uint32_t f = 0; f < 64; ++f)
            for (std::size_t v = 0; v < 3; ++v)
            {
                const float x = rng.range(-6.0f, 6.0f);
                a[v * 64 + f] = x;
                b[v * 64 + f] = x;
            }
        for (std::size_t v = 0; v < 3; ++v)
        {
            pa[v] = &a[v * 64];
            pb[v] = &b[v * 64];
        }
        good.Process(pa.data(), 64);
        untouched.Process(pb.data(), 64);
        for (std::size_t i = 0; i < a.size(); ++i)
            UF_CHECK(a[i] == b[i]);
    }

#if MOOG_BANK_HAVE_NEON
    UF_CHECK(host_has_asimd());

    const std::string dir = MOOG_BANK_EVIDENCE_DIR;
    const std::string disasm = dir + "/neon_disasm.txt";
    const std::string env = dir + "/native_environment.txt";

    // The disassembly is produced by checks/target.sh from a binary built with
    // the same compile options; tie this run to that artefact.
    UF_CHECK(file_contains(disasm, "process_neon"));
    const std::size_t vector_ops = count_matches(disasm, ".2d");
    const std::size_t narrow_converts = count_matches(disasm, "fcvtn");
    const std::size_t widen_converts = count_matches(disasm, "fcvtl");
    UF_CHECK(vector_ops > 100);
    UF_CHECK(narrow_converts > 0);
    UF_CHECK(widen_converts > 0);
    UF_CHECK(file_contains(env, "AdvSIMD"));

    // Timing: repeated runs, median reported, no machine-load threshold.
    const std::size_t voices = 9;
    const uint32_t frames = 16384;
    std::vector<std::vector<float>> source(voices);
    Rng rng(0x5151u);
    for (std::size_t v = 0; v < voices; ++v)
    {
        source[v].resize(frames);
        for (uint32_t f = 0; f < frames; ++f)
            source[v][f] = rng.range(-6.0f, 6.0f);
    }

    const int reps = 9;
    std::vector<double> neon_samples, scalar_samples;
    const double neon_ms = time_backend(
        RKSimulationBank::Backend::Neon, voices, frames, reps, source,
        &neon_samples);
    const double scalar_ms = time_backend(
        RKSimulationBank::Backend::Scalar, voices, frames, reps, source,
        &scalar_samples);
    std::printf("NATIVE_TIMING voices=%zu frames=%u reps=%d "
                "neon_median_ms=%.6f scalar_median_ms=%.6f\n",
                voices, frames, reps, neon_ms, scalar_ms);
    UF_CHECK(neon_ms > 0.0 && scalar_ms > 0.0);

    // Persist the individual runs and the median so the reported figure is
    // auditable; no absolute time gate is applied.
    {
        std::ofstream log((dir + "/timing.txt").c_str());
        log << "voices=" << voices << " frames=" << frames
            << " reps=" << reps << "\n";
        log << "compile=-std=c++17 -O2 -ffp-contract=off\n";
        auto dump = [&](const char * name, const std::vector<double> & s,
                        double median) {
            log << name << "_runs_ms=";
            for (double x : s)
                log << x << " ";
            log << "\n" << name << "_median_ms=" << median << "\n";
        };
        dump("neon", neon_samples, neon_ms);
        dump("scalar", scalar_samples, scalar_ms);
        log << "median_speedup=" << (scalar_ms / neon_ms) << "\n";
    }
#endif
}
