// Copyright (c) 2026 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.

// Multi-track Cue mode tests. This file intentionally has no main(): the
// task_support driver supplies one and invokes task_cases(). All media is
// synthesized in C++; there is no GoogleTest or external clip dependency.

#include <cstdio>
#include <string>
#include <vector>

#include "common/webmids.h"
#include "mkvmuxer/mkvmuxer.h"
#include "mkvmuxer/mkvwriter.h"
#include "mkvparser/mkvparser.h"
#include "mkvparser/mkvreader.h"
#include "task_support/check.h"

namespace {

const uint64_t kMs = 1000000ULL;
const uint64_t kTimecodeScale = 2000000ULL;  // 2 ms, a non-default scale.
const uint64_t kVideo = 1;
const uint64_t kAudioA = 2;
const uint64_t kAudioB = 3;
const uint64_t kGeneric = 4;  // Existing but unconfigured, non A/V track.

// In-memory, seekable writer whose Write() can be made to fail on demand.
class MemoryWriter : public mkvmuxer::IMkvWriter {
 public:
  mkvmuxer::int32 Write(const void* buf, mkvmuxer::uint32 len) override {
    // The logical position always advances, even when the write is rejected,
    // so later seeks remain valid. Only successful bytes are retained.
    virtual_pos_ += len;
    if (fail_ && len >= fail_min_len_)
      return -1;
    const unsigned char* p = static_cast<const unsigned char*>(buf);
    bytes_.insert(bytes_.end(), p, p + len);
    return 0;
  }
  mkvmuxer::int64 Position() const override {
    return static_cast<mkvmuxer::int64>(virtual_pos_);
  }
  mkvmuxer::int32 Position(mkvmuxer::int64 position) override {
    if (position < 0)
      return -1;
    virtual_pos_ = position;
    if (static_cast<mkvmuxer::int64>(bytes_.size()) >= position)
      bytes_.resize(static_cast<size_t>(position));
    return 0;
  }
  bool Seekable() const override { return seekable_; }
  void ElementStartNotify(mkvmuxer::uint64, mkvmuxer::int64) override {}
  void StartFailing(mkvmuxer::uint32 min_len) {
    fail_ = true;
    fail_min_len_ = min_len;
  }
  void set_seekable(bool seekable) { seekable_ = seekable; }

