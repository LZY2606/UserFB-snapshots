/// @file writer.hpp
#pragma once
#include <algorithm>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <stdint.h>
#include <string>
#include <type_traits>
#include <vector>
#include "chunks.hpp"
#include "utils.hpp"

namespace bw64 {

  const uint32_t MAX_NUMBER_OF_UIDS = 1024;

  /**
   * @brief BW64 Writer class
   *
   * Normally, you will create an instance of this class using
   * bw64::writeFile().
   *
   * This is a
   * [RAII](https://en.wikipedia.org/wiki/Resource_acquisition_is_initialization)
   * class, meaning that the file will be opened and initialized (required
   * headers etc.) on construction, and closed and finalized (writing chunk
   * sizes etc.) on destruction.
   */
  class Bw64Writer {
   public:
    /**
     * @brief Open a new BW64 file for writing
     *
     * Opens a new BW64 file for writing, initializes everything up to the
     * `data` chunk. Afterwards, you may write interleaved audio samples to this
     * file.
     *
     * @warning If the file already exists it will be overwritten.
     *
     * If you need any chunks to appear *before* the data chunk, include them in
     * the `additionalChunks`. They will be written directly after opening the
     * file.
     *
     * @note For convenience, you might consider using the `writeFile` helper
     * function.
     */
    Bw64Writer(const char* filename, uint16_t channels, uint32_t sampleRate,
               uint16_t bitDepth,
               std::vector<std::shared_ptr<Chunk>> additionalChunks) {
      output_ = &fileStream_;
      fileStream_.open(filename, std::fstream::out | std::fstream::binary);
      if (!fileStream_.is_open()) {
        std::stringstream errorString;
        errorString << "Could not open file: " << filename;
        throw std::runtime_error(errorString.str());
      }
      writeRiffHeader();
      // 28 byte ds64 header + 12 byte entry for axml
      writeChunkPlaceholder(utils::fourCC("JUNK"), 40u);
      auto formatChunk =
          std::make_shared<FormatInfoChunk>(channels, sampleRate, bitDepth);
      writeChunk(formatChunk);

      for (auto chunk : additionalChunks) {
        writeChunk(chunk);
      }
      if (!chnaChunk()) {
        writeChunkPlaceholder(utils::fourCC("chna"),
                              MAX_NUMBER_OF_UIDS * 40 + 4);
      }
      auto dataChunk = std::make_shared<DataChunk>();
      writeChunk(dataChunk);
    }

    /**
     * @brief Open a new RIFF or BW64 stream for forward writing
     *
     * Writes a complete, final header (up to and including the `data` chunk
     * header) to a non-seekable stream, based on the exact number of frames
     * and the metadata known up-front. Depending on the resulting file size,
     * either a RIFF or a BW64 header (with a `ds64` chunk holding the real
     * sizes) is written. Afterwards, exactly `frames` frames of audio data
     * must be written with write(); close() does not rewrite any part of the
     * header.
     *
     * The stream is owned by the caller and is not closed by this class.
     * Sizes which can not be represented are reported before any byte is
     * written. setChnaChunk(), setAxmlChunk() and useRf64Id() are not
     * supported in this mode and throw `std::logic_error`.
     *
     * @note For convenience, you might consider using the `writeStream`
     * helper function.
     */
    Bw64Writer(std::ostream& stream, uint64_t frames, uint16_t channels,
               uint32_t sampleRate, uint16_t bitDepth,
               std::shared_ptr<ChnaChunk> chnaChunk,
               std::shared_ptr<AxmlChunk> axmlChunk)
        : output_(&stream), forwardMode_(true), declaredFrames_(frames) {
      auto formatChunk =
          std::make_shared<FormatInfoChunk>(channels, sampleRate, bitDepth);

      // checked size of the data chunk payload
      uint16_t blockAlignment = formatChunk->blockAlignment();
      if (blockAlignment != 0 &&
          frames > (std::numeric_limits<uint64_t>::max)() / blockAlignment) {
        throw std::runtime_error(
            "number of frames is too large to be represented");
      }
      uint64_t dataSize = frames * blockAlignment;

      std::vector<std::shared_ptr<Chunk>> preDataChunks;
      preDataChunks.push_back(formatChunk);
      if (chnaChunk) preDataChunks.push_back(chnaChunk);
      if (axmlChunk) preDataChunks.push_back(axmlChunk);

      // ds64 table entries for oversized chunks other than the data chunk
      std::map<uint32_t, uint64_t> ds64Table;
      for (auto& chunk : preDataChunks) {
        if (chunk->size() >= UINT32_MAX) ds64Table[chunk->id()] = chunk->size();
      }
      uint64_t ds64Size = 28u + 12u * ds64Table.size();

      // checked size of a chunk on disk: header + payload + padding
      auto chunkTotalSize = [](uint64_t payloadSize) {
        return utils::safeAdd<uint64_t>(
            utils::safeAdd<uint64_t>(8u, payloadSize), payloadSize % 2);
      };
      auto riffSizeFor = [&](bool withDs64) {
        uint64_t size = 4u;  // WAVE id
        if (withDs64) size = utils::safeAdd<uint64_t>(size, 8u + ds64Size);
        for (auto& chunk : preDataChunks) {
          size = utils::safeAdd<uint64_t>(size, chunkTotalSize(chunk->size()));
        }
        return utils::safeAdd<uint64_t>(size, chunkTotalSize(dataSize));
      };

      bool need64 = dataSize >= UINT32_MAX || !ds64Table.empty();
      uint64_t riffSize = riffSizeFor(need64);
      if (!need64 && riffSize > UINT32_MAX) {
        need64 = true;
        riffSize = riffSizeFor(true);
      }

      if (need64) {
        utils::writeValue(out(), utils::fourCC("BW64"));
        utils::writeValue(out(), (std::numeric_limits<uint32_t>::max)());
        utils::writeValue(out(), utils::fourCC("WAVE"));
        auto ds64Chunk = std::make_shared<DataSize64Chunk>(
            riffSize, dataSize, ds64Table);
        ds64Chunk->dummySize(frames);  // sample count
        writeChunk(ds64Chunk);
      } else {
        utils::writeValue(out(), utils::fourCC("RIFF"));
        utils::writeValue(out(), utils::safeCast<uint32_t>(riffSize));
        utils::writeValue(out(), utils::fourCC("WAVE"));
      }

      for (auto& chunk : preDataChunks) {
        writeChunk(chunk);
      }

      auto dataChunk = std::make_shared<DataChunk>();
      dataChunk->setSize(dataSize);
      writeChunk(dataChunk);

      if (!out().good())
        throw std::runtime_error("stream error while writing header");
    }

