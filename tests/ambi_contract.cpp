// Offline Ambisonics contract test.
//
// Everything here goes through the public AL/ALC loopback API: a 48 kHz 3D
// B-Format float loopback device renders synthesized B-Format buffers, and the
// PCM is compared sample-by-sample against the independent double-precision
// reference in ambi_reference.cpp. No audio device, files or player UI are
// involved.
//
// Four groups are reported, each printing:
//   PASS <group> cases=<N> max_error=<...>
// Any failed assertion makes the program return non-zero.
#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <string>
#include <vector>

#include "ambi_reference.hpp"

namespace {

using ambiref::AmbiReference;
using ambiref::DirA;
using ambiref::InputRep;
using ambiref::Orientation;

constexpr int kSampleRate = 48000;
constexpr int kSettleFrames = 2048;
constexpr double kAbsTolerance = 2e-4;
constexpr float kSentinel = 0.7531974f;
constexpr double kPi = std::numbers::pi;

struct Failure {
    bool hit{false};
    std::string first;
};

[[noreturn]] void fatal(const char *what)
{
    std::fprintf(stderr, "FATAL: %s (AL=0x%x ALC=0x%x)\n", what, alGetError(),
        alcGetError(nullptr));
    std::exit(2);
}

void requireAl(bool condition, const char *what, Failure &fail)
{
    if(condition) return;
    fail.hit = true;
    if(fail.first.empty()) fail.first = std::string{"API error: "} + what;
    std::fprintf(stderr, "FAIL %s (AL=0x%x)\n", what, alGetError());
}

// A signed directional field. The direction is fixed in the source's local
// OpenAL frame (+X right, +Y up, -Z forward); none of the directions lie on
// a coordinate axis or the equator, and the mixed signs let every spherical
// harmonic component of each order be distinguished.
struct Field {
    double sign;
    std::array<double,3> localDir;
    double amplitude;
    double frequency;
    double phase;
};

struct Signal {
    double bedAmplitude{0.02};
    double bedFrequency{220.0};
    std::vector<Field> fields;
};

auto makeSignal() -> Signal
{
    Signal sig;
    sig.fields = {
        Field{+1.0, ambiref::normalized({0.55, 0.37, -0.75}), 0.025, 173.3, 0.3},
        Field{-1.0, ambiref::normalized({-0.62, 0.44, 0.65}), 0.022, 91.7, 1.7},
        Field{+1.0, ambiref::normalized({0.31, -0.58, -0.75}), 0.020, 263.1, 2.9},
    };
    return sig;
}

auto envelope(const Field &f, int frame) -> double
{ return f.amplitude * std::sin(2.0*kPi*f.frequency*frame/kSampleRate + f.phase); }

// Expected N3D/ACN sample for one channel at one frame, derived purely from
// the geometrically transformed directions.
auto expectedSample(const AmbiReference &ref, const Signal &sig, std::size_t channel, int frame,
    const Orientation &source, const Orientation &listener, bool headRelative) -> double
{
    double value = (channel == 0)
        ? sig.bedAmplitude*std::sin(2.0*kPi*sig.bedFrequency*frame/kSampleRate) : 0.0;
    for(const Field &f : sig.fields)
    {
        const DirA world = AmbiReference::rotateDir(f.localDir, source, listener, headRelative);
        value += f.sign * envelope(f, frame) * ref.realSH(channel, world);
    }
    return value;
}

// Builds the input buffer PCM for a given representation: synthesizes the N3D
// field, then applies the inverse scaling and the FuMa channel permutation.
auto buildInput(const AmbiReference &ref, const Signal &sig, int frames, InputRep rep,
    const Orientation &source, const Orientation &listener, bool headRelative)
    -> std::vector<float>
{
    const std::size_t channels = ref.channels();
    auto data = std::vector<float>(static_cast<std::size_t>(frames)*channels, 0.0f);
    for(int n=0; n<frames; ++n)
    {
        for(std::size_t acn=0; acn<channels; ++acn)
        {
            const double n3d = expectedSample(ref, sig, acn, n, source, listener, headRelative);
            const std::size_t slot = AmbiReference::inputSlot(rep, acn);
            data[static_cast<std::size_t>(n)*channels + slot] =
                static_cast<float>(n3d * AmbiReference::inputScale(rep, acn));
        }
    }
    return data;
}

// RAII wrapper around a loopback device rendering B-Format 3D float PCM.
struct Loopback {
    ALCdevice *device{nullptr};
    ALCcontext *context{nullptr};
    int order{0};
    std::size_t channels{0};

