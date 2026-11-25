// Tests for AudioTransportSource::notifySourceChanged().
//
// These exercise the real JUCE AudioTransportSource together with the real
// BufferingAudioSource and ResamplingAudioSource.  Thread interleaving is
// controlled with condition variables (a read barrier), not timed guesses.
// Every case checks actual rendered PCM through TASK_CHECK and returns 0.

#include "task_support/check.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

using namespace juce;

namespace
{

constexpr double kDeviceRate = 44100.0;

//==============================================================================
// Blocks exactly one source read whose range contains a target frame, after the
// source has copied its (old) snapshot.  Interleaving is driven by signals.
struct ReadBarrier
{
    void arm (int64 frameToContain)
    {
        std::lock_guard<std::mutex> lock (mutex);
        target = frameToContain;
        armed = true;
        consumed = false;
        entered = false;
        released = false;
        enteredStart = -1;
    }

    void intercept (int64 start, int numSamples)
    {
        std::unique_lock<std::mutex> lock (mutex);
        ++seenCalls;

        const bool match = armed && ! consumed
                        && start <= target && target < start + numSamples;

        if (! match)
            return;

        consumed = true;
        entered = true;
        enteredStart = start;
        condition.notify_all();

        condition.wait (lock, [this] { return released; });
    }

    bool waitEntered (int timeoutMs = 8000)
    {
        std::unique_lock<std::mutex> lock (mutex);

        return condition.wait_for (lock, std::chrono::milliseconds (timeoutMs),
                                   [this] { return entered; });
    }

    void release()
    {
        std::lock_guard<std::mutex> lock (mutex);
        released = true;
        condition.notify_all();
    }

    std::mutex mutex;
    std::condition_variable condition;
    int64 target = 0;
    int64 enteredStart = -1;
    int seenCalls = 0;
    bool armed = false;
    bool consumed = false;
    bool entered = false;
    bool released = false;
};

//==============================================================================
// A finite, addressable, non-looping memory source with per-channel content.
struct MemorySource final : public PositionableAudioSource
{
    MemorySource (int numChannels, int64 lengthFrames, float left = 0.25f, float right = -0.2f)
        : channels (numChannels), length (lengthFrames)
    {
        samples.resize ((size_t) numChannels);

        for (int c = 0; c < numChannels; ++c)
        {
            samples[(size_t) c].resize ((size_t) lengthFrames);
            const float base = (c == 0) ? left : right;

            for (int64 i = 0; i < lengthFrames; ++i)
                samples[(size_t) c][(size_t) i] = base + 0.00001f * (float) (i % 31);
        }
    }

    void prepareToPlay (int, double) override {}
    void releaseResources() override {}

    void setNextReadPosition (int64 newPosition) override
    {
        std::lock_guard<std::mutex> lock (mutex);
        position = newPosition;
    }

    int64 getNextReadPosition() const override
    {
        std::lock_guard<std::mutex> lock (mutex);
        return position;
    }

    int64 getTotalLength() const override { return length; }
    bool isLooping() const override { return false; }

    void getNextAudioBlock (const AudioSourceChannelInfo& info) override
    {
        int64 start = 0;

        {
            std::lock_guard<std::mutex> lock (mutex);
            start = position;

            for (int c = 0; c < info.buffer->getNumChannels(); ++c)
            {
                const int sourceChannel = jmin (c, channels - 1);

                for (int i = 0; i < info.numSamples; ++i)
                {
                    const int64 index = position + i;
                    float value = 0.0f;

                    if (index >= 0 && index < length)
                        value = samples[(size_t) sourceChannel][(size_t) index];

                    info.buffer->setSample (c, info.startSample + i, value);
                }
            }

            position += info.numSamples;
        }

        if (barrier != nullptr)
            barrier->intercept (start, info.numSamples);
    }

    void setRegion (int64 start, int64 count, float left, float right)
    {
        std::lock_guard<std::mutex> lock (mutex);

        for (int64 i = start; i < start + count && i < length; ++i)
        {
            if (i >= 0)
            {
                samples[0][(size_t) i] = left;

                if (channels > 1)
                    samples[1][(size_t) i] = right;
            }
        }
    }

