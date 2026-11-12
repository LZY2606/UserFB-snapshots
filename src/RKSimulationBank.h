/*
RKSimulationBank.h

Voice-parallel counterpart of RKSimulationMoog. A bank runs the same
fourth-order Runge-Kutta transistor-ladder model as RKSimulationModel.h for
many mutually independent voices at once, applying one arithmetic operation
to several voices per instruction instead of looping over per-voice filter
objects.

The per-voice math is intentionally identical to RKSimulationMoog:

  * four double integration states and double RK temporaries per voice;
  * the MoogUtils.h clip() saturation, including its float arguments and
    float return value and its cubic evaluated in double;
  * cutoff and resonance stored as float (cutoff keeps the 2*pi rounding
    point of the original SetCutoff) and widened to double per use.

Only the scheduling across voices changes. No feedback quantity is shared
between voices. The original RKSimulationMoog, LadderFilterBase and the other
filter models are untouched.

Backends:
  Backend::Auto   - NEON on AArch64 (unless MOOG_BANK_FORCE_SCALAR is set),
                    scalar everywhere else.
  Backend::Scalar - portable per-voice scalar math, same evaluation order.
  Backend::Neon   - requires AArch64 NEON and MOOG_BANK_FORCE_SCALAR unset;
                    otherwise the constructor throws std::invalid_argument.

The backend is fixed at construction; there is no process-global policy.
*/

#pragma once

#ifndef RK_SIMULATION_BANK_H
#define RK_SIMULATION_BANK_H

#include "MoogUtils.h"

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <stdexcept>
#include <vector>

#if defined(__aarch64__) && !defined(MOOG_BANK_FORCE_SCALAR)
#define MOOG_BANK_HAVE_NEON 1
#include "RKSimulationBankNeon.h"
#else
#define MOOG_BANK_HAVE_NEON 0
#endif

namespace moog_bank_detail {

constexpr float kSaturation = 3.0f;
constexpr float kSaturationInv = 1.0f / 3.0f;

// Scalar twin of the NEON clip helper: literally MoogUtils.h :: clip() with
// the fixed saturation constants, isolated so both backends share one call
// site convention. Kept in a translation-unit-local inline function.
static inline float clip_scalar(double value)
{
    return clip(static_cast<float>(value), kSaturation, kSaturationInv);
}

} // namespace moog_bank_detail

class RKSimulationBank
{
public:

    enum class Backend { Auto, Scalar, Neon };

    RKSimulationBank(float sampleRate, std::size_t voices,
                     Backend backend = Backend::Auto)
        : voice_count_(voices)
    {
        if (!(std::isfinite(sampleRate) && sampleRate > 0.0f))
            throw std::invalid_argument(
                "RKSimulationBank: sample rate must be a finite positive number");
        if (voices == 0)
            throw std::invalid_argument(
                "RKSimulationBank: voice count must be greater than zero");

        if (backend == Backend::Neon && !MOOG_BANK_HAVE_NEON)
            throw std::invalid_argument(
                "RKSimulationBank: NEON backend is not available on this target "
                "(non-AArch64 build or MOOG_BANK_FORCE_SCALAR defined)");
        if (backend == Backend::Auto)
            backend = MOOG_BANK_HAVE_NEON
                          ? Backend::Neon
                          : Backend::Scalar;
        backend_ = backend;

        sample_rate_ = sampleRate;
        step_size_ = 1.0 / static_cast<double>(sampleRate);

        // Initial parameters match RKSimulationMoog's constructor.
        cutoff_.assign(voices, 0.0f);
        resonance_.assign(voices, 0.0f);
        for (std::size_t v = 0; v < voices; ++v)
        {
            cutoff_[v] = static_cast<float>(2.0 * MOOG_PI * 1000.0f);
            resonance_[v] = 1.0f;
        }

        // Stage-major, voice-contiguous storage: state_[stage][voice].
        // The NEON path loads two adjacent voices per vector; the extra
        // padding element after an odd voice count is scratch space only and
        // is never read back as a voice.
        const std::size_t stages = 4;
        const std::size_t slots = MOOG_BANK_HAVE_NEON
                                      ? round_up_two(voices)
                                      : voices;
        state_.assign(stages * slots, 0.0);
        state_slots_ = slots;
    }

