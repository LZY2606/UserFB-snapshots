// Regression tests for maxiSyncOsc.  The band-limited reference lives in
// hardsync_ref.cpp (no Maximilian dependencies); argv[1], when given, is
// the path of the separately compiled standalone reference program,
// which is exercised through popen() and checked against the in-process
// reference numbers.
#include "maximilian.h"
#include "libs/maxiSyncOsc.h"
#include "hardsync_ref.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checksRun = 0;

void check(bool condition, const std::string &message)
{
    ++checksRun;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", message.c_str());
    }
}

double rms(const std::vector<double> &v)
{
    double s = 0.0;
    for (double x : v) {
        s += x * x;
    }
    return std::sqrt(s / v.size());
}

double rmsDiff(const std::vector<double> &a, const std::vector<double> &b)
{
    double s = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double d = a[i] - b[i];
        s += d * d;
    }
    return std::sqrt(s / a.size());
}

double mean(const std::vector<double> &v)
{
    double s = 0.0;
    for (double x : v) {
        s += x;
    }
    return s / static_cast<double>(v.size());
}

// Hard accuracy metric against the independently integrated reference.
void accuracyCase(double sampleRate, double masterHz, double slaveHz,
                  size_t skip, size_t count, bool integerRatio)
{
    maxiSettings::setup(static_cast<size_t>(sampleRate), 1, 64);
    maxiSyncOsc osc;
    const size_t lat = osc.latency();
    check(lat <= 64, "latency within 64 samples");

    std::vector<double> raw(skip + lat + count);
    for (size_t i = 0; i < raw.size(); ++i) {
        raw[i] = osc.play(masterHz, slaveHz);
    }
    std::vector<double> got(raw.begin()
                                + static_cast<std::ptrdiff_t>(skip + lat),
                            raw.end());

    std::vector<double> ref(count), naive(count);
    hardsync_ref::bandlimited(sampleRate, masterHz, slaveHz, skip, count,
                              ref.data());
    hardsync_ref::naiveHardReset(sampleRate, masterHz, slaveHz, skip, count,
                                 naive.data());

    const double errBL = rmsDiff(got, ref);
    const double errNaive = rmsDiff(naive, ref);
    std::printf("  fs=%.0f master=%.2f slave=%.2f rms=%.4f (ref %.4f)"
                " blErr=%.5f naiveErr=%.5f ratio=%.3f dc=%+.4f/%+.4f\n",
                sampleRate, masterHz, slaveHz, rms(got), rms(ref),
                errBL, errNaive, errBL / errNaive,
                mean(got), mean(ref));

    for (double x : got) {
        check(std::isfinite(x), "all output samples finite");
    }
    check(std::fabs(mean(got) - mean(ref)) < 0.02,
          "true DC component preserved");
    if (integerRatio) {
        check(errBL < 0.08, "band-limited RMS error below 0.08");
        check(errBL < 0.60 * errNaive,
              "band-limited error below 60% of direct-reset error");
    } else {
        // Non-integer ratios use the fine-grid fallback reference.
        check(errBL < 0.60 * errNaive,
              "non-integer ratio: band-limited beats direct reset");
    }
}

// Runs the standalone reference program and checks it against the
// in-process reference for the same request.
void standaloneProgram(const std::string &program)
{
    const double fs = 48000.0, m = 750.0, s = 1973.0;
    const size_t skip = 4096, count = 64;
    std::vector<double> ref(count);
    hardsync_ref::bandlimited(fs, m, s, skip, count, ref.data());

    const std::string cmd = program + " bl "
        + std::to_string(fs) + " " + std::to_string(m) + " "
        + std::to_string(s) + " " + std::to_string(skip) + " "
        + std::to_string(count) + " 2>&1";
    FILE *pipe = popen(cmd.c_str(), "r");
    check(pipe != nullptr, "standalone reference program started");
    if (!pipe) {
        return;
    }
    size_t lines = 0;
    double value = 0.0;
    while (lines < count && std::fscanf(pipe, "%lf", &value) == 1) {
        check(std::fabs(value - ref[lines]) < 1e-9,
              "standalone program matches in-process reference");
        ++lines;
    }
    pclose(pipe);
    check(lines == count, "standalone program produced every sample");
}

} // namespace

