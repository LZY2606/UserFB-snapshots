// Copyright (c) 2026 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
//
// Tests for the optional Cluster CRC-32 feature. The checks use only the C++
// standard library plus libwebm's public headers. CRC math and EBML framing
// are re-implemented independently here so the tests do not trust the
// production code to validate itself.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "common/cluster_crc.h"
#include "common/webmids.h"
#include "mkvmuxer/mkvmuxer.h"
#include "mkvmuxer/mkvwriter.h"
#include "mkvparser/mkvparser.h"
#include "mkvparser/mkvreader.h"
#include "task_support/check.h"

namespace {

typedef unsigned char byte;
typedef std::vector<byte> Bytes;

const uint64_t kNsPerMs = 1000000ULL;

// ---------------------------------------------------------------------------
// Independent CRC-32 implementation (reflected IEEE CRC-32). This deliberately
// uses a different expression of the mask than common/cluster_crc.cc.
// ---------------------------------------------------------------------------
uint32_t IndependentCRC(const Bytes& data, size_t begin, size_t end) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = begin; i < end; ++i) {
    crc ^= static_cast<uint32_t>(data[i]);
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t low_bit_mask = 0U - (crc & 1U);
      crc = (crc >> 1) ^ (0xEDB88320U & low_bit_mask);
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

uint32_t IndependentCRC(const Bytes& data) {
  return IndependentCRC(data, 0, data.size());
}

// ---------------------------------------------------------------------------
// Independent EBML delimiting helpers.
// ---------------------------------------------------------------------------
int EbmlIdLength(byte first) {
  int len = 1;
  byte marker = 0x80;
  while ((first & marker) == 0 && len < 4) {
    ++len;
    marker >>= 1;
  }
  return len;
}

int EbmlSizeLength(byte first) {
  int len = 1;
  byte marker = 0x80;
  while ((first & marker) == 0 && len < 8) {
    ++len;
    marker >>= 1;
  }
  return len;
}

uint64_t EbmlDecodeId(const Bytes& b, size_t pos, int len) {
  uint64_t id = 0;
  for (int i = 0; i < len; ++i)
    id = (id << 8) | b[pos + i];
  return id;
}

// Decodes an EBML size field, keeping the length marker bits cleared.
int64_t EbmlDecodeSize(const Bytes& b, size_t pos, int len) {
  uint64_t size = b[pos] & (0xFF >> len);
  for (int i = 1; i < len; ++i)
    size = (size << 8) | b[pos + i];
  return static_cast<int64_t>(size);
}

Bytes EbmlEncodeId(uint64_t id) {
  Bytes out;
  for (int shift = 24; shift >= 0; shift -= 8) {
    const byte v = static_cast<byte>(id >> shift);
    if (v != 0 || !out.empty())
      out.push_back(v);
  }
  return out;
}

Bytes EbmlEncodeSize(uint64_t size) {
  Bytes raw;
  uint64_t v = size;
  do {
    raw.push_back(static_cast<byte>(v & 0xFF));
    v >>= 8;
  } while (v != 0);
  std::reverse(raw.begin(), raw.end());
  raw[0] |= static_cast<byte>(0x80 >> (raw.size() - 1));
  return raw;
}

struct EbmlElement {
  uint64_t id;
  int64_t size;
  size_t header_size;
  size_t payload_pos;
};

bool ParseOneElement(const Bytes& b, size_t pos, EbmlElement* out) {
  if (pos >= b.size())
    return false;
  const int id_len = EbmlIdLength(b[pos]);
  if (pos + id_len >= b.size())
    return false;
  const uint64_t id = EbmlDecodeId(b, pos, id_len);
  const size_t size_pos = pos + id_len;
  const int size_len = EbmlSizeLength(b[size_pos]);
  if (size_pos + size_len > b.size())
    return false;
  const int64_t size = EbmlDecodeSize(b, size_pos, size_len);
  out->id = id;
  out->size = size;
  out->header_size = id_len + size_len;
  out->payload_pos = size_pos + size_len;
  return true;
}

// Locates Cluster elements by walking the Segment's level-1 siblings instead
// of searching bytes, so the same ID appearing inside a SeekHead entry is not
// mistaken for a cluster.
std::vector<size_t> FindClusterStarts(const Bytes& b) {
  std::vector<size_t> starts;

  // Walk level 0 until the Segment element.
  size_t p = 0;
  EbmlElement element;
  while (p < b.size()) {
    if (!ParseOneElement(b, p, &element))
      return starts;
    if (element.id == libwebm::kMkvSegment)
      break;
    p = element.payload_pos + static_cast<size_t>(element.size);
  }
  if (p >= b.size() || element.id != libwebm::kMkvSegment)
    return starts;

  // Walk the Segment's level-1 children by their declared ranges.
  size_t q = element.payload_pos;
  while (q < b.size()) {
    if (!ParseOneElement(b, q, &element))
      break;

    const int size_len = EbmlSizeLength(b[q + EbmlIdLength(b[q])]);
    const int64_t unknown_size = (1LL << (7 * size_len)) - 1;

    if (element.id == libwebm::kMkvCluster)
      starts.push_back(q);

    if (element.size == unknown_size)
      break;  // no further sibling can be located deterministically

    const size_t next =
        element.payload_pos + static_cast<size_t>(element.size);
    if (next <= q || next > b.size())
      break;
    q = next;
  }
  return starts;
}

Bytes ReadFile(const std::string& path) {
  std::ifstream in(path.c_str(), std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(in)),
               std::istreambuf_iterator<char>());
}

void WriteFile(const std::string& path, const Bytes& data) {
  std::ofstream out(path.c_str(), std::ios::binary);
  out.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
}

std::string TempPath(const char* name) {
  const char* dir = std::getenv("TMPDIR");
  static int counter = 0;
  std::string base = (dir != NULL && dir[0] != '\0') ? dir : ".";
  base += "/libwebm_crc_";
  base += name;
  base += "_";
  base += std::to_string(static_cast<long long>(counter++));
  base += ".webm";
  return base;
}

// ---------------------------------------------------------------------------
// In-memory muxer writer with configurable behavior.
// ---------------------------------------------------------------------------
class VectorWriter : public mkvmuxer::IMkvWriter {
 public:
  explicit VectorWriter(bool seekable) : seekable_(seekable) {}

  int32_t Write(const void* buf, uint32_t len) override {
    const byte* p = static_cast<const byte*>(buf);
    data_.insert(data_.end(), p, p + len);
    return 0;
  }
  int64_t Position() const override {
    return static_cast<int64_t>(data_.size());
  }
  int32_t Position(int64_t position) override {
    if (position < 0 || static_cast<uint64_t>(position) > data_.size())
      return -1;
    data_.resize(static_cast<size_t>(position));
    return 0;
  }
  bool Seekable() const override { return seekable_; }
  void ElementStartNotify(uint64_t, int64_t) override {}

  const Bytes& data() const { return data_; }

 private:
  bool seekable_;
  Bytes data_;
};

// Writer that can be made to fail writes or seeks, used to drive the segment
// into its failed state.
class FailingWriter : public mkvmuxer::IMkvWriter {
 public:
  FailingWriter() {}

  int32_t Write(const void* buf, uint32_t len) override {
    ++write_calls_;
    if (fail_writes_)
      return -1;
    const byte* p = static_cast<const byte*>(buf);
    data_.insert(data_.end(), p, p + len);
    return 0;
  }
  int64_t Position() const override {
    return static_cast<int64_t>(data_.size());
  }
  int32_t Position(int64_t) override {
    ++seek_calls_;
    if (fail_seeks_)
      return -1;
    return 0;
  }
  bool Seekable() const override { return true; }
  void ElementStartNotify(uint64_t, int64_t) override {}

  void FailWrites() { fail_writes_ = true; }
  void FailSeeks() { fail_seeks_ = true; }
  int write_calls() const { return write_calls_; }
  int seek_calls() const { return seek_calls_; }

