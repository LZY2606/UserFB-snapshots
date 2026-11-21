//=======================================================================
/** @file bwf_loudness.cpp
 *
 * Regression tests for AudioFileFormat::Bwf.
 *
 * Everything expected here is derived independently in this file:
 *  - a scalar two-biquad K-weighting filter and hopping 400 ms / 3 s
 *    window meter written from the BS.1770 / EBU Tech 3342 text;
 *  - standard full-scale normalisation of the actual output data bytes;
 *  - a hand-rolled RIFF/WAVE walker and chunk builder.
 *
 * None of the expectations are produced by the production BWF code.
 */
//=======================================================================

#include "AudioFile.h"
#include "task_support/check.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using Bytes = std::vector<uint8_t>;

    //===================================================================
    // Independent little-endian readers
    //===================================================================
    uint16_t refU16 (const Bytes& b, size_t p)
    {
        return static_cast<uint16_t> (b[p]) | static_cast<uint16_t> (b[p + 1] << 8);
    }

    uint32_t refU32 (const Bytes& b, size_t p)
    {
        return static_cast<uint32_t> (b[p])
             | (static_cast<uint32_t> (b[p + 1]) << 8)
             | (static_cast<uint32_t> (b[p + 2]) << 16)
             | (static_cast<uint32_t> (b[p + 3]) << 24);
    }

    int16_t refS16 (const Bytes& b, size_t p)
    {
        return static_cast<int16_t> (refU16 (b, p));
    }

    void put16 (Bytes& b, uint16_t v)
    {
        b.push_back (static_cast<uint8_t> (v & 0xFF));
        b.push_back (static_cast<uint8_t> (v >> 8));
    }

    void put32 (Bytes& b, uint32_t v)
    {
        b.push_back (static_cast<uint8_t> (v & 0xFF));
        b.push_back (static_cast<uint8_t> ((v >> 8) & 0xFF));
        b.push_back (static_cast<uint8_t> ((v >> 16) & 0xFF));
        b.push_back (static_cast<uint8_t> ((v >> 24) & 0xFF));
    }

    void setU16 (Bytes& b, size_t p, uint16_t v)
    {
        b[p] = static_cast<uint8_t> (v & 0xFF);
        b[p + 1] = static_cast<uint8_t> (v >> 8);
    }

    void putFourCC (Bytes& b, const char* id)
    {
        for (int i = 0; i < 4; ++i)
            b.push_back (static_cast<uint8_t> (id[i]));
    }

    //===================================================================
    // Independent RIFF/WAVE walker (payloads stay opaque)
    //===================================================================
    struct RefChunk
    {
        char id[5];
        uint32_t size = 0;
        Bytes payload;
        bool hasPad = false;
        uint8_t padByte = 0;
    };

    struct RefRiff
    {
        Bytes header;
        std::vector<RefChunk> chunks;
        uint32_t riffSize = 0;
    };

    bool refWalk (const Bytes& fileData, RefRiff& out)
    {
        out.chunks.clear();
        if (fileData.size() < 12 || std::memcmp (fileData.data(), "RIFF", 4) != 0
            || std::memcmp (fileData.data() + 8, "WAVE", 4) != 0)
            return false;

        out.riffSize = refU32 (fileData, 4);
        if (static_cast<size_t> (out.riffSize) + 8 != fileData.size())
            return false;

        size_t p = 12;
        while (p < fileData.size())
        {
            if (fileData.size() - p < 8)
                return false;

            RefChunk chunk;
            std::memcpy (chunk.id, fileData.data() + p, 4);
            chunk.id[4] = 0;
            chunk.size = refU32 (fileData, p + 4);

            const size_t payloadStart = p + 8;
            if (payloadStart > fileData.size()
                || static_cast<uint64_t> (chunk.size) > fileData.size() - payloadStart)
                return false;

            chunk.payload.assign (fileData.begin() + payloadStart,
                                  fileData.begin() + payloadStart + chunk.size);
            p = payloadStart + chunk.size;

            if (chunk.size & 1u)
            {
                if (p >= fileData.size())
                    return false;
                chunk.hasPad = true;
                chunk.padByte = fileData[p];
                ++p;
            }

            out.chunks.push_back (std::move (chunk));
        }

        return p == fileData.size();
    }

    const RefChunk* refFind (const RefRiff& riff, const char* id, int occurrence = 0)
    {
        int seen = 0;
        for (const auto& chunk : riff.chunks)
        {
            if (std::memcmp (chunk.id, id, 4) == 0)
            {
                if (seen == occurrence)
                    return &chunk;
                ++seen;
            }
        }
        return nullptr;
    }

    int refCount (const RefRiff& riff, const char* id)
    {
        int n = 0;
        for (const auto& chunk : riff.chunks)
            if (std::memcmp (chunk.id, id, 4) == 0)
                ++n;
        return n;
    }

    //===================================================================
    // Independent RIFF builder for crafting source containers
    //===================================================================
    struct ChunkSpec
    {
        const char* id;
        Bytes payload;
        bool forcePad = false;
        uint8_t padByte = 0;
    };

    Bytes refBuildRiff (const std::vector<ChunkSpec>& specs)
    {
        Bytes body;
        putFourCC (body, "WAVE");

        for (const auto& spec : specs)
        {
            putFourCC (body, spec.id);
            put32 (body, static_cast<uint32_t> (spec.payload.size()));
            body.insert (body.end(), spec.payload.begin(), spec.payload.end());
            if ((spec.payload.size() & 1u) != 0 || spec.forcePad)
                body.push_back (spec.padByte);
        }

        Bytes file;
        putFourCC (file, "RIFF");
        put32 (file, static_cast<uint32_t> (body.size()));
        file.insert (file.end(), body.begin(), body.end());
        return file;
    }

    Bytes refFmt (uint16_t tag, uint16_t channels, uint32_t rate, uint16_t bits)
    {
        Bytes p;
        put16 (p, tag);
        put16 (p, channels);
        put32 (p, rate);
        put32 (p, rate * channels * (bits / 8u));
        put16 (p, static_cast<uint16_t> (channels * (bits / 8u)));
        put16 (p, bits);
        return p;
    }

    Bytes refPcmData (const std::vector<std::vector<double> >& channels, int bits)
    {
        const size_t frames = channels[0].size();
        const int numChannels = static_cast<int> (channels.size());
        Bytes payload;

        for (size_t f = 0; f < frames; ++f)
        {
            for (int c = 0; c < numChannels; ++c)
            {
                double v = std::max (-1.0, std::min (1.0, channels[c][f]));

                if (bits == 8)
                {
                    int byteValue = static_cast<int> (std::floor ((v + 1.0) * 127.5 + 0.5));
                    payload.push_back (static_cast<uint8_t> (std::max (0, std::min (255, byteValue))));
                }
                else if (bits == 16)
                {
                    int32_t iv = static_cast<int32_t> (std::floor (v * 32767.0 + (v >= 0 ? 0.5 : -0.5)));
                    iv = std::max<int32_t> (-32768, std::min<int32_t> (32767, iv));
                    payload.push_back (static_cast<uint8_t> (iv & 0xFF));
                    payload.push_back (static_cast<uint8_t> ((iv >> 8) & 0xFF));
                }
                else if (bits == 24)
                {
                    int32_t iv = static_cast<int32_t> (std::floor (v * 8388607.0 + (v >= 0 ? 0.5 : -0.5)));
                    iv = std::max<int32_t> (-8388608, std::min<int32_t> (8388607, iv));
                    payload.push_back (static_cast<uint8_t> (iv & 0xFF));
                    payload.push_back (static_cast<uint8_t> ((iv >> 8) & 0xFF));
                    payload.push_back (static_cast<uint8_t> ((iv >> 16) & 0xFF));
                }
            }
        }

        return payload;
    }

    //===================================================================
    // Independent normalisation of the final data bytes
    // (standard full scale 2^(bits-1); 8-bit offset removed first)
    //===================================================================
    std::vector<std::vector<double> > refNormalise (const Bytes& payload, int channels, int bits, bool ieeeFloat)
    {
        const int width = bits / 8;
        const size_t frames = payload.size() / (static_cast<size_t> (channels) * width);
        std::vector<std::vector<double> > out (static_cast<size_t> (channels),
                                               std::vector<double> (frames));

        for (size_t f = 0; f < frames; ++f)
        {
            for (int c = 0; c < channels; ++c)
            {
                const size_t q = (f * channels + c) * width;
                double v = 0.0;

                if (ieeeFloat)
                {
                    uint32_t raw = refU32 (payload, q);
                    float fv;
                    std::memcpy (&fv, &raw, sizeof (float));
                    v = static_cast<double> (fv);
                }
                else if (bits == 8)
                {
                    v = (static_cast<int> (payload[q]) - 128) / 128.0;
                }
                else if (bits == 16)
                {
                    int16_t iv;
                    uint16_t raw = refU16 (payload, q);
                    std::memcpy (&iv, &raw, sizeof (int16_t));
                    v = static_cast<double> (iv) / 32768.0;
                }
                else if (bits == 24)
                {
                    int32_t iv = payload[q] | (payload[q + 1] << 8) | (payload[q + 2] << 16);
                    if (iv & 0x800000)
                        iv -= 1 << 24;
                    v = static_cast<double> (iv) / 8388608.0;
                }
                else
                {
                    int32_t iv = static_cast<int32_t> (refU32 (payload, q));
                    v = static_cast<double> (iv) / 2147483648.0;
                }

                out[static_cast<size_t> (c)][f] = v;
            }
        }

        return out;
    }

    //===================================================================
    // Independent scalar BS.1770 K-weighting implementation
    //===================================================================
    struct ScalarBiquad
    {
        double b0, b1, b2, a1, a2;
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

        double step (double x)
        {
            double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            return y;
        }
    };

    std::vector<double> refKWeight (const std::vector<double>& input)
    {
        ScalarBiquad pre { 1.53512485958697, -2.69169618940638, 1.19839281085285,
                           -1.69065929318241, 0.73248077421585 };
        ScalarBiquad highPass { 1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621 };

        std::vector<double> output;
        output.reserve (input.size());
        for (double x : input)
            output.push_back (highPass.step (pre.step (x)));
        return output;
    }

    struct RefMeter
    {
        double integrated = 0.0, maxMomentary = 0.0, maxShortTerm = 0.0, lra = 0.0;
        bool integratedKnown = false, maxMomentaryKnown = false;
        bool maxShortTermKnown = false, lraKnown = false;
        size_t momentaryWindowCount = 0, shortTermWindowCount = 0;
        size_t integratedGateCount = 0, lraGateCount = 0;
    };

    double refLoudness (double meanEnergy)
    {
        return -0.691 + 10.0 * std::log10 (meanEnergy);
    }

    double refPercentile (std::vector<double> sorted, double q)
    {
        const double position = q * static_cast<double> (sorted.size() - 1);
        const size_t lo = static_cast<size_t> (position);
        if (lo + 1 >= sorted.size())
            return sorted.back();
        const double frac = position - static_cast<double> (lo);
        return sorted[lo] + frac * (sorted[lo + 1] - sorted[lo]);
    }

    // Range-based windowing: frame-0 aligned, 100 ms hops, complete only.
    RefMeter refMeasure (const std::vector<std::vector<double> >& channels)
    {
        RefMeter meter;
        const size_t frames = channels[0].size();
        for (const auto& c : channels)
            assert (c.size() == frames);

        std::vector<std::vector<double> > filtered;
        for (const auto& c : channels)
            filtered.push_back (refKWeight (c));

        constexpr size_t rate = 48000;
        const size_t hop = rate / 10;
        const size_t mLen = rate * 4 / 10;
        const size_t sLen = rate * 3;

        auto windowEnergy = [&] (size_t start, size_t len)
        {
            double sum = 0.0;
            for (size_t f = start; f < start + len; ++f)
                for (const auto& c : filtered)
                    sum += c[f] * c[f];
            return sum / static_cast<double> (len);
        };

        std::vector<double> mEnergies, sEnergies;
        for (size_t start = 0; start + mLen <= frames; start += hop)
            mEnergies.push_back (windowEnergy (start, mLen));
        for (size_t start = 0; start + sLen <= frames; start += hop)
            sEnergies.push_back (windowEnergy (start, sLen));

        meter.momentaryWindowCount = mEnergies.size();
        meter.shortTermWindowCount = sEnergies.size();

        // Integrated: strict -70 absolute gate, then -10 LU relative gate.
        double absSum = 0.0;
        size_t absCount = 0;
        for (double e : mEnergies)
            if (e > 0.0 && refLoudness (e) > -70.0)
            {
                absSum += e;
                ++absCount;
            }

        if (absCount > 0)
        {
            const double relativeThreshold = (absSum / static_cast<double> (absCount)) * std::pow (10.0, -1.0);
            double gatedSum = 0.0;
            for (double e : mEnergies)
            {
                if (e > 0.0 && refLoudness (e) > -70.0 && e > relativeThreshold)
                {
                    gatedSum += e;
                    ++meter.integratedGateCount;
                }
            }

            if (meter.integratedGateCount > 0)
            {
                meter.integrated = refLoudness (gatedSum / static_cast<double> (meter.integratedGateCount));
                meter.integratedKnown = std::isfinite (meter.integrated);
            }
        }

        if (! mEnergies.empty())
        {
            double maxEnergy = 0.0;
            for (double e : mEnergies)
                maxEnergy = std::max (maxEnergy, e);
            if (maxEnergy > 0.0)
            {
                meter.maxMomentary = refLoudness (maxEnergy);
                meter.maxMomentaryKnown = std::isfinite (meter.maxMomentary);
            }
        }

        if (! sEnergies.empty())
        {
            double maxEnergy = 0.0;
            for (double e : sEnergies)
                maxEnergy = std::max (maxEnergy, e);
            if (maxEnergy > 0.0)
            {
                meter.maxShortTerm = refLoudness (maxEnergy);
                meter.maxShortTermKnown = std::isfinite (meter.maxShortTerm);
            }

            // LRA: absolute gate then -20 LU relative gate.
            double sAbsSum = 0.0;
            size_t sAbsCount = 0;
            for (double e : sEnergies)
                if (e > 0.0 && refLoudness (e) > -70.0)
                {
                    sAbsSum += e;
                    ++sAbsCount;
                }

            if (sAbsCount > 0)
            {
                const double relativeThreshold = (sAbsSum / static_cast<double> (sAbsCount)) * std::pow (10.0, -2.0);
                std::vector<double> gatedLoudness;
                for (double e : sEnergies)
                {
                    if (e > 0.0 && refLoudness (e) > -70.0 && e > relativeThreshold)
                    {
                        gatedLoudness.push_back (refLoudness (e));
                        ++meter.lraGateCount;
                    }
                }

                if (gatedLoudness.size() == 1)
                {
                    meter.lra = 0.0;
                    meter.lraKnown = true;
                }
                else if (gatedLoudness.size() >= 2)
                {
                    std::sort (gatedLoudness.begin(), gatedLoudness.end());
                    meter.lra = refPercentile (gatedLoudness, 0.95) - refPercentile (gatedLoudness, 0.10);
                    meter.lraKnown = std::isfinite (meter.lra);
                }
            }
        }

        return meter;
    }

    //===================================================================
    // bext encoding expectations, derived from the independent meter
    //===================================================================
    int64_t refRoundAway (double v)
    {
        return v >= 0.0 ? static_cast<int64_t> (std::floor (v + 0.5))
                        : static_cast<int64_t> (std::ceil (v - 0.5));
    }

    int32_t refEncode (bool known, double loudness)
    {
        if (! known || ! std::isfinite (loudness))
            return 0x7fff;
        const int64_t scaled = refRoundAway (loudness * 100.0);
        if (scaled < std::numeric_limits<int16_t>::min() || scaled > std::numeric_limits<int16_t>::max())
            return 0x7fff;
        return static_cast<int32_t> (scaled);
    }

    const RefChunk* bextChunk (const RefRiff& riff)
    {
        return refFind (riff, "bext");
    }

    // Compare the four measurements plus the true-peak marker. Known
    // values allow one centi-LU of tolerance; unknown must be exact.
    bool checkBextMeasurements (const Bytes& bwf, const RefMeter& ref)
    {
        RefRiff riff;
        if (! refWalk (bwf, riff))
            return false;

        const RefChunk* bext = bextChunk (riff);
        if (bext == nullptr || bext->payload.size() < 602)
            return false;

        const Bytes& p = bext->payload;
        if (refU16 (p, 346) != 2)
            return false;

        struct Field { size_t offset; bool known; double value; };
        const Field fields[] = {
            { 412, ref.integratedKnown, ref.integrated },
            { 414, ref.lraKnown, ref.lra },
            { 418, ref.maxMomentaryKnown, ref.maxMomentary },
            { 420, ref.maxShortTermKnown, ref.maxShortTerm }
        };

        for (const auto& field : fields)
        {
            const int32_t expected = refEncode (field.known, field.value);
            const int32_t actual = refS16 (p, field.offset);
            if (expected == 0x7fff)
            {
                if (actual != 0x7fff)
                    return false;
            }
            else if (std::abs (actual - expected) > 1)
            {
                return false;
            }
        }

        return refS16 (p, 416) == static_cast<int16_t> (0x7fff);
    }

    // Meter the actual output bytes of a BWF file independently.
    RefMeter meterBwfOutput (const Bytes& bwf)
    {
        RefRiff riff;
        if (! refWalk (bwf, riff))
            return RefMeter();

        const RefChunk* fmt = refFind (riff, "fmt ");
        const RefChunk* data = refFind (riff, "data");
        const uint16_t tag = refU16 (fmt->payload, 0);
        const uint16_t channels = refU16 (fmt->payload, 2);
        const uint16_t bits = refU16 (fmt->payload, 14);
        auto normalised = refNormalise (data->payload, channels, bits, tag == 3);
        return refMeasure (normalised);
    }

    //===================================================================
    // Shared test helpers
    //===================================================================
    template <class T>
    bool saveBwf (AudioFile<T>& audio, Bytes& out)
    {
        return audio.saveToMemory (out, AudioFileFormat::Bwf);
    }

    std::vector<std::vector<double> > makeTone (int channels, size_t frames,
                                                double amplitude, double frequency,
                                                double phase = 0.0, bool invertedSecond = false)
    {
        std::vector<std::vector<double> > buffer (static_cast<size_t> (channels),
                                                  std::vector<double> (frames));
        for (size_t i = 0; i < frames; ++i)
        {
            const double t = static_cast<double> (i) / 48000.0;
            const double v = amplitude * std::sin (2.0 * 3.14159265358979 * frequency * t + phase);
            buffer[0][i] = v;
            if (channels == 2)
                buffer[1][i] = invertedSecond ? -v : v;
        }
        return buffer;
    }

    template <class T>
    AudioFile<T> makeAudio (const std::vector<std::vector<double> >& buffer, int bits)
    {
        AudioFile<T> audio;
        audio.shouldLogErrorsToConsole (false);
        audio.setAudioBufferSize (static_cast<int> (buffer.size()), static_cast<int> (buffer[0].size()));
        audio.setSampleRate (48000);
        audio.setBitDepth (bits);
        for (size_t c = 0; c < buffer.size(); ++c)
            for (size_t i = 0; i < buffer[0].size(); ++i)
                audio.samples[c][i] = static_cast<T> (buffer[c][i]);
        return audio;
    }
}

