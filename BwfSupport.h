//=======================================================================
/** @file BwfSupport.h
 *
 * Broadcast Wave Format (BWF) support for the AudioFile library.
 *
 * This is an internal implementation header used by AudioFile.h. It adds
 * strict RIFF/WAVE container parsing (so that unknown chunks and their
 * padding are preserved), a BS.1770-4 K-weighted loudness meter that
 * measures the actual quantised output bytes, and a BWF writer that keeps
 * the original container skeleton while refreshing the fmt, data, bext,
 * iXML and fact chunks.
 *
 * The meter implements:
 *  - ITU-R BS.1770 K-weighting (two cascaded second-order biquads) with
 *    per-channel energy summation, integrated / momentary / short-term
 *    loudness using the dual (absolute + relative) gating procedure;
 *  - EBU Tech 3342 loudness range (LRA), including the P95 - P10
 *    percentile interpolation;
 *  - no true-peak measurement (the true-peak bext field is unknown).
 */
//=======================================================================

#ifndef _AS_BwfSupport_h
#define _AS_BwfSupport_h

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace bwf_support
{
    //=============================================================
    using Bytes = std::vector<uint8_t>;

    //=============================================================
    /** A top-level RIFF chunk together with its position and padding. */
    struct RiffChunk
    {
        std::array<char, 4> id;
        size_t position = 0;       // position of the FourCC in the file
        uint32_t size = 0;         // declared payload length
        Bytes payload;             // exact payload (no padding)
        uint8_t padByte = 0;       // the single padding byte, when size is odd
        bool hasPad = false;
    };

    //=============================================================
    struct RiffContainer
    {
        std::vector<RiffChunk> chunks;
        int fmtIndex = -1;
        int dataIndex = -1;
        int bextIndex = -1;
        int factIndex = -1;
        int ixmlIndex = -1;
        int formatTag = 0;         // WAVE_FORMAT tag from the fmt chunk
        uint16_t channels = 0;
        uint32_t sampleRate = 0;
        uint16_t bitsPerSample = 0;
    };

    //=============================================================
    inline uint16_t rd16 (const Bytes& b, size_t p)
    {
        return static_cast<uint16_t> (b[p]) | (static_cast<uint16_t> (b[p + 1]) << 8);
    }

    inline uint32_t rd32 (const Bytes& b, size_t p)
    {
        return static_cast<uint32_t> (b[p])
             | (static_cast<uint32_t> (b[p + 1]) << 8)
             | (static_cast<uint32_t> (b[p + 2]) << 16)
             | (static_cast<uint32_t> (b[p + 3]) << 24);
    }

    inline void wr16 (Bytes& b, size_t p, uint16_t v)
    {
        b[p] = static_cast<uint8_t> (v & 0xFF);
        b[p + 1] = static_cast<uint8_t> ((v >> 8) & 0xFF);
    }

    inline void wr32 (Bytes& b, size_t p, uint32_t v)
    {
        b[p] = static_cast<uint8_t> (v & 0xFF);
        b[p + 1] = static_cast<uint8_t> ((v >> 8) & 0xFF);
        b[p + 2] = static_cast<uint8_t> ((v >> 16) & 0xFF);
        b[p + 3] = static_cast<uint8_t> ((v >> 24) & 0xFF);
    }

    inline bool fourCC (const std::array<char, 4>& id, const char* text)
    {
        return std::memcmp (id.data(), text, 4) == 0;
    }

    //=============================================================
    /** Strictly parse a little-endian RIFF/WAVE container.
     *
     *  Every top-level chunk is recorded verbatim, including its payload
     *  and its odd-byte padding. Chunk payloads are treated as opaque: no
     *  searching for chunk names takes place inside them. Exactly one fmt
     *  chunk and one whole-frame data chunk are required. bext, fact and
     *  iXML may each appear at most once. The RIFF size and every chunk
     *  length are fully checked.
     */
    inline bool parseRiffWave (const Bytes& fileData, RiffContainer& out)
    {
        out = RiffContainer();

        if (fileData.size() < 12)
            return false;

        if (std::memcmp (fileData.data(), "RIFF", 4) != 0
            || std::memcmp (fileData.data() + 8, "WAVE", 4) != 0)
            return false;

        // RF64/RIFX use different identifiers and are out of scope.
        const uint32_t riffSize = rd32 (fileData, 4);
        if (static_cast<uint64_t> (riffSize) + 8u != static_cast<uint64_t> (fileData.size()))
            return false;

        size_t p = 12;
        const size_t end = fileData.size();

        while (p < end)
        {
            if (end - p < 8)
                return false;

            RiffChunk chunk;
            std::memcpy (chunk.id.data(), fileData.data() + p, 4);
            chunk.position = p;
            chunk.size = rd32 (fileData, p + 4);

            const uint64_t payloadStart = static_cast<uint64_t> (p) + 8;
            if (payloadStart > end || static_cast<uint64_t> (chunk.size) > end - payloadStart)
                return false;

            chunk.payload.assign (fileData.begin() + p + 8,
                                  fileData.begin() + p + 8 + chunk.size);

            size_t next = p + 8 + chunk.size;
            if ((chunk.size & 1u) != 0)
            {
                if (next >= end)
                    return false;

                chunk.hasPad = true;
                chunk.padByte = fileData[next];
                ++next;
            }

            if (fourCC (chunk.id, "fmt "))
            {
                if (out.fmtIndex != -1 || chunk.size < 16)
                    return false;
                out.fmtIndex = static_cast<int> (out.chunks.size());
            }
            else if (fourCC (chunk.id, "data"))
            {
                if (out.dataIndex != -1)
                    return false;
                out.dataIndex = static_cast<int> (out.chunks.size());
            }
            else if (fourCC (chunk.id, "bext"))
            {
                if (out.bextIndex != -1 || chunk.size < 602)
                    return false;
                out.bextIndex = static_cast<int> (out.chunks.size());
            }
            else if (fourCC (chunk.id, "fact"))
            {
                if (out.factIndex != -1 || chunk.size < 4)
                    return false;
                out.factIndex = static_cast<int> (out.chunks.size());
            }
            else if (fourCC (chunk.id, "iXML"))
            {
                if (out.ixmlIndex != -1)
                    return false;
                out.ixmlIndex = static_cast<int> (out.chunks.size());
            }

            out.chunks.push_back (std::move (chunk));
            p = next;
        }

        if (p != end || out.fmtIndex == -1 || out.dataIndex == -1)
            return false;

        const RiffChunk& fmt = out.chunks[static_cast<size_t> (out.fmtIndex)];
        const Bytes& fp = fmt.payload;
        out.formatTag = rd16 (fp, 0);
        out.channels = rd16 (fp, 2);
        out.sampleRate = rd32 (fp, 4);
        out.bitsPerSample = rd16 (fp, 14);

        const uint32_t bytesPerSecond = rd32 (fp, 8);
        const uint16_t blockAlign = rd16 (fp, 12);

        if (out.channels < 1 || out.channels > 128)
            return false;
        if (out.bitsPerSample != 8 && out.bitsPerSample != 16
            && out.bitsPerSample != 24 && out.bitsPerSample != 32)
            return false;

        const uint32_t blockSize = static_cast<uint32_t> (out.channels) * (out.bitsPerSample / 8u);
        if (blockAlign != blockSize || bytesPerSecond != out.sampleRate * blockSize)
            return false;

        // Ordinary reading keeps PCM, IEEE float and Extensible; the BWF
        // save path rejects Extensible and compressed formats separately.
        if (out.formatTag != 0x0001 && out.formatTag != 0x0003 && out.formatTag != 0xFFFE)
            return false;

        const RiffChunk& data = out.chunks[static_cast<size_t> (out.dataIndex)];
        if (blockSize == 0 || (data.size % blockSize) != 0)
            return false;

        if (out.bextIndex != -1)
        {
            const Bytes& bext = out.chunks[static_cast<size_t> (out.bextIndex)].payload;
            const uint16_t version = rd16 (bext, 346);
            if (version > 2)
                return false;
        }

        return true;
    }

    //=============================================================
    /** One of the four BS.1770/EBU Tech 3342 measurements. A value that
     *  cannot be determined is flagged unknown rather than invented. */
    struct LoudnessMeasurements
    {
        double integrated = 0.0;
        double maxMomentary = 0.0;
        double maxShortTerm = 0.0;
        double lra = 0.0;
        bool integratedKnown = false;
        bool maxMomentaryKnown = false;
        bool maxShortTermKnown = false;
        bool lraKnown = false;
        bool lraSingleWindow = false;
    };

    //=============================================================
    /** A direct-form-I biquad: y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2 */
    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

        double process (double x)
        {
            const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            return y;
        }
    };

    //=============================================================
    /** K-weighting filter chain for one channel, with zero history. */
    struct KWeightingChannel
    {
        // Stage 1: pre-filter (shelving). Coefficients from BS.1770-4.
        Biquad stage1 { 1.53512485958697, -2.69169618940638, 1.19839281085285,
                        -1.69065929318241, 0.73248077421585, 0.0, 0.0, 0.0, 0.0 };

        // Stage 2: high-pass filter.
        Biquad stage2 { 1.0, -2.0, 1.0,
                        -1.99004745483398, 0.99007225036621, 0.0, 0.0, 0.0, 0.0 };

        double process (double x)
        {
            return stage2.process (stage1.process (x));
        }
    };

    //=============================================================
    namespace detail
    {
        inline double loudnessFromEnergy (double meanEnergy)
        {
            return -0.691 + 10.0 * std::log10 (meanEnergy);
        }

        /** Linear-interpolated quantile at index q * (n - 1). */
        inline double percentile (std::vector<double>& sorted, double q)
        {
            const double pos = q * static_cast<double> (sorted.size() - 1);
            const size_t lo = static_cast<size_t> (pos);
            const size_t hi = lo + 1;
            const double frac = pos - static_cast<double> (lo);

            if (hi >= sorted.size())
                return sorted.back();

            return sorted[lo] + frac * (sorted[hi] - sorted[lo]);
        }

        /** The BS.1770 dual-gating mean energy. The absolute gate keeps
         *  windows strictly louder than -70 LUFS; the relative gate sits
         *  10 LU below the mean of the survivors. */
        inline double gatedMeanEnergy (const std::vector<double>& windowEnergies)
        {
            double absoluteSum = 0.0;
            size_t absoluteCount = 0;

            for (double energy : windowEnergies)
            {
                if (energy > 0.0 && loudnessFromEnergy (energy) > -70.0)
                {
                    absoluteSum += energy;
                    ++absoluteCount;
                }
            }

            if (absoluteCount == 0)
                return 0.0;

            const double relativeThresholdEnergy = (absoluteSum / static_cast<double> (absoluteCount))
                                                   * std::pow (10.0, -1.0);
            double gatedSum = 0.0;
            size_t gatedCount = 0;

            for (double energy : windowEnergies)
            {
                if (energy > 0.0
                    && loudnessFromEnergy (energy) > -70.0
                    && energy > relativeThresholdEnergy)
                {
                    gatedSum += energy;
                    ++gatedCount;
                }
            }

            if (gatedCount == 0)
                return 0.0;

            return gatedSum / static_cast<double> (gatedCount);
        }
    }

    //=============================================================
    /** Measure the final output signal.
     *
     *  @param normalisedChannels  the sound represented by the actual data
     *                             bytes that will be written, one vector per
     *                             channel, all of equal length
     *
     *  Each channel is filtered independently from zero history through
     *  both biquads; the per-channel squared values are then summed, so a
     *  mono signal is not down-mixed and receives no extra weighting.
     *  Filter history is never reset between windows.
     */
    inline LoudnessMeasurements measureLoudness (const std::vector<std::vector<double> >& normalisedChannels)
    {
        LoudnessMeasurements result;

        const size_t numChannels = normalisedChannels.size();
        if (numChannels == 0)
            return result;

        const size_t numFrames = normalisedChannels[0].size();
        for (const auto& channel : normalisedChannels)
        {
            if (channel.size() != numFrames)
                return result;
        }

        constexpr uint32_t sampleRate = 48000;
        const size_t hop = sampleRate / 10;                 // 4800 frames, 100 ms
        const size_t momentaryLength = sampleRate * 4 / 10; // 19200 frames, 400 ms
        const size_t shortTermLength = sampleRate * 3;      // 144000 frames, 3 s

        std::vector<KWeightingChannel> filters (numChannels);

        // Per-frame summed per-channel squared signal, retained so each
        // sliding window can subtract the samples that leave it. The
        // filter history itself is never reset between windows.
        std::vector<double> summedSquares;
        summedSquares.resize (numFrames);

        for (size_t frame = 0; frame < numFrames; ++frame)
        {
            double value = 0.0;
            for (size_t c = 0; c < numChannels; ++c)
            {
                const double filtered = filters[c].process (normalisedChannels[c][frame]);
                value += filtered * filtered;
            }
            summedSquares[frame] = value;
        }

        std::vector<double> momentaryEnergies;
        std::vector<double> shortTermEnergies;
        momentaryEnergies.reserve (numFrames / hop + 1);
        shortTermEnergies.reserve (numFrames / hop + 1);

        // Running sums over the current frame-0-aligned sliding windows.
        double momentaryWindowSum = 0.0;
        double shortTermWindowSum = 0.0;

        for (size_t frame = 0; frame < numFrames; ++frame)
        {
            momentaryWindowSum += summedSquares[frame];
            shortTermWindowSum += summedSquares[frame];

            if (frame >= momentaryLength)
                momentaryWindowSum -= summedSquares[frame - momentaryLength];
            if (frame >= shortTermLength)
                shortTermWindowSum -= summedSquares[frame - shortTermLength];

            const bool windowComplete = ((frame + 1) % hop) == 0;
            if (! windowComplete)
                continue;

            // Only complete, frame-0-aligned windows are recorded and the
            // non-multiple-of-100 ms tail is never padded with zeros.
            if (frame + 1 >= momentaryLength)
                momentaryEnergies.push_back (momentaryWindowSum / static_cast<double> (momentaryLength));

            if (frame + 1 >= shortTermLength)
                shortTermEnergies.push_back (shortTermWindowSum / static_cast<double> (shortTermLength));
        }

        // -----------------------------------------------------------
        // Integrated loudness: 400 ms windows, BS.1770 dual gating.
        if (! momentaryEnergies.empty())
        {
            const double meanEnergy = detail::gatedMeanEnergy (momentaryEnergies);
            if (meanEnergy > 0.0)
            {
                result.integrated = detail::loudnessFromEnergy (meanEnergy);
                result.integratedKnown = std::isfinite (result.integrated);
            }

            double maxEnergy = 0.0;
            for (double energy : momentaryEnergies)
                maxEnergy = std::max (maxEnergy, energy);

            if (maxEnergy > 0.0)
            {
                result.maxMomentary = detail::loudnessFromEnergy (maxEnergy);
                result.maxMomentaryKnown = std::isfinite (result.maxMomentary);
            }
        }

        // -----------------------------------------------------------
        // Max short-term loudness: every complete 3 s window.
        if (! shortTermEnergies.empty())
        {
            double maxEnergy = 0.0;
            for (double energy : shortTermEnergies)
                maxEnergy = std::max (maxEnergy, energy);

            if (maxEnergy > 0.0)
            {
                result.maxShortTerm = detail::loudnessFromEnergy (maxEnergy);
                result.maxShortTermKnown = std::isfinite (result.maxShortTerm);
            }
        }

        // -----------------------------------------------------------
        // LRA per EBU Tech 3342: 3 s windows, absolute gate then a
        // relative gate 20 LU below the absolute-gated mean.
        if (! shortTermEnergies.empty())
        {
            double absoluteSum = 0.0;
            size_t absoluteCount = 0;
            for (double energy : shortTermEnergies)
            {
                if (energy > 0.0 && detail::loudnessFromEnergy (energy) > -70.0)
                {
                    absoluteSum += energy;
                    ++absoluteCount;
                }
            }

            if (absoluteCount > 0)
            {
                const double absoluteMean = absoluteSum / static_cast<double> (absoluteCount);
                const double relativeThresholdEnergy = absoluteMean * std::pow (10.0, -2.0);

                std::vector<double> gatedLoudness;
                for (double energy : shortTermEnergies)
                {
                    if (energy > 0.0
                        && detail::loudnessFromEnergy (energy) > -70.0
                        && energy > relativeThresholdEnergy)
                    {
                        gatedLoudness.push_back (detail::loudnessFromEnergy (energy));
                    }
                }

                if (gatedLoudness.size() == 1)
                {
                    result.lra = 0.0;
                    result.lraKnown = true;
                    result.lraSingleWindow = true;
                }
                else if (gatedLoudness.size() >= 2)
                {
                    std::sort (gatedLoudness.begin(), gatedLoudness.end());
                    const double p95 = detail::percentile (gatedLoudness, 0.95);
                    const double p10 = detail::percentile (gatedLoudness, 0.10);
                    result.lra = p95 - p10;
                    result.lraKnown = std::isfinite (result.lra);
                }
            }
        }

        return result;
    }

    //=============================================================
    /** Round to the nearest integer, with halves rounded away from zero
     *  ("positive and negative mid-points move away from zero"). */
    inline int64_t roundAwayFromZero (double value)
    {
        if (! std::isfinite (value))
            return 0;

        return value >= 0.0 ? static_cast<int64_t> (std::floor (value + 0.5))
                            : static_cast<int64_t> (std::ceil (value - 0.5));
    }

    /** Encode a loudness value in LUFS into the bext centi-LUFS field.
     *  Unknown values, non-finite values and values outside the int16
     *  range are written as the BWF unknown marker. */
    inline int16_t encodeBextValue (bool known, double loudness)
    {
        if (! known || ! std::isfinite (loudness))
            return 0x7fff;

        const int64_t scaled = roundAwayFromZero (loudness * 100.0);
        if (scaled < static_cast<int64_t> (std::numeric_limits<int16_t>::min())
            || scaled > static_cast<int64_t> (std::numeric_limits<int16_t>::max()))
            return 0x7fff;

        return static_cast<int16_t> (scaled);
    }

    //=============================================================
    /** Append a chunk (FourCC, size, payload, RIFF padding) to a file. */
    inline void appendChunk (Bytes& fileData, const char* id, const Bytes& payload)
    {
        const uint32_t size = static_cast<uint32_t> (payload.size());
        fileData.insert (fileData.end(), id, id + 4);
        const size_t sizePosition = fileData.size();
        fileData.resize (sizePosition + 4);
        wr32 (fileData, sizePosition, size);
        fileData.insert (fileData.end(), payload.begin(), payload.end());
        if ((size & 1u) != 0)
            fileData.push_back (0);
    }

    inline void appendChunk (Bytes& fileData, const char* id, const uint8_t* data, size_t size)
    {
        Bytes payload (data, data + size);
        appendChunk (fileData, id, payload);
    }

    //=============================================================
    /** Build a fmt chunk payload matching the existing encoder rules. */
    inline Bytes makeFmtPayload (uint16_t formatTag, uint16_t channels, uint32_t rate,
                                 uint16_t bitsPerSample)
    {
        const uint16_t blockAlign = static_cast<uint16_t> (channels * (bitsPerSample / 8u));
        const uint32_t bytesPerSecond = rate * blockAlign;
        const bool floatFormat = formatTag == 0x0003;

        Bytes payload;
        payload.resize (floatFormat ? 18 : 16);
        wr16 (payload, 0, formatTag);
        wr16 (payload, 2, channels);
        wr32 (payload, 4, rate);
        wr32 (payload, 8, bytesPerSecond);
        wr16 (payload, 12, blockAlign);
        wr16 (payload, 14, bitsPerSample);
        if (floatFormat)
            wr16 (payload, 16, 0); // extension size
        return payload;
    }

    //=============================================================
    /** A zero-initialised 602-byte bext chunk with Version set to 2. */
    inline Bytes makeDefaultBext()
    {
        Bytes bext (602, 0);
        wr16 (bext, 346, 2); // Version
        static const size_t loudnessOffsets[] = { 412u, 414u, 416u, 418u, 420u };
        for (size_t offset : loudnessOffsets)
            wr16 (bext, offset, 0x7fff); // unknown loudness / true peak
        return bext;
    }

    //=============================================================
    /** Rebuild a BWF file around the original container skeleton.
     *
     *  The fmt and data chunks are replaced in place; the bext chunk is
     *  updated in place (or inserted directly before fmt when absent);
     *  iXML is updated in place from iXMLChunk or appended; the fact chunk
     *  has its frame count refreshed. Every other chunk, including its
     *  original (possibly non-zero) padding byte, is copied verbatim. New
     *  or replaced chunks are padded with zero per the RIFF rule.
     */
    inline bool buildBwfFile (const RiffContainer* container,
                              const std::string& iXMLChunk,
                              uint16_t formatTag,
                              uint16_t channels,
                              uint32_t rate,
                              uint16_t bitsPerSample,
                              const Bytes& dataPayload,
                              uint32_t frameCount,
                              const LoudnessMeasurements& loudness,
                              Bytes& output)
    {
        output.clear();
        output.reserve (container ? 4096 + dataPayload.size() : 1024 + dataPayload.size());

        static const uint8_t riffHeader[12] = {
            'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'
        };
        output.insert (output.end(), riffHeader, riffHeader + 12);

        const Bytes fmtPayload = makeFmtPayload (formatTag, channels, rate, bitsPerSample);

        // bext payload to write: the preserved chunk (measurement fields
        // refreshed), or a fresh zero-initialised Version-2 chunk. A BWF
        // file always carries a bext chunk.
        Bytes bextPayload;
        if (container != nullptr && container->bextIndex != -1)
        {
            bextPayload = container->chunks[static_cast<size_t> (container->bextIndex)].payload;
        }
        else
        {
            bextPayload = makeDefaultBext();
        }
        const bool haveBext = true;

        wr16 (bextPayload, 346, 2); // Version = 2
        wr16 (bextPayload, 412, encodeBextValue (loudness.integratedKnown, loudness.integrated));
        wr16 (bextPayload, 414, encodeBextValue (loudness.lraKnown, loudness.lra));
        wr16 (bextPayload, 416, 0x7fff); // true peak: never measured
        wr16 (bextPayload, 418, encodeBextValue (loudness.maxMomentaryKnown, loudness.maxMomentary));
        wr16 (bextPayload, 420, encodeBextValue (loudness.maxShortTermKnown, loudness.maxShortTerm));

        const Bytes ixmlPayload (iXMLChunk.begin(), iXMLChunk.end());
        const bool writeIxml = ! iXMLChunk.empty();

        // fact payload: refresh the leading sample/frame DWORD, keeping
        // any remaining bytes of the original payload.
        Bytes factPayload;
        bool haveFact = false;
        if (container != nullptr && container->factIndex != -1)
        {
            factPayload = container->chunks[static_cast<size_t> (container->factIndex)].payload;
            wr32 (factPayload, 0, frameCount);
            haveFact = true;
        }
        else if (formatTag == 0x0003)
        {
            // IEEE float output without a fact chunk gains a minimal one.
            factPayload.resize (4);
            wr32 (factPayload, 0, frameCount);
            haveFact = true;
        }

        if (container == nullptr)
        {
            // Brand-new object: canonical ordering, bext before fmt.
            appendChunk (output, "bext", bextPayload);
            appendChunk (output, "fmt ", fmtPayload);
            if (haveFact)
                appendChunk (output, "fact", factPayload);
            appendChunk (output, "data", dataPayload);
            if (writeIxml)
                appendChunk (output, "iXML", ixmlPayload);
        }
        else
        {
            bool insertedBext = haveBext && container->bextIndex == -1;
            bool insertedFact = haveFact && container->factIndex == -1;
            bool appendedIxml = writeIxml && container->ixmlIndex == -1;

            for (size_t i = 0; i < container->chunks.size(); ++i)
            {
                const RiffChunk& chunk = container->chunks[i];

                if (fourCC (chunk.id, "fmt "))
                {
                    if (insertedBext)
                    {
                        appendChunk (output, "bext", bextPayload);
                        insertedBext = false;
                    }
                    appendChunk (output, "fmt ", fmtPayload);
                }
                else if (fourCC (chunk.id, "data"))
                {
                    if (insertedFact)
                    {
                        appendChunk (output, "fact", factPayload);
                        insertedFact = false;
                    }
                    appendChunk (output, "data", dataPayload);
                }
                else if (fourCC (chunk.id, "bext"))
                {
                    appendChunk (output, "bext", bextPayload);
                }
                else if (fourCC (chunk.id, "fact"))
                {
                    appendChunk (output, "fact", factPayload);
                }
                else if (fourCC (chunk.id, "iXML"))
                {
                    if (writeIxml)
                        appendChunk (output, "iXML", ixmlPayload);
                    // an emptied iXML chunk drops out of the file
                }
                else
                {
                    // Unknown chunk: preserve payload and original padding
                    // byte exactly, including repeated FourCCs.
                    const size_t chunkStart = output.size();
                    output.insert (output.end(), chunk.id.data(), chunk.id.data() + 4);
                    output.resize (chunkStart + 8);
                    wr32 (output, chunkStart + 4, chunk.size);
                    output.insert (output.end(), chunk.payload.begin(), chunk.payload.end());
                    if (chunk.hasPad)
                        output.push_back (chunk.padByte);
                }
            }

            if (insertedBext)
                appendChunk (output, "bext", bextPayload);
            if (insertedFact)
                appendChunk (output, "fact", factPayload);
            if (appendedIxml)
                appendChunk (output, "iXML", ixmlPayload);
        }

        // The RIFF size covers everything after the leading 8 bytes.
        if (output.size() < 8 || output.size() - 8 > static_cast<size_t> (std::numeric_limits<uint32_t>::max()))
            return false;

        wr32 (output, 4, static_cast<uint32_t> (output.size() - 8));
        return true;
    }
}

#endif /* _AS_BwfSupport_h */
