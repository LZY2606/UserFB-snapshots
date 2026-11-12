/*
RKSimulationBankNeon.h

AArch64 NEON internals for RKSimulationBank. The routines here vectorise one
Runge-Kutta stage across two independent voices at a time (a vector lane per
voice). Every arithmetic step of the scalar solver has a vector counterpart:
only the planar gather/scatter at a frame boundary is scalar, because the
public API hands over one independent buffer per voice without any alignment
guarantee.

Numerical conventions are deliberately matched to RKSimulationMoog instead of
being "cleaned up":

  * the four integration states and all RK temporaries are double;
  * clip() arguments and results still round-trip through float, including the
    float saturation constants, and the cubic itself is evaluated in double
    after the float widening;
  * cutoff and resonance stay stored as float and are widened per use.

The helpers in this file are fuzz-checked bit-for-bit against the scalar
reference in tests/target_regression.cpp.
*/

#pragma once

#ifndef RK_SIMULATION_BANK_NEON_H
#define RK_SIMULATION_BANK_NEON_H

#include <arm_neon.h>

namespace moog_bank_detail {

// Vector equivalent of MoogUtils.h :: clip(value, 3.0, 1.0/3.0), keeping the
// float -> double -> float promotion pattern of the scalar function.
static inline float64x2_t clip_neon(float64x2_t value)
{
    const float32x2_t saturation_inv = vdup_n_f32(1.0f / 3.0f);
    const float32x2_t one = vdup_n_f32(1.0f);
    const float32x2_t minus_one = vdup_n_f32(-1.0f);

    // Scalar clip takes a float argument, so the double state is rounded here.
    float32x2_t value_f = vcvt_f32_f64(value);
    float32x2_t scaled = vmul_f32(value_f, saturation_inv);

    // Branchless form of the scalar ternary chain. NaNs fall through to
    // "scaled" exactly like the scalar comparisons do.
    uint32x2_t above = vcgt_f32(scaled, one);
    uint32x2_t below = vclt_f32(scaled, minus_one);
    float32x2_t v2 = vbsl_f32(above, one,
                      vbsl_f32(below, minus_one, scaled));

    // The scalar cubic widens v2 to double and evaluates left-to-right in
    // double as (1/3) * v2 * v2 * v2; the final float return value is what
    // widens back to double. The multiplication order is load-bearing.
    float64x2_t vd = vcvt_f64_f32(v2);
    float64x2_t cubic = vmulq_f64(vdupq_n_f64(1.0 / 3.0), vd);
    cubic = vmulq_f64(cubic, vd);
    cubic = vmulq_f64(cubic, vd);
    float64x2_t result_d = vmulq_f64(vdupq_n_f64(3.0), vsubq_f64(vd, cubic));
    return vcvt_f64_f32(vcvt_f32_f64(result_d));
}

// Vector equivalent of RKSimulationMoog::calculateDerivatives. Lane 0 and
// lane 1 belong to different voices; feedback never crosses lanes.
static inline void derivatives_neon(
    const float64x2_t state[4], float64x2_t input,
    float64x2_t cutoff, float64x2_t resonance,
    float64x2_t dstate[4])
{
    float64x2_t sat0 = clip_neon(state[0]);
    float64x2_t sat1 = clip_neon(state[1]);
    float64x2_t sat2 = clip_neon(state[2]);
    float64x2_t sat3 = clip_neon(state[3]);

    float64x2_t feedback = vsubq_f64(input, vmulq_f64(resonance, state[3]));

    dstate[0] = vmulq_f64(cutoff, vsubq_f64(clip_neon(feedback), sat0));
    dstate[1] = vmulq_f64(cutoff, vsubq_f64(sat0, sat1));
    dstate[2] = vmulq_f64(cutoff, vsubq_f64(sat1, sat2));
    dstate[3] = vmulq_f64(cutoff, vsubq_f64(sat2, sat3));
}

// One fourth-order Runge-Kutta step advanced for two voices simultaneously.
// Operation order mirrors RKSimulationMoog::rungekutteSolver so that, with
// fused multiply-add contraction disabled, each lane is bit-identical to the
// scalar solver.
static inline void runge_kutta_step_neon(
    float64x2_t state[4], float64x2_t input,
    float64x2_t cutoff, float64x2_t resonance,
    float64x2_t half_step, float64x2_t step, float64x2_t sixth_step)
{
    float64x2_t deriv1[4], deriv2[4], deriv3[4], deriv4[4], temp_state[4];

    derivatives_neon(state, input, cutoff, resonance, deriv1);
    for (int i = 0; i < 4; ++i)
        temp_state[i] = vaddq_f64(state[i], vmulq_f64(half_step, deriv1[i]));

    derivatives_neon(temp_state, input, cutoff, resonance, deriv2);
    for (int i = 0; i < 4; ++i)
        temp_state[i] = vaddq_f64(state[i], vmulq_f64(half_step, deriv2[i]));

    derivatives_neon(temp_state, input, cutoff, resonance, deriv3);
    for (int i = 0; i < 4; ++i)
        temp_state[i] = vaddq_f64(state[i], vmulq_f64(step, deriv3[i]));

    derivatives_neon(temp_state, input, cutoff, resonance, deriv4);

    for (int i = 0; i < 4; ++i)
    {
        float64x2_t accumulated = vaddq_f64(
            deriv1[i], vmulq_f64(vdupq_n_f64(2.0), deriv2[i]));
        accumulated = vaddq_f64(
            accumulated, vmulq_f64(vdupq_n_f64(2.0), deriv3[i]));
        accumulated = vaddq_f64(accumulated, deriv4[i]);
        state[i] = vaddq_f64(state[i], vmulq_f64(sixth_step, accumulated));
    }
}

} // namespace moog_bank_detail

#endif
