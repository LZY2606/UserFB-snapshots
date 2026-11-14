// Copyright (c) 2026 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
//
// Tests for mkvmuxer::Segment::InitAppend(). The cases build recognizable but
// entirely synthetic WebM containers through the public muxer API, destroy the
// producing objects, reopen the bytes with InitAppend(), append two rounds, and
// verify the result with both mkvparser and an independent hand-written EBML
// walker.

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/webmids.h"
#include "mkvmuxer/mkvmuxer.h"
#include "mkvparser/mkvparser.h"
#include "task_support/check.h"

// check.h provides TASK_CHECK; define the equality and ordering comparisons the
// cases need on top of it. Every macro returns 1 from the enclosing task case.
#define TASK_CHECK_EQ(a, b) TASK_CHECK((a) == (b))
#define TASK_CHECK_GT(a, b) TASK_CHECK((a) > (b))
#define TASK_CHECK_GE(a, b) TASK_CHECK((a) >= (b))
#define TASK_CHECK_LE(a, b) TASK_CHECK((a) <= (b))

// Assertions usable inside helpers that do not return an int status. The
// task-case entry points themselves use the TASK_CHECK* macros instead.
#define EXPECT(cond) assert(cond)
#define EXPECT_EQ(a, b) assert((a) == (b))
#define EXPECT_GT(a, b) assert((a) > (b))
#define EXPECT_GE(a, b) assert((a) >= (b))
#define EXPECT_LE(a, b) assert((a) <= (b))

namespace {

const uint64_t kNsPerMs = 1000000ULL;

// In-memory seekable writer. It keeps every byte so reopening and overwriting
// the tail can be exercised without touching the real filesystem.
class VectorWriter : public mkvmuxer::IMkvWriter {
 public:
  mkvmuxer::int32 Write(const void* buf, mkvmuxer::uint32 len) override {
    if (fail_write_)
      return -1;
    if (static_cast<uint64_t>(pos_) + len > bytes_.size())
      bytes_.resize(static_cast<size_t>(pos_) + len);
    std::memcpy(bytes_.data() + pos_, buf, len);
    pos_ += len;
    return 0;
  }

  mkvmuxer::int64 Position() const override { return pos_; }

  mkvmuxer::int32 Position(mkvmuxer::int64 pos) override {
    if (pos < 0)
      return -1;
    if (fail_seek_)
      return -1;
    pos_ = pos;
    if (static_cast<uint64_t>(pos_) > bytes_.size())
      bytes_.resize(static_cast<size_t>(pos_));
    return 0;
  }

  bool Seekable() const override { return true; }
  void ElementStartNotify(mkvmuxer::uint64, mkvmuxer::int64) override {}

  void FailWrites() { fail_write_ = true; }
  void FailSeeks() { fail_seek_ = true; }

  std::vector<uint8_t> bytes_;
  mkvmuxer::int64 pos_ = 0;

 private:
  bool fail_write_ = false;
  bool fail_seek_ = false;
};

// Copies the bytes so the reader stays valid even when the writer grows or
// truncates the underlying vector.
class VectorReader : public mkvparser::IMkvReader {
 public:
  explicit VectorReader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}

  int Read(long long pos, long len, unsigned char* buf) override {
    if (pos < 0 || len < 0 ||
        pos + len > static_cast<long long>(bytes_.size())) {
      return -1;
    }
    std::memcpy(buf, bytes_.data() + pos, static_cast<size_t>(len));
    return 0;
  }

  int Length(long long* total, long long* available) override {
    *total = *available = static_cast<long long>(bytes_.size());
    return 0;
  }

 private:
  std::vector<uint8_t> bytes_;
};

// A reader that never serves bytes. Length reports the real file size so the
// rejection is proven to come from a Read() failure inside recovery, not from a
// length mismatch.
class FailingReader : public mkvparser::IMkvReader {
 public:
  explicit FailingReader(long long length) : length_(length) {}

  int Read(long long, long, unsigned char*) override { return -1; }

  int Length(long long* total, long long* available) override {
    *total = *available = length_;
    return 0;
  }

 private:
  long long length_;
};

// A reader that never serves the frame payload region. InitAppend() must only
// read metadata and block headers, so this reader permits everything else but
// fails any read whose start is inside a block payload.
class PayloadBlockedReader : public mkvparser::IMkvReader {
 public:
  PayloadBlockedReader(const std::vector<uint8_t>& bytes,
                       std::vector<std::pair<int64_t, int64_t> > payloads)
      : bytes_(bytes), blocked_(std::move(payloads)) {}

  int Read(long long pos, long len, unsigned char* buf) override {
    if (pos < 0 || len <= 0)
      return -1;
    for (const auto& range : blocked_) {
      if (pos >= range.first && pos < range.second)
        return -1;
    }
    if (pos + len > static_cast<long long>(bytes_.size()))
      return -1;
    std::memcpy(buf, bytes_.data() + pos, static_cast<size_t>(len));
    return 0;
  }

  int Length(long long* total, long long* available) override {
    *total = *available = static_cast<long long>(bytes_.size());
    return 0;
  }

 private:
  const std::vector<uint8_t>& bytes_;
  std::vector<std::pair<int64_t, int64_t> > blocked_;
};

// Description of one frame handed to the muxer.
struct FrameSpec {
  uint64_t track;
  uint64_t timestamp_ns;
  uint64_t duration_ns;
  bool key;
  uint8_t seed;
};

// Fills |frame| with distinctive, decode-free bytes and the requested timing.
void MakeFrame(mkvmuxer::Frame* frame, const FrameSpec& spec) {
  uint8_t data[8];
  for (int i = 0; i < 8; ++i)
    data[i] = static_cast<uint8_t>(spec.seed + i);
  assert(frame->Init(data, sizeof(data)));
  frame->set_track_number(spec.track);
  frame->set_timestamp(spec.timestamp_ns);
  frame->set_duration(spec.duration_ns);
  frame->set_is_key(spec.key);
}

