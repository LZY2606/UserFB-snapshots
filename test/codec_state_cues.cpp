// Regression tests for codec-state-aware cue generation (KaxCues
// codec state tracking) and for KaxCues::GetDecodePosition.
//
// The cases only use the C++ standard library, libebml and libmatroska
// itself; no external media tools are involved.

#include "task_support/check.h"

#include <cstring>
#include <memory>
#include <stdexcept>

#include "matroska/KaxSegment.h"
#include "matroska/KaxTracks.h"
#include "matroska/KaxCluster.h"
#include "matroska/KaxCues.h"
#include "matroska/KaxCuesData.h"
#include "matroska/KaxBlockData.h"
#include "matroska/KaxSemantic.h"
#include "ebml/EbmlStream.h"
#include "ebml/EbmlVoid.h"
#include "ebml/MemIOCallback.h"

using namespace libebml;
using namespace libmatroska;

namespace {

const uint64 SCALE = 1000000; // 1 ms ticks, timecodes below are in nanoseconds

// codec state payloads written into KaxCodecState elements
static const binary STATE_1[]  = {0xC0, 0xDE, 0x01};
static const binary STATE_1U[] = {0xC0, 0xDE, 0x09, 0x55};
static const binary STATE_1B[] = {0xC0, 0xDE, 0x02, 0x77};
static const binary STATE_2[]  = {0xBA, 0x5E};

// frame payloads (kept alive for the whole test run)
static binary FRAME_A[] = {'A', '1'};
static binary FRAME_B[] = {'B', '1'};
static binary FRAME_C[] = {'C', '1'};
static binary FRAME_D[] = {'D', '1'};
static binary FRAME_E[] = {'E', '1'};
static binary FRAME_F[] = {'F', '1'};
static binary FRAME_G[] = {'G', '1'};
static binary FRAME_H[] = {'H', '1'};
static binary FRAME_I[] = {'I', '1'};
static binary FRAME_J[] = {'J', '1'};
static binary FRAME_K[] = {'K', '1'};
static binary FRAME_L[] = {'L', '1'};

DataBuffer & Wrap(binary *data, uint32 size)
{
  return *new DataBuffer(data, size); // leaked on purpose, lives long enough
}

// Remove the block elements from a cluster without deleting them: the
// KaxBlockBlob wrappers own them and will delete them themselves.
void DetachBlocks(KaxCluster & cluster)
{
  for (size_t i = cluster.ListSize(); i-- > 0;) {
    if (EbmlId(*cluster[i]) == EBML_ID(KaxBlockGroup) || EbmlId(*cluster[i]) == EBML_ID(KaxSimpleBlock))
      cluster.Remove(i);
  }
}

// A two-track file exercising both write paths (direct KaxBlockGroup and
// KaxBlockBlob), codec state changes in non-indexed BlockGroups, lacing and
// several blocks sharing the same tick.
//
// Cluster 1 (direct KaxBlockGroup path), physical order:
//   #1 g1  track1 @ 0ms  CodecState S1     (indexed)
//   #2 gL  track1 @ 2ms  EBML-laced, 2 frames, counts as one block (indexed)
//   #3 g2  track2 @ 1ms                     (indexed, no state seen for track2)
//   #4 g3  track1 @ 10ms                    (indexed, physically before S1U/S1B)
//   #5 gU  track1 @ 8ms  CodecState S1U     (NOT indexed: state carriers need no cue)
// Cluster 2 (KaxBlockBlob path), physical order:
//   #1 b1  track1 @ 5ms  SimpleBlock        (indexed, registered twice)
//   #2 b1b track1 @ 5ms  SimpleBlock        (indexed, same tick as b1)
//   #3 b2  track1 @ 6ms  CodecState S1B     (indexed)
//   #4 b3  track2 @ 20ms CodecState S2      (indexed)
//   #5 b4  track1 @ 25ms SimpleBlock        (indexed, must not inherit S2)
//   #6 b5  track2 @ 30ms SimpleBlock        (indexed, inherits S2)
struct DemoFile {
  KaxSegment segment;
  KaxTrackEntry track1, track2;
  KaxCluster cluster1, cluster2;
  KaxCues cues;

  KaxBlockGroup *g1{nullptr}, *gL{nullptr}, *g2{nullptr}, *g3{nullptr}, *gU{nullptr};
  KaxCodecState *stateS1{nullptr}, *stateS1U{nullptr}, *stateS1B{nullptr}, *stateS2{nullptr};

  KaxBlockBlob r1, rL, r2, r3;           // wrappers for the direct groups
  KaxBlockBlob b1, b1b, b2, b3, b4, b5;  // blob-path blocks

