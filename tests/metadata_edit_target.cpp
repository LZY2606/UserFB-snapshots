/**
 * @file metadata_edit_target.cpp
 *
 * Regression tests for the reserved metadata arena (reserveMetadata /
 * setMetadata) and for bw64::updateMetadataInPlace().
 *
 * The on-disk layout is checked with an independent hand-written byte
 * scanner and hand-crafted chunk bytes -- not only by round-tripping through
 * the library itself. There is no main(): task_support/driver.c drives the
 * registered task_cases.
 */
#include "task_support/check.h"

#include <bw64/bw64.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

using namespace bw64;

namespace {

  // ----------------------------------------------------------------------
  // small helpers
  // ----------------------------------------------------------------------

  std::string tmpPath(const char* name) {
    const char* dir = std::getenv("TMPDIR");
    std::string base = (dir && *dir) ? std::string(dir) : std::string(".");
    return base + "/bw64_meta_" + name + ".wav";
  }

  void removeIfExists(const std::string& path) { std::remove(path.c_str()); }

  void putU16(std::string& out, uint16_t value) {
    out.append(reinterpret_cast<const char*>(&value), 2);
  }
  void putU32(std::string& out, uint32_t value) {
    out.append(reinterpret_cast<const char*>(&value), 4);
  }
  void putU64(std::string& out, uint64_t value) {
    out.append(reinterpret_cast<const char*>(&value), 8);
  }

  uint16_t getU16(const std::string& b, size_t off) {
    uint16_t v = 0;
    std::memcpy(&v, b.data() + off, 2);
    return v;
  }
  uint32_t getU32(const std::string& b, size_t off) {
    uint32_t v = 0;
    std::memcpy(&v, b.data() + off, 4);
    return v;
  }
  uint64_t getU64(const std::string& b, size_t off) {
    uint64_t v = 0;
    std::memcpy(&v, b.data() + off, 8);
    return v;
  }

  std::string fixedField(const std::string& value, size_t width) {
    std::string out(width, ' ');
    const size_t n = value.size() < width ? value.size() : width;
    std::copy(value.begin(), value.begin() + n, out.begin());
    return out;
  }

  std::string readFileBytes(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }

  void writeFileBytes(const std::string& path, const std::string& bytes) {
    std::ofstream f(path.c_str(),
                    std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  std::string readAt(const std::string& path, uint64_t offset, size_t n) {
    std::ifstream f(path.c_str(), std::ios::binary);
    f.seekg(static_cast<std::streamoff>(offset));
    std::string out(n, '\0');
    f.read(&out[0], static_cast<std::streamsize>(n));
    return out;
  }

  uint64_t fileSize64(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    f.seekg(0, std::ios::end);
    return static_cast<uint64_t>(f.tellg());
  }

  bool regionAllZero(const std::string& path, uint64_t offset, uint64_t n) {
    std::ifstream f(path.c_str(), std::ios::binary);
    f.seekg(static_cast<std::streamoff>(offset));
    const size_t bufSize = 4096;
    std::vector<char> buf(bufSize);
    uint64_t done = 0;
    while (done < n) {
      const size_t want = static_cast<size_t>(
          (n - done) < bufSize ? (n - done) : bufSize);
      f.read(buf.data(), static_cast<std::streamsize>(want));
      if (f.gcount() != static_cast<std::streamsize>(want)) return false;
      for (size_t i = 0; i < want; ++i)
        if (buf[i] != '\0') return false;
      done += want;
    }
    return true;
  }

  // ----------------------------------------------------------------------
  // independent chunk scanner (raw 32-bit size fields, header-only)
  // ----------------------------------------------------------------------

  struct RawChunk {
    std::string id;
    uint32_t sizeField;
    uint64_t position;
    uint64_t physicalSize() const {
      return 8ull + sizeField + (static_cast<uint64_t>(sizeField) & 1ull);
    }
  };

  std::vector<RawChunk> rawScan(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    f.seekg(0, std::ios::end);
    const uint64_t end = static_cast<uint64_t>(f.tellg());
    std::vector<RawChunk> chunks;
    uint64_t off = 12;
    while (off + 8ull <= end) {
      f.clear();
      f.seekg(static_cast<std::streamoff>(off));
      char idBytes[4];
      uint32_t sizeField = 0;
      f.read(idBytes, 4);
      f.read(reinterpret_cast<char*>(&sizeField), 4);
      RawChunk chunk;
      chunk.id = std::string(idBytes, 4);
      chunk.sizeField = sizeField;
      chunk.position = off;
      chunks.push_back(chunk);
      off += chunk.physicalSize();
    }
    return chunks;
  }

  const RawChunk* rawFind(const std::vector<RawChunk>& chunks,
                          const std::string& id) {
    for (size_t i = 0; i < chunks.size(); ++i)
      if (chunks[i].id == id) return &chunks[i];
    return nullptr;
  }

  size_t rawCount(const std::vector<RawChunk>& chunks,
                  const std::string& id) {
    size_t n = 0;
    for (size_t i = 0; i < chunks.size(); ++i)
      if (chunks[i].id == id) ++n;
    return n;
  }

  // find the last chunk with this id (the arena JUNK comes after the ds64
  // placeholder JUNK)
  const RawChunk* rawFindLast(const std::vector<RawChunk>& chunks,
                              const std::string& id) {
    const RawChunk* found = nullptr;
    for (size_t i = 0; i < chunks.size(); ++i)
      if (chunks[i].id == id) found = &chunks[i];
    return found;
  }

  // ----------------------------------------------------------------------
  // hand-crafted chunk payloads (no library calls used for construction)
  // ----------------------------------------------------------------------

  struct RawAudioId {
    uint16_t trackIndex;
    std::string uid;
    std::string trackRef;
    std::string packRef;
  };

  std::string rawChnaPayload(const std::vector<RawAudioId>& ids) {
    std::set<uint16_t> tracks;
    for (size_t i = 0; i < ids.size(); ++i) tracks.insert(ids[i].trackIndex);
    std::string out;
    putU16(out, static_cast<uint16_t>(tracks.size()));
    putU16(out, static_cast<uint16_t>(ids.size()));
    for (size_t i = 0; i < ids.size(); ++i) {
      putU16(out, ids[i].trackIndex);
      out += fixedField(ids[i].uid, 12);
      out += fixedField(ids[i].trackRef, 14);
      out += fixedField(ids[i].packRef, 11);
      out.push_back(' ');  // padding
    }
    return out;
  }

  std::string rawFmtPayload(uint16_t channels, uint32_t sampleRate,
                            uint16_t bitsPerSample) {
    std::string out;
    putU16(out, 1);  // WAVE_FORMAT_PCM
    putU16(out, channels);
    putU32(out, sampleRate);
    putU32(out, static_cast<uint32_t>(sampleRate) * channels *
                    bitsPerSample / 8u);
    putU16(out, static_cast<uint16_t>(channels * bitsPerSample / 8u));
    putU16(out, bitsPerSample);
    return out;
  }

  std::string makeChunk(const std::string& id, const std::string& payload) {
    std::string out;
    out += id;
    putU32(out, static_cast<uint32_t>(payload.size()));
    out += payload;
    if (payload.size() % 2u == 1u) out.push_back('\0');
    return out;
  }

  struct RawSpec {
    std::string id;
    std::string payload;
  };

  // build a plain RIFF file from raw chunk specs
  void buildPlainRiff(const std::string& path,
                      const std::vector<RawSpec>& preData,
                      const std::string& dataPayload,
                      const std::vector<RawSpec>& postData) {
    std::string bytes;
    bytes += "RIFF";
    putU32(bytes, 0);  // patched below
    bytes += "WAVE";
    for (size_t i = 0; i < preData.size(); ++i)
      bytes += makeChunk(preData[i].id, preData[i].payload);
    bytes += makeChunk("data", dataPayload);
    for (size_t i = 0; i < postData.size(); ++i)
      bytes += makeChunk(postData[i].id, postData[i].payload);
    const uint32_t riffSize = static_cast<uint32_t>(bytes.size() - 8u);
    std::memcpy(&bytes[4], &riffSize, 4);
    writeFileBytes(path, bytes);
  }

  // build a sparse >4 GiB BW64/RF64 file with head and tail PCM markers
  struct SparseFile {
    std::string path;
    uint64_t dataSize;
    uint64_t dataPayloadOffset;
    uint64_t totalSize;
    uint64_t arenaOffset;
    uint32_t arenaSize;
  };

  SparseFile buildSparse64(const std::string& path,
                           const std::string& containerId,
                           uint32_t ds64TableLength) {
    const uint16_t channels = 1;
    const uint16_t bits = 24;
    const uint64_t dataSize = 0x100000008ull;  // 4 GiB + 8, /3 is exact
    const uint32_t arenaSize = 96;

    std::string ds64Payload;
    const uint64_t ds64PayloadSize = 28ull + 12ull * ds64TableLength;
    const uint64_t headerSize =
        12ull + 8ull + ds64PayloadSize + 24ull + arenaSize + 8ull;
    const uint64_t totalSize = headerSize + dataSize;
    putU64(ds64Payload, totalSize - 8ull);  // bw64Size
    putU64(ds64Payload, dataSize);
    putU64(ds64Payload, 0);  // dummy
    putU32(ds64Payload, ds64TableLength);
    for (uint32_t i = 0; i < ds64TableLength; ++i) {
      putU32(ds64Payload, utils::fourCC("JUNK"));
      putU64(ds64Payload, 1234ull);
    }

    std::string prefix;
    prefix += containerId;
    putU32(prefix, 0xffffffffu);
    prefix += "WAVE";
    prefix += makeChunk("ds64", ds64Payload);
    prefix += makeChunk("fmt ", rawFmtPayload(channels, 48000, bits));
    const uint64_t arenaOffset = prefix.size();
    prefix += makeChunk("JUNK", std::string(arenaSize - 8u, '\0'));
    const uint64_t dataHeaderOffset = prefix.size();
    prefix += "data";
    putU32(prefix, 0xffffffffu);
    const uint64_t dataPayloadOffset = dataHeaderOffset + 8ull;

    std::fstream f(path.c_str(),
                   std::ios::in | std::ios::out | std::ios::binary |
                       std::ios::trunc);
    f.write(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    const char head[3] = {static_cast<char>(0x11), static_cast<char>(0x22),
                          static_cast<char>(0x33)};
    const char tail[3] = {static_cast<char>(0xaa), static_cast<char>(0xbb),
                          static_cast<char>(0xcc)};
    f.write(head, 3);
    f.seekp(static_cast<std::streamoff>(dataPayloadOffset + dataSize - 3ull));
    f.write(tail, 3);
    f.close();

    SparseFile info;
    info.path = path;
    info.dataSize = dataSize;
    info.dataPayloadOffset = dataPayloadOffset;
    info.totalSize = totalSize;
    info.arenaOffset = arenaOffset;
    info.arenaSize = arenaSize;
    return info;
  }

  // library-built chna/axml convenience constructors
  std::shared_ptr<ChnaChunk> makeChna(uint16_t channels,
                                      const std::string& tag) {
    auto chna = std::make_shared<ChnaChunk>();
    for (uint16_t ch = 1; ch <= channels; ++ch) {
      const std::string digit(1, static_cast<char>('0' + ch));
      chna->addAudioId(AudioId(
          ch, std::string("ATU_000000") + tag + digit,
          std::string("AT_0001000") + digit + "_01",
          std::string("AP_0001000") + digit));
    }
    return chna;
  }

  std::shared_ptr<AxmlChunk> makeAxml(const std::string& text,
                                      bool oddLength) {
    std::string data = "<adm>" + text + "</adm>";
    if (oddLength && data.size() % 2u == 0u) data.push_back('x');
    if (!oddLength && data.size() % 2u == 1u) data.push_back('x');
    return std::make_shared<AxmlChunk>(data);
  }

  std::vector<float> distinctivePcm(uint64_t frames, uint16_t channels) {
    std::vector<float> pcm(static_cast<size_t>(frames) * channels);
    for (uint64_t frame = 0; frame < frames; ++frame)
      for (uint16_t ch = 0; ch < channels; ++ch)
        pcm[frame * channels + ch] =
            0.1f + 0.05f * static_cast<float>(ch) +
            0.001f * static_cast<float>(frame % 7);
    return pcm;
  }

  // ======================================================================
  // reserve_lifecycle
  // ======================================================================

  int lifecycle_chain() {
    const std::string path = tmpPath("lifecycle");
    removeIfExists(path);
    const uint16_t channels = 2;
    const uint64_t frames = 8;
    const std::vector<float> pcm = distinctivePcm(frames, channels);

    {
      auto writer = writeFile(path, channels, 48000, 24);
      TASK_CHECK(writer->metadataReserved() == false);

      // invalid reserve must leave the writer usable
      bool threw = false;
      try {
        writer->reserveMetadata(7);
      } catch (const std::exception&) {
        threw = true;
      }
      TASK_CHECK(threw);
      TASK_CHECK(writer->metadataReserved() == false);

      writer->reserveMetadata(256);
      TASK_CHECK(writer->metadataReserved());

      // a second reserve is rejected
      threw = false;
      try {
        writer->reserveMetadata(128);
      } catch (const std::logic_error&) {
        threw = true;
      }
      TASK_CHECK(threw);

      // write audio first, then edit metadata
      TASK_CHECK(writer->write(const_cast<float*>(pcm.data()), frames) ==
                 frames);
      auto chna = makeChna(channels, "1");
      auto axml = makeAxml("version-one", true);  // odd AXML
      writer->setMetadata(chna, axml);

      // external mutation of the caller's chunk must not change the snapshot
      chna->addAudioId(
          AudioId(channels, "ATU_EXTRA001", "AT_00090001_01",
                  "AP_00090001"));
      TASK_CHECK(writer->reservedChnaChunk()->numUids() == channels);

      // a short 6-byte tail must be rejected and leave the arena untouched
      threw = false;
      try {
        // 256-byte arena, axml only: a 242-byte even payload occupies 250
        // physical bytes, leaving exactly 6 bytes which cannot hold a JUNK
        // header -> the layout is rejected
        auto shortTail = std::make_shared<AxmlChunk>(std::string(242, 'z'));
        writer->setMetadata(nullptr, shortTail);
      } catch (const std::exception&) {
        threw = true;
      }
      TASK_CHECK(threw);

      // the previous snapshot (the odd version-one axml) is unchanged
      TASK_CHECK(writer->reservedAxmlChunk()->data() == axml->data());
      TASK_CHECK(writer->reservedChnaChunk()->numUids() == channels);

      // writing audio still works after the failed update
      TASK_CHECK(writer->write(const_cast<float*>(pcm.data()), frames) ==
                 frames);

      // retry with metadata that fits
      writer->setMetadata(chna, axml);
      TASK_CHECK(writer->reservedAxmlChunk()->data() == axml->data());

      // delete both, then re-add
      writer->setMetadata(nullptr, nullptr);
      TASK_CHECK(writer->reservedChnaChunk() == nullptr);
      TASK_CHECK(writer->reservedAxmlChunk() == nullptr);
      auto chna2 = makeChna(channels, "2");
      auto axml2 = makeAxml("version-two", false);
      writer->setMetadata(chna2, axml2);
      TASK_CHECK(writer->reservedChnaChunk()->numUids() == channels);

      writer->close();

      // closed writers reject both calls
      threw = false;
      try {
        writer->reserveMetadata(64);
      } catch (const std::logic_error&) {
        threw = true;
      }
      TASK_CHECK(threw);
      threw = false;
      try {
        writer->setMetadata(nullptr, nullptr);
      } catch (const std::logic_error&) {
        threw = true;
      }
      TASK_CHECK(threw);
    }

    // reopen and check the final snapshots and all PCM frames
    auto reader = readFile(path);
    TASK_CHECK(reader->channels() == channels);
    TASK_CHECK(reader->numberOfFrames() == frames * 2);
    TASK_CHECK(reader->chnaChunk() != nullptr);
    TASK_CHECK(reader->chnaChunk()->numUids() == channels);
    TASK_CHECK(reader->axmlChunk() != nullptr);
    TASK_CHECK(reader->axmlChunk()->data() ==
               makeAxml("version-two", false)->data());
    std::vector<float> got(static_cast<size_t>(frames * 2) * channels);
    TASK_CHECK(reader->read(got.data(), frames * 2) == frames * 2);
    for (uint64_t f = 0; f < frames * 2; ++f)
      for (uint16_t c = 0; c < channels; ++c) {
        const float expected = pcm[(f % frames) * channels + c];
        const float diff = got[f * channels + c] - expected;
        TASK_CHECK(diff > -1e-6f && diff < 1e-6f);
      }
    reader->close();
    removeIfExists(path);
    return 0;
  }

  int reserve_argument_guards() {
    const std::string path = tmpPath("guards");
    removeIfExists(path);
    auto writer = writeFile(path, 1, 48000, 16);

    const uint32_t badSizes[] = {0, 6, 7, 9, 16u * 1024u * 1024u + 2u,
                                 17u * 1024u * 1024u};
    for (size_t i = 0; i < sizeof(badSizes) / sizeof(badSizes[0]); ++i) {
      bool threw = false;
      try {
        writer->reserveMetadata(badSizes[i]);
      } catch (const std::exception&) {
        threw = true;
      }
      TASK_CHECK(threw);
      TASK_CHECK(writer->metadataReserved() == false);
    }

    // setMetadata without a reservation is a logic_error
    bool threw = false;
    try {
      writer->setMetadata(nullptr, nullptr);
    } catch (const std::logic_error&) {
      threw = true;
    }
    TASK_CHECK(threw);

    // a legal call after all the invalid ones works
    writer->reserveMetadata(64);
    TASK_CHECK(writer->metadataReserved());

    // reserving after an audio write is rejected
    float one = 0.5f;
    writer->write(&one, 1);
    threw = false;
    try {
      writer->reserveMetadata(64);
    } catch (const std::logic_error&) {
      threw = true;
    }
    TASK_CHECK(threw);
    writer->close();
    removeIfExists(path);
    return 0;
  }

  int reserve_boundary_sizes() {
    // the legal range is the closed interval [8, 16 MiB], even only
    const std::string path = tmpPath("boundary_sizes");
    removeIfExists(path);
    {
      auto writer = writeFile(path, 1, 48000, 16);
      writer->reserveMetadata(8);  // smallest: one JUNK with zero payload
      TASK_CHECK(writer->metadataReserved());
      float v = 0.1f;
      writer->write(&v, 1);
      writer->close();
    }
    {
      const std::vector<RawChunk> chunks = rawScan(path);
      const RawChunk* data = rawFind(chunks, "data");
      const RawChunk* fmt = rawFind(chunks, "fmt ");
      const RawChunk* arenaJunk = nullptr;
      for (size_t i = 0; i < chunks.size(); ++i)
        if (chunks[i].id == "JUNK" && chunks[i].position > fmt->position)
          arenaJunk = &chunks[i];
      TASK_CHECK(arenaJunk != nullptr);
      TASK_CHECK(arenaJunk->physicalSize() == 8);
      TASK_CHECK(arenaJunk->position + 8 == data->position);
    }
    removeIfExists(path);

    {
      auto writer = writeFile(path, 1, 48000, 16);
      const uint32_t maxBytes = 16u * 1024u * 1024u;
      writer->reserveMetadata(maxBytes);  // largest legal arena
      TASK_CHECK(writer->metadataReserved());
      float v = 0.2f;
      writer->write(&v, 1);
      writer->close();
    }
    {
      const std::vector<RawChunk> chunks = rawScan(path);
      const RawChunk* data = rawFind(chunks, "data");
      const RawChunk* fmt = rawFind(chunks, "fmt ");
      const RawChunk* arenaJunk = nullptr;
      for (size_t i = 0; i < chunks.size(); ++i)
        if (chunks[i].id == "JUNK" && chunks[i].position > fmt->position)
          arenaJunk = &chunks[i];
      TASK_CHECK(arenaJunk != nullptr);
      TASK_CHECK(arenaJunk->physicalSize() == 16u * 1024u * 1024u);
      TASK_CHECK(arenaJunk->position + arenaJunk->physicalSize() ==
                 data->position);
      // the file still parses and the audio round-trips
      auto reader = readFile(path);
      TASK_CHECK(reader->numberOfFrames() == 1);
      float got = 0;
      TASK_CHECK(reader->read(&got, 1) == 1);
      reader->close();
    }
    removeIfExists(path);
    return 0;
  }

  int initial_metadata_moved() {
    const std::string path = tmpPath("moved");
    removeIfExists(path);
    auto chna = makeChna(2, "1");
    auto axml = makeAxml("initial", false);
    {
      auto writer = writeFile(path, 2, 48000, 24, chna, axml);
      writer->reserveMetadata(512);
      float silence[4] = {0};
      writer->write(silence, 2);
      writer->close();
    }

    // independent scan: exactly one chna and one axml, both before data
    const std::vector<RawChunk> chunks = rawScan(path);
    TASK_CHECK(rawCount(chunks, "chna") == 1);
    TASK_CHECK(rawCount(chunks, "axml") == 1);
    const RawChunk* chnaRaw = rawFind(chunks, "chna");
    const RawChunk* axmlRaw = rawFind(chunks, "axml");
    const RawChunk* data = rawFind(chunks, "data");
    TASK_CHECK(chnaRaw && axmlRaw && data);
    TASK_CHECK(chnaRaw->position < axmlRaw->position);
    TASK_CHECK(axmlRaw->position < data->position);

    // no stale copy: the word chna/axml must not appear anywhere after data
    const std::string bytes = readFileBytes(path);
    TASK_CHECK(bytes.find("chna", data->position) == std::string::npos);
    TASK_CHECK(bytes.find("axml", data->position) == std::string::npos);

    auto reader = readFile(path);
    TASK_CHECK(reader->chnaChunk()->numUids() == 2);
    TASK_CHECK(reader->axmlChunk()->data() == axml->data());
    reader->close();
    removeIfExists(path);
    return 0;
  }

  int reserve_too_small_for_initial() {
    const std::string path = tmpPath("reserve_small");
    removeIfExists(path);
    // chna occupies 52 physical bytes; the 300-byte axml occupies 308, so
    // together they need 360 bytes and cannot fit in a 64-byte arena
    const std::string bigAxml(300, 'a');
    {
      auto chna = std::make_shared<ChnaChunk>();
      chna->addAudioId(
          AudioId(1, "ATU_00000001", "AT_00010001_01", "AP_00010001"));
      auto writer = writeFile(path, 1, 48000, 24, chna,
                              std::make_shared<AxmlChunk>(bigAxml));
      const std::string before = readFileBytes(path);

      bool threw = false;
      try {
        writer->reserveMetadata(64);
      } catch (const std::exception&) {
        threw = true;
      }
      TASK_CHECK(threw);
      TASK_CHECK(writer->metadataReserved() == false);

      // the failed reserve must not truncate or otherwise alter the file
      TASK_CHECK(readFileBytes(path) == before);

      // a legal, large reservation then moves the initial metadata in
      writer->reserveMetadata(512);
      TASK_CHECK(writer->metadataReserved());
      TASK_CHECK(writer->reservedChnaChunk()->numUids() == 1);
      TASK_CHECK(writer->reservedAxmlChunk()->data() == bigAxml);
      writer->close();
    }

    // reopen only after close() so the writer stream is fully flushed
    {
      auto reader = readFile(path);
      TASK_CHECK(reader->chnaChunk()->numUids() == 1);
      TASK_CHECK(reader->axmlChunk()->data() == bigAxml);
      reader->close();
    }
    const std::vector<RawChunk> chunks = rawScan(path);
    TASK_CHECK(rawCount(chunks, "chna") == 1);
    TASK_CHECK(rawCount(chunks, "axml") == 1);
    const RawChunk* data = rawFind(chunks, "data");
    TASK_CHECK(rawFind(chunks, "axml")->position < data->position);
    removeIfExists(path);
    return 0;
  }

  int legacy_no_reserve_control() {
    const std::string path = tmpPath("legacy");
    removeIfExists(path);
    {
      auto writer = writeFile(path, 1, 48000, 24);
      auto chna = std::make_shared<ChnaChunk>();
      chna->addAudioId(
          AudioId(1, "ATU_00000001", "AT_00010001_01", "AP_00010001"));
      writer->setChnaChunk(chna);  // legacy placeholder overwrite
      writer->setAxmlChunk(std::make_shared<AxmlChunk>("legacy-axml"));
      float v[3] = {0.25f, -0.25f, 0.5f};
      writer->write(v, 3);
      writer->close();
    }
    // independent scan: axml stays after the data chunk
    const std::vector<RawChunk> chunks = rawScan(path);
    const RawChunk* data = rawFind(chunks, "data");
    const RawChunk* axml = rawFind(chunks, "axml");
    TASK_CHECK(data && axml);
    TASK_CHECK(axml->position > data->position);
    TASK_CHECK(rawCount(chunks, "chna") == 1);

    auto reader = readFile(path);
    TASK_CHECK(reader->numberOfFrames() == 3);
    TASK_CHECK(reader->chnaChunk()->numUids() == 1);
    TASK_CHECK(reader->axmlChunk()->data() == "legacy-axml");
    float got[3] = {0};
    TASK_CHECK(reader->read(got, 3) == 3);
    reader->close();
    removeIfExists(path);
    return 0;
  }

  int chna_rules() {
    const std::string path = tmpPath("chna_rules");
    removeIfExists(path);
    auto writer = writeFile(path, 3, 48000, 24);
    writer->reserveMetadata(1024);

    auto expectReject = [&](std::shared_ptr<ChnaChunk> bad) -> int {
      bool threw = false;
      try {
        writer->setMetadata(bad, nullptr);
      } catch (const std::exception&) {
        threw = true;
      }
      TASK_CHECK(threw);
      return 0;
    };

    auto zeroTrack = std::make_shared<ChnaChunk>();
    zeroTrack->addAudioId(
        AudioId(0, "ATU_00000001", "AT_00010001_01", "AP_00010001"));
    expectReject(zeroTrack);

    auto tooHigh = std::make_shared<ChnaChunk>();
    tooHigh->addAudioId(
        AudioId(4, "ATU_00000001", "AT_00010001_01", "AP_00010001"));
    expectReject(tooHigh);

    auto duplicate = std::make_shared<ChnaChunk>();
    duplicate->addAudioId(
        AudioId(1, "ATU_00000001", "AT_00010001_01", "AP_00010001"));
    duplicate->addAudioId(
        AudioId(2, "ATU_00000001", "AT_00010002_01", "AP_00010002"));
    expectReject(duplicate);

    // several different UIDs on the same track are legal
    auto sameTrack = std::make_shared<ChnaChunk>();
    sameTrack->addAudioId(
        AudioId(1, "ATU_00000001", "AT_00010001_01", "AP_00010001"));
    sameTrack->addAudioId(
        AudioId(1, "ATU_00000002", "AT_00010002_01", "AP_00010002"));
    writer->setMetadata(sameTrack, nullptr);
    TASK_CHECK(writer->reservedChnaChunk()->numUids() == 2);
    TASK_CHECK(writer->reservedChnaChunk()->numTracks() == 1);
    writer->close();
    removeIfExists(path);
    return 0;
  }

  // ======================================================================
  // arena_layout
  // ======================================================================

  int writer_arena_bytes() {
    const std::string path = tmpPath("arena_bytes");
    removeIfExists(path);
    const uint32_t arenaCapacity = 200;  // even
    auto chna = std::make_shared<ChnaChunk>();
    chna->addAudioId(
        AudioId(1, "ATU_00000001", "AT_00010001_01", "AP_00010001"));
    auto axml = makeAxml("odd", true);  // odd payload -> one pad byte
    {
      auto writer = writeFile(path, 1, 48000, 24);
      writer->reserveMetadata(arenaCapacity);
      writer->setMetadata(chna, axml);
      float v[2] = {0.3f, -0.3f};
      writer->write(v, 2);
      writer->close();
    }

    const std::vector<RawChunk> chunks = rawScan(path);
    const RawChunk* fmt = rawFind(chunks, "fmt ");
    const RawChunk* chnaRaw = rawFind(chunks, "chna");
    const RawChunk* axmlRaw = rawFind(chunks, "axml");
    const RawChunk* junk = rawFindLast(chunks, "JUNK");
    const RawChunk* data = rawFind(chunks, "data");
    TASK_CHECK(fmt && chnaRaw && axmlRaw && junk && data);

    // arena is the contiguous chna/axml/JUNK run directly before data
    TASK_CHECK(chnaRaw->position >= fmt->position + fmt->physicalSize());
    TASK_CHECK(chnaRaw->position < axmlRaw->position);
    TASK_CHECK(axmlRaw->position < junk->position);
    TASK_CHECK(junk->position + junk->physicalSize() == data->position);
    const uint64_t arenaStart = chnaRaw->position;
    TASK_CHECK(data->position - arenaStart == arenaCapacity);

    // chna: 4 header bytes payload fields + 40 per UID, no pad (even)
    TASK_CHECK(chnaRaw->sizeField == 44);
    TASK_CHECK(chnaRaw->physicalSize() == 52);

    // odd axml: size field excludes the RIFF padding byte, which is zero
    TASK_CHECK(axmlRaw->sizeField == axml->data().size());
    TASK_CHECK((axmlRaw->sizeField & 1u) == 1u);
    const std::string padByte =
        readAt(path, axmlRaw->position + 8ull + axmlRaw->sizeField, 1);
    TASK_CHECK(padByte.size() == 1 && padByte[0] == '\0');

    // the JUNK tail is a complete chunk with an all-zero payload
    TASK_CHECK(junk->physicalSize() >= 8);
    TASK_CHECK(regionAllZero(path, junk->position + 8, junk->sizeField));
    const uint64_t covered = chnaRaw->physicalSize() +
                             axmlRaw->physicalSize() + junk->physicalSize();
    TASK_CHECK(covered == arenaCapacity);

    // RIFF size field matches the physical file length
    const std::string head = readAt(path, 0, 12);
    TASK_CHECK(getU32(head, 4) ==
               static_cast<uint32_t>(fileSize64(path) - 8ull));

    // independently parse the hand-crafted-style chna content bytes
    const std::string chnaBytes =
        readAt(path, chnaRaw->position + 8, chnaRaw->sizeField);
    TASK_CHECK(getU16(chnaBytes, 0) == 1);  // numTracks
    TASK_CHECK(getU16(chnaBytes, 2) == 1);  // numUids
    TASK_CHECK(getU16(chnaBytes, 4) == 1);  // trackIndex
    TASK_CHECK(chnaBytes.substr(6, 12) == fixedField("ATU_00000001", 12));
    removeIfExists(path);
    return 0;
  }

  int delete_both_all_junk() {
    const std::string path = tmpPath("all_junk");
    removeIfExists(path);
    const uint32_t arenaCapacity = 128;
    {
      auto writer = writeFile(path, 2, 48000, 16);
      writer->reserveMetadata(arenaCapacity);
      writer->setMetadata(makeChna(2, "1"), makeAxml("x", false));
      writer->setMetadata(nullptr, nullptr);  // delete both
      float v[2] = {0.1f, -0.1f};
      writer->write(v, 1);
      writer->close();
    }
    const std::vector<RawChunk> chunks = rawScan(path);
    TASK_CHECK(rawCount(chunks, "chna") == 0);
    TASK_CHECK(rawCount(chunks, "axml") == 0);
    // the ds64 placeholder JUNK before fmt, plus the arena JUNK after fmt
    TASK_CHECK(rawCount(chunks, "JUNK") == 2);
    const RawChunk* data = rawFind(chunks, "data");
    const RawChunk* fmt = rawFind(chunks, "fmt ");
    const RawChunk* arenaJunk = nullptr;
    for (size_t i = 0; i < chunks.size(); ++i)
      if (chunks[i].id == "JUNK" &&
          chunks[i].position > fmt->position)
        arenaJunk = &chunks[i];
    TASK_CHECK(arenaJunk != nullptr);
    TASK_CHECK(arenaJunk->physicalSize() == arenaCapacity);
    TASK_CHECK(arenaJunk->position + arenaJunk->physicalSize() ==
               data->position);
    TASK_CHECK(
        regionAllZero(path, arenaJunk->position + 8, arenaJunk->sizeField));
    removeIfExists(path);
    return 0;
  }

  int short_tail_reject_retry() {
    const std::string path = tmpPath("short_tail");
    removeIfExists(path);
    const uint32_t arenaCapacity = 64;
    {
      auto writer = writeFile(path, 1, 48000, 24);
      writer->reserveMetadata(arenaCapacity);
      auto good = std::make_shared<AxmlChunk>(std::string(48, 'g'));
      writer->setMetadata(nullptr, good);  // 8 + 48 = 56, tail 8 -> JUNK(0)

      // 50-byte payload: physical 58, leaves a 6-byte tail -> rejected
      auto bad = std::make_shared<AxmlChunk>(std::string(50, 'b'));
      bool threw = false;
      try {
        writer->setMetadata(nullptr, bad);
      } catch (const std::exception&) {
        threw = true;
      }
      TASK_CHECK(threw);

      // the previous arena content is still the good 48-byte axml
      TASK_CHECK(writer->reservedAxmlChunk()->data() == good->data());
      const std::vector<RawChunk> before = rawScan(path);
      const RawChunk* axmlBefore = rawFind(before, "axml");
      TASK_CHECK(axmlBefore != nullptr);
      TASK_CHECK(axmlBefore->sizeField == 48);

      // retry with a fitting payload
      auto retry = std::make_shared<AxmlChunk>(std::string(46, 'r'));
      writer->setMetadata(nullptr, retry);
      writer->close();
    }
    const std::vector<RawChunk> chunks = rawScan(path);
    const RawChunk* axml = rawFind(chunks, "axml");
    const RawChunk* junk = rawFindLast(chunks, "JUNK");
    const RawChunk* data = rawFind(chunks, "data");
    TASK_CHECK(axml && junk && data);
    TASK_CHECK(axml->sizeField == 46);
    TASK_CHECK(axml->physicalSize() + junk->physicalSize() ==
               arenaCapacity);
    TASK_CHECK(junk->position + junk->physicalSize() == data->position);
    removeIfExists(path);
    return 0;
  }

  int odd_24bit_data_and_unknown() {
    const std::string path = tmpPath("odd_unknown");
    removeIfExists(path);
    const uint16_t channels = 1;
    const uint64_t frames = 5;  // 5 * 3 = 15 bytes, odd data size
    const std::vector<float> pcm = distinctivePcm(frames, channels);
    {
      auto writer = writeFile(path, channels, 48000, 24);
      writer->reserveMetadata(128);
      writer->setMetadata(makeChna(channels, "1"),
                          makeAxml("odd-data", true));
      writer->write(const_cast<float*>(pcm.data()), frames);
      // unknown chunk after the audio, payload hand-crafted (odd length so
      // it also exercises the post-data RIFF padding byte)
      class SevenChunk : public Chunk {
       public:
        uint32_t id() const override { return utils::fourCC("xyz1"); }
        uint64_t size() const override { return 7; }
        void write(std::ostream& s) const override { s << "qrstuvw"; }
      };
      writer->appendPostDataChunk(std::make_shared<SevenChunk>());
      writer->close();
    }

    const std::string before = readFileBytes(path);
    const std::vector<RawChunk> chunks = rawScan(path);
    const RawChunk* data = rawFind(chunks, "data");
    const RawChunk* xyz = rawFind(chunks, "xyz1");
    TASK_CHECK(data && xyz);
    TASK_CHECK(data->sizeField == 15);  // odd
    const std::string dataPad =
        readAt(path, data->position + 8ull + 15ull, 1);
    TASK_CHECK(dataPad.size() == 1 && dataPad[0] == '\0');
    TASK_CHECK(xyz->position > data->position);
    TASK_CHECK(xyz->sizeField == 7);
    const std::string xyzPad =
        readAt(path, xyz->position + 8ull + 7ull, 1);
    TASK_CHECK(xyzPad[0] == '\0');
    TASK_CHECK(fileSize64(path) ==
               xyz->position + xyz->physicalSize());

    // in-place update must preserve everything outside the arena
    auto chna2 = makeChna(channels, "2");
    updateMetadataInPlace(path, chna2, nullptr);
    const std::string after = readFileBytes(path);
    TASK_CHECK(after.size() == before.size());
    const std::vector<RawChunk> afterScan = rawScan(path);
    const RawChunk* arenaAxml = rawFind(afterScan, "axml");
    TASK_CHECK(arenaAxml == nullptr);  // axml deleted
    const RawChunk* chnaAfter = rawFind(afterScan, "chna");
    TASK_CHECK(chnaAfter != nullptr);
    const uint64_t arenaStart = chnaAfter->position;
    const RawChunk* dataAfter = rawFind(afterScan, "data");
    TASK_CHECK(dataAfter != nullptr);
    for (size_t i = 0; i < after.size(); ++i) {
      const bool inArena =
          i >= arenaStart && i < dataAfter->position;
      if (!inArena) TASK_CHECK(before[i] == after[i]);
    }
    removeIfExists(path);
    return 0;
  }

  int sparse_bw64_64bit() {
    const std::string path = tmpPath("sparse_bw64");
    removeIfExists(path);
    const SparseFile info =
        buildSparse64(path, "BW64", 0);
    TASK_CHECK(info.totalSize > 0x100000000ull);
    TASK_CHECK(fileSize64(path) == info.totalSize);
#if defined(__unix__) || defined(__APPLE__)
    struct stat st;
    TASK_CHECK(stat(path.c_str(), &st) == 0);
    TASK_CHECK(static_cast<uint64_t>(st.st_size) == info.totalSize);
    // the hole means far fewer blocks are allocated than the logical size
    TASK_CHECK(static_cast<uint64_t>(st.st_blocks) * 512ull <
               info.totalSize / 2ull);
#endif

    const std::string before = readAt(path, 0, 12);
    auto chna = std::make_shared<ChnaChunk>();
    chna->addAudioId(
        AudioId(1, "ATU_00000001", "AT_00010001_01", "AP_00010001"));
    auto axml = std::make_shared<AxmlChunk>("sparse");
    updateMetadataInPlace(path, chna, axml);

    // file length, container, ds64 and data header untouched
    TASK_CHECK(fileSize64(path) == info.totalSize);
    TASK_CHECK(readAt(path, 0, 12) == before);
    const std::string dataHeader =
        readAt(path, info.dataPayloadOffset - 8ull, 8);
    TASK_CHECK(dataHeader.substr(0, 4) == "data");
    TASK_CHECK(getU32(dataHeader, 4) == 0xffffffffu);

    // head and tail PCM markers at real 64-bit offsets
    const std::string head = readAt(path, info.dataPayloadOffset, 3);
    TASK_CHECK(head == std::string("\x11\x22\x33", 3));
    const std::string tail =
        readAt(path, info.dataPayloadOffset + info.dataSize - 3ull, 3);
    TASK_CHECK(tail == std::string("\xaa\xbb\xcc", 3));

    // arena rewritten, ds64 (directly after WAVE) untouched and still first
    const std::vector<RawChunk> chunks = rawScan(path);
    TASK_CHECK(chunks.size() >= 5);
    TASK_CHECK(chunks[0].id == "ds64");
    TASK_CHECK(chunks[0].position == 12);
    const RawChunk* chnaRaw = rawFind(chunks, "chna");
    const RawChunk* axmlRaw = rawFind(chunks, "axml");
    const RawChunk* junk = rawFind(chunks, "JUNK");
    TASK_CHECK(chnaRaw && axmlRaw && junk);
    TASK_CHECK(chnaRaw->position == info.arenaOffset);
    TASK_CHECK(chnaRaw->position + chnaRaw->physicalSize() ==
               axmlRaw->position);
    TASK_CHECK(axmlRaw->position + axmlRaw->physicalSize() ==
               junk->position);
    TASK_CHECK(junk->position + junk->physicalSize() ==
               info.dataPayloadOffset - 8ull);

    // the library reader must handle the real 64-bit data position too
    auto reader = readFile(path);
    TASK_CHECK(reader->channels() == 1);
    TASK_CHECK(reader->numberOfFrames() == info.dataSize / 3ull);
    TASK_CHECK(reader->chnaChunk()->numUids() == 1);
    TASK_CHECK(reader->axmlChunk()->data() == "sparse");
    reader->seek(static_cast<int32_t>(reader->numberOfFrames() - 1ull));
    float last = 0;
    TASK_CHECK(reader->read(&last, 1) == 1);
    const std::string tailAgain =
        readAt(path, info.dataPayloadOffset + info.dataSize - 3ull, 3);
    TASK_CHECK(tailAgain == tail);
    reader->close();
    removeIfExists(path);
    return 0;
  }

  int sparse_rf64_64bit() {
    const std::string path = tmpPath("sparse_rf64");
    removeIfExists(path);
    const SparseFile info = buildSparse64(path, "RF64", 0);
    TASK_CHECK(fileSize64(path) == info.totalSize);
    updateMetadataInPlace(path, nullptr,
                          std::make_shared<AxmlChunk>("rf64"));
    const std::vector<RawChunk> chunks = rawScan(path);
    TASK_CHECK(chunks[0].id == "ds64");
    const RawChunk* axml = rawFind(chunks, "axml");
    TASK_CHECK(axml != nullptr);
    TASK_CHECK(axml->position >= info.arenaOffset);
    TASK_CHECK(fileSize64(path) == info.totalSize);
    const std::string tail =
        readAt(path, info.dataPayloadOffset + info.dataSize - 3ull, 3);
    TASK_CHECK(tail == std::string("\xaa\xbb\xcc", 3));
    auto reader = readFile(path);
    TASK_CHECK(reader->fileFormat() == utils::fourCC("RF64"));
    TASK_CHECK(reader->axmlChunk()->data() == "rf64");
    reader->close();
    removeIfExists(path);
    return 0;
  }

  // ======================================================================
  // reopen_preserve
  // ======================================================================

  int reopen_two_updates_pcm() {
    const std::string path = tmpPath("reopen");
    removeIfExists(path);
    const uint16_t channels = 2;
    const uint64_t frames = 6;
    const std::vector<float> pcm = distinctivePcm(frames, channels);
    {
      auto writer = writeFile(path, channels, 48000, 24,
                              makeChna(channels, "1"),
                              makeAxml("reopen-zero", false));
      writer->reserveMetadata(2048);
      writer->write(const_cast<float*>(pcm.data()), frames);
      writer->close();
    }

    // first update through the public free function, reopened with readFile
    updateMetadataInPlace(path, makeChna(channels, "2"),
                          makeAxml("reopen-one", true));
    {
      auto reader = readFile(path);
      TASK_CHECK(reader->chnaChunk()->numUids() == channels);
      TASK_CHECK(reader->axmlChunk()->data() ==
                 makeAxml("reopen-one", true)->data());
      std::vector<float> got(frames * channels);
      TASK_CHECK(reader->read(got.data(), frames) == frames);
      for (size_t i = 0; i < got.size(); ++i) {
        const float d = got[i] - pcm[i];
        TASK_CHECK(d > -1e-6f && d < 1e-6f);
      }
      reader->close();
    }

    // second update: delete chna, swap axml again
    updateMetadataInPlace(path, nullptr, makeAxml("reopen-two", false));
    {
      auto reader = readFile(path);
      TASK_CHECK(reader->chnaChunk() == nullptr);
      TASK_CHECK(reader->axmlChunk()->data() ==
                 makeAxml("reopen-two", false)->data());
      TASK_CHECK(reader->numberOfFrames() == frames);
      reader->close();
    }

    // raw PCM bytes are identical to a freshly rendered reference
    const std::string refPath = tmpPath("reopen_ref");
    removeIfExists(refPath);
    {
      auto writer = writeFile(refPath, channels, 48000, 24);
      writer->write(const_cast<float*>(pcm.data()), frames);
      writer->close();
    }
    const std::vector<RawChunk> edited = rawScan(path);
    const std::vector<RawChunk> ref = rawScan(refPath);
    const RawChunk* editedData = rawFind(edited, "data");
    const RawChunk* refData = rawFind(ref, "data");
    TASK_CHECK(editedData->sizeField == refData->sizeField);
    const std::string editedPcm =
        readAt(path, editedData->position + 8, editedData->sizeField);
    const std::string refPcm =
        readAt(refPath, refData->position + 8, refData->sizeField);
    TASK_CHECK(editedPcm == refPcm);
    removeIfExists(path);
    removeIfExists(refPath);
    return 0;
  }

  int outside_arena_unchanged() {
    const std::string path = tmpPath("outside");
    removeIfExists(path);
    {
      auto writer = writeFile(path, 2, 48000, 16, makeChna(2, "1"),
                              makeAxml("outside", false));
      writer->reserveMetadata(300);
      float pcm[6] = {0.1f, 0.2f, -0.3f, 0.4f, -0.5f, 0.6f};
      writer->write(pcm, 3);
      writer->close();
    }
    const std::string before = readFileBytes(path);
    const std::vector<RawChunk> initialScan = rawScan(path);
    const uint64_t dataPos = rawFind(initialScan, "data")->position;

    for (int iteration = 0; iteration < 3; ++iteration) {
      if (iteration == 1) {
        updateMetadataInPlace(path, nullptr, nullptr);
      } else {
        updateMetadataInPlace(path, makeChna(2, "3"),
                              makeAxml("outside-new", true));
      }
      const std::string after = readFileBytes(path);
      TASK_CHECK(after.size() == before.size());
      const std::vector<RawChunk> chunks = rawScan(path);
      const RawChunk* data = rawFind(chunks, "data");
      TASK_CHECK(data->position == dataPos);  // data never moves
      // find arena start: first chna/axml/JUNK after fmt
      const RawChunk* fmt = rawFind(chunks, "fmt ");
      uint64_t arenaStart = data->position;
      for (size_t i = 0; i < chunks.size(); ++i) {
        if (chunks[i].position > fmt->position &&
            chunks[i].position < arenaStart &&
            (chunks[i].id == "chna" || chunks[i].id == "axml" ||
             chunks[i].id == "JUNK")) {
          arenaStart = chunks[i].position;
        }
      }
      for (size_t i = 0; i < after.size(); ++i) {
        const bool inArena = i >= arenaStart && i < data->position;
        if (!inArena) TASK_CHECK(before[i] == after[i]);
      }
    }
    removeIfExists(path);
    return 0;
  }

  int reject_duplicate_and_outside() {
    const std::string path = tmpPath("reject_dup");
    removeIfExists(path);

    // two chna chunks inside the pre-data run -> duplicate metadata
    {
      std::vector<RawSpec> pre;
      pre.push_back(RawSpec{"fmt ", rawFmtPayload(1, 48000, 24)});
      const std::string chnaBytes = rawChnaPayload(
          std::vector<RawAudioId>{
              RawAudioId{1, "ATU_00000001", "AT_00010001_01",
                         "AP_00010001"}});
      pre.push_back(RawSpec{"chna", chnaBytes});
      pre.push_back(RawSpec{"chna", chnaBytes});
      buildPlainRiff(path, pre, std::string(6, '\x7f'),
                     std::vector<RawSpec>());
    }
    std::string before = readFileBytes(path);
    bool threw = false;
    try {
      updateMetadataInPlace(path, nullptr, nullptr);
    } catch (const std::exception& e) {
      threw = true;
      TASK_CHECK(std::string(e.what()).find("chna") != std::string::npos);
    }
    TASK_CHECK(threw);
    TASK_CHECK(readFileBytes(path) == before);

    // axml after the data chunk is outside the arena -> rejected
    {
      std::vector<RawSpec> pre;
      pre.push_back(RawSpec{"fmt ", rawFmtPayload(1, 48000, 24)});
      pre.push_back(RawSpec{"JUNK", std::string(24, '\0')});
      std::vector<RawSpec> post;
      post.push_back(RawSpec{"axml", std::string("outside")});
      buildPlainRiff(path, pre, std::string(3, '\x40'), post);
    }
    before = readFileBytes(path);
    threw = false;
    try {
      updateMetadataInPlace(path, nullptr,
                            std::make_shared<AxmlChunk>("new"));
    } catch (const std::exception& e) {
      threw = true;
      TASK_CHECK(std::string(e.what()).find("axml") != std::string::npos);
    }
    TASK_CHECK(threw);
    TASK_CHECK(readFileBytes(path) == before);
    removeIfExists(path);
    return 0;
  }

  int reject_no_arena_and_truncation() {
    const std::string path = tmpPath("reject_noarena");
    removeIfExists(path);

    // fmt directly followed by data: no editable arena
    {
      std::vector<RawSpec> pre;
      pre.push_back(RawSpec{"fmt ", rawFmtPayload(1, 48000, 16)});
      buildPlainRiff(path, pre, std::string(4, '\0'),
                     std::vector<RawSpec>());
    }
    const std::string before = readFileBytes(path);
    bool threw = false;
    try {
      updateMetadataInPlace(path, nullptr, nullptr);
    } catch (const std::exception&) {
      threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(readFileBytes(path) == before);

    // a truncated reserved file is rejected without further damage
    const std::string reserved = tmpPath("reject_trunc");
    removeIfExists(reserved);
    {
      auto writer = writeFile(reserved, 1, 48000, 24);
      writer->reserveMetadata(128);
      float v[4] = {0.2f, -0.2f, 0.4f, -0.4f};
      writer->write(v, 4);
      writer->close();
    }
    const uint64_t fullSize = fileSize64(reserved);
    const std::string reservedBytes = readFileBytes(reserved);
    TASK_CHECK(reservedBytes.size() == fullSize);
    // cut the last five bytes so the final chunk overruns the file
    writeFileBytes(reserved,
                   reservedBytes.substr(0, reservedBytes.size() - 5));
    const std::string truncated = readFileBytes(reserved);
    threw = false;
    try {
      updateMetadataInPlace(reserved, makeChna(1, "9"), nullptr);
    } catch (const std::exception&) {
      threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(readFileBytes(reserved) == truncated);
    removeIfExists(path);
    removeIfExists(reserved);
    return 0;
  }

  int reject_ds64_table_and_extended() {
    const std::string path = tmpPath("reject_ds64");
    removeIfExists(path);

    // non-empty ds64 table is explicitly unsupported
    SparseFile info = buildSparse64(path, "BW64", 1);
    const std::string before = readAt(path, 0, 64);
    bool threw = false;
    try {
      updateMetadataInPlace(path, nullptr, nullptr);
    } catch (const std::exception& e) {
      threw = true;
      TASK_CHECK(std::string(e.what()).find("ds64") != std::string::npos);
    }
    TASK_CHECK(threw);
    TASK_CHECK(readAt(path, 0, 64) == before);
    removeIfExists(path);

    // 0xffffffff on a non-data chunk is unsupported
    {
      std::string body;
      body += "WAVE";
      std::string ds64;
      // total file: 12 (RIFF) + 36 (ds64) + 24 (fmt) + 24 (JUNK) +
      // 16 (data) + 16 (big JUNK) = 128 -> bw64Size 120, data size 8
      putU64(ds64, 120ull);
      putU64(ds64, 8ull);
      putU64(ds64, 0);
      putU32(ds64, 0);
      body += makeChunk("ds64", ds64);
      body += makeChunk("fmt ", rawFmtPayload(1, 48000, 16));
      body += makeChunk("JUNK", std::string(16, '\0'));
      body += makeChunk("data", std::string(8, '\0'));
      // a JUNK after data carrying an extended size marker
      std::string bigJunk = "JUNK";
      putU32(bigJunk, 0xffffffffu);
      bigJunk += std::string(8, '\0');
      body += bigJunk;

      std::string bytes;
      bytes += "BW64";
      putU32(bytes, 0xffffffffu);
      bytes += body;
      writeFileBytes(path, bytes);
    }
    const std::string before2 = readFileBytes(path);
    threw = false;
    try {
      updateMetadataInPlace(path, nullptr, nullptr);
    } catch (const std::exception& e) {
      threw = true;
      TASK_CHECK(std::string(e.what()).find("JUNK") != std::string::npos);
    }
    TASK_CHECK(threw);
    TASK_CHECK(readFileBytes(path) == before2);
    removeIfExists(path);
    (void)info;
    return 0;
  }

  int unknown_chunk_is_boundary() {
    const std::string path = tmpPath("boundary");
    removeIfExists(path);
    {
      std::vector<RawSpec> pre;
      pre.push_back(RawSpec{"fmt ", rawFmtPayload(2, 48000, 24)});
      pre.push_back(RawSpec{"xyz1", std::string("boundary-marker")});
      pre.push_back(RawSpec{"JUNK", std::string(120, '\0')});  // 128 phys
      std::vector<RawSpec> post;
      post.push_back(RawSpec{"xyz2", std::string("tail-marker")});
      buildPlainRiff(path, pre, std::string(12, '\x55'), post);
    }
    const std::string before = readFileBytes(path);
    const std::vector<RawChunk> chunksBefore = rawScan(path);
    const RawChunk* xyz1 = rawFind(chunksBefore, "xyz1");
    const RawChunk* arenaJunk = rawFind(chunksBefore, "JUNK");
    const RawChunk* data = rawFind(chunksBefore, "data");
    TASK_CHECK(xyz1 && arenaJunk && data);
    TASK_CHECK(xyz1->position + xyz1->physicalSize() ==
               arenaJunk->position);  // unknown chunk bounds the arena
    TASK_CHECK(arenaJunk->position + arenaJunk->physicalSize() ==
               data->position);

    auto chna = std::make_shared<ChnaChunk>();
    chna->addAudioId(
        AudioId(1, "ATU_00000001", "AT_00010001_01", "AP_00010001"));
    chna->addAudioId(
        AudioId(2, "ATU_00000002", "AT_00010002_01", "AP_00010002"));
    updateMetadataInPlace(path, chna,
                          std::make_shared<AxmlChunk>("boundary"));

    const std::string after = readFileBytes(path);
    TASK_CHECK(after.size() == before.size());
    const std::vector<RawChunk> chunksAfter = rawScan(path);
    TASK_CHECK(rawCount(chunksAfter, "xyz1") == 1);
    TASK_CHECK(rawCount(chunksAfter, "xyz2") == 1);
    const RawChunk* xyz1After = rawFind(chunksAfter, "xyz1");
    const RawChunk* xyz2After = rawFind(chunksAfter, "xyz2");
    TASK_CHECK(xyz1After->position == xyz1->position);
    TASK_CHECK(readAt(path, xyz1After->position + 8, 15) ==
               "boundary-marker");
    TASK_CHECK(readAt(path, xyz2After->position + 8, 11) ==
               "tail-marker");
    const RawChunk* chnaRaw = rawFind(chunksAfter, "chna");
    TASK_CHECK(chnaRaw != nullptr);
    TASK_CHECK(chnaRaw->position == arenaJunk->position);  // arena start fixed
    TASK_CHECK(rawFind(chunksAfter, "data")->position == data->position);

    auto reader = readFile(path);
    TASK_CHECK(reader->channels() == 2);
    TASK_CHECK(reader->chnaChunk()->numUids() == 2);
    TASK_CHECK(reader->axmlChunk()->data() == "boundary");
    reader->close();
    removeIfExists(path);
    return 0;
  }

}  // namespace

// ======================================================================
// registration (global symbols matching the extern declarations in
// task_support/check.h; the runner above supplies main())
// ======================================================================

const struct task_case task_cases[] = {
    {"reserve_lifecycle", "lifecycle_chain", lifecycle_chain},
    {"reserve_lifecycle", "reserve_argument_guards",
     reserve_argument_guards},
    {"reserve_lifecycle", "reserve_boundary_sizes",
     reserve_boundary_sizes},
    {"reserve_lifecycle", "initial_metadata_moved",
     initial_metadata_moved},
    {"reserve_lifecycle", "reserve_too_small_for_initial",
     reserve_too_small_for_initial},
    {"reserve_lifecycle", "legacy_no_reserve_control",
     legacy_no_reserve_control},
    {"reserve_lifecycle", "chna_rules", chna_rules},

    {"arena_layout", "writer_arena_bytes", writer_arena_bytes},
    {"arena_layout", "delete_both_all_junk", delete_both_all_junk},
    {"arena_layout", "short_tail_reject_retry", short_tail_reject_retry},
    {"arena_layout", "odd_24bit_data_and_unknown",
     odd_24bit_data_and_unknown},
    {"arena_layout", "sparse_bw64_64bit", sparse_bw64_64bit},
    {"arena_layout", "sparse_rf64_64bit", sparse_rf64_64bit},

    {"reopen_preserve", "reopen_two_updates_pcm",
     reopen_two_updates_pcm},
    {"reopen_preserve", "outside_arena_unchanged",
     outside_arena_unchanged},
    {"reopen_preserve", "reject_duplicate_and_outside",
     reject_duplicate_and_outside},
    {"reopen_preserve", "reject_no_arena_and_truncation",
     reject_no_arena_and_truncation},
    {"reopen_preserve", "reject_ds64_table_and_extended",
     reject_ds64_table_and_extended},
    {"reopen_preserve", "unknown_chunk_is_boundary",
     unknown_chunk_is_boundary},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