int main(int argc, char **argv)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    // ---- 1. Stable-signal accuracy, the hard numeric metric ----------
    std::printf("[1] stable signal accuracy (skip 4096, latency compensated)\n");
    accuracyCase(48000.0, 750.0, 1973.0, 4096, 8192, true);
    accuracyCase(48000.0, 1500.0, 3300.0, 4096, 8192, true);

    // ---- 2. Non-integer frequency ratios ------------------------------
    std::printf("[2] non-integer frequency ratios\n");
    accuracyCase(48000.0, 750.5, 1973.7, 4096, 4096, false);
    accuracyCase(44100.0, 666.7, 1777.3, 4096, 4096, false);

    // ---- 3. Coincident reset and natural slave wrap -------------------
    std::printf("[3] coincident jumps emit a single correction\n");
    accuracyCase(48000.0, 1000.0, 1000.0, 4096, 4096, true);
    accuracyCase(48000.0, 1000.0, 2000.0, 4096, 4096, true);
    // The coincidence lands inside the sample interval, not on a grid
    // point: the wrap time of a 2200 Hz slave matches the 1100 Hz reset.
    accuracyCase(48000.0, 1100.0, 2200.0, 4096, 4096, true);
    {
        // White-box check at the first exact coincidence: no stacked
        // corrections may push the output beyond a small overshoot.
        maxiSettings::setup(48000, 1, 64);
        maxiSyncOsc osc;
        double peak = 0.0;
        for (int i = 0; i < 4096; ++i) {
            const double y = osc.play(1000.0, 2000.0);
            peak = std::max(peak, std::fabs(y));
        }
        std::printf("  peak |output| at coincident jumps: %.4f\n", peak);
        check(peak < 1.05, "coincident jumps produce no double-correction spike");

        // Same check for the in-interval coincidence.
        maxiSyncOsc osc2;
        double peak2 = 0.0;
        for (int i = 0; i < 4096; ++i) {
            peak2 = std::max(peak2, std::fabs(osc2.play(1100.0, 2200.0)));
        }
        std::printf("  peak |output| at fractional coincident jumps: %.4f\n",
                    peak2);
        check(peak2 < 1.05,
              "fractional coincident jumps produce no double-correction spike");
    }

    // ---- 4. Slave slower than the master ------------------------------
    std::printf("[4] slave frequency below master frequency\n");
    accuracyCase(48000.0, 2000.0, 700.0, 4096, 4096, true);
    accuracyCase(44100.0, 3000.0, 440.0, 4096, 4096, true);

    // ---- 5. Zero frequencies ------------------------------------------
    std::printf("[5] zero master (free running) and zero slave\n");
    accuracyCase(48000.0, 0.0, 1234.0, 4096, 4096, true);
    {
        maxiSettings::setup(48000, 1, 64);
        maxiSyncOsc osc;
        // Slave stationary at phase 0 until the first master reset;
        // resets must still fire and afterwards it stays at phase 0.
        double first = 0.0, afterReset = 0.0;
        bool sawReset = false;
        for (int i = 0; i < 96; ++i) {
            const double y = osc.play(1000.0, 0.0);
            if (i == 0) {
                first = y;
            }
            if (i == 49) {
                afterReset = y; // first master wrap is at sample 48
                sawReset = true;
            }
        }
        std::printf("  zero slave: first=%.4f afterReset=%.4f\n",
                    first, afterReset);
        check(std::fabs(first + 1.0) < 1e-12,
              "zero slave starts at 2*0-1 = -1");
        check(sawReset && std::fabs(afterReset + 1.0) < 0.02,
              "master reset still acts on a 0 Hz slave");
        // Both zero: constant -1 forever.
        maxiSyncOsc quiet;
        bool constant = true;
        for (int i = 0; i < 256; ++i) {
            constant &= quiet.play(0.0, 0.0) == -1.0;
        }
        check(constant, "0/0 Hz holds a constant -1");
    }

    // ---- 6. Control validity boundaries --------------------------------
    std::printf("[6] invalid control pairs output zero and freeze state\n");
    {
        maxiSettings::setup(48000, 1, 64);
        maxiSyncOsc osc, reference;
        const double limit = 0.45 * 48000.0;
        const std::array<double, 8> badMaster = {
            -1.0, nan, inf, limit + 1.0, 100.0, 100.0, 100.0, 100.0};
        const std::array<double, 8> badSlave = {
            100.0, 100.0, 100.0, 100.0, -0.5, nan, inf, limit * 1.1};
        std::vector<double> expected;
        for (size_t i = 0; i < badMaster.size(); ++i) {
            const double y = osc.play(badMaster[i], badSlave[i]);
            check(y == 0.0, "illegal pair returns exactly zero");
            // Reference oscillator only ever sees the legal calls, which
            // the frozen oscillator must still line up with afterwards.
            expected.push_back(reference.play(100.0, 200.0));
        }
        for (size_t i = 0; i < expected.size(); ++i) {
            const double y = osc.play(100.0, 200.0);
            check(y == expected[i],
                  "state resumes unchanged after illegal controls");
        }
        // Boundary value itself is legal.
        maxiSyncOsc edge;
        check(std::isfinite(edge.play(limit, 0.0)),
              "0.45*sampleRate is accepted");
    }

    // ---- 7. play/process equivalence and 1/7/31 frame chunking --------
    std::printf("[7] process() matches play() for 1/7/31 frame chunks\n");
    {
        maxiSettings::setup(48000, 1, 64);
        const size_t total = 1000;
        std::vector<double> master(total), slave(total);
        for (size_t i = 0; i < total; ++i) {
            // Switch frequencies at exact samples, include 0 Hz and
            // non-integer ratios.
            master[i] = (i / 137) % 3 == 0 ? 0.0
                       : (i / 137) % 3 == 1 ? 750.0 : 1500.25;
            slave[i] = (i / 211) % 4 == 0 ? 0.0
                       : (i / 211) % 4 == 1 ? 1973.0
                       : (i / 211) % 4 == 2 ? 3300.5 : 700.0;
        }
        // Sprinkle illegal samples.
        master[10] = -1.0;
        slave[20] = std::numeric_limits<double>::quiet_NaN();
        master[333] = 48000.0;

        maxiSyncOsc byPlay;
        std::vector<double> refPlay(total);
        for (size_t i = 0; i < total; ++i) {
            refPlay[i] = byPlay.play(master[i], slave[i]);
        }

        const size_t chunkSizes[] = {1, 7, 31};
        for (size_t chunk : chunkSizes) {
            maxiSyncOsc byProcess;
            std::vector<double> got(total, 0.0);
            for (size_t off = 0; off < total; off += chunk) {
                const size_t n = std::min(chunk, total - off);
                byProcess.process(master.data() + off, slave.data() + off,
                                  got.data() + off, n);
            }
            double maxDiff = 0.0;
            for (size_t i = 0; i < total; ++i) {
                maxDiff = std::max(maxDiff, std::fabs(got[i] - refPlay[i]));
            }
            std::printf("  chunk=%zu max|process-play|=%.3e\n",
                        chunk, maxDiff);
            check(maxDiff == 0.0, "process() is bit-identical to play()");
        }

        // count == 0 must not touch the pointers.
        maxiSyncOsc untouched;
        untouched.process(nullptr, nullptr, nullptr, 0);
        check(true, "process(..., 0) does not dereference pointers");
    }

    // ---- 8. Two sample rates -------------------------------------------
    std::printf("[8] behaviour at both supported sample rates\n");
    accuracyCase(48000.0, 480.0, 1440.0, 4096, 4096, true);
    accuracyCase(44100.0, 441.0, 1323.0, 4096, 4096, true);

    // ---- 9. reset() and setSampleRate() --------------------------------
    std::printf("[9] reset() and setSampleRate()\n");
    {
        maxiSettings::setup(48000, 1, 64);
        maxiSyncOsc osc;
        for (int i = 0; i < 500; ++i) {
            osc.play(750.0, 1973.0);
        }
        osc.reset();
        maxiSyncOsc fresh;
        bool same = true;
        for (int i = 0; i < 1000; ++i) {
            same &= osc.play(750.0, 1973.0)
                    == fresh.play(750.0, 1973.0);
        }
        check(same, "reset() reproduces a freshly constructed oscillator");

        // Invalid sample rates throw and leave the state untouched.
        maxiSyncOsc guarded;
        for (int i = 0; i < 100; ++i) {
            guarded.play(750.0, 1973.0);
        }
        maxiSyncOsc expected2;
        for (int i = 0; i < 100; ++i) {
            expected2.play(750.0, 1973.0);
        }
        const double badRates[] = {0.0, -48000.0, nan, inf};
        for (double bad : badRates) {
            bool threw = false;
            try {
                guarded.setSampleRate(bad);
            } catch (const std::invalid_argument &) {
                threw = true;
            }
            check(threw, "invalid setSampleRate throws std::invalid_argument");
        }
        bool preserved = true;
        for (int i = 0; i < 256; ++i) {
            preserved &= guarded.play(750.0, 1973.0)
                         == expected2.play(750.0, 1973.0);
        }
        check(preserved, "failed setSampleRate preserves the old state");

        // A valid rate change resets phases and history.
        guarded.setSampleRate(44100.0);
        check(guarded.play(441.0, 1323.0) == -1.0,
              "setSampleRate resets phase to zero");

        // The constructor reads maxiSettings::sampleRate once; later
        // changes to the global setting must not move an existing object.
        maxiSettings::setup(48000, 1, 64);
        maxiSyncOsc established, twin;
        for (int i = 0; i < 128; ++i) {
            established.play(750.0, 1973.0);
            twin.play(750.0, 1973.0);
        }
        maxiSettings::setup(22050, 1, 64);
        maxiSyncOsc newRate; // this one picks up 22050
        maxiSettings::setup(48000, 1, 64);
        bool unaffected = true;
        for (int i = 0; i < 512; ++i) {
            unaffected &= established.play(750.0, 1973.0)
                          == twin.play(750.0, 1973.0);
        }
        check(unaffected,
              "later maxiSettings changes do not affect existing objects");
        check(std::isfinite(newRate.play(1102.5, 1000.0)),
              "a new object picks up the current maxiSettings rate");
    }

    // ---- 10. Latency is fixed regardless of frequency ------------------
    std::printf("[10] latency reporting\n");
    {
        maxiSettings::setup(48000, 1, 64);
        maxiSyncOsc osc;
        const size_t reported = osc.latency();
        const double probe[] = {0.0, 1.0, 440.0, 5000.0, 0.45 * 48000.0};
        bool stable = true;
        for (double f : probe) {
            osc.play(750.0, f);
            stable &= osc.latency() == reported;
        }
        std::printf("  reported latency: %zu samples (constant across freq)\n",
                    reported);
        check(stable && reported <= 64,
              "latency is a fixed integer no larger than 64");
    }

    // ---- 11. Independent objects ---------------------------------------
    std::printf("[11] object independence\n");
    {
        maxiSettings::setup(48000, 1, 64);
        maxiSyncOsc a, b;
        bool independent = true;
        for (int i = 0; i < 512; ++i) {
            const double ya = a.play(750.0, 1973.0);
            const double yb1 = b.play(1500.0, 3300.0);
            const double yb2 = b.play(1500.0, 3300.0);
            (void)yb1;
            independent &= std::isfinite(ya) && std::isfinite(yb2);
        }
        check(independent, "two oscillators run without interference");
    }

    // ---- 12. Standalone reference program ------------------------------
    if (argc > 1) {
        std::printf("[12] standalone reference program: %s\n", argv[1]);
        standaloneProgram(argv[1]);
    }

    std::printf("\n%d checks, %d failure(s)\n", checksRun, failures);
    return failures == 0 ? 0 : 1;
}