 private:
  bool fail_writes_ = false;
  bool fail_seeks_ = false;
  int write_calls_ = 0;
  int seek_calls_ = 0;
  Bytes data_;
};

// ---------------------------------------------------------------------------
// Reader over an in-memory prefix of a WebM byte vector.
// ---------------------------------------------------------------------------
class LimitedReader : public mkvparser::IMkvReader {
 public:
  LimitedReader(const Bytes* data, size_t limit)
      : data_(data), limit_(limit), fail_after_(-1) {}

  void set_limit(size_t limit) { limit_ = limit; }

  // When non-negative, Read() calls whose offset reaches |fail_after| return
  // this exact negative code instead of data.
  void set_read_failure(long long fail_after, int code) {
    fail_after_ = fail_after;
    failure_code_ = code;
  }

  int Read(long long pos, long len, unsigned char* buf) override {
    if (pos < 0 || len < 0)
      return -1;
    if (fail_after_ >= 0 && pos >= fail_after_)
      return failure_code_;
    if (static_cast<size_t>(pos) + static_cast<size_t>(len) > limit_)
      return 1;  // bytes not available yet
    std::memcpy(buf, data_->data() + pos, static_cast<size_t>(len));
    return 0;
  }

  int Length(long long* total, long long* available) override {
    if (total)
      *total = static_cast<long long>(data_->size());
    if (available)
      *available = static_cast<long long>(limit_);
    return 0;
  }

 private:
  const Bytes* const data_;
  size_t limit_;
  long long fail_after_;
  int failure_code_ = 0;
};

// ---------------------------------------------------------------------------
// Recognizable synthetic frame payloads.
// ---------------------------------------------------------------------------
Bytes MakeFrame(byte seed, size_t len) {
  Bytes frame(len);
  for (size_t i = 0; i < len; ++i)
    frame[i] = static_cast<byte>(seed + static_cast<byte>(i * 7));
  return frame;
}

// ---------------------------------------------------------------------------
// Muxing helpers. Each returns true on success and fills |out| with the file
// bytes.
// ---------------------------------------------------------------------------
bool MuxBasic(const std::string& path, bool crc, uint64_t timecode_scale,
              Bytes* out) {
  mkvmuxer::MkvWriter writer;
  if (!writer.Open(path.c_str()))
    return false;
  mkvmuxer::Segment segment;
  if (!segment.Init(&writer))
    return false;
  if (crc && !segment.SetClusterCRC(true))
    return false;
  segment.GetSegmentInfo()->set_timecode_scale(timecode_scale);

  const uint64_t video = segment.AddVideoTrack(16, 16, 1);
  const uint64_t audio = segment.AddAudioTrack(48000, 2, 2);
  if (video == 0 || audio == 0)
    return false;
  segment.GetTrackByNumber(video)->set_codec_id("V_VP8");
  segment.GetTrackByNumber(audio)->set_codec_id("A_OPUS");

  const Bytes v0 = MakeFrame(0x10, 8);
  const Bytes a0 = MakeFrame(0x40, 6);
  const Bytes v1 = MakeFrame(0x70, 8);
  const Bytes a1 = MakeFrame(0xA0, 6);
  const uint64_t scale = timecode_scale;

  if (!segment.AddFrame(v0.data(), v0.size(), video, 0, true))
    return false;
  if (!segment.AddFrame(a0.data(), a0.size(), audio, scale, true))
    return false;
  segment.ForceNewClusterOnNextFrame();
  if (!segment.AddFrame(v1.data(), v1.size(), video, 10 * scale, true))
    return false;
  if (!segment.AddFrame(a1.data(), a1.size(), audio, 11 * scale, true))
    return false;
  if (!segment.Finalize())
    return false;
  writer.Close();
  *out = ReadFile(path);
  return !out->empty();
}

// Includes a frame carrying a duration, which the muxer must serialize inside
// a BlockGroup rather than a SimpleBlock.
bool MuxBlockGroups(const std::string& path, bool crc, Bytes* out) {
  mkvmuxer::MkvWriter writer;
  if (!writer.Open(path.c_str()))
    return false;
  mkvmuxer::Segment segment;
  if (!segment.Init(&writer))
    return false;
  if (crc && !segment.SetClusterCRC(true))
    return false;

  const uint64_t video = segment.AddVideoTrack(16, 16, 1);
  segment.GetTrackByNumber(video)->set_codec_id("V_VP8");

  const Bytes key = MakeFrame(0x20, 12);
  const Bytes inter = MakeFrame(0x30, 12);
  if (!segment.AddFrame(key.data(), key.size(), video, 0, true))
    return false;

  // A duration forces the Block + BlockDuration / BlockGroup encoding.
  mkvmuxer::Frame frame;
  if (!frame.Init(inter.data(), inter.size()))
    return false;
  frame.set_track_number(video);
  frame.set_timestamp(kNsPerMs);
  frame.set_is_key(false);
  frame.set_duration(kNsPerMs);
  if (!segment.AddGenericFrame(&frame))
    return false;
  if (!segment.Finalize())
    return false;
  writer.Close();
  *out = ReadFile(path);
  return !out->empty();
}

// Video keyframes plus interleaved audio with accurate cluster duration,
// exercising delayed audio frame draining and the Finalize() tail path. One
// frame is deliberately large so CRC verification crosses the 4096 byte read
// window.
bool MuxDelayed(const std::string& path, bool crc, int* video_frames,
                int* audio_frames, Bytes* out) {
  mkvmuxer::MkvWriter writer;
  if (!writer.Open(path.c_str()))
    return false;
  mkvmuxer::Segment segment;
  if (!segment.Init(&writer))
    return false;
  if (crc && !segment.SetClusterCRC(true))
    return false;
  segment.AccurateClusterDuration(true);
  segment.set_max_cluster_duration(5 * kNsPerMs);

  const uint64_t video = segment.AddVideoTrack(16, 16, 1);
  const uint64_t audio = segment.AddAudioTrack(48000, 2, 2);
  segment.GetTrackByNumber(video)->set_codec_id("V_VP8");
  segment.GetTrackByNumber(audio)->set_codec_id("A_OPUS");

  int vcount = 0;
  int acount = 0;
  for (int i = 0; i < 6; ++i) {
    const size_t payload_len = (i == 3) ? 5000 : 16;
    const Bytes vf = MakeFrame(static_cast<byte>(0x50 + i), payload_len);
    if (!segment.AddFrame(vf.data(), vf.size(), video, i * 10 * kNsPerMs,
                          true))
      return false;
    ++vcount;

    for (int j = 0; j < 2; ++j) {
      const Bytes af = MakeFrame(static_cast<byte>(0x80 + i * 2 + j), 10);
      const uint64_t ts = (i * 10 + 2 + j * 3) * kNsPerMs;
      if (!segment.AddFrame(af.data(), af.size(), audio, ts, true))
        return false;
      ++acount;
    }
  }
  if (!segment.Finalize())
    return false;
  writer.Close();
  *video_frames = vcount;
  *audio_frames = acount;
  *out = ReadFile(path);
  return !out->empty();
}

// ---------------------------------------------------------------------------
// Parser helpers.
// ---------------------------------------------------------------------------
struct ParsedSegment {
  mkvparser::Segment* segment = NULL;
  mkvparser::MkvReader* reader = NULL;
};

bool OpenFileSegment(const std::string& path, ParsedSegment* parsed) {
  mkvparser::MkvReader* reader = new mkvparser::MkvReader();
  if (reader->Open(path.c_str()) != 0) {
    delete reader;
    return false;
  }
  long long pos = 0;
  mkvparser::EBMLHeader header;
  if (header.Parse(reader, pos) < 0) {
    delete reader;
    return false;
  }
  mkvparser::Segment* segment = NULL;
  if (mkvparser::Segment::CreateInstance(reader, pos, segment) != 0) {
    delete reader;
    return false;
  }
  if (segment->Load() < 0) {
    delete segment;
    delete reader;
    return false;
  }
  parsed->segment = segment;
  parsed->reader = reader;
  return true;
}