 private:
  std::vector<unsigned char> bytes_;
  mkvmuxer::int64 virtual_pos_ = 0;
  bool fail_ = false;
  mkvmuxer::uint32 fail_min_len_ = 0;
  bool seekable_ = true;
};

struct PlannedFrame {
  uint64_t track;
  unsigned ms;
  bool key;
  unsigned char label;
};

void PutFrame(mkvmuxer::Segment& segment, const PlannedFrame& f) {
  unsigned char data[3] = {f.label, 0x53, 0x7e};
  TASK_CHECK(segment.AddFrame(data, sizeof(data), f.track,
                              f.ms * kMs, f.key));
}

// One CuePoint/TrackPosition triple read straight from the EBML Cues element,
// independently of libwebm's Cue parser.
struct RawCue {
  long long time;       // unscaled CueTime (timecode units)
  long long track;
  long long cluster_pos;
  long long block;
};

struct BlockInfo {
  long long cluster_pos;
  long block;
  long long track;
  long long time_ns;
  unsigned char label;
  bool key;
};

std::string TempPath(const char* name) {
  const char* dir = std::getenv("TMPDIR");
  std::string path = (dir && *dir) ? dir : "/tmp";
  path += '/';
  path += name;
  return path;
}

// Opens and fully loads a parser segment. Caller owns *segment.
void LoadSegment(const std::string& path, mkvparser::MkvReader& reader,
                  mkvparser::Segment*& segment) {
  TASK_CHECK(reader.Open(path.c_str()) == 0);
  long long pos = 0;
  mkvparser::EBMLHeader header;
  TASK_CHECK(header.Parse(&reader, pos) == 0);
  TASK_CHECK(mkvparser::Segment::CreateInstance(&reader, pos, segment) == 0);
  TASK_CHECK(segment != NULL);
  TASK_CHECK(segment->Load() >= 0);
}

// Enumerates every physical block, in file order. The (cluster position,
// physical block number) pair is the addressing used by CuePoints.
void EnumerateBlocks(mkvparser::Segment* segment, mkvparser::MkvReader& reader,
                     std::vector<BlockInfo>& blocks) {
  const mkvparser::Cluster* cluster = segment->GetFirst();
  TASK_CHECK(cluster != NULL);
  for (; cluster && !cluster->EOS();
       cluster = segment->GetNext(cluster)) {
    const mkvparser::BlockEntry* entry = NULL;
    TASK_CHECK(cluster->GetFirst(entry) == 0);
    long block = 0;
    while (entry && !entry->EOS()) {
      ++block;
      const mkvparser::Block* const b = entry->GetBlock();
      TASK_CHECK(b != NULL);
      TASK_CHECK(b->GetFrameCount() == 1);
      unsigned char payload[3] = {0, 0, 0};
      TASK_CHECK(b->GetFrame(0).Read(&reader, payload) == 0);
      BlockInfo info;
      info.cluster_pos = cluster->GetPosition();
      info.block = block;
      info.track = b->GetTrackNumber();
      info.time_ns = b->GetTime(cluster);
      info.label = payload[0];
      info.key = b->IsKey();
      blocks.push_back(info);
      TASK_CHECK(cluster->GetNext(entry, entry) == 0);
    }
  }
}

// Reads the Cues element directly as EBML, without using mkvparser::Cues.
void ReadRawCues(mkvparser::Segment* segment, mkvparser::MkvReader& reader,
                 std::vector<RawCue>& cues) {
  const mkvparser::Cues* const parsed_cues = segment->GetCues();
  TASK_CHECK(parsed_cues != NULL);
  long long pos = parsed_cues->m_start;
  const long long stop = pos + parsed_cues->m_size;

  while (pos < stop) {
    long id_len = 0;
    const long long id = mkvparser::ReadID(&reader, pos, id_len);
    TASK_CHECK(id == libwebm::kMkvCuePoint);
    pos += id_len;

    long size_len = 0;
    const long long point_size = mkvparser::ReadUInt(&reader, pos, size_len);
    TASK_CHECK(point_size >= 0);
    pos += size_len;

    const long long point_stop = pos + point_size;
    long long cue_time = -1;
    while (pos < point_stop) {
      long child_id_len = 0;
      const long long child_id =
          mkvparser::ReadID(&reader, pos, child_id_len);
      pos += child_id_len;

      long child_size_len = 0;
      const long long child_size =
          mkvparser::ReadUInt(&reader, pos, child_size_len);
      TASK_CHECK(child_size >= 0);
      pos += child_size_len;

      if (child_id == libwebm::kMkvCueTime) {
        cue_time = mkvparser::UnserializeUInt(&reader, pos, child_size);
        TASK_CHECK(cue_time >= 0);
      } else if (child_id == libwebm::kMkvCueTrackPositions) {
        TASK_CHECK(cue_time >= 0);
        long long tpos = pos;
        const long long tstop = pos + child_size;
        RawCue cue;
        cue.time = cue_time;
        cue.track = -1;
        cue.cluster_pos = -1;
        cue.block = 1;  // CueBlockNumber defaults to 1 when absent.
        while (tpos < tstop) {
          long tid_len = 0;
          const long long tid = mkvparser::ReadID(&reader, tpos, tid_len);
          tpos += tid_len;
          long tsize_len = 0;
          const long long tsize =
              mkvparser::ReadUInt(&reader, tpos, tsize_len);
          TASK_CHECK(tsize >= 0);
          tpos += tsize_len;
          if (tid == libwebm::kMkvCueTrack)
            cue.track = mkvparser::UnserializeUInt(&reader, tpos, tsize);
          else if (tid == libwebm::kMkvCueClusterPosition)
            cue.cluster_pos =
                mkvparser::UnserializeUInt(&reader, tpos, tsize);
          else if (tid == libwebm::kMkvCueBlockNumber)
            cue.block = mkvparser::UnserializeUInt(&reader, tpos, tsize);
          tpos += tsize;
        }
        TASK_CHECK(cue.track > 0);
        TASK_CHECK(cue.cluster_pos >= 0);
        TASK_CHECK(cue.block >= 1);
        cues.push_back(cue);
      }
      pos += child_size;
    }
    TASK_CHECK(pos == point_stop);
  }
}

const BlockInfo* FindBlock(const std::vector<BlockInfo>& blocks,
                           long long cluster_pos, long long block) {
  for (size_t i = 0; i < blocks.size(); ++i) {
    if (blocks[i].cluster_pos == cluster_pos && blocks[i].block == block)
      return &blocks[i];
  }
  return NULL;
}

// Builds the main multi-track file: two audio tracks, one video track, a
// non-default timecode scale, forced cluster cuts, trailing audio and an
// unconfigured generic track.
const std::vector<PlannedFrame> kSchedule = {
    {kVideo, 0, true, 10},   {kAudioA, 4, true, 20}, {kAudioB, 4, true, 30},
    {kGeneric, 6, true, 40}, {kVideo, 8, false, 11}, {kAudioA, 8, true, 21},
    // Force a new cluster around the second video key frame.
    {kVideo, 12, true, 12},  {kAudioA, 12, true, 22}, {kAudioB, 14, true, 31},
    {kVideo, 16, false, 13}, {kAudioA, 24, true, 23}, {kAudioB, 26, true, 32},
    // Force an audio-only tail cluster; the video track gets no cue there.
    {kAudioA, 30, true, 24}, {kAudioB, 32, true, 33}, {kAudioA, 34, true, 25},
};

const int kForceAfterFrame = 6;   // index of the 12 ms video key frame.
const int kForceAfterFrame2 = 12;  // index of the 30 ms audio frame.

void ConfigureAndFill(mkvmuxer::Segment& segment, bool output_cues = true) {
  TASK_CHECK(segment.AddVideoTrack(16, 16, static_cast<int>(kVideo)) ==
             kVideo);
  TASK_CHECK(segment.AddAudioTrack(48000, 1, static_cast<int>(kAudioA)) ==
             kAudioA);
  TASK_CHECK(segment.AddAudioTrack(48000, 1, static_cast<int>(kAudioB)) ==
             kAudioB);
  // A non-A/V track (Matroska subtitle type 17): give it a valid codec so the
  // header can write, but its type is neither audio nor video, so
  // SetCuesTracks rejects it.
  mkvmuxer::Track* const generic =
      segment.AddTrack(static_cast<int>(kGeneric));
  TASK_CHECK(generic != NULL);
  generic->set_type(17);
  generic->set_codec_id(mkvmuxer::Tracks::kWebVttSubtitlesId);

  const uint64_t tracks[] = {kVideo, kAudioA, kAudioB};
  TASK_CHECK(segment.SetCuesTracks(tracks, 3));
  segment.GetSegmentInfo()->set_timecode_scale(kTimecodeScale);
  segment.OutputCues(output_cues);

  for (size_t i = 0; i < kSchedule.size(); ++i) {
    if (i == static_cast<size_t>(kForceAfterFrame) ||
        i == static_cast<size_t>(kForceAfterFrame2))
      segment.ForceNewClusterOnNextFrame();
    PutFrame(segment, kSchedule[i]);
  }
}

void BuildMultiFile(const std::string& path, bool output_cues = true) {
  mkvmuxer::MkvWriter writer;
  TASK_CHECK(writer.Open(path.c_str()));
  mkvmuxer::Segment segment;
  TASK_CHECK(segment.Init(&writer));
  ConfigureAndFill(segment, output_cues);
  TASK_CHECK(segment.Finalize());
  writer.Close();
}

// Derives the expected cue set from the physical blocks by applying the
// registration rule independently: per cluster, audio registers its first
// block; video registers its first key frame. Returns one entry per
// (cluster, track) that should be cued.
struct ExpectedCue {
  long long cluster_pos;
  long long block;
  long long track;
  long long time_ns;
  unsigned char label;
};

void DeriveExpectedCues(const std::vector<BlockInfo>& blocks,
                        const uint64_t* cue_tracks, size_t cue_track_count,
                        std::vector<ExpectedCue>& expected) {
  std::vector<long long> cluster_order;
  for (size_t i = 0; i < blocks.size(); ++i) {
    if (cluster_order.empty() ||
        cluster_order.back() != blocks[i].cluster_pos)
      cluster_order.push_back(blocks[i].cluster_pos);
  }
  for (size_t c = 0; c < cluster_order.size(); ++c) {
    const long long cluster_pos = cluster_order[c];
    for (size_t t = 0; t < cue_track_count; ++t) {
      const long long track = static_cast<long long>(cue_tracks[t]);
      const BlockInfo* chosen = NULL;
      for (size_t i = 0; i < blocks.size(); ++i) {
        const BlockInfo& info = blocks[i];
        if (info.cluster_pos != cluster_pos || info.track != track)
          continue;
        // Audio: first block. Video: first key frame.
        const bool locatable =
            (track == static_cast<long long>(kVideo)) ? info.key : true;
        if (locatable) {
          chosen = &info;
          break;
        }
      }
      if (chosen != NULL) {
        ExpectedCue e;
        e.cluster_pos = chosen->cluster_pos;
        e.block = chosen->block;
        e.track = chosen->track;
        e.time_ns = chosen->time_ns;
        e.label = chosen->label;
        expected.push_back(e);
      }
    }
  }
}

// Compares raw EBML cues against the rule-derived expectation and verifies
// every cue addresses the exact payload block it claims.
void VerifyCuesPointToPayload(mkvparser::Segment* segment,
                              mkvparser::MkvReader& reader,
                              const std::vector<BlockInfo>& blocks,
                              const std::vector<RawCue>& raw_cues) {
  const uint64_t cue_tracks[] = {kVideo, kAudioA, kAudioB};
  std::vector<ExpectedCue> expected;
  DeriveExpectedCues(blocks, cue_tracks, 3, expected);

  TASK_CHECK(raw_cues.size() == expected.size());

  // Same-tick multi-track points are expressed as separate CuePoints.
  for (size_t i = 0; i < raw_cues.size(); ++i) {
    const RawCue& rc = raw_cues[i];
    bool matched = false;
    for (size_t j = 0; j < expected.size(); ++j) {
      const ExpectedCue& e = expected[j];
      if (e.track != rc.track || e.cluster_pos != rc.cluster_pos ||
          e.block != rc.block)
        continue;
      matched = true;
      TASK_CHECK(rc.time * static_cast<long long>(kTimecodeScale) ==
                 e.time_ns);
      const BlockInfo* const target =
          FindBlock(blocks, rc.cluster_pos, rc.block);
      TASK_CHECK(target != NULL);
      TASK_CHECK(target->track == rc.track);
      TASK_CHECK(target->time_ns ==
                 rc.time * static_cast<long long>(kTimecodeScale));
      TASK_CHECK(target->label == e.label);
      break;
    }
    TASK_CHECK(matched);
  }

  // The cluster position must address a real Cluster element in the file.
  for (size_t i = 0; i < raw_cues.size(); ++i) {
    long id_len = 0;
    const long long absolute =
        segment->m_start + raw_cues[i].cluster_pos;
    const long long id =
        mkvparser::ReadID(&reader, absolute, id_len);
    TASK_CHECK(id == libwebm::kMkvCluster);
  }

  // Video cues never target a non-key frame.
  for (size_t i = 0; i < raw_cues.size(); ++i) {
    if (raw_cues[i].track != static_cast<long long>(kVideo))
      continue;
    const BlockInfo* const target =
        FindBlock(blocks, raw_cues[i].cluster_pos, raw_cues[i].block);
    TASK_CHECK(target != NULL && target->key);
  }
}

// Exercises mkvparser::Cues::Find per-track semantics against the raw cues.
void VerifyFindSemantics(mkvparser::Segment* segment,
                         const std::vector<RawCue>& raw_cues) {
  const mkvparser::Cues* const cues = segment->GetCues();
  TASK_CHECK(cues != NULL);
  while (!cues->DoneParsing())
    TASK_CHECK(cues->LoadCuePoint());

  const mkvparser::Tracks* const tracks = segment->GetTracks();
  const long long scale = segment->GetInfo()->GetTimeCodeScale();

  const long long query_tracks[] = {
      static_cast<long long>(kVideo), static_cast<long long>(kAudioA),
      static_cast<long long>(kAudioB)};
  for (size_t q = 0; q < 3; ++q) {
    const long long track_number = query_tracks[q];
    const mkvparser::Track* const track =
        tracks->GetTrackByNumber(track_number);
    TASK_CHECK(track != NULL);

    // Per-track cue points in file order.
    std::vector<RawCue> track_cues;
    for (size_t i = 0; i < raw_cues.size(); ++i) {
      if (raw_cues[i].track == track_number)
        track_cues.push_back(raw_cues[i]);
    }
    TASK_CHECK(!track_cues.empty());

    const long long first_ns = track_cues.front().time * scale;
    const long long last_ns = track_cues.back().time * scale;

    // A request before the track's first point returns the first point. For a
    // track whose first point sits at time 0 there is no non-negative earlier
    // time, so query 0 itself.
    const long long early_ns =
        (first_ns >= scale) ? (first_ns - scale) : 0;
    const mkvparser::CuePoint* cp = NULL;
    const mkvparser::CuePoint::TrackPosition* tp = NULL;
    TASK_CHECK(cues->Find(early_ns, track, cp, tp));
    TASK_CHECK(cp != NULL && tp != NULL);
    TASK_CHECK(tp->m_pos == track_cues.front().cluster_pos);
    TASK_CHECK(tp->m_block == track_cues.front().block);

    // Each exact cue time resolves to that cue, and a point belonging to
    // another track must not hide the requested track's predecessor.
    for (size_t i = 0; i < track_cues.size(); ++i) {
      const long long ns = track_cues[i].time * scale;
      cp = NULL;
      tp = NULL;
      TASK_CHECK(cues->Find(ns, track, cp, tp));
      TASK_CHECK(tp->m_pos == track_cues[i].cluster_pos);
      TASK_CHECK(tp->m_block == track_cues[i].block);

      // The block reached through the parser is genuinely on this track and
      // at the cue's scaled time.
      const mkvparser::BlockEntry* const entry = cues->GetBlock(cp, tp);
      TASK_CHECK(entry != NULL && !entry->EOS());
      TASK_CHECK(entry->GetBlock()->GetTrackNumber() == track_number);
      TASK_CHECK(entry->GetBlock()->GetTime(entry->GetCluster()) == ns);
    }

    // A request after the last point clamps to the last point.
    cp = NULL;
    tp = NULL;
    TASK_CHECK(cues->Find(last_ns + 100 * scale, track, cp, tp));
    TASK_CHECK(tp->m_pos == track_cues.back().cluster_pos);
    TASK_CHECK(tp->m_block == track_cues.back().block);

    // Midpoint requests return the nearest point not later than the request.
    if (track_cues.size() >= 2) {
      for (size_t i = 1; i < track_cues.size(); ++i) {
        const long long prev_ns = track_cues[i - 1].time * scale;
        const long long cur_ns = track_cues[i].time * scale;
        if (cur_ns <= prev_ns + scale)
          continue;
        const long long mid = prev_ns + scale;  // strictly between the two
        cp = NULL;
        tp = NULL;
        TASK_CHECK(cues->Find(mid, track, cp, tp));
        TASK_CHECK(tp->m_pos == track_cues[i - 1].cluster_pos);
        TASK_CHECK(tp->m_block == track_cues[i - 1].block);
      }
    }
  }

  // The unconfigured generic track has no points: Find must fail.
  const mkvparser::Track* const generic =
      tracks->GetTrackByNumber(static_cast<long>(kGeneric));
  TASK_CHECK(generic != NULL);
  const mkvparser::CuePoint* cp = NULL;
  const mkvparser::CuePoint::TrackPosition* tp = NULL;
  TASK_CHECK(!cues->Find(0, generic, cp, tp));

  // The original rejection conventions are preserved.
  TASK_CHECK(!cues->Find(-1, tracks->GetTrackByNumber(kVideo), cp, tp));
  TASK_CHECK(!cues->Find(0, NULL, cp, tp));
}

void TestMainFileRoundTrip() {
  const std::string path = TempPath("multicue_main.webm");
  BuildMultiFile(path);

  mkvparser::MkvReader reader;
  mkvparser::Segment* segment = NULL;
  LoadSegment(path, reader, segment);

  std::vector<BlockInfo> blocks;
  EnumerateBlocks(segment, reader, blocks);
  TASK_CHECK(blocks.size() == kSchedule.size());

  std::vector<RawCue> raw_cues;
  ReadRawCues(segment, reader, raw_cues);

  // 8 locatable (cluster, track) pairs; same-tick tracks stay separate.
  TASK_CHECK(raw_cues.size() == 8);

  // The two audio tracks share their first tick (4 ms -> timecode 2) but are
  // expressed as two distinct, independently locatable CuePoints.
  int same_tick_pairs = 0;
  for (size_t i = 0; i < raw_cues.size(); ++i) {
    for (size_t j = i + 1; j < raw_cues.size(); ++j) {
      if (raw_cues[i].time == raw_cues[j].time &&
          raw_cues[i].track != raw_cues[j].track)
        ++same_tick_pairs;
    }
  }
  TASK_CHECK(same_tick_pairs >= 1);
  bool found_audio_pair = false;
  for (size_t i = 0; i < raw_cues.size(); ++i) {
    for (size_t j = i + 1; j < raw_cues.size(); ++j) {
      if (raw_cues[i].time == 2 &&
          ((raw_cues[i].track == static_cast<long long>(kAudioA) &&
            raw_cues[j].track == static_cast<long long>(kAudioB)) ||
           (raw_cues[j].track == static_cast<long long>(kAudioA) &&
            raw_cues[i].track == static_cast<long long>(kAudioB))))
        found_audio_pair = true;
    }
  }
  TASK_CHECK(found_audio_pair);

  // CuePoints are emitted ascending by CueTime; equal times for separate
  // tracks may sit next to each other.
  for (size_t i = 1; i < raw_cues.size(); ++i)
    TASK_CHECK(raw_cues[i - 1].time <= raw_cues[i].time);

  // Per-track registration counts. The tail cluster is audio-only, so the
  // video track gets no cue there; each audio track cues once per cluster.
  size_t video_cues = 0, audio_a_cues = 0, audio_b_cues = 0, generic_cues = 0;
  for (size_t i = 0; i < raw_cues.size(); ++i) {
    if (raw_cues[i].track == static_cast<long long>(kVideo))
      ++video_cues;
    else if (raw_cues[i].track == static_cast<long long>(kAudioA))
      ++audio_a_cues;
    else if (raw_cues[i].track == static_cast<long long>(kAudioB))
      ++audio_b_cues;
    else if (raw_cues[i].track == static_cast<long long>(kGeneric))
      ++generic_cues;
  }
  TASK_CHECK(video_cues == 2);
  TASK_CHECK(audio_a_cues == 3);
  TASK_CHECK(audio_b_cues == 3);
  TASK_CHECK(generic_cues == 0);

  VerifyCuesPointToPayload(segment, reader, blocks, raw_cues);
  VerifyFindSemantics(segment, raw_cues);

  delete segment;
  reader.Close();
  std::remove(path.c_str());
}

void TestCuesMovedBeforeClusters() {
  const std::string src = TempPath("multicue_src.webm");
  const std::string dst = TempPath("multicue_moved.webm");

  // Produce the source file and keep the segment alive: the copy helper must
  // run against the same finalized segment that owns the cluster list.
  mkvmuxer::MkvWriter in;
  TASK_CHECK(in.Open(src.c_str()));
  mkvmuxer::Segment segment;
  TASK_CHECK(segment.Init(&in));
  ConfigureAndFill(segment);
  TASK_CHECK(segment.Finalize());
  in.Close();

  mkvparser::MkvReader reader;
  TASK_CHECK(reader.Open(src.c_str()) == 0);
  mkvmuxer::MkvWriter out;
  TASK_CHECK(out.Open(dst.c_str()));
  TASK_CHECK(segment.CopyAndMoveCuesBeforeClusters(&reader, &out));
  out.Close();
  reader.Close();

  mkvparser::MkvReader dst_reader;
  mkvparser::Segment* parsed = NULL;
  LoadSegment(dst, dst_reader, parsed);

  // The Cues element now sits before every cluster.
  const mkvparser::Cues* const cues = parsed->GetCues();
  TASK_CHECK(cues != NULL);
  const mkvparser::Cluster* const first_cluster = parsed->GetFirst();
  TASK_CHECK(first_cluster != NULL);
  const long long first_cluster_abs =
      parsed->m_start + first_cluster->GetPosition();
  TASK_CHECK(cues->m_element_start < first_cluster_abs);

  std::vector<BlockInfo> blocks;
  EnumerateBlocks(parsed, dst_reader, blocks);
  std::vector<RawCue> raw_cues;
  ReadRawCues(parsed, dst_reader, raw_cues);
  TASK_CHECK(raw_cues.size() == 8);

  // Every moved cue still resolves through the parser to the original
  // payload (matched by track, scaled time and label).
  while (!cues->DoneParsing())
    TASK_CHECK(cues->LoadCuePoint());
  for (size_t i = 0; i < raw_cues.size(); ++i) {
    const RawCue& rc = raw_cues[i];
    const BlockInfo* const target =
        FindBlock(blocks, rc.cluster_pos, rc.block);
    TASK_CHECK(target != NULL);
    TASK_CHECK(target->track == rc.track);
    TASK_CHECK(target->time_ns ==
               rc.time * static_cast<long long>(kTimecodeScale));

    const mkvparser::Track* const track =
        parsed->GetTracks()->GetTrackByNumber(
            static_cast<long>(rc.track));
    const mkvparser::CuePoint* cp = NULL;
    const mkvparser::CuePoint::TrackPosition* tp = NULL;
    TASK_CHECK(cues->Find(target->time_ns, track, cp, tp));
    const mkvparser::BlockEntry* const entry = cues->GetBlock(cp, tp);
    TASK_CHECK(entry != NULL && !entry->EOS());
    TASK_CHECK(entry->GetBlock()->GetTrackNumber() == rc.track);
  }

  delete parsed;
  dst_reader.Close();
  std::remove(src.c_str());
  std::remove(dst.c_str());
}

void TestConfigurationRejection() {
  // Null pointer and zero count.
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    TASK_CHECK(segment.Init(&writer));
    TASK_CHECK(segment.AddVideoTrack(16, 16, 1) == 1);
    TASK_CHECK(segment.AddAudioTrack(48000, 1, 2) == 2);
    const uint64_t good[] = {1, 2};
    TASK_CHECK(segment.SetCuesTracks(good, 2));
    TASK_CHECK(!segment.SetCuesTracks(NULL, 2));
    TASK_CHECK(!segment.SetCuesTracks(good, 0));
    // The good configuration survived the failed calls: a bad reconfiguration
    // must leave the existing one untouched.
    TASK_CHECK(!segment.SetCuesTracks(NULL, 0));
  }