// Builds the initial recording. When |with_audio| is false only a video track
// is created. |timecode_scale| overrides the default 1ms scale. The returned
// bytes form a complete, finalized, seekable WebM file.
std::vector<uint8_t> BuildInitial(bool with_audio, bool opus,
                                  uint64_t timecode_scale,
                                  uint64_t video_count,
                                  std::vector<FrameSpec>* all_frames) {
  VectorWriter writer;
  mkvmuxer::Segment segment;
  EXPECT(segment.Init(&writer));
  segment.GetSegmentInfo()->set_timecode_scale(timecode_scale);

  const uint64_t video_track = segment.AddVideoTrack(16, 16, 1);
  EXPECT_EQ(video_track, 1u);
  segment.GetTrackByNumber(1)->set_uid(0x1111111111111111ULL);
  segment.GetTrackByNumber(1)->set_default_duration(40 * kNsPerMs);
  static const uint8_t kVideoPrivate[] = {0x10, 0x20, 0x30, 0x40};
  EXPECT(segment.GetTrackByNumber(1)->SetCodecPrivate(kVideoPrivate,
                                                      sizeof(kVideoPrivate)));

  uint64_t audio_track = 0;
  if (with_audio) {
    audio_track = segment.AddAudioTrack(48000, 2, 2);
    EXPECT_EQ(audio_track, 2u);
    segment.GetTrackByNumber(2)->set_uid(0x2222222222222222ULL);
    segment.GetTrackByNumber(2)->set_default_duration(20 * kNsPerMs);
    if (opus) {
      segment.GetTrackByNumber(2)->set_codec_id(
          mkvmuxer::Tracks::kOpusCodecId);
      segment.GetTrackByNumber(2)->set_codec_delay(6500000ULL);
      segment.GetTrackByNumber(2)->set_seek_pre_roll(80000ULL);
      static const uint8_t kOpusHead[] = {'O', 'p', 'u', 's', 'H', 'e',
                                          'a', 'd'};
      EXPECT(segment.GetTrackByNumber(2)->SetCodecPrivate(
          kOpusHead, sizeof(kOpusHead)));
    } else {
      static const uint8_t kVorbisComment[] = {0x03, 'v', 'o', 'r', 'b', 'i',
                                              's'};
      EXPECT(segment.GetTrackByNumber(2)->SetCodecPrivate(
          kVorbisComment, sizeof(kVorbisComment)));
    }
  }

  // Force the VP9 codec id on the video track when requested.
  if (opus) {
    segment.GetTrackByNumber(1)->set_codec_id(
        mkvmuxer::Tracks::kVp9CodecId);
  }

  EXPECT(segment.CuesTrack(video_track));

  uint8_t seed = 0;
  for (uint64_t i = 0; i < video_count; ++i) {
    FrameSpec video;
    video.track = video_track;
    video.timestamp_ns = i * 40 * kNsPerMs;
    video.duration_ns = 40 * kNsPerMs;
    video.key = true;
    video.seed = seed++;
    mkvmuxer::Frame vframe;
    MakeFrame(&vframe, video);
    EXPECT(segment.AddGenericFrame(&vframe));
    if (all_frames)
      all_frames->push_back(video);

    if (with_audio) {
      FrameSpec audio;
      audio.track = audio_track;
      audio.timestamp_ns = i * 40 * kNsPerMs + 10 * kNsPerMs;
      audio.duration_ns = 20 * kNsPerMs;
      audio.key = true;
      audio.seed = seed++;
      mkvmuxer::Frame aframe;
      MakeFrame(&aframe, audio);
      EXPECT(segment.AddGenericFrame(&aframe));
      if (all_frames)
        all_frames->push_back(audio);
    }
  }

  EXPECT(segment.Finalize());
  return writer.bytes_;
}

// Appends one round of frames whose timestamps start strictly after
// |start_ns| and returns the new writer bytes.
std::vector<uint8_t> AppendRound(const std::vector<uint8_t>& input,
                                 uint64_t start_ns, bool with_audio,
                                 uint8_t seed_base) {
  VectorWriter writer;
  writer.bytes_ = input;
  writer.pos_ = static_cast<mkvmuxer::int64>(input.size());
  VectorReader reader(input);

  mkvmuxer::Segment segment;
  EXPECT(segment.InitAppend(&reader, &writer));
  // After InitAppend the writer must be positioned at or before the file end
  // (at the old Cues start, which the new clusters overwrite).
  EXPECT_LE(writer.Position(),
            static_cast<mkvmuxer::int64>(input.size()));

  const int kFrames = 3;
  for (int i = 0; i < kFrames; ++i) {
    FrameSpec video;
    video.track = 1;
    video.timestamp_ns = start_ns + static_cast<uint64_t>(i) * 40 * kNsPerMs;
    video.duration_ns = 40 * kNsPerMs;
    video.key = true;
    video.seed = static_cast<uint8_t>(seed_base + i);
    mkvmuxer::Frame vframe;
    MakeFrame(&vframe, video);
    EXPECT(segment.AddGenericFrame(&vframe));

    if (with_audio) {
      FrameSpec audio;
      audio.track = 2;
      audio.timestamp_ns =
          start_ns + static_cast<uint64_t>(i) * 40 * kNsPerMs + 10 * kNsPerMs;
      audio.duration_ns = 20 * kNsPerMs;
      audio.key = true;
      audio.seed = static_cast<uint8_t>(seed_base + 40 + i);
      mkvmuxer::Frame aframe;
      MakeFrame(&aframe, audio);
      EXPECT(segment.AddGenericFrame(&aframe));
    }
  }

  EXPECT(segment.Finalize());
  return writer.bytes_;
}

// Parses a WebM byte vector and returns the loaded mkvparser segment. The
// caller owns the returned object.
mkvparser::Segment* ParseWebM(const std::vector<uint8_t>& bytes) {
  VectorReader* reader = new VectorReader(bytes);
  mkvparser::EBMLHeader header;
  long long pos = 0;
  EXPECT_EQ(header.Parse(reader, pos), 0);

  mkvparser::Segment* segment = NULL;
  EXPECT_EQ(mkvparser::Segment::CreateInstance(reader, pos, segment), 0);
  EXPECT(segment != NULL);
  EXPECT_EQ(segment->Load(), 0);
  return segment;
}

