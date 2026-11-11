#include "maxiSyncOsc.h"

#include "../maximilian.h"

#include <cmath>
#include <stdexcept>

namespace {
// A jump landing at (or beyond, by rounding) the end of the current
// interval is treated as landing exactly on the next sample boundary.
const double BOUNDARY_EPS = 1e-12;
}

maxiSyncOsc::maxiSyncOsc()
    : sampleRate(static_cast<double>(maxiSettings::sampleRate)),
      masterPhase(0.0),
      slavePhase(0.0),
      blepPos(0)
{
    if (!(sampleRate > 0.0) || !std::isfinite(sampleRate)) {
        sampleRate = 44100.0;
    }
    clearHistory();
}

void maxiSyncOsc::clearHistory()
{
    for (size_t i = 0; i < BLEP_TAPS; ++i) {
        blepBuffer[i] = 0.0;
    }
    blepPos = 0;
}

void maxiSyncOsc::reset()
{
    masterPhase = 0.0;
    slavePhase = 0.0;
    clearHistory();
}

void maxiSyncOsc::setSampleRate(double newSampleRate)
{
    if (!std::isfinite(newSampleRate) || newSampleRate <= 0.0) {
        throw std::invalid_argument(
            "maxiSyncOsc::setSampleRate requires a finite positive value");
    }
    sampleRate = newSampleRate;
    reset();
}

size_t maxiSyncOsc::latency() const
{
    // Corrections only touch the current and the next sample, so the
    // band-limited output is reported with zero fixed delay.  This
    // value never changes with frequency.
    return 0;
}

// Schedules a band-limited step of the given magnitude.  offset is the
// jump time as a fraction tau of the current sample interval, in [0, 1].
// The naive output holds the pre-jump level at this sample and the
// post-jump level at the next one, so the causal two-tap PolyBLEP
// residuals are +0.5*M*(1-tau)^2 now and -0.5*M*tau^2 next.  The
// quadratic form stays correct at both boundaries (tau 0 and 1), so no
// special-casing is needed.
void maxiSyncOsc::addJump(double offset, double magnitude)
{
    if (offset < 0.0) {
        offset = 0.0;
    } else if (offset > 1.0) {
        offset = 1.0;
    }
    const size_t next = (blepPos + 1) % BLEP_TAPS;
    const double a = 1.0 - offset;
    blepBuffer[blepPos] += 0.5 * magnitude * a * a;
    blepBuffer[next] -= 0.5 * magnitude * offset * offset;
}

double maxiSyncOsc::step(double masterHz, double slaveHz)
{
    if (!std::isfinite(masterHz) || !std::isfinite(slaveHz)
        || masterHz < 0.0 || slaveHz < 0.0
        || masterHz > 0.45 * sampleRate
        || slaveHz > 0.45 * sampleRate) {
        // Illegal control pair: zero output, state does not move.
        return 0.0;
    }

    // Naive sample value at the start of the interval.
    const double naive = 2.0 * slavePhase - 1.0;

    const double dm = masterHz / sampleRate;
    const double ds = slaveHz / sampleRate;

    // Time (fraction of the interval) of the master wrap, if any.
    const bool masterWraps = (dm > 0.0) && (masterPhase + dm >= 1.0);
    const double tMaster = masterWraps ? (1.0 - masterPhase) / dm : 1.0;

    // Time of the slave's natural wrap without a reset, if any.
    const bool slaveWraps = (ds > 0.0) && (slavePhase + ds >= 1.0);
    const double tSlave = slaveWraps ? (1.0 - slavePhase) / ds : 2.0;

    // Schedule every discontinuity of this interval first; the
    // correction landing on this sample is added to the slot that is
    // read below, the remainder stays for the next sample.
    if (masterWraps && slaveWraps && tSlave <= tMaster) {
        // Natural slave wrap at or before the reset point.
        addJump(tSlave, -2.0);
        if (tMaster - tSlave <= BOUNDARY_EPS) {
            // Coincident wrap and reset: a single real jump, never two
            // stacked corrections.  Left limit is +1, right limit -1,
            // which is exactly the -2 jump scheduled above.
        } else {
            // Separate events: the reset jumps from 2*phi-1 down to -1,
            // i.e. by -2*phi where phi is the phase reached since the wrap.
            const double phiAtReset = slavePhase + ds * tMaster - 1.0;
            addJump(tMaster, -2.0 * phiAtReset);
        }
        slavePhase = ds * (1.0 - tMaster);
    } else if (masterWraps) {
        // Reset first (or the slave is stationary).  Jump magnitude
        // -2*phi for waveform 2*phi-1 reset to phase 0.
        const double phiAtReset = slavePhase + ds * tMaster;
        addJump(tMaster, -2.0 * phiAtReset);
        slavePhase = ds * (1.0 - tMaster);
    } else {
        // No reset this interval; the slave wraps on its own.
        if (slaveWraps) {
            addJump(tSlave, -2.0);
            slavePhase = slavePhase + ds - 1.0;
        } else {
            slavePhase += ds;
        }
    }

    if (masterWraps) {
        masterPhase = masterPhase + dm - 1.0;
    } else {
        masterPhase += dm;
    }

    const double output = naive + blepBuffer[blepPos];

    // Consume the current slot and rotate onto the next sample.
    blepBuffer[blepPos] = 0.0;
    blepPos = (blepPos + 1) % BLEP_TAPS;

    return output;
}

double maxiSyncOsc::play(double masterHz, double slaveHz)
{
    return step(masterHz, slaveHz);
}

void maxiSyncOsc::process(const double *masterHz, const double *slaveHz,
                          double *out, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        out[i] = step(masterHz[i], slaveHz[i]);
    }
}