//===================================================================
// quantized group: measurements reflect the quantised output bytes
//===================================================================

static int quant_8bit_silence()
{
    // A non-zero buffer whose every sample quantises to the unsigned
    // 8-bit silence byte 128: the measured sound must be fully unknown.
    const size_t frames = 48000 * 2;
    auto buffer = makeTone (1, frames, 0.0, 1000.0);
    for (size_t i = 0; i < frames; ++i)
    {
        const double t = static_cast<double> (i) / 48000.0;
        buffer[0][i] = 0.003 + 0.002 * std::sin (2.0 * 3.14159265358979 * 1000.0 * t);
    }

    auto audio = makeAudio<double> (buffer, 8);
    Bytes out;
    TASK_CHECK (saveBwf (audio, out));

    RefRiff riff;
    TASK_CHECK (refWalk (out, riff));
    const RefChunk* data = refFind (riff, "data");
    TASK_CHECK (data != nullptr && data->payload.size() == frames);
    for (uint8_t byte : data->payload)
        TASK_CHECK (byte == 128);

    const RefMeter ref = meterBwfOutput (out);
    TASK_CHECK (! ref.integratedKnown && ! ref.maxMomentaryKnown
                && ! ref.maxShortTermKnown && ! ref.lraKnown);
    TASK_CHECK (checkBextMeasurements (out, ref));

    // Saving must not alter the in-memory audio at all.
    TASK_CHECK (audio.samples.size() == buffer.size());
    for (size_t i = 0; i < frames; ++i)
        TASK_CHECK (audio.samples[0][i] == buffer[0][i]);
    return 0;
}

