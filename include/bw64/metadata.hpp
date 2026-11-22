/**
 * @file metadata.hpp
 *
 * Helpers for the editable pre-data metadata arena (chna/axml/JUNK) and for
 * updating the metadata of a closed file in place.
 *
 * The AXML chunk is treated as an exact byte string -- the XML itself is
 * never interpreted, and no XML parser is required.
 */
#pragma once
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <stdint.h>
#include <string>
#include <vector>
#include "chunks.hpp"
#include "utils.hpp"

namespace bw64 {

  /// maximum number of audioTrackUID entries allowed in a chna chunk
  static const uint32_t MAX_NUMBER_OF_UIDS = 1024u;

  /// minimum physical size of a reserved metadata arena (one JUNK chunk)
  static const uint32_t METADATA_ARENA_MIN_SIZE = 8u;
  /// maximum physical size of a reserved metadata arena (16 MiB)
  static const uint32_t METADATA_ARENA_MAX_SIZE = 16u * 1024u * 1024u;

  namespace detail {

    inline std::shared_ptr<ChnaChunk> cloneChna(const ChnaChunk& chunk) {
      return std::make_shared<ChnaChunk>(chunk.audioIds());
    }

    inline std::shared_ptr<AxmlChunk> cloneAxml(const AxmlChunk& chunk) {
      return std::make_shared<AxmlChunk>(chunk.data());
    }

    /// validate a CHNA against the actual number of channels: every
    /// trackIndex must be in [1, channels], and UIDs must not repeat (several
    /// UIDs on the same track are allowed)
    inline void validateChnaForChannels(const ChnaChunk& chunk,
                                        uint16_t channels) {
      if (chunk.numUids() > MAX_NUMBER_OF_UIDS) {
        std::stringstream msg;
        msg << "chna chunk has " << chunk.numUids()
            << " UIDs, maximum is " << MAX_NUMBER_OF_UIDS;
        throw std::invalid_argument(msg.str());
      }
      std::set<std::string> seenUids;
      const std::vector<AudioId> ids = chunk.audioIds();
      for (size_t i = 0; i < ids.size(); ++i) {
        const uint16_t trackIndex = ids[i].trackIndex();
        if (trackIndex < 1u || trackIndex > channels) {
          std::stringstream msg;
          msg << "chna entry " << i << " has trackIndex " << trackIndex
              << " which is outside the range [1, " << channels << "]";
          throw std::invalid_argument(msg.str());
        }
        if (!seenUids.insert(ids[i].uid()).second) {
          std::stringstream msg;
          msg << "duplicate audioTrackUID '" << ids[i].uid()
              << "' in chna chunk";
          throw std::invalid_argument(msg.str());
        }
      }
    }

    inline void appendFourCc(std::string& out, uint32_t id) {
      out.append(reinterpret_cast<const char*>(&id), 4);
    }

    /// serialize one chunk (header + payload + RIFF padding byte, which is
    /// always zero); the chunk length field never includes the padding byte
    inline std::string serializeArenaChunk(uint32_t id,
                                           const std::string& payload) {
      if (payload.size() >
          static_cast<size_t>((std::numeric_limits<uint32_t>::max)())) {
        throw std::length_error("metadata chunk payload exceeds uint32 range");
      }
      std::string out;
      appendFourCc(out, id);
      const uint32_t size = static_cast<uint32_t>(payload.size());
      out.append(reinterpret_cast<const char*>(&size), 4);
      out.append(payload);
      if (payload.size() % 2u == 1u) out.push_back('\0');
      return out;
    }