    Loopback() = default;
    Loopback(const Loopback&) = delete;
    auto operator=(const Loopback&) -> Loopback& = delete;

    explicit Loopback(int ambiOrder)
    {
        order = ambiOrder;
        channels = static_cast<std::size_t>((order+1)*(order+1));
        const ALCint attrs[] = {
            ALC_FREQUENCY, kSampleRate,
            ALC_FORMAT_CHANNELS_SOFT, ALC_BFORMAT3D_SOFT,
            ALC_FORMAT_TYPE_SOFT, ALC_FLOAT_SOFT,
            ALC_AMBISONIC_LAYOUT_SOFT, ALC_ACN_SOFT,
            ALC_AMBISONIC_SCALING_SOFT, ALC_N3D_SOFT,
            ALC_AMBISONIC_ORDER_SOFT, order,
            ALC_OUTPUT_LIMITER_SOFT, ALC_FALSE,
            0};
        device = alcLoopbackOpenDeviceSOFT(nullptr);
        if(!device) fatal("loopback open");
        context = alcCreateContext(device, attrs);
        if(!context || !alcMakeContextCurrent(context)) fatal("context create");
        if(!alIsExtensionPresent("AL_SOFT_bformat_ex")) fatal("bformat extension missing");
        alDistanceModel(AL_NONE);
    }

    ~Loopback()
    {
        if(context)
        {
            alcMakeContextCurrent(nullptr);
            alcDestroyContext(context);
        }
        if(device) alcCloseDevice(device);
    }