static int quant_negative_and_full_scale()
{
    const size_t frames = 48000 * 4;
    std::vector<std::vector<double> > buffer (1, std::vector<double> (frames));
    for (size_t i = 0; i < frames; ++i)
    {
        const double t = static_cast<double> (i) / 48000.0;
        // reaches -1.0 exactly at the sine peaks, never positive
        buffer[0][i] = -0.9 - 0.1 * std::sin (2.0 * 3.14159265358979 * 300.0 * t);
    }

    auto audio = makeAudio<double> (buffer, 16);
    Bytes out;
    TASK_CHECK (saveBwf (audio, out));

    RefRiff riff;
    TASK_CHECK (refWalk (out, riff));
    const RefChunk* data = refFind (riff, "data");

    // Standard full scale: byte value -32768 maps to -1.0 exactly.
    auto normalised = refNormalise (data->payload, 1, 16, false);
    bool sawNegativeFullScale = false;
    for (double v : normalised[0])
    {
        TASK_CHECK (v >= -1.0000001 && v <= 1.0000001);
        if (v <= -0.9999)
            sawNegativeFullScale = true;
    }
    TASK_CHECK (sawNegativeFullScale);

    const RefMeter ref = refMeasure (normalised);
    TASK_CHECK (ref.integratedKnown);
    TASK_CHECK (checkBextMeasurements (out, ref));

    // 8-bit full-scale endpoints use the 128 convention, not 127.
    auto audio8 = makeAudio<double> (buffer, 8);
    Bytes out8;
    TASK_CHECK (saveBwf (audio8, out8));
    RefRiff riff8;
    TASK_CHECK (refWalk (out8, riff8));
    auto norm8 = refNormalise (refFind (riff8, "data")->payload, 1, 8, false);
    // 8-bit full-scale endpoints use the 128 convention, not 127. The
    // encoder emits bytes in [1, 255], so the deepest value is -127/128.
    double min8 = std::numeric_limits<double>::infinity();
    double max8 = -std::numeric_limits<double>::infinity();
    for (double v : norm8[0])
    {
        min8 = std::min (min8, v);
        max8 = std::max (max8, v);
    }
    TASK_CHECK (min8 == -127.0 / 128.0 && max8 < -0.7);
    // byte 0 is decoded by the standard -128/128 = -1.0 convention here,
    // whereas the reader's max-positive rule would give -128/127.
    TASK_CHECK (refNormalise (Bytes ({ 0 }), 1, 8, false)[0][0] == -1.0);
    TASK_CHECK (refNormalise (Bytes ({ 255 }), 1, 8, false)[0][0] == 127.0 / 128.0);
    const RefMeter ref8 = refMeasure (norm8);
    TASK_CHECK (checkBextMeasurements (out8, ref8));
    return 0;
}

static int quant_all_bit_depths_match_data_reference()
{
    auto buffer = makeTone (2, 48000 * 5, 0.55, 880.0, 0.3, true);

    for (int bits : { 8, 16, 24, 32 })
    {
        auto audio = makeAudio<double> (buffer, bits);
        Bytes out;
        TASK_CHECK (saveBwf (audio, out));
        const RefMeter ref = meterBwfOutput (out);
        TASK_CHECK (ref.integratedKnown && ref.maxMomentaryKnown
                    && ref.maxShortTermKnown && ref.lraKnown);
        TASK_CHECK (checkBextMeasurements (out, ref));

        // float template instantiation must agree with double for 32-bit float
        auto audioF = makeAudio<float> (buffer, bits);
        Bytes outF;
        TASK_CHECK (saveBwf (audioF, outF));
        const RefMeter refF = meterBwfOutput (outF);
        TASK_CHECK (checkBextMeasurements (outF, refF));
    }
    return 0;
}

static int quant_repeated_save_stable()
{
    auto buffer = makeTone (2, 48000 * 4, 0.4, 660.0, 0.0, true);
    auto audio = makeAudio<double> (buffer, 16);

    Bytes first, second;
    TASK_CHECK (saveBwf (audio, first));
    TASK_CHECK (saveBwf (audio, second));
    TASK_CHECK (first == second);

    // Round trip through the loader and save again: 16-bit is lossless.
    AudioFile<double> reloaded;
    reloaded.shouldLogErrorsToConsole (false);
    TASK_CHECK (reloaded.loadFromMemory (first));
    Bytes third;
    TASK_CHECK (saveBwf (reloaded, third));

    RefRiff a, b;
    TASK_CHECK (refWalk (first, a) && refWalk (third, b));
    TASK_CHECK (refFind (a, "data")->payload == refFind (b, "data")->payload);
    TASK_CHECK (refFind (a, "bext")->payload == refFind (b, "bext")->payload);
    return 0;
}

static int quant_bit_depth_change_remeasures()
{
    auto buffer = makeTone (1, 48000 * 4, 0.01, 1200.0);
    auto audio = makeAudio<double> (buffer, 16);

    Bytes out16;
    TASK_CHECK (saveBwf (audio, out16));
    const RefMeter ref16 = meterBwfOutput (out16);
    TASK_CHECK (checkBextMeasurements (out16, ref16));

    audio.setBitDepth (8);
    Bytes out8;
    TASK_CHECK (saveBwf (audio, out8));
    const RefMeter ref8 = meterBwfOutput (out8);
    TASK_CHECK (checkBextMeasurements (out8, ref8));

    // Coarser quantisation must move at least one measurement.
    RefRiff r16, r8;
    TASK_CHECK (refWalk (out16, r16));
    TASK_CHECK (refWalk (out8, r8));
    const RefChunk* b16 = refFind (r16, "bext");
    const RefChunk* b8 = refFind (r8, "bext");
    TASK_CHECK (std::memcmp (b16->payload.data() + 412, b8->payload.data() + 412, 10) != 0);
    return 0;
}

static int quant_length_change_remeasures()
{
    auto buffer = makeTone (1, 48000 * 5, 0.5, 500.0);
    auto audio = makeAudio<double> (buffer, 16);

    Bytes full;
    TASK_CHECK (saveBwf (audio, full));
    TASK_CHECK (meterBwfOutput (full).shortTermWindowCount > 0);

    // Trim below the 3 s boundary: short-term and LRA become unknown.
    audio.setNumSamplesPerChannel (48000 * 2);
    Bytes trimmed;
    TASK_CHECK (saveBwf (audio, trimmed));
    const RefMeter refTrim = meterBwfOutput (trimmed);
    TASK_CHECK (refTrim.shortTermWindowCount == 0);
    TASK_CHECK (! refTrim.maxShortTermKnown && ! refTrim.lraKnown);
    TASK_CHECK (refTrim.momentaryWindowCount > 0 && refTrim.integratedKnown);
    TASK_CHECK (checkBextMeasurements (trimmed, refTrim));

    RefRiff riff;
    TASK_CHECK (refWalk (trimmed, riff));
    TASK_CHECK (refFind (riff, "data")->payload.size() == static_cast<size_t> (48000 * 2 * 2));
    return 0;
}

static int quant_zero_frames_all_unknown()
{
    AudioFile<double> audio;
    audio.shouldLogErrorsToConsole (false);
    audio.setAudioBufferSize (1, 0);
    audio.setSampleRate (48000);
    audio.setBitDepth (16);

    Bytes out;
    TASK_CHECK (saveBwf (audio, out));
    const RefMeter ref = meterBwfOutput (out);
    TASK_CHECK (ref.momentaryWindowCount == 0 && ref.shortTermWindowCount == 0);
    TASK_CHECK (checkBextMeasurements (out, ref));
    return 0;
}