  explicit DemoFile(bool tracking)
    : r1(BLOCK_BLOB_NO_SIMPLE), rL(BLOCK_BLOB_NO_SIMPLE)
    , r2(BLOCK_BLOB_NO_SIMPLE), r3(BLOCK_BLOB_NO_SIMPLE)
    , b1(BLOCK_BLOB_ALWAYS_SIMPLE), b1b(BLOCK_BLOB_ALWAYS_SIMPLE)
    , b2(BLOCK_BLOB_NO_SIMPLE), b3(BLOCK_BLOB_NO_SIMPLE)
    , b4(BLOCK_BLOB_ALWAYS_SIMPLE), b5(BLOCK_BLOB_ALWAYS_SIMPLE)
  {
    track1.SetGlobalTimecodeScale(SCALE);
    GetChild<KaxTrackNumber>(track1).SetValue(1);
    track1.EnableLacing(true);
    track2.SetGlobalTimecodeScale(SCALE);
    GetChild<KaxTrackNumber>(track2).SetValue(2);
    track2.EnableLacing(false);

    cues.SetGlobalTimecodeScale(SCALE);
    if (tracking)
      cues.SetCodecStateTracking(true);

    cluster1.SetParent(segment);
    cluster1.InitTimecode(0, SCALE);
    cluster2.SetParent(segment);
    cluster2.InitTimecode(0, SCALE);

    KaxBlockGroup *tmp = nullptr;
    cluster1.AddFrame(track1, 0 * SCALE, Wrap(FRAME_A, sizeof(FRAME_A)), g1, LACING_NONE);
    cluster1.AddFrame(track1, 2 * SCALE, Wrap(FRAME_B, sizeof(FRAME_B)), gL, LACING_EBML);
    cluster1.AddFrame(track1, 4 * SCALE, Wrap(FRAME_C, sizeof(FRAME_C)), tmp, LACING_EBML); // joins gL
    cluster1.AddFrame(track2, 1 * SCALE, Wrap(FRAME_D, sizeof(FRAME_D)), g2, LACING_NONE);
    cluster1.AddFrame(track1, 10 * SCALE, Wrap(FRAME_E, sizeof(FRAME_E)), g3, LACING_NONE);
    cluster1.AddFrame(track1, 8 * SCALE, Wrap(FRAME_L, sizeof(FRAME_L)), gU, LACING_NONE);

    stateS1 = &GetChild<KaxCodecState>(*g1);
    stateS1->CopyBuffer(STATE_1, sizeof(STATE_1));
    // gU changes the codec state of track 1 but is deliberately never
    // registered as an index point
    stateS1U = &GetChild<KaxCodecState>(*gU);
    stateS1U->CopyBuffer(STATE_1U, sizeof(STATE_1U));

    b1.SetParent(cluster2);
    b1.AddFrameAuto(track1, 5 * SCALE, Wrap(FRAME_F, sizeof(FRAME_F)), LACING_NONE);
    b1b.SetParent(cluster2);
    b1b.AddFrameAuto(track1, 5 * SCALE, Wrap(FRAME_G, sizeof(FRAME_G)), LACING_NONE);
    b2.SetParent(cluster2);
    b2.AddFrameAuto(track1, 6 * SCALE, Wrap(FRAME_H, sizeof(FRAME_H)), LACING_NONE);
    stateS1B = &GetChild<KaxCodecState>(static_cast<KaxBlockGroup &>(b2));
    stateS1B->CopyBuffer(STATE_1B, sizeof(STATE_1B));
    b3.SetParent(cluster2);
    b3.AddFrameAuto(track2, 20 * SCALE, Wrap(FRAME_I, sizeof(FRAME_I)), LACING_NONE);
    stateS2 = &GetChild<KaxCodecState>(static_cast<KaxBlockGroup &>(b3));
    stateS2->CopyBuffer(STATE_2, sizeof(STATE_2));
    b4.SetParent(cluster2);
    b4.AddFrameAuto(track1, 25 * SCALE, Wrap(FRAME_J, sizeof(FRAME_J)), LACING_NONE);
    b5.SetParent(cluster2);
    b5.AddFrameAuto(track2, 30 * SCALE, Wrap(FRAME_K, sizeof(FRAME_K)), LACING_NONE);

    // register the index points before any rendering happens
    r1.SetBlockGroup(*g1);
    rL.SetBlockGroup(*gL);
    r2.SetBlockGroup(*g2);
    r3.SetBlockGroup(*g3);
    cues.AddBlockBlob(r1);
    cues.AddBlockBlob(rL);
    cues.AddBlockBlob(r2);
    cues.AddBlockBlob(r3);
    cues.AddBlockBlob(b1);
    cues.AddBlockBlob(b1); // same block registered twice: still a single cue
    cues.AddBlockBlob(b1b);
    cues.AddBlockBlob(b2);
    cues.AddBlockBlob(b3);
    cues.AddBlockBlob(b4);
    cues.AddBlockBlob(b5);
  }