  // Unknown track number and duplicates.
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    TASK_CHECK(segment.Init(&writer));
    TASK_CHECK(segment.AddVideoTrack(16, 16, 1) == 1);
    TASK_CHECK(segment.AddAudioTrack(48000, 1, 2) == 2);
    const uint64_t unknown[] = {1, 7};
    TASK_CHECK(!segment.SetCuesTracks(unknown, 2));
    const uint64_t dup[] = {1, 1};
    TASK_CHECK(!segment.SetCuesTracks(dup, 2));
    const uint64_t good[] = {1, 2};
    TASK_CHECK(segment.SetCuesTracks(good, 2));
    const uint64_t bad_after_good[] = {2, 9};
    TASK_CHECK(!segment.SetCuesTracks(bad_after_good, 2));
  }

  // Non audio/video track numbers are rejected.
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    TASK_CHECK(segment.Init(&writer));
    TASK_CHECK(segment.AddVideoTrack(16, 16, 1) == 1);
    TASK_CHECK(segment.AddTrack(4) != NULL);  // generic, type 0
    const uint64_t with_generic[] = {1, 4};
    TASK_CHECK(!segment.SetCuesTracks(with_generic, 2));
  }

  // Configuration after the header has been written is rejected.
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    TASK_CHECK(segment.Init(&writer));
    TASK_CHECK(segment.AddVideoTrack(16, 16, 1) == 1);
    TASK_CHECK(segment.AddAudioTrack(48000, 1, 2) == 2);
    unsigned char data[3] = {1, 2, 3};
    TASK_CHECK(segment.AddFrame(data, sizeof(data), 1, 0, true));
    const uint64_t tracks[] = {1, 2};
    TASK_CHECK(!segment.SetCuesTracks(tracks, 2));
  }

  // kLive mode is unsupported.
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    TASK_CHECK(segment.Init(&writer));
    TASK_CHECK(segment.AddVideoTrack(16, 16, 1) == 1);
    segment.set_mode(mkvmuxer::Segment::kLive);
    const uint64_t tracks[] = {1};
    TASK_CHECK(!segment.SetCuesTracks(tracks, 1));
  }

  // Chunking is unsupported.
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    TASK_CHECK(segment.Init(&writer));
    TASK_CHECK(segment.AddVideoTrack(16, 16, 1) == 1);
    const std::string base = TempPath("multicue_chunk");
    TASK_CHECK(segment.SetChunking(true, base.c_str()));
    const uint64_t tracks[] = {1};
    TASK_CHECK(!segment.SetCuesTracks(tracks, 1));
  }

  // AccurateClusterDuration is incompatible.
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    TASK_CHECK(segment.Init(&writer));
    TASK_CHECK(segment.AddVideoTrack(16, 16, 1) == 1);
    segment.AccurateClusterDuration(true);
    const uint64_t tracks[] = {1};
    TASK_CHECK(!segment.SetCuesTracks(tracks, 1));
  }

  // A non-seekable writer is unsupported.
  {
    MemoryWriter writer;
    writer.set_seekable(false);
    mkvmuxer::Segment segment;
    TASK_CHECK(segment.Init(&writer));
    TASK_CHECK(segment.AddVideoTrack(16, 16, 1) == 1);
    const uint64_t tracks[] = {1};
    TASK_CHECK(!segment.SetCuesTracks(tracks, 1));
  }
}