//===================================================================
// meter group: gating, windowing, percentile and boundary behaviour
//===================================================================

static int meter_inverted_stereo_and_channel_sum()
{
    const size_t frames = 48000 * 6;
    auto stereo = makeTone (2, frames, 0.5, 700.0, 0.7, true);
    auto mono = stereo;
    mono.resize (1);

    auto sAudio = makeAudio<double> (stereo, 24);
    auto mAudio = makeAudio<double> (mono, 24);

    Bytes sOut, mOut;
    TASK_CHECK (saveBwf (sAudio, sOut));
    TASK_CHECK (saveBwf (mAudio, mOut));

    const RefMeter sRef = meterBwfOutput (sOut);
    const RefMeter mRef = meterBwfOutput (mOut);
    TASK_CHECK (sRef.integratedKnown && mRef.integratedKnown);
    TASK_CHECK (checkBextMeasurements (sOut, sRef));
    TASK_CHECK (checkBextMeasurements (mOut, mRef));

    // Inverted second channel doubles every window energy: every
    // measurement sits exactly 10 log10(2) above the mono version.
    TASK_CHECK (std::abs ((sRef.integrated - mRef.integrated) - 10.0 * std::log10 (2.0)) < 0.01);
    TASK_CHECK (std::abs ((sRef.maxMomentary - mRef.maxMomentary) - 10.0 * std::log10 (2.0)) < 0.01);
    TASK_CHECK (std::abs ((sRef.maxShortTerm - mRef.maxShortTerm) - 10.0 * std::log10 (2.0)) < 0.01);
    return 0;
}

static int meter_loud_quiet_and_below_gate()
{
    const size_t frames = 48000 * 12;
    std::vector<std::vector<double> > buffer (1, std::vector<double> (frames));
    for (size_t i = 0; i < frames; ++i)
    {
        const double t = static_cast<double> (i) / 48000.0;
        const double second = static_cast<double> (i % frames) / static_cast<double> (frames);
        double amplitude;
        if (second < 0.4)
            amplitude = 0.5;                       // loud passage
        else if (second < 0.7)
            amplitude = 0.0001;                    // far below -70 LUFS
        else
            amplitude = 0.12;                      // moderate passage
        buffer[0][i] = amplitude * std::sin (2.0 * 3.14159265358979 * 440.0 * t);
    }

    auto audio = makeAudio<double> (buffer, 32);
    Bytes out;
    TASK_CHECK (saveBwf (audio, out));
    const RefMeter ref = meterBwfOutput (out);

    // Independently confirm that some 400 ms windows fail the absolute gate.
    TASK_CHECK (ref.momentaryWindowCount > ref.integratedGateCount + 4);
    TASK_CHECK (ref.integratedKnown && ref.lraKnown);
    TASK_CHECK (checkBextMeasurements (out, ref));
    return 0;
}

static int meter_window_tail_alignment()
{
    // 21600 frames = 400 ms + a 50 ms non-window tail: exactly one
    // complete momentary window and no complete short-term window. The
    // tail must not be zero-padded into an extra window.
    auto shortBuffer = makeTone (1, 21600, 0.4, 330.0);
    auto shortAudio = makeAudio<double> (shortBuffer, 16);
    Bytes shortOut;
    TASK_CHECK (saveBwf (shortAudio, shortOut));
    const RefMeter shortRef = meterBwfOutput (shortOut);
    TASK_CHECK (shortRef.momentaryWindowCount == 1);
    TASK_CHECK (shortRef.shortTermWindowCount == 0);
    TASK_CHECK (shortRef.integratedKnown && shortRef.maxMomentaryKnown);
    TASK_CHECK (! shortRef.maxShortTermKnown && ! shortRef.lraKnown);
    // With a single surviving window, integrated equals max momentary.
    TASK_CHECK (std::abs (shortRef.integrated - shortRef.maxMomentary) < 1e-9);
    TASK_CHECK (checkBextMeasurements (shortOut, shortRef));
    return 0;
}

static int meter_three_second_tail()
{
    // 160000 frames: four complete 3 s windows (144000 .. 158400),
    // then a 1600-frame tail that must not be zero-padded into a window.
    auto buffer = makeTone (1, 160000, 0.45, 250.0);
    auto audio = makeAudio<double> (buffer, 16);
    Bytes out;
    TASK_CHECK (saveBwf (audio, out));
    const RefMeter ref = meterBwfOutput (out);
    TASK_CHECK (ref.shortTermWindowCount == 4);
    TASK_CHECK (ref.momentaryWindowCount == (160000 - 19200) / 4800 + 1);
    TASK_CHECK (checkBextMeasurements (out, ref));
    return 0;
}

static int meter_percentile_interpolation()
{
    // Stepwise levels give a gated 3 s window set whose sorted loudness
    // values land on fractional q*(n-1) interpolation positions.
    const size_t frames = 48000 * 15;
    std::vector<std::vector<double> > buffer (1, std::vector<double> (frames));
    const double levels[] = { 0.5, 0.25, 0.08, 0.35, 0.15 };
    for (size_t i = 0; i < frames; ++i)
    {
        const double t = static_cast<double> (i) / 48000.0;
        const size_t segment = i / (frames / 5);
        buffer[0][i] = levels[segment] * std::sin (2.0 * 3.14159265358979 * 440.0 * t);
    }

    auto audio = makeAudio<double> (buffer, 32);
    Bytes out;
    TASK_CHECK (saveBwf (audio, out));
    const RefMeter ref = meterBwfOutput (out);
    TASK_CHECK (ref.lraKnown && ref.lraGateCount >= 3);
    TASK_CHECK (checkBextMeasurements (out, ref));

    // Independently verify the q*(n-1) linear interpolation on a
    // hand-built sorted set, including fractional positions.
    std::vector<double> hand = { 1.0, 2.0, 3.0, 4.0 };
    TASK_CHECK (std::abs (refPercentile (hand, 0.50) - 2.5) < 1e-12);   // pos 1.5
    TASK_CHECK (std::abs (refPercentile (hand, 0.10) - 1.3) < 1e-12);   // pos 0.3
    TASK_CHECK (std::abs (refPercentile (hand, 0.95) - 3.85) < 1e-12);  // pos 2.85
    TASK_CHECK (refPercentile (hand, 1.0) == 4.0);                      // pos clamps to last
    TASK_CHECK (refPercentile (hand, 0.0) == 1.0);
    return 0;
}

static int meter_lra_single_window_is_zero()
{
    // Exactly 3 s: one complete 3 s window, so LRA is defined as zero
    // rather than unknown.
    auto buffer = makeTone (1, 48000 * 3, 0.3, 440.0);
    auto audio = makeAudio<double> (buffer, 16);
    Bytes out;
    TASK_CHECK (saveBwf (audio, out));
    const RefMeter ref = meterBwfOutput (out);
    TASK_CHECK (ref.lraKnown && ref.lra == 0.0);
    TASK_CHECK (checkBextMeasurements (out, ref));
    return 0;
}

static int meter_bext_rounding_and_unknown_encoding()
{
    // Exact encoding rules, independent of the one-unit measurement
    // tolerance: halves round away from zero and out-of-range/unknown
    // values become the 0x7fff marker.
    TASK_CHECK (refRoundAway (2.345) == 2 && refRoundAway (2.5) == 3);
    TASK_CHECK (refRoundAway (-2.5) == -3 && refRoundAway (-2.499) == -2);
    TASK_CHECK (refEncode (false, 0.0) == 0x7fff);
    TASK_CHECK (refEncode (true, std::numeric_limits<double>::quiet_NaN()) == 0x7fff);
    TASK_CHECK (refEncode (true, 400.0) == 0x7fff);   // +40000 out of int16
    TASK_CHECK (refEncode (true, -400.0) == 0x7fff);  // -40000 out of int16
    TASK_CHECK (refEncode (true, -23.735) == -2374);  // half moves away from zero
    TASK_CHECK (refEncode (true, 327.67) == 32767);
    TASK_CHECK (refEncode (true, -327.68) == -32768);

    // A silent file yields four unknown fields and the true-peak marker.
    auto silent = makeAudio<double> (makeTone (1, 48000 * 2, 0.0, 1000.0), 16);
    Bytes out;
    TASK_CHECK (saveBwf (silent, out));
    RefRiff riff;
    TASK_CHECK (refWalk (out, riff));
    const Bytes& bext = refFind (riff, "bext")->payload;
    for (size_t offset : { 412u, 414u, 416u, 418u, 420u })
        TASK_CHECK (refS16 (bext, offset) == static_cast<int16_t> (0x7fff));
    return 0;
}