    /// finalise and close the file
    ///
    /// Write all yet-to-be-written chunks to the file and finalize all
    /// required information, i.e. the final chunk sizes etc.
    ///
    /// It is recommended to call this before the destructor, to handle
    /// exceptions. If it does throw, this object may be in an invalid state,
    /// so do not try again without creating a new object.
    void close() {
      if (forwardMode_) {
        if (forwardClosed_) return;
        // mark closed first, so the destructor does not throw the same
        // exception again
        forwardClosed_ = true;
        if (forwardFramesWritten_ != declaredFrames_)
          throw std::runtime_error(
              "fewer frames written than declared in the header");
        // the header is final; only the data chunk padding may be missing
        if (dataChunk()->size() % 2 == 1) {
          utils::writeValue(out(), '\0');
        }
        out().flush();
        if (!out().good())
          throw std::runtime_error("stream error detected when closing");
        return;
      }

      if (!fileStream_.is_open()) return;

      try {
        finalizeDataChunk();
        for (auto chunk : postDataChunks_) {
          writeChunk(chunk);
        }
        finalizeRiffChunk();
        fileStream_.close();
      } catch (...) {
        // ensure that if an exception is thrown the file is still closed, so
        // the destructor does not throw the same exception
        fileStream_.close();
        throw;
      }

      if (!fileStream_.good())
        throw std::runtime_error("file error detected when closing");
    }

    /// destructor; this will finalise and close the file if it has not
    /// already been done, but it is recommended to call close() first to
    /// handle exceptions. Exceptions are not propagated from the destructor.
    ~Bw64Writer() {
      try {
        close();
      } catch (...) {
      }
    }

    /// @brief Get format tag
    uint16_t formatTag() const { return formatChunk()->formatTag(); };
    /// @brief Get number of channels
    uint16_t channels() const { return formatChunk()->channelCount(); };
    /// @brief Get sample rate
    uint32_t sampleRate() const { return formatChunk()->sampleRate(); };
    /// @brief Get bit depth
    uint16_t bitDepth() const { return formatChunk()->bitsPerSample(); };
    /// @brief Get number of frames
    uint64_t framesWritten() const {
      if (forwardMode_) return forwardFramesWritten_;
      return dataChunk()->size() / formatChunk()->blockAlignment();
    }

    template <typename ChunkType>
    std::vector<std::shared_ptr<ChunkType>> chunksWithId(
        const std::vector<Chunk>& chunks, uint32_t chunkId) const {
      std::vector<char> foundChunks;
      auto chunk =
          std::copy_if(chunks.begin(), chunks.end(), foundChunks.begin(),
                       [chunkId](const std::shared_ptr<Chunk> chunk) {
                         return chunk->id() == chunkId;
                       });
      return foundChunks;
    }