    int channels;
    int64 length;
    std::vector<std::vector<float>> samples;
    mutable std::mutex mutex;
    int64 position = 0;
    ReadBarrier* barrier = nullptr;
};

//==============================================================================
struct PathConfig
{
    double ratio = 1.0;          // sourceRate / deviceRate
    int channels = 2;
    std::vector<int> chunks { 64 };
    int prefixChunks = 2;        // edit after this many callback chunks
    bool useBuffering = true;    // wrap in a real BufferingAudioSource we own
    int bufferSamples = 8192;
    int64 seekSourceFrame = 0;
    bool preEdited = false;      // reference: place new PCM before playback
    bool doubleEdit = false;
};

struct PathResult
{
    std::vector<float> interleaved;
    int totalFrames = 0;
    bool ready = true;
};

int64 editStartFor (const PathConfig&, int64 prefixDeviceFrames, double ratio, int64 seekFrame)
{
    const double q = (double) seekFrame + (double) prefixDeviceFrames * ratio;
    // A small negative epsilon keeps an exact-integer q on the tight boundary.
    return (int64) std::ceil (q - 1e-9) + 2;
}

// Renders one path.  For a live path it edits + notifies after the prefix.
PathResult renderPath (const PathConfig& cfg)
{
    constexpr int64 sourceLength = 32768;
    constexpr int64 editCount = 4096;
    constexpr float newLeft = -0.5f;
    constexpr float newRight = 0.9f;
    constexpr float finalLeft = -0.125f;
    constexpr float finalRight = 0.625f;

    PathResult result;

    MemorySource source (cfg.channels, sourceLength);
    auto thread = std::make_unique<TimeSliceThread> ("task-prefetch");
    thread->startThread();

    std::unique_ptr<BufferingAudioSource> buffering;
    PositionableAudioSource* transportSource = &source;

    if (cfg.useBuffering)
    {
        buffering = std::make_unique<BufferingAudioSource> (
            &source, *thread, false, cfg.bufferSamples, cfg.channels, true);
        transportSource = buffering.get();
    }

    AudioTransportSource transport;
    const double sourceRate = kDeviceRate * cfg.ratio;
    transport.setSource (transportSource, 0, nullptr, sourceRate, cfg.channels);
    transport.prepareToPlay (64, kDeviceRate);

    if (cfg.seekSourceFrame != 0)
    {
        const auto devicePosition = (int64) std::llround ((double) cfg.seekSourceFrame / cfg.ratio);
        transport.setNextReadPosition (devicePosition);
    }

    transport.start();

    int prefixFrames = 0;

    for (int i = 0; i < cfg.prefixChunks && i < (int) cfg.chunks.size(); ++i)
        prefixFrames += cfg.chunks[(size_t) i];

    const int64 editStart = editStartFor (cfg, prefixFrames, cfg.ratio, cfg.seekSourceFrame);

    if (cfg.preEdited)
    {
        source.setRegion (editStart, editCount, newLeft, newRight);

        if (cfg.doubleEdit)
            source.setRegion (editStart, editCount, finalLeft, finalRight);
    }

    int delivered = 0;

    for (int chunkIndex = 0; chunkIndex < (int) cfg.chunks.size(); ++chunkIndex)
    {
        const int numSamples = cfg.chunks[(size_t) chunkIndex];

        if (! cfg.preEdited && chunkIndex == cfg.prefixChunks)
        {
            source.setRegion (editStart, editCount, newLeft, newRight);
            transport.notifySourceChanged ({ editStart, editStart + editCount });

            if (cfg.doubleEdit)
            {
                source.setRegion (editStart, editCount, finalLeft, finalRight);
                transport.notifySourceChanged ({ editStart, editStart + editCount });
            }
        }

        if (buffering != nullptr)
        {
            const int waitSamples = cfg.ratio > 1.0001
                                      ? roundToInt ((double) numSamples * cfg.ratio) + 4
                                      : numSamples + 4;
            AudioBuffer<float> waitBuffer (cfg.channels, jmax (8, waitSamples));
            AudioSourceChannelInfo waitInfo (&waitBuffer, 0, jmax (8, waitSamples));

            if (! buffering->waitForNextAudioBlockReady (waitInfo, 8000))
                result.ready = false;
        }

        AudioBuffer<float> block (cfg.channels, numSamples);
        block.clear();
        transport.getNextAudioBlock ({ &block, 0, numSamples });

        const size_t base = result.interleaved.size();
        result.interleaved.resize (base + (size_t) numSamples * cfg.channels);

        for (int f = 0; f < numSamples; ++f)
            for (int c = 0; c < cfg.channels; ++c)
                result.interleaved[base + (size_t) f * cfg.channels + c] = block.getSample (c, f);

        delivered += numSamples;
        result.totalFrames += numSamples;
    }

    transport.setSource (nullptr);

    if (buffering != nullptr)
        buffering->releaseResources();

    thread->stopThread (4000);
    (void) delivered;
    return result;
}

double maxDifference (const PathResult& a, const PathResult& b, int channels, int fromFrame)
{
    double worst = 0.0;
    const int frames = jmin (a.totalFrames, b.totalFrames);

    for (int f = jmax (0, fromFrame); f < frames; ++f)
        for (int c = 0; c < channels; ++c)
        {
            const size_t index = (size_t) f * channels + c;
            worst = jmax (worst, (double) std::abs (a.interleaved[index] - b.interleaved[index]));
        }

    return worst;
}

int prefixFrameCount (const std::vector<int>& chunks, int prefixChunks)
{
    int sum = 0;

    for (int i = 0; i < prefixChunks && i < (int) chunks.size(); ++i)
        sum += chunks[(size_t) i];

    return sum;
}

// Renders a pre-replaced reference and checks direct + buffered edited paths.
int checkRatioPaths (double ratio, const std::vector<int>& chunks, int prefixChunks, int channels)
{
    PathConfig refCfg;
    refCfg.ratio = ratio;
    refCfg.channels = channels;
    refCfg.chunks = chunks;
    refCfg.prefixChunks = prefixChunks;
    refCfg.useBuffering = false;
    refCfg.preEdited = true;

    const PathResult reference = renderPath (refCfg);
    TASK_CHECK (reference.ready);

    for (bool useBuffer : { false, true })
    {
        PathConfig cfg = refCfg;
        cfg.preEdited = false;
        cfg.useBuffering = useBuffer;

        const PathResult got = renderPath (cfg);
        TASK_CHECK (got.ready);
        TASK_CHECK (got.totalFrames == reference.totalFrames);
        TASK_CHECK (prefixFrameCount (chunks, prefixChunks) > 0);

        const double worst = maxDifference (reference, got, channels, 0);
        TASK_CHECK (worst <= 2.0e-5);
    }

    return 0;
}

} // namespace

