// Multi-track cue mode tests for the muxer, plus per-track Cues::Find tests
// for the parser. No GoogleTest; uses task_support/check.h.

#include <stdint.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "mkvparser/mkvparser.h"
#include "mkvparser/mkvreader.h"
#include "mkvmuxer/mkvmuxer.h"
#include "mkvmuxer/mkvwriter.h"
#include "task_support/check.h"

namespace {

// ---------------------------------------------------------------------------
// Memory writer. Supports optional non-seekable mode and write-failure
// injection.
class MemWriter : public mkvmuxer::IMkvWriter {
 public:
  MemWriter() : seekable_(true), fail_next_writes_(0), pos_(0) {}

  void set_seekable(bool seekable) { seekable_ = seekable; }
  void FailNextWrites(int count) { fail_next_writes_ = count; }

  int Write(const void* buf, unsigned int len) override {
    if (fail_next_writes_ > 0) {
      --fail_next_writes_;
      return -1;
    }
    if (len > 0) {
      if (pos_ + len > data_.size())
        data_.resize(pos_ + len);
      if (buf != NULL)
        memcpy(data_.data() + pos_, buf, len);
      pos_ += len;
    }
    return 0;
  }

  long long Position() const override { return pos_; }

  int Position(long long position) override {
    if (!seekable_ || position < 0 ||
        position > static_cast<long long>(data_.size())) {
      return -1;
    }
    pos_ = position;
    return 0;
  }

  bool Seekable() const override { return seekable_; }
  void ElementStartNotify(uint64_t, long long) override {}

  const uint8_t* data() const { return data_.empty() ? NULL : &data_[0]; }
  long long size() const { return data_.size(); }

 private:
  std::vector<uint8_t> data_;
  bool seekable_;
  int fail_next_writes_;
  long long pos_;
};

// Memory reader.
class MemReader : public mkvparser::IMkvReader {
 public:
  MemReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  int Read(long long position, long length, unsigned char* buffer) override {
    if (position < 0 || length < 0 ||
        static_cast<uint64_t>(position) + static_cast<uint64_t>(length) >
            size_) {
      return -1;
    }
    if (length > 0)
      memcpy(buffer, data_ + position, length);
    return 0;
  }

  int Length(long long* total, long long* available) override {
    if (total != NULL)
      *total = size_;
    if (available != NULL)
      *available = size_;
    return 0;
  }