static int meter_absolute_gate_boundary_strict()
{
    // Bisect an amplitude whose K-weighted 400 ms loudness is exactly
    // -70 LUFS, using only the independent reference.
    const size_t window = 19200;
    auto levelAt = [&] (double amplitude)
    {
        auto buffer = makeTone (1, window, amplitude, 1000.0);
        // measure the single window directly
        auto filtered = refKWeight (buffer[0]);
        double sum = 0.0;
        for (double v : filtered)
            sum += v * v;
        return refLoudness (sum / static_cast<double> (window));
    };

    double lo = 1e-9, hi = 1e-2;
    TASK_CHECK (levelAt (lo) < -70.0 && levelAt (hi) > -70.0);
    for (int iteration = 0; iteration < 200; ++iteration)
    {
        const double mid = std::sqrt (lo * hi);
        if (levelAt (mid) < -70.0)
            lo = mid;
        else
            hi = mid;
    }
    const double boundary = std::sqrt (lo * hi);

    // Judge the strict gate on the actual output bytes. The independent
    // reference measures each file itself: a signal it places below -70
    // must be reported unknown by the BWF, one it places above must pass.
    auto below = makeTone (1, 48000 * 3, boundary * 0.90, 1000.0);
    auto belowAudio = makeAudio<double> (below, 32);
    Bytes belowOut;
    TASK_CHECK (saveBwf (belowAudio, belowOut));
    const RefMeter belowRef = meterBwfOutput (belowOut);
    TASK_CHECK (belowRef.momentaryWindowCount > 0);
    TASK_CHECK (belowRef.maxMomentaryKnown && belowRef.maxMomentary < -70.0);
    TASK_CHECK (! belowRef.integratedKnown);
    TASK_CHECK (checkBextMeasurements (belowOut, belowRef));

    auto above = makeTone (1, 48000 * 3, boundary * 1.10, 1000.0);
    auto aboveAudio = makeAudio<double> (above, 32);
    Bytes aboveOut;
    TASK_CHECK (saveBwf (aboveAudio, aboveOut));
    const RefMeter aboveRef = meterBwfOutput (aboveOut);
    TASK_CHECK (aboveRef.maxMomentaryKnown && aboveRef.maxMomentary > -70.0);
    TASK_CHECK (aboveRef.integratedKnown);
    TASK_CHECK (checkBextMeasurements (aboveOut, aboveRef));
    return 0;
}

//===================================================================
// container group: skeleton preservation and RIFF correctness
//===================================================================

static Bytes makeRichContainer()
{
    const uint16_t channels = 1;
    const uint16_t bits = 16;
    const size_t frames = 9600;

    auto tone = makeTone (channels, frames, 0.4, 440.0);
    Bytes dataPayload = refPcmData (tone, bits);

    Bytes junkPayload = { 'd', 'a', 't', 'a', 'f', 'm', 't', ' ', 'x' }; // 9 bytes, odd
    Bytes secondJunk = { 'b', 'e', 'x', 't', 0x01 };                      // 5 bytes, odd

    Bytes bext (602, 0);
    bext[346] = 2; // Version = 2 (little-endian low byte)
    bext[347] = 0;
    const char description[] = "preserved description";
    std::memcpy (bext.data(), description, sizeof (description));
    const char originator[] = "preserved-originator";
    std::memcpy (bext.data() + 256, originator, sizeof (originator));
    // timeReference (300..307) and UMID (364..427) carry distinctive bytes
    for (int i = 300; i < 308; ++i)
        bext[static_cast<size_t> (i)] = static_cast<uint8_t> (0xA0 + i);
    for (int i = 364; i < 428; ++i)
        bext[static_cast<size_t> (i)] = static_cast<uint8_t> (0x10 + (i % 200));
    for (size_t i = 422; i < 602; ++i)
        bext[i] = static_cast<uint8_t> (1 + (i % 250)); // CodingHistory area
    // pre-existing (stale) measurement values
    setU16 (bext, 412, static_cast<uint16_t> (-1234));
    setU16 (bext, 414, 567);
    setU16 (bext, 416, 111);
    setU16 (bext, 418, static_cast<uint16_t> (-2222));
    setU16 (bext, 420, 333);

    Bytes factPayload;
    put32 (factPayload, static_cast<uint32_t> (frames));
    factPayload.push_back (0xDE);
    factPayload.push_back (0xAD);
    factPayload.push_back (0xBE);
    factPayload.push_back (0xEF);

    Bytes ixmlPayload = { '<', 'i', 'X', 'M', 'L', '/', '>' };

    std::vector<ChunkSpec> specs;
    specs.push_back ({ "fmt ", refFmt (1, channels, 48000, bits) });
    specs.push_back ({ "junk", junkPayload, false, 0xAB }); // odd size -> pad 0xAB
    specs.push_back ({ "data", dataPayload });
    specs.push_back ({ "bext", bext });                     // bext AFTER data
    specs.push_back ({ "fact", factPayload });
    specs.push_back ({ "junk", secondJunk, false, 0xCD });  // repeated FourCC, pad 0xCD
    specs.push_back ({ "iXML", ixmlPayload });
    return refBuildRiff (specs);
}

static int container_odd_chunks_fake_bytes_and_order()
{
    const Bytes source = makeRichContainer();

    AudioFile<double> audio;
    audio.shouldLogErrorsToConsole (false);
    TASK_CHECK (audio.loadFromMemory (source));
    TASK_CHECK (audio.getNumSamplesPerChannel() == 9600);

    // Change the audio so fmt/data must be rebuilt and fact refreshed.
    audio.setNumSamplesPerChannel (14400);
    for (int i = 9600; i < 14400; ++i)
        audio.samples[0][i] = 0.2 * std::sin (2.0 * 3.14159265358979 * 220.0 * i / 48000.0);

    Bytes out;
    TASK_CHECK (saveBwf (audio, out));

    RefRiff sourceRiff, outRiff;
    TASK_CHECK (refWalk (source, sourceRiff));
    TASK_CHECK (refWalk (out, outRiff));

    // Chunk count: same skeleton, all chunks retained.
    TASK_CHECK (outRiff.chunks.size() == sourceRiff.chunks.size());

    const char* expectedOrder[] = { "fmt ", "junk", "data", "bext", "fact", "junk", "iXML" };
    for (size_t i = 0; i < outRiff.chunks.size(); ++i)
        TASK_CHECK (std::memcmp (outRiff.chunks[i].id, expectedOrder[i], 4) == 0);

    // The two odd-length unknown chunks keep payload AND non-zero padding.
    TASK_CHECK (refCount (outRiff, "junk") == 2);
    const RefChunk& junk0 = outRiff.chunks[1];
    const RefChunk& junk1 = outRiff.chunks[5];
    TASK_CHECK (junk0.payload == sourceRiff.chunks[1].payload && junk0.hasPad && junk0.padByte == 0xAB);
    TASK_CHECK (junk1.payload == sourceRiff.chunks[5].payload && junk1.hasPad && junk1.padByte == 0xCD);
    TASK_CHECK (junk0.payload[0] == 'd' && junk0.payload[3] == 'a'); // fake chunk names stayed opaque

    // fact: frame DWORD refreshed, trailing payload bytes preserved.
    const RefChunk* fact = refFind (outRiff, "fact");
    TASK_CHECK (fact != nullptr && fact->payload.size() == 8);
    TASK_CHECK (refU32 (fact->payload, 0) == 14400);
    TASK_CHECK (fact->payload[4] == 0xDE && fact->payload[5] == 0xAD
                && fact->payload[6] == 0xBE && fact->payload[7] == 0xEF);

    // bext: non-measurement bytes preserved byte-for-byte; only the
    // measurement fields and the true-peak marker change.
    const RefChunk* srcBext = refFind (sourceRiff, "bext");
    const RefChunk* outBext = refFind (outRiff, "bext");
    TASK_CHECK (outBext->payload.size() == srcBext->payload.size());
    for (size_t i = 0; i < 602; ++i)
    {
        const bool measurementField = (i >= 412 && i < 422);
        if (! measurementField)
            TASK_CHECK (outBext->payload[i] == srcBext->payload[i]);
    }
    TASK_CHECK (refS16 (outBext->payload, 416) == static_cast<int16_t> (0x7fff));
    TASK_CHECK (refU16 (outBext->payload, 346) == 2);
    TASK_CHECK (checkBextMeasurements (out, meterBwfOutput (out)));
    return 0;
}

static int container_bext_inserted_before_fmt()
{
    const size_t frames = 19200;
    auto tone = makeTone (1, frames, 0.3, 440.0);
    std::vector<ChunkSpec> specs = {
        { "fmt ", refFmt (1, 1, 48000, 16) },
        { "data", refPcmData (tone, 16) }
    };
    const Bytes source = refBuildRiff (specs);

    AudioFile<double> audio;
    audio.shouldLogErrorsToConsole (false);
    TASK_CHECK (audio.loadFromMemory (source));

    Bytes out;
    TASK_CHECK (saveBwf (audio, out));
    RefRiff riff;
    TASK_CHECK (refWalk (out, riff));
    TASK_CHECK (riff.chunks.size() == 3);
    TASK_CHECK (std::memcmp (riff.chunks[0].id, "bext", 4) == 0);
    TASK_CHECK (std::memcmp (riff.chunks[1].id, "fmt ", 4) == 0);
    TASK_CHECK (std::memcmp (riff.chunks[2].id, "data", 4) == 0);
    const RefChunk* bext = refFind (riff, "bext");
    TASK_CHECK (bext->payload.size() == 602);
    bool allZeroExceptVersionAndMarkers = true;
    for (size_t i = 0; i < 602; ++i)
    {
        const bool allowed = i == 346 || i == 347 || (i >= 412 && i < 422);
        if (! allowed && bext->payload[i] != 0)
            allZeroExceptVersionAndMarkers = false;
    }
    TASK_CHECK (allZeroExceptVersionAndMarkers);
    TASK_CHECK (checkBextMeasurements (out, meterBwfOutput (out)));
    return 0;
}

