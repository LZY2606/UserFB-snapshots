/**
 * @file bw64.hpp
 *
 * libbw64 main header file. This should be the only file you need to include
 * into your user code.
 */
#pragma once
#include "reader.hpp"
#include "writer.hpp"

namespace bw64 {

  /**
   * @brief Open a BW64 file for reading
   *
   * @param filename path of the file to read
   *
   * Convenience function to open a BW64 file for reading.
   *
   * @returns `unique_ptr` to a Bw64Reader instance that is ready to read
   * samples.
   */
  inline std::unique_ptr<Bw64Reader> readFile(const std::string& filename) {
    return std::unique_ptr<Bw64Reader>(new Bw64Reader(filename.c_str()));
  }

  /**
   * @brief Open a BW64 file for writing
   *
   * Convenience function to open a new BW64 file for writing, adding `axml` and
   * `chna` chunks.
   *
   * If passed to this function, the `axml` and `chna` chunks will be added to
   * the BW64 file *before* the actual data chunk, which is the recommended
   * practice if all components are already known before writing a file.
   *
   * @param filename path of the file to write
   * @param channels the channel count of the new file
   * @param sampleRate the samplerate of the new file
   * @param bitDepth target bitdepth of the new file
   * @param chnaChunk Channel allocation chunk to include, if any
   * @param axmlChunk AXML chunk to include, if any
   *
   * @returns `unique_ptr` to a Bw64Writer instance that is ready to write
   * samples.
   *
   */
  inline std::unique_ptr<Bw64Writer> writeFile(
      const std::string& filename, uint16_t channels = 1u,
      uint32_t sampleRate = 48000u, uint16_t bitDepth = 24u,
      std::shared_ptr<ChnaChunk> chnaChunk = nullptr,
      std::shared_ptr<AxmlChunk> axmlChunk = nullptr) {
    std::vector<std::shared_ptr<Chunk>> additionalChunks;
    if (chnaChunk) {
      additionalChunks.push_back(chnaChunk);
    }
    if (axmlChunk) {
      additionalChunks.push_back(axmlChunk);
    }
    return std::unique_ptr<Bw64Writer>(new Bw64Writer(
        filename.c_str(), channels, sampleRate, bitDepth, additionalChunks));
  }

  /**
   * @brief Open a RIFF, BW64 or RF64 stream for forward reading
   *
   * Convenience function to read a RIFF, BW64 or RF64 WAVE file from a
   * non-seekable stream. Only the input up to the start of the `data` chunk
   * payload is consumed on construction; the audio data can then be read in
   * blocks, and chunks after the `data` chunk become available once the audio
   * data has been consumed (see Bw64Reader::finish()).
   *
   * The stream is owned by the caller and is neither closed nor repositioned.
   *
   * @param stream stream to read from
   *
   * @returns `unique_ptr` to a Bw64Reader instance that is ready to read
   * samples.
   */
  inline std::unique_ptr<Bw64Reader> readStream(std::istream& stream) {
    return std::unique_ptr<Bw64Reader>(new Bw64Reader(stream));
  }

  /**
   * @brief Open a stream for forward writing
   *
   * Convenience function to write a RIFF or BW64 WAVE file to a non-seekable
   * stream. The exact number of frames and all metadata must be known
   * up-front; a complete header (RIFF or BW64, depending on the resulting
   * file size) is written immediately, and close() does not rewrite any part
   * of it.
   *
   * The stream is owned by the caller and is not closed by the writer.
   *
   * @param stream stream to write to
   * @param frames the exact number of frames which will be written
   * @param channels the channel count of the new file
   * @param sampleRate the samplerate of the new file
   * @param bitDepth target bitdepth of the new file
   * @param chnaChunk Channel allocation chunk to include, if any
   * @param axmlChunk AXML chunk to include, if any
   *
   * @returns `unique_ptr` to a Bw64Writer instance that is ready to write
   * samples.
   */
  inline std::unique_ptr<Bw64Writer> writeStream(
      std::ostream& stream, uint64_t frames, uint16_t channels = 1u,
      uint32_t sampleRate = 48000u, uint16_t bitDepth = 24u,
      std::shared_ptr<ChnaChunk> chnaChunk = nullptr,
      std::shared_ptr<AxmlChunk> axmlChunk = nullptr) {
    return std::unique_ptr<Bw64Writer>(
        new Bw64Writer(stream, frames, channels, sampleRate, bitDepth,
                       chnaChunk, axmlChunk));
  }

}  // namespace bw64
