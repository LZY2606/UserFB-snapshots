// Regression tests for the forward (non-seekable) streaming API:
// bw64::readStream and bw64::writeStream.
//
// Input bytes are constructed and output headers are parsed independently of
// the library, so that the library does not only validate itself.

#include <bw64/bw64.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "task_support/check.h"

namespace {

  // ------------------------------------------------------------------
  // independent byte construction helpers
  // ------------------------------------------------------------------

  void put16(std::string& s, uint16_t v) {
    s.push_back(char(v & 0xff));
    s.push_back(char((v >> 8) & 0xff));
  }
  void put32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; i++) s.push_back(char((v >> (8 * i)) & 0xff));
  }
  void put64(std::string& s, uint64_t v) {
    for (int i = 0; i < 8; i++) s.push_back(char((v >> (8 * i)) & 0xff));
  }

  // chunk header + payload + padding byte for odd sizes
  std::string makeChunk(const std::string& id, const std::string& payload) {
    std::string s = id;
    put32(s, uint32_t(payload.size()));
    s += payload;
    if (payload.size() % 2) s.push_back('\0');
    return s;
  }

  std::string makeFmtPayload(uint16_t channels, uint32_t sampleRate,
                             uint16_t bitDepth) {
    std::string p;
    put16(p, 1);  // PCM
    put16(p, channels);
    put32(p, sampleRate);
    put32(p, sampleRate * channels * bitDepth / 8);
    put16(p, channels * bitDepth / 8);
    put16(p, bitDepth);
    return p;
  }

  // RIFF file from everything after the WAVE id
  std::string makeRiff(const std::string& chunks) {
    std::string s = "RIFF";
    put32(s, uint32_t(4 + chunks.size()));
    s += "WAVE";
    s += chunks;
    return s;
  }

  // BW64 file with a ds64 chunk and a 0xffffffff data size field
  std::string makeBw64(const std::string& id, uint64_t dataSize,
                       uint64_t sampleCount, const std::string& fmtPayload,
                       const std::string& dataPayload,
                       const std::string& tailChunks = "") {
    uint64_t riffSize = 4 + (8 + 28) + (8 + fmtPayload.size()) + 8 +
                        dataSize + (dataSize % 2) + tailChunks.size();
    std::string ds64;
    put64(ds64, riffSize);
    put64(ds64, dataSize);
    put64(ds64, sampleCount);
    put32(ds64, 0);  // tableLength

    std::string s = id;
    put32(s, 0xffffffffu);
    s += "WAVE";
    s += makeChunk("ds64", ds64);
    s += makeChunk("fmt ", fmtPayload);
    s += "data";
    put32(s, 0xffffffffu);
    s += dataPayload;
    if (dataPayload.size() % 2) s.push_back('\0');
    s += tailChunks;
    return s;
  }

  void putSample16(std::string& s, int16_t v) { put16(s, uint16_t(v)); }
  void putSample24(std::string& s, int32_t v) {
    s.push_back(char(v & 0xff));
    s.push_back(char((v >> 8) & 0xff));
    s.push_back(char((v >> 16) & 0xff));
  }
  void putSample32(std::string& s, int32_t v) { put32(s, uint32_t(v)); }

  float expected16(int16_t v) { return v / 32768.0f; }
  float expected24(int32_t v) { return v / 8388608.0f; }

  // per-channel distinguishable sample patterns
  int16_t pattern16(uint64_t frame, uint16_t channel) {
    int16_t v = int16_t(1000 + (frame % 100) * 100 + channel * 17);
    return channel % 2 ? int16_t(-v) : v;
  }
  int32_t pattern24(uint64_t frame, uint16_t channel) {
    int32_t v = int32_t(((frame * 4099 + channel * 1000003) % 8000000)) - 4000000;
    return v;
  }

  std::string makeChnaPayload() {
    std::string p;
    put16(p, 1);             // numTracks
    put16(p, 1);             // numUids
    put16(p, 1);             // trackIndex
    p += "ATU_00000001";    // 12
    p += "AT_00010001_01";  // 14
    p += "AP_00010001";     // 11
    p += ' ';
    return p;
  }

  // ------------------------------------------------------------------
  // independent byte parsing helpers
  // ------------------------------------------------------------------

  uint16_t get16(const std::string& s, size_t o) {
    return uint16_t(uint8_t(s[o])) | uint16_t(uint16_t(uint8_t(s[o + 1])) << 8);
  }
  uint32_t get32(const std::string& s, size_t o) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= uint32_t(uint8_t(s[o + i])) << (8 * i);
    return v;
  }
  uint64_t get64(const std::string& s, size_t o) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= uint64_t(uint8_t(s[o + i])) << (8 * i);
    return v;
  }
  std::string get4(const std::string& s, size_t o) { return s.substr(o, 4); }

  std::string loadFile(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
  }

  // ------------------------------------------------------------------
  // non-seekable stream buffers
  // ------------------------------------------------------------------

  class ForwardInBuf : public std::streambuf {
   public:
    explicit ForwardInBuf(const std::string& data) : data_(data) {}
    bool seekAttempted = false;

   protected:
    int_type underflow() override {
      if (pos_ >= data_.size()) return traits_type::eof();
      return traits_type::to_int_type(data_[pos_]);
    }
    int_type uflow() override {
      if (pos_ >= data_.size()) return traits_type::eof();
      return traits_type::to_int_type(data_[pos_++]);
    }
    std::streamsize xsgetn(char* s, std::streamsize n) override {
      std::streamsize count =
          std::min(n, std::streamsize(data_.size() - pos_));
      if (count > 0) {
        std::memcpy(s, data_.data() + pos_, size_t(count));
        pos_ += size_t(count);
      }
      return count;
    }
    pos_type seekoff(off_type, std::ios_base::seekdir,
                     std::ios_base::openmode) override {
      seekAttempted = true;
      return pos_type(off_type(-1));
    }
    pos_type seekpos(pos_type, std::ios_base::openmode) override {
      seekAttempted = true;
      return pos_type(off_type(-1));
    }

   private:
    const std::string& data_;
    size_t pos_ = 0;
  };

  class ForwardOutBuf : public std::streambuf {
   public:
    std::string data;
    bool seekAttempted = false;

   protected:
    int_type overflow(int_type c) override {
      if (!traits_type::eq_int_type(c, traits_type::eof()))
        data.push_back(traits_type::to_char_type(c));
      return traits_type::not_eof(c);
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
      data.append(s, size_t(n));
      return n;
    }
    pos_type seekoff(off_type, std::ios_base::seekdir,
                     std::ios_base::openmode) override {
      seekAttempted = true;
      return pos_type(off_type(-1));
    }
    pos_type seekpos(pos_type, std::ios_base::openmode) override {
      seekAttempted = true;
      return pos_type(off_type(-1));
    }
  };

  // output buffer which fails after a given number of bytes
  class FailingOutBuf : public std::streambuf {
   public:
    explicit FailingOutBuf(size_t okBytes) : remaining_(okBytes) {}

   protected:
    int_type overflow(int_type c) override {
      if (remaining_ == 0) return traits_type::eof();
      --remaining_;
      return c;
    }
    std::streamsize xsputn(const char*, std::streamsize n) override {
      std::streamsize count = std::min<std::streamsize>(n, remaining_);
      remaining_ -= size_t(count);
      return count;
    }

   private:
    size_t remaining_;
  };

  template <typename Ex, typename F>
  bool throws(F&& f) {
    try {
      f();
    } catch (const Ex&) {
      return true;
    } catch (...) {
      return false;
    }
    return false;
  }

  // ------------------------------------------------------------------
  // reader_forward
  // ------------------------------------------------------------------

  int reader_basic_riff() {
    const uint64_t frames = 10;
    std::string pcm;
    for (uint64_t f = 0; f < frames; f++)
      for (uint16_t c = 0; c < 2; c++) putSample16(pcm, pattern16(f, c));
    std::string file = makeRiff(makeChunk("fmt ", makeFmtPayload(2, 48000, 16)) +
                                makeChunk("data", pcm));

    ForwardInBuf buf(file);
    std::istream stream(&buf);
    auto reader = bw64::readStream(stream);
    TASK_CHECK(reader->fileFormat() == bw64::utils::fourCC("RIFF"));
    TASK_CHECK(reader->formatTag() == 1);
    TASK_CHECK(reader->channels() == 2);
    TASK_CHECK(reader->sampleRate() == 48000);
    TASK_CHECK(reader->bitDepth() == 16);
    TASK_CHECK(reader->numberOfFrames() == frames);
    TASK_CHECK(reader->tell() == 0);
    TASK_CHECK(!reader->eof());
    TASK_CHECK(reader->hasChunk(bw64::utils::fourCC("fmt ")));
    TASK_CHECK(reader->hasChunk(bw64::utils::fourCC("data")));

    std::vector<float> out(frames * 2);
    TASK_CHECK(reader->read(out.data(), 4) == 4);
    TASK_CHECK(reader->tell() == 4);
    TASK_CHECK(reader->read(out.data() + 8, 4) == 4);
    TASK_CHECK(reader->read(out.data() + 16, 100) == 2);  // clamped
    TASK_CHECK(reader->tell() == frames);
    TASK_CHECK(reader->eof());
    for (uint64_t f = 0; f < frames; f++)
      for (uint16_t c = 0; c < 2; c++)
        TASK_CHECK(out[f * 2 + c] == expected16(pattern16(f, c)));
    TASK_CHECK(reader->read(out.data(), 1) == 0);
    TASK_CHECK(!buf.seekAttempted);
    return 0;
  }

  int reader_bw64() {
    // 24 bit mono, 5 frames -> odd 15 byte data payload, plus trailing axml
    const uint64_t frames = 5;
    std::string pcm;
    for (uint64_t f = 0; f < frames; f++) putSample24(pcm, pattern24(f, 0));
    std::string axml = "<a/>";
    std::string file = makeBw64("BW64", pcm.size(), frames,
                                makeFmtPayload(1, 48000, 24), pcm,
                                makeChunk("axml", axml));

    ForwardInBuf buf(file);
    std::istream stream(&buf);
    auto reader = bw64::readStream(stream);
    TASK_CHECK(reader->fileFormat() == bw64::utils::fourCC("BW64"));
    TASK_CHECK(reader->ds64Chunk());
    TASK_CHECK(reader->ds64Chunk()->bw64Size() == file.size() - 8);
    TASK_CHECK(reader->ds64Chunk()->dataSize() == 15);
    TASK_CHECK(reader->ds64Chunk()->dummySize() == frames);
    TASK_CHECK(reader->numberOfFrames() == frames);
    TASK_CHECK(reader->bitDepth() == 24);
    TASK_CHECK(!reader->axmlChunk());  // after data: not parsed yet

    std::vector<float> out(frames);
    TASK_CHECK(reader->read(out.data(), frames) == frames);
    for (uint64_t f = 0; f < frames; f++)
      TASK_CHECK(out[f] == expected24(pattern24(f, 0)));
    TASK_CHECK(reader->read(out.data(), 1) == 0);
    TASK_CHECK(reader->axmlChunk());
    TASK_CHECK(reader->axmlChunk()->data() == axml);
    TASK_CHECK(!buf.seekAttempted);
    return 0;
  }

  int reader_rf64() {
    const uint64_t frames = 4;
    std::string pcm;
    for (uint64_t f = 0; f < frames; f++) putSample16(pcm, pattern16(f, 0));
    std::string file =
        makeBw64("RF64", pcm.size(), frames, makeFmtPayload(1, 44100, 16), pcm);

    ForwardInBuf buf(file);
    std::istream stream(&buf);
    auto reader = bw64::readStream(stream);
    TASK_CHECK(reader->fileFormat() == bw64::utils::fourCC("RF64"));
    TASK_CHECK(reader->ds64Chunk());
    TASK_CHECK(reader->sampleRate() == 44100);
    TASK_CHECK(reader->numberOfFrames() == frames);
    std::vector<float> out(frames);
    TASK_CHECK(reader->read(out.data(), frames) == frames);
    for (uint64_t f = 0; f < frames; f++)
      TASK_CHECK(out[f] == expected16(pattern16(f, 0)));
    TASK_CHECK(reader->read(out.data(), 1) == 0);
    TASK_CHECK(!buf.seekAttempted);
    return 0;
  }

  int reader_tail_metadata() {
    const uint64_t frames = 6;
    std::string pcm;
    for (uint64_t f = 0; f < frames; f++) putSample16(pcm, pattern16(f, 0));
    std::string axml = "<adm>odd</adm>";  // 13 bytes, odd
    std::string tail = makeChunk("axml", axml) + makeChunk("chna", makeChnaPayload()) +
                       makeChunk("abcd", std::string("XYZ", 3));
    std::string file = makeRiff(makeChunk("fmt ", makeFmtPayload(1, 48000, 16)) +
                                makeChunk("data", pcm) + tail);

    ForwardInBuf buf(file);
    std::istream stream(&buf);
    auto reader = bw64::readStream(stream);
    // only chunks up to the data chunk are known so far
    TASK_CHECK(!reader->axmlChunk());
    TASK_CHECK(!reader->chnaChunk());
    TASK_CHECK(!reader->hasChunk(bw64::utils::fourCC("abcd")));
    TASK_CHECK(reader->chunks().size() == 2);

    std::vector<float> out(frames);
    TASK_CHECK(reader->read(out.data(), frames) == frames);
    TASK_CHECK(!reader->axmlChunk());  // still not parsed
    // the first read returning 0 parses the trailer
    TASK_CHECK(reader->read(out.data(), 1) == 0);
    TASK_CHECK(reader->axmlChunk());
    TASK_CHECK(reader->axmlChunk()->data() == axml);
    TASK_CHECK(reader->chnaChunk());
    TASK_CHECK(reader->chnaChunk()->numTracks() == 1);
    TASK_CHECK(reader->chnaChunk()->numUids() == 1);
    TASK_CHECK(reader->chnaChunk()->audioIds()[0].uid() == "ATU_00000001");
    TASK_CHECK(reader->chnaChunk()->audioIds()[0].trackRef() == "AT_00010001_01");
    TASK_CHECK(reader->chnaChunk()->audioIds()[0].packRef() == "AP_00010001");
    TASK_CHECK(reader->hasChunk(bw64::utils::fourCC("abcd")));
    TASK_CHECK(reader->chunks().size() == 5);
    TASK_CHECK(!buf.seekAttempted);
    return 0;
  }

  int reader_two_files() {
    std::string pcm1, pcm2;
    for (uint64_t f = 0; f < 4; f++) putSample16(pcm1, pattern16(f, 0));
    for (uint64_t f = 0; f < 3; f++)
      for (uint16_t c = 0; c < 2; c++) putSample24(pcm2, pattern24(f, c));
    std::string file1 = makeRiff(makeChunk("fmt ", makeFmtPayload(1, 48000, 16)) +
                                 makeChunk("data", pcm1));
    std::string file2 = makeRiff(makeChunk("fmt ", makeFmtPayload(2, 44100, 24)) +
                                 makeChunk("data", pcm2));
    std::string combined = file1 + file2;

    ForwardInBuf buf(combined);
    std::istream stream(&buf);
    auto reader1 = bw64::readStream(stream);
    TASK_CHECK(reader1->numberOfFrames() == 4);
    std::vector<float> out(6);
    TASK_CHECK(reader1->read(out.data(), 4) == 4);
    for (uint64_t f = 0; f < 4; f++)
      TASK_CHECK(out[f] == expected16(pattern16(f, 0)));
    reader1->finish();
    reader1->finish();  // idempotent
    TASK_CHECK(reader1->tell() == 4);

    // the second file must not have been consumed by the first reader
    auto reader2 = bw64::readStream(stream);
    TASK_CHECK(reader2->channels() == 2);
    TASK_CHECK(reader2->sampleRate() == 44100);
    TASK_CHECK(reader2->bitDepth() == 24);
    TASK_CHECK(reader2->numberOfFrames() == 3);
    TASK_CHECK(reader2->read(out.data(), 3) == 3);
    for (uint64_t f = 0; f < 3; f++)
      for (uint16_t c = 0; c < 2; c++)
        TASK_CHECK(out[f * 2 + c] == expected24(pattern24(f, c)));
    reader2->finish();
    TASK_CHECK(stream.peek() == std::char_traits<char>::eof());
    TASK_CHECK(!buf.seekAttempted);
    return 0;
  }

  int reader_partial_finish() {
    const uint64_t frames = 10;
    std::string pcm;
    for (uint64_t f = 0; f < frames; f++) putSample16(pcm, pattern16(f, 0));
    std::string axml = "<adm/>";
    std::string file = makeRiff(makeChunk("fmt ", makeFmtPayload(1, 48000, 16)) +
                                makeChunk("data", pcm) + makeChunk("axml", axml));

    ForwardInBuf buf(file);
    std::istream stream(&buf);
    auto reader = bw64::readStream(stream);
    std::vector<float> out(frames);
    TASK_CHECK(reader->read(out.data(), 3) == 3);
    TASK_CHECK(reader->tell() == 3);
    for (uint64_t f = 0; f < 3; f++)
      TASK_CHECK(out[f] == expected16(pattern16(f, 0)));
    // discard the remaining audio and parse the trailer
    reader->finish();
    TASK_CHECK(reader->tell() == frames);
    TASK_CHECK(reader->eof());
    TASK_CHECK(reader->axmlChunk());
    TASK_CHECK(reader->axmlChunk()->data() == axml);
    reader->finish();  // idempotent
    TASK_CHECK(reader->tell() == frames);
    TASK_CHECK(reader->read(out.data(), 1) == 0);
    TASK_CHECK(!buf.seekAttempted);
    return 0;
  }

  int reader_partial_close() {
    std::string pcm1, pcm2;
    for (uint64_t f = 0; f < 4; f++) putSample16(pcm1, pattern16(f, 0));
    for (uint64_t f = 0; f < 3; f++) putSample16(pcm2, pattern16(f, 1));
    std::string file1 = makeRiff(makeChunk("fmt ", makeFmtPayload(1, 48000, 16)) +
                                 makeChunk("data", pcm1));
    std::string file2 = makeRiff(makeChunk("fmt ", makeFmtPayload(1, 48000, 16)) +
                                 makeChunk("data", pcm2));
    std::string combined = file1 + file2;

    ForwardInBuf buf(combined);
    std::istream stream(&buf);
    {
      auto reader = bw64::readStream(stream);
      float tmp[1];
      TASK_CHECK(reader->read(tmp, 1) == 1);
      reader->close();  // must not consume the remaining input
    }
    std::string rest{std::istreambuf_iterator<char>(stream),
                     std::istreambuf_iterator<char>()};
    // header of file1 is 12 + 24 + 8 = 44 bytes, plus one 2 byte frame
    TASK_CHECK(rest == combined.substr(44 + 2));
    TASK_CHECK(!buf.seekAttempted);
    return 0;
  }

  int reader_seek_throws() {
    std::string pcm;
    for (uint64_t f = 0; f < 6; f++) putSample16(pcm, pattern16(f, 0));
    std::string file = makeRiff(makeChunk("fmt ", makeFmtPayload(1, 48000, 16)) +
                                makeChunk("data", pcm));

    ForwardInBuf buf(file);
    std::istream stream(&buf);
    auto reader = bw64::readStream(stream);
    std::vector<float> out(6);
    TASK_CHECK(reader->read(out.data(), 2) == 2);
    TASK_CHECK(throws<std::logic_error>([&] { reader->seek(0); }));
    TASK_CHECK(throws<std::logic_error>([&] { reader->seek(1, std::ios::cur); }));
    TASK_CHECK(throws<std::logic_error>([&] { reader->seek(-1, std::ios::end); }));
    // position unchanged, reading continues normally
    TASK_CHECK(reader->tell() == 2);
    TASK_CHECK(reader->read(out.data(), 1) == 1);
    TASK_CHECK(out[0] == expected16(pattern16(2, 0)));
    TASK_CHECK(!buf.seekAttempted);
    return 0;
  }

  int reader_truncated() {
    std::string fmt = makeChunk("fmt ", makeFmtPayload(1, 48000, 16));

    // truncated data payload
    TASK_CHECK(throws<std::runtime_error>([&] {
      std::string file = "RIFF";
      put32(file, uint32_t(4 + fmt.size() + 8 + 8));
      file += "WAVE" + fmt + "data";
      put32(file, 8);
      file += std::string(4, '\x01');  // only 4 of 8 bytes present
      ForwardInBuf buf(file);
      std::istream stream(&buf);
      auto reader = bw64::readStream(stream);
      float tmp[4];
      reader->read(tmp, 4);  // 8 bytes announced, only 4 present
    }));

    // truncated chunk header after the data chunk
    TASK_CHECK(throws<std::runtime_error>([&] {
      std::string pcm(4, '\x01');
      std::string file = "RIFF";
      put32(file, uint32_t(4 + fmt.size() + 8 + pcm.size() + 4));
      file += "WAVE" + fmt + "data";
      put32(file, uint32_t(pcm.size()));
      file += pcm;
      file += "abcd";  // half a chunk header
      ForwardInBuf buf(file);
      std::istream stream(&buf);
      auto reader = bw64::readStream(stream);
      float tmp[2];
      reader->read(tmp, 2);
      reader->read(tmp, 1);  // triggers trailer parsing
    }));

    // truncated chunk payload after the data chunk
    TASK_CHECK(throws<std::runtime_error>([&] {
      std::string pcm(4, '\x01');
      std::string file = "RIFF";
      put32(file, uint32_t(4 + fmt.size() + 8 + pcm.size() + 8 + 10));
      file += "WAVE" + fmt + "data";
      put32(file, uint32_t(pcm.size()));
      file += pcm;
      file += "axml";
      put32(file, 10);
      file += std::string(3, 'x');  // only 3 of 10 bytes present
      ForwardInBuf buf(file);
      std::istream stream(&buf);
      auto reader = bw64::readStream(stream);
      float tmp[2];
      reader->read(tmp, 2);
      reader->read(tmp, 1);
    }));

    // truncated fmt chunk
    TASK_CHECK(throws<std::runtime_error>([&] {
      std::string file = "RIFF";
      put32(file, uint32_t(4 + 8 + 16));
      file += "WAVE";
      file += "fmt ";
      put32(file, 16);
      file += std::string(8, '\x01');  // only 8 of 16 bytes present
      ForwardInBuf buf(file);
      std::istream stream(&buf);
      auto reader = bw64::readStream(stream);
      (void)reader;
    }));

    // missing padding byte of an odd-sized trailing chunk
    TASK_CHECK(throws<std::runtime_error>([&] {
      std::string pcm(4, '\x01');
      std::string axml(9, 'x');  // odd size, padding declared but missing
      std::string file = "RIFF";
      put32(file, uint32_t(4 + fmt.size() + 8 + pcm.size() + 8 + 9 + 1));
      file += "WAVE" + fmt + "data";
      put32(file, uint32_t(pcm.size()));
      file += pcm;
      file += "axml";
      put32(file, 9);
      file += axml;
      ForwardInBuf buf(file);
      std::istream stream(&buf);
      auto reader = bw64::readStream(stream);
      float tmp[2];
      reader->read(tmp, 2);
      reader->read(tmp, 1);
    }));

    // truncated riff header
    TASK_CHECK(throws<std::runtime_error>([&] {
      std::string file = "RIF";
      ForwardInBuf buf(file);
      std::istream stream(&buf);
      auto reader = bw64::readStream(stream);
      (void)reader;
    }));
    return 0;
  }

  int reader_fmt_after_data() {
    std::string pcm(8, '\x01');
    std::string fmt = makeChunk("fmt ", makeFmtPayload(1, 48000, 16));

    // fmt chunk after the data chunk
    TASK_CHECK(throws<std::runtime_error>([&] {
      std::string file = makeRiff(makeChunk("data", pcm) + fmt);
      ForwardInBuf buf(file);
      std::istream stream(&buf);
      auto reader = bw64::readStream(stream);
      (void)reader;
    }));

    // BW64 without the mandatory ds64 chunk
    TASK_CHECK(throws<std::runtime_error>([&] {
      std::string file = "BW64";
      put32(file, 0xffffffffu);
      file += "WAVE" + fmt + makeChunk("data", pcm);
      ForwardInBuf buf(file);
      std::istream stream(&buf);
      auto reader = bw64::readStream(stream);
      (void)reader;
    }));

    // not a RIFF/BW64/RF64 file
    TASK_CHECK(throws<std::runtime_error>([&] {
      std::string file = "NOPE";
      put32(file, 4);
      file += "WAVE";
      ForwardInBuf buf(file);
      std::istream stream(&buf);
      auto reader = bw64::readStream(stream);
      (void)reader;
    }));
    return 0;
  }

  int reader_file_compare() {
    const char* paths[] = {"tests/test_data/rect_16bit.wav",
                           "tests/test_data/rect_24bit_rf64.wav",
                           "tests/test_data/noise_24bit_uneven_data_chunk_size.wav"};
    for (auto path : paths) {
      auto fileReader = bw64::readFile(path);
      std::string bytes = loadFile(path);
      TASK_CHECK(!bytes.empty());
      ForwardInBuf buf(bytes);
      std::istream stream(&buf);
      auto streamReader = bw64::readStream(stream);

      TASK_CHECK(streamReader->channels() == fileReader->channels());
      TASK_CHECK(streamReader->sampleRate() == fileReader->sampleRate());
      TASK_CHECK(streamReader->bitDepth() == fileReader->bitDepth());
      TASK_CHECK(streamReader->formatTag() == fileReader->formatTag());
      TASK_CHECK(streamReader->numberOfFrames() == fileReader->numberOfFrames());

      std::vector<float> fileSamples(7 * fileReader->channels());
      std::vector<float> streamSamples(7 * fileReader->channels());
      while (!fileReader->eof()) {
        auto fileFrames = fileReader->read(fileSamples.data(), 7);
        auto streamFrames = streamReader->read(streamSamples.data(), 7);
        TASK_CHECK(fileFrames == streamFrames);
        for (uint64_t i = 0; i < fileFrames * fileReader->channels(); i++)
          TASK_CHECK(fileSamples[i] == streamSamples[i]);
      }
      streamReader->finish();
      TASK_CHECK(streamReader->tell() == streamReader->numberOfFrames());
      TASK_CHECK(!buf.seekAttempted);
    }
    return 0;
  }

  // ------------------------------------------------------------------
  // writer_forward
  // ------------------------------------------------------------------

  int writer_riff_header() {
    ForwardOutBuf buf;
    std::ostream stream(&buf);
    float samples[6] = {0.5f, -0.5f, 0.25f, -0.25f, 0.0f, 1.0f};
    {
      auto writer = bw64::writeStream(stream, 3, 2, 48000, 16);
      TASK_CHECK(writer->framesWritten() == 0);
      TASK_CHECK(writer->write(samples, 2) == 2);
      TASK_CHECK(writer->write(samples + 4, 1) == 1);
      TASK_CHECK(writer->framesWritten() == 3);
      writer->close();
      writer->close();  // repeatable
    }
    const std::string& out = buf.data;
    // independent parse of the header
    TASK_CHECK(out.size() == 44 + 12);
    TASK_CHECK(get4(out, 0) == "RIFF");
    TASK_CHECK(get32(out, 4) == out.size() - 8);
    TASK_CHECK(get4(out, 8) == "WAVE");
    TASK_CHECK(get4(out, 12) == "fmt ");
    TASK_CHECK(get32(out, 16) == 16);
    TASK_CHECK(get16(out, 20) == 1);      // PCM
    TASK_CHECK(get16(out, 22) == 2);      // channels
    TASK_CHECK(get32(out, 24) == 48000);  // sampleRate
    TASK_CHECK(get32(out, 28) == 48000 * 4);
    TASK_CHECK(get16(out, 32) == 4);
    TASK_CHECK(get16(out, 34) == 16);
    TASK_CHECK(get4(out, 36) == "data");
    TASK_CHECK(get32(out, 40) == 12);
    // independent encoding of the expected PCM payload
    std::string expectedPcm;
    putSample16(expectedPcm, 16384);   // 0.5
    putSample16(expectedPcm, -16384);  // -0.5
    putSample16(expectedPcm, 8192);    // 0.25
    putSample16(expectedPcm, -8192);   // -0.25
    putSample16(expectedPcm, 0);
    putSample16(expectedPcm, 32767);  // 1.0 clipped
    TASK_CHECK(out.substr(44) == expectedPcm);
    TASK_CHECK(!buf.seekAttempted);

    // cross-check with the file reader
    const char* path = ".task-build/fwd_writer_riff.wav";
    {
      std::ofstream file(path, std::ios::binary);
      file << out;
    }
    auto reader = bw64::readFile(path);
    TASK_CHECK(reader->numberOfFrames() == 3);
    TASK_CHECK(reader->channels() == 2);
    float decoded[6];
    TASK_CHECK(reader->read(decoded, 3) == 3);
    TASK_CHECK(decoded[0] == 0.5f);
    TASK_CHECK(decoded[1] == -0.5f);
    TASK_CHECK(decoded[4] == 0.0f);
    reader->close();
    TASK_CHECK(std::remove(path) == 0);

    // cross-check with the stream reader
    ForwardInBuf inBuf(out);
    std::istream inStream(&inBuf);
    auto streamReader = bw64::readStream(inStream);
    TASK_CHECK(streamReader->numberOfFrames() == 3);
    TASK_CHECK(streamReader->read(decoded, 3) == 3);
    TASK_CHECK(decoded[2] == 0.25f);
    TASK_CHECK(decoded[3] == -0.25f);
    return 0;
  }

  int writer_empty() {
    ForwardOutBuf buf;
    std::ostream stream(&buf);
    {
      auto writer = bw64::writeStream(stream, 0, 1, 48000, 24);
      writer->close();
    }
    const std::string& out = buf.data;
    TASK_CHECK(out.size() == 44);
    TASK_CHECK(get4(out, 0) == "RIFF");
    TASK_CHECK(get32(out, 4) == 36);
    TASK_CHECK(get4(out, 36) == "data");
    TASK_CHECK(get32(out, 40) == 0);

    const char* path = ".task-build/fwd_writer_empty.wav";
    {
      std::ofstream file(path, std::ios::binary);
      file << out;
    }
    auto reader = bw64::readFile(path);
    TASK_CHECK(reader->numberOfFrames() == 0);
    TASK_CHECK(reader->eof());
    float tmp[1];
    TASK_CHECK(reader->read(tmp, 1) == 0);
    reader->close();
    TASK_CHECK(std::remove(path) == 0);
    return 0;
  }

  int writer_odd_data_pad() {
    ForwardOutBuf buf;
    std::ostream stream(&buf);
    float samples[3] = {0.5f, -0.5f, 0.25f};
    {
      auto writer = bw64::writeStream(stream, 3, 1, 48000, 24);
      TASK_CHECK(writer->write(samples, 3) == 3);
      writer->close();
    }
    const std::string& out = buf.data;
    // 24 bit mono: 9 data bytes plus one padding byte
    TASK_CHECK(out.size() == 44 + 9 + 1);
    TASK_CHECK(get32(out, 4) == out.size() - 8);
    TASK_CHECK(get4(out, 36) == "data");
    TASK_CHECK(get32(out, 40) == 9);
    TASK_CHECK(out[53] == '\0');
    std::string expectedPcm;
    putSample24(expectedPcm, 4194304);   // 0.5
    putSample24(expectedPcm, -4194304);  // -0.5
    putSample24(expectedPcm, 2097152);   // 0.25
    TASK_CHECK(out.substr(44, 9) == expectedPcm);

    ForwardInBuf inBuf(out);
    std::istream inStream(&inBuf);
    auto reader = bw64::readStream(inStream);
    TASK_CHECK(reader->numberOfFrames() == 3);
    float decoded[3];
    TASK_CHECK(reader->read(decoded, 3) == 3);
    TASK_CHECK(decoded[0] == 0.5f);
    TASK_CHECK(decoded[1] == -0.5f);
    TASK_CHECK(decoded[2] == 0.25f);
    return 0;
  }

  int writer_bw64_giant() {
    // declared data size above 4 GiB: only the header is produced and
    // checked, no bulk audio is written
    ForwardOutBuf buf;
    std::ostream stream(&buf);
    const uint64_t frames = (uint64_t(1) << 30) + 1;  // 32 bit mono
    const uint64_t dataSize = frames * 4;             // 2^32 + 4
    const uint64_t riffSize = 4 + (8 + 28) + (8 + 16) + (8 + dataSize);
    {
      auto writer = bw64::writeStream(stream, frames, 1, 48000, 32);
      const std::string& out = buf.data;
      TASK_CHECK(out.size() == 80);
      TASK_CHECK(get4(out, 0) == "BW64");
      TASK_CHECK(get32(out, 4) == 0xffffffffu);
      TASK_CHECK(get4(out, 8) == "WAVE");
      TASK_CHECK(get4(out, 12) == "ds64");
      TASK_CHECK(get32(out, 16) == 28);
      TASK_CHECK(get64(out, 20) == riffSize);
      TASK_CHECK(get64(out, 28) == dataSize);
      TASK_CHECK(get64(out, 36) == frames);  // sample count
      TASK_CHECK(get32(out, 44) == 0);       // table length
      TASK_CHECK(get4(out, 48) == "fmt ");
      TASK_CHECK(get32(out, 52) == 16);
      TASK_CHECK(get16(out, 56) == 1);
      TASK_CHECK(get16(out, 58) == 1);
      TASK_CHECK(get32(out, 60) == 48000);
      TASK_CHECK(get32(out, 64) == 48000 * 4);
      TASK_CHECK(get16(out, 68) == 4);
      TASK_CHECK(get16(out, 70) == 32);
      TASK_CHECK(get4(out, 72) == "data");
      TASK_CHECK(get32(out, 76) == 0xffffffffu);
      TASK_CHECK(!buf.seekAttempted);

      float sample[1] = {0.5f};
      TASK_CHECK(writer->write(sample, 1) == 1);
      TASK_CHECK(writer->framesWritten() == 1);
      // writes beyond the declared frame count are rejected before writing
      size_t sizeBefore = buf.data.size();
      TASK_CHECK(throws<std::runtime_error>(
          [&] { writer->write(sample, frames); }));
      TASK_CHECK(buf.data.size() == sizeBefore);
      // metadata is fixed with the header
      TASK_CHECK(throws<std::logic_error>([&] {
        writer->setAxmlChunk(std::make_shared<bw64::AxmlChunk>("x"));
      }));
      TASK_CHECK(throws<std::logic_error>([&] {
        writer->setChnaChunk(std::make_shared<bw64::ChnaChunk>());
      }));
      TASK_CHECK(throws<std::logic_error>([&] { writer->useRf64Id(true); }));
      // closing with missing frames fails; the destructor must not throw again
      TASK_CHECK(throws<std::runtime_error>([&] { writer->close(); }));
    }

    // the header can be parsed back by the stream reader
    ForwardInBuf inBuf(buf.data);
    std::istream inStream(&inBuf);
    auto reader = bw64::readStream(inStream);
    TASK_CHECK(reader->fileFormat() == bw64::utils::fourCC("BW64"));
    TASK_CHECK(reader->numberOfFrames() == frames);
    float decoded[1];
    TASK_CHECK(reader->read(decoded, 1) == 1);
    TASK_CHECK(decoded[0] == 0.5f);
    // the announced payload is not there: truncation must be reported
    TASK_CHECK(throws<std::runtime_error>([&] { reader->read(decoded, 1); }));
    return 0;
  }

  int writer_unrepresentable() {
    ForwardOutBuf buf;
    std::ostream stream(&buf);
    // frames * blockAlignment overflows uint64_t
    TASK_CHECK(throws<std::runtime_error>(
        [&] { bw64::writeStream(stream, UINT64_MAX, 2, 48000, 16); }));
    TASK_CHECK(buf.data.empty());
    // the riff size overflows uint64_t
    TASK_CHECK(throws<std::runtime_error>(
        [&] { bw64::writeStream(stream, UINT64_MAX / 4, 1, 48000, 32); }));
    TASK_CHECK(buf.data.empty());
    return 0;
  }

  int writer_failing_stream() {
    // header can not be written
    TASK_CHECK(throws<std::runtime_error>([&] {
      FailingOutBuf buf(5);
      std::ostream stream(&buf);
      bw64::writeStream(stream, 3, 2, 48000, 16);
    }));

    // audio data can not be written
    TASK_CHECK(throws<std::runtime_error>([&] {
      FailingOutBuf buf(44 + 4);
      std::ostream stream(&buf);
      auto writer = bw64::writeStream(stream, 2, 2, 48000, 16);
      float samples[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      writer->write(samples, 2);
    }));

    // final padding byte can not be written
    TASK_CHECK(throws<std::runtime_error>([&] {
      FailingOutBuf buf(44 + 3);
      std::ostream stream(&buf);
      auto writer = bw64::writeStream(stream, 1, 1, 48000, 24);
      float samples[1] = {0.5f};
      writer->write(samples, 1);
      writer->close();
    }));
    return 0;
  }

  int writer_metadata() {
    auto chna = std::make_shared<bw64::ChnaChunk>(
        std::vector<bw64::AudioId>{bw64::AudioId(1, "ATU_00000001",
                                                 "AT_00010001_01",
                                                 "AP_00010001")});
    std::string axml = "<adm>xy</adm>";  // 13 bytes, odd
    auto axmlChunk = std::make_shared<bw64::AxmlChunk>(axml);

    ForwardOutBuf buf;
    std::ostream stream(&buf);
    float samples[4] = {0.5f, -0.5f, 0.25f, -0.25f};
    {
      auto writer =
          bw64::writeStream(stream, 2, 2, 48000, 24, chna, axmlChunk);
      TASK_CHECK(writer->chnaChunk());
      TASK_CHECK(writer->axmlChunk());
      TASK_CHECK(writer->write(samples, 2) == 2);
      writer->close();
    }
    const std::string& out = buf.data;
    // layout: riff(12) fmt(24) chna(8+44) axml(8+13+1) data(8+12)
    TASK_CHECK(out.size() == 130);
    TASK_CHECK(get32(out, 4) == out.size() - 8);
    TASK_CHECK(get4(out, 12) == "fmt ");
    TASK_CHECK(get4(out, 36) == "chna");
    TASK_CHECK(get32(out, 40) == 44);
    TASK_CHECK(get16(out, 44) == 1);  // numTracks
    TASK_CHECK(get16(out, 46) == 1);  // numUids
    TASK_CHECK(get16(out, 48) == 1);  // trackIndex
    TASK_CHECK(out.substr(50, 12) == "ATU_00000001");
    TASK_CHECK(out.substr(62, 14) == "AT_00010001_01");
    TASK_CHECK(out.substr(76, 11) == "AP_00010001");
    TASK_CHECK(out[87] == ' ');
    TASK_CHECK(get4(out, 88) == "axml");
    TASK_CHECK(get32(out, 92) == 13);
    TASK_CHECK(out.substr(96, 13) == axml);
    TASK_CHECK(out[109] == '\0');  // padding of the odd-sized axml chunk
    TASK_CHECK(get4(out, 110) == "data");
    TASK_CHECK(get32(out, 114) == 12);

    // cross-check with the file reader
    const char* path = ".task-build/fwd_writer_meta.wav";
    {
      std::ofstream file(path, std::ios::binary);
      file << out;
    }
    auto reader = bw64::readFile(path);
    TASK_CHECK(reader->numberOfFrames() == 2);
    TASK_CHECK(reader->chnaChunk());
    TASK_CHECK(reader->chnaChunk()->audioIds()[0].uid() == "ATU_00000001");
    TASK_CHECK(reader->axmlChunk());
    TASK_CHECK(reader->axmlChunk()->data() == axml);
    reader->close();
    TASK_CHECK(std::remove(path) == 0);
    return 0;
  }

  int writer_file_compare() {
    auto chna = std::make_shared<bw64::ChnaChunk>(
        std::vector<bw64::AudioId>{bw64::AudioId(1, "ATU_00000001",
                                                 "AT_00010001_01",
                                                 "AP_00010001")});
    auto axml = std::make_shared<bw64::AxmlChunk>("<adm/>");
    const uint64_t frames = 32;
    std::vector<float> samples(frames * 2);
    for (uint64_t f = 0; f < frames; f++)
      for (uint16_t c = 0; c < 2; c++)
        samples[f * 2 + c] = expected16(pattern16(f, c));

    const char* filePath = ".task-build/fwd_cmp_file.wav";
    const char* streamPath = ".task-build/fwd_cmp_stream.wav";
    {
      auto writer = bw64::writeFile(filePath, 2, 48000, 16, chna, axml);
      writer->write(samples.data(), frames);
      writer->close();
    }
    {
      ForwardOutBuf buf;
      std::ostream stream(&buf);
      auto writer = bw64::writeStream(stream, frames, 2, 48000, 16, chna, axml);
      writer->write(samples.data(), frames);
      writer->close();
      std::ofstream file(streamPath, std::ios::binary);
      file << buf.data;
    }

    auto fileReader = bw64::readFile(filePath);
    auto streamReader = bw64::readFile(streamPath);
    TASK_CHECK(streamReader->channels() == fileReader->channels());
    TASK_CHECK(streamReader->sampleRate() == fileReader->sampleRate());
    TASK_CHECK(streamReader->bitDepth() == fileReader->bitDepth());
    TASK_CHECK(streamReader->formatTag() == fileReader->formatTag());
    TASK_CHECK(streamReader->numberOfFrames() == fileReader->numberOfFrames());
    TASK_CHECK(streamReader->axmlChunk());
    TASK_CHECK(streamReader->axmlChunk()->data() ==
               fileReader->axmlChunk()->data());
    TASK_CHECK(streamReader->chnaChunk());
    TASK_CHECK(streamReader->chnaChunk()->audioIds()[0].uid() ==
               fileReader->chnaChunk()->audioIds()[0].uid());
    std::vector<float> fileSamples(frames * 2);
    std::vector<float> streamSamples(frames * 2);
    TASK_CHECK(fileReader->read(fileSamples.data(), frames) == frames);
    TASK_CHECK(streamReader->read(streamSamples.data(), frames) == frames);
    for (uint64_t i = 0; i < frames * 2; i++)
      TASK_CHECK(fileSamples[i] == streamSamples[i]);
    fileReader->close();
    streamReader->close();
    TASK_CHECK(std::remove(filePath) == 0);
    TASK_CHECK(std::remove(streamPath) == 0);
    return 0;
  }

  // ------------------------------------------------------------------
  // pipe_roundtrip
  // ------------------------------------------------------------------

  int pipe_basic() {
    const uint64_t frames = 100;
    std::vector<float> samples(frames * 2);
    for (uint64_t f = 0; f < frames; f++)
      for (uint16_t c = 0; c < 2; c++)
        samples[f * 2 + c] = expected24(pattern24(f, c));

    ForwardOutBuf outBuf;
    std::ostream outStream(&outBuf);
    {
      auto writer = bw64::writeStream(outStream, frames, 2, 48000, 24);
      uint64_t written = 0;
      while (written < frames) {
        uint64_t block = std::min<uint64_t>(13, frames - written);
        TASK_CHECK(writer->write(samples.data() + written * 2, block) == block);
        written += block;
      }
      writer->close();
    }
    TASK_CHECK(!outBuf.seekAttempted);

    ForwardInBuf inBuf(outBuf.data);
    std::istream inStream(&inBuf);
    auto reader = bw64::readStream(inStream);
    TASK_CHECK(reader->numberOfFrames() == frames);
    TASK_CHECK(reader->channels() == 2);
    TASK_CHECK(reader->bitDepth() == 24);
    std::vector<float> decoded(frames * 2);
    uint64_t total = 0;
    while (!reader->eof()) {
      uint64_t framesRead = reader->read(decoded.data() + total * 2, 17);
      TASK_CHECK(framesRead <= 17);
      total += framesRead;
    }
    TASK_CHECK(total == frames);
    for (uint64_t i = 0; i < frames * 2; i++)
      TASK_CHECK(decoded[i] == samples[i]);
    TASK_CHECK(!inBuf.seekAttempted);
    return 0;
  }

  int pipe_metadata() {
    auto chna = std::make_shared<bw64::ChnaChunk>(
        std::vector<bw64::AudioId>{bw64::AudioId(1, "ATU_00000001",
                                                 "AT_00010001_01",
                                                 "AP_00010001")});
    std::string axml = "<a>bc</a>";  // 9 bytes, odd
    const uint64_t frames = 8;
    std::vector<float> samples(frames);
    for (uint64_t f = 0; f < frames; f++) samples[f] = expected16(pattern16(f, 0));

    ForwardOutBuf outBuf;
    std::ostream outStream(&outBuf);
    {
      auto writer = bw64::writeStream(outStream, frames, 1, 48000, 16, chna,
                                      std::make_shared<bw64::AxmlChunk>(axml));
      writer->write(samples.data(), frames);
      writer->close();
    }

    ForwardInBuf inBuf(outBuf.data);
    std::istream inStream(&inBuf);
    auto reader = bw64::readStream(inStream);
    // chunks before the data chunk are available right after construction
    TASK_CHECK(reader->chnaChunk());
    TASK_CHECK(reader->chnaChunk()->audioIds()[0].uid() == "ATU_00000001");
    TASK_CHECK(reader->axmlChunk());
    TASK_CHECK(reader->axmlChunk()->data() == axml);
    std::vector<float> decoded(frames);
    TASK_CHECK(reader->read(decoded.data(), frames) == frames);
    for (uint64_t f = 0; f < frames; f++)
      TASK_CHECK(decoded[f] == samples[f]);
    TASK_CHECK(reader->read(decoded.data(), 1) == 0);
    return 0;
  }

  int pipe_two_files() {
    auto chna = std::make_shared<bw64::ChnaChunk>(
        std::vector<bw64::AudioId>{bw64::AudioId(1, "ATU_00000001",
                                                 "AT_00010001_01",
                                                 "AP_00010001")});
    ForwardOutBuf outBuf;
    std::ostream outStream(&outBuf);
    std::vector<float> samples1(4), samples2(3 * 2);
    for (uint64_t f = 0; f < 4; f++) samples1[f] = expected16(pattern16(f, 0));
    for (uint64_t f = 0; f < 3; f++)
      for (uint16_t c = 0; c < 2; c++)
        samples2[f * 2 + c] = expected24(pattern24(f, c));
    {
      auto writer = bw64::writeStream(outStream, 4, 1, 48000, 16);
      writer->write(samples1.data(), 4);
      writer->close();
    }
    {
      auto writer = bw64::writeStream(outStream, 3, 2, 44100, 24, chna,
                                      std::make_shared<bw64::AxmlChunk>("<x/>"));
      writer->write(samples2.data(), 3);
      writer->close();
    }

    ForwardInBuf inBuf(outBuf.data);
    std::istream inStream(&inBuf);
    auto reader1 = bw64::readStream(inStream);
    TASK_CHECK(reader1->numberOfFrames() == 4);
    std::vector<float> decoded(6);
    TASK_CHECK(reader1->read(decoded.data(), 4) == 4);
    for (uint64_t f = 0; f < 4; f++)
      TASK_CHECK(decoded[f] == samples1[f]);
    reader1->finish();

    auto reader2 = bw64::readStream(inStream);
    TASK_CHECK(reader2->channels() == 2);
    TASK_CHECK(reader2->sampleRate() == 44100);
    TASK_CHECK(reader2->bitDepth() == 24);
    TASK_CHECK(reader2->chnaChunk());
    TASK_CHECK(reader2->axmlChunk());
    TASK_CHECK(reader2->axmlChunk()->data() == "<x/>");
    TASK_CHECK(reader2->read(decoded.data(), 3) == 3);
    for (uint64_t i = 0; i < 6; i++) TASK_CHECK(decoded[i] == samples2[i]);
    reader2->finish();
    TASK_CHECK(inStream.peek() == std::char_traits<char>::eof());
    TASK_CHECK(!inBuf.seekAttempted);
    return 0;
  }

  int pipe_32bit() {
    const uint64_t frames = 16;
    std::vector<float> samples(frames);
    for (uint64_t f = 0; f < frames; f++)
      samples[f] = (f % 8) / 8.0f - 0.5f;

    ForwardOutBuf outBuf;
    std::ostream outStream(&outBuf);
    {
      auto writer = bw64::writeStream(outStream, frames, 1, 96000, 32);
      writer->write(samples.data(), frames);
      writer->close();
    }
    TASK_CHECK(get4(outBuf.data, 0) == "RIFF");
    TASK_CHECK(get32(outBuf.data, 40) == frames * 4);

    ForwardInBuf inBuf(outBuf.data);
    std::istream inStream(&inBuf);
    auto reader = bw64::readStream(inStream);
    TASK_CHECK(reader->bitDepth() == 32);
    TASK_CHECK(reader->sampleRate() == 96000);
    std::vector<float> decoded(frames);
    TASK_CHECK(reader->read(decoded.data(), frames) == frames);
    for (uint64_t f = 0; f < frames; f++)
      TASK_CHECK(decoded[f] == samples[f]);
    return 0;
  }

}  // namespace