static int container_ixml_inplace_drop_and_append()
{
    const size_t frames = 19200;
    auto tone = makeTone (1, frames, 0.3, 440.0);
    Bytes ixmlPayload = { 'o', 'l', 'd' };
    std::vector<ChunkSpec> specs = {
        { "fmt ", refFmt (1, 1, 48000, 16) },
        { "iXML", ixmlPayload },
        { "data", refPcmData (tone, 16) }
    };
    const Bytes source = refBuildRiff (specs);

    // In-place update keeps the iXML position between fmt and data (the
    // inserted bext sits ahead of fmt).
    AudioFile<double> audio;
    audio.shouldLogErrorsToConsole (false);
    TASK_CHECK (audio.loadFromMemory (source));
    TASK_CHECK (audio.iXMLChunk == "old");
    audio.iXMLChunk = "new-ixml";
    Bytes updated;
    TASK_CHECK (saveBwf (audio, updated));
    RefRiff riffUpdated;
    TASK_CHECK (refWalk (updated, riffUpdated));
    int ixmlPos = -1, fmtPos = -1, dataPos = -1;
    for (int i = 0; i < static_cast<int> (riffUpdated.chunks.size()); ++i)
    {
        const RefChunk& chunk = riffUpdated.chunks[static_cast<size_t> (i)];
        if (std::memcmp (chunk.id, "iXML", 4) == 0) ixmlPos = i;
        if (std::memcmp (chunk.id, "fmt ", 4) == 0) fmtPos = i;
        if (std::memcmp (chunk.id, "data", 4) == 0) dataPos = i;
    }
    TASK_CHECK (ixmlPos != -1 && fmtPos < ixmlPos && ixmlPos < dataPos);
    TASK_CHECK (riffUpdated.chunks[static_cast<size_t> (ixmlPos)].payload
                == Bytes ({ 'n', 'e', 'w', '-', 'i', 'x', 'm', 'l' }));

    // Emptying iXML drops the chunk.
    audio.iXMLChunk.clear();
    Bytes dropped;
    TASK_CHECK (saveBwf (audio, dropped));
    RefRiff riffDropped;
    TASK_CHECK (refWalk (dropped, riffDropped));
    TASK_CHECK (refCount (riffDropped, "iXML") == 0);

    // A fresh object with iXML but no source gets it appended.
    auto fresh = makeAudio<double> (tone, 16);
    fresh.iXMLChunk = "appended";
    Bytes appended;
    TASK_CHECK (saveBwf (fresh, appended));
    RefRiff riffAppended;
    TASK_CHECK (refWalk (appended, riffAppended));
    TASK_CHECK (std::memcmp (riffAppended.chunks.back().id, "iXML", 4) == 0);
    TASK_CHECK (riffAppended.chunks.back().payload == Bytes ({ 'a', 'p', 'p', 'e', 'n', 'd', 'e', 'd' }));
    return 0;
}

static int container_float_fact_rules()
{
    auto tone = makeTone (1, 24000, 0.3, 440.0);

    // Float output with no source fact gains a 4-byte fact before data.
    auto fresh = makeAudio<double> (tone, 32);
    Bytes out;
    TASK_CHECK (saveBwf (fresh, out));
    RefRiff riff;
    TASK_CHECK (refWalk (out, riff));
    TASK_CHECK (refCount (riff, "fact") == 1);
    const RefChunk* fact = refFind (riff, "fact");
    TASK_CHECK (fact->payload.size() == 4 && refU32 (fact->payload, 0) == 24000);
    TASK_CHECK (std::memcmp (riff.chunks[2].id, "fact", 4) == 0);
    TASK_CHECK (std::memcmp (riff.chunks[3].id, "data", 4) == 0);

    // A source fact with trailing bytes keeps them on float re-save.
    Bytes factPayload;
    put32 (factPayload, 1);
    factPayload.push_back (0x77);
    factPayload.push_back (0x88);
    std::vector<ChunkSpec> specs = {
        { "fmt ", refFmt (3, 1, 48000, 32) },
        { "fact", factPayload },
        { "data", Bytes (24000 * 4, 0) }
    };
    AudioFile<double> loaded;
    loaded.shouldLogErrorsToConsole (false);
    TASK_CHECK (loaded.loadFromMemory (refBuildRiff (specs)));
    TASK_CHECK (loaded.getNumSamplesPerChannel() == 24000);
    Bytes again;
    TASK_CHECK (saveBwf (loaded, again));
    RefRiff riffAgain;
    TASK_CHECK (refWalk (again, riffAgain));
    const RefChunk* factAgain = refFind (riffAgain, "fact");
    TASK_CHECK (factAgain->payload.size() == 6); // 4-byte frame count + 2 preserved
    TASK_CHECK (refU32 (factAgain->payload, 0) == 24000);
    TASK_CHECK (factAgain->payload[4] == 0x77 && factAgain->payload[5] == 0x88);
    TASK_CHECK (! factAgain->hasPad); // even payload needs no pad byte
    return 0;
}

static int container_reread_sound_and_duration()
{
    auto buffer = makeTone (2, 48000 * 2, 0.6, 520.0, 0.1, true);
    auto audio = makeAudio<double> (buffer, 16);
    Bytes out;
    TASK_CHECK (saveBwf (audio, out));

    AudioFile<double> reread;
    reread.shouldLogErrorsToConsole (false);
    TASK_CHECK (reread.loadFromMemory (out));
    TASK_CHECK (reread.getSampleRate() == 48000);
    TASK_CHECK (reread.getNumChannels() == 2);
    TASK_CHECK (reread.getBitDepth() == 16);
    TASK_CHECK (reread.getNumSamplesPerChannel() == 48000 * 2);
    TASK_CHECK (std::abs (reread.getLengthInSeconds() - 2.0) < 1e-9);

    RefRiff riff;
    TASK_CHECK (refWalk (out, riff));
    auto standard = refNormalise (refFind (riff, "data")->payload, 2, 16, false);

    // The library reader keeps its max-positive-integer convention
    // (int16 / 32767); derive it independently from the same bytes and
    // confirm it matches the re-read samples exactly.
    const Bytes& dataBytes = refFind (riff, "data")->payload;
    for (int c = 0; c < 2; ++c)
    {
        for (int i = 0; i < reread.getNumSamplesPerChannel(); ++i)
        {
            const size_t q = static_cast<size_t> ((i * 2 + c) * 2);
            const int16_t iv = static_cast<int16_t> (refU16 (dataBytes, q));
            const double readerConvention = static_cast<double> (iv) / 32767.0;
            TASK_CHECK (std::abs (reread.samples[c][i] - readerConvention) < 1e-12);
            // standard full-scale and reader convention stay close (one LSB)
            TASK_CHECK (std::abs (reread.samples[c][i] - standard[c][i]) <= 1.0 / 32768.0 + 1e-12);
            // and both reproduce the original sound within one quantisation step
            TASK_CHECK (std::abs (reread.samples[c][i] - buffer[c][i]) <= 1.0 / 32768.0 + 1e-12);
        }
    }

    // Ordinary Wave saving from a BWF-loaded object stays BWF-free.
    Bytes wave;
    TASK_CHECK (audio.saveToMemory (wave, AudioFileFormat::Wave));
    RefRiff waveRiff;
    TASK_CHECK (refWalk (wave, waveRiff));
    TASK_CHECK (refCount (waveRiff, "bext") == 0);
    return 0;
}

static int container_aiff_load_replaces_riff_skeleton()
{
    // After a successful BWF/WAVE load, a successful AIFF load must drop
    // the old RIFF skeleton: the next BWF save has no source chunks.
    auto wavAudio = makeAudio<double> (makeTone (1, 19200, 0.3, 440.0), 16);
    Bytes bwf;
    TASK_CHECK (saveBwf (wavAudio, bwf));

    AudioFile<double> audio;
    audio.shouldLogErrorsToConsole (false);
    TASK_CHECK (audio.loadFromMemory (bwf));

    Bytes aiff;
    TASK_CHECK (audio.saveToMemory (aiff, AudioFileFormat::Aiff));
    TASK_CHECK (audio.loadFromMemory (aiff));
    TASK_CHECK (audio.getNumSamplesPerChannel() == 19200);

    Bytes rebuilt;
    TASK_CHECK (saveBwf (audio, rebuilt));
    RefRiff riff;
    TASK_CHECK (refWalk (rebuilt, riff));
    // A fresh skeleton: exactly bext, fmt, data (16-bit, no fact).
    TASK_CHECK (riff.chunks.size() == 3);
    TASK_CHECK (std::memcmp (riff.chunks[0].id, "bext", 4) == 0);
    TASK_CHECK (std::memcmp (riff.chunks[1].id, "fmt ", 4) == 0);
    TASK_CHECK (std::memcmp (riff.chunks[2].id, "data", 4) == 0);
    TASK_CHECK (checkBextMeasurements (rebuilt, meterBwfOutput (rebuilt)));
    return 0;
}