    Backend GetBackend() const { return backend_; }

    void SetCutoff(std::size_t voice, float hz)
    {
        if (voice >= voice_count_)
            throw std::invalid_argument(
                "RKSimulationBank::SetCutoff: voice index out of range");
        if (!std::isfinite(hz) || hz < 0.0f)
            throw std::invalid_argument(
                "RKSimulationBank::SetCutoff: cutoff must be finite and >= 0 Hz");
        // Same rounding point as RKSimulationMoog::SetCutoff.
        cutoff_[voice] = static_cast<float>(2.0 * MOOG_PI * hz);
    }

    void SetResonance(std::size_t voice, float r)
    {
        if (voice >= voice_count_)
            throw std::invalid_argument(
                "RKSimulationBank::SetResonance: voice index out of range");
        if (!std::isfinite(r) || r < 0.0f)
            throw std::invalid_argument(
                "RKSimulationBank::SetResonance: resonance must be finite and >= 0");
        resonance_[voice] = r;
    }

    // Zeroes the four integration states of one voice only; parameters are
    // retained and every other voice keeps ringing.
    void ResetVoice(std::size_t voice)
    {
        if (voice >= voice_count_)
            throw std::invalid_argument(
                "RKSimulationBank::ResetVoice: voice index out of range");
        for (std::size_t stage = 0; stage < 4; ++stage)
            state_[stage * state_slots_ + voice] = 0.0;
    }

    // Processes `frames` samples per voice. planar[v] is an independent
    // mono float buffer of length `frames` and is overwritten in place with
    // the filtered output. No alignment is required. Frames == 0 is a no-op
    // that never dereferences the pointers. The caller guarantees valid
    // pointers and finite inputs for non-zero frame counts.
    void Process(float * const * planar, uint32_t frames)
    {
        if (frames == 0)
            return;

#if MOOG_BANK_HAVE_NEON
        if (backend_ == Backend::Neon)
        {
            process_neon(planar, frames);
            return;
        }
#endif
        process_scalar(planar, frames);
    }

private:

    static std::size_t round_up_two(std::size_t n)
    {
        return (n + std::size_t(1)) & ~std::size_t(1);
    }

    double & state_ref(std::size_t stage, std::size_t voice)
    {
        return state_[stage * state_slots_ + voice];
    }

    // Exact re-creation of RKSimulationMoog::calculateDerivatives for one
    // voice, with the float/double boundaries kept in the same places.
    void calculate_derivatives_scalar(
        float input, const double state[4], float cutoff, float resonance,
        double dstate[4]) const
    {
        double sat0 = moog_bank_detail::clip_scalar(state[0]);
        double sat1 = moog_bank_detail::clip_scalar(state[1]);
        double sat2 = moog_bank_detail::clip_scalar(state[2]);

        double feedback = static_cast<double>(input)
                        - static_cast<double>(resonance) * state[3];
        dstate[0] = static_cast<double>(cutoff)
                  * (moog_bank_detail::clip_scalar(feedback) - sat0);
        dstate[1] = static_cast<double>(cutoff) * (sat0 - sat1);
        dstate[2] = static_cast<double>(cutoff) * (sat1 - sat2);
        dstate[3] = static_cast<double>(cutoff)
                  * (sat2 - moog_bank_detail::clip_scalar(state[3]));
    }

    // Exact re-creation of RKSimulationMoog::rungekutteSolver.
    void runge_kutta_step_scalar(
        float input, double state[4], float cutoff, float resonance) const
    {
        double deriv1[4], deriv2[4], deriv3[4], deriv4[4], temp_state[4];

        calculate_derivatives_scalar(input, state, cutoff, resonance, deriv1);
        for (int i = 0; i < 4; ++i)
            temp_state[i] = state[i] + 0.5 * step_size_ * deriv1[i];

        calculate_derivatives_scalar(input, temp_state, cutoff, resonance, deriv2);
        for (int i = 0; i < 4; ++i)
            temp_state[i] = state[i] + 0.5 * step_size_ * deriv2[i];

        calculate_derivatives_scalar(input, temp_state, cutoff, resonance, deriv3);
        for (int i = 0; i < 4; ++i)
            temp_state[i] = state[i] + step_size_ * deriv3[i];

        calculate_derivatives_scalar(input, temp_state, cutoff, resonance, deriv4);

        for (int i = 0; i < 4; ++i)
            state[i] += (1.0 / 6.0) * step_size_
                      * (deriv1[i] + 2.0 * deriv2[i]
                         + 2.0 * deriv3[i] + deriv4[i]);
    }

