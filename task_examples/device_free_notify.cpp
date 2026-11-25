// =============================================================================
//  Device-free playback example for AudioTransportSource::notifySourceChanged()
//
//  This is a complete C++17 program: it opens no audio device.  Instead it
//  pulls blocks from AudioTransportSource exactly like an audio callback would,
//  using the real BufferingAudioSource read-ahead thread and the real
//  ResamplingAudioSource.
//
//  Build (from the repository root, after running task_support/build.sh once):
//
//    c++ -std=c++17 -O2 -I. -Imodules \
//        task_examples/device_free_notify.cpp \
//        .task-build/transport.o .task-build/juce_audio_basics.o \
//        .task-build/juce_events.o .task-build/juce_core.o \
//        -framework Cocoa -framework Foundation -framework IOKit \
//        -framework Security -framework Accelerate -pthread \
//        -o "$TMPDIR/device_free_notify"
//
//  It prints a short PCM report and exits with status 0 when the notification
//  is audible sample-accurately.
// =============================================================================

#include <juce_audio_devices/juce_audio_devices.h>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace juce;

namespace
{

// A finite, addressable, non-looping mono source whose samples can be edited
// in place.  The caller synchronises its own edits (here with a CriticalSection).
class EditableToneSource final : public PositionableAudioSource
{
public:
    explicit EditableToneSource (int64 length) : pcm ((size_t) length, 0.25f) {}

    void prepareToPlay (int, double) override {}
    void releaseResources() override {}

    void setNextReadPosition (int64 p) override
    {
        const ScopedLock lock (positionLock);
        position = p;
    }

    int64 getNextReadPosition() const override { return position; }
    int64 getTotalLength() const override { return (int64) pcm.size(); }
    bool isLooping() const override { return false; }

    void getNextAudioBlock (const AudioSourceChannelInfo& info) override
    {
        const ScopedLock lock (positionLock);

        for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
        {
            for (int i = 0; i < info.numSamples; ++i)
            {
                const int64 index = position + i;
                const float value = index < (int64) pcm.size() ? pcm[(size_t) index] : 0.0f;
                info.buffer->setSample (ch, info.startSample + i, value);
            }
        }

        position += info.numSamples;
    }

    void replace (int64 start, int64 count, float value)
    {
        const ScopedLock lock (positionLock);

        for (int64 i = start; i < start + count && i < (int64) pcm.size(); ++i)
            pcm[(size_t) i] = value;
    }

private:
    std::vector<float> pcm;
    int64 position = 0;
    CriticalSection positionLock;
};

// Renders numDeviceFrames in fixed blocks and returns mono PCM.  When
// notifyFrame >= 0 the source region beginning at editSourceFrame is changed
// and notifySourceChanged() is called after notifyFrame delivered frames.
std::vector<float> render (double sourceRate, double deviceRate,
                           int numDeviceFrames, int blockSize,
                           int64 editSourceFrame, int notifyAfterDeviceFrames,
                           bool notify, float newValue)
{
    constexpr int64 sourceLength = 16384;
    EditableToneSource source (sourceLength);

    TimeSliceThread readAheadThread ("example-read-ahead");
    readAheadThread.startThread();

    AudioTransportSource transport;
    transport.setSource (&source, 4096, &readAheadThread, sourceRate, 1);
    transport.prepareToPlay (blockSize, deviceRate);
    transport.start();

    std::vector<float> output;
    output.reserve ((size_t) numDeviceFrames);

    int delivered = 0;
    bool done = false;

    while (delivered < numDeviceFrames)
    {
        const int n = jmin (blockSize, numDeviceFrames - delivered);

        if (! done && delivered >= notifyAfterDeviceFrames)
        {
            source.replace (editSourceFrame, 512, newValue);

            if (notify)
                transport.notifySourceChanged ({ editSourceFrame, editSourceFrame + 512 });

            done = true;
        }

        AudioBuffer<float> buffer (1, n);
        buffer.clear();
        transport.getNextAudioBlock ({ &buffer, 0, n });

        for (int i = 0; i < n; ++i)
            output.push_back (buffer.getSample (0, i));

        delivered += n;
    }

    transport.setSource (nullptr);
    readAheadThread.stopThread (2000);
    return output;
}

} // namespace

int main()
{
    ScopedJuceInitialiser_GUI juceInit;

    constexpr double deviceRate = 44100.0;
    constexpr double sourceRate = 66150.0; // source/device = 3/2 (down-sampling pre-filter)
    constexpr int totalFrames = 512;
    constexpr int blockSize = 64;
    constexpr int deliveredBeforeEdit = 128;

    const double ratio = sourceRate / deviceRate;
    const int64 q = (int64) ((double) deliveredBeforeEdit * ratio);
    const int64 editStart = (int64) std::ceil ((double) q - 1e-9) + 2;

    // Audible path: edit + notify after deliveredBeforeEdit device frames.
    const auto heard = render (sourceRate, deviceRate, totalFrames, blockSize,
                               editStart, deliveredBeforeEdit, true, -0.5f);

    // Reference sound: replace the future region first, then play the same
    // route from the same start with the same resampling ratio.
    EditableToneSource refSource (16384);
    refSource.replace (editStart, 512, -0.5f);
    TimeSliceThread refThread ("ref-read-ahead");
    refThread.startThread();
    AudioTransportSource refTransport;
    refTransport.setSource (&refSource, 4096, &refThread, sourceRate, 1);
    refTransport.prepareToPlay (blockSize, deviceRate);
    refTransport.start();

    std::vector<float> ref;
    int done = 0;

    while (done < totalFrames)
    {
        const int n = jmin (blockSize, totalFrames - done);
        AudioBuffer<float> b (1, n);
        b.clear();
        refTransport.getNextAudioBlock ({ &b, 0, n });
        for (int i = 0; i < n; ++i)
            ref.push_back (b.getSample (0, i));
        done += n;
    }

    refTransport.setSource (nullptr);
    refThread.stopThread (2000);

    double worst = 0.0;

    for (int i = 0; i < totalFrames; ++i)
        worst = jmax (worst, (double) std::abs (heard[(size_t) i] - ref[(size_t) i]));

    std::printf ("editStart(source frames)=%lld  q=%.1f ratio=%.4f\n",
                 (long long) editStart, (double) q, ratio);
    std::printf ("output[0]=%.5f output[%d]=%.5f output[%d]=%.5f\n",
                 heard[0], deliveredBeforeEdit,
                 heard[(size_t) deliveredBeforeEdit],
                 deliveredBeforeEdit + 4,
                 heard[(size_t) deliveredBeforeEdit + 4]);
    std::printf ("worst |notified - reference| = %.3e (tolerance 2e-5)\n", worst);

    if (worst > 2.0e-5)
    {
        std::fprintf (stderr, "FAIL: notified playback differs from reference\n");
        return 1;
    }

    std::printf ("DEVICE_FREE_EXAMPLE_OK\n");
    return 0;
}