static int container_oversized_bext_preserved()
{
    // A 701-byte (odd) bext carrying CodingHistory must keep every
    // non-measurement byte; the rewritten chunk is re-padded with zero.
    const size_t frames = 19200;
    auto tone = makeTone (1, frames, 0.3, 440.0);

    Bytes bext (701, 0);
    bext[346] = 1; // source Version 1 must be upgraded to 2
    const char history[] = "A=PCM F=48000 W=16 M=mono";
    std::memcpy (bext.data() + 602, history, sizeof (history));
    for (size_t i = 602 + sizeof (history); i < 701; ++i)
        bext[i] = static_cast<uint8_t> (i);

    std::vector<ChunkSpec> specs = {
        { "fmt ", refFmt (1, 1, 48000, 16) },
        { "bext", bext },
        { "data", refPcmData (tone, 16) }
    };
    const Bytes source = refBuildRiff (specs);

    AudioFile<double> audio;
    audio.shouldLogErrorsToConsole (false);
    TASK_CHECK (audio.loadFromMemory (source));
    Bytes out;
    TASK_CHECK (saveBwf (audio, out));

    RefRiff riff;
    TASK_CHECK (refWalk (out, riff));
    const RefChunk* outBext = refFind (riff, "bext");
    TASK_CHECK (outBext->payload.size() == 701);
    TASK_CHECK (outBext->hasPad && outBext->padByte == 0); // replaced chunk -> zero pad
    TASK_CHECK (refU16 (outBext->payload, 346) == 2);
    for (size_t i = 0; i < 701; ++i)
    {
        const bool measurementField = (i >= 412 && i < 422);
        const bool versionField = (i == 346 || i == 347);
        if (! measurementField && ! versionField)
            TASK_CHECK (outBext->payload[i] == bext[i]);
    }
    TASK_CHECK (checkBextMeasurements (out, meterBwfOutput (out)));
    return 0;
}

static int container_save_to_file_roundtrip()
{
    const char* tmpdir = std::getenv ("TMPDIR");
    TASK_CHECK (tmpdir != nullptr);
    const std::string path = std::string (tmpdir) + "/bwf_loudness_roundtrip.wav";

    auto audio = makeAudio<double> (makeTone (1, 19200, 0.35, 440.0), 16);
    TASK_CHECK (audio.save (path, AudioFileFormat::Bwf));

    AudioFile<double> reread;
    reread.shouldLogErrorsToConsole (false);
    TASK_CHECK (reread.load (path));
    TASK_CHECK (reread.getNumSamplesPerChannel() == 19200);
    std::remove (path.c_str());
    return 0;
}

//===================================================================
// failure group: rejected configurations and corrupt containers
//===================================================================

static int failure_unsupported_configuration()
{
    auto base = makeAudio<double> (makeTone (1, 19200, 0.3, 440.0), 16);

    Bytes sentinel (64, 0xCD);

    AudioFile<double> wrongRate = base;
    wrongRate.setSampleRate (44100);
    Bytes a = sentinel;
    TASK_CHECK (! saveBwf (wrongRate, a));
    TASK_CHECK (a == sentinel); // output vector untouched on failure

    AudioFile<double> threeChannels;
    threeChannels.shouldLogErrorsToConsole (false);
    threeChannels.setAudioBufferSize (3, 19200);
    threeChannels.setSampleRate (48000);
    threeChannels.setBitDepth (16);
    Bytes b = sentinel;
    TASK_CHECK (! saveBwf (threeChannels, b));
    TASK_CHECK (b == sentinel);

    AudioFile<double> badBits = base;
    badBits.setBitDepth (12);
    Bytes c = sentinel;
    TASK_CHECK (! saveBwf (badBits, c));
    TASK_CHECK (c == sentinel);

    AudioFile<int> integerAudio;
    integerAudio.shouldLogErrorsToConsole (false);
    integerAudio.setAudioBufferSize (1, 19200);
    integerAudio.setSampleRate (48000);
    integerAudio.setBitDepth (16);
    Bytes d = sentinel;
    TASK_CHECK (! integerAudio.saveToMemory (d, AudioFileFormat::Bwf));
    TASK_CHECK (d == sentinel);
    return 0;
}

static int failure_nonfinite_and_unequal_channels()
{
    auto audio = makeAudio<double> (makeTone (2, 19200, 0.3, 440.0), 16);
    Bytes sentinel (32, 0xEE);

    audio.samples[0][100] = std::numeric_limits<double>::quiet_NaN();
    Bytes a = sentinel;
    TASK_CHECK (! saveBwf (audio, a));
    TASK_CHECK (a == sentinel);

    audio.samples[0][100] = std::numeric_limits<double>::infinity();
    Bytes b = sentinel;
    TASK_CHECK (! saveBwf (audio, b));
    TASK_CHECK (b == sentinel);

    auto unequal = makeAudio<double> (makeTone (2, 19200, 0.3, 440.0), 16);
    unequal.samples[1].resize (100);
    Bytes c = sentinel;
    TASK_CHECK (! saveBwf (unequal, c));
    TASK_CHECK (c == sentinel);
    return 0;
}

static int failure_save_does_not_truncate_file()
{
    const char* tmpdir = std::getenv ("TMPDIR");
    TASK_CHECK (tmpdir != nullptr);
    const std::string path = std::string (tmpdir) + "/bwf_loudness_existing.wav";
    {
        std::FILE* f = std::fopen (path.c_str(), "wb");
        TASK_CHECK (f != nullptr);
        const char marker[] = "PRE-EXISTING-CONTENT-MUST-SURVIVE";
        std::fwrite (marker, 1, sizeof (marker), f);
        std::fclose (f);
    }

    auto audio = makeAudio<double> (makeTone (1, 19200, 0.3, 440.0), 16);
    audio.setSampleRate (44100); // invalid for BWF
    TASK_CHECK (! audio.save (path, AudioFileFormat::Bwf));

    Bytes onDisk;
    {
        std::FILE* f = std::fopen (path.c_str(), "rb");
        TASK_CHECK (f != nullptr);
        uint8_t buffer[128];
        size_t n = std::fread (buffer, 1, sizeof (buffer), f);
        onDisk.assign (buffer, buffer + n);
        std::fclose (f);
    }
    const std::string marker = "PRE-EXISTING-CONTENT-MUST-SURVIVE";
    TASK_CHECK (onDisk.size() == marker.size() + 1);
    TASK_CHECK (std::memcmp (onDisk.data(), marker.data(), marker.size()) == 0);
    std::remove (path.c_str());
    return 0;
}

static int failure_rf64_rifx_and_compressed()
{
    auto good = makeAudio<double> (makeTone (1, 19200, 0.3, 440.0), 16);
    Bytes goodBytes;
    TASK_CHECK (saveBwf (good, goodBytes));
    TASK_CHECK (good.loadFromMemory (goodBytes));
    const auto preservedSamples = good.samples;

    // Every failed load is attempted on the already-good object so that
    // we directly prove its previous audio and container are preserved.
    auto corrupt = [&] (const char* tag)
    {
        Bytes copy = goodBytes;
        std::memcpy (copy.data(), tag, 4);
        return good.loadFromMemory (copy);
    };

    TASK_CHECK (! corrupt ("RF64"));
    TASK_CHECK (good.samples == preservedSamples);
    TASK_CHECK (! corrupt ("RIFX"));
    TASK_CHECK (good.samples == preservedSamples);

    // Compressed fmt tag fails the strict parse.
    Bytes compressed = goodBytes;
    RefRiff riff;
    TASK_CHECK (refWalk (compressed, riff));
    for (auto& chunk : riff.chunks)
    {
        if (std::memcmp (chunk.id, "fmt ", 4) == 0)
        {
            chunk.payload[0] = 0x06; // A-law
            chunk.payload[1] = 0x00;
        }
    }
    // Rebuild with the corrupted fmt using the independent builder.
    std::vector<ChunkSpec> specs;
    for (const auto& chunk : riff.chunks)
        specs.push_back ({ chunk.id, chunk.payload, chunk.hasPad, chunk.padByte });
    compressed = refBuildRiff (specs);
    TASK_CHECK (! good.loadFromMemory (compressed));
    TASK_CHECK (good.samples == preservedSamples);

    // The previously loaded audio and container are untouched and still
    // reproduce the identical BWF file.
    Bytes again;
    TASK_CHECK (saveBwf (good, again));
    TASK_CHECK (again == goodBytes);
    return 0;
}

static int failure_extensible_loads_but_cannot_save_bwf()
{
    auto tone = makeTone (1, 19200, 0.3, 440.0);
    std::vector<ChunkSpec> specs = {
        { "fmt ", refFmt (0xFFFE, 1, 48000, 16) },
        { "data", refPcmData (tone, 16) }
    };
    const Bytes source = refBuildRiff (specs);

    AudioFile<double> audio;
    audio.shouldLogErrorsToConsole (false);
    TASK_CHECK (audio.loadFromMemory (source)); // ordinary read still works
    Bytes out (16, 0x11);
    TASK_CHECK (! saveBwf (audio, out));
    TASK_CHECK (out == Bytes (16, 0x11));
    return 0;
}

static int failure_duplicate_and_undersized_chunks()
{
    auto tone = makeTone (1, 19200, 0.3, 440.0);
    const Bytes data = refPcmData (tone, 16);
    const Bytes fmt = refFmt (1, 1, 48000, 16);

    auto tryLoad = [&] (const std::vector<ChunkSpec>& specs)
    {
        AudioFile<double> probe;
        probe.shouldLogErrorsToConsole (false);
        return probe.loadFromMemory (refBuildRiff (specs));
    };

    TASK_CHECK (! tryLoad ({ { "fmt ", fmt }, { "fmt ", fmt }, { "data", data } }));
    TASK_CHECK (! tryLoad ({ { "fmt ", fmt }, { "data", data }, { "data", data } }));

    Bytes bextShort (601, 0);
    TASK_CHECK (! tryLoad ({ { "fmt ", fmt }, { "bext", bextShort }, { "data", data } }));
    Bytes bextDup (602, 0);
    TASK_CHECK (! tryLoad ({ { "fmt ", fmt }, { "bext", bextDup }, { "bext", bextDup }, { "data", data } }));
    Bytes bextBadVersion (602, 0);
    bextBadVersion[346] = 3;
    TASK_CHECK (! tryLoad ({ { "fmt ", fmt }, { "bext", bextBadVersion }, { "data", data } }));

    Bytes factShort (3, 0);
    TASK_CHECK (! tryLoad ({ { "fmt ", fmt }, { "fact", factShort }, { "data", data } }));
    Bytes factA;
    put32 (factA, 1);
    TASK_CHECK (! tryLoad ({ { "fmt ", fmt }, { "fact", factA }, { "fact", factA }, { "data", data } }));
    TASK_CHECK (! tryLoad ({ { "fmt ", fmt }, { "iXML", Bytes ({ 'a' }) }, { "iXML", Bytes ({ 'b' }) }, { "data", data } }));
    return 0;
}