//==============================================================================
// prefetch group
//==============================================================================

// The exact baseline situation, but with a notification: the very next block
// already contains the new PCM (the caller does not wait or pad).
static int prefetch_internal_buffer_immediate()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    MemorySource source (1, 8192, 0.25f, 0.25f);
    auto thread = std::make_unique<TimeSliceThread> ("pf");
    thread->startThread();

    AudioTransportSource transport;
    transport.setSource (&source, 4096, thread.get(), 0.0, 1);
    transport.prepareToPlay (64, kDeviceRate);
    transport.start();

    AudioBuffer<float> first (1, 64);
    transport.getNextAudioBlock ({ &first, 0, 64 });
    TASK_CHECK (std::abs (first.getSample (0, 0) - 0.25f) < 1.0e-6f);

    source.setRegion (128, 64, -0.5f, -0.5f);
    transport.notifySourceChanged ({ 128, 192 });

    AudioBuffer<float> next (1, 192);
    transport.getNextAudioBlock ({ &next, 0, 192 });

    TASK_CHECK (std::abs (next.getSample (0, 64) - (-0.5f)) < 1.0e-5f);
    TASK_CHECK (std::abs (next.getSample (0, 127) - (-0.5f)) < 1.0e-5f);
    TASK_CHECK (std::abs (next.getSample (0, 128) - source.samples[0][192]) < 1.0e-5f);

    transport.setSource (nullptr);
    thread->stopThread (4000);
    return 0;
}

// Without a notification the historical cached behaviour is preserved.
static int prefetch_no_notification_keeps_cache()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    MemorySource source (1, 8192, 0.25f, 0.25f);
    auto thread = std::make_unique<TimeSliceThread> ("pf");
    thread->startThread();

    AudioTransportSource transport;
    transport.setSource (&source, 4096, thread.get(), 0.0, 1);
    transport.prepareToPlay (64, kDeviceRate);
    transport.start();

    AudioBuffer<float> first (1, 64);
    transport.getNextAudioBlock ({ &first, 0, 64 });

    source.setRegion (128, 64, -0.5f, -0.5f);
    // Deliberately no notifySourceChanged(): the old cached result remains.

    AudioBuffer<float> next (1, 192);
    transport.getNextAudioBlock ({ &next, 0, 192 });
    const float staleExpected = 0.25f + 0.00001f * (float) (128 % 31);
    TASK_CHECK (std::abs (next.getSample (0, 64) - staleExpected) < 1.0e-6f);

    transport.setSource (nullptr);
    thread->stopThread (4000);
    return 0;
}