const task_case task_cases[] = {
    {"reader_forward", "basic_riff", reader_basic_riff},
    {"reader_forward", "bw64", reader_bw64},
    {"reader_forward", "rf64", reader_rf64},
    {"reader_forward", "tail_metadata", reader_tail_metadata},
    {"reader_forward", "two_files", reader_two_files},
    {"reader_forward", "partial_finish", reader_partial_finish},
    {"reader_forward", "partial_close", reader_partial_close},
    {"reader_forward", "seek_throws", reader_seek_throws},
    {"reader_forward", "truncated", reader_truncated},
    {"reader_forward", "fmt_after_data", reader_fmt_after_data},
    {"reader_forward", "file_compare", reader_file_compare},
    {"writer_forward", "riff_header", writer_riff_header},
    {"writer_forward", "empty", writer_empty},
    {"writer_forward", "odd_data_pad", writer_odd_data_pad},
    {"writer_forward", "bw64_giant", writer_bw64_giant},
    {"writer_forward", "unrepresentable", writer_unrepresentable},
    {"writer_forward", "failing_stream", writer_failing_stream},
    {"writer_forward", "metadata", writer_metadata},
    {"writer_forward", "file_compare", writer_file_compare},
    {"pipe_roundtrip", "basic", pipe_basic},
    {"pipe_roundtrip", "metadata", pipe_metadata},
    {"pipe_roundtrip", "two_files", pipe_two_files},
    {"pipe_roundtrip", "pcm32", pipe_32bit},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
