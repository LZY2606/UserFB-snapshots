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
#include "metadata.hpp"
#include "utils.hpp"

namespace bw64 {

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
      filename_ = filename;
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

    /// finalise and close the file
    ///
    /// Write all yet-to-be-written chunks to the file and finalize all
    /// required information, i.e. the final chunk sizes etc.
    ///
    /// It is recommended to call this before the destructor, to handle
    /// exceptions. If it does throw, this object may be in an invalid state,
    /// so do not try again without creating a new object.
    void close() {
      if (!fileStream_.is_open()) return;

      try {
        if (reserveMode_) {
          writeMetadataArena();
        }
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
    /// handle exceptions
    ~Bw64Writer() { close(); }

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
    void useRf64Id(bool state) { useRf64Id_ = state; }

    /**
     * @brief Reserve a fixed-size editable metadata arena before the data
     * chunk.
     *
     * Must be called at most once, after opening the file and before the
     * first non-zero-length write. @p bytes is the physical size of the
     * whole reserved area (chunk headers, even-byte alignment and the
     * trailing JUNK included); it must be even and between 8 bytes and
     * 16 MiB. Any chna/axml chunks passed at construction are moved into the
     * reserved area and their old positions are discarded when the header
     * prefix is rebuilt, so no stale copy remains.
     *
     * Invalid calls leave the file and the writer state unchanged; a legal
     * call may follow an invalid one.
     */
    void reserveMetadata(uint32_t bytes) {
      if (!fileStream_.is_open()) {
        throw std::logic_error(
            "reserveMetadata() must not be called after close()");
      }
      if (reserveMode_) {
        throw std::logic_error("reserveMetadata() can only be called once");
      }
      if (framesWritten() != 0u) {
        throw std::logic_error(
            "reserveMetadata() must be called before the first audio write");
      }
      if (bytes < METADATA_ARENA_MIN_SIZE ||
          bytes > METADATA_ARENA_MAX_SIZE || (bytes % 2u) != 0u) {
        std::stringstream msg;
        msg << "reserveMetadata: " << bytes
            << " bytes is not an even size between "
            << METADATA_ARENA_MIN_SIZE << " and " << METADATA_ARENA_MAX_SIZE;
        throw std::invalid_argument(msg.str());
      }

      // snapshot the initial metadata; the chna placeholder is only a
      // header, so a real chunk exists only when it was passed explicitly
      std::shared_ptr<ChnaChunk> initialChna = chnaChunk();
      std::shared_ptr<AxmlChunk> initialAxml = axmlChunk();
      currentChna_ = initialChna ? detail::cloneChna(*initialChna)
                                 : std::shared_ptr<ChnaChunk>();
      currentAxml_ = initialAxml ? detail::cloneAxml(*initialAxml)
                                 : std::shared_ptr<AxmlChunk>();
      if (currentChna_) {
        detail::validateChnaForChannels(*currentChna_, channels());
      }

      // build the new arena (initial chna first, then axml, trailing JUNK)
      // before touching the file, so a capacity or layout failure leaves the
      // file and the writer state unchanged
      const std::vector<char> initialArena = detail::buildMetadataArena(
          bytes, currentChna_.get(), currentAxml_.get());

      // no audio has been written yet, so rebuild the whole header-only
      // prefix: RIFF, ds64 JUNK placeholder, fmt, any other pre-data chunks,
      // the reserved arena and the data chunk header
      auto fmt = formatChunk();
      auto data = dataChunk();
      std::vector<std::shared_ptr<Chunk>> otherPreData;
      for (size_t i = 0; i < chunks_.size(); ++i) {
        const uint32_t id = chunks_[i]->id();
        if (id != utils::fourCC("fmt ") && id != utils::fourCC("data") &&
            id != utils::fourCC("chna") && id != utils::fourCC("axml")) {
          otherPreData.push_back(chunks_[i]);
        }
      }

      fileStream_.close();
      // an ofstream opened with out truncates, so this rebuilds the prefix
      // from scratch; a failed open leaves the previous file in place
      fileStream_.clear();
      fileStream_.open(filename_,
                       std::fstream::out | std::fstream::binary |
                           std::fstream::trunc);
      if (!fileStream_.is_open()) {
        std::stringstream msg;
        msg << "reserveMetadata: failed to reopen " << filename_;
        throw std::runtime_error(msg.str());
      }

      chunkHeaders_.clear();
      chunks_.clear();
      writeRiffHeader();
      // 28 byte ds64 header + 12 byte entry for axml
      writeChunkPlaceholder(utils::fourCC("JUNK"), 40u);
      writeChunk(fmt);
      for (auto chunk : otherPreData) {
        writeChunk(chunk);
      }
      const uint64_t arenaPosition =
          static_cast<uint64_t>(fileStream_.tellp());
      fileStream_.write(initialArena.data(),
                        static_cast<std::streamsize>(initialArena.size()));
      if (!fileStream_.good()) {
        throw std::runtime_error(
            "failed to write the reserved metadata arena");
      }
      // bookkeeping: the arena is tracked as one JUNK-sized region
      chunkHeaders_.push_back(
          ChunkHeader(utils::fourCC("JUNK"),
                      static_cast<uint64_t>(bytes) - 8u, arenaPosition));
      writeChunk(data);

      reserveMode_ = true;
      arenaPosition_ = arenaPosition;
      arenaCapacity_ = bytes;
    }

    /**
     * @brief Replace both metadata chunks inside the reserved arena.
     *
     * Only available after reserveMetadata(); otherwise std::logic_error is
     * thrown. Either argument may be nullptr to delete that chunk. The
     * stored values are deep copies (snapshots), so later modifications of
     * the caller's chunks do not affect the file. Validation, serialization
     * or capacity failures leave the current snapshots and the file
     * unchanged; writing audio, further updates and close() still work.
     */
    void setMetadata(std::shared_ptr<ChnaChunk> chna,
                     std::shared_ptr<AxmlChunk> axml) {
      if (!fileStream_.is_open()) {
        throw std::logic_error(
            "setMetadata() must not be called after close()");
      }
      if (!reserveMode_) {
        throw std::logic_error(
            "setMetadata() requires reserveMetadata() to be called first");
      }

      std::shared_ptr<ChnaChunk> chnaSnapshot;
      std::shared_ptr<AxmlChunk> axmlSnapshot;
      if (chna) chnaSnapshot = detail::cloneChna(*chna);
      if (axml) axmlSnapshot = detail::cloneAxml(*axml);
      if (chnaSnapshot) {
        detail::validateChnaForChannels(*chnaSnapshot, channels());
      }

      // serialize fully before touching the file or replacing snapshots
      const std::vector<char> arenaBytes = detail::buildMetadataArena(
          arenaCapacity_, chnaSnapshot.get(), axmlSnapshot.get());

      writeArenaBytes(arenaBytes);
      currentChna_ = chnaSnapshot;
      currentAxml_ = axmlSnapshot;
    }

    /// current CHNA snapshot in reserved mode (nullptr if deleted)
    std::shared_ptr<ChnaChunk> reservedChnaChunk() const {
      return currentChna_;
    }
    /// current AXML snapshot in reserved mode (nullptr if deleted)
    std::shared_ptr<AxmlChunk> reservedAxmlChunk() const {
      return currentAxml_;
    }
    /// whether reserveMetadata() has been activated
    bool metadataReserved() const { return reserveMode_; }

    /**
     * @brief Replace just the CHNA chunk.
     *
     * In reserved mode this keeps the current AXML snapshot and rewrites the
     * metadata arena. Without a reservation the legacy placeholder
     * overwrite behaviour is retained.
     */
    void setChnaChunk(std::shared_ptr<ChnaChunk> chunk) {
      if (reserveMode_) {
        setMetadata(chunk, currentAxml_);
        return;
      }
      if (chunk->numUids() > 1024) {
        // TODO: make pre data chunk chna chunk a JUNK chunk and add chnaChunk
        // to postDataChunks_?
        throw std::runtime_error("number of trackUids is > 1024");
      }
      auto last_position = fileStream_.tellp();
      overwriteChunk(utils::fourCC("chna"), chunk);
      fileStream_.seekp(last_position);
    }

    /**
     * @brief Replace just the AXML chunk.
     *
     * In reserved mode this keeps the current CHNA snapshot and rewrites the
     * metadata arena; AXML never moves behind the audio data in that mode.
     * Without a reservation the legacy behaviour (the chunk, which may be any
     * Chunk type, is appended after the data chunk) is retained.
     */
    void setAxmlChunk(std::shared_ptr<Chunk> chunk) {
      if (reserveMode_) {
        auto axml = std::dynamic_pointer_cast<AxmlChunk>(chunk);
        if (!axml) {
          throw std::invalid_argument(
              "setAxmlChunk() in reserved mode requires an AxmlChunk");
        }
        setMetadata(currentChna_, axml);
        return;
      }
      postDataChunks_.push_back(chunk);
    }

    /**
     * @brief Append an arbitrary chunk after the data chunk.
     *
     * Allows unknown post-data chunks (e.g. custom FourCC payloads) to be
     * written at close() time, in both reserved and legacy modes.
     */
    void appendPostDataChunk(std::shared_ptr<Chunk> chunk) {
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
        uint64_t position = fileStream_.tellp();
        chunkHeaders_.push_back(
            ChunkHeader(chunk->id(), chunk->size(), position));
        utils::writeChunk<ChunkType>(fileStream_, chunk,
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
      uint64_t bytesWritten = frames * formatChunk()->blockAlignment();
      rawDataBuffer_.resize(bytesWritten);
      utils::encodePcmSamples(inBuffer, &rawDataBuffer_[0],
                              frames * formatChunk()->channelCount(),
                              formatChunk()->bitsPerSample());
      fileStream_.write(&rawDataBuffer_[0], bytesWritten);
      dataChunk()->setSize(dataChunk()->size() + bytesWritten);
      chunkHeader(utils::fourCC("data")).size = dataChunk()->size();
      return frames;
    }

   private:
    /// write a fully serialized arena, restoring the data write position
    void writeArenaBytes(const std::vector<char>& arenaBytes) {
      const std::streamoff dataWritePosition = fileStream_.tellp();
      fileStream_.clear();
      fileStream_.seekp(static_cast<std::streamoff>(arenaPosition_),
                        std::ios::beg);
      fileStream_.write(arenaBytes.data(),
                        static_cast<std::streamsize>(arenaBytes.size()));
      if (!fileStream_.good()) {
        throw std::runtime_error(
            "failed to write reserved metadata arena");
      }
      fileStream_.clear();
      fileStream_.seekp(dataWritePosition, std::ios::beg);
      if (!fileStream_.good()) {
        throw std::runtime_error(
            "failed to restore audio write position after metadata update");
      }
    }

    /// serialize the current snapshots into the reserved arena (used at
    /// close() time)
    void writeMetadataArena() {
      const std::vector<char> arenaBytes = detail::buildMetadataArena(
          arenaCapacity_, currentChna_.get(), currentAxml_.get());
      writeArenaBytes(arenaBytes);
    }

    std::ofstream fileStream_;
    std::string filename_;
    std::vector<char> rawDataBuffer_;
    std::vector<std::shared_ptr<Chunk>> chunks_;
    std::vector<ChunkHeader> chunkHeaders_;
    std::vector<std::shared_ptr<Chunk>> postDataChunks_;
    bool useRf64Id_{false};

    // editable pre-data metadata arena (reserveMetadata mode)
    bool reserveMode_{false};
    uint64_t arenaPosition_{0};
    uint32_t arenaCapacity_{0};
    std::shared_ptr<ChnaChunk> currentChna_;
    std::shared_ptr<AxmlChunk> currentAxml_;
  };

}  // namespace bw64