    void process_scalar(float * const * planar, uint32_t frames)
    {
        double local_state[4];
        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            for (std::size_t v = 0; v < voice_count_; ++v)
            {
                for (std::size_t stage = 0; stage < 4; ++stage)
                    local_state[stage] = state_ref(stage, v);

                runge_kutta_step_scalar(
                    planar[v][frame], local_state, cutoff_[v], resonance_[v]);

                for (std::size_t stage = 0; stage < 4; ++stage)
                    state_ref(stage, v) = local_state[stage];
                planar[v][frame] = static_cast<float>(local_state[3]);
            }
        }
    }

#if MOOG_BANK_HAVE_NEON
    // Two voices per vector. Tail voices that do not fill a pair are advanced
    // with the same scalar solver so that no non-existent pointer is touched
    // and no phantom voice state ever moves.
    void process_neon(float * const * planar, uint32_t frames)
    {
        using namespace moog_bank_detail;

        const float64x2_t half_step = vdupq_n_f64(0.5 * step_size_);
        const float64x2_t step = vdupq_n_f64(step_size_);
        const float64x2_t sixth_step = vdupq_n_f64((1.0 / 6.0) * step_size_);

        const std::size_t paired_voices = voice_count_ & ~std::size_t(1);

        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            for (std::size_t base = 0; base < paired_voices; base += 2)
            {
                float64x2_t state[4];
                for (std::size_t stage = 0; stage < 4; ++stage)
                    state[stage] = vld1q_f64(&state_[stage * state_slots_ + base]);

                // One lane per voice, gathered from independent planar
                // buffers; no alignment assumption. vld1 has no alignment
                // requirement and the pair lives in a register-sized slot.
                float input_pair[2] = { planar[base][frame],
                                        planar[base + 1][frame] };
                float64x2_t input = vcvt_f64_f32(vld1_f32(input_pair));
                float64x2_t cutoff = vcvt_f64_f32(
                    vld1_f32(&cutoff_[base]));
                float64x2_t resonance = vcvt_f64_f32(
                    vld1_f32(&resonance_[base]));

                runge_kutta_step_neon(state, input, cutoff, resonance,
                                      half_step, step, sixth_step);

                for (std::size_t stage = 0; stage < 4; ++stage)
                    vst1q_f64(&state_[stage * state_slots_ + base], state[stage]);

                planar[base][frame] = static_cast<float>(vgetq_lane_f64(state[3], 0));
                planar[base + 1][frame] =
                    static_cast<float>(vgetq_lane_f64(state[3], 1));
            }

            if (paired_voices < voice_count_)
            {
                const std::size_t v = paired_voices;
                double local_state[4];
                for (std::size_t stage = 0; stage < 4; ++stage)
                    local_state[stage] = state_ref(stage, v);
                runge_kutta_step_scalar(
                    planar[v][frame], local_state, cutoff_[v], resonance_[v]);
                for (std::size_t stage = 0; stage < 4; ++stage)
                    state_ref(stage, v) = local_state[stage];
                planar[v][frame] = static_cast<float>(local_state[3]);
            }
        }
    }
#endif

    std::size_t voice_count_;
    std::size_t state_slots_;
    float sample_rate_;
    double step_size_;
    Backend backend_;
    std::vector<float> cutoff_;
    std::vector<float> resonance_;
    // 4 stages * state_slots_ doubles, stage-major.
    std::vector<double> state_;
};

// MOOG_BANK_HAVE_NEON is intentionally left defined so translation units that
// include this header (including the regression test) can branch on the
// compile-time NEON availability. It is re-guarded on every inclusion.

#endif