// Counts clusters and blocks through the parser and returns the last block
// timestamp in nanoseconds.
void CountContents(mkvparser::Segment* segment, int* clusters, int* blocks,
                   int64_t* last_block_ns) {
  *clusters = 0;
  *blocks = 0;
  *last_block_ns = 0;
  for (const mkvparser::Cluster* cluster = segment->GetFirst();
       cluster && !cluster->EOS();
       cluster = segment->GetNext(cluster)) {
    ++(*clusters);
    const mkvparser::BlockEntry* entry = NULL;
    EXPECT_EQ(cluster->GetFirst(entry), 0);
    while (entry && !entry->EOS()) {
      const mkvparser::Block* block = entry->GetBlock();
      EXPECT(block != NULL);
      ++(*blocks);
      const int64_t time_ns = block->GetTime(cluster);
      EXPECT_GE(time_ns, 0);
      if (time_ns > *last_block_ns)
        *last_block_ns = time_ns;
      const mkvparser::BlockEntry* next = NULL;
      EXPECT_EQ(cluster->GetNext(entry, next), 0);
      entry = next;
    }
  }
}

// Independent EBML helpers that do not use mkvparser for the structural
// assertions. Every helper reads only the bytes it names.
namespace ebml {

int IdLength(uint8_t first) {
  if (first == 0)
    return -1;
  for (int i = 0; i < 4; ++i) {
    if (first & (0x80 >> i))
      return i + 1;
  }
  return -1;
}

int SizeLength(uint8_t first) {
  if (first == 0)
    return -1;
  int len = 1;
  uint8_t marker = 0x80;
  while (!(first & marker)) {
    marker >>= 1;
    ++len;
    if (len > 8)
      return -1;
  }
  return len;
}

bool ReadId(const std::vector<uint8_t>& bytes, int64_t pos, int64_t* id,
            int* id_len) {
  if (pos < 0 || pos >= static_cast<int64_t>(bytes.size()))
    return false;
  const int len = IdLength(bytes[static_cast<size_t>(pos)]);
  if (len <= 0 || pos + len > static_cast<int64_t>(bytes.size()))
    return false;
  int64_t value = 0;
  for (int i = 0; i < len; ++i)
    value = (value << 8) | bytes[static_cast<size_t>(pos + i)];
  *id = value;
  *id_len = len;
  return true;
}

bool ReadSize(const std::vector<uint8_t>& bytes, int64_t pos, int64_t* size,
              int* size_len) {
  if (pos < 0 || pos >= static_cast<int64_t>(bytes.size()))
    return false;
  const int len = SizeLength(bytes[static_cast<size_t>(pos)]);
  if (len <= 0 || pos + len > static_cast<int64_t>(bytes.size()))
    return false;
  int64_t value = 0;
  for (int i = 0; i < len; ++i)
    value = (value << 8) | bytes[static_cast<size_t>(pos + i)];
  value &= (len == 8) ? ((1LL << 56) - 1) : ((1LL << (7 * len)) - 1);
  *size = value;
  *size_len = len;
  return true;
}

// Reads an element header at |pos|; returns the payload position in
// |payload_pos| and the payload size in |payload_size|.
bool Header(const std::vector<uint8_t>& bytes, int64_t pos, int64_t stop,
            int64_t* id, int64_t* payload_pos, int64_t* payload_size) {
  int id_len = 0;
  if (!ReadId(bytes, pos, id, &id_len))
    return false;
  int size_len = 0;
  int64_t size = 0;
  if (!ReadSize(bytes, pos + id_len, &size, &size_len))
    return false;
  const int64_t value_pos = pos + id_len + size_len;
  if (stop >= 0 && value_pos + size > stop)
    return false;
  *payload_pos = value_pos;
  *payload_size = size;
  return true;
}

}  // namespace ebml

// Walks the level-1 elements and records the byte range of every cluster.
struct Level1Layout {
  int64_t segment_payload = 0;
  std::vector<std::pair<int64_t, int64_t> > clusters;  // start, end
  int64_t cues_start = -1;
  int64_t cues_end = -1;
};

bool WalkLevel1(const std::vector<uint8_t>& bytes, Level1Layout* layout) {
  VectorReader reader(bytes);
  mkvparser::EBMLHeader ebml;
  long long pos = 0;
  if (ebml.Parse(&reader, pos) != 0)
    return false;

  int64_t id = 0;
  int id_len = 0;
  if (!ebml::ReadId(bytes, pos, &id, &id_len) ||
      id != libwebm::kMkvSegment) {
    return false;
  }
  int64_t seg_size = 0;
  int seg_size_len = 0;
  if (!ebml::ReadSize(bytes, pos + id_len, &seg_size, &seg_size_len))
    return false;
  const int64_t payload = pos + id_len + seg_size_len;
  const int64_t stop = payload + seg_size;
  if (stop != static_cast<int64_t>(bytes.size()))
    return false;
  layout->segment_payload = payload;

  int64_t cur = payload;
  while (cur < stop) {
    int64_t elem_id = 0;
    int64_t elem_payload = 0;
    int64_t elem_size = 0;
    if (!ebml::Header(bytes, cur, stop, &elem_id, &elem_payload, &elem_size))
      return false;
    const int64_t elem_end = elem_payload + elem_size;
    if (elem_id == libwebm::kMkvCluster) {
      layout->clusters.push_back(std::make_pair(cur, elem_end));
    } else if (elem_id == libwebm::kMkvCues) {
      layout->cues_start = cur;
      layout->cues_end = elem_end;
    }
    cur = elem_end;
  }
  return cur == stop && layout->cues_start >= 0 &&
         layout->cues_end == stop;
}
// Reopens |bytes| with no new frames and finalizes immediately; the file must
// remain byte-for-byte identical and the writer must end where it started.
int ReopenNoChangeCase() {
  const std::vector<uint8_t> original =
      BuildInitial(true, false, 1000000ULL, 5, NULL);

  VectorWriter writer;
  writer.bytes_ = original;
  writer.pos_ = static_cast<mkvmuxer::int64>(original.size());
  VectorReader reader(original);

  mkvmuxer::Segment segment;
  TASK_CHECK(segment.InitAppend(&reader, &writer));
  TASK_CHECK(segment.Finalize());
  TASK_CHECK(writer.bytes_ == original);

  // Reopening again must also work and leave the file untouched.
  VectorWriter writer2;
  writer2.bytes_ = writer.bytes_;
  writer2.pos_ = static_cast<mkvmuxer::int64>(writer2.bytes_.size());
  VectorReader reader2(writer2.bytes_);
  mkvmuxer::Segment segment2;
  TASK_CHECK(segment2.InitAppend(&reader2, &writer2));
  TASK_CHECK(segment2.Finalize());
  TASK_CHECK(writer2.bytes_ == original);
  return 0;
}