void TestRevertToSingleTrackMode() {
  const std::string path = TempPath("multicue_revert.webm");
  mkvmuxer::MkvWriter writer;
  TASK_CHECK(writer.Open(path.c_str()));
  mkvmuxer::Segment segment;
  TASK_CHECK(segment.Init(&writer));
  TASK_CHECK(segment.AddVideoTrack(16, 16, 1) == 1);
  TASK_CHECK(segment.AddAudioTrack(48000, 1, 2) == 2);
  const uint64_t multi[] = {1, 2};
  TASK_CHECK(segment.SetCuesTracks(multi, 2));
  // A successful CuesTrack() switches back to legacy single-track mode.
  TASK_CHECK(segment.CuesTrack(1));

  unsigned char data[3] = {7, 8, 9};
  TASK_CHECK(segment.AddFrame(data, sizeof(data), 1, 0, true));
  TASK_CHECK(segment.AddFrame(data, sizeof(data), 2, 1 * kMs, true));
  segment.ForceNewClusterOnNextFrame();
  TASK_CHECK(segment.AddFrame(data, sizeof(data), 1, 4 * kMs, true));
  TASK_CHECK(segment.Finalize());
  writer.Close();

  mkvparser::MkvReader reader;
  mkvparser::Segment* parsed = NULL;
  LoadSegment(path, reader, parsed);
  std::vector<RawCue> raw_cues;
  ReadRawCues(parsed, reader, raw_cues);
  TASK_CHECK(!raw_cues.empty());
  for (size_t i = 0; i < raw_cues.size(); ++i)
    TASK_CHECK(raw_cues[i].track == 1);  // only the video track is cued
  delete parsed;
  reader.Close();
  std::remove(path.c_str());
}