void CloseParsedSegment(ParsedSegment* parsed) {
  delete parsed->segment;
  delete parsed->reader;
  parsed->segment = NULL;
  parsed->reader = NULL;
}

// Preloads the first cluster of |data| through a reader limited to |limit|
// bytes. The caller takes ownership of |*reader_holder| and |*segment_holder|.
bool PreloadFirstCluster(const Bytes& data, size_t limit,
                         LimitedReader** reader_holder,
                         mkvparser::Segment** segment_holder,
                         const mkvparser::Cluster** cluster_holder) {
  LimitedReader* reader = new LimitedReader(&data, limit);
  long long pos = 0;
  mkvparser::EBMLHeader header;
  if (header.Parse(reader, pos) < 0) {
    delete reader;
    return false;
  }
  mkvparser::Segment* segment = NULL;
  if (mkvparser::Segment::CreateInstance(reader, pos, segment) != 0) {
    delete reader;
    return false;
  }
  if (segment->ParseHeaders() < 0) {
    delete segment;
    delete reader;
    return false;
  }
  const std::vector<size_t> starts = FindClusterStarts(data);
  if (starts.empty()) {
    delete segment;
    delete reader;
    return false;
  }
  const long long offset =
      static_cast<long long>(starts[0]) - segment->m_start;
  const mkvparser::Cluster* cluster = segment->FindOrPreloadCluster(offset);
  if (cluster == NULL) {
    delete segment;
    delete reader;
    return false;
  }
  *reader_holder = reader;
  *segment_holder = segment;
  *cluster_holder = cluster;
  return true;
}

// Verifies every cluster reachable from a fully loaded file segment.
long VerifyAllClusters(const ParsedSegment& parsed) {
  for (const mkvparser::Cluster* cluster = parsed.segment->GetFirst();
       cluster != NULL && !cluster->EOS();
       cluster = parsed.segment->GetNext(cluster)) {
    long long pos = 0;
    long len = 0;
    const long status = cluster->VerifyCRC(pos, len);
    if (status != 0)
      return status;
  }
  return 0;
}

int CountBlockEntries(const ParsedSegment& parsed) {
  int count = 0;
  for (const mkvparser::Cluster* cluster = parsed.segment->GetFirst();
       cluster != NULL && !cluster->EOS();
       cluster = parsed.segment->GetNext(cluster)) {
    const mkvparser::BlockEntry* entry = NULL;
    if (cluster->GetFirst(entry) < 0)
      return -1;
    while (entry != NULL && !entry->EOS()) {
      ++count;
      const mkvparser::BlockEntry* next = NULL;
      if (cluster->GetNext(entry, next) < 0)
        return -1;
      entry = next;
    }
  }
  return count;
}