// Reproduces notify() intersecting a stale read parked inside the source.
// JUCE serialises source reads against cache copies with callbackLock, so the
// sequence is: park a read, notify while it is parked, release it, then pull.
static int prefetch_barrier_inflight_read_fenced()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    constexpr int64 target = 100;

    MemorySource source (1, 16384, 0.25f, 0.25f);
    ReadBarrier barrier;
    source.barrier = &barrier;

    // Arm before the thread starts so the very first chunk is parked after
    // copying its old snapshot.
    barrier.arm (target);

    auto thread = std::make_unique<TimeSliceThread> ("barrier");
    thread->startThread();

    // prefill disabled so prepareToPlay cannot wait on the armed read.
    auto buffering = std::make_unique<BufferingAudioSource> (
        &source, *thread, false, 8192, 1, false);

    AudioTransportSource transport;
    transport.setSource (buffering.get(), 0, nullptr, 0.0, 1);
    transport.prepareToPlay (64, kDeviceRate);
    transport.start();

    TASK_CHECK (barrier.waitEntered (8000));

    // Overwrite while the old read is parked, then notify while it is still
    // parked: notify must return promptly and must not adopt that stale read.
    source.setRegion (target, 4096, -0.5f, -0.5f);
    transport.notifySourceChanged ({ target, target + 4096 });

    barrier.release();

    constexpr int lastFrame = 512;
    int delivered = 0;

    while (delivered < lastFrame)
    {
        constexpr int n = 64;

        AudioBuffer<float> waitBuffer (1, n);
        AudioSourceChannelInfo waitInfo (&waitBuffer, 0, n);
        TASK_CHECK (buffering->waitForNextAudioBlockReady (waitInfo, 8000));

        AudioBuffer<float> block (1, n);
        block.clear();
        transport.getNextAudioBlock ({ &block, 0, n });

        for (int i = 0; i < n; ++i)
        {
            const int64 frame = delivered + i;
            const float value = block.getSample (0, i);

            if (frame >= target)
                TASK_CHECK (std::abs (value + 0.5f) <= 2.0e-5f);
            else
            {
                const float original = 0.25f + 0.00001f * (float) (frame % 31);
                TASK_CHECK (std::abs (value - original) <= 2.0e-5f);
            }
        }

        delivered += n;
    }

    transport.setSource (nullptr);
    buffering->releaseResources();
    thread->stopThread (4000);
    return 0;
}

// Direct source vs real read-ahead path with asymmetric stereo content.
static int prefetch_direct_vs_buffered_stereo()
{
    return checkRatioPaths (1.0, { 64, 64, 128, 192, 64, 64 }, 2, 2);
}

//==============================================================================
// resampling group
//==============================================================================

static int resampling_ratio_3_over_2_prefilter()
{
    // Tiny/odd chunks exercise the down-sampling input pre-filter and buffer
    // growth; the edit sits on the tight legal boundary ceil(q)+2.
    return checkRatioPaths (1.5, { 1, 2, 3, 7, 64, 192, 3, 1, 128, 64, 32, 16 }, 4, 2);
}

static int resampling_ratio_2_over_3_postfilter()
{
    return checkRatioPaths (2.0 / 3.0, { 3, 1, 64, 7, 192, 2, 2, 128, 64, 5 }, 3, 2);
}

static int resampling_mono_3_over_2_boundary()
{
    return checkRatioPaths (1.5, { 1, 1, 1, 1, 1, 1, 64, 64, 64 }, 6, 1);
}

static int resampling_chunk_independence()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    PathConfig a;
    a.ratio = 1.5;
    a.channels = 2;
    a.chunks = { 64, 64, 64, 64, 64, 64, 64, 64 };
    a.prefixChunks = 2;

    const PathResult ra = renderPath (a);
    TASK_CHECK (ra.ready);

    PathConfig refA = a;
    refA.preEdited = true;
    refA.useBuffering = false;
    const PathResult referenceA = renderPath (refA);
    TASK_CHECK (maxDifference (referenceA, ra, 2, 0) <= 2.0e-5);

    PathConfig b = a;
    b.chunks = { 17, 13, 101, 3, 71, 59, 128, 47, 61, 12 };

    const PathResult rb = renderPath (b);
    TASK_CHECK (rb.ready);

    PathConfig refB = b;
    refB.preEdited = true;
    refB.useBuffering = false;
    const PathResult referenceB = renderPath (refB);
    TASK_CHECK (maxDifference (referenceB, rb, 2, 0) <= 2.0e-5);

    return 0;
}