void TestOutputCuesDisabled() {
  const std::string path = TempPath("multicue_nocues.webm");
  BuildMultiFile(path, false);

  mkvparser::MkvReader reader;
  mkvparser::Segment* parsed = NULL;
  LoadSegment(path, reader, parsed);
  TASK_CHECK(parsed->GetCues() == NULL);  // no automatic index was written
  delete parsed;
  reader.Close();
  std::remove(path.c_str());
}

void TestLegacySingleTrackAndAccurateDuration() {
  const std::string path = TempPath("multicue_legacy.webm");
  mkvmuxer::MkvWriter writer;
  TASK_CHECK(writer.Open(path.c_str()));
  mkvmuxer::Segment segment;
  TASK_CHECK(segment.Init(&writer));
  TASK_CHECK(segment.AddVideoTrack(16, 16, 1) == 1);
  TASK_CHECK(segment.AddAudioTrack(48000, 1, 2) == 2);
  TASK_CHECK(segment.CuesTrack(1));
  segment.AccurateClusterDuration(true);  // legacy combination still allowed

  unsigned char data[3] = {3, 4, 5};
  TASK_CHECK(segment.AddFrame(data, sizeof(data), 1, 0, true));
  TASK_CHECK(segment.AddFrame(data, sizeof(data), 2, 2 * kMs, true));
  segment.ForceNewClusterOnNextFrame();
  TASK_CHECK(segment.AddFrame(data, sizeof(data), 1, 8 * kMs, true));
  TASK_CHECK(segment.Finalize());
  writer.Close();

  mkvparser::MkvReader reader;
  mkvparser::Segment* parsed = NULL;
  LoadSegment(path, reader, parsed);
  std::vector<RawCue> raw_cues;
  ReadRawCues(parsed, reader, raw_cues);
  TASK_CHECK(raw_cues.size() == 2);  // one cue per video key frame
  for (size_t i = 0; i < raw_cues.size(); ++i)
    TASK_CHECK(raw_cues[i].track == 1);
  delete parsed;
  reader.Close();
  std::remove(path.c_str());
}