// Audio staged ahead of any video key frame is simply discarded by Finalize
// when no new cluster is ever opened; the file must stay byte-for-byte
// identical because nothing was committed.
int ReopenStagedAudioNoClusterCase() {
  const std::vector<uint8_t> original =
      BuildInitial(true, false, 1000000ULL, 4, NULL);

  VectorWriter writer;
  writer.bytes_ = original;
  writer.pos_ = static_cast<mkvmuxer::int64>(original.size());
  VectorReader reader(original);

  mkvmuxer::Segment segment;
  TASK_CHECK(segment.InitAppend(&reader, &writer));

  // Leading audio is accepted into the hold-back queue but cannot open a
  // cluster on its own.
  mkvmuxer::Frame audio;
  MakeFrame(&audio, FrameSpec{2, 205 * kNsPerMs, 20 * kNsPerMs, true, 99});
  TASK_CHECK(segment.AddGenericFrame(&audio));

  // No video key frame follows, so nothing is committed and nothing changes.
  TASK_CHECK(segment.Finalize());
  TASK_CHECK(writer.bytes_ == original);

  // The untouched file can still be reopened for a real append afterwards.
  // Reposition the shared writer at the file end, as the InitAppend contract
  // requires.
  writer.pos_ = static_cast<mkvmuxer::int64>(writer.bytes_.size());
  VectorReader reader2(writer.bytes_);
  mkvmuxer::Segment segment2;
  TASK_CHECK(segment2.InitAppend(&reader2, &writer));
  mkvmuxer::Frame video;
  MakeFrame(&video, FrameSpec{1, 200 * kNsPerMs, 40 * kNsPerMs, true, 100});
  TASK_CHECK(segment2.AddGenericFrame(&video));
  TASK_CHECK(segment2.Finalize());
  TASK_CHECK(writer.bytes_ != original);
  return 0;
}

// Two append rounds, verifying old clusters stay at their original offsets,
// the merged Cues point at real clusters, and the duration grows correctly.
int ReopenAppendRoundsCase() {
  std::vector<FrameSpec> initial_frames;
  const std::vector<uint8_t> initial =
      BuildInitial(true, true, 1000000ULL, 5, &initial_frames);

  Level1Layout initial_layout;
  TASK_CHECK(WalkLevel1(initial, &initial_layout));
  TASK_CHECK_GT(initial_layout.clusters.size(), 0u);

  std::vector<uint8_t> once = AppendRound(initial, 240 * kNsPerMs, true, 70);
  std::vector<uint8_t> twice = AppendRound(once, 400 * kNsPerMs, true, 90);

  // Every old cluster must remain byte-for-byte at its original position.
  for (const auto& range : initial_layout.clusters) {
    TASK_CHECK_GT(range.second, range.first);
    TASK_CHECK_EQ(std::memcmp(initial.data() + range.first,
                              twice.data() + range.first,
                              static_cast<size_t>(range.second - range.first)),
                  0);
  }

  // The immutable header elements (TimecodeScale, Duration field shape,
  // MuxingApp, WritingApp and the whole Tracks element) are preserved byte for
  // byte. The SeekHead is intentionally rewritten to point at the new tail
  // Cues, so it is compared structurally through the parser below rather than
  // byte-for-byte.
  std::unique_ptr<mkvparser::Segment> before(ParseWebM(initial));
  std::unique_ptr<mkvparser::Segment> after(ParseWebM(twice));
  TASK_CHECK_EQ(before->GetInfo()->GetTimeCodeScale(),
                after->GetInfo()->GetTimeCodeScale());
  TASK_CHECK_EQ(before->GetTracks()->GetTracksCount(),
                after->GetTracks()->GetTracksCount());
  for (unsigned long t = 0; t < before->GetTracks()->GetTracksCount(); ++t) {
    const mkvparser::Track* a = before->GetTracks()->GetTrackByIndex(t);
    const mkvparser::Track* b = after->GetTracks()->GetTrackByIndex(t);
    TASK_CHECK_EQ(a->GetNumber(), b->GetNumber());
    TASK_CHECK_EQ(a->GetUid(), b->GetUid());
    TASK_CHECK_EQ(a->GetType(), b->GetType());
    TASK_CHECK_EQ(std::string(a->GetCodecId()),
                  std::string(b->GetCodecId()));
    TASK_CHECK_EQ(a->GetDefaultDuration(), b->GetDefaultDuration());
    size_t a_private = 0;
    size_t b_private = 0;
    const unsigned char* ap = a->GetCodecPrivate(a_private);
    const unsigned char* bp = b->GetCodecPrivate(b_private);
    TASK_CHECK_EQ(a_private, b_private);
    if (a_private > 0)
      TASK_CHECK_EQ(std::memcmp(ap, bp, a_private), 0);
  }

  // Parse the final file and count everything.
  std::unique_ptr<mkvparser::Segment> parsed(ParseWebM(twice));
  int clusters = 0;
  int blocks = 0;
  int64_t last_ns = 0;
  CountContents(parsed.get(), &clusters, &blocks, &last_ns);
  // 5 initial frames of each kind plus 3+3 appended per kind = 11 per track.
  TASK_CHECK_EQ(blocks, 22);
  TASK_CHECK_GE(clusters, 7);
  TASK_CHECK_GT(last_ns, 400 * static_cast<int64_t>(kNsPerMs));

  // Duration must cover the last appended frame end.
  TASK_CHECK_GE(parsed->GetDuration(),
                static_cast<int64_t>(440 * kNsPerMs));

  // The new tail Cues must point only at real cluster elements. Walk every
  // CuePoint through the parser and confirm the referenced file offset holds a
  // Cluster ID.
  const mkvparser::Cues* cues = parsed->GetCues();
  TASK_CHECK(cues != NULL);
  const mkvparser::Tracks* tracks = parsed->GetTracks();
  const mkvparser::Track* video_track = tracks->GetTrackByNumber(1);
  TASK_CHECK(video_track != NULL);
  Level1Layout final_layout;
  TASK_CHECK(WalkLevel1(twice, &final_layout));

  int cue_count = 0;
  while (cues->LoadCuePoint()) {
    const mkvparser::CuePoint* point = cues->GetFirst();
    if (cue_count > 0)
      point = cues->GetNext(point);
    TASK_CHECK(point != NULL);
    const mkvparser::CuePoint::TrackPosition* position =
        point->Find(video_track);
    TASK_CHECK(position != NULL);
    const int64_t cluster_file_pos =
        parsed->m_start + position->m_pos;
    int id_len = 0;
    int64_t id = 0;
    TASK_CHECK(ebml::ReadId(twice, cluster_file_pos, &id, &id_len));
    TASK_CHECK_EQ(id, static_cast<int64_t>(libwebm::kMkvCluster));
    ++cue_count;
  }
  TASK_CHECK_GE(cue_count, 5);

  // The final Cues region must start at or after the initial Cues start and the
  // file must end exactly at the new Cues end (no stale fragment left behind).
  TASK_CHECK_GE(final_layout.cues_start, initial_layout.cues_start);
  TASK_CHECK_EQ(final_layout.cues_end, static_cast<int64_t>(twice.size()));

  // The SeekHead must point at the new tail Cues, and the encoded Segment size
  // must reach the true file end.
  const mkvparser::SeekHead* seek_head = parsed->GetSeekHead();
  TASK_CHECK(seek_head != NULL);
  bool found_cues_entry = false;
  for (int i = 0; i < seek_head->GetCount(); ++i) {
    const mkvparser::SeekHead::Entry* entry = seek_head->GetEntry(i);
    if (entry->id != libwebm::kMkvCues)
      continue;
    found_cues_entry = true;
    const int64_t referenced_cues = parsed->m_start + entry->pos;
    TASK_CHECK_EQ(referenced_cues, final_layout.cues_start);
  }
  TASK_CHECK(found_cues_entry);

  // The encoded 8-byte Segment size, after stripping its EBML marker, must equal
  // the distance from the payload start to the actual file end.
  int64_t seg_size = 0;
  int seg_size_len = 0;
  TASK_CHECK(ebml::ReadSize(twice, parsed->m_start - 8, &seg_size,
                            &seg_size_len));
  TASK_CHECK_EQ(seg_size_len, 8);
  TASK_CHECK_EQ(seg_size,
                static_cast<int64_t>(twice.size()) - parsed->m_start);
  return 0;
}