  ~DemoFile()
  {
    DetachBlocks(cluster1);
    DetachBlocks(cluster2);
  }

  void AddCluster2Blobs()
  {
    cluster2.AddBlockBlob(&b1);
    cluster2.AddBlockBlob(&b1b);
    cluster2.AddBlockBlob(&b2);
    cluster2.AddBlockBlob(&b3);
    cluster2.AddBlockBlob(&b4);
    cluster2.AddBlockBlob(&b5);
  }

  void RenderClusters(MemIOCallback & output)
  {
    AddCluster2Blobs();
    cluster1.Render(output, cues, false);
    cluster2.Render(output, cues, false);
  }

  void RenderCluster2(MemIOCallback & output)
  {
    AddCluster2Blobs();
    cluster2.Render(output, cues, false);
  }
};

struct CueData {
  bool found{false};
  uint64 cluster{0};
  bool hasBlock{false};
  uint64 block{0};
  bool hasState{false};   // a nonzero CueCodecState is present
  uint64 state{0};        // effective CueCodecState value (0 means format default)
};

// The effective CueCodecState of a positions element. The format default is
// 0 and libebml keeps a mandatory 0-valued element in memory, so look at all
// of them and take the nonzero one (there is at most one).
uint64 EffectiveState(const KaxCueTrackPositions & pos, bool & hasNonzero)
{
  uint64 result = 0;
  hasNonzero = false;
  for (const auto elt : pos) {
    if (EbmlId(*elt) == EBML_ID(KaxCueCodecState)) {
      const uint64 value = static_cast<uint64>(*static_cast<const KaxCueCodecState *>(elt));
      if (value != 0) {
        result = value;
        hasNonzero = true;
      }
    }
  }
  return result;
}

// Find the cue entry for a track/timecode (in ms); when blockNum >= 0 the
// CueBlockNumber must match as well.
CueData FindCue(const KaxCues & cues, uint64 track, uint64 timeMs, int blockNum)
{
  CueData result;
  for (const auto elt : cues) {
    if (EbmlId(*elt) != EBML_ID(KaxCuePoint))
      continue;
    const auto & point = *static_cast<const KaxCuePoint *>(elt);
    const auto time = static_cast<const KaxCueTime *>(point.FindFirstElt(EBML_INFO(KaxCueTime)));
    if (time == nullptr || static_cast<uint64>(*time) != timeMs)
      continue;
    auto pos = static_cast<const KaxCueTrackPositions *>(point.FindFirstElt(EBML_INFO(KaxCueTrackPositions)));
    while (pos != nullptr) {
      if (pos->TrackNumber() == track) {
        const auto blockElt = static_cast<const KaxCueBlockNumber *>(pos->FindFirstElt(EBML_INFO(KaxCueBlockNumber)));
        const uint64 block = blockElt != nullptr ? static_cast<uint64>(*blockElt) : 0;
        if (blockNum < 0 || block == static_cast<uint64>(blockNum)) {
          result.found = true;
          result.cluster = pos->ClusterPosition();
          result.hasBlock = blockElt != nullptr;
          result.block = block;
          result.state = EffectiveState(*pos, result.hasState);
          return result;
        }
      }
      pos = static_cast<const KaxCueTrackPositions *>(point.FindNextElt(*pos));
    }
  }
  return result;
}

uint64 BlockOf(const KaxCueTrackPositions * pos)
{
  const auto elt = static_cast<const KaxCueBlockNumber *>(pos->FindFirstElt(EBML_INFO(KaxCueBlockNumber)));
  return elt != nullptr ? static_cast<uint64>(*elt) : 0;
}

uint64 StateOf(const KaxCueTrackPositions * pos)
{
  bool hasNonzero = false;
  return EffectiveState(*pos, hasNonzero); // missing means 0
}

// Independently verify that a CueCodecState value really points at a
// KaxCodecState element holding the expected bytes in the written file.
bool CheckStateBytes(MemIOCallback & output, const KaxSegment & segment, uint64 relativePos,
                     const binary * expected, size_t length)
{
  output.setFilePointer(segment.GetGlobalPosition(relativePos));
  binary head[2];
  if (output.read(head, 2) != 2)
    return false;
  if (head[0] != 0xA4) // KaxCodecState EBML ID
    return false;
  if (head[1] != (0x80 | length)) // short-form element size
    return false;
  binary data[16];
  if (length > sizeof(data) || output.read(data, length) != length)
    return false;
  return std::memcmp(data, expected, length) == 0;
}

// An output that starts failing after a byte budget, to simulate a broken
// sink in the middle of a Cluster.Render().
class FailingIO : public IOCallback {
  public:
    FailingIO(MemIOCallback & sink, size_t budget) : mSink(sink), mBudget(budget) {}