void TestVideoNonKeyClusterHasNoCue() {
  const std::string path = TempPath("multicue_nonkey.webm");
  mkvmuxer::MkvWriter writer;
  TASK_CHECK(writer.Open(path.c_str()));
  mkvmuxer::Segment segment;
  TASK_CHECK(segment.Init(&writer));
  TASK_CHECK(segment.AddVideoTrack(16, 16, 1) == 1);
  TASK_CHECK(segment.AddAudioTrack(48000, 1, 2) == 2);
  const uint64_t tracks[] = {1, 2};
  TASK_CHECK(segment.SetCuesTracks(tracks, 2));

  unsigned char data[3] = {9, 9, 9};
  TASK_CHECK(segment.AddFrame(data, sizeof(data), 1, 0, true));  // key
  TASK_CHECK(segment.AddFrame(data, sizeof(data), 2, 2 * kMs, true));
  TASK_CHECK(segment.AddFrame(data, sizeof(data), 2, 4 * kMs, true));
  segment.ForceNewClusterOnNextFrame();
  // Two queued audio frames: one is dequeued into the first cluster, the other
  // opens the middle cluster, whose only video block is a non-key frame.
  TASK_CHECK(segment.AddFrame(data, sizeof(data), 2, 6 * kMs, true));
  TASK_CHECK(segment.AddFrame(data, sizeof(data), 1, 10 * kMs, false));
  // A later video key frame opens the third cluster.
  TASK_CHECK(segment.AddFrame(data, sizeof(data), 1, 16 * kMs, true));
  TASK_CHECK(segment.AddFrame(data, sizeof(data), 2, 18 * kMs, true));
  TASK_CHECK(segment.Finalize());
  writer.Close();

  mkvparser::MkvReader reader;
  mkvparser::Segment* parsed = NULL;
  LoadSegment(path, reader, parsed);

  std::vector<BlockInfo> blocks;
  EnumerateBlocks(parsed, reader, blocks);
  TASK_CHECK(blocks.size() == 7);

  std::vector<RawCue> raw_cues;
  ReadRawCues(parsed, reader, raw_cues);

  // Two video cues (first and third clusters), three audio cues (one per
  // cluster); the non-key-only middle cluster has no video cue.
  size_t video_cues = 0, audio_cues = 0;
  for (size_t i = 0; i < raw_cues.size(); ++i) {
    if (raw_cues[i].track == 1)
      ++video_cues;
    else if (raw_cues[i].track == 2)
      ++audio_cues;
  }
  TASK_CHECK(video_cues == 2);
  TASK_CHECK(audio_cues == 3);

  // Locate the middle cluster (the one holding the non-key video block) and
  // prove no video cue addresses it.
  long long middle_cluster_pos = -1;
  for (size_t i = 0; i < blocks.size(); ++i) {
    if (blocks[i].track == 1 && !blocks[i].key) {
      middle_cluster_pos = blocks[i].cluster_pos;
      break;
    }
  }
  TASK_CHECK(middle_cluster_pos >= 0);
  for (size_t i = 0; i < raw_cues.size(); ++i) {
    if (raw_cues[i].track == 1)
      TASK_CHECK(raw_cues[i].cluster_pos != middle_cluster_pos);
  }

  // Every video cue still lands on a key frame.
  for (size_t i = 0; i < raw_cues.size(); ++i) {
    if (raw_cues[i].track != 1)
      continue;
    const BlockInfo* const target =
        FindBlock(blocks, raw_cues[i].cluster_pos, raw_cues[i].block);
    TASK_CHECK(target != NULL && target->key);
  }

  delete parsed;
  reader.Close();
  std::remove(path.c_str());
}