// Non-default timecode scale plus video-only content, reopened twice.
int NonDefaultTimebaseVideoOnlyCase() {
  const std::vector<uint8_t> initial =
      BuildInitial(false, false, 250000ULL, 4, NULL);
  std::unique_ptr<mkvparser::Segment> parsed0(ParseWebM(initial));
  TASK_CHECK_EQ(parsed0->GetInfo()->GetTimeCodeScale(), 250000LL);

  std::vector<uint8_t> once = AppendRound(initial, 200 * kNsPerMs, false, 30);
  std::vector<uint8_t> twice = AppendRound(once, 360 * kNsPerMs, false, 50);

  std::unique_ptr<mkvparser::Segment> parsed(ParseWebM(twice));
  TASK_CHECK_EQ(parsed->GetInfo()->GetTimeCodeScale(), 250000LL);
  TASK_CHECK_EQ(parsed->GetTracks()->GetTracksCount(), 1UL);
  int clusters = 0;
  int blocks = 0;
  int64_t last_ns = 0;
  CountContents(parsed.get(), &clusters, &blocks, &last_ns);
  TASK_CHECK_EQ(blocks, 10);  // 4 + 3 + 3 video frames.
  return 0;
}

// Rejects frames for unknown tracks, missing duration, time regression and
// timestamp+duration overflow, while a following legal frame still commits.
int RejectInvalidFrameCase() {
  const std::vector<uint8_t> initial =
      BuildInitial(true, false, 1000000ULL, 4, NULL);

  VectorWriter writer;
  writer.bytes_ = initial;
  writer.pos_ = static_cast<mkvmuxer::int64>(initial.size());
  VectorReader reader(initial);
  mkvmuxer::Segment segment;
  TASK_CHECK(segment.InitAppend(&reader, &writer));

  const uint64_t start = 200 * kNsPerMs;

  // Unknown track number.
  {
    mkvmuxer::Frame frame;
    FrameSpec spec{99, start, 40 * kNsPerMs, true, 1};
    MakeFrame(&frame, spec);
    TASK_CHECK(!segment.AddGenericFrame(&frame));
  }

  // Zero duration.
  {
    mkvmuxer::Frame frame;
    FrameSpec spec{1, start, 0, true, 2};
    MakeFrame(&frame, spec);
    TASK_CHECK(!segment.AddGenericFrame(&frame));
  }

  // Missing duration entirely (duration not set).
  {
    uint8_t data[4] = {1, 2, 3, 4};
    mkvmuxer::Frame frame;
    TASK_CHECK(frame.Init(data, sizeof(data)));
    frame.set_track_number(1);
    frame.set_timestamp(start);
    frame.set_is_key(true);
    TASK_CHECK(!segment.AddGenericFrame(&frame));
  }

  // Timestamp not strictly greater than the largest old block timestamp.
  {
    mkvmuxer::Frame frame;
    FrameSpec spec{1, 120 * kNsPerMs, 40 * kNsPerMs, true, 3};
    MakeFrame(&frame, spec);
    TASK_CHECK(!segment.AddGenericFrame(&frame));
  }

  // timestamp + duration overflow.
  {
    mkvmuxer::Frame frame;
    FrameSpec spec{1, UINT64_MAX - 5, 40 * kNsPerMs, true, 4};
    MakeFrame(&frame, spec);
    TASK_CHECK(!segment.AddGenericFrame(&frame));
  }

  // A legal frame after the rejections must still commit and finalize.
  {
    mkvmuxer::Frame frame;
    FrameSpec spec{1, start, 40 * kNsPerMs, true, 5};
    MakeFrame(&frame, spec);
    TASK_CHECK(segment.AddGenericFrame(&frame));
  }
  TASK_CHECK(segment.Finalize());

  std::unique_ptr<mkvparser::Segment> parsed(ParseWebM(writer.bytes_));
  int clusters = 0;
  int blocks = 0;
  int64_t last_ns = 0;
  CountContents(parsed.get(), &clusters, &blocks, &last_ns);
  // 8 initial blocks (4 video + 4 audio) plus one appended video block.
  TASK_CHECK_EQ(blocks, 9);
  return 0;
}