    void renderInto(float *data, int frames)
    {
        alcRenderSamplesSOFT(device, data, frames);
        if(alcGetError(device) != ALC_NO_ERROR) fatal("render");
    }
};

void setBufferFormat(ALuint buffer, int order, InputRep rep)
{
    alBufferi(buffer, AL_UNPACK_AMBISONIC_ORDER_SOFT, order);
    alBufferi(buffer, AL_AMBISONIC_LAYOUT_SOFT,
        rep == InputRep::FuMa ? AL_FUMA_SOFT : AL_ACN_SOFT);
    alBufferi(buffer, AL_AMBISONIC_SCALING_SOFT,
        rep == InputRep::FuMa ? AL_FUMA_SOFT
        : rep == InputRep::AcnSn3d ? AL_SN3D_SOFT : AL_N3D_SOFT);
    if(alGetError() != AL_NO_ERROR) fatal("buffer format");
}

void makeBuffer(ALuint buffer, int order, InputRep rep, const std::vector<float> &data)
{
    setBufferFormat(buffer, order, rep);
    alBufferData(buffer, AL_FORMAT_BFORMAT3D_FLOAT32, data.data(),
        static_cast<ALsizei>(data.size()*sizeof(float)), kSampleRate);
    if(alGetError() != AL_NO_ERROR) fatal("buffer data");
}

void checkSanity(const std::vector<float> &pcm, std::size_t channels, int expectedFrames,
    const char *scenario, Failure &fail)
{
    if(static_cast<int>(pcm.size()/channels) != expectedFrames || pcm.size()%channels != 0)
    {
        fail.hit = true;
        if(fail.first.empty()) fail.first = std::string{scenario} + ": bad output length";
    }
    for(std::size_t i=0; i<pcm.size(); ++i)
    {
        if(!std::isfinite(pcm[i]))
        {
            fail.hit = true;
            if(fail.first.empty())
                fail.first = std::string{scenario} + ": non-finite sample at index "
                    + std::to_string(i);
            return;
        }
        if(pcm[i] == kSentinel)
        {
            fail.hit = true;
            if(fail.first.empty())
                fail.first = std::string{scenario} + ": unwritten sentinel at index "
                    + std::to_string(i);
            return;
        }
    }
}

// Compares a stable frame window against the geometric expectation. Records
// the first mismatch and the worst absolute error.
auto compareWindow(std::size_t channels, const AmbiReference &ref, const Signal &sig,
    const std::vector<float> &pcm, int begin, int end, const Orientation &source,
    const Orientation &listener, bool headRelative, const char *scenario, Failure &fail)
    -> double
{
    double maxError = 0.0;
    for(int n=begin; n<end; ++n)
    {
        for(std::size_t c=0; c<channels; ++c)
        {
            const double want = expectedSample(ref, sig, c, n, source, listener, headRelative);
            const float got = pcm[static_cast<std::size_t>(n)*channels + c];
            const double err = std::abs(static_cast<double>(got) - want);
            maxError = std::max(maxError, err);
            if(err > kAbsTolerance && fail.first.empty())
            {
                fail.hit = true;
                fail.first = std::string{scenario} + ": frame=" + std::to_string(n)
                    + " channel=" + std::to_string(c) + " expected=" + std::to_string(want)
                    + " actual=" + std::to_string(got);
            }
        }
    }
    return maxError;
}

// Rotation helpers: intrinsic yaw (about +Y/up), then pitch (about the
// current right axis), then roll (about the current at axis). The resulting
// at/up pair is the only thing the reference and the library both consume, so
// the convention stays self-consistent.
auto rotateVec(const std::array<double,3> &v, const std::array<double,3> &axis, double angle)
    -> std::array<double,3>
{
    const double c = std::cos(angle), s = std::sin(angle);
    const double d = ambiref::dot3(axis, v);
    const std::array<double,3> cr = ambiref::cross3(axis, v);
    return ambiref::normalized({
        v[0]*c + cr[0]*s + axis[0]*d*(1.0-c),
        v[1]*c + cr[1]*s + axis[1]*d*(1.0-c),
        v[2]*c + cr[2]*s + axis[2]*d*(1.0-c)});
}

auto eulerOrientation(double yaw, double pitch, double roll) -> Orientation
{
    std::array<double,3> at{0.0, 0.0, -1.0};
    std::array<double,3> up{0.0, 1.0, 0.0};
    at = rotateVec(at, up, yaw);
    std::array<double,3> right = ambiref::normalized(ambiref::cross3(at, up));
    up = rotateVec(up, right, pitch);
    at = rotateVec(at, right, pitch);
    at = rotateVec(at, at, roll);
    up = rotateVec(up, at, roll);
    return Orientation{at, ambiref::normalized(up)};
}

void applyOrientation(ALuint source, const Orientation &sourceOrient,
    const Orientation &listenerOrient, bool headRelative)
{
    const std::array<ALfloat,6> listenerVals{
        static_cast<ALfloat>(listenerOrient.at[0]),
        static_cast<ALfloat>(listenerOrient.at[1]),
        static_cast<ALfloat>(listenerOrient.at[2]),
        static_cast<ALfloat>(listenerOrient.up[0]),
        static_cast<ALfloat>(listenerOrient.up[1]),
        static_cast<ALfloat>(listenerOrient.up[2])};
    alListenerfv(AL_ORIENTATION, listenerVals.data());
    const std::array<ALfloat,6> sourceVals{
        static_cast<ALfloat>(sourceOrient.at[0]),
        static_cast<ALfloat>(sourceOrient.at[1]),
        static_cast<ALfloat>(sourceOrient.at[2]),
        static_cast<ALfloat>(sourceOrient.up[0]),
        static_cast<ALfloat>(sourceOrient.up[1]),
        static_cast<ALfloat>(sourceOrient.up[2])};
    alSourcefv(source, AL_ORIENTATION, sourceVals.data());
    alSourcei(source, AL_SOURCE_RELATIVE, headRelative ? AL_TRUE : AL_FALSE);
}

const Orientation kDefaultOrient{};

// ---------------------------------------------------------------------------
// Group 1: input format conversion. The same physical field is fed through
// FuMa, ACN/SN3D and ACN/N3D buffers and must render identically.
// ---------------------------------------------------------------------------
auto runNormalization(Failure &fail) -> int
{
    struct Case { int order; InputRep rep; const char *name; };
    const Case cases[] = {
        {1, InputRep::FuMa,    "order1 FuMa"},
        {1, InputRep::AcnSn3d, "order1 ACN/SN3D"},
        {1, InputRep::AcnN3d,  "order1 ACN/N3D"},
        {2, InputRep::AcnSn3d, "order2 ACN/SN3D"},
        {2, InputRep::AcnN3d,  "order2 ACN/N3D"},
        {3, InputRep::AcnSn3d, "order3 ACN/SN3D"},
        {3, InputRep::AcnN3d,  "order3 ACN/N3D"},
    };

    constexpr int frames = 4096;
    const Signal sig = makeSignal();
    double groupMax = 0.0;
    int passed = 0;
    for(const Case &tc : cases)
    {
        const bool failedBefore = fail.hit;
        AmbiReference ref(tc.order);
        const std::vector<float> input = buildInput(ref, sig, frames, tc.rep,
            kDefaultOrient, kDefaultOrient, /*headRelative=*/true);
        Loopback lb(tc.order);
        ALuint buffer{0}, source{0};
        alGenBuffers(1, &buffer);
        alGenSources(1, &source);
        makeBuffer(buffer, tc.order, tc.rep, input);
        alSourcei(source, AL_BUFFER, static_cast<ALint>(buffer));
        alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
        alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);

        auto pcm = std::vector<float>(static_cast<std::size_t>(frames)*lb.channels, kSentinel);
        alSourcePlay(source);
        requireAl(alGetError() == AL_NO_ERROR, "source play", fail);
        lb.renderInto(pcm.data(), frames);
        checkSanity(pcm, lb.channels, frames, tc.name, fail);
        groupMax = std::max(groupMax, compareWindow(lb.channels, ref, sig, pcm, kSettleFrames,
            frames, kDefaultOrient, kDefaultOrient, true, tc.name, fail));
        if(fail.hit == failedBefore) ++passed;
        alDeleteSources(1, &source);
        alDeleteBuffers(1, &buffer);
    }
    std::printf("PASS normalization cases=%d max_error=%.9g\n", passed, groupMax);
    return passed;
}