void TestWriteFailures() {
  // Failure while cutting a cluster: AddFrame must return false.
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    TASK_CHECK(segment.Init(&writer));
    TASK_CHECK(segment.AddVideoTrack(16, 16, 1) == 1);
    TASK_CHECK(segment.AddAudioTrack(48000, 1, 2) == 2);
    TASK_CHECK(segment.AddAudioTrack(48000, 1, 3) == 3);
    const uint64_t tracks[] = {1, 2, 3};
    TASK_CHECK(segment.SetCuesTracks(tracks, 3));

    unsigned char data[3] = {1, 2, 3};
    TASK_CHECK(segment.AddFrame(data, sizeof(data), 1, 0, true));
    TASK_CHECK(segment.AddFrame(data, sizeof(data), 2, 2 * kMs, true));
    TASK_CHECK(segment.AddFrame(data, sizeof(data), 3, 3 * kMs, true));

    writer.StartFailing(0);
    segment.ForceNewClusterOnNextFrame();
    TASK_CHECK(!segment.AddFrame(data, sizeof(data), 1, 8 * kMs, true));
  }

  // Failure during the Finalize drain: Finalize must return false.
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    TASK_CHECK(segment.Init(&writer));
    TASK_CHECK(segment.AddVideoTrack(16, 16, 1) == 1);
    TASK_CHECK(segment.AddAudioTrack(48000, 1, 2) == 2);
    const uint64_t tracks[] = {1, 2};
    TASK_CHECK(segment.SetCuesTracks(tracks, 2));

    unsigned char data[3] = {1, 2, 3};
    TASK_CHECK(segment.AddFrame(data, sizeof(data), 1, 0, true));
    TASK_CHECK(segment.AddFrame(data, sizeof(data), 2, 2 * kMs, true));

    writer.StartFailing(0);
    TASK_CHECK(!segment.Finalize());
  }

  // The legacy single-track mode keeps its existing "keep going" convention
  // when a queued audio block cannot be written during Finalize. Use frames
  // larger than any EBML metadata write and fail only large writes, so the
  // block payload is dropped while cluster size, duration and seek head still
  // write.
  {
    MemoryWriter writer;
    mkvmuxer::Segment segment;
    TASK_CHECK(segment.Init(&writer));
    TASK_CHECK(segment.AddVideoTrack(16, 16, 1) == 1);
    TASK_CHECK(segment.AddAudioTrack(48000, 1, 2) == 2);
    TASK_CHECK(segment.CuesTrack(1));

    unsigned char data[100] = {0};
    TASK_CHECK(segment.AddFrame(data, sizeof(data), 1, 0, true));
    TASK_CHECK(segment.AddFrame(data, sizeof(data), 2, 2 * kMs, true));

    writer.StartFailing(50);  // only the 100-byte block payloads fail
    TASK_CHECK(segment.Finalize());
  }
}

}  // namespace

void task_cases(void) {
  TestMainFileRoundTrip();
  TestCuesMovedBeforeClusters();
  TestConfigurationRejection();
  TestRevertToSingleTrackMode();
  TestOutputCuesDisabled();
  TestLegacySingleTrackAndAccurateDuration();
  TestVideoNonKeyClusterHasNoCue();
  TestWriteFailures();
}