// The first frame actually written must be a video key frame. Leading audio is
// held back by the existing muxer rule rather than rejected, but a non-key
// video frame cannot open the first cluster, and a video key frame then flushes
// the staged audio.
int FirstFrameMustBeVideoKeyCase() {
  const std::vector<uint8_t> initial =
      BuildInitial(true, false, 1000000ULL, 3, NULL);

  VectorWriter writer;
  writer.bytes_ = initial;
  writer.pos_ = static_cast<mkvmuxer::int64>(initial.size());
  VectorReader reader(initial);
  mkvmuxer::Segment segment;
  TASK_CHECK(segment.InitAppend(&reader, &writer));

  // Leading audio is staged (no cluster is created yet).
  {
    mkvmuxer::Frame audio;
    FrameSpec spec{2, 205 * kNsPerMs, 20 * kNsPerMs, true, 9};
    MakeFrame(&audio, spec);
    TASK_CHECK(segment.AddGenericFrame(&audio));
  }

  // A non-key video frame still cannot open the first cluster.
  {
    mkvmuxer::Frame video;
    FrameSpec spec{1, 200 * kNsPerMs, 40 * kNsPerMs, false, 10};
    MakeFrame(&video, spec);
    TASK_CHECK(!segment.AddGenericFrame(&video));
  }

  // A video key frame opens the cluster and the staged audio is flushed into it
  // by Finalize.
  {
    mkvmuxer::Frame video;
    FrameSpec spec{1, 200 * kNsPerMs, 40 * kNsPerMs, true, 11};
    MakeFrame(&video, spec);
    TASK_CHECK(segment.AddGenericFrame(&video));
  }
  TASK_CHECK(segment.Finalize());

  std::unique_ptr<mkvparser::Segment> parsed(ParseWebM(writer.bytes_));
  int clusters = 0;
  int blocks = 0;
  int64_t last_ns = 0;
  CountContents(parsed.get(), &clusters, &blocks, &last_ns);
  TASK_CHECK_EQ(blocks, 8);  // 6 initial + 1 video + 1 audio.
  return 0;
}

// ForceNewCluster on the resumed session creates another cluster at the old
// Cues starting point.
int ForceNewClusterCase() {
  const std::vector<uint8_t> initial =
      BuildInitial(true, false, 1000000ULL, 4, NULL);
  Level1Layout layout;
  TASK_CHECK(WalkLevel1(initial, &layout));

  VectorWriter writer;
  writer.bytes_ = initial;
  writer.pos_ = static_cast<mkvmuxer::int64>(initial.size());
  VectorReader reader(initial);
  mkvmuxer::Segment segment;
  TASK_CHECK(segment.InitAppend(&reader, &writer));

  mkvmuxer::Frame first;
  MakeFrame(&first, FrameSpec{1, 200 * kNsPerMs, 40 * kNsPerMs, true, 20});
  TASK_CHECK(segment.AddGenericFrame(&first));

  segment.ForceNewClusterOnNextFrame();
  mkvmuxer::Frame second;
  MakeFrame(&second, FrameSpec{1, 240 * kNsPerMs, 40 * kNsPerMs, true, 21});
  TASK_CHECK(segment.AddGenericFrame(&second));
  TASK_CHECK(segment.Finalize());

  Level1Layout after;
  TASK_CHECK(WalkLevel1(writer.bytes_, &after));
  // The first NEW cluster begins exactly where the old Cues used to be; the
  // earlier clusters in the layout are the untouched original clusters.
  int64_t first_new_cluster = -1;
  for (const auto& range : after.clusters) {
    if (range.first >= layout.cues_start) {
      first_new_cluster = range.first;
      break;
    }
  }
  TASK_CHECK_GE(first_new_cluster, 0);
  TASK_CHECK_EQ(first_new_cluster, layout.cues_start);
  TASK_CHECK_GT(after.clusters.size(), layout.clusters.size());
  return 0;
}

// A read failure during recovery and a writer not positioned at the file end
// both fail without modifying any byte.
int ReadFailureAndWriterPositionCase() {
  const std::vector<uint8_t> initial =
      BuildInitial(true, false, 1000000ULL, 3, NULL);

  FailingReader failing_reader(
      static_cast<long long>(initial.size()));
  VectorWriter writer;
  writer.bytes_ = initial;
  writer.pos_ = static_cast<mkvmuxer::int64>(initial.size());
  mkvmuxer::Segment segment;
  TASK_CHECK(!segment.InitAppend(&failing_reader, &writer));
  TASK_CHECK(writer.bytes_ == initial);

  // A writer left short of the file end must be rejected.
  VectorWriter writer2;
  writer2.bytes_ = initial;
  writer2.pos_ = static_cast<mkvmuxer::int64>(initial.size()) - 1;
  VectorReader reader2(initial);
  mkvmuxer::Segment segment2;
  TASK_CHECK(!segment2.InitAppend(&reader2, &writer2));

  // A non-seekable writer is rejected.
  class NonSeekableWriter : public VectorWriter {
   public:
    bool Seekable() const override { return false; }
  };
  NonSeekableWriter writer3;
  writer3.bytes_ = initial;
  writer3.pos_ = static_cast<mkvmuxer::int64>(initial.size());
  VectorReader reader3(initial);
  mkvmuxer::Segment segment3;
  TASK_CHECK(!segment3.InitAppend(&reader3, &writer3));

  // If the final repositioning seek onto the old Cues fails, InitAppend must
  // fail even though recovery itself read everything successfully.
  VectorWriter writer4;
  writer4.bytes_ = initial;
  writer4.pos_ = static_cast<mkvmuxer::int64>(initial.size());
  writer4.FailSeeks();
  VectorReader reader4(initial);
  mkvmuxer::Segment segment4;
  TASK_CHECK(!segment4.InitAppend(&reader4, &writer4));
  TASK_CHECK(writer4.bytes_ == initial);
  return 0;
}

