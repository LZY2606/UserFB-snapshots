/*############################################################################*/
/*#                                                                          #*/
/*#  Numerical checks for AmbisonicMovingEncoder                             #*/
/*#  Groups: arrival, direction, doppler, stream                             #*/
/*#                                                                          #*/
/*#  The reference implementation is independent of the component under     #*/
/*#  test: it first lists every encoded emission vector with its arrival    #*/
/*#  time, then walks the listener clock interval by interval.              #*/
/*#                                                                          #*/
/*############################################################################*/

#include "AmbisonicMovingEncoder.h"
#include "BFormat.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

using namespace spaudio;

namespace {

    const unsigned kSpeedOfSound = 344;
    const float kMaxError = 3e-5f;

    int g_checks = 0;
    bool g_failed = false;

    void Record(bool condition, const char* message, const char* file, int line)
    {
        g_checks++;
        if (!condition)
        {
            g_failed = true;
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", message, file, line);
        }
    }

    #define CHECK(cond) Record((cond), #cond, __FILE__, __LINE__)

    void CheckClose(float actual, double expected, float tolerance,
        const char* message, const char* file, int line)
    {
        g_checks++;
        if (!std::isfinite(actual) || std::fabs(actual - expected) > tolerance)
        {
            g_failed = true;
            std::fprintf(stderr,
                "FAIL: %s got %.9g expected %.9g tol %.2g (%s:%d)\n",
                message, (double)actual, expected, (double)tolerance, file, line);
        }
    }