    template <typename ChunkType>
    std::shared_ptr<ChunkType> chunk(
        const std::vector<std::shared_ptr<Chunk>>& chunks,
        uint32_t chunkId) const {
      auto chunk = std::find_if(chunks.begin(), chunks.end(),
                                [chunkId](const std::shared_ptr<Chunk> chunk) {
                                  return chunk->id() == chunkId;
                                });
      if (chunk != chunks.end()) {
        return std::static_pointer_cast<ChunkType>(*chunk);
      } else {
        return nullptr;
      }
    }

    std::shared_ptr<DataSize64Chunk> ds64Chunk() const {
      return chunk<DataSize64Chunk>(chunks_, utils::fourCC("ds64"));
    }
    std::shared_ptr<FormatInfoChunk> formatChunk() const {
      return chunk<FormatInfoChunk>(chunks_, utils::fourCC("fmt "));
    }
    std::shared_ptr<DataChunk> dataChunk() const {
      return chunk<DataChunk>(chunks_, utils::fourCC("data"));
    }
    std::shared_ptr<ChnaChunk> chnaChunk() const {
      return chunk<ChnaChunk>(chunks_, utils::fourCC("chna"));
    }
    std::shared_ptr<AxmlChunk> axmlChunk() const {
      return chunk<AxmlChunk>(chunks_, utils::fourCC("axml"));
    }

    /// @brief Check if file is bigger than 4GB and therefore a BW64 file
    bool isBw64File() {
      if (riffChunkSize() > UINT32_MAX) {
        return true;
      }

      for (auto& header : chunkHeaders_)
        if (header.size > UINT32_MAX) return true;

      return false;
    }

    /// @brief Use RF64 ID for outer chunk (when >4GB) rather than BW64
    void useRf64Id(bool state) {
      if (forwardMode_)
        throw std::logic_error(
            "useRf64Id is not supported in forward writing mode");
      useRf64Id_ = state;
    }

    void setChnaChunk(std::shared_ptr<ChnaChunk> chunk) {
      if (forwardMode_)
        throw std::logic_error(
            "setChnaChunk is not supported in forward writing mode");
      if (chunk->numUids() > 1024) {
        // TODO: make pre data chunk chna chunk a JUNK chunk and add chnaChunk
        // to postDataChunks_?
        throw std::runtime_error("number of trackUids is > 1024");
      }
      auto last_position = fileStream_.tellp();
      overwriteChunk(utils::fourCC("chna"), chunk);
      fileStream_.seekp(last_position);
    }

    void setAxmlChunk(std::shared_ptr<Chunk> chunk) {
      if (forwardMode_)
        throw std::logic_error(
            "setAxmlChunk is not supported in forward writing mode");
      postDataChunks_.push_back(chunk);
    }

    /// @brief Get the chunk size for header
    uint32_t chunkSizeForHeader(uint32_t id) {
      if (chunkHeader(id).size >= UINT32_MAX) {
        return UINT32_MAX;
      } else {
        return static_cast<uint32_t>(chunkHeader(id).size);
      }
    }

    /// @brief Calculate riff chunk size
    uint64_t riffChunkSize() {
      auto last_position = fileStream_.tellp();
      fileStream_.seekp(0, std::ios::end);
      uint64_t endPos = fileStream_.tellp();
      fileStream_.seekp(last_position);
      return endPos - 8u;
    }

    /// @brief Write RIFF header
    void writeRiffHeader() {
      uint32_t RiffId = utils::fourCC("RIFF");
      uint32_t fileSize = UINT32_MAX;
      uint32_t WaveId = utils::fourCC("WAVE");
      utils::writeValue(fileStream_, RiffId);
      utils::writeValue(fileStream_, fileSize);
      utils::writeValue(fileStream_, WaveId);
    }

    /// @brief Update RIFF header
    void finalizeRiffChunk() {
      auto last_position = fileStream_.tellp();
      fileStream_.seekp(0);
      if (isBw64File()) {
        utils::writeValue(fileStream_,
                          utils::fourCC(useRf64Id_ ? "RF64" : "BW64"));
        utils::writeValue(fileStream_, (std::numeric_limits<uint32_t>::max)());
        overwriteJunkWithDs64Chunk();
      } else {
        utils::writeValue(fileStream_, utils::fourCC("RIFF"));
        uint32_t fileSize = static_cast<uint32_t>(riffChunkSize());
        utils::writeValue(fileStream_, fileSize);
      }
      fileStream_.seekp(last_position);
    }

    void overwriteJunkWithDs64Chunk() {
      auto ds64Chunk = std::make_shared<DataSize64Chunk>();
      ds64Chunk->bw64Size(riffChunkSize());
      // write data size even if it's not too big
      ds64Chunk->dataSize(dataChunk()->size());

      for (auto& header : chunkHeaders_)
        if (header.size > UINT32_MAX)
          ds64Chunk->setChunkSize(header.id, header.size);

      overwriteChunk(utils::fourCC("JUNK"), ds64Chunk);
    }