// A block carrying BlockAdditions is an unsupported layout and must be
// rejected without writing anything.
int BlockAdditionsRejectedCase() {
  const uint64_t ms = kNsPerMs;
  VectorWriter writer;
  mkvmuxer::Segment segment;
  EXPECT(segment.Init(&writer));
  const uint64_t video = segment.AddVideoTrack(16, 16, 1);
  EXPECT_EQ(video, 1u);
  EXPECT(segment.CuesTrack(video));

  uint8_t data[6] = {1, 2, 3, 4, 5, 6};
  uint8_t additional[3] = {7, 8, 9};
  for (int i = 0; i < 3; ++i) {
    mkvmuxer::Frame frame;
    EXPECT(frame.Init(data, sizeof(data)));
    EXPECT(frame.AddAdditionalData(additional, sizeof(additional), 1));
    frame.set_track_number(video);
    frame.set_timestamp(static_cast<uint64_t>(i) * 40 * ms);
    frame.set_is_key(true);
    EXPECT(segment.AddGenericFrame(&frame));
  }
  EXPECT(segment.Finalize());

  const std::vector<uint8_t> original = writer.bytes_;
  VectorWriter reopen_writer;
  reopen_writer.bytes_ = original;
  reopen_writer.pos_ =
      static_cast<mkvmuxer::int64>(original.size());
  VectorReader reader(original);
  mkvmuxer::Segment reopened;
  TASK_CHECK(!reopened.InitAppend(&reader, &reopen_writer));
  TASK_CHECK(reopen_writer.bytes_ == original);
  return 0;
}

// InitAppend must never read frame payload bytes.
int NoPayloadReadCase() {
  const std::vector<uint8_t> initial =
      BuildInitial(true, false, 1000000ULL, 4, NULL);

  // Determine the block payload ranges through the parser, then construct a
  // reader that fails if any of those bytes are touched.
  std::unique_ptr<mkvparser::Segment> parsed(ParseWebM(initial));
  std::vector<std::pair<int64_t, int64_t> > blocked;
  for (const mkvparser::Cluster* cluster = parsed->GetFirst();
       cluster && !cluster->EOS();
       cluster = parsed->GetNext(cluster)) {
    const mkvparser::BlockEntry* entry = NULL;
    TASK_CHECK_EQ(cluster->GetFirst(entry), 0);
    while (entry && !entry->EOS()) {
      const mkvparser::Block* block = entry->GetBlock();
      TASK_CHECK_EQ(block->GetFrameCount(), 1);
      const mkvparser::Block::Frame& frame = block->GetFrame(0);
      blocked.push_back(std::make_pair(frame.pos, frame.pos + frame.len));
      const mkvparser::BlockEntry* next = NULL;
      TASK_CHECK_EQ(cluster->GetNext(entry, next), 0);
      entry = next;
    }
  }
  TASK_CHECK_GT(blocked.size(), 0u);

  PayloadBlockedReader reader(initial, blocked);
  VectorWriter writer;
  writer.bytes_ = initial;
  writer.pos_ = static_cast<mkvmuxer::int64>(initial.size());
  mkvmuxer::Segment segment;
  TASK_CHECK(segment.InitAppend(&reader, &writer));
  TASK_CHECK(segment.Finalize());
  TASK_CHECK(writer.bytes_ == initial);
  return 0;
}

// Once a real write fails, AddGenericFrame and Finalize keep failing and the
// session cannot be recovered.
int WriteFailureStateCase() {
  const std::vector<uint8_t> initial =
      BuildInitial(true, false, 1000000ULL, 3, NULL);

  VectorWriter writer;
  writer.bytes_ = initial;
  writer.pos_ = static_cast<mkvmuxer::int64>(initial.size());
  VectorReader reader(initial);
  mkvmuxer::Segment segment;
  TASK_CHECK(segment.InitAppend(&reader, &writer));

  // The first valid frame opens the new cluster before the failure is armed.
  mkvmuxer::Frame good;
  MakeFrame(&good, FrameSpec{1, 200 * kNsPerMs, 40 * kNsPerMs, true, 60});
  TASK_CHECK(segment.AddGenericFrame(&good));

  writer.FailWrites();
  mkvmuxer::Frame doomed;
  MakeFrame(&doomed, FrameSpec{1, 240 * kNsPerMs, 40 * kNsPerMs, true, 61});
  TASK_CHECK(!segment.AddGenericFrame(&doomed));

  // The session is now in the failure state permanently.
  mkvmuxer::Frame later;
  MakeFrame(&later, FrameSpec{1, 280 * kNsPerMs, 40 * kNsPerMs, true, 62});
  TASK_CHECK(!segment.AddGenericFrame(&later));
  TASK_CHECK(!segment.Finalize());
  return 0;
}

// A failure while flushing buffered audio at Finalize must be reported, not
// swallowed as a successful drain.
int AudioDrainFailureCase() {
  const std::vector<uint8_t> initial =
      BuildInitial(true, false, 1000000ULL, 3, NULL);

  VectorWriter writer;
  writer.bytes_ = initial;
  writer.pos_ = static_cast<mkvmuxer::int64>(initial.size());
  VectorReader reader(initial);
  mkvmuxer::Segment segment;
  TASK_CHECK(segment.InitAppend(&reader, &writer));

  // Stage audio before the key frame so it is held in the muxer queue.
  mkvmuxer::Frame audio;
  MakeFrame(&audio, FrameSpec{2, 205 * kNsPerMs, 20 * kNsPerMs, true, 70});
  TASK_CHECK(segment.AddGenericFrame(&audio));

  // The video key frame opens the cluster; arm failure before the Finalize
  // drain writes the buffered audio block.
  mkvmuxer::Frame video;
  MakeFrame(&video, FrameSpec{1, 200 * kNsPerMs, 40 * kNsPerMs, true, 71});
  TASK_CHECK(segment.AddGenericFrame(&video));

  writer.FailWrites();
  TASK_CHECK(!segment.Finalize());
  TASK_CHECK(!segment.AddGenericFrame(&video));
  return 0;
}