 private:
  const uint8_t* data_;
  size_t size_;
};

// ---------------------------------------------------------------------------
// Minimal independent EBML reader used to inspect Cues and resolve the blocks
// they point to, without relying on mkvparser's CuePoint logic.

// Reads an EBML variable-length integer at |pos|. When |keep_marker| is true
// the length descriptor bits are kept (element IDs); otherwise they are
// stripped (data sizes). Returns the decoded value, or -1 on error. On
// success |*len| receives the byte length of the integer.
long long ReadEbmlVint(const uint8_t* data, size_t size, size_t pos,
                       bool keep_marker, int* len) {
  if (pos >= size)
    return -1;
  const uint8_t first = data[pos];
  if (first == 0)
    return -1;
  int n = 1;
  uint8_t mask = 0x80;
  while (!(first & mask)) {
    mask >>= 1;
    ++n;
  }
  if (pos + static_cast<size_t>(n) > size)
    return -1;
  uint64_t value = keep_marker ? first : (first & ~mask);
  for (int i = 1; i < n; ++i)
    value = (value << 8) | data[pos + i];
  *len = n;
  return static_cast<long long>(value);
}

uint64_t ReadUint(const uint8_t* data, size_t pos, size_t len) {
  uint64_t value = 0;
  for (size_t i = 0; i < len; ++i)
    value = (value << 8) | data[pos + i];
  return value;
}

struct EbmlElement {
  uint64_t id;
  size_t header_size;
  uint64_t payload_size;
  size_t payload_pos;
  bool unknown_size;
};

bool ReadEbmlElement(const uint8_t* data, size_t size, size_t pos,
                     EbmlElement* e) {
  int id_len = 0;
  const long long id = ReadEbmlVint(data, size, pos, true, &id_len);
  if (id < 0)
    return false;
  int size_len = 0;
  const long long payload =
      ReadEbmlVint(data, size, pos + id_len, false, &size_len);
  if (payload < 0)
    return false;
  e->id = static_cast<uint64_t>(id);
  e->header_size = static_cast<size_t>(id_len + size_len);
  const uint64_t unknown = (1ULL << (7 * size_len)) - 1;
  e->unknown_size = (static_cast<uint64_t>(payload) == unknown);
  e->payload_size = e->unknown_size ? 0 : static_cast<uint64_t>(payload);
  e->payload_pos = pos + id_len + size_len;
  return true;
}

struct CueRecord {
  uint64_t cue_time;
  uint64_t cue_track;
  uint64_t cue_cluster_pos;
  uint64_t cue_block_number;
};

struct FileLayout {
  size_t segment_payload;  // file offset of the segment payload
  bool has_cues;
  size_t cues_pos;  // file offset of the Cues element
  std::vector<size_t> clusters;  // file offsets of Cluster elements in order
};

// Walks the top-level elements of a segment, recording the Cues element and
// the Cluster element offsets.
bool ScanFile(const uint8_t* data, size_t size, FileLayout* layout) {
  EbmlElement e;
  size_t pos = 0;
  if (!ReadEbmlElement(data, size, pos, &e) || e.id != 0x1A45DFA3)  // EBML
    return false;
  pos += e.header_size + e.payload_size;
  if (!ReadEbmlElement(data, size, pos, &e) || e.id != 0x18538067)  // Segment
    return false;
  layout->segment_payload = e.payload_pos;
  const size_t end = pos + e.header_size + e.payload_size;
  pos = e.payload_pos;
  layout->has_cues = false;
  layout->cues_pos = 0;
  while (pos < end) {
    EbmlElement child;
    if (!ReadEbmlElement(data, size, pos, &child))
      return false;
    if (child.id == 0x1C53BB6B) {  // Cues
      layout->has_cues = true;
      layout->cues_pos = pos;
    } else if (child.id == 0x1F43B675) {  // Cluster
      layout->clusters.push_back(pos);
    }
    pos = child.payload_pos + child.payload_size;
  }
  return true;
}

// Parses the Cues element at |cues_pos| and appends one record per CuePoint.
bool ParseCues(const uint8_t* data, size_t size, size_t cues_pos,
               std::vector<CueRecord>* out) {
  EbmlElement cues;
  if (!ReadEbmlElement(data, size, cues_pos, &cues) || cues.id != 0x1C53BB6B)
    return false;
  size_t pos = cues.payload_pos;
  const size_t end = cues.payload_pos + cues.payload_size;
  while (pos < end) {
    EbmlElement cp;
    if (!ReadEbmlElement(data, size, pos, &cp) || cp.id != 0xBB)  // CuePoint
      return false;
    CueRecord rec;
    rec.cue_time = 0;
    rec.cue_track = 0;
    rec.cue_cluster_pos = 0;
    rec.cue_block_number = 1;  // default when CueBlockNumber is absent
    bool have_time = false;
    bool have_pos = false;
    size_t p = cp.payload_pos;
    const size_t cp_end = cp.payload_pos + cp.payload_size;
    while (p < cp_end) {
      EbmlElement child;
      if (!ReadEbmlElement(data, size, p, &child))
        return false;
      if (child.id == 0xB3) {  // CueTime
        rec.cue_time = ReadUint(data, child.payload_pos, child.payload_size);
        have_time = true;
      } else if (child.id == 0xB7) {  // CueTrackPositions
        size_t q = child.payload_pos;
        const size_t q_end = child.payload_pos + child.payload_size;
        while (q < q_end) {
          EbmlElement tp;
          if (!ReadEbmlElement(data, size, q, &tp))
            return false;
          if (tp.id == 0xF7)  // CueTrack
            rec.cue_track = ReadUint(data, tp.payload_pos, tp.payload_size);
          else if (tp.id == 0xF1) {  // CueClusterPosition
            rec.cue_cluster_pos =
                ReadUint(data, tp.payload_pos, tp.payload_size);
            have_pos = true;
          } else if (tp.id == 0x5378) {  // CueBlockNumber
            rec.cue_block_number =
                ReadUint(data, tp.payload_pos, tp.payload_size);
          }
          q = tp.payload_pos + tp.payload_size;
        }
      }
      p = child.payload_pos + child.payload_size;
    }
    if (!have_time || !have_pos)
      return false;
    out->push_back(rec);
    pos = cp.payload_pos + cp.payload_size;
  }
  return true;
}

struct BlockInfo {
  uint64_t track;
  long long abs_timecode;  // cluster timecode + relative timecode
  uint8_t label;           // first payload byte
};

// Appends every block of the cluster at |cluster_pos| to |out|. Returns false
// on a parse error.
bool DumpBlocks(const uint8_t* data, size_t size, size_t cluster_pos,
                uint64_t* cluster_timecode, std::vector<BlockInfo>* out) {
  EbmlElement cluster;
  if (!ReadEbmlElement(data, size, cluster_pos, &cluster) ||
      cluster.id != 0x1F43B675) {
    return false;
  }
  uint64_t timecode = 0;
  bool have_timecode = false;
  size_t pos = cluster.payload_pos;
  const size_t end = cluster.payload_pos + cluster.payload_size;
  while (pos < end) {
    EbmlElement child;
    if (!ReadEbmlElement(data, size, pos, &child))
      return false;
    if (child.id == 0xE7) {  // Timecode
      timecode = ReadUint(data, child.payload_pos, child.payload_size);
      have_timecode = true;
    } else if (child.id == 0xA3 || child.id == 0xA0) {  // SimpleBlock/BlockGroup
      size_t block_pos = child.payload_pos;
      size_t block_size = child.payload_size;
      if (child.id == 0xA0) {  // BlockGroup: descend to the Block child.
        bool found = false;
        size_t q = child.payload_pos;
        const size_t q_end = child.payload_pos + child.payload_size;
        while (q < q_end) {
          EbmlElement sub;
          if (!ReadEbmlElement(data, size, q, &sub))
            return false;
          if (sub.id == 0xA1) {  // Block
            block_pos = sub.payload_pos;
            block_size = sub.payload_size;
            found = true;
            break;
          }
          q = sub.payload_pos + sub.payload_size;
        }
        if (!found)
          return false;
      }
      int tlen = 0;
      const long long track = ReadEbmlVint(data, size, block_pos, false, &tlen);
      if (track < 0 || block_size < static_cast<size_t>(tlen) + 4)
        return false;
      const int rel = static_cast<int16_t>(
          (data[block_pos + tlen] << 8) | data[block_pos + tlen + 1]);
      const uint8_t flags = data[block_pos + tlen + 2];
      if (flags & 0x06)  // lacing is not expected
        return false;
      BlockInfo info;
      info.track = static_cast<uint64_t>(track);
      info.abs_timecode = static_cast<long long>(timecode) + rel;
      info.label = data[block_pos + tlen + 3];
      out->push_back(info);
    }
    pos = child.payload_pos + child.payload_size;
  }
  *cluster_timecode = timecode;
  return have_timecode;
}

// ---------------------------------------------------------------------------
// Test helpers.

void Put(mkvmuxer::Segment* seg, uint64_t track, uint64_t ms, uint8_t label,
         bool key) {
  uint8_t bytes[3] = {label, 0x53, 0x7e};
  TASK_CHECK(seg->AddFrame(bytes, sizeof bytes, track, ms * 1000000ULL, key));
}

struct ExpectedCue {
  uint64_t track;
  uint64_t timecode;  // CueTime in timecode units
  int cluster;        // 0-based cluster ordinal
  uint64_t block;     // CueBlockNumber
  uint8_t label;      // first payload byte of the referenced block
};

// Parses |reader| with the real parser and returns the loaded segment.
mkvparser::Segment* LoadSegment(MemReader* reader) {
  mkvparser::EBMLHeader header;
  long long pos = 0;
  TASK_CHECK(header.Parse(reader, pos) == 0);
  mkvparser::Segment* raw = NULL;
  TASK_CHECK(mkvparser::Segment::CreateInstance(reader, pos, raw) == 0);
  TASK_CHECK(raw != NULL);
  TASK_CHECK(raw->Load() >= 0);
  return raw;
}

// Expected cue points for the main scenario, in file order.
const ExpectedCue kExpected[] = {
    {1, 0, 0, 1, 11},  {2, 2, 0, 2, 21}, {3, 3, 0, 3, 31},
    {2,  7, 1, 2, 23}, {3, 7, 1, 3, 32}, {3, 7, 2, 1, 33},
    {1, 10, 2, 2, 14}, {2, 11, 2, 3, 24},
};
const size_t kCueCount = sizeof kExpected / sizeof kExpected[0];

void MuxMainScenario(mkvmuxer::Segment* seg) {
  Put(seg, 1, 0, 11, true);    // C0: V key @0
  Put(seg, 2, 4, 21, true);    // (staged audio)
  Put(seg, 3, 6, 31, true);    // (staged audio)
  Put(seg, 2, 8, 22, true);    // (staged audio)
  Put(seg, 1, 10, 12, false);  // C0: V non-key @10
  seg->ForceNewClusterOnNextFrame();
  Put(seg, 1, 12, 13, false);  // C1: V non-key @12 (no cue for track 1)
  Put(seg, 2, 14, 23, true);   // (staged audio)
  Put(seg, 3, 14, 32, true);   // (staged audio, same tick as A@14)
  Put(seg, 3, 14, 33, true);   // (staged audio, same tick again)
  Put(seg, 1, 20, 14, true);   // C2: V key @20
  Put(seg, 2, 22, 24, true);   // trailing audio
  Put(seg, 3, 22, 34, true);   // trailing audio
  TASK_CHECK(seg->Finalize());
}

// ---------------------------------------------------------------------------
// The main multi-track scenario: one video track (1) and two audio tracks
// (2, 3), non-default 2ms timecode scale, a forced cluster split, same-tick
// packets on different tracks, staged audio split across a cluster boundary,
// and trailing audio flushed by Finalize.
void MultiTrackCues() {
  MemWriter writer;
  mkvmuxer::Segment seg;
  TASK_CHECK(seg.Init(&writer));
  TASK_CHECK(seg.AddVideoTrack(16, 16, 1) == 1);
  TASK_CHECK(seg.AddAudioTrack(48000, 1, 2) == 2);
  TASK_CHECK(seg.AddAudioTrack(48000, 1, 3) == 3);
  seg.GetSegmentInfo()->set_timecode_scale(2000000);  // 2ms per tick

  // Configuration validation; failures must leave the configuration alone.
  const uint64_t good[] = {1, 2, 3};
  const uint64_t dup[] = {1, 1};
  const uint64_t unknown[] = {99};
  TASK_CHECK(!seg.SetCuesTracks(NULL, 1));
  TASK_CHECK(!seg.SetCuesTracks(good, 0));
  TASK_CHECK(!seg.SetCuesTracks(dup, 2));
  TASK_CHECK(!seg.SetCuesTracks(unknown, 1));
  TASK_CHECK(seg.SetCuesTracks(good, 3));
  TASK_CHECK(!seg.SetCuesTracks(dup, 2));  // config stays {1, 2, 3}

  MuxMainScenario(&seg);

  const uint8_t* data = writer.data();
  const size_t size = static_cast<size_t>(writer.size());
  TASK_CHECK(data != NULL && size > 0);

  // Expected block layout (12 blocks across 3 clusters).
  struct ExpBlock {
    uint64_t track;
    long long timecode;
    uint8_t label;
  };
  static const ExpBlock kBlocks[] = {
      {1, 0, 11}, {2, 2, 21}, {3, 3, 31}, {2, 4, 22}, {1, 5, 12},  // C0
      {1, 6, 13}, {2, 7, 23}, {3, 7, 32},                          // C1
      {3, 7, 33}, {1, 10, 14}, {2, 11, 24}, {3, 11, 34},           // C2
  };
  const size_t kBlockCount = sizeof kBlocks / sizeof kBlocks[0];

  // Independent EBML inspection.
  FileLayout layout;
  TASK_CHECK(ScanFile(data, size, &layout));
  TASK_CHECK(layout.has_cues);
  TASK_CHECK(layout.clusters.size() == 3);

  std::vector<CueRecord> cues;
  TASK_CHECK(ParseCues(data, size, layout.cues_pos, &cues));
  TASK_CHECK(cues.size() == kCueCount);

  // Dump every block and verify the muxing order.
  std::vector<BlockInfo> blocks;
  for (size_t i = 0; i < layout.clusters.size(); ++i) {
    uint64_t tc = 0;
    TASK_CHECK(DumpBlocks(data, size, layout.clusters[i], &tc, &blocks));
  }
  TASK_CHECK(blocks.size() == kBlockCount);
  for (size_t i = 0; i < kBlockCount; ++i) {
    TASK_CHECK(blocks[i].track == kBlocks[i].track);
    TASK_CHECK(blocks[i].abs_timecode == kBlocks[i].timecode);
    TASK_CHECK(blocks[i].label == kBlocks[i].label);
  }

  // Resolve each cue: CueClusterPosition must land on a Cluster element and
  // CueBlockNumber must select a block of CueTrack whose absolute timecode
  // equals CueTime, with the expected payload label.
  for (size_t i = 0; i < kCueCount; ++i) {
    const CueRecord& cue = cues[i];
    const ExpectedCue& exp = kExpected[i];
    TASK_CHECK(cue.cue_track == exp.track);
    TASK_CHECK(cue.cue_time == exp.timecode);
    TASK_CHECK(cue.cue_block_number == exp.block);
    // CueClusterPosition is relative to the segment payload.
    const size_t cluster_file_pos =
        layout.segment_payload + cue.cue_cluster_pos;
    TASK_CHECK(cluster_file_pos == layout.clusters[exp.cluster]);

    std::vector<BlockInfo> cluster_blocks;
    uint64_t tc = 0;
    TASK_CHECK(DumpBlocks(data, size, cluster_file_pos, &tc, &cluster_blocks));
    TASK_CHECK(cue.cue_block_number >= 1 &&
               cue.cue_block_number <= cluster_blocks.size());
    const BlockInfo& b = cluster_blocks[cue.cue_block_number - 1];
    TASK_CHECK(b.track == cue.cue_track);
    TASK_CHECK(b.abs_timecode == static_cast<long long>(cue.cue_time));
    TASK_CHECK(b.label == exp.label);
  }

  // Cross-check with the real parser.
  MemReader reader(data, size);
  std::unique_ptr<mkvparser::Segment> parsed(LoadSegment(&reader));
  const mkvparser::Cues* pcues = parsed->GetCues();
  TASK_CHECK(pcues != NULL);
  while (!pcues->DoneParsing())
    TASK_CHECK(pcues->LoadCuePoint());
  TASK_CHECK(pcues->GetCount() == static_cast<long>(kCueCount));

  const mkvparser::CuePoint* cp = pcues->GetFirst();
  for (size_t i = 0; i < kCueCount; ++i) {
    TASK_CHECK(cp != NULL);
    const ExpectedCue& exp = kExpected[i];
    TASK_CHECK(cp->GetTime(parsed.get()) ==
               static_cast<long long>(exp.timecode) * 2000000LL);
    const mkvparser::Track* track =
        parsed->GetTracks()->GetTrackByNumber(exp.track);
    TASK_CHECK(track != NULL);
    const mkvparser::CuePoint::TrackPosition* tp = cp->Find(track);
    TASK_CHECK(tp != NULL);
    TASK_CHECK(tp->m_block == static_cast<long long>(exp.block));
    TASK_CHECK(layout.segment_payload + tp->m_pos ==
               layout.clusters[exp.cluster]);
    cp = pcues->GetNext(cp);
  }
  TASK_CHECK(cp == NULL);

  // Per-track Find queries.
  const mkvparser::Track* t1 = parsed->GetTracks()->GetTrackByNumber(1);
  const mkvparser::Track* t2 = parsed->GetTracks()->GetTrackByNumber(2);
  const mkvparser::Track* t3 = parsed->GetTracks()->GetTrackByNumber(3);
  TASK_CHECK(t1 != NULL && t2 != NULL && t3 != NULL);

  const mkvparser::CuePoint* fcp = NULL;
  const mkvparser::CuePoint::TrackPosition* ftp = NULL;

  // Latest cue point not after the request.
  TASK_CHECK(pcues->Find(0, t1, fcp, ftp));
  TASK_CHECK(fcp->GetTime(parsed.get()) == 0 && ftp->m_block == 1);
  TASK_CHECK(pcues->Find(15000000, t1, fcp, ftp));  // between 0 and 20ms
  TASK_CHECK(fcp->GetTime(parsed.get()) == 0 && ftp->m_block == 1);
  TASK_CHECK(pcues->Find(25000000, t1, fcp, ftp));
  TASK_CHECK(fcp->GetTime(parsed.get()) == 20000000 && ftp->m_block == 2);

  // Request before the track's first cue point returns that first point.
  TASK_CHECK(pcues->Find(1000000, t2, fcp, ftp));
  TASK_CHECK(fcp->GetTime(parsed.get()) == 4000000 && ftp->m_block == 2);
  TASK_CHECK(pcues->Find(0, t2, fcp, ftp));
  TASK_CHECK(fcp->GetTime(parsed.get()) ==  4000000 && ftp->m_block == 2);

  // Same track with two cue points at 14ms: the earlier one in the file
  // (cluster 1, block 3) wins.
  TASK_CHECK(pcues->Find(14000000, t3, fcp, ftp));
  TASK_CHECK(fcp->GetTime(parsed.get()) == 14000000);
  TASK_CHECK(ftp->m_block == 3);
  TASK_CHECK(layout.segment_payload + ftp->m_pos == layout.clusters[1]);
  TASK_CHECK(pcues->Find(16000000, t3, fcp, ftp));
  TASK_CHECK(fcp->GetTime(parsed.get()) == 14000000 && ftp->m_block == 3);
  TASK_CHECK(pcues->Find(13000000, t3, fcp, ftp));
  TASK_CHECK(fcp->GetTime(parsed.get()) == 6000000 && ftp->m_block == 3);
  TASK_CHECK(pcues->Find(4000000, t3, fcp, ftp));  // before track 3's first
  TASK_CHECK(fcp->GetTime(parsed.get()) == 6000000 && ftp->m_block == 3);

  // Resolve one Find result to its block through the parser.
  TASK_CHECK(pcues->Find(25000000, t1, fcp, ftp));
  const mkvparser::BlockEntry* entry = pcues->GetBlock(fcp, ftp);
  TASK_CHECK(entry != NULL && !entry->EOS());
  TASK_CHECK(entry->GetBlock()->GetTrackNumber() == 1);

  // Negative time and invalid track keep the original rejection.
  TASK_CHECK(!pcues->Find(-1, t1, fcp, ftp));
  TASK_CHECK(!pcues->Find(0, NULL, fcp, ftp));
}

// ---------------------------------------------------------------------------
// Cues written before the clusters must still locate the same packets.
void MoveCuesBeforeClusters() {
  MemWriter writer;
  mkvmuxer::Segment seg;
  TASK_CHECK(seg.Init(&writer));
  TASK_CHECK(seg.AddVideoTrack(16, 16, 1) == 1);
  TASK_CHECK(seg.AddAudioTrack(48000, 1, 2) == 2);
  TASK_CHECK(seg.AddAudioTrack(48000, 1, 3) == 3);
  seg.GetSegmentInfo()->set_timecode_scale(2000000);
  const uint64_t tracks[] = {1, 2, 3};
  TASK_CHECK(seg.SetCuesTracks(tracks, 3));

  MuxMainScenario(&seg);

  // Copy the file with the Cues element moved before the Clusters.
  MemReader reader(writer.data(), static_cast<size_t>(writer.size()));
  MemWriter moved;
  TASK_CHECK(seg.CopyAndMoveCuesBeforeClusters(&reader, &moved));

  const uint8_t* data = moved.data();
  const size_t size = static_cast<size_t>(moved.size());

  FileLayout layout;
  TASK_CHECK(ScanFile(data, size, &layout));
  TASK_CHECK(layout.has_cues);
  TASK_CHECK(layout.clusters.size() == 3);
  // The Cues element now precedes the first Cluster.
  TASK_CHECK(layout.cues_pos < layout.clusters[0]);

  std::vector<CueRecord> cues;
  TASK_CHECK(ParseCues(data, size, layout.cues_pos, &cues));
  TASK_CHECK(cues.size() == kCueCount);
  for (size_t i = 0; i < kCueCount; ++i) {
    const CueRecord& cue = cues[i];
    const ExpectedCue& exp = kExpected[i];
    TASK_CHECK(cue.cue_track == exp.track);
    TASK_CHECK(cue.cue_time == exp.timecode);
    TASK_CHECK(cue.cue_block_number == exp.block);
    const size_t cluster_file_pos =
        layout.segment_payload + cue.cue_cluster_pos;
    TASK_CHECK(cluster_file_pos == layout.clusters[exp.cluster]);
    std::vector<BlockInfo> cluster_blocks;
    uint64_t tc = 0;
    TASK_CHECK(DumpBlocks(data, size, cluster_file_pos, &tc, &cluster_blocks));
    const BlockInfo& b = cluster_blocks[cue.cue_block_number - 1];
    TASK_CHECK(b.track == cue.cue_track);
    TASK_CHECK(b.abs_timecode == static_cast<long long>(cue.cue_time));
    TASK_CHECK(b.label == exp.label);
  }

  // The moved file must also parse cleanly and answer per-track queries.
  MemReader moved_reader(data, size);
  std::unique_ptr<mkvparser::Segment> parsed(LoadSegment(&moved_reader));
  const mkvparser::Cues* pcues = parsed->GetCues();
  TASK_CHECK(pcues != NULL);
  while (!pcues->DoneParsing())
    TASK_CHECK(pcues->LoadCuePoint());
  TASK_CHECK(pcues->GetCount() == static_cast<long>(kCueCount));
  const mkvparser::Track* t3 = parsed->GetTracks()->GetTrackByNumber(3);
  const mkvparser::CuePoint* fcp = NULL;
  const mkvparser::CuePoint::TrackPosition* ftp = NULL;
  TASK_CHECK(pcues->Find(16000000, t3, fcp, ftp));
  TASK_CHECK(fcp->GetTime(parsed.get()) == 14000000 && ftp->m_block == 3);
}

// ---------------------------------------------------------------------------
// Configuration validation: too-late and incompatible combinations.
void ValidateConfiguration() {
  // Configuring after the header has been written is too late.
  {
    MemWriter w;
    mkvmuxer::Segment seg;
    TASK_CHECK(seg.Init(&w));
    TASK_CHECK(seg.AddVideoTrack(16, 16, 1) == 1);
    uint8_t bytes[3] = {1, 2, 3};
    TASK_CHECK(seg.AddFrame(bytes, sizeof bytes, 1, 0, true));
    const uint64_t t[] = {1};
    TASK_CHECK(!seg.SetCuesTracks(t, 1));
    TASK_CHECK(seg.Finalize());
  }

  // kLive is not supported.
  {
    MemWriter w;
    mkvmuxer::Segment seg;
    TASK_CHECK(seg.Init(&w));
    TASK_CHECK(seg.AddVideoTrack(16, 16, 1) == 1);
    seg.set_mode(mkvmuxer::Segment::kLive);
    const uint64_t t[] = {1};
    TASK_CHECK(!seg.SetCuesTracks(t, 1));
  }
  {
    MemWriter w;
    mkvmuxer::Segment seg;
    TASK_CHECK(seg.Init(&w));
    TASK_CHECK(seg.AddVideoTrack(16, 16, 1) == 1);
    const uint64_t t[] = {1};
    TASK_CHECK(seg.SetCuesTracks(t, 1));
    seg.set_mode(mkvmuxer::Segment::kLive);
    uint8_t bytes[3] = {1, 2, 3};
    TASK_CHECK(!seg.AddFrame(bytes, sizeof bytes, 1, 0, true));
  }

  // AccurateClusterDuration is not supported.
  {
    MemWriter w;
    mkvmuxer::Segment seg;
    TASK_CHECK(seg.Init(&w));
    TASK_CHECK(seg.AddVideoTrack(16, 16, 1) == 1);
    seg.AccurateClusterDuration(true);
    const uint64_t t[] = {1};
    TASK_CHECK(!seg.SetCuesTracks(t, 1));
  }
  {
    MemWriter w;
    mkvmuxer::Segment seg;
    TASK_CHECK(seg.Init(&w));
    TASK_CHECK(seg.AddVideoTrack(16, 16, 1) == 1);
    const uint64_t t[] = {1};
    TASK_CHECK(seg.SetCuesTracks(t, 1));
    seg.AccurateClusterDuration(true);
    uint8_t bytes[3] = {1, 2, 3};
    TASK_CHECK(!seg.AddFrame(bytes, sizeof bytes, 1, 0, true));
  }

  // Chunking is not supported.
  {
    MemWriter w;
    mkvmuxer::Segment seg;
    TASK_CHECK(seg.Init(&w));
    TASK_CHECK(seg.AddVideoTrack(16, 16, 1) == 1);
    TASK_CHECK(seg.SetChunking(true, "mt_chunk_a"));
    const uint64_t t[] = {1};
    TASK_CHECK(!seg.SetCuesTracks(t, 1));
  }
  {
    MemWriter w;
    mkvmuxer::Segment seg;
    TASK_CHECK(seg.Init(&w));
    TASK_CHECK(seg.AddVideoTrack(16, 16, 1) == 1);
    const uint64_t t[] = {1};
    TASK_CHECK(seg.SetCuesTracks(t, 1));
    TASK_CHECK(seg.SetChunking(true, "mt_chunk_b"));
    uint8_t bytes[3] = {1, 2, 3};
    TASK_CHECK(!seg.AddFrame(bytes, sizeof bytes, 1, 0, true));
  }

  // A non-seekable writer is not supported.
  {
    MemWriter w;
    w.set_seekable(false);
    mkvmuxer::Segment seg;
    TASK_CHECK(seg.Init(&w));
    TASK_CHECK(seg.AddVideoTrack(16, 16, 1) == 1);
    const uint64_t t[] = {1};
    TASK_CHECK(!seg.SetCuesTracks(t, 1));
  }
}

// ---------------------------------------------------------------------------
// A successful CuesTrack() call after SetCuesTracks() switches back to
// single-track cue mode.
void CuesTrackAfterSetCuesTracks() {
  MemWriter w;
  mkvmuxer::Segment seg;
  TASK_CHECK(seg.Init(&w));
  TASK_CHECK(seg.AddVideoTrack(16, 16, 1) == 1);
  TASK_CHECK(seg.AddAudioTrack(48000, 1, 2) == 2);
  const uint64_t t[] = {1, 2};
  TASK_CHECK(seg.SetCuesTracks(t, 2));
  TASK_CHECK(seg.CuesTrack(2));  // back to single-track mode on track 2

  Put(&seg, 1, 0, 11, true);
  Put(&seg, 2, 4, 21, true);
  Put(&seg, 1, 8, 12, true);
  TASK_CHECK(seg.Finalize());

  FileLayout layout;
  TASK_CHECK(ScanFile(w.data(), static_cast<size_t>(w.size()), &layout));
  TASK_CHECK(layout.has_cues);
  std::vector<CueRecord> cues;
  TASK_CHECK(
      ParseCues(w.data(), static_cast<size_t>(w.size()), layout.cues_pos,
                &cues));
  TASK_CHECK(cues.size() == 1);
  TASK_CHECK(cues[0].cue_track == 2);
}

// ---------------------------------------------------------------------------
// Legacy single-track default behavior is preserved.
void LegacyDefaultCues() {
  MemWriter w;
  mkvmuxer::Segment seg;
  TASK_CHECK(seg.Init(&w));
  TASK_CHECK(seg.AddVideoTrack(16, 16, 1) == 1);
  TASK_CHECK(seg.AddAudioTrack(48000, 1, 2) == 2);

  Put(&seg, 1, 0, 11, true);
  Put(&seg, 2, 4, 21, true);
  Put(&seg, 1, 8, 12, true);
  TASK_CHECK(seg.Finalize());

  FileLayout layout;
  TASK_CHECK(ScanFile(w.data(), static_cast<size_t>(w.size()), &layout));
  TASK_CHECK(layout.has_cues);
  std::vector<CueRecord> cues;
  TASK_CHECK(
      ParseCues(w.data(), static_cast<size_t>(w.size()), layout.cues_pos,
                &cues));
  TASK_CHECK(cues.size() == 2);
  for (size_t i = 0; i < cues.size(); ++i)
    TASK_CHECK(cues[i].cue_track == 1);  // video track by default
}

// Legacy single-track mode keeps working with AccurateClusterDuration.
void LegacyAccurateClusterDuration() {
  MemWriter w;
  mkvmuxer::Segment seg;
  TASK_CHECK(seg.Init(&w));
  TASK_CHECK(seg.AddVideoTrack(16, 16, 1) == 1);
  TASK_CHECK(seg.AddAudioTrack(48000, 1, 2) == 2);
  seg.AccurateClusterDuration(true);

  Put(&seg, 1, 0, 11, true);
  Put(&seg, 2, 4, 21, true);
  Put(&seg, 1, 8, 12, true);
  TASK_CHECK(seg.Finalize());

  FileLayout layout;
  TASK_CHECK(ScanFile(w.data(), static_cast<size_t>(w.size()), &layout));
  TASK_CHECK(layout.has_cues);
  std::vector<CueRecord> cues;
  TASK_CHECK(
      ParseCues(w.data(), static_cast<size_t>(w.size()), layout.cues_pos,
                &cues));
  TASK_CHECK(cues.size() == 2);
  for (size_t i = 0; i < cues.size(); ++i)
    TASK_CHECK(cues[i].cue_track == 1);
}

// ---------------------------------------------------------------------------
// OutputCues(false) still disables the automatic index in multi-track mode.
void OutputCuesDisabled() {
  MemWriter w;
  mkvmuxer::Segment seg;
  TASK_CHECK(seg.Init(&w));
  TASK_CHECK(seg.AddVideoTrack(16, 16, 1) == 1);
  TASK_CHECK(seg.AddAudioTrack(48000, 1, 2) == 2);
  const uint64_t t[] = {1, 2};
  TASK_CHECK(seg.SetCuesTracks(t, 2));
  seg.OutputCues(false);

  Put(&seg, 1, 0, 11, true);
  Put(&seg, 2, 4, 21, true);
  Put(&seg, 1, 8, 12, true);
  TASK_CHECK(seg.Finalize());

  FileLayout layout;
  TASK_CHECK(ScanFile(w.data(), static_cast<size_t>(w.size()), &layout));
  TASK_CHECK(!layout.has_cues);

  MemReader reader(w.data(), static_cast<size_t>(w.size()));
  std::unique_ptr<mkvparser::Segment> parsed(LoadSegment(&reader));
  TASK_CHECK(parsed->GetCues() == NULL);
}

// ---------------------------------------------------------------------------
// Write failures in multi-track cue mode surface as AddFrame/Finalize
// failures and never produce cue points for unwritten blocks.
void WriteFailures() {
  // Direct write path: a failing write makes AddFrame return false, and no
  // cue point references the unwritten block.
  {
    MemWriter w;
    mkvmuxer::Segment seg;
    TASK_CHECK(seg.Init(&w));
    TASK_CHECK(seg.AddVideoTrack(16, 16, 1) == 1);
    TASK_CHECK(seg.AddAudioTrack(48000, 1, 2) == 2);
    const uint64_t t[] = {1, 2};
    TASK_CHECK(seg.SetCuesTracks(t, 2));
    uint8_t v[3] = {11, 0x53, 0x7e};
    TASK_CHECK(seg.AddFrame(v, sizeof v, 1, 0, true));
    w.FailNextWrites(1);
    uint8_t bad[3] = {12, 0x53, 0x7e};
    TASK_CHECK(!seg.AddFrame(bad, sizeof bad, 1, 2000000, false));
    TASK_CHECK(seg.Finalize());

    FileLayout layout;
    TASK_CHECK(ScanFile(w.data(), static_cast<size_t>(w.size()), &layout));
    TASK_CHECK(layout.has_cues);
    std::vector<CueRecord> cues;
    TASK_CHECK(
        ParseCues(w.data(), static_cast<size_t>(w.size()), layout.cues_pos,
                  &cues));
    TASK_CHECK(cues.size() == 1);  // only the successfully written key frame
    TASK_CHECK(cues[0].cue_track == 1 && cues[0].cue_time == 0);
  }

  // Staged-audio flush path (WriteFramesLessThan): multi-track mode fails
  // AddFrame; legacy single-track mode keeps its old lenient behavior.
  {
    MemWriter w;
    mkvmuxer::Segment seg;
    TASK_CHECK(seg.Init(&w));
    TASK_CHECK(seg.AddVideoTrack(16, 16, 1) == 1);
    TASK_CHECK(seg.AddAudioTrack(48000, 1, 2) == 2);
    const uint64_t t[] = {1, 2};
    TASK_CHECK(seg.SetCuesTracks(t, 2));
    uint8_t v[3] = {11, 0x53, 0x7e};
    uint8_t a1[3] = {21, 0x53, 0x7e};
    uint8_t a2[3] = {22, 0x53, 0x7e};
    uint8_t v2[3] = {12, 0x53, 0x7e};
    TASK_CHECK(seg.AddFrame(v, sizeof v, 1, 0, true));
    TASK_CHECK(seg.AddFrame(a1, sizeof a1, 2, 2000000, true));  // staged
    TASK_CHECK(seg.AddFrame(a2, sizeof a2, 2, 4000000, true));  // staged
    w.FailNextWrites(1);  // the next write (flushing a1) fails
    TASK_CHECK(!seg.AddFrame(v2, sizeof v2, 1, 8000000, true));
  }
  {
    MemWriter w;
    mkvmuxer::Segment seg;
    TASK_CHECK(seg.Init(&w));
    TASK_CHECK(seg.AddVideoTrack(16, 16, 1) == 1);
    TASK_CHECK(seg.AddAudioTrack(48000, 1, 2) == 2);
    uint8_t v[3] = {11, 0x53, 0x7e};
    uint8_t a1[3] = {21, 0x53, 0x7e};
    uint8_t a2[3] = {22, 0x53, 0x7e};
    uint8_t v2[3] = {12, 0x53, 0x7e};
    TASK_CHECK(seg.AddFrame(v, sizeof v, 1, 0, true));
    TASK_CHECK(seg.AddFrame(a1, sizeof a1, 2, 2000000, true));
    TASK_CHECK(seg.AddFrame(a2, sizeof a2, 2, 4000000, true));
    w.FailNextWrites(1);  // legacy mode swallows the staged-audio failure
    TASK_CHECK(seg.AddFrame(v2, sizeof v2, 1, 8000000, true));
  }

  // Finalize flush path: multi-track mode fails Finalize; legacy mode keeps
  // its old lenient behavior.
  {
    MemWriter w;
    mkvmuxer::Segment seg;
    TASK_CHECK(seg.Init(&w));
    TASK_CHECK(seg.AddVideoTrack(16, 16, 1) == 1);
    TASK_CHECK(seg.AddAudioTrack(48000, 1, 2) == 2);
    const uint64_t t[] = {1, 2};
    TASK_CHECK(seg.SetCuesTracks(t, 2));
    uint8_t v[3] = {11, 0x53, 0x7e};
    uint8_t a1[3] = {21, 0x53, 0x7e};
    TASK_CHECK(seg.AddFrame(v, sizeof v, 1, 0, true));
    TASK_CHECK(seg.AddFrame(a1, sizeof a1, 2, 4000000, true));  // staged
    w.FailNextWrites(1);
    TASK_CHECK(!seg.Finalize());
  }
  {
    MemWriter w;
    mkvmuxer::Segment seg;
    TASK_CHECK(seg.Init(&w));
    TASK_CHECK(seg.AddVideoTrack(16, 16, 1) == 1);
    TASK_CHECK(seg.AddAudioTrack(48000, 1, 2) == 2);
    uint8_t v[3] = {11, 0x53, 0x7e};
    uint8_t a1[3] = {21, 0x53, 0x7e};
    TASK_CHECK(seg.AddFrame(v, sizeof v, 1, 0, true));
    TASK_CHECK(seg.AddFrame(a1, sizeof a1, 2, 4000000, true));
    w.FailNextWrites(1);  // legacy mode swallows the failure
    TASK_CHECK(seg.Finalize());
  }
}

}  // namespace

void task_cases(void) {
  MultiTrackCues();
  MoveCuesBeforeClusters();
  ValidateConfiguration();
  CuesTrackAfterSetCuesTracks();
  LegacyDefaultCues();
  LegacyAccurateClusterDuration();
  OutputCuesDisabled();
  WriteFailures();
}