    /**
     * @brief Serialize a metadata arena of exactly @p capacity bytes.
     *
     * CHNA comes first (if present), then AXML (if present). Any remaining
     * space is either zero bytes or exactly one complete JUNK chunk with a
     * zero payload. A remainder of 2/4/6 bytes (a chunk header would not
     * fit) is rejected, as is any overflow. When both chunks are null the
     * whole arena becomes one JUNK chunk.
     */
    inline std::vector<char> buildMetadataArena(
        uint32_t capacity, const ChnaChunk* chna, const AxmlChunk* axml) {
      std::string blob;
      if (chna) {
        std::ostringstream payload;
        chna->write(payload);
        blob += serializeArenaChunk(utils::fourCC("chna"), payload.str());
      }
      if (axml) {
        blob += serializeArenaChunk(utils::fourCC("axml"), axml->data());
      }
      if (blob.size() > static_cast<size_t>(capacity)) {
        std::stringstream msg;
        msg << "metadata needs " << blob.size()
            << " bytes but the arena holds only " << capacity;
        throw std::length_error(msg.str());
      }
      const uint64_t remainder =
          static_cast<uint64_t>(capacity) - static_cast<uint64_t>(blob.size());
      if (remainder != 0u) {
        if (remainder < 8u) {
          std::stringstream msg;
          msg << "metadata arena leaves a " << remainder
              << "-byte tail which cannot hold a JUNK chunk";
          throw std::length_error(msg.str());
        }
        std::string junk;
        appendFourCc(junk, utils::fourCC("JUNK"));
        const uint32_t junkPayloadSize =
            static_cast<uint32_t>(remainder - 8u);
        junk.append(reinterpret_cast<const char*>(&junkPayloadSize), 4);
        junk.append(junkPayloadSize, '\0');
        blob += junk;
      }
      return std::vector<char>(blob.begin(), blob.end());
    }

    /// one chunk as found while scanning a closed file (headers only)
    struct ScannedChunk {
      uint32_t id;
      uint32_t sizeField;  /// raw 32-bit size field as stored on disk
      uint64_t size;      /// effective payload size (ds64 resolved)
      uint64_t position;  /// absolute offset of the chunk header
      uint64_t physicalSize() const { return 8u + size + (size % 2u); }
    };

    inline uint32_t readU32(std::istream& stream, uint64_t offset,
                            const char* what) {
      stream.clear();
      stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
      uint32_t value = 0;
      utils::readValue(stream, value);
      if (!stream.good()) {
        std::stringstream msg;
        msg << "failed to read " << what << " at offset " << offset;
        throw std::runtime_error(msg.str());
      }
      return value;
    }

    /**
     * @brief Header-only scan of a closed RIFF/BW64/RF64 file.
     *
     * Only chunk headers and the mandatory fmt/ds64 payloads are read; PCM
     * data and unknown chunk payloads are skipped with seeks.
     */
    struct ScannedFile {
      uint32_t riffId;
      uint64_t fileSize;
      bool is64Bit;
      uint64_t ds64DataSize;
      uint16_t channels;
      uint16_t bitDepth;
      uint64_t arenaStart;
      uint64_t arenaSize;
      std::vector<ScannedChunk> chunks;
    };