// A write failure while opening the first new cluster (which drains any staged
// audio through WriteFramesLessThan) must be surfaced and leave the session in
// the permanent failure state.
int ClusterOpenDrainFailureCase() {
  const std::vector<uint8_t> initial =
      BuildInitial(true, false, 1000000ULL, 3, NULL);

  VectorWriter writer;
  writer.bytes_ = initial;
  writer.pos_ = static_cast<mkvmuxer::int64>(initial.size());
  VectorReader reader(initial);
  mkvmuxer::Segment segment;
  TASK_CHECK(segment.InitAppend(&reader, &writer));

  // Stage audio; it is held until a video key frame opens a cluster.
  mkvmuxer::Frame audio;
  MakeFrame(&audio, FrameSpec{2, 205 * kNsPerMs, 20 * kNsPerMs, true, 80});
  TASK_CHECK(segment.AddGenericFrame(&audio));

  // Arm a write failure before the key frame triggers cluster creation and the
  // WriteFramesLessThan drain of the staged audio.
  writer.FailWrites();
  mkvmuxer::Frame video;
  MakeFrame(&video, FrameSpec{1, 200 * kNsPerMs, 40 * kNsPerMs, true, 81});
  TASK_CHECK(!segment.AddGenericFrame(&video));

  // The failure state sticks: no further frame or Finalize can succeed.
  TASK_CHECK(!segment.Finalize());
  return 0;
}

// Recovers codec private data, UIDs, default duration, codec delay and seek
// pre-roll and verifies them through the parser after reopening.
int TrackMetadataRecoveryCase() {
  const std::vector<uint8_t> initial =
      BuildInitial(true, true, 1000000ULL, 3, NULL);

  // Reopen and immediately finalize so the metadata is preserved untouched.
  VectorWriter writer;
  writer.bytes_ = initial;
  writer.pos_ = static_cast<mkvmuxer::int64>(initial.size());
  VectorReader reader(initial);
  mkvmuxer::Segment segment;
  TASK_CHECK(segment.InitAppend(&reader, &writer));

  const mkvmuxer::Track* video = segment.GetTrackByNumber(1);
  const mkvmuxer::Track* audio = segment.GetTrackByNumber(2);
  TASK_CHECK(video != NULL);
  TASK_CHECK(audio != NULL);
  TASK_CHECK_EQ(video->uid(), 0x1111111111111111ULL);
  TASK_CHECK_EQ(audio->uid(), 0x2222222222222222ULL);
  TASK_CHECK_EQ(video->default_duration(), 40 * kNsPerMs);
  TASK_CHECK_EQ(audio->default_duration(), 20 * kNsPerMs);
  TASK_CHECK_EQ(audio->codec_delay(), 6500000ULL);
  TASK_CHECK_EQ(audio->seek_pre_roll(), 80000ULL);
  TASK_CHECK(std::string(video->codec_id()) ==
             std::string(mkvmuxer::Tracks::kVp9CodecId));
  TASK_CHECK(std::string(audio->codec_id()) ==
             std::string(mkvmuxer::Tracks::kOpusCodecId));
  TASK_CHECK_EQ(video->codec_private_length(), 4u);

  TASK_CHECK(segment.Finalize());

  // Confirm through the independent parser as well.
  std::unique_ptr<mkvparser::Segment> parsed(ParseWebM(writer.bytes_));
  const mkvparser::Track* parsed_audio =
      parsed->GetTracks()->GetTrackByNumber(2);
  TASK_CHECK_EQ(parsed_audio->GetCodecDelay(), 6500000ULL);
  TASK_CHECK_EQ(parsed_audio->GetSeekPreRoll(), 80000ULL);
  size_t private_size = 0;
  TASK_CHECK(parsed_audio->GetCodecPrivate(private_size) != NULL);
  TASK_CHECK_EQ(private_size, 8u);
  return 0;
}

// Calling InitAppend twice or calling it on an initialized Segment must fail.
int InitializationGuardCase() {
  const std::vector<uint8_t> initial =
      BuildInitial(true, false, 1000000ULL, 3, NULL);

  VectorReader reader(initial);
  VectorWriter writer;
  writer.bytes_ = initial;
  writer.pos_ = static_cast<mkvmuxer::int64>(initial.size());
  mkvmuxer::Segment segment;
  TASK_CHECK(segment.InitAppend(&reader, &writer));
  TASK_CHECK(!segment.InitAppend(&reader, &writer));

  // A normally initialized Segment cannot be recovered.
  VectorWriter fresh_writer;
  mkvmuxer::Segment normal;
  TASK_CHECK(normal.Init(&fresh_writer));
  TASK_CHECK(!normal.InitAppend(&reader, &fresh_writer));
  return 0;
}

}  // namespace

// Exact symbols consumed by task_support/driver.c. The three group names are
// the Reopen, Timeline and Failure groups expected by the task harness.
const struct task_case task_cases[] = {
    // Reopen: recovery, byte preservation and multi-round continuation.
    {"Reopen", "no_change_reopen", ReopenNoChangeCase},
    {"Reopen", "staged_audio_no_cluster", ReopenStagedAudioNoClusterCase},
    {"Reopen", "two_append_rounds", ReopenAppendRoundsCase},
    {"Reopen", "non_default_timebase_video_only",
     NonDefaultTimebaseVideoOnlyCase},
    {"Reopen", "no_payload_reads", NoPayloadReadCase},
    {"Reopen", "block_additions_rejected", BlockAdditionsRejectedCase},
    {"Reopen", "track_metadata_recovery", TrackMetadataRecoveryCase},
    {"Reopen", "initialization_guard", InitializationGuardCase},
    // Timeline: timestamp rules, first-frame rules and cluster placement.
    {"Timeline", "reject_invalid_frames", RejectInvalidFrameCase},
    {"Timeline", "first_frame_must_be_video_key",
     FirstFrameMustBeVideoKeyCase},
    {"Timeline", "force_new_cluster", ForceNewClusterCase},
    // Failure: read/write failures and the permanent failure state.
    {"Failure", "read_failure_and_writer_position",
     ReadFailureAndWriterPositionCase},
    {"Failure", "write_failure_state", WriteFailureStateCase},
    {"Failure", "audio_drain_failure", AudioDrainFailureCase},
    {"Failure", "cluster_open_drain_failure", ClusterOpenDrainFailureCase},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