static int failure_truncation_and_bad_sizes_preserve_state()
{
    auto good = makeAudio<double> (makeTone (1, 19200, 0.3, 440.0), 16);
    Bytes goodBytes;
    TASK_CHECK (saveBwf (good, goodBytes));
    TASK_CHECK (good.loadFromMemory (goodBytes));
    const auto preserved = good.samples;

    // All failures are attempted on the object that already holds a good
    // load, proving the previous audio and container survive intact.
    TASK_CHECK (! good.loadFromMemory (Bytes (goodBytes.begin(), goodBytes.end() - 1)));
    TASK_CHECK (good.samples == preserved);
    TASK_CHECK (! good.loadFromMemory (Bytes (goodBytes.begin(), goodBytes.begin() + 20)));
    TASK_CHECK (good.samples == preserved);

    Bytes wrongRiffSize = goodBytes;
    wrongRiffSize[4] = static_cast<uint8_t> (wrongRiffSize[4] + 1);
    TASK_CHECK (! good.loadFromMemory (wrongRiffSize));
    TASK_CHECK (good.samples == preserved);

    // A chunk that declares more payload than the file contains.
    Bytes overrun = goodBytes;
    size_t p = 12;
    while (p < overrun.size() && std::memcmp (overrun.data() + p, "data", 4) != 0)
    {
        uint32_t sz = refU32 (overrun, p + 4);
        p += 8 + sz + (sz & 1u);
    }
    TASK_CHECK (p < overrun.size());
    overrun[p + 4] = 0xFF;
    overrun[p + 5] = 0xFF;
    TASK_CHECK (! good.loadFromMemory (overrun));
    TASK_CHECK (good.samples == preserved);

    // The object is still fully functional and reproduces the same BWF.
    Bytes reSaved;
    TASK_CHECK (saveBwf (good, reSaved));
    TASK_CHECK (reSaved == goodBytes);
    return 0;
}

static int failure_data_frame_and_padding_rules()
{
    // 8-bit mono, one frame: data payload is a single (odd) byte and the
    // RIFF padding byte is required; this must load and re-save.
    Bytes oneFrame;
    oneFrame.push_back (200);
    std::vector<ChunkSpec> goodSpecs = {
        { "fmt ", refFmt (1, 1, 48000, 8) },
        { "data", oneFrame }
    };
    const Bytes goodSource = refBuildRiff (goodSpecs);
    RefRiff goodRiff;
    TASK_CHECK (refWalk (goodSource, goodRiff));
    TASK_CHECK (refFind (goodRiff, "data")->size == 1 && refFind (goodRiff, "data")->hasPad);

    AudioFile<double> audio;
    audio.shouldLogErrorsToConsole (false);
    TASK_CHECK (audio.loadFromMemory (goodSource));
    TASK_CHECK (audio.getNumSamplesPerChannel() == 1 && audio.getBitDepth() == 8);
    // reader convention: (byte - 128) / 127
    TASK_CHECK (std::abs (audio.samples[0][0] - (200 - 128) / 127.0) < 1e-12);

    Bytes reSaved;
    TASK_CHECK (saveBwf (audio, reSaved));
    RefRiff reRiff;
    TASK_CHECK (refWalk (reSaved, reRiff));
    const RefChunk* reData = refFind (reRiff, "data");
    TASK_CHECK (reData->payload.size() == 1 && reData->hasPad); // odd byte re-padded

    // Re-reading the re-saved BWF reproduces its actual byte under the
    // reader convention, and still represents one frame.
    AudioFile<double> reRead;
    reRead.shouldLogErrorsToConsole (false);
    TASK_CHECK (reRead.loadFromMemory (reSaved));
    TASK_CHECK (reRead.getNumSamplesPerChannel() == 1);
    const uint8_t reByte = reData->payload[0];
    TASK_CHECK (std::abs (reRead.samples[0][0] - (static_cast<int> (reByte) - 128) / 127.0) < 1e-12);

    // 16-bit mono data whose size is not a whole frame must be rejected.
    Bytes brokenData = { 0, 0, 0 }; // 3 bytes, block align is 2
    std::vector<ChunkSpec> badSpecs = {
        { "fmt ", refFmt (1, 1, 48000, 16) },
        { "data", brokenData }
    };
    AudioFile<double> probe;
    probe.shouldLogErrorsToConsole (false);
    TASK_CHECK (! probe.loadFromMemory (refBuildRiff (badSpecs)));
    return 0;
}

static int failure_unknown_payload_never_scanned()
{
    // An unknown chunk whose payload contains full fake fmt and data
    // headers must be treated as opaque; only the real chunks count.
    auto tone = makeTone (1, 19200, 0.3, 440.0);
    Bytes sneaky;
    putFourCC (sneaky, "fmt ");
    put32 (sneaky, 16);
    sneaky.insert (sneaky.end(), 16, 0);
    putFourCC (sneaky, "data");
    put32 (sneaky, 4);
    sneaky.insert (sneaky.end(), 4, 0);

    std::vector<ChunkSpec> specs = {
        { "fmt ", refFmt (1, 1, 48000, 16) },
        { "junk", sneaky },
        { "data", refPcmData (tone, 16) }
    };
    const Bytes source = refBuildRiff (specs);

    AudioFile<double> audio;
    audio.shouldLogErrorsToConsole (false);
    TASK_CHECK (audio.loadFromMemory (source));
    TASK_CHECK (audio.getNumSamplesPerChannel() == 19200);

    Bytes out;
    TASK_CHECK (saveBwf (audio, out));
    RefRiff riff;
    TASK_CHECK (refWalk (out, riff));
    TASK_CHECK (refCount (riff, "fmt ") == 1 && refCount (riff, "data") == 1);
    TASK_CHECK (refFind (riff, "junk")->payload == sneaky);
    return 0;
}

//===================================================================
// Registry
//===================================================================
extern const task_case task_cases[] = {
    { "quantized", "8bit_quantized_silence_is_unknown", quant_8bit_silence },
    { "quantized", "negative_samples_and_full_scale", quant_negative_and_full_scale },
    { "quantized", "all_bit_depths_match_data_byte_reference", quant_all_bit_depths_match_data_reference },
    { "quantized", "repeated_save_is_stable", quant_repeated_save_stable },
    { "quantized", "bit_depth_change_remeasures", quant_bit_depth_change_remeasures },
    { "quantized", "length_change_remeasures", quant_length_change_remeasures },
    { "quantized", "zero_frames_all_unknown", quant_zero_frames_all_unknown },

    { "meter", "inverted_stereo_channel_energy_sum", meter_inverted_stereo_and_channel_sum },
    { "meter", "loud_quiet_below_gate_passages", meter_loud_quiet_and_below_gate },
    { "meter", "window_tail_alignment", meter_window_tail_alignment },
    { "meter", "three_second_tail_no_zero_padding", meter_three_second_tail },
    { "meter", "percentile_interpolation", meter_percentile_interpolation },
    { "meter", "lra_single_window_zero", meter_lra_single_window_is_zero },
    { "meter", "bext_rounding_and_unknown_encoding", meter_bext_rounding_and_unknown_encoding },
    { "meter", "absolute_gate_strict_boundary", meter_absolute_gate_boundary_strict },

    { "container", "odd_chunks_fake_bytes_order_preserved", container_odd_chunks_fake_bytes_and_order },
    { "container", "bext_inserted_before_fmt", container_bext_inserted_before_fmt },
    { "container", "ixml_inplace_drop_append", container_ixml_inplace_drop_and_append },
    { "container", "float_fact_rules", container_float_fact_rules },
    { "container", "reread_sound_and_duration", container_reread_sound_and_duration },
    { "container", "aiff_load_replaces_riff_skeleton", container_aiff_load_replaces_riff_skeleton },
    { "container", "oversized_bext_preserved", container_oversized_bext_preserved },
    { "container", "save_to_file_roundtrip", container_save_to_file_roundtrip },

    { "failure", "unsupported_configuration_leaves_output", failure_unsupported_configuration },
    { "failure", "nonfinite_and_unequal_channels", failure_nonfinite_and_unequal_channels },
    { "failure", "save_failure_preserves_target_file", failure_save_does_not_truncate_file },
    { "failure", "rf64_rifx_compressed_preserve_state", failure_rf64_rifx_and_compressed },
    { "failure", "extensible_loads_but_bwf_save_rejected", failure_extensible_loads_but_cannot_save_bwf },
    { "failure", "duplicate_and_undersized_chunks", failure_duplicate_and_undersized_chunks },
    { "failure", "truncation_and_bad_sizes", failure_truncation_and_bad_sizes_preserve_state },
    { "failure", "data_frame_and_padding_rules", failure_data_frame_and_padding_rules },
    { "failure", "unknown_payload_never_scanned", failure_unknown_payload_never_scanned }
};

extern const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases[0]);