// Independent byte-level audit of a muxed, CRC-enabled file: every cluster's
// declared size matches its on-disk span, the CRC is its first child, its
// value matches an independent computation, and coverage excludes only the
// six CRC bytes.
bool IndependentClusterAudit(const Bytes& data, int expected_clusters) {
  const std::vector<size_t> starts = FindClusterStarts(data);
  if (static_cast<int>(starts.size()) != expected_clusters)
    return false;

  for (size_t ci = 0; ci < starts.size(); ++ci) {
    EbmlElement cluster_header;
    if (!ParseOneElement(data, starts[ci], &cluster_header) ||
        cluster_header.id != libwebm::kMkvCluster)
      return false;

    const size_t payload_pos = cluster_header.payload_pos;
    const size_t payload_end =
        payload_pos + static_cast<size_t>(cluster_header.size);
    if (payload_end > data.size())
      return false;
    if (ci + 1 < starts.size() && payload_end > starts[ci + 1])
      return false;

    EbmlElement first_child;
    if (!ParseOneElement(data, payload_pos, &first_child) ||
        first_child.id != libwebm::kMkvCRC32 || first_child.size != 4)
      return false;

    const size_t crc_end =
        first_child.payload_pos + static_cast<size_t>(first_child.size);
    uint32_t stored = 0;
    for (int i = 0; i < 4; ++i)
      stored |= static_cast<uint32_t>(data[first_child.payload_pos + i])
                << (8 * i);

    // The muxer places the CRC first, so coverage is every payload byte after
    // the six byte CRC element.
    uint32_t crc = 0xFFFFFFFFU;
    libwebm::ClusterCRC32Update(crc, data.data() + crc_end, payload_end - crc_end);
    const uint32_t computed = libwebm::ClusterCRC32Finalize(crc);
    if (stored != computed)
      return false;

    // And the fully independent formulation must agree too.
    const Bytes covered(data.begin() + crc_end, data.begin() + payload_end);
    if (IndependentCRC(covered) != stored)
      return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Hand-built external clusters, so the parser is exercised against output it
// did not write itself.
// ---------------------------------------------------------------------------
struct ExternalChild {
  uint64_t id;
  Bytes payload;
  bool crc_child = false;  // payload already holds a little endian CRC value
};

Bytes BuildExternalCluster(const std::vector<ExternalChild>& children,
                           bool unknown_size) {
  Bytes payload;
  for (size_t i = 0; i < children.size(); ++i) {
    const Bytes id = EbmlEncodeId(children[i].id);
    const Bytes size = EbmlEncodeSize(children[i].payload.size());
    payload.insert(payload.end(), id.begin(), id.end());
    payload.insert(payload.end(), size.begin(), size.end());
    payload.insert(payload.end(), children[i].payload.begin(),
                   children[i].payload.end());
  }

  Bytes out;
  const Bytes cluster_id = EbmlEncodeId(libwebm::kMkvCluster);
  out.insert(out.end(), cluster_id.begin(), cluster_id.end());
  if (unknown_size) {
    out.push_back(0xFF);  // one byte unknown size marker
  } else {
    const Bytes size = EbmlEncodeSize(payload.size());
    out.insert(out.end(), size.begin(), size.end());
  }
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

// Builds a minimal valid WebM prefix (everything up to the first cluster) by
// muxing a tiny file, then appends |replacement| as the cluster region.
Bytes WrapExternalCluster(const Bytes& replacement) {
  const std::string path = TempPath("external_base");
  Bytes base;
  if (!MuxBasic(path, false, 1000000ULL, &base))
    return Bytes();
  std::remove(path.c_str());

  const std::vector<size_t> starts = FindClusterStarts(base);
  if (starts.empty())
    return Bytes();

  Bytes out(base.begin(), base.begin() + starts[0]);
  out.insert(out.end(), replacement.begin(), replacement.end());
  return out;
}

Bytes MakeTimecodeChild(uint64_t timecode) {
  ExternalChild child;
  child.id = libwebm::kMkvTimecode;
  uint64_t v = timecode;
  do {
    child.payload.push_back(static_cast<byte>(v & 0xFF));
    v >>= 8;
  } while (v != 0);
  std::reverse(child.payload.begin(), child.payload.end());
  return child.payload;
}

Bytes MakeSimpleBlockChild(byte track, int16_t relative_timecode,
                           const Bytes& frame, bool key) {
  ExternalChild child;
  child.id = libwebm::kMkvSimpleBlock;
  child.payload.push_back(track);
  child.payload.push_back(
      static_cast<byte>(static_cast<uint16_t>(relative_timecode) >> 8));
  child.payload.push_back(static_cast<byte>(relative_timecode));
  child.payload.push_back(key ? 0x80 : 0x00);
  child.payload.insert(child.payload.end(), frame.begin(), frame.end());
  return child.payload;
}

// Builds an external cluster whose CRC sits between the Timecode and the
// SimpleBlock, i.e. not in the muxer's first-child position.
Bytes BuildMiddleCrcCluster(bool unknown_size) {
  const Bytes frame = MakeFrame(0x66, 33);
  const Bytes timecode_payload = MakeTimecodeChild(7);
  const Bytes block_payload = MakeSimpleBlockChild(1, 0, frame, true);

  Bytes covered;
  Bytes timecode_elem = EbmlEncodeId(libwebm::kMkvTimecode);
  Bytes timecode_size = EbmlEncodeSize(timecode_payload.size());
  timecode_elem.insert(timecode_elem.end(), timecode_size.begin(),
                       timecode_size.end());
  timecode_elem.insert(timecode_elem.end(), timecode_payload.begin(),
                       timecode_payload.end());
  Bytes block_elem = EbmlEncodeId(libwebm::kMkvSimpleBlock);
  Bytes block_size = EbmlEncodeSize(block_payload.size());
  block_elem.insert(block_elem.end(), block_size.begin(), block_size.end());
  block_elem.insert(block_elem.end(), block_payload.begin(),
                    block_payload.end());
  covered.insert(covered.end(), timecode_elem.begin(), timecode_elem.end());
  covered.insert(covered.end(), block_elem.begin(), block_elem.end());

  const uint32_t crc = IndependentCRC(covered);

  std::vector<ExternalChild> children;
  ExternalChild tc;
  tc.id = libwebm::kMkvTimecode;
  tc.payload = timecode_payload;
  children.push_back(tc);

  ExternalChild crc_child;
  crc_child.id = libwebm::kMkvCRC32;
  crc_child.crc_child = true;
  crc_child.payload.resize(4);
  for (int i = 0; i < 4; ++i)
    crc_child.payload[i] = static_cast<byte>(crc >> (8 * i));
  children.push_back(crc_child);

  ExternalChild block;
  block.id = libwebm::kMkvSimpleBlock;
  block.payload = block_payload;
  children.push_back(block);

  return BuildExternalCluster(children, unknown_size);
}

// Runs VerifyCRC against |data| through a fully available reader.
long VerifyBytesOnce(const Bytes& data) {
  LimitedReader* reader = NULL;
  mkvparser::Segment* segment = NULL;
  const mkvparser::Cluster* cluster = NULL;
  if (!PreloadFirstCluster(data, data.size(), &reader, &segment, &cluster))
    return mkvparser::E_PARSE_FAILED;
  long long pos = 0;
  long len = 0;
  const long status = cluster->VerifyCRC(pos, len);
  delete segment;
  delete reader;
  return status;
}

}  // namespace

// ===========================================================================
// Coverage group
// ===========================================================================

static int CoverageMultiCluster() {
  const std::string path = TempPath("coverage_multi");
  Bytes data;
  TASK_CHECK(MuxBasic(path, true, 1000000ULL, &data));
  std::remove(path.c_str());

  // Parse the produced bytes through a stable file path.
  WriteFile(path, data);
  ParsedSegment parsed;
  TASK_CHECK(OpenFileSegment(path, &parsed));
  TASK_CHECK(VerifyAllClusters(parsed) == 0);

  int clusters = 0;
  for (const mkvparser::Cluster* c = parsed.segment->GetFirst();
       c != NULL && !c->EOS(); c = parsed.segment->GetNext(c))
    ++clusters;
  TASK_CHECK(clusters == 2);
  TASK_CHECK(CountBlockEntries(parsed) == 4);
  CloseParsedSegment(&parsed);

  TASK_CHECK(IndependentClusterAudit(data, 2));
  std::remove(path.c_str());
  return 0;
}

static int CoverageNonDefaultScale() {
  const std::string path = TempPath("coverage_scale");
  Bytes data;
  const uint64_t scale = 123456ULL;
  TASK_CHECK(MuxBasic(path, true, scale, &data));
  std::remove(path.c_str());
  WriteFile(path, data);

  ParsedSegment parsed;
  TASK_CHECK(OpenFileSegment(path, &parsed));
  TASK_CHECK(parsed.segment->GetInfo()->GetTimeCodeScale() ==
             static_cast<long long>(scale));
  TASK_CHECK(VerifyAllClusters(parsed) == 0);
  CloseParsedSegment(&parsed);
  TASK_CHECK(IndependentClusterAudit(data, 2));
  std::remove(path.c_str());
  return 0;
}

static int CoverageBlockGroup() {
  const std::string path = TempPath("coverage_bg");
  Bytes data;
  TASK_CHECK(MuxBlockGroups(path, true, &data));
  std::remove(path.c_str());
  WriteFile(path, data);

  ParsedSegment parsed;
  TASK_CHECK(OpenFileSegment(path, &parsed));
  TASK_CHECK(VerifyAllClusters(parsed) == 0);

  const mkvparser::Cluster* cluster = parsed.segment->GetFirst();
  TASK_CHECK(cluster != NULL && !cluster->EOS());
  const mkvparser::BlockEntry* entry = NULL;
  TASK_CHECK(cluster->GetFirst(entry) == 0 && entry != NULL);
  bool saw_group = false;
  bool saw_simple = false;
  while (entry != NULL && !entry->EOS()) {
    if (entry->GetKind() == mkvparser::BlockEntry::kBlockGroup)
      saw_group = true;
    if (entry->GetKind() == mkvparser::BlockEntry::kBlockSimple)
      saw_simple = true;
    const mkvparser::BlockEntry* next = NULL;
    TASK_CHECK(cluster->GetNext(entry, next) == 0);
    entry = next;
  }
  TASK_CHECK(saw_group && saw_simple);
  CloseParsedSegment(&parsed);
  TASK_CHECK(IndependentClusterAudit(data, 1));
  std::remove(path.c_str());
  return 0;
}

static int CoverageCuePositions() {
  const std::string path = TempPath("coverage_cues");
  Bytes data;
  TASK_CHECK(MuxBasic(path, true, 1000000ULL, &data));

  ParsedSegment parsed;
  TASK_CHECK(OpenFileSegment(path, &parsed));
  TASK_CHECK(VerifyAllClusters(parsed) == 0);

  const mkvparser::Cues* cues = parsed.segment->GetCues();
  TASK_CHECK(cues != NULL);
  TASK_CHECK(cues->LoadCuePoint());
  const mkvparser::CuePoint* cue = cues->GetFirst();
  TASK_CHECK(cue != NULL);
  const mkvparser::Track* track =
      parsed.segment->GetTracks()->GetTrackByNumber(1);
  TASK_CHECK(track != NULL);
  const mkvparser::CuePoint::TrackPosition* tp = cue->Find(track);
  TASK_CHECK(tp != NULL);

  // CueClusterPosition must point at the Cluster element itself, i.e. real
  // payload rather than the CRC element.
  const long long cluster_abs =
      parsed.segment->m_start + tp->m_pos;
  TASK_CHECK(cluster_abs < static_cast<long long>(data.size()) - 4);
  TASK_CHECK(data[static_cast<size_t>(cluster_abs)] == 0x1F &&
             data[static_cast<size_t>(cluster_abs) + 1] == 0x43);
  TASK_CHECK(tp->m_block == 1);
  CloseParsedSegment(&parsed);
  std::remove(path.c_str());
  return 0;
}

static int CoverageExternalMiddleCrc() {
  const Bytes cluster = BuildMiddleCrcCluster(false);
  TASK_CHECK(!cluster.empty());
  const Bytes data = WrapExternalCluster(cluster);
  TASK_CHECK(!data.empty());

  TASK_CHECK(VerifyBytesOnce(data) == 0);

  // A second, independent verification run must produce the same answer.
  TASK_CHECK(VerifyBytesOnce(data) == 0);
  return 0;
}

static int CoverageMissingCrc() {
  const std::string path = TempPath("coverage_missing");
  Bytes data;
  TASK_CHECK(MuxBasic(path, false, 1000000ULL, &data));
  std::remove(path.c_str());
  TASK_CHECK(VerifyBytesOnce(data) == 1);

  // An external cluster without a CRC child reports the same "no CRC" result.
  const Bytes frame = MakeFrame(0x33, 12);
  std::vector<ExternalChild> children;
  ExternalChild tc;
  tc.id = libwebm::kMkvTimecode;
  tc.payload = MakeTimecodeChild(0);
  children.push_back(tc);
  ExternalChild block;
  block.id = libwebm::kMkvSimpleBlock;
  block.payload = MakeSimpleBlockChild(1, 0, frame, true);
  children.push_back(block);
  const Bytes external = WrapExternalCluster(
      BuildExternalCluster(children, false));
  TASK_CHECK(!external.empty());
  TASK_CHECK(VerifyBytesOnce(external) == 1);
  return 0;
}

static int CoverageDuplicateCrc() {
  const Bytes frame = MakeFrame(0x44, 12);
  const Bytes tc_payload = MakeTimecodeChild(0);
  const Bytes block_payload = MakeSimpleBlockChild(1, 0, frame, true);

  std::vector<ExternalChild> children;
  ExternalChild tc;
  tc.id = libwebm::kMkvTimecode;
  tc.payload = tc_payload;
  children.push_back(tc);
  for (int i = 0; i < 2; ++i) {
    ExternalChild crc_child;
    crc_child.id = libwebm::kMkvCRC32;
    crc_child.payload.assign(4, static_cast<byte>(0x11 + i));
    children.push_back(crc_child);
  }
  ExternalChild block;
  block.id = libwebm::kMkvSimpleBlock;
  block.payload = block_payload;
  children.push_back(block);

  const Bytes data =
      WrapExternalCluster(BuildExternalCluster(children, false));
  TASK_CHECK(!data.empty());
  TASK_CHECK(VerifyBytesOnce(data) == mkvparser::E_FILE_FORMAT_INVALID);
  return 0;
}

static int CoverageBadCrcLength() {
  const Bytes frame = MakeFrame(0x55, 12);
  for (int bad_size = 0; bad_size < 2; ++bad_size) {
    std::vector<ExternalChild> children;
    ExternalChild tc;
    tc.id = libwebm::kMkvTimecode;
    tc.payload = MakeTimecodeChild(0);
    children.push_back(tc);

    ExternalChild crc_child;
    crc_child.id = libwebm::kMkvCRC32;
    crc_child.payload.assign(bad_size == 0 ? 3 : 5, 0xAB);
    children.push_back(crc_child);

    ExternalChild block;
    block.id = libwebm::kMkvSimpleBlock;
    block.payload = MakeSimpleBlockChild(1, 0, frame, true);
    children.push_back(block);

    const Bytes data =
        WrapExternalCluster(BuildExternalCluster(children, false));
    TASK_CHECK(!data.empty());
    TASK_CHECK(VerifyBytesOnce(data) == mkvparser::E_FILE_FORMAT_INVALID);
  }
  return 0;
}

static int CoverageFlipPayloadByte() {
  const std::string path = TempPath("coverage_flip_payload");
  Bytes data;
  TASK_CHECK(MuxBasic(path, true, 1000000ULL, &data));
  std::remove(path.c_str());
  TASK_CHECK(VerifyBytesOnce(data) == 0);

  // Flip a byte inside the second SimpleBlock payload, well past the CRC and
  // Timecode elements.
  const std::vector<size_t> starts = FindClusterStarts(data);
  TASK_CHECK(starts.size() == 2);
  EbmlElement cluster_header;
  TASK_CHECK(ParseOneElement(data, starts[0], &cluster_header));
  size_t cursor = cluster_header.payload_pos;
  EbmlElement child;
  TASK_CHECK(ParseOneElement(data, cursor, &child));
  TASK_CHECK(child.id == libwebm::kMkvCRC32);
  cursor = child.payload_pos + static_cast<size_t>(child.size);
  TASK_CHECK(ParseOneElement(data, cursor, &child));
  TASK_CHECK(child.id == libwebm::kMkvTimecode);
  cursor = child.payload_pos + static_cast<size_t>(child.size);
  TASK_CHECK(ParseOneElement(data, cursor, &child));
  TASK_CHECK(child.id == libwebm::kMkvSimpleBlock);
  const size_t flip = child.payload_pos + 4;  // first frame payload byte
  Bytes corrupted = data;
  corrupted[flip] ^= 0x01;
  TASK_CHECK(VerifyBytesOnce(corrupted) == mkvparser::E_FILE_FORMAT_INVALID);
  return 0;
}

static int CoverageFlipTimecodeByte() {
  const std::string path = TempPath("coverage_flip_tc");
  Bytes data;
  TASK_CHECK(MuxBasic(path, true, 1000000ULL, &data));
  std::remove(path.c_str());

  const std::vector<size_t> starts = FindClusterStarts(data);
  TASK_CHECK(starts.size() == 2);
  EbmlElement cluster_header;
  TASK_CHECK(ParseOneElement(data, starts[0], &cluster_header));
  size_t cursor = cluster_header.payload_pos;
  EbmlElement child;
  TASK_CHECK(ParseOneElement(data, cursor, &child));
  TASK_CHECK(child.id == libwebm::kMkvCRC32);
  cursor = child.payload_pos + static_cast<size_t>(child.size);
  TASK_CHECK(ParseOneElement(data, cursor, &child));
  TASK_CHECK(child.id == libwebm::kMkvTimecode);
  const size_t flip = child.payload_pos;
  Bytes corrupted = data;
  corrupted[flip] ^= 0x01;
  TASK_CHECK(VerifyBytesOnce(corrupted) == mkvparser::E_FILE_FORMAT_INVALID);
  return 0;
}

static int CoverageUnknownSize() {
  // Unknown size cluster terminated by physical EOF.
  const Bytes cluster = BuildMiddleCrcCluster(true);
  TASK_CHECK(!cluster.empty());
  const Bytes data = WrapExternalCluster(cluster);
  TASK_CHECK(!data.empty());
  TASK_CHECK(VerifyBytesOnce(data) == 0);

  // A truncated prefix cannot determine the boundary yet.
  LimitedReader* reader = NULL;
  mkvparser::Segment* segment = NULL;
  const mkvparser::Cluster* cluster_obj = NULL;
  const size_t preload_limit = data.size() - 2;
  TASK_CHECK(PreloadFirstCluster(data, preload_limit, &reader, &segment,
                                 &cluster_obj));
  long long pos = 0;
  long len = 0;
  TASK_CHECK(cluster_obj->VerifyCRC(pos, len) ==
             mkvparser::E_BUFFER_NOT_FULL);
  TASK_CHECK(pos >= 0 && len > 0);

  // After the remainder arrives the same cluster verifies.
  LimitedReader* limited = reader;
  limited->set_limit(data.size());
  TASK_CHECK(cluster_obj->VerifyCRC(pos, len) == 0);
  delete segment;
  delete reader;
  return 0;
}

static int CoverageIndependentDelimiterCheck() {
  const std::string path = TempPath("coverage_delimiter");
  Bytes data;
  TASK_CHECK(MuxBasic(path, true, 1000000ULL, &data));
  std::remove(path.c_str());

  const std::vector<size_t> starts = FindClusterStarts(data);
  TASK_CHECK(starts.size() == 2);
  for (size_t i = 0; i < starts.size(); ++i) {
    EbmlElement cluster_header;
    TASK_CHECK(ParseOneElement(data, starts[i], &cluster_header));
    TASK_CHECK(cluster_header.id == libwebm::kMkvCluster);
    const size_t end =
        cluster_header.payload_pos +
        static_cast<size_t>(cluster_header.size);

    // Walk every child boundary independently.
    size_t cursor = cluster_header.payload_pos;
    int crc_count = 0;
    int block_count = 0;
    while (cursor < end) {
      EbmlElement child;
      TASK_CHECK(ParseOneElement(data, cursor, &child));
      if (child.id == libwebm::kMkvCRC32) {
        TASK_CHECK(child.size == 4);
        ++crc_count;
        TASK_CHECK(cursor == cluster_header.payload_pos);  // first child
      }
      if (child.id == libwebm::kMkvSimpleBlock ||
          child.id == libwebm::kMkvBlockGroup)
        ++block_count;
      cursor = child.payload_pos + static_cast<size_t>(child.size);
    }
    TASK_CHECK(cursor == end);
    TASK_CHECK(crc_count == 1);
    TASK_CHECK(block_count >= 1);
  }
  return 0;
}

// ===========================================================================
// Incremental group
// ===========================================================================

static int IncrementalGrowingPrefix() {
  const std::string path = TempPath("incr_grow");
  Bytes data;
  TASK_CHECK(MuxBasic(path, true, 1000000ULL, &data));
  std::remove(path.c_str());

  const std::vector<size_t> starts = FindClusterStarts(data);
  TASK_CHECK(starts.size() == 2);

  // Start inside the first cluster (its header plus part of the CRC element),
  // so the very first verification is short of data. Preloading a cluster does
  // not require its blocks to be present.
  size_t limit = starts[0] + 14;
  TASK_CHECK(limit < data.size());

  LimitedReader* reader = NULL;
  mkvparser::Segment* segment = NULL;
  const mkvparser::Cluster* cluster = NULL;
  TASK_CHECK(PreloadFirstCluster(data, limit, &reader, &segment, &cluster));
  LimitedReader* limited = reader;

  bool saw_underflow = false;
  for (; limit < data.size(); ++limit) {
    limited->set_limit(limit);
    long long pos = 0;
    long len = 0;
    const long status = cluster->VerifyCRC(pos, len);
    if (status == mkvparser::E_BUFFER_NOT_FULL) {
      saw_underflow = true;
      TASK_CHECK(pos >= 0 && len >= 1 && len <= 4096);
    } else {
      TASK_CHECK(status == 0);
      break;
    }
  }
  TASK_CHECK(saw_underflow);

  long long pos = 0;
  long len = 0;
  limited->set_limit(data.size());
  TASK_CHECK(cluster->VerifyCRC(pos, len) == 0);

  // A fresh pass over the fully available data must agree.
  TASK_CHECK(VerifyBytesOnce(data) == 0);
  delete segment;
  delete reader;
  return 0;
}

static int IncrementalStepSizes() {
  const std::string path = TempPath("incr_steps");
  Bytes data;
  int vframes = 0;
  int aframes = 0;
  TASK_CHECK(MuxDelayed(path, true, &vframes, &aframes, &data));
  std::remove(path.c_str());
  TASK_CHECK(vframes == 6 && aframes == 12);

  const std::vector<size_t> starts = FindClusterStarts(data);
  TASK_CHECK(!starts.empty());

  static const size_t kSteps[] = {1, 7, 31, 257, 4095, 4096, 4097};
  for (size_t s = 0; s < sizeof(kSteps) / sizeof(kSteps[0]); ++s) {
    size_t limit = starts[0] + 20;
    LimitedReader* reader = NULL;
    mkvparser::Segment* segment = NULL;
    const mkvparser::Cluster* cluster = NULL;
    TASK_CHECK(PreloadFirstCluster(data, limit, &reader, &segment, &cluster));
    LimitedReader* limited = reader;

    long last_status = mkvparser::E_BUFFER_NOT_FULL;
    while (last_status == mkvparser::E_BUFFER_NOT_FULL && limit < data.size()) {
      limit = std::min(data.size(), limit + kSteps[s]);
      limited->set_limit(limit);
      long long pos = 0;
      long len = 0;
      last_status = cluster->VerifyCRC(pos, len);
      TASK_CHECK(last_status == 0 ||
                 last_status == mkvparser::E_BUFFER_NOT_FULL);
    }
    TASK_CHECK(last_status == 0);
    delete segment;
    delete reader;
  }
  TASK_CHECK(VerifyBytesOnce(data) == 0);
  return 0;
}

static int IncrementalCursorUnaffected() {
  const std::string path = TempPath("incr_cursor");
  Bytes data;
  TASK_CHECK(MuxBasic(path, true, 1000000ULL, &data));

  ParsedSegment parsed;
  TASK_CHECK(OpenFileSegment(path, &parsed));
  const mkvparser::Cluster* cluster = parsed.segment->GetFirst();
  TASK_CHECK(cluster != NULL && !cluster->EOS());

  const mkvparser::BlockEntry* before = NULL;
  TASK_CHECK(cluster->GetFirst(before) == 0);
  TASK_CHECK(before != NULL);

  long long pos = 0;
  long len = 0;
  TASK_CHECK(cluster->VerifyCRC(pos, len) == 0);

  // The block traversal cursor must be exactly where it was.
  const mkvparser::BlockEntry* again = NULL;
  TASK_CHECK(cluster->GetFirst(again) == 0);
  TASK_CHECK(again == before);

  // The single entry cluster reports EOS after its only block; verification
  // must not change that.
  const mkvparser::BlockEntry* end = NULL;
  TASK_CHECK(cluster->GetNext(before, end) == 0);
  TASK_CHECK(end == NULL || end->EOS());
  TASK_CHECK(cluster->VerifyCRC(pos, len) == 0);
  const mkvparser::BlockEntry* end_again = NULL;
  TASK_CHECK(cluster->GetNext(before, end_again) == 0);
  TASK_CHECK((end == NULL && end_again == NULL) ||
             (end != NULL && end_again != NULL && end->EOS() &&
              end_again->EOS()));

  // A cluster with multiple blocks keeps its cursor stable across VerifyCRC.
  const mkvparser::Cluster* second = parsed.segment->GetNext(cluster);
  TASK_CHECK(second != NULL && !second->EOS());
  const mkvparser::BlockEntry* first = NULL;
  TASK_CHECK(second->GetFirst(first) == 0 && first != NULL);
  const mkvparser::BlockEntry* next = NULL;
  TASK_CHECK(second->GetNext(first, next) == 0);
  TASK_CHECK(next != NULL && !next->EOS());
  TASK_CHECK(second->VerifyCRC(pos, len) == 0);
  const mkvparser::BlockEntry* next_again = NULL;
  TASK_CHECK(second->GetNext(first, next_again) == 0);
  TASK_CHECK(next_again == next);

  CloseParsedSegment(&parsed);
  std::remove(path.c_str());
  return 0;
}

static int IncrementalMiddleCrc() {
  const Bytes cluster = BuildMiddleCrcCluster(false);
  const Bytes data = WrapExternalCluster(cluster);
  TASK_CHECK(!data.empty());

  size_t limit = FindClusterStarts(data)[0] + 25;
  LimitedReader* reader = NULL;
  mkvparser::Segment* segment = NULL;
  const mkvparser::Cluster* cluster_obj = NULL;
  TASK_CHECK(PreloadFirstCluster(data, limit, &reader, &segment,
                                 &cluster_obj));
  LimitedReader* limited = reader;

  bool underflow = false;
  while (limit < data.size()) {
    limited->set_limit(limit);
    long long pos = 0;
    long len = 0;
    const long status = cluster_obj->VerifyCRC(pos, len);
    if (status == 0)
      break;
    TASK_CHECK(status == mkvparser::E_BUFFER_NOT_FULL);
    underflow = true;
    limit += 13;
  }
  TASK_CHECK(underflow);
  limited->set_limit(data.size());
  long long pos = 0;
  long len = 0;
  TASK_CHECK(cluster_obj->VerifyCRC(pos, len) == 0);
  delete segment;
  delete reader;
  return 0;
}

static int IncrementalReadFailure() {
  const std::string path = TempPath("incr_readfail");
  Bytes data;
  TASK_CHECK(MuxBasic(path, true, 1000000ULL, &data));
  std::remove(path.c_str());

  const int kInjectedError = -77;
  LimitedReader* reader = NULL;
  mkvparser::Segment* segment = NULL;
  const mkvparser::Cluster* cluster = NULL;
  TASK_CHECK(PreloadFirstCluster(data, data.size(), &reader, &segment,
                                 &cluster));
  LimitedReader* limited = reader;
  limited->set_read_failure(static_cast<long long>(data.size() / 2),
                            kInjectedError);
  long long pos = 0;
  long len = 0;
  TASK_CHECK(cluster->VerifyCRC(pos, len) == kInjectedError);
  delete segment;
  delete reader;
  return 0;
}

static int IncrementalUnknownSizeTerminator() {
  // Unknown size cluster followed by a second cluster: the next Cluster ID is
  // the boundary, and until it is available the result is underflow.
  Bytes first = BuildMiddleCrcCluster(true);
  Bytes second = BuildMiddleCrcCluster(false);
  Bytes wrapped_first = WrapExternalCluster(first);
  Bytes data = wrapped_first;
  data.insert(data.end(), second.begin(), second.end());

  size_t limit = data.size() - second.size() + 3;  // terminator incomplete
  LimitedReader* reader = NULL;
  mkvparser::Segment* segment = NULL;
  const mkvparser::Cluster* cluster = NULL;
  TASK_CHECK(PreloadFirstCluster(data, limit, &reader, &segment, &cluster));
  LimitedReader* limited = reader;
  long long pos = 0;
  long len = 0;
  TASK_CHECK(cluster->VerifyCRC(pos, len) == mkvparser::E_BUFFER_NOT_FULL);

  limited->set_limit(data.size());
  TASK_CHECK(cluster->VerifyCRC(pos, len) == 0);
  delete segment;
  delete reader;

  // And with no terminator at all, physical EOF closes the cluster.
  const Bytes eof_data = wrapped_first;
  TASK_CHECK(VerifyBytesOnce(eof_data) == 0);
  return 0;
}

// ===========================================================================
// MuxState group
// ===========================================================================

static int MuxStateByteIdenticalWhenDisabled() {
  bool matched = false;
  for (int attempt = 0; attempt < 3 && !matched; ++attempt) {
    const std::string default_path = TempPath("state_default");
    const std::string off_path = TempPath("state_off");
    Bytes default_bytes;
    Bytes off_bytes;
    TASK_CHECK(MuxBasic(default_path, false, 1000000ULL, &default_bytes));
    TASK_CHECK(MuxBasic(off_path, false, 1000000ULL, &off_bytes));
    std::remove(default_path.c_str());
    std::remove(off_path.c_str());
    matched = (default_bytes == off_bytes);
  }
  TASK_CHECK(matched);

  // The disabled output must not carry a CRC element as its first child.
  const std::string path = TempPath("state_check");
  Bytes data;
  TASK_CHECK(MuxBasic(path, false, 1000000ULL, &data));
  std::remove(path.c_str());
  const std::vector<size_t> starts = FindClusterStarts(data);
  TASK_CHECK(!starts.empty());
  EbmlElement cluster_header;
  TASK_CHECK(ParseOneElement(data, starts[0], &cluster_header));
  EbmlElement first_child;
  TASK_CHECK(ParseOneElement(data, cluster_header.payload_pos, &first_child));
  TASK_CHECK(first_child.id != libwebm::kMkvCRC32);
  return 0;
}

static int MuxStateLiveRejected() {
  VectorWriter writer(true);
  mkvmuxer::Segment segment;
  TASK_CHECK(segment.Init(&writer));
  segment.set_mode(mkvmuxer::Segment::kLive);
  TASK_CHECK(!segment.SetClusterCRC(true));

  // Live muxing without CRC still works.
  const uint64_t video = segment.AddVideoTrack(8, 8, 1);
  segment.GetTrackByNumber(video)->set_codec_id("V_VP8");
  const Bytes frame = MakeFrame(0x90, 8);
  TASK_CHECK(segment.AddFrame(frame.data(), frame.size(), video, 0, true));
  TASK_CHECK(segment.Finalize());

  // Enabling after the header has been written is rejected.
  TASK_CHECK(!segment.SetClusterCRC(true));
  return 0;
}

static int MuxStateIncompatibleCombinations() {
  // Non-seekable writer is rejected.
  VectorWriter non_seekable(false);
  mkvmuxer::Segment segment;
  TASK_CHECK(segment.Init(&non_seekable));
  TASK_CHECK(!segment.SetClusterCRC(true));

  // Chunking enabled before CRC is rejected.
  const std::string chunk_base = TempPath("state_chunk");
  mkvmuxer::MkvWriter file_writer;
  TASK_CHECK(file_writer.Open((chunk_base + ".out").c_str()));
  mkvmuxer::Segment chunked;
  TASK_CHECK(chunked.Init(&file_writer));
  TASK_CHECK(chunked.SetChunking(true, chunk_base.c_str()));
  TASK_CHECK(!chunked.SetClusterCRC(true));
  file_writer.Close();
  std::remove((chunk_base + ".out").c_str());
  std::remove((chunk_base + ".hdr").c_str());
  // SetChunking opens an initial chunk writer.
  std::remove((chunk_base + "_000000.chk").c_str());

  // CRC enabled first, then switching to live: the header write fails and the
  // segment enters its failed state.
  VectorWriter live_writer(true);
  mkvmuxer::Segment switched;
  TASK_CHECK(switched.Init(&live_writer));
  TASK_CHECK(switched.SetClusterCRC(true));
  switched.set_mode(mkvmuxer::Segment::kLive);
  const uint64_t video = switched.AddVideoTrack(8, 8, 1);
  switched.GetTrackByNumber(video)->set_codec_id("V_VP8");
  const Bytes frame = MakeFrame(0x91, 8);
  TASK_CHECK(!switched.AddFrame(frame.data(), frame.size(), video, 0, true));
  TASK_CHECK(switched.failed());
  TASK_CHECK(!switched.Finalize());
  return 0;
}

static int MuxStateWriteFailure() {
  FailingWriter writer;
  mkvmuxer::Segment segment;
  TASK_CHECK(segment.Init(&writer));
  TASK_CHECK(segment.SetClusterCRC(true));

  const uint64_t video = segment.AddVideoTrack(8, 8, 1);
  segment.GetTrackByNumber(video)->set_codec_id("V_VP8");
  const Bytes f0 = MakeFrame(0x01, 8);
  TASK_CHECK(segment.AddFrame(f0.data(), f0.size(), video, 0, true));

  writer.FailWrites();
  const Bytes f1 = MakeFrame(0x02, 8);
  TASK_CHECK(!segment.AddFrame(f1.data(), f1.size(), video, kNsPerMs, true));
  TASK_CHECK(segment.failed());

  // Every subsequent operation stays failed.
  const Bytes f2 = MakeFrame(0x03, 8);
  TASK_CHECK(!segment.AddFrame(f2.data(), f2.size(), video, 2 * kNsPerMs,
                               true));
  mkvmuxer::Frame generic;
  TASK_CHECK(generic.Init(f2.data(), f2.size()));
  generic.set_track_number(video);
  generic.set_timestamp(3 * kNsPerMs);
  generic.set_is_key(true);
  TASK_CHECK(!segment.AddGenericFrame(&generic));
  TASK_CHECK(!segment.Finalize());
  return 0;
}

static int MuxStateBackfillFailure() {
  FailingWriter writer;
  mkvmuxer::Segment segment;
  TASK_CHECK(segment.Init(&writer));
  TASK_CHECK(segment.SetClusterCRC(true));

  const uint64_t video = segment.AddVideoTrack(8, 8, 1);
  segment.GetTrackByNumber(video)->set_codec_id("V_VP8");
  const Bytes f0 = MakeFrame(0x04, 8);
  TASK_CHECK(segment.AddFrame(f0.data(), f0.size(), video, 0, true));
  TASK_CHECK(writer.seek_calls() == 0);

  // The first seek happens while finalizing the cluster (CRC backfill and
  // size backfill). Failing it must poison the segment.
  writer.FailSeeks();
  TASK_CHECK(!segment.Finalize());
  TASK_CHECK(segment.failed());
  TASK_CHECK(writer.seek_calls() > 0);

  // Retrying Finalize after the failure keeps returning false.
  TASK_CHECK(!segment.Finalize());
  return 0;
}

static int MuxStateDelayedDrain() {
  const std::string path = TempPath("state_delayed");
  Bytes data;
  int vframes = 0;
  int aframes = 0;
  TASK_CHECK(MuxDelayed(path, true, &vframes, &aframes, &data));
  std::remove(path.c_str());
  WriteFile(path, data);

  ParsedSegment parsed;
  TASK_CHECK(OpenFileSegment(path, &parsed));
  TASK_CHECK(VerifyAllClusters(parsed) == 0);
  TASK_CHECK(CountBlockEntries(parsed) == vframes + aframes);

  // The final cluster must contain the drained tail audio frame.
  const mkvparser::Cluster* last = NULL;
  int clusters = 0;
  for (const mkvparser::Cluster* c = parsed.segment->GetFirst();
       c != NULL && !c->EOS(); c = parsed.segment->GetNext(c)) {
    last = c;
    ++clusters;
  }
  TASK_CHECK(clusters >= 2 && last != NULL);
  const mkvparser::BlockEntry* entry = NULL;
  TASK_CHECK(last->GetFirst(entry) == 0);
  bool last_cluster_has_audio = false;
  while (entry != NULL && !entry->EOS()) {
    const mkvparser::Block* block = entry->GetBlock();
    TASK_CHECK(block != NULL);
    if (block->GetTrackNumber() == 2)
      last_cluster_has_audio = true;
    const mkvparser::BlockEntry* next = NULL;
    TASK_CHECK(last->GetNext(entry, next) == 0);
    entry = next;
  }
  TASK_CHECK(last_cluster_has_audio);
  CloseParsedSegment(&parsed);
  std::remove(path.c_str());
  return 0;
}

static int MuxStateCuesMovedBeforeClusters() {
  const std::string src_path = TempPath("state_move_src");
  const std::string dst_path = TempPath("state_move_dst");

  // Produce a CRC enabled file while keeping the producing segment alive.
  mkvmuxer::MkvWriter writer;
  TASK_CHECK(writer.Open(src_path.c_str()));
  mkvmuxer::Segment segment;
  TASK_CHECK(segment.Init(&writer));
  TASK_CHECK(segment.SetClusterCRC(true));
  const uint64_t video = segment.AddVideoTrack(16, 16, 1);
  const uint64_t audio = segment.AddAudioTrack(48000, 2, 2);
  segment.GetTrackByNumber(video)->set_codec_id("V_VP8");
  segment.GetTrackByNumber(audio)->set_codec_id("A_OPUS");
  const Bytes v0 = MakeFrame(0x10, 8);
  const Bytes a0 = MakeFrame(0x40, 6);
  const Bytes v1 = MakeFrame(0x70, 8);
  TASK_CHECK(segment.AddFrame(v0.data(), v0.size(), video, 0, true));
  TASK_CHECK(segment.AddFrame(a0.data(), a0.size(), audio, kNsPerMs, true));
  segment.ForceNewClusterOnNextFrame();
  TASK_CHECK(segment.AddFrame(v1.data(), v1.size(), video, 10 * kNsPerMs,
                              true));
  TASK_CHECK(segment.Finalize());
  writer.Close();

  // Move the Cues in front of the Clusters; cluster payloads are copied byte
  // for byte, so the existing CRC values stay valid.
  mkvparser::MkvReader src_reader;
  TASK_CHECK(src_reader.Open(src_path.c_str()) == 0);
  mkvmuxer::MkvWriter moved_writer;
  TASK_CHECK(moved_writer.Open(dst_path.c_str()));
  TASK_CHECK(segment.CopyAndMoveCuesBeforeClusters(&src_reader, &moved_writer));
  moved_writer.Close();
  src_reader.Close();

  const Bytes moved = ReadFile(dst_path);
  TASK_CHECK(!moved.empty());

  ParsedSegment parsed;
  TASK_CHECK(OpenFileSegment(dst_path, &parsed));
  TASK_CHECK(VerifyAllClusters(parsed) == 0);
  const mkvparser::Cues* cues = parsed.segment->GetCues();
  TASK_CHECK(cues != NULL && cues->LoadCuePoint());
  const mkvparser::CuePoint* cue = cues->GetFirst();
  TASK_CHECK(cue != NULL);
  const mkvparser::Track* track =
      parsed.segment->GetTracks()->GetTrackByNumber(1);
  const mkvparser::CuePoint::TrackPosition* tp = cue->Find(track);
  TASK_CHECK(tp != NULL);

  // CueClusterPosition still points at real cluster payload.
  const long long cluster_abs = parsed.segment->m_start + tp->m_pos;
  TASK_CHECK(moved[static_cast<size_t>(cluster_abs)] == 0x1F &&
             moved[static_cast<size_t>(cluster_abs) + 1] == 0x43);
  TASK_CHECK(tp->m_block == 1);
  CloseParsedSegment(&parsed);
  std::remove(src_path.c_str());
  std::remove(dst_path.c_str());
  return 0;
}

static int MuxStateDelayedWithoutCrcUnchanged() {
  // The delayed drain path must behave when CRC is disabled too, and the
  // resulting file must parse without a CRC.
  const std::string path = TempPath("state_delayed_off");
  Bytes data;
  int vframes = 0;
  int aframes = 0;
  TASK_CHECK(MuxDelayed(path, false, &vframes, &aframes, &data));
  std::remove(path.c_str());
  WriteFile(path, data);
  ParsedSegment parsed;
  TASK_CHECK(OpenFileSegment(path, &parsed));
  TASK_CHECK(CountBlockEntries(parsed) == vframes + aframes);
  for (const mkvparser::Cluster* c = parsed.segment->GetFirst();
       c != NULL && !c->EOS(); c = parsed.segment->GetNext(c)) {
    long long pos = 0;
    long len = 0;
    TASK_CHECK(c->VerifyCRC(pos, len) == 1);
  }
  CloseParsedSegment(&parsed);
  std::remove(path.c_str());
  return 0;
}

// ===========================================================================
// Registration
// ===========================================================================

const task_case task_cases[] = {
    {"Coverage", "MultiCluster", CoverageMultiCluster},
    {"Coverage", "NonDefaultTimecodeScale", CoverageNonDefaultScale},
    {"Coverage", "SimpleBlockAndBlockGroup", CoverageBlockGroup},
    {"Coverage", "CueClusterPositionAndBlockNumber", CoverageCuePositions},
    {"Coverage", "ExternalCrcInMiddle", CoverageExternalMiddleCrc},
    {"Coverage", "CrcMissing", CoverageMissingCrc},
    {"Coverage", "DuplicateCrcRejected", CoverageDuplicateCrc},
    {"Coverage", "WrongCrcLengthRejected", CoverageBadCrcLength},
    {"Coverage", "PayloadByteFlipDetected", CoverageFlipPayloadByte},
    {"Coverage", "TimecodeByteFlipDetected", CoverageFlipTimecodeByte},
    {"Coverage", "UnknownSizeClusterBoundary", CoverageUnknownSize},
    {"Coverage", "IndependentEbmlDelimiterAudit",
     CoverageIndependentDelimiterCheck},

    {"Incremental", "GrowingPrefix", IncrementalGrowingPrefix},
    {"Incremental", "MultipleStepSizes", IncrementalStepSizes},
    {"Incremental", "BlockCursorUnaffected", IncrementalCursorUnaffected},
    {"Incremental", "ExternalMiddleCrcIncremental", IncrementalMiddleCrc},
    {"Incremental", "ReaderFailurePreserved", IncrementalReadFailure},
    {"Incremental", "UnknownSizeTerminator", IncrementalUnknownSizeTerminator},

    {"MuxState", "ByteIdenticalWhenDisabled", MuxStateByteIdenticalWhenDisabled},
    {"MuxState", "LiveModeRejected", MuxStateLiveRejected},
    {"MuxState", "IncompatibleCombinations", MuxStateIncompatibleCombinations},
    {"MuxState", "WriteFailureEntersFailedState", MuxStateWriteFailure},
    {"MuxState", "BackfillFailureEntersFailedState", MuxStateBackfillFailure},
    {"MuxState", "DelayedDrainAndFinalizeAudio", MuxStateDelayedDrain},
    {"MuxState", "CuesMovedBeforeClusters", MuxStateCuesMovedBeforeClusters},
    {"MuxState", "DelayedDrainWithoutCrc", MuxStateDelayedWithoutCrcUnchanged},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