// ---------------------------------------------------------------------------
// Group 2: rotation must land in the audio. Yaw, pitch and roll are combined
// with angles whose application order does not commute; the expected field is
// obtained by geometrically rotating each direction and re-evaluating the
// spherical harmonics.
// ---------------------------------------------------------------------------
auto runRotation(Failure &fail) -> int
{
    struct Case {
        int order;
        bool headRelative;
        Orientation source;
        Orientation listener;
        const char *name;
    };
    const Orientation rotatedSource = eulerOrientation(-50.0*kPi/180.0, 20.0*kPi/180.0,
        -30.0*kPi/180.0);
    const Orientation rotatedListener = eulerOrientation(35.0*kPi/180.0, -25.0*kPi/180.0,
        40.0*kPi/180.0);
    const Case cases[] = {
        {3, false, rotatedSource, rotatedListener, "order3 world rotated"},
        {3, true,  rotatedSource, rotatedListener, "order3 head-relative rotated"},
        {1, false, rotatedSource, rotatedListener, "order1 world rotated"},
        {2, true,  rotatedSource, rotatedListener, "order2 head-relative, listener turned"},
    };

    constexpr int frames = 4096;
    const Signal sig = makeSignal();
    double groupMax = 0.0;
    int passed = 0;
    for(const Case &tc : cases)
    {
        const bool failedBefore = fail.hit;
        AmbiReference ref(tc.order);
        // The buffer carries the local (unrotated) field; the library performs
        // the rotation from the source/listener orientation.
        const std::vector<float> input = buildInput(ref, sig, frames, InputRep::AcnN3d,
            kDefaultOrient, kDefaultOrient, true);
        Loopback lb(tc.order);
        ALuint buffer{0}, source{0};
        alGenBuffers(1, &buffer);
        alGenSources(1, &source);
        makeBuffer(buffer, tc.order, InputRep::AcnN3d, input);
        alSourcei(source, AL_BUFFER, static_cast<ALint>(buffer));
        alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);
        applyOrientation(source, tc.source, tc.listener, tc.headRelative);

        auto pcm = std::vector<float>(static_cast<std::size_t>(frames)*lb.channels, kSentinel);
        alSourcePlay(source);
        requireAl(alGetError() == AL_NO_ERROR, "source play", fail);
        lb.renderInto(pcm.data(), frames);
        checkSanity(pcm, lb.channels, frames, tc.name, fail);
        groupMax = std::max(groupMax, compareWindow(lb.channels, ref, sig, pcm, kSettleFrames,
            frames, tc.source, tc.listener, tc.headRelative, tc.name, fail));
        if(fail.hit == failedBefore) ++passed;
        alDeleteSources(1, &source);
        alDeleteBuffers(1, &buffer);
    }
    std::printf("PASS rotation cases=%d max_error=%.9g\n", passed, groupMax);
    return passed;
}