    void finalizeDataChunk() {
      if (dataChunk()->size() % 2 == 1) {
        utils::writeValue(fileStream_, '\0');
      }
      auto last_position = fileStream_.tellp();
      seekChunk(utils::fourCC("data"));
      utils::writeValue(fileStream_, utils::fourCC("data"));
      utils::writeValue(fileStream_, chunkSizeForHeader(utils::fourCC("data")));
      fileStream_.seekp(last_position);
    }

    /// @brief Write chunk template
    template <typename ChunkType>
    void writeChunk(std::shared_ptr<ChunkType> chunk) {
      if (chunk) {
        uint64_t position =
            forwardMode_ ? 0 : static_cast<uint64_t>(out().tellp());
        chunkHeaders_.push_back(
            ChunkHeader(chunk->id(), chunk->size(), position));
        utils::writeChunk<ChunkType>(out(), chunk,
                                     chunkSizeForHeader(chunk->id()));
        chunks_.push_back(chunk);
      }
    }

    void writeChunkPlaceholder(uint32_t id, uint32_t size) {
      uint64_t position = fileStream_.tellp();
      chunkHeaders_.push_back(ChunkHeader(id, size, position));
      utils::writeChunkPlaceholder(fileStream_, id, size);
    }

    /// @brief Overwrite chunk template
    template <typename ChunkType>
    void overwriteChunk(uint32_t id, std::shared_ptr<ChunkType> chunk) {
      if (chunk->size() > chunkHeader(id).size) {
        std::stringstream errorMsg;
        errorMsg << utils::fourCCToStr(chunk->id()) << " chunk is too large ("
                 << chunk->size() << " bytes) to overwrite "
                 << utils::fourCCToStr(id) << " chunk (" << chunkHeader(id).size
                 << " bytes)";
        throw std::runtime_error(errorMsg.str());
      }

      auto last_position = fileStream_.tellp();
      seekChunk(id);
      utils::writeChunk<ChunkType>(fileStream_, chunk, chunkSizeForHeader(id));
      fileStream_.seekp(last_position);
    }

    void seekChunk(uint32_t id) {
      auto header = chunkHeader(id);
      fileStream_.clear();
      fileStream_.seekp(header.position);
    }

    ChunkHeader& chunkHeader(uint32_t id) {
      auto foundHeader = std::find_if(
          chunkHeaders_.begin(), chunkHeaders_.end(),
          [id](const ChunkHeader header) { return header.id == id; });
      if (foundHeader != chunkHeaders_.end()) {
        return *foundHeader;
      }
      std::stringstream errorMsg;
      errorMsg << "no chunk with id '" << utils::fourCCToStr(id) << "' found";
      throw std::runtime_error(errorMsg.str());
    }

    /**
     * @brief Write frames to dataChunk
     *
     * @param[out] inBuffer Buffer to read samples from
     * @param[in]  frames   Number of frames to write
     *
     * @returns number of frames written
     */
    template <typename T, typename std::enable_if<
                              std::is_floating_point<T>::value, int>::type = 0>
    uint64_t write(T* inBuffer, uint64_t frames) {
      if (forwardMode_) {
        if (forwardClosed_)
          throw std::logic_error("cannot write to a closed Bw64Writer");
        if (frames > declaredFrames_ - forwardFramesWritten_)
          throw std::runtime_error(
              "write would exceed the number of frames declared in the "
              "header");
      }
      uint64_t bytesWritten = frames * formatChunk()->blockAlignment();
      rawDataBuffer_.resize(bytesWritten);
      utils::encodePcmSamples(inBuffer, rawDataBuffer_.data(),
                              frames * formatChunk()->channelCount(),
                              formatChunk()->bitsPerSample());
      if (bytesWritten) {
        out().write(rawDataBuffer_.data(), bytesWritten);
      }
      if (forwardMode_) {
        if (!out().good())
          throw std::runtime_error("stream error while writing frames");
        forwardFramesWritten_ += frames;
      } else {
        dataChunk()->setSize(dataChunk()->size() + bytesWritten);
        chunkHeader(utils::fourCC("data")).size = dataChunk()->size();
      }
      return frames;
    }

   private:
    std::ostream& out() { return *output_; }

    std::ofstream fileStream_;
    std::ostream* output_ = nullptr;
    bool forwardMode_ = false;
    bool forwardClosed_ = false;
    uint64_t declaredFrames_ = 0;
    uint64_t forwardFramesWritten_ = 0;
    std::vector<char> rawDataBuffer_;
    std::vector<std::shared_ptr<Chunk>> chunks_;
    std::vector<ChunkHeader> chunkHeaders_;
    std::vector<std::shared_ptr<Chunk>> postDataChunks_;
    bool useRf64Id_{false};
  };

}  // namespace bw64