//==============================================================================
// continuity group
//==============================================================================

static int continuity_no_skip_no_repeat()
{
    for (double ratio : { 1.0, 1.5, 2.0 / 3.0 })
    {
        const int rc = checkRatioPaths (ratio, { 32, 32, 32, 200, 17, 64, 128 }, 3, 2);
        TASK_CHECK (rc == 0);
    }

    return 0;
}

static int continuity_two_edits_last_wins()
{
    PathConfig refCfg;
    refCfg.ratio = 1.5;
    refCfg.channels = 2;
    refCfg.chunks = { 64, 64, 128, 128, 64, 64 };
    refCfg.prefixChunks = 2;
    refCfg.doubleEdit = true;
    refCfg.preEdited = true;
    refCfg.useBuffering = false;

    const PathResult reference = renderPath (refCfg);
    TASK_CHECK (reference.ready);

    for (bool useBuffer : { false, true })
    {
        PathConfig got = refCfg;
        got.preEdited = false;
        got.useBuffering = useBuffer;

        const PathResult result = renderPath (got);
        TASK_CHECK (result.ready);
        TASK_CHECK (maxDifference (reference, result, 2, 0) <= 2.0e-5);
    }

    return 0;
}

static int continuity_empty_range_noop()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    MemorySource source (1, 4096);
    AudioTransportSource transport;
    transport.setSource (&source, 0, nullptr, 0.0, 1);
    transport.prepareToPlay (64, kDeviceRate);
    transport.start();

    transport.notifySourceChanged ({});
    transport.notifySourceChanged ({ 100, 100 });

    AudioBuffer<float> block (1, 64);
    transport.getNextAudioBlock ({ &block, 0, 64 });
    TASK_CHECK (std::abs (block.getSample (0, 0) - 0.25f) < 1.0e-6f);

    transport.setSource (nullptr);
    return 0;
}

// Explicit positioning and pause retain their original semantics.
static int continuity_seek_and_pause_unchanged()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    MemorySource source (1, 8192, 0.25f, 0.25f);
    source.setRegion (512, 64, -0.75f, -0.75f);

    AudioTransportSource transport;
    transport.setSource (&source, 0, nullptr, 0.0, 1);
    transport.prepareToPlay (64, kDeviceRate);

    TASK_CHECK (! transport.isPlaying());
    transport.setNextReadPosition (512);
    TASK_CHECK (std::llabs (transport.getNextReadPosition() - 512) <= 1);

    transport.start();
    TASK_CHECK (transport.isPlaying());

    AudioBuffer<float> atSeek (1, 16);
    transport.getNextAudioBlock ({ &atSeek, 0, 16 });
    TASK_CHECK (std::abs (atSeek.getSample (0, 0) + 0.75f) < 1.0e-5f);

    transport.stop();
    TASK_CHECK (! transport.isPlaying());

    transport.setPosition (0.0);
    TASK_CHECK (transport.getCurrentPosition() < 1.0e-6);

    transport.setSource (nullptr);
    return 0;
}

//==============================================================================
const struct task_case task_cases[] =
{
    { "prefetch",    "internal_buffer_immediate",   prefetch_internal_buffer_immediate },
    { "prefetch",    "no_notification_keeps_cache", prefetch_no_notification_keeps_cache },
    { "prefetch",    "barrier_inflight_read_fenced", prefetch_barrier_inflight_read_fenced },
    { "prefetch",    "direct_vs_buffered_stereo",   prefetch_direct_vs_buffered_stereo },

    { "resampling",  "ratio_3_over_2_prefilter",     resampling_ratio_3_over_2_prefilter },
    { "resampling",  "ratio_2_over_3_postfilter",   resampling_ratio_2_over_3_postfilter },
    { "resampling",  "mono_3_over_2_boundary",      resampling_mono_3_over_2_boundary },
    { "resampling",  "chunk_independence",           resampling_chunk_independence },

    { "continuity",  "no_skip_no_repeat",            continuity_no_skip_no_repeat },
    { "continuity",  "two_edits_last_wins",          continuity_two_edits_last_wins },
    { "continuity",  "empty_range_noop",             continuity_empty_range_noop },
    { "continuity",  "seek_and_pause_unchanged",     continuity_seek_and_pause_unchanged },
};

const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases[0]);