// Renders a queued three-buffer source, changing source and listener
// orientation at non-boundary frames while rendering continues across queue
// boundaries.
struct LiveRun {
    std::vector<float> pcm;
    int totalFrames{0};
    int processedAtEnd{0};
    ALint finalState{0};
};

auto renderLive(int order, const Signal &sig, const std::array<int,3> &bufferLengths,
    const Orientation sourceOrients[3], const Orientation listenerOrients[3],
    int updateFrame1, int updateFrame2, bool headRelative, Failure &fail) -> LiveRun
{
    AmbiReference ref(order);
    const int total = bufferLengths[0] + bufferLengths[1] + bufferLengths[2];
    Loopback lb(order);
    ALuint buffers[3]{};
    ALuint source{0};
    alGenBuffers(3, buffers);
    alGenSources(1, &source);
    int offset = 0;
    for(int i=0; i<3; ++i)
    {
        auto part = std::vector<float>(static_cast<std::size_t>(bufferLengths[i])*lb.channels);
        for(int n=0; n<bufferLengths[i]; ++n)
            for(std::size_t c=0; c<lb.channels; ++c)
                part[static_cast<std::size_t>(n)*lb.channels + c] = static_cast<float>(
                    expectedSample(ref, sig, c, offset+n, kDefaultOrient, kDefaultOrient, true));
        makeBuffer(buffers[i], order, InputRep::AcnN3d, part);
        offset += bufferLengths[i];
    }
    alSourceQueueBuffers(source, 3, buffers);
    alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);
    applyOrientation(source, sourceOrients[0], listenerOrients[0], headRelative);

    LiveRun run;
    run.totalFrames = total;
    constexpr int tail = 1024;
    run.pcm.assign(static_cast<std::size_t>(total + tail)*lb.channels, kSentinel);
    alSourcePlay(source);
    requireAl(alGetError() == AL_NO_ERROR, "queued play", fail);

    int rendered = 0;
    constexpr int chunk = 512;
    int applied = 0;
    while(rendered < total + tail)
    {
        const int now = std::min(chunk, total + tail - rendered);
        lb.renderInto(run.pcm.data() + static_cast<std::size_t>(rendered)*lb.channels, now);
        const int windowStart = rendered;
        rendered += now;
        // Property changes take effect at the next mixer update; apply them as
        // soon as the target frame is inside the chunk just rendered.
        if(applied < 1 && windowStart < updateFrame1 && rendered >= updateFrame1)
        {
            applyOrientation(source, sourceOrients[1], listenerOrients[1], headRelative);
            applied = 1;
        }
        if(applied < 2 && windowStart < updateFrame2 && rendered >= updateFrame2)
        {
            applyOrientation(source, sourceOrients[2], listenerOrients[2], headRelative);
            applied = 2;
        }
    }

    ALint processed{0};
    alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
    run.processedAtEnd = processed;
    alGetSourcei(source, AL_SOURCE_STATE, &run.finalState);

    alSourceUnqueueBuffers(source, 3, buffers);
    alDeleteSources(1, &source);
    alDeleteBuffers(3, buffers);
    return run;
}