    #define CHECK_CLOSE(actual, expected, tol) \
        CheckClose((actual), (expected), (tol), #actual, __FILE__, __LINE__)

    //----------------------------------------------------------------------
    // Independent reference model
    //----------------------------------------------------------------------

    // Spherical harmonic gains in 3D ACN/SN3D, transcribed from the field
    // convention used by AmbisonicSource so that third order can be checked
    // against the established soundfield convention.
    std::vector<double> ReferenceCoefficients(unsigned order,
        double azimuth, double elevation)
    {
        const double sqrt32 = std::sqrt(3.0) / 2.0;
        const double sqrt58 = std::sqrt(5.0 / 8.0);
        const double sqrt152 = std::sqrt(15.0) / 2.0;
        const double sqrt38 = std::sqrt(3.0 / 8.0);

        unsigned channels = (order + 1) * (order + 1);
        std::vector<double> coeff(channels, 0.0);

        double cosAz = std::cos(azimuth);
        double sinAz = std::sin(azimuth);
        double cosEl = std::cos(elevation);
        double sinEl = std::sin(elevation);
        double cos2Az = std::cos(2.0 * azimuth);
        double sin2Az = std::sin(2.0 * azimuth);
        double sin2El = std::sin(2.0 * elevation);

        coeff[0] = 1.0;
        if (order >= 1)
        {
            // Analytic first order gains.
            coeff[1] = sinAz * cosEl;
            coeff[2] = sinEl;
            coeff[3] = cosAz * cosEl;
        }
        if (order >= 2)
        {
            coeff[4] = sqrt32 * sin2Az * cosEl * cosEl;
            coeff[5] = sqrt32 * sinAz * sin2El;
            coeff[6] = 1.5 * sinEl * sinEl - 0.5;
            coeff[7] = sqrt32 * cosAz * sin2El;
            coeff[8] = sqrt32 * cos2Az * cosEl * cosEl;
        }
        if (order >= 3)
        {
            coeff[9] = sqrt58 * std::sin(3.0 * azimuth) * cosEl * cosEl * cosEl;
            coeff[10] = sqrt152 * sin2Az * sinEl * cosEl * cosEl;
            coeff[11] = sqrt38 * sinAz * cosEl * (5.0 * sinEl * sinEl - 1.0);
            coeff[12] = sinEl * (5.0 * sinEl * sinEl - 3.0) * 0.5;
            coeff[13] = sqrt38 * cosAz * cosEl * (5.0 * sinEl * sinEl - 1.0);
            coeff[14] = sqrt152 * cos2Az * sinEl * cosEl * cosEl;
            coeff[15] = sqrt58 * std::cos(3.0 * azimuth) * cosEl * cosEl * cosEl;
        }
        return coeff;
    }

    struct ReferenceEmission
    {
        double arrival;
        std::vector<double> vector;
    };

    // Builds the full emission list (virtual silent frame -1, real samples,
    // silent tail frame N) and renders every listener frame from scratch.
    std::vector<std::vector<double> > ReferenceRender(unsigned order,
        unsigned sampleRate, float roomRadius,
        const std::vector<float>& pcm,
        const std::vector<PolarPosition<float> >& positions)
    {
        const unsigned long long N = (unsigned long long)pcm.size();
        unsigned channels = (order + 1) * (order + 1);

        std::vector<ReferenceEmission> emissions;
        emissions.reserve((size_t)N + 2);

        auto arrivalOf = [&](unsigned long long index, float distance) {
            return (double)index + (double)distance * (double)sampleRate
                / (double)kSpeedOfSound;
        };

        // Virtual silent emission frame -1 uses the first frame distance.
        double firstArrival = N == 0 ? 0.0 : arrivalOf(0, positions[0].distance);
        emissions.push_back({ firstArrival - 1.0,
            std::vector<double>(channels, 0.0) });

        for (unsigned long long n = 0; n < N; n++)
        {
            const PolarPosition<float>& pos = positions[(size_t)n];
            std::vector<double> coeff = ReferenceCoefficients(order,
                (double)pos.azimuth, (double)pos.elevation);

            double wGain = 0.0;
            double otherGain = 0.0;
            if ((double)pos.distance < (double)roomRadius)
            {
                wGain = 1.0 - (double)pos.distance / (2.0 * (double)roomRadius);
                otherGain = (double)pos.distance / (2.0 * (double)roomRadius);
            }
            else
            {
                wGain = (double)roomRadius / (2.0 * (double)pos.distance);
                otherGain = wGain;
            }

            std::vector<double> vector(channels, 0.0);
            for (unsigned ch = 0; ch < channels; ch++)
            {
                double gain = ch == 0 ? wGain : otherGain;
                vector[ch] = (double)pcm[(size_t)n] * coeff[ch] * gain;
            }
            emissions.push_back({ arrivalOf(n, pos.distance), vector });
        }

        std::vector<std::vector<double> > output;
        if (N == 0)
            return output;

        // Silent tail emission at index N, holding the last distance.
        float lastDistance = positions.back().distance;
        double tailArrival = arrivalOf(N, lastDistance);
        emissions.push_back({ tailArrival,
            std::vector<double>(channels, 0.0) });

        double epsilon = 1e-12 * std::max(1.0, tailArrival);
        unsigned long long endFrame = (unsigned long long)
            std::ceil(tailArrival - epsilon);

        // Listener clock: locate each frame inside its arrival interval.
        size_t j = 0;
        for (unsigned long long r = 0; r < endFrame; r++)
        {
            while ((double)r > emissions[j + 1].arrival)
                j++;

            double fraction = ((double)r - emissions[j].arrival)
                / (emissions[j + 1].arrival - emissions[j].arrival);
            fraction = std::max(0.0, std::min(1.0, fraction));

            std::vector<double> frame(channels, 0.0);
            for (unsigned ch = 0; ch < channels; ch++)
            {
                frame[ch] = (1.0 - fraction) * emissions[j].vector[ch]
                    + fraction * emissions[j + 1].vector[ch];
            }
            output.push_back(frame);
        }
        return output;
    }

    //----------------------------------------------------------------------
    // Component driver helpers
    //----------------------------------------------------------------------

    struct RunOutput
    {
        bool allAccepted;
        std::vector<std::vector<float> > channels;
    };

    // Streams a whole input through Process() in chunks of the given size.
    RunOutput RunInChunks(AmbisonicMovingEncoder& encoder, unsigned order,
        const std::vector<float>& pcm,
        const std::vector<PolarPosition<float> >& positions, unsigned chunk)
    {
        RunOutput result;
        result.allAccepted = true;
        unsigned channels = (order + 1) * (order + 1);
        result.channels.resize(channels);

        size_t offset = 0;
        while (offset < pcm.size())
        {
            unsigned n = (unsigned)std::min((size_t)chunk, pcm.size() - offset);
            BFormat buffer;
            CHECK(buffer.Configure(order, true, n));
            bool accepted = encoder.Process(
                const_cast<float*>(&pcm[offset]), n,
                &positions[offset], &buffer);
            CHECK(accepted);
            if (!accepted)
            {
                result.allAccepted = false;
                return result;
            }
            for (unsigned ch = 0; ch < channels; ch++)
            {
                float* data = buffer.GetChannelPointer(ch);
                result.channels[ch].insert(result.channels[ch].end(),
                    data, data + n);
            }
            offset += n;
        }
        return result;
    }

    // Drains Finish() with repeatedly allocated small output capacities.
    std::vector<std::vector<float> > DrainFinish(
        AmbisonicMovingEncoder& encoder, unsigned order, unsigned capacity)
    {
        unsigned channels = (order + 1) * (order + 1);
        std::vector<std::vector<float> > tail(channels);

        while (true)
        {
            BFormat buffer;
            CHECK(buffer.Configure(order, true, capacity));
            unsigned written = encoder.Finish(&buffer, capacity);
            for (unsigned ch = 0; ch < channels; ch++)
            {
                float* data = buffer.GetChannelPointer(ch);
                tail[ch].insert(tail[ch].end(), data, data + written);
            }
            if (written == 0)
                break;
        }
        return tail;
    }

    void CompareToReference(const std::vector<std::vector<float> >& actual,
        const std::vector<std::vector<double> >& reference, const char* label)
    {
        if (reference.empty())
        {
            for (size_t ch = 0; ch < actual.size(); ch++)
                CHECK(actual[ch].empty());
            return;
        }
        unsigned channels = (unsigned)actual.size();
        CHECK(actual.size() == reference[0].size());
        CHECK(actual[0].size() == reference.size());
        for (size_t r = 0; r < reference.size(); r++)
            for (unsigned ch = 0; ch < channels; ch++)
            {
                char message[128];
                std::snprintf(message, sizeof(message),
                    "%s frame %zu channel %u", label, r, ch);
                CheckClose(actual[ch][r], reference[r][ch], kMaxError,
                    message, __FILE__, __LINE__);
            }
    }

    // Upward zero-crossing frequency estimate in cycles per listener sample.
    double EstimateFrequency(const std::vector<float>& signal,
        unsigned long long lastArrivalFrame)
    {
        std::vector<double> crossings;
        for (size_t i = 0; i + 1 < signal.size() && (long long)i < (long long)lastArrivalFrame; i++)
        {
            if (signal[i] <= 0.f && signal[i + 1] > 0.f
                && std::fabs(signal[i]) + std::fabs(signal[i + 1]) > 1e-4f)
            {
                double fraction = (0.0 - (double)signal[i])
                    / ((double)signal[i + 1] - (double)signal[i]);
                crossings.push_back((double)i + fraction);
            }
        }
        CHECK(crossings.size() >= 5);
        if (crossings.size() < 2)
            return 0.0;
        return (double)(crossings.size() - 1)
            / (crossings.back() - crossings.front());
    }

    PolarPosition<float> MakePosition(float azimuth, float elevation,
        float distance)
    {
        PolarPosition<float> position;
        position.azimuth = azimuth;
        position.elevation = elevation;
        position.distance = distance;
        return position;
    }

    //----------------------------------------------------------------------
    // arrival group: fixed integer and fractional delays
    //----------------------------------------------------------------------

    int ArrivalGroup()
    {
        const unsigned order = 1;
        const unsigned sampleRate = 344; // 1 sample per metre of travel
        const unsigned maxBlock = 32;
        const float roomRadius = 10.f;
        const unsigned N = 8;

        std::vector<float> pcm(N, 0.f);
        pcm[0] = 1.f; // single impulse

        for (int mode = 0; mode < 2; mode++)
        {
            float distance = mode == 0 ? 1.f : 0.5f;
            std::vector<PolarPosition<float> > positions(N,
                MakePosition(0.f, 0.f, distance));

            std::vector<std::vector<double> > reference = ReferenceRender(
                order, sampleRate, roomRadius, pcm, positions);

            double delaySamples = (double)distance * sampleRate / kSpeedOfSound;
            double expectedTotal = std::ceil((double)N + delaySamples - 1e-12);
            CHECK((double)reference.size() == expectedTotal);

            unsigned chunks[] = { 1, 7, 31, N };
            for (unsigned chunk : chunks)
            {
                AmbisonicMovingEncoder encoder;
                CHECK(encoder.Configure(order, true, sampleRate,
                    maxBlock, roomRadius));
                RunOutput run = RunInChunks(encoder, order, pcm,
                    positions, chunk);
                CHECK(run.allAccepted);
                std::vector<std::vector<float> > tail = DrainFinish(
                    encoder, order, 3);
                for (size_t ch = 0; ch < tail.size(); ch++)
                    run.channels[ch].insert(run.channels[ch].end(),
                        tail[ch].begin(), tail[ch].end());

                CompareToReference(run.channels, reference, "arrival");
                CHECK(run.channels[0].size() == reference.size());

                if (mode == 0)
                {
                    // Integer delay: the impulse vector is recovered exactly.
                    CHECK_CLOSE(run.channels[0][1], 0.95, kMaxError); // W
                    CHECK_CLOSE(run.channels[3][1], 0.05, kMaxError); // X
                    CHECK_CLOSE(run.channels[1][1], 0.0, kMaxError);  // Y
                    CHECK_CLOSE(run.channels[2][1], 0.0, kMaxError);  // Z
                    CHECK_CLOSE(run.channels[0][0], 0.0, kMaxError);
                    CHECK_CLOSE(run.channels[0][2], 0.0, kMaxError);
                }
                else
                {
                    // Half-sample delay: the impulse splits 50/50.
                    float wGain = 1.f - distance / (2.f * roomRadius);
                    CHECK_CLOSE(run.channels[0][0], 0.5 * wGain, kMaxError);
                    CHECK_CLOSE(run.channels[0][1], 0.5 * wGain, kMaxError);
                    CHECK_CLOSE(run.channels[0][2], 0.0, kMaxError);
                }
            }
        }

        if (g_failed)
            return 1;
        std::printf("PASS arrival checks=%d\n", g_checks);
        return 0;
    }

    //----------------------------------------------------------------------
    // direction group: moving direction and distance, in-flight memory
    //----------------------------------------------------------------------

    int DirectionGroup()
    {
        const unsigned order = 1;
        const unsigned sampleRate = 344;
        const unsigned N = 48;
        const unsigned maxBlock = N;
        const float roomRadius = 10.f;

        std::vector<float> pcm(N);
        std::vector<PolarPosition<float> > positions(N);
        for (unsigned n = 0; n < N; n++)
        {
            pcm[n] = (float)std::sin(0.35 * n) + (n % 11 == 0 ? 0.8f : 0.f);
            float azimuth = (float)(2.0 * M_PI) * 2.f * (float)n / (float)N;
            float elevation = 0.3f * (float)std::sin(2.0 * M_PI * n / 17.0);
            float distance = 4.f + 3.f * (float)std::sin(2.0 * M_PI * n / 60.0);
            positions[n] = MakePosition(azimuth, elevation, distance);
        }

        std::vector<std::vector<double> > reference = ReferenceRender(
            order, sampleRate, roomRadius, pcm, positions);

        unsigned chunks[] = { 1, 7, 31, N };
        for (unsigned chunk : chunks)
        {
            AmbisonicMovingEncoder encoder;
            CHECK(encoder.Configure(order, true, sampleRate,
                maxBlock, roomRadius));
            RunOutput run = RunInChunks(encoder, order, pcm, positions, chunk);
            CHECK(run.allAccepted);
            std::vector<std::vector<float> > tail = DrainFinish(
                encoder, order, 5);
            for (size_t ch = 0; ch < tail.size(); ch++)
                run.channels[ch].insert(run.channels[ch].end(),
                    tail[ch].begin(), tail[ch].end());
            CompareToReference(run.channels, reference, "direction");
        }

        // Two impulses emitted in opposite directions keep their own
        // direction after propagation, including across a block boundary.
        const unsigned M = 16;
        std::vector<float> impulsePcm(M, 0.f);
        impulsePcm[2] = 1.f;
        impulsePcm[10] = -1.f;
        std::vector<PolarPosition<float> > impulsePositions(M,
            MakePosition(0.f, 0.f, 2.f));
        impulsePositions[2] = MakePosition((float)M_PI / 2.f, 0.f, 2.f);
        impulsePositions[10] = MakePosition(-(float)M_PI / 2.f, 0.f, 2.f);

        AmbisonicMovingEncoder encoder;
        CHECK(encoder.Configure(order, true, sampleRate, 7, roomRadius));
        RunOutput run = RunInChunks(encoder, order, impulsePcm,
            impulsePositions, 7);
        CHECK(run.allAccepted);
        // Delay is exactly 2 samples: arrivals at frames 4 and 12.
        float otherGain = 2.f / (2.f * roomRadius);
        CHECK_CLOSE(run.channels[1][4], otherGain, kMaxError);  // +Y
        CHECK_CLOSE(run.channels[3][4], 0.0, kMaxError);
        // Signed product: sample -1 toward -Y gives (-1)*sin(-90)*g = +g.
        CHECK_CLOSE(run.channels[1][12], otherGain, kMaxError);
        CHECK_CLOSE(run.channels[3][12], 0.0, kMaxError);

        // Third order: all 16 channels follow the established convention.
        const unsigned order3 = 3;
        const unsigned N3 = 20;
        std::vector<float> pcm3(N3);
        std::vector<PolarPosition<float> > positions3(N3);
        for (unsigned n = 0; n < N3; n++)
        {
            pcm3[n] = 0.5f * (float)std::cos(0.7 * n + 0.2) + 0.25f;
            positions3[n] = MakePosition(0.9f * (float)n,
                0.25f * (float)std::sin(0.5 * n), 1.f + 0.15f * n);
        }
        std::vector<std::vector<double> > reference3 = ReferenceRender(
            order3, sampleRate, roomRadius, pcm3, positions3);

        AmbisonicMovingEncoder encoder3;
        CHECK(encoder3.Configure(order3, true, sampleRate, 31, roomRadius));
        RunOutput run3 = RunInChunks(encoder3, order3, pcm3, positions3, 7);
        CHECK(run3.allAccepted);
        std::vector<std::vector<float> > tail3 = DrainFinish(
            encoder3, order3, 11);
        for (size_t ch = 0; ch < tail3.size(); ch++)
            run3.channels[ch].insert(run3.channels[ch].end(),
                tail3[ch].begin(), tail3[ch].end());
        CompareToReference(run3.channels, reference3, "order3");

        // Exact front-centre third order constants at the first arrival.
        AmbisonicMovingEncoder encoderFront;
        CHECK(encoderFront.Configure(order3, true, sampleRate, 8, roomRadius));
        std::vector<float> frontPcm(8, 0.f);
        frontPcm[0] = 1.f;
        std::vector<PolarPosition<float> > frontPos(8,
            MakePosition(0.f, 0.f, 1.f));
        RunOutput runFront = RunInChunks(encoderFront, order3, frontPcm,
            frontPos, 8);
        CHECK(runFront.allAccepted);
        double wGainFront = 1.0 - 1.0 / (2.0 * roomRadius);
        double otherGainFront = 1.0 / (2.0 * roomRadius);
        CHECK_CLOSE(runFront.channels[0][1], wGainFront, kMaxError);
        CHECK_CLOSE(runFront.channels[3][1], otherGainFront, kMaxError); // X
        CHECK_CLOSE(runFront.channels[6][1], -0.5 * otherGainFront, kMaxError);
        CHECK_CLOSE(runFront.channels[8][1],
            (std::sqrt(3.0) / 2.0) * otherGainFront, kMaxError);
        CHECK_CLOSE(runFront.channels[13][1],
            -std::sqrt(3.0 / 8.0) * otherGainFront, kMaxError);
        CHECK_CLOSE(runFront.channels[15][1],
            std::sqrt(5.0 / 8.0) * otherGainFront, kMaxError);

        if (g_failed)
            return 1;
        std::printf("PASS direction checks=%d\n", g_checks);
        return 0;
    }

    //----------------------------------------------------------------------
    // doppler group: receding and approaching trajectories
    //----------------------------------------------------------------------

    int DopplerGroup()
    {
        const unsigned order = 1;
        const unsigned sampleRate = 344;
        const unsigned maxBlock = 400;
        const float roomRadius = 150.f;
        const double emissionFrequency = 0.02; // cycles per emission sample
        const float distanceStep = 0.25f;      // 0.25 sample of delay per frame

        for (int mode = 0; mode < 2; mode++)
        {
            bool receding = mode == 0;
            const unsigned N = receding ? 400 : 360;

            std::vector<float> pcm(N);
            std::vector<PolarPosition<float> > positions(N);
            for (unsigned n = 0; n < N; n++)
            {
                pcm[n] = (float)std::sin(2.0 * M_PI * emissionFrequency * n);
                float distance = receding
                    ? 1.f + distanceStep * (float)n
                    : 100.f - distanceStep * (float)n;
                positions[n] = MakePosition(0.f, 0.f, distance);
            }

            std::vector<std::vector<double> > reference = ReferenceRender(
                order, sampleRate, roomRadius, pcm, positions);

            unsigned chunks[] = { 1, 7, 31, N };
            for (unsigned chunk : chunks)
            {
                AmbisonicMovingEncoder encoder;
                CHECK(encoder.Configure(order, true, sampleRate,
                    maxBlock, roomRadius));
                RunOutput run = RunInChunks(encoder, order, pcm,
                    positions, chunk);
                CHECK(run.allAccepted);
                std::vector<std::vector<float> > tail = DrainFinish(
                    encoder, order, 17);
                for (size_t ch = 0; ch < tail.size(); ch++)
                    run.channels[ch].insert(run.channels[ch].end(),
                        tail[ch].begin(), tail[ch].end());
                CompareToReference(run.channels, reference, "doppler");
            }

            // Frequency measured on the rendered W channel, not on duration.
            AmbisonicMovingEncoder encoder;
            CHECK(encoder.Configure(order, true, sampleRate,
                maxBlock, roomRadius));
            RunOutput run = RunInChunks(encoder, order, pcm, positions, 31);
            CHECK(run.allAccepted);
            std::vector<std::vector<float> > tail = DrainFinish(
                encoder, order, 17);
            run.channels[0].insert(run.channels[0].end(),
                tail[0].begin(), tail[0].end());

            float lastDistance = positions.back().distance;
            unsigned long long lastArrival = (unsigned long long)std::floor(
                (double)(N - 1) + (double)lastDistance * sampleRate
                / kSpeedOfSound);
            double measured = EstimateFrequency(run.channels[0], lastArrival);
            double expected = receding
                ? emissionFrequency / 1.25
                : emissionFrequency / 0.75;
            CHECK(measured > 0.0);
            CHECK(std::fabs(measured - expected) / expected < 0.02);
            if (receding)
                CHECK(measured < emissionFrequency);
            else
                CHECK(measured > emissionFrequency);
        }

        if (g_failed)
            return 1;
        std::printf("PASS doppler checks=%d\n", g_checks);
        return 0;
    }

    //----------------------------------------------------------------------
    // stream group: empty calls, rejected blocks, reset, batched finish
    //----------------------------------------------------------------------

    int StreamGroup()
    {
        const unsigned order = 1;
        const unsigned sampleRate = 344;
        const unsigned maxBlock = 16;
        const float roomRadius = 10.f;

        // Configuration validation. AmbisonicBase asserts on order > 3, so the
        // order ceiling is covered by the accepted 3rd-order configuration.
        AmbisonicMovingEncoder probe;
        CHECK(!probe.Configure(order, true, 0, maxBlock, roomRadius));
        CHECK(!probe.Configure(order, true, sampleRate, 0, roomRadius));
        CHECK(!probe.Configure(order, true, sampleRate, maxBlock, 0.f));
        CHECK(!probe.Configure(order, true, sampleRate, maxBlock, -1.f));
        CHECK(!probe.Configure(order, true, sampleRate, maxBlock,
            std::numeric_limits<float>::quiet_NaN()));
        CHECK(probe.Configure(0, true, sampleRate, maxBlock, roomRadius));
        CHECK(probe.Configure(3, false, sampleRate, maxBlock, roomRadius));
        CHECK(probe.Configure(order, true, sampleRate, maxBlock, roomRadius));

        // Empty stream: empty Process calls and a zero-frame Finish.
        AmbisonicMovingEncoder emptyEncoder;
        CHECK(emptyEncoder.Configure(order, true, sampleRate,
            maxBlock, roomRadius));
        BFormat smallBuffer;
        CHECK(smallBuffer.Configure(order, true, 4));
        CHECK(emptyEncoder.Process(nullptr, 0, nullptr, &smallBuffer));
        CHECK(emptyEncoder.Finish(&smallBuffer, 4) == 0);
        CHECK(emptyEncoder.Finish(&smallBuffer, 4) == 0);
        CHECK(emptyEncoder.IsFinished());

        // Rejected blocks consume nothing: every invalid block is followed by
        // a valid stream that must match a reference of only the valid data.
        const unsigned N = 24;
        std::vector<float> pcm(N);
        std::vector<PolarPosition<float> > positions(N);
        for (unsigned n = 0; n < N; n++)
        {
            pcm[n] = (float)std::sin(0.4 * n) + 0.3f;
            positions[n] = MakePosition(0.5f * (float)n,
                0.1f * (float)std::sin(n), 2.f + 0.1f * (float)n);
        }
        std::vector<std::vector<double> > reference = ReferenceRender(
            order, sampleRate, roomRadius, pcm, positions);

        AmbisonicMovingEncoder encoder;
        CHECK(encoder.Configure(order, true, sampleRate,
            maxBlock, roomRadius));

        float nan = std::numeric_limits<float>::quiet_NaN();
        float inf = std::numeric_limits<float>::infinity();
        std::vector<float> badPcm(4, 0.5f);
        std::vector<PolarPosition<float> > badPositions(4,
            MakePosition(0.f, 0.f, 2.f));

        badPcm[1] = nan;
        CHECK(!encoder.Process(badPcm.data(), 4, badPositions.data(),
            &smallBuffer));
        badPcm[1] = inf;
        CHECK(!encoder.Process(badPcm.data(), 4, badPositions.data(),
            &smallBuffer));
        badPcm[1] = 0.5f;
        badPositions[0].azimuth = nan;
        CHECK(!encoder.Process(badPcm.data(), 4, badPositions.data(),
            &smallBuffer));
        badPositions[0].azimuth = 0.f;
        badPositions[2].elevation = (float)M_PI / 2.f + 0.1f;
        CHECK(!encoder.Process(badPcm.data(), 4, badPositions.data(),
            &smallBuffer));
        badPositions[2].elevation = -(float)M_PI / 2.f - 0.1f;
        CHECK(!encoder.Process(badPcm.data(), 4, badPositions.data(),
            &smallBuffer));
        badPositions[2].elevation = 0.f;
        badPositions[1].distance = -0.1f;
        CHECK(!encoder.Process(badPcm.data(), 4, badPositions.data(),
            &smallBuffer));
        badPositions[1].distance = 150.1f;
        CHECK(!encoder.Process(badPcm.data(), 4, badPositions.data(),
            &smallBuffer));

        // Arrival difference out of range inside one block.
        std::vector<PolarPosition<float> > jumpPositions(4,
            MakePosition(0.f, 0.f, 1.f));
        jumpPositions[1].distance = 8.f; // difference of 8 samples
        CHECK(!encoder.Process(badPcm.data(), 4, jumpPositions.data(),
            &smallBuffer));
        jumpPositions[1].distance = 0.1f; // difference of 0.1 samples
        CHECK(!encoder.Process(badPcm.data(), 4, jumpPositions.data(),
            &smallBuffer));

        // Arrival difference out of range across a Process() boundary. This
        // uses a separate encoder so the main encoder still has no consumed
        // data after the rejected blocks above.
        AmbisonicMovingEncoder boundaryEncoder;
        CHECK(boundaryEncoder.Configure(order, true, sampleRate,
            maxBlock, roomRadius));
        std::vector<float> boundaryPcm(2, 0.7f);
        std::vector<PolarPosition<float> > boundaryPositions(2,
            MakePosition(0.f, 0.f, 1.f));
        BFormat twoBuffer;
        CHECK(twoBuffer.Configure(order, true, 2));
        CHECK(boundaryEncoder.Process(boundaryPcm.data(), 2,
            boundaryPositions.data(), &twoBuffer));
        std::vector<PolarPosition<float> > afterJump(2,
            MakePosition(0.f, 0.f, 1.f));
        afterJump[0].distance = 8.f;
        CHECK(!boundaryEncoder.Process(boundaryPcm.data(), 2,
            afterJump.data(), &twoBuffer));
        // The rejected block did not consume: an identical valid block works.
        std::vector<PolarPosition<float> > afterValid(2,
            MakePosition(0.f, 0.f, 1.f));
        CHECK(boundaryEncoder.Process(boundaryPcm.data(), 2,
            afterValid.data(), &twoBuffer));
        boundaryEncoder.Reset();

        // All rejections left the state untouched: the valid stream now
        // renders exactly as a fresh reference run.
        RunOutput run = RunInChunks(encoder, order, pcm, positions, 7);
        CHECK(run.allAccepted);
        std::vector<std::vector<float> > tail = DrainFinish(
            encoder, order, 3);
        for (size_t ch = 0; ch < tail.size(); ch++)
            run.channels[ch].insert(run.channels[ch].end(),
                tail[ch].begin(), tail[ch].end());
        CompareToReference(run.channels, reference, "stream");

        // After Finish, non-empty Process is rejected, empty accepted, and no
        // more sound comes out.
        BFormat oneBuffer;
        CHECK(oneBuffer.Configure(order, true, 1));
        CHECK(encoder.IsFinished());
        CHECK(!encoder.Process(pcm.data(), 1, positions.data(), &oneBuffer));
        CHECK(encoder.Process(nullptr, 0, nullptr, &oneBuffer));
        CHECK(encoder.Finish(&oneBuffer, 1) == 0);

        // Reset clears clocks and in-flight sound but keeps the configuration.
        encoder.Reset();
        CHECK(!encoder.IsFinished());
        RunOutput runAfterReset = RunInChunks(encoder, order, pcm,
            positions, 7);
        CHECK(runAfterReset.allAccepted);
        AmbisonicMovingEncoder freshEncoder;
        CHECK(freshEncoder.Configure(order, true, sampleRate,
            maxBlock, roomRadius));
        RunOutput runFresh = RunInChunks(freshEncoder, order, pcm,
            positions, 7);
        CHECK(runFresh.allAccepted);
        CHECK(runAfterReset.channels.size() == runFresh.channels.size());
        for (size_t ch = 0; ch < runAfterReset.channels.size(); ch++)
        {
            CHECK(runAfterReset.channels[ch].size()
                == runFresh.channels[ch].size());
            for (size_t i = 0; i < runAfterReset.channels[ch].size(); i++)
                CHECK_CLOSE(runAfterReset.channels[ch][i],
                    runFresh.channels[ch][i], kMaxError);
        }

        // Batched Finish with several small capacities drains the exact tail.
        std::vector<std::vector<float> > tailReset = DrainFinish(
            encoder, order, 3);
        std::vector<std::vector<float> > tailFresh = DrainFinish(
            freshEncoder, order, 7);
        for (size_t ch = 0; ch < tailReset.size(); ch++)
        {
            CHECK(tailReset[ch].size() == tailFresh[ch].size());
            for (size_t i = 0; i < tailReset[ch].size(); i++)
                CHECK_CLOSE(tailReset[ch][i], tailFresh[ch][i], kMaxError);
        }
        unsigned long long totalFrames = runAfterReset.channels[0].size()
            + tailReset[0].size();
        double expectedTotal = std::ceil((double)N
            + (double)positions.back().distance * sampleRate / kSpeedOfSound
            - 1e-12);
        CHECK((double)totalFrames == expectedTotal);
        // The padded zero is not a real sample and the last sample is never
        // replayed. Inside the final arrival interval [a(N-1), a(N)] every
        // rendered frame is (1-fraction) times the last vector interpolated
        // toward the zero tail, so the W magnitude decays strictly to zero.
        CHECK(!tailReset[0].empty());
        double lastArrivalTime = (double)(N - 1)
            + (double)positions.back().distance * sampleRate / kSpeedOfSound;
        size_t finalIntervalStart = (size_t)std::max(0.0,
            std::ceil(lastArrivalTime - 1e-12) - (double)N);
        CHECK(finalIntervalStart < tailReset[0].size());
        for (size_t i = finalIntervalStart + 1; i < tailReset[0].size(); i++)
            CHECK(std::fabs(tailReset[0][i])
                <= std::fabs(tailReset[0][i - 1]) + kMaxError);
        CHECK(std::fabs(tailReset[0].back())
            <= std::fabs(tailReset[0][finalIntervalStart]) + kMaxError);

        if (g_failed)
            return 1;
        std::printf("PASS stream checks=%d\n", g_checks);
        return 0;
    }

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s arrival|direction|doppler|stream\n",
            argv[0]);
        return 2;
    }

    std::string group = argv[1];
    if (group == "arrival")
        return ArrivalGroup();
    if (group == "direction")
        return DirectionGroup();
    if (group == "doppler")
        return DopplerGroup();
    if (group == "stream")
        return StreamGroup();

    std::fprintf(stderr, "unknown group: %s\n", group.c_str());
    return 2;
}