    uint32 read(void *, size_t) override { return 0; }
    void setFilePointer(int64 offset, seek_mode mode) override { mSink.setFilePointer(offset, mode); }
    size_t write(const void * buffer, size_t size) override
    {
      if (size > mBudget) {
        mBudget = 0;
        throw std::runtime_error("simulated output failure");
      }
      mBudget -= size;
      return mSink.write(buffer, size);
    }
    uint64 getFilePointer() override { return mSink.getFilePointer(); }
    void close() override {}

  private:
    MemIOCallback & mSink;
    size_t mBudget;
};

std::unique_ptr<KaxCues> ParseCues(MemIOCallback & buffer)
{
  buffer.setFilePointer(0);
  EbmlStream stream(buffer);
  std::unique_ptr<EbmlElement> element(stream.FindNextID(EBML_INFO(KaxCues), buffer.GetDataBufferSize()));
  if (!element)
    return nullptr;
  auto * parsed = dynamic_cast<KaxCues *>(element.get());
  if (parsed == nullptr)
    return nullptr;
  int upperLevel = 0;
  EbmlElement * found = nullptr;
  parsed->Read(stream, KaxCues::ClassInfos.Context, upperLevel, found, true);
  parsed->SetGlobalTimecodeScale(SCALE);
  return std::unique_ptr<KaxCues>(static_cast<KaxCues *>(element.release()));
}

// ---------------------------------------------------------------- inheritance

int case_tracking_inheritance()
{
  DemoFile file(true);
  MemIOCallback output;
  file.segment.WriteHead(output, 5, false);
  file.RenderClusters(output);

  const uint64 c1 = file.cluster1.GetPosition();
  const uint64 c2 = file.cluster2.GetPosition();
  const uint64 s1 = file.segment.GetRelativePosition(*file.stateS1);
  const uint64 s1u = file.segment.GetRelativePosition(*file.stateS1U);
  const uint64 s1b = file.segment.GetRelativePosition(*file.stateS1B);
  const uint64 s2 = file.segment.GetRelativePosition(*file.stateS2);
  TASK_CHECK(s1 != 0 && s1u != 0 && s1b != 0 && s2 != 0);
  TASK_CHECK(s1 != s1u && s1u != s1b && s1b != s2 && s1 != s2);

  // the duplicated registration of b1 produced a single cue
  TASK_CHECK(file.cues.ListSize() == 10);

  // cluster 1, direct KaxBlockGroup path
  CueData cue = FindCue(file.cues, 1, 0, 1);
  TASK_CHECK(cue.found && cue.cluster == c1 && cue.hasState && cue.state == s1); // own state
  cue = FindCue(file.cues, 1, 2, 2);
  TASK_CHECK(cue.found && cue.cluster == c1 && cue.hasState && cue.state == s1); // laced group = one block
  cue = FindCue(file.cues, 2, 1, 3);
  TASK_CHECK(cue.found && cue.cluster == c1 && !cue.hasState); // no state ever for track 2: omitted (0)
  cue = FindCue(file.cues, 1, 10, 4);
  TASK_CHECK(cue.found && cue.cluster == c1 && cue.hasState && cue.state == s1); // physical order, not CueTime order

  // gU carries a codec state but was never registered: it has no cue, yet
  // its state is inherited by the physically following blocks
  TASK_CHECK(!FindCue(file.cues, 1, 8, -1).found);

  // cluster 2, KaxBlockBlob path
  cue = FindCue(file.cues, 1, 5, 1);
  TASK_CHECK(cue.found && cue.cluster == c2 && cue.hasState && cue.state == s1u); // cross-cluster inheritance
  cue = FindCue(file.cues, 1, 5, 2);
  TASK_CHECK(cue.found && cue.cluster == c2 && cue.hasState && cue.state == s1u); // same tick, distinct block
  cue = FindCue(file.cues, 1, 6, 3);
  TASK_CHECK(cue.found && cue.cluster == c2 && cue.hasState && cue.state == s1b); // own state from this group on
  cue = FindCue(file.cues, 2, 20, 4);
  TASK_CHECK(cue.found && cue.cluster == c2 && cue.hasState && cue.state == s2);
  cue = FindCue(file.cues, 1, 25, 5);
  TASK_CHECK(cue.found && cue.cluster == c2 && cue.hasState && cue.state == s1b); // track 2 state must not leak in
  cue = FindCue(file.cues, 2, 30, 6);
  TASK_CHECK(cue.found && cue.cluster == c2 && cue.hasState && cue.state == s2);

  // every CueCodecState value points at the expected state bytes in the file
  TASK_CHECK(CheckStateBytes(output, file.segment, s1, STATE_1, sizeof(STATE_1)));
  TASK_CHECK(CheckStateBytes(output, file.segment, s1u, STATE_1U, sizeof(STATE_1U)));
  TASK_CHECK(CheckStateBytes(output, file.segment, s1b, STATE_1B, sizeof(STATE_1B)));
  TASK_CHECK(CheckStateBytes(output, file.segment, s2, STATE_2, sizeof(STATE_2)));
  cue = FindCue(file.cues, 1, 5, 1);
  TASK_CHECK(CheckStateBytes(output, file.segment, cue.state, STATE_1U, sizeof(STATE_1U)));
  cue = FindCue(file.cues, 1, 25, 5);
  TASK_CHECK(CheckStateBytes(output, file.segment, cue.state, STATE_1B, sizeof(STATE_1B)));
  cue = FindCue(file.cues, 2, 30, 6);
  TASK_CHECK(CheckStateBytes(output, file.segment, cue.state, STATE_2, sizeof(STATE_2)));

  return 0;
}

int case_default_mode_untouched()
{
  DemoFile file(false); // tracking left off: the historical behavior
  MemIOCallback output;
  file.segment.WriteHead(output, 5, false);
  file.RenderClusters(output);

  const uint64 s1 = file.segment.GetRelativePosition(*file.stateS1);
  const uint64 s1b = file.segment.GetRelativePosition(*file.stateS1B);

  TASK_CHECK(file.cues.ListSize() == 10);

  // a group carrying a CodecState still references its own state...
  CueData cue = FindCue(file.cues, 1, 0, -1);
  TASK_CHECK(cue.found && cue.hasState && cue.state == s1);
  cue = FindCue(file.cues, 1, 6, -1);
  TASK_CHECK(cue.found && cue.hasState && cue.state == s1b);

  // ...but nothing is inherited and no CueBlockNumber is produced
  cue = FindCue(file.cues, 1, 2, -1);
  TASK_CHECK(cue.found && !cue.hasState);
  cue = FindCue(file.cues, 1, 10, -1);
  TASK_CHECK(cue.found && !cue.hasState);
  cue = FindCue(file.cues, 1, 5, -1);
  TASK_CHECK(cue.found && !cue.hasState);
  cue = FindCue(file.cues, 1, 25, -1);
  TASK_CHECK(cue.found && !cue.hasState);
  cue = FindCue(file.cues, 2, 30, -1);
  TASK_CHECK(cue.found && !cue.hasState);

  unsigned int blockNumbers = 0;
  for (const auto elt : file.cues) {
    if (EbmlId(*elt) != EBML_ID(KaxCuePoint))
      continue;
    const auto & point = *static_cast<const KaxCuePoint *>(elt);
    auto pos = static_cast<const KaxCueTrackPositions *>(point.FindFirstElt(EBML_INFO(KaxCueTrackPositions)));
    while (pos != nullptr) {
      if (pos->FindFirstElt(EBML_INFO(KaxCueBlockNumber)) != nullptr)
        ++blockNumbers;
      pos = static_cast<const KaxCueTrackPositions *>(point.FindNextElt(*pos));
    }
  }
  TASK_CHECK(blockNumbers == 0);

  // the mode can no longer be changed once cueing happened
  TASK_CHECK(!file.cues.SetCodecStateTracking(true));
  TASK_CHECK(!file.cues.CodecStateTracking());

  return 0;
}

int case_late_configuration_rejected()
{
  KaxCues cues;
  cues.SetGlobalTimecodeScale(SCALE);
  TASK_CHECK(cues.SetCodecStateTracking(false)); // no-op on the default mode
  TASK_CHECK(cues.SetCodecStateTracking(true));
  TASK_CHECK(cues.CodecStateTracking());
  TASK_CHECK(cues.SetCodecStateTracking(false)); // still configurable before any use
  TASK_CHECK(!cues.CodecStateTracking());

  // a registration locks the mode
  KaxBlockBlob blob(BLOCK_BLOB_NO_SIMPLE);
  TASK_CHECK(cues.AddBlockBlob(blob));
  TASK_CHECK(!cues.SetCodecStateTracking(true));
  TASK_CHECK(!cues.CodecStateTracking());

  // a cluster render locks the mode as well
  KaxSegment segment;
  MemIOCallback output;
  segment.WriteHead(output, 5, false);
  KaxTrackEntry track;
  track.SetGlobalTimecodeScale(SCALE);
  GetChild<KaxTrackNumber>(track).SetValue(1);
  track.EnableLacing(false);
  KaxCluster cluster;
  cluster.SetParent(segment);
  cluster.InitTimecode(0, SCALE);
  KaxBlockGroup *group = nullptr;
  static binary data[] = {0x11, 0x22};
  cluster.AddFrame(track, 0, Wrap(data, sizeof(data)), group, LACING_NONE);

  KaxCues cues2;
  cues2.SetGlobalTimecodeScale(SCALE);
  TASK_CHECK(cues2.SetCodecStateTracking(true)); // not too late yet
  KaxCues cues3;
  cues3.SetGlobalTimecodeScale(SCALE);
  cluster.Render(output, cues3, false);
  TASK_CHECK(!cues3.SetCodecStateTracking(true)); // a render already fed these cues

  // consume the pending registration of "cues" so it can go out of scope
  KaxCluster cluster2;
  cluster2.SetParent(segment);
  cluster2.InitTimecode(0, SCALE);
  static binary data2[] = {0x33, 0x44};
  blob.SetParent(cluster2);
  blob.AddFrameAuto(track, 0, Wrap(data2, sizeof(data2)), LACING_NONE);
  cluster2.AddBlockBlob(&blob);
  cluster2.Render(output, cues, false);
  TASK_CHECK(!cues.SetCodecStateTracking(true)); // still locked
  DetachBlocks(cluster2); // the blob owns its group

  return 0;
}

// ----------------------------------------------------------------- relocation

int case_rerender_updates_addresses()
{
  DemoFile file(true);
  MemIOCallback output1;
  file.segment.WriteHead(output1, 5, false);
  file.RenderClusters(output1);

  const uint64 oldCluster1 = file.cluster1.GetPosition();
  const uint64 oldS1 = file.segment.GetRelativePosition(*file.stateS1);
  TASK_CHECK(FindCue(file.cues, 1, 0, 1).state == oldS1);

  // write the same source clusters again on the same Segment object, with
  // extra elements (a Void here) shifting every block
  MemIOCallback output2;
  file.segment.WriteHead(output2, 5, false);
  EbmlVoid spacer;
  spacer.SetSize(100);
  spacer.Render(output2);
  file.cluster1.Render(output2, file.cues, false); // first copy
  const uint64 secondCopy = output2.getFilePointer();
  file.cluster1.Render(output2, file.cues, false); // rendering the same cluster twice is allowed
  file.RenderCluster2(output2);

  const uint64 newCluster1 = file.segment.GetRelativePosition(secondCopy);
  const uint64 newS1 = file.segment.GetRelativePosition(*file.stateS1);
  const uint64 newS1U = file.segment.GetRelativePosition(*file.stateS1U);
  const uint64 newS1B = file.segment.GetRelativePosition(*file.stateS1B);
  TASK_CHECK(newCluster1 != oldCluster1);
  TASK_CHECK(newS1 != oldS1);
  TASK_CHECK(file.cluster1.GetPosition() == newCluster1); // points at the last render

  // no stale addresses, no duplicated cues
  TASK_CHECK(file.cues.ListSize() == 10);

  CueData cue = FindCue(file.cues, 1, 0, 1);
  TASK_CHECK(cue.found && cue.cluster == newCluster1 && cue.state == newS1);
  cue = FindCue(file.cues, 1, 10, 4);
  TASK_CHECK(cue.found && cue.cluster == newCluster1 && cue.state == newS1);
  // the state anchor lives in (the unregistered gU of) cluster 1, the cue in
  // cluster 2: both are updated by the re-render
  cue = FindCue(file.cues, 1, 5, 1);
  TASK_CHECK(cue.found && cue.cluster == file.cluster2.GetPosition() && cue.state == newS1U);
  cue = FindCue(file.cues, 1, 25, 5);
  TASK_CHECK(cue.found && cue.state == newS1B);

  // the new addresses really contain the state bytes in the second output
  TASK_CHECK(CheckStateBytes(output2, file.segment, newS1, STATE_1, sizeof(STATE_1)));
  TASK_CHECK(CheckStateBytes(output2, file.segment, newS1U, STATE_1U, sizeof(STATE_1U)));
  TASK_CHECK(CheckStateBytes(output2, file.segment, newS1B, STATE_1B, sizeof(STATE_1B)));
  cue = FindCue(file.cues, 1, 5, 1);
  TASK_CHECK(CheckStateBytes(output2, file.segment, cue.state, STATE_1U, sizeof(STATE_1U)));

  return 0;
}

int case_failed_render_keeps_committed()
{
  DemoFile file(true);

  // the very first render fails midway: nothing may be committed
  MemIOCallback sink0;
  FailingIO failing0(sink0, 30);
  bool threw = false;
  try {
    file.cluster1.Render(failing0, file.cues, false);
  } catch (const std::exception &) {
    threw = true;
  }
  TASK_CHECK(threw);
  TASK_CHECK(file.cues.ListSize() == 0);

  // retrying on a fixed output works
  MemIOCallback output1;
  file.segment.WriteHead(output1, 5, false);
  file.RenderClusters(output1);
  TASK_CHECK(file.cues.ListSize() == 10);
  const CueData committed = FindCue(file.cues, 1, 0, 1);
  TASK_CHECK(committed.found);

  // a failed re-render keeps the previously committed results
  MemIOCallback sink1;
  FailingIO failing1(sink1, 30);
  threw = false;
  try {
    file.cluster1.Render(failing1, file.cues, false);
  } catch (const std::exception &) {
    threw = true;
  }
  TASK_CHECK(threw);
  TASK_CHECK(file.cues.ListSize() == 10);
  CueData cue = FindCue(file.cues, 1, 0, 1);
  TASK_CHECK(cue.found && cue.cluster == committed.cluster && cue.state == committed.state);
  cue = FindCue(file.cues, 1, 25, 5);
  TASK_CHECK(cue.found && cue.state == file.segment.GetRelativePosition(*file.stateS1B));

  // once the output is fixed the retry relocates everything again
  MemIOCallback output2;
  file.segment.WriteHead(output2, 5, false);
  EbmlVoid spacer;
  spacer.SetSize(48);
  spacer.Render(output2);
  file.RenderClusters(output2);
  cue = FindCue(file.cues, 1, 0, 1);
  TASK_CHECK(cue.found && cue.cluster == file.cluster1.GetPosition() && cue.cluster != committed.cluster);
  TASK_CHECK(CheckStateBytes(output2, file.segment, cue.state, STATE_1, sizeof(STATE_1)));

  return 0;
}

// ------------------------------------------------------------------- readback

int QueryBattery(const KaxCues & cues, uint64 c1, uint64 c2, uint64 s1, uint64 s1u, uint64 s1b, uint64 s2)
{
  const KaxCueTrackPositions * pos = cues.GetDecodePosition(0, 1);
  TASK_CHECK(pos != nullptr && pos->ClusterPosition() == c1 && BlockOf(pos) == 1 && StateOf(pos) == s1);

  // the first track 2 cue is at 1ms: no predecessor at 0
  TASK_CHECK(cues.GetDecodePosition(0, 2) == nullptr);

  pos = cues.GetDecodePosition(1 * SCALE, 2);
  TASK_CHECK(pos != nullptr && pos->ClusterPosition() == c1 && BlockOf(pos) == 3 && StateOf(pos) == 0);

  pos = cues.GetDecodePosition(4 * SCALE, 1); // between 2ms and 5ms
  TASK_CHECK(pos != nullptr && pos->ClusterPosition() == c1 && BlockOf(pos) == 2 && StateOf(pos) == s1);

  // two blocks share the 5ms tick: the physically earlier one wins
  pos = cues.GetDecodePosition(5 * SCALE, 1);
  TASK_CHECK(pos != nullptr && pos->ClusterPosition() == c2 && BlockOf(pos) == 1 && StateOf(pos) == s1u);

  pos = cues.GetDecodePosition(6 * SCALE, 1);
  TASK_CHECK(pos != nullptr && pos->ClusterPosition() == c2 && BlockOf(pos) == 3 && StateOf(pos) == s1b);

  pos = cues.GetDecodePosition(10 * SCALE, 1);
  TASK_CHECK(pos != nullptr && pos->ClusterPosition() == c1 && BlockOf(pos) == 4 && StateOf(pos) == s1);

  pos = cues.GetDecodePosition(24 * SCALE, 1); // between 10ms and 25ms
  TASK_CHECK(pos != nullptr && pos->ClusterPosition() == c1 && BlockOf(pos) == 4 && StateOf(pos) == s1);

  pos = cues.GetDecodePosition(25 * SCALE, 1);
  TASK_CHECK(pos != nullptr && pos->ClusterPosition() == c2 && BlockOf(pos) == 5 && StateOf(pos) == s1b);

  pos = cues.GetDecodePosition(29 * SCALE, 2);
  TASK_CHECK(pos != nullptr && pos->ClusterPosition() == c2 && BlockOf(pos) == 4 && StateOf(pos) == s2);

  pos = cues.GetDecodePosition(1000 * SCALE, 2); // past the last cue
  TASK_CHECK(pos != nullptr && pos->ClusterPosition() == c2 && BlockOf(pos) == 6 && StateOf(pos) == s2);

  TASK_CHECK(cues.GetDecodePosition(1000 * SCALE, 9) == nullptr); // unknown track

  // queries are independent of the order they are issued in
  const KaxCueTrackPositions * first = cues.GetDecodePosition(25 * SCALE, 1);
  cues.GetDecodePosition(0, 1);
  cues.GetDecodePosition(1000 * SCALE, 2);
  cues.GetDecodePosition(6 * SCALE, 1);
  TASK_CHECK(cues.GetDecodePosition(25 * SCALE, 1) == first);

  return 0;
}

int case_decode_position_lookup()
{
  DemoFile file(true);
  MemIOCallback output;
  file.segment.WriteHead(output, 5, false);
  file.RenderClusters(output);

  const uint64 c1 = file.cluster1.GetPosition();
  const uint64 c2 = file.cluster2.GetPosition();
  const uint64 s1 = file.segment.GetRelativePosition(*file.stateS1);
  const uint64 s1u = file.segment.GetRelativePosition(*file.stateS1U);
  const uint64 s1b = file.segment.GetRelativePosition(*file.stateS1B);
  const uint64 s2 = file.segment.GetRelativePosition(*file.stateS2);

  // works on the live element...
  TASK_CHECK(QueryBattery(file.cues, c1, c2, s1, s1u, s1b, s2) == 0);

  // ...and on cues parsed back from the written bytes, with no dependency
  // on the write-time objects
  MemIOCallback cueBuffer;
  file.cues.Render(cueBuffer, false);
  auto parsed = ParseCues(cueBuffer);
  TASK_CHECK(parsed != nullptr);
  TASK_CHECK(parsed->ListSize() == 10);
  TASK_CHECK(QueryBattery(*parsed, c1, c2, s1, s1u, s1b, s2) == 0);

  return 0;
}

int case_parsed_cues_roundtrip()
{
  DemoFile file(true);
  MemIOCallback output;
  file.segment.WriteHead(output, 5, false);
  file.RenderClusters(output);

  const uint64 c2 = file.cluster2.GetPosition();
  const uint64 s1b = file.segment.GetRelativePosition(*file.stateS1B);
  const uint64 s2 = file.segment.GetRelativePosition(*file.stateS2);

  MemIOCallback cueBuffer;
  file.cues.Render(cueBuffer, false);

  // parse the whole Cues element a first time
  auto parsed1 = ParseCues(cueBuffer);
  TASK_CHECK(parsed1 != nullptr && parsed1->ListSize() == 10);
  CueData cue = FindCue(*parsed1, 1, 25, 5);
  TASK_CHECK(cue.found && cue.cluster == c2 && cue.hasState && cue.state == s1b);
  cue = FindCue(*parsed1, 2, 30, 6);
  TASK_CHECK(cue.found && cue.hasState && cue.state == s2);
  cue = FindCue(*parsed1, 2, 1, 3);
  TASK_CHECK(cue.found && !cue.hasState); // default 0 stayed omitted

  // re-encode the parsed cues and parse the whole element again
  MemIOCallback cueBuffer2;
  parsed1->Render(cueBuffer2, false);
  auto parsed2 = ParseCues(cueBuffer2);
  TASK_CHECK(parsed2 != nullptr && parsed2->ListSize() == 10);
  cue = FindCue(*parsed2, 1, 25, 5);
  TASK_CHECK(cue.found && cue.cluster == c2 && cue.hasState && cue.state == s1b);
  const KaxCueTrackPositions * pos = parsed2->GetDecodePosition(25 * SCALE, 1);
  TASK_CHECK(pos != nullptr && BlockOf(pos) == 5 && StateOf(pos) == s1b);
  pos = parsed2->GetDecodePosition(30 * SCALE, 2);
  TASK_CHECK(pos != nullptr && BlockOf(pos) == 6 && StateOf(pos) == s2);
  TASK_CHECK(parsed2->GetDecodePosition(0, 2) == nullptr);

  return 0;
}

} // namespace

const struct task_case task_cases[] = {
  {"inheritance", "tracking_inheritance", case_tracking_inheritance},
  {"inheritance", "default_mode_untouched", case_default_mode_untouched},
  {"inheritance", "late_configuration_rejected", case_late_configuration_rejected},
  {"relocation", "rerender_updates_addresses", case_rerender_updates_addresses},
  {"relocation", "failed_render_keeps_committed", case_failed_render_keeps_committed},
  {"readback", "decode_position_lookup", case_decode_position_lookup},
  {"readback", "parsed_cues_roundtrip", case_parsed_cues_roundtrip},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