auto runLiveUpdate(Failure &fail) -> int
{
    const Signal sig = makeSignal();
    const std::array<int,3> lengths{3000, 2500, 3500}; // unequal, total 9000
    constexpr int update1 = 3200; // inside buffer 2, not on a boundary
    constexpr int update2 = 6100; // inside buffer 3, not on a boundary
    struct Case {
        int order;
        bool headRelative;
        const char *name;
    };
    const Case cases[] = {
        {3, false, "order3 world live updates"},
        {2, true,  "order2 head-relative live updates"},
    };

    double groupMax = 0.0;
    int passed = 0;
    for(const Case &tc : cases)
    {
        const bool failedBefore = fail.hit;
        AmbiReference ref(tc.order);
        const Orientation sourceOrients[3]{
            eulerOrientation(15.0*kPi/180.0, 10.0*kPi/180.0, 5.0*kPi/180.0),
            eulerOrientation(-35.0*kPi/180.0, 25.0*kPi/180.0, -20.0*kPi/180.0),
            eulerOrientation(55.0*kPi/180.0, -15.0*kPi/180.0, 30.0*kPi/180.0)};
        // Distinct listener sequence so dropping the listener transform is
        // detectable on its own.
        const Orientation listenerOrients[3]{
            eulerOrientation(-10.0*kPi/180.0, 8.0*kPi/180.0, 12.0*kPi/180.0),
            eulerOrientation(42.0*kPi/180.0, -18.0*kPi/180.0, -28.0*kPi/180.0),
            eulerOrientation(-48.0*kPi/180.0, 22.0*kPi/180.0, 16.0*kPi/180.0)};
        LiveRun run = renderLive(tc.order, sig, lengths, sourceOrients, listenerOrients,
            update1, update2, tc.headRelative, fail);

        const std::string scenario = tc.name;
        checkSanity(run.pcm, ref.channels(), run.totalFrames + 1024, scenario.c_str(), fail);
        if(run.processedAtEnd != 3)
        {
            fail.hit = true;
            if(fail.first.empty())
                fail.first = scenario + ": expected 3 processed buffers, got "
                    + std::to_string(run.processedAtEnd);
        }
        if(run.finalState != AL_STOPPED)
        {
            fail.hit = true;
            if(fail.first.empty())
                fail.first = scenario + ": source did not stop, state="
                    + std::to_string(run.finalState);
        }

        // Stable windows leave 2048 frames after each play/update and stay
        // clear of the following transition and the queue boundaries.
        struct Window { int begin, end; int orientIndex; };
        const Window windows[] = {
            {kSettleFrames, update1, 0},
            {update1 + kSettleFrames, update2, 1},
            {update2 + kSettleFrames, run.totalFrames, 2},
        };
        for(const Window &w : windows)
        {
            groupMax = std::max(groupMax, compareWindow(ref.channels(), ref, sig, run.pcm,
                w.begin, w.end, sourceOrients[w.orientIndex], listenerOrients[w.orientIndex],
                tc.headRelative, scenario.c_str(), fail));
        }

        // Static control: the same signal as one uninterrupted buffer with the
        // initial orientation held for the whole render.
        const std::vector<float> staticInput = buildInput(ref, sig, run.totalFrames,
            InputRep::AcnN3d, kDefaultOrient, kDefaultOrient, true);
        Loopback control(tc.order);
        ALuint buffer{0}, source{0};
        alGenBuffers(1, &buffer);
        alGenSources(1, &source);
        makeBuffer(buffer, tc.order, InputRep::AcnN3d, staticInput);
        alSourcei(source, AL_BUFFER, static_cast<ALint>(buffer));
        alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);
        applyOrientation(source, sourceOrients[0], listenerOrients[0], tc.headRelative);
        auto controlPcm = std::vector<float>(static_cast<std::size_t>(run.totalFrames)
            *control.channels, kSentinel);
        alSourcePlay(source);
        control.renderInto(controlPcm.data(), run.totalFrames);
        checkSanity(controlPcm, control.channels, run.totalFrames,
            (scenario + " static control").c_str(), fail);
        // The first stable segment of the queued render and the static render
        // must be the same field with continuous phase.
        for(int n=kSettleFrames; n<update1; ++n)
            for(std::size_t c=0; c<ref.channels(); ++c)
            {
                const float a = run.pcm[static_cast<std::size_t>(n)*ref.channels() + c];
                const float b = controlPcm[static_cast<std::size_t>(n)*ref.channels() + c];
                const double err = std::abs(static_cast<double>(a) - static_cast<double>(b));
                groupMax = std::max(groupMax, err);
                if(err > kAbsTolerance && fail.first.empty())
                {
                    fail.hit = true;
                    fail.first = scenario + ": static control diverges at frame="
                        + std::to_string(n) + " channel=" + std::to_string(c);
                }
            }
        alDeleteSources(1, &source);
        alDeleteBuffers(1, &buffer);
        if(fail.hit == failedBefore) ++passed;
    }
    std::printf("PASS live-update cases=%d max_error=%.9g\n", passed, groupMax);
    return passed;
}