    inline ScannedFile scanEditableFile(std::fstream& stream) {
      stream.clear();
      stream.seekg(0, std::ios::end);
      const std::streamoff endOff = stream.tellg();
      if (endOff < 12) throw std::runtime_error("file is too small to be RIFF");
      const uint64_t fileSize = static_cast<uint64_t>(endOff);

      ScannedFile result;
      result.fileSize = fileSize;
      result.ds64DataSize = 0;
      result.arenaStart = 0;
      result.arenaSize = 0;

      const uint32_t riffId = readU32(stream, 0, "RIFF id");
      const uint32_t riffSizeField = readU32(stream, 4, "RIFF size");
      const uint32_t waveId = readU32(stream, 8, "WAVE id");
      result.riffId = riffId;
      result.is64Bit = (riffId == utils::fourCC("BW64") ||
                        riffId == utils::fourCC("RF64"));
      if (!result.is64Bit && riffId != utils::fourCC("RIFF")) {
        throw std::runtime_error(
            "not a RIFF, BW64 or RF64 file (bad container id at offset 0)");
      }
      if (waveId != utils::fourCC("WAVE")) {
        throw std::runtime_error("not a WAVE file (bad WAVE id at offset 8)");
      }

      uint64_t cursor = 12;
      uint64_t ds64DataSize = 0;

      if (result.is64Bit) {
        // ds64 must be the first chunk, directly after WAVE
        const uint32_t ds64Id = readU32(stream, cursor, "ds64 id");
        if (ds64Id != utils::fourCC("ds64")) {
          std::stringstream msg;
          msg << "BW64/RF64 file: expected 'ds64' at offset " << cursor
              << " but found '" << utils::fourCCToStr(ds64Id) << "'";
          throw std::runtime_error(msg.str());
        }
        const uint32_t ds64Size =
            readU32(stream, cursor + 4, "ds64 size");
        if (ds64Size < 28u) {
          std::stringstream msg;
          msg << "illegal ds64 chunk size " << ds64Size << " at offset "
              << cursor;
          throw std::runtime_error(msg.str());
        }
        if (cursor + 8u + static_cast<uint64_t>(ds64Size) > fileSize) {
          throw std::runtime_error("ds64 chunk extends past end of file");
        }
        stream.clear();
        stream.seekg(static_cast<std::streamoff>(cursor + 8));
        uint64_t bw64Size = 0;
        uint64_t dataSize = 0;
        uint64_t dummySize = 0;
        uint32_t tableLength = 0;
        utils::readValue(stream, bw64Size);
        utils::readValue(stream, dataSize);
        utils::readValue(stream, dummySize);
        utils::readValue(stream, tableLength);
        if (!stream.good())
          throw std::runtime_error("failed to read ds64 payload");
        if (tableLength != 0u) {
          std::stringstream msg;
          msg << "ds64 chunk at offset " << cursor
              << " has a non-empty chunk size table, which is not supported";
          throw std::runtime_error(msg.str());
        }
        if (bw64Size != fileSize - 8u) {
          std::stringstream msg;
          msg << "ds64 bw64Size " << bw64Size
              << " does not match physical file size " << (fileSize - 8u)
              << " (ds64 at offset " << cursor << ")";
          throw std::runtime_error(msg.str());
        }
        ds64DataSize = dataSize;
        result.ds64DataSize = dataSize;
        cursor += 8u + static_cast<uint64_t>(ds64Size);
      } else {
        if (static_cast<uint64_t>(riffSizeField) != fileSize - 8u) {
          std::stringstream msg;
          msg << "RIFF size field " << riffSizeField
              << " does not match physical file size " << (fileSize - 8u);
          throw std::runtime_error(msg.str());
        }
      }

      const uint32_t sentinel = 0xffffffffu;
      while (cursor + 8u <= fileSize) {
        const uint64_t chunkPosition = cursor;
        const uint32_t id = readU32(stream, cursor, "chunk id");
        const uint32_t sizeField =
            readU32(stream, cursor + 4, "chunk size");
        uint64_t size = sizeField;
        if (result.is64Bit && sizeField == sentinel) {
          if (id != utils::fourCC("data")) {
            std::stringstream msg;
            msg << "extended (ds64) size for chunk '"
                << utils::fourCCToStr(id) << "' at offset " << chunkPosition
                << " is not supported; only data may use 0xffffffff";
            throw std::runtime_error(msg.str());
          }
          size = ds64DataSize;
        }
        ScannedChunk chunk;
        chunk.id = id;
        chunk.sizeField = sizeField;
        chunk.size = size;
        chunk.position = chunkPosition;
        if (chunk.physicalSize() > fileSize - chunkPosition) {
          std::stringstream msg;
          msg << "chunk '" << utils::fourCCToStr(id) << "' at offset "
              << chunkPosition << " extends past end of file";
          throw std::runtime_error(msg.str());
        }
        result.chunks.push_back(chunk);
        cursor = chunkPosition + chunk.physicalSize();
      }
      if (cursor != fileSize) {
        std::stringstream msg;
        msg << "stray " << (fileSize - cursor)
            << " byte(s) after last chunk (file does not tile exactly)";
        throw std::runtime_error(msg.str());
      }

      // exactly one fmt and one data, integer PCM
      int fmtCount = 0;
      int dataCount = 0;
      const ScannedChunk* fmtChunk = nullptr;
      const ScannedChunk* dataChunk = nullptr;
      for (size_t i = 0; i < result.chunks.size(); ++i) {
        if (result.chunks[i].id == utils::fourCC("fmt ")) {
          ++fmtCount;
          fmtChunk = &result.chunks[i];
        }
        if (result.chunks[i].id == utils::fourCC("data")) {
          ++dataCount;
          dataChunk = &result.chunks[i];
        }
      }
      if (fmtCount != 1) {
        std::stringstream msg;
        msg << "expected exactly one fmt chunk, found " << fmtCount;
        throw std::runtime_error(msg.str());
      }
      if (dataCount != 1) {
        std::stringstream msg;
        msg << "expected exactly one data chunk, found " << dataCount;
        throw std::runtime_error(msg.str());
      }
      if (fmtChunk->size != 16u) {
        std::stringstream msg;
        msg << "fmt chunk at offset " << fmtChunk->position
            << " is not a 16-byte PCM fmt chunk (size " << fmtChunk->size
            << ")";
        throw std::runtime_error(msg.str());
      }
      stream.clear();
      stream.seekg(static_cast<std::streamoff>(fmtChunk->position + 8u));
      uint16_t formatTag = 0;
      uint16_t channels = 0;
      uint32_t sampleRate = 0;
      uint32_t bytesPerSecond = 0;
      uint16_t blockAlignment = 0;
      uint16_t bitsPerSample = 0;
      utils::readValue(stream, formatTag);
      utils::readValue(stream, channels);
      utils::readValue(stream, sampleRate);
      utils::readValue(stream, bytesPerSecond);
      utils::readValue(stream, blockAlignment);
      utils::readValue(stream, bitsPerSample);
      if (!stream.good())
        throw std::runtime_error("failed to read fmt payload");
      if (formatTag != 1u) {
        std::stringstream msg;
        msg << "unsupported format tag " << formatTag
            << ", only integer PCM is supported";
        throw std::runtime_error(msg.str());
      }
      if (bitsPerSample != 16u && bitsPerSample != 24u &&
          bitsPerSample != 32u) {
        std::stringstream msg;
        msg << "unsupported bit depth " << bitsPerSample << " in fmt chunk";
        throw std::runtime_error(msg.str());
      }
      if (static_cast<uint32_t>(blockAlignment) !=
          static_cast<uint32_t>(channels) * bitsPerSample / 8u) {
        throw std::runtime_error(
            "fmt blockAlignment does not match channels and bit depth");
      }
      result.channels = channels;
      result.bitDepth = bitsPerSample;

      if (result.is64Bit) {
        if (dataChunk->sizeField != sentinel &&
            static_cast<uint64_t>(dataChunk->sizeField) != ds64DataSize) {
          throw std::runtime_error(
              "ds64 dataSize does not match the data chunk size field");
        }
      }

      // metadata arena: the maximal run of chna/axml/JUNK chunks directly
      // before the data chunk
      const uint64_t dataPosition = dataChunk->position;
      uint64_t arenaStart = dataPosition;
      for (size_t i = result.chunks.size(); i-- > 0;) {
        const ScannedChunk& chunk = result.chunks[i];
        if (chunk.position >= dataPosition) continue;
        if (chunk.position + chunk.physicalSize() != arenaStart) break;
        if (chunk.id != utils::fourCC("chna") &&
            chunk.id != utils::fourCC("axml") &&
            chunk.id != utils::fourCC("JUNK")) {
          break;
        }
        arenaStart = chunk.position;
      }
      result.arenaStart = arenaStart;
      result.arenaSize = dataPosition - arenaStart;

      // metadata must live inside the arena, at most once each
      int chnaCount = 0;
      int axmlCount = 0;
      for (size_t i = 0; i < result.chunks.size(); ++i) {
        const ScannedChunk& chunk = result.chunks[i];
        const bool inArena = chunk.position >= arenaStart &&
                             chunk.position < dataPosition;
        if (chunk.id == utils::fourCC("chna")) {
          ++chnaCount;
          if (!inArena) {
            std::stringstream msg;
            msg << "chna chunk at offset " << chunk.position
                << " is outside the metadata arena before data at offset "
                << dataPosition;
            throw std::runtime_error(msg.str());
          }
        }
        if (chunk.id == utils::fourCC("axml")) {
          ++axmlCount;
          if (!inArena) {
            std::stringstream msg;
            msg << "axml chunk at offset " << chunk.position
                << " is outside the metadata arena before data at offset "
                << dataPosition;
            throw std::runtime_error(msg.str());
          }
        }
      }
      if (chnaCount > 1) {
        std::stringstream msg;
        msg << "found " << chnaCount << " chna chunks, at most one is allowed";
        throw std::runtime_error(msg.str());
      }
      if (axmlCount > 1) {
        std::stringstream msg;
        msg << "found " << axmlCount << " axml chunks, at most one is allowed";
        throw std::runtime_error(msg.str());
      }
      if (result.arenaSize == 0u) {
        std::stringstream msg;
        msg << "no editable metadata arena (chna/axml/JUNK run) directly "
               "before the data chunk at offset "
            << dataPosition;
        throw std::runtime_error(msg.str());
      }
      if (result.arenaSize >
          static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())) {
        throw std::length_error("metadata arena exceeds uint32 range");
      }
      return result;
    }

  }  // namespace detail

  /**
   * @brief Replace the chna/axml metadata of a closed BW64/RIFF/RF64 file
   * in place.
   *
   * Only the contiguous chna/axml/JUNK arena directly before the data chunk
   * is rewritten. The file length, the data header and the absolute position
   * and bytes of the PCM payload, fmt, unknown chunks and container length
   * information are left untouched.
   *
   * @p chna or @p axml may be null to delete that chunk. The input chunks
   * are validated and serialized before anything is written, so validation
   * and capacity failures leave the file byte-for-byte unchanged.
   */
  inline void updateMetadataInPlace(
      const std::string& filename,
      std::shared_ptr<ChnaChunk> chna,
      std::shared_ptr<AxmlChunk> axml) {
    std::fstream stream(filename.c_str(),
                        std::fstream::in | std::fstream::out |
                            std::fstream::binary);
    if (!stream.is_open()) {
      std::stringstream msg;
      msg << "could not open file for in-place metadata update: " << filename;
      throw std::runtime_error(msg.str());
    }

    // snapshot (deep copy) and validate against the file before writing
    std::shared_ptr<ChnaChunk> chnaSnapshot;
    std::shared_ptr<AxmlChunk> axmlSnapshot;
    if (chna) chnaSnapshot = detail::cloneChna(*chna);
    if (axml) axmlSnapshot = detail::cloneAxml(*axml);

    const detail::ScannedFile scanned = detail::scanEditableFile(stream);
    if (chnaSnapshot) {
      detail::validateChnaForChannels(*chnaSnapshot, scanned.channels);
    }

    // build the whole new arena in memory first; this never touches the file
    const std::vector<char> arenaBytes = detail::buildMetadataArena(
        static_cast<uint32_t>(scanned.arenaSize), chnaSnapshot.get(),
        axmlSnapshot.get());
    if (static_cast<uint64_t>(arenaBytes.size()) != scanned.arenaSize) {
      throw std::runtime_error("internal error: arena size mismatch");
    }

    stream.clear();
    stream.seekp(static_cast<std::streamoff>(scanned.arenaStart),
                 std::ios::beg);
    if (!stream.good()) {
      std::stringstream msg;
      msg << "could not seek to metadata arena at offset "
          << scanned.arenaStart;
      throw std::runtime_error(msg.str());
    }
    stream.write(arenaBytes.data(),
                 static_cast<std::streamsize>(arenaBytes.size()));
    if (!stream.good()) {
      throw std::runtime_error(
          "failed to write metadata arena (the file may be partially "
          "updated)");
    }
    stream.flush();
    if (!stream.good()) {
      throw std::runtime_error(
          "failed to flush metadata arena (the file may be partially "
          "updated)");
    }
  }

}  // namespace bw64