// ---------------------------------------------------------------------------
// Group 4: queue handoff. Phase must continue across the three buffer
// boundaries, the processed-buffer counters must advance in order, and the
// source must stop after the last buffer.
// ---------------------------------------------------------------------------
auto runQueue(Failure &fail) -> int
{
    const Signal sig = makeSignal();
    const std::array<int,3> lengths{2800, 3300, 2900}; // unequal, total 9000
    struct Case { int order; const char *name; };
    const Case cases[] = {
        {3, "order3 queue handoff"},
        {1, "order1 queue handoff"},
    };

    double groupMax = 0.0;
    int passed = 0;
    for(const Case &tc : cases)
    {
        const bool failedBefore = fail.hit;
        AmbiReference ref(tc.order);
        const int total = lengths[0] + lengths[1] + lengths[2];
        Loopback lb(tc.order);
        ALuint buffers[3]{};
        ALuint source{0};
        alGenBuffers(3, buffers);
        alGenSources(1, &source);
        int offset = 0;
        for(int i=0; i<3; ++i)
        {
            std::vector<float> part(static_cast<std::size_t>(lengths[i])*lb.channels);
            for(int n=0; n<lengths[i]; ++n)
                for(std::size_t c=0; c<lb.channels; ++c)
                    part[static_cast<std::size_t>(n)*lb.channels + c] = static_cast<float>(
                        expectedSample(ref, sig, c, offset+n, kDefaultOrient, kDefaultOrient,
                            true));
            makeBuffer(buffers[i], tc.order, InputRep::AcnN3d, part);
            offset += lengths[i];
        }
        alSourceQueueBuffers(source, 3, buffers);
        alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
        alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);

        constexpr int tail = 1024;
        auto pcm = std::vector<float>(static_cast<std::size_t>(total + tail)*lb.channels,
            kSentinel);
        alSourcePlay(source);
        requireAl(alGetError() == AL_NO_ERROR, "queue play", fail);

        const int boundaries[2]{lengths[0], lengths[0] + lengths[1]};
        int seenAtFrame[3]{-1, -1, -1};
        int rendered = 0;
        constexpr int chunk = 256;
        while(rendered < total + tail)
        {
            const int now = std::min(chunk, total + tail - rendered);
            lb.renderInto(pcm.data() + static_cast<std::size_t>(rendered)*lb.channels, now);
            rendered += now;
            ALint processed{0};
            alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
            for(int p=0; p<processed && p<3; ++p)
                if(seenAtFrame[p] < 0) seenAtFrame[p] = rendered;
        }
        ALint state{0};
        alGetSourcei(source, AL_SOURCE_STATE, &state);
        checkSanity(pcm, lb.channels, total + tail, tc.name, fail);
        if(seenAtFrame[0] < 0 || seenAtFrame[1] <= seenAtFrame[0]
            || seenAtFrame[2] <= seenAtFrame[1])
        {
            fail.hit = true;
            if(fail.first.empty())
                fail.first = std::string{tc.name} + ": processed counters out of order ("
                    + std::to_string(seenAtFrame[0]) + "," + std::to_string(seenAtFrame[1])
                    + "," + std::to_string(seenAtFrame[2]) + ")";
        }
        for(int b=0; b<2; ++b)
        {
            // A buffer must be marked processed near its boundary (allowing
            // for the mixer update period), not thousands of frames early.
            if(seenAtFrame[b] < 0 || seenAtFrame[b] > boundaries[b] + 2048)
            {
                fail.hit = true;
                if(fail.first.empty())
                    fail.first = std::string{tc.name} + ": buffer processed at wrong frame "
                        + std::to_string(seenAtFrame[b]);
            }
        }
        if(state != AL_STOPPED)
        {
            fail.hit = true;
            if(fail.first.empty())
                fail.first = std::string{tc.name} + ": expected AL_STOPPED, got "
                    + std::to_string(state);
        }

        // Compare windows straddling each queue boundary against the geometric
        // expectation: any restart, sample rewind or dropped frames show up
        // here. The mixer only ramps at (re)start, so boundary neighborhoods
        // must match within tolerance.
        const int spans[2][2]{
            {boundaries[0] - 256, boundaries[0] + 256},
            {boundaries[1] - 256, boundaries[1] + 256}};
        for(const auto &span : spans)
            groupMax = std::max(groupMax, compareWindow(lb.channels, ref, sig, pcm, span[0],
                span[1], kDefaultOrient, kDefaultOrient, true, tc.name, fail));
        groupMax = std::max(groupMax, compareWindow(lb.channels, ref, sig, pcm, kSettleFrames,
            total - 256, kDefaultOrient, kDefaultOrient, true, tc.name, fail));

        alSourceUnqueueBuffers(source, 3, buffers);
        alDeleteSources(1, &source);
        alDeleteBuffers(3, buffers);
        if(fail.hit == failedBefore) ++passed;
    }
    std::printf("PASS queue cases=%d max_error=%.9g\n", passed, groupMax);
    return passed;
}

} // namespace

int main()
{
    Failure fail;
    try
    {
        AmbiReference::selfTest();
        std::puts("REFERENCE_SELFTEST OK (constant term and axial first-order signs)");
    }
    catch(const std::exception &e)
    {
        std::fprintf(stderr, "FATAL: %s\n", e.what());
        return 2;
    }

    const int n1 = runNormalization(fail);
    const int n2 = runRotation(fail);
    const int n3 = runLiveUpdate(fail);
    const int n4 = runQueue(fail);

    if(n1 <= 0 || n2 <= 0 || n3 <= 0 || n4 <= 0)
    {
        std::fprintf(stderr, "FATAL: every group must contain at least one passing case\n");
        return 2;
    }
    if(fail.hit)
    {
        std::fprintf(stderr, "FIRST_MISMATCH %s\n", fail.first.c_str());
        return 1;
    }
    std::puts("AMBI_CONTRACT_OK");
    return 0;
}
