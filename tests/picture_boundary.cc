/*
 * Picture-boundary notification tests for libde265.
 *
 * The cases drive the public de265_push_picture_boundary() /
 * de265_get_completed_picture() API with the real girlshy.h265 access units
 * shipped in testdata/. They check that completion notifications follow
 * decoding order (never the POC display order), that the display output,
 * PTS, user data and all three pixel planes are unchanged, and that stalls,
 * reset and worker threads do not lose or duplicate notifications.
 *
 * The file only defines task_cases; main() lives in task_support/driver.c.
 */

// check.h declares task_cases/task_case_count with C++ linkage, but
// task_support/driver.c is compiled as C and needs unmangled symbols. Rename
// the header's declarations so we can provide extern "C" definitions below;
// the struct task_case type and task_check() are still taken from check.h.
#define task_cases task_cases_cxx_extern_unused
#define task_case_count task_case_count_cxx_extern_unused
#include "task_support/check.h"
#undef task_cases
#undef task_case_count
#include "libde265/de265.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <deque>
#include <string>
#include <functional>

namespace {

// check.h's TASK_CHECK does "return 1", which only compiles in int-returning
// functions. This TU also asserts from helpers that return vectors, structs
// or void. Redefine the macro to throw instead, while still calling
// task_check() so the driver's check counting and failure logging are
// unchanged. The registration thunk below turns the exception back into the
// int return code the driver expects.
struct TaskCheckFailure {};

}  // namespace

#undef TASK_CHECK
#define TASK_CHECK(x) \
  do { if (!task_check(!!(x), #x, __FILE__, __LINE__)) throw TaskCheckFailure(); } while (0)

namespace {


struct Nal {
  std::vector<uint8_t> data;  // NAL unit without start code, stuffing bytes kept
  int type = 0;
  bool first_slice = false;
};

struct AccessUnit {
  std::vector<size_t> nals;  // indices into the global NAL table
};

struct FrameInfo {
  int64_t pts = 0;
  intptr_t user_data = 0;
  uint64_t plane_hash[3] = {0, 0, 0};
};

constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

uint64_t fnv1a(const uint8_t* p, size_t n)
{
  uint64_t h = kFnvOffset;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= kFnvPrime;
  }
  return h;
}

std::vector<uint8_t> load_file(const char* path)
{
  std::ifstream f(path, std::ios::binary);
  TASK_CHECK(bool(f));
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

// Stateful Annex-B start-code scan; accepts both 00 00 01 and 00 00 00 01 and
// therefore never double-detects a start position. For every NAL it records
// the index of its first payload byte and the index where its start code
// begins, so the payload can be cut without swallowing the next start code.
std::vector<Nal> split_nals(const std::vector<uint8_t>& bytes)
{
  struct CodePos { size_t code_start; size_t nal_start; };
  std::vector<CodePos> codes;
  int zeros = 0;
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (bytes[i] == 0) { ++zeros; continue; }
    if (bytes[i] == 1 && zeros >= 2) {
      // The start code begins 'zeros' zeros back (the longest run, which for
      // 00 00 00 01 is three zeros back).
      codes.push_back(CodePos{i - std::min(zeros, 3), i + 1});
    }
    zeros = 0;
  }

  std::vector<Nal> nals;
  for (size_t i = 0; i < codes.size(); ++i) {
    size_t begin = codes[i].nal_start;
    size_t end = (i + 1 < codes.size()) ? codes[i + 1].code_start : bytes.size();
    Nal nal;
    nal.data.assign(bytes.begin() + begin, bytes.begin() + end);
    TASK_CHECK(nal.data.size() >= 3);
    nal.type = (nal.data[0] >> 1) & 0x3f;
    // first_slice_segment_in_pic_flag is the first bit of the slice header,
    // i.e. the first payload byte after the two-byte NAL header.
    nal.first_slice = (nal.type < 32) && (nal.data[2] & 0x80);
    nals.push_back(std::move(nal));
  }
  return nals;
}

std::vector<AccessUnit> group_access_units(const std::vector<Nal>& nals)
{
  std::vector<AccessUnit> aus;
  AccessUnit current;
  for (size_t i = 0; i < nals.size(); ++i) {
    if (nals[i].first_slice && !current.nals.empty()) {
      aus.push_back(current);
      current = AccessUnit();
    }
    current.nals.push_back(i);
  }
  aus.push_back(current);
  return aus;
}

// Pull every available output picture and record PTS, user data and one
// hash per pixel plane.
std::vector<FrameInfo> drain_outputs(de265_decoder_context* ctx)
{
  std::vector<FrameInfo> out;
  while (const de265_image* img = de265_peek_next_picture(ctx)) {
    FrameInfo info;
    info.pts = de265_get_image_PTS(img);
    info.user_data = reinterpret_cast<intptr_t>(de265_get_image_user_data(img));
    for (int c = 0; c < 3; ++c) {
      int stride = 0;
      const uint8_t* plane = de265_get_image_plane(img, c, &stride);
      int w = de265_get_image_width(img, c);
      int h = de265_get_image_height(img, c);
      TASK_CHECK(plane && w > 0 && h > 0);
      uint64_t hash = kFnvOffset;
      for (int y = 0; y < h; ++y) {
        hash ^= fnv1a(plane + y * stride, w);
        // fold the row into the running picture hash (fnv of an empty run is
        // the offset, so combine explicitly)
        hash *= kFnvPrime;
      }
      info.plane_hash[c] = hash;
    }
    out.push_back(info);
    de265_release_next_picture(ctx);
  }
  return out;
}

std::vector<uint64_t> drain_tags(de265_decoder_context* ctx)
{
  std::vector<uint64_t> tags;
  uint64_t tag = 0;
  while (de265_get_completed_picture(ctx, &tag) == 1) {
    tags.push_back(tag);
  }
  return tags;
}

// Run decode() until the decoder asks for more input or settles. Output
// pictures and completion tags are harvested after every call. When the DPB
// is full, accumulated outputs are released and decoding is resumed.
struct PumpResult {
  std::vector<FrameInfo> frames;
  std::vector<uint64_t> tags;
  bool hit_buffer_full = false;
};

PumpResult pump_until_waiting(de265_decoder_context* ctx, int max_iters = 100000)
{
  PumpResult result;
  for (int iter = 0; iter < max_iters; ++iter) {
    int more = 0;
    de265_error err = de265_decode(ctx, &more);

    auto pics = drain_outputs(ctx);
    result.frames.insert(result.frames.end(), pics.begin(), pics.end());
    auto tags = drain_tags(ctx);
    result.tags.insert(result.tags.end(), tags.begin(), tags.end());

    if (err == DE265_ERROR_WAITING_FOR_INPUT_DATA) {
      return result;
    }
    if (err == DE265_ERROR_IMAGE_BUFFER_FULL) {
      result.hit_buffer_full = true;
      // Outputs were drained above; on the next iteration the DPB has room.
      continue;
    }
    TASK_CHECK(err == DE265_OK);
    if (!more) {
      return result;
    }
  }
  TASK_CHECK(0);  // decoder never settled
  return result;
}

struct Stream {
  std::vector<uint8_t> bytes;
  std::vector<Nal> nals;
  std::vector<AccessUnit> aus;
  size_t picture_count = 0;
};

Stream make_stream()
{
  Stream s;
  s.bytes = load_file("testdata/girlshy.h265");
  TASK_CHECK(!s.bytes.empty());
  s.nals = split_nals(s.bytes);
  s.aus = group_access_units(s.nals);
  for (const Nal& n : s.nals) {
    if (n.first_slice) { ++s.picture_count; }
  }
  TASK_CHECK(s.picture_count == 75);
  // First access unit is VPS/SPS/PPS only: it is the empty-boundary control.
  TASK_CHECK(s.aus.size() == 76);
  for (size_t idx : s.aus[0].nals) {
    TASK_CHECK(s.nals[idx].type >= 32);
  }
  return s;
}

// Picture number (1..75, decoding order) for an access unit; 0 for the
// parameter-set-only AU.
int picture_number_of_au(const Stream& s, size_t au)
{
  int pic = 0;
  for (size_t a = 0; a <= au; ++a) {
    for (size_t idx : s.aus[a].nals) {
      if (s.nals[idx].first_slice) { ++pic; }
    }
  }
  return pic;
}

// Build a suffix SEI NAL (user_data_unregistered, payload type 5) carrying a
// few deterministic bytes plus RBSP trailing bits. No 00 00 0x sequence is
// present, so no emulation-prevention bytes are needed.
std::vector<uint8_t> make_suffix_sei_nal()
{
  static const char payload[] = "libde265-boundary-suffix-sei";
  std::vector<uint8_t> nal;
  nal.push_back(0x50);  // nal_unit_type = 40 (SUFFIX_SEI_NUT), forbidden bit 0
  nal.push_back(0x01);  // nuh_temporal_id_plus1 = 1
  nal.push_back(5);     // payload_type = user_data_unregistered
  nal.push_back((uint8_t)std::strlen(payload));
  for (size_t i = 0; i < std::strlen(payload); ++i) {
    nal.push_back((uint8_t)payload[i]);
  }
  nal.push_back(0x80);  // rbsp_trailing_bits
  return nal;
}

void push_au_annexb_bytewise(de265_decoder_context* ctx, const Stream& s,
                             size_t au, int64_t pts, intptr_t ud)
{
  for (size_t ni = 0; ni < s.aus[au].nals.size(); ++ni) {
    const std::vector<uint8_t>& nal = s.nals[s.aus[au].nals[ni]].data;
    static const uint8_t start_code[3] = {0, 0, 1};
    for (uint8_t b : start_code) {
      TASK_CHECK(de265_push_data(ctx, &b, 1, pts, reinterpret_cast<void*>(ud)) == DE265_OK);
    }
    for (uint8_t b : nal) {
      TASK_CHECK(de265_push_data(ctx, &b, 1, pts, reinterpret_cast<void*>(ud)) == DE265_OK);
    }
  }
}

void push_au_full_nal(de265_decoder_context* ctx, const Stream& s, size_t au,
                      int64_t pts, intptr_t ud,
                      const std::vector<uint8_t>* extra_suffix_sei)
{
  for (size_t idx : s.aus[au].nals) {
    const std::vector<uint8_t>& nal = s.nals[idx].data;
    TASK_CHECK(de265_push_NAL(ctx, nal.data(), (int)nal.size(), pts,
                              reinterpret_cast<void*>(ud)) == DE265_OK);
  }
  if (extra_suffix_sei) {
    TASK_CHECK(de265_push_NAL(ctx, extra_suffix_sei->data(),
                              (int)extra_suffix_sei->size(), pts,
                              reinterpret_cast<void*>(ud)) == DE265_OK);
  }
}

// Push one AU and, when it contains a picture, close it with a boundary
// carrying its picture number as the tag. The parameter-set AU gets a
// boundary too, which must stay silent.
void push_au_boundary(de265_decoder_context* ctx, const Stream& s, size_t au)
{
  int pic = picture_number_of_au(s, au);
  TASK_CHECK(de265_push_picture_boundary(ctx, (uint64_t)pic) == DE265_OK);
}

// Decode reference using only the pre-existing API: complete NAL units per
// access unit with the same PTS/user-data assignment as the boundary cases,
// then flush the display queue. The pixel planes are the contract checked by
// the production baseline separately.
std::vector<FrameInfo> reference_decode(const Stream& s, int workers)
{
  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx != nullptr);
  if (workers) {
    TASK_CHECK(de265_start_worker_threads(ctx, workers) == DE265_OK);
  }

  for (size_t au = 0; au < s.aus.size(); ++au) {
    int pic = picture_number_of_au(s, au);
    push_au_full_nal(ctx, s, au, pic, (intptr_t)pic, nullptr);
  }
  TASK_CHECK(de265_flush_data(ctx) == DE265_OK);
  PumpResult r = pump_until_waiting(ctx);

  TASK_CHECK(de265_free_decoder(ctx) == DE265_OK);
  TASK_CHECK(r.frames.size() == 75);
  return r.frames;
}

void expect_tag_sequence_1_to_n(const std::vector<uint64_t>& tags, size_t n)
{
  TASK_CHECK(tags.size() == n);
  for (size_t i = 0; i < n; ++i) {
    TASK_CHECK(tags[i] == (uint64_t)(i + 1));
  }
}

void expect_frames_match_reference(const std::vector<FrameInfo>& got,
                                   const std::vector<FrameInfo>& ref)
{
  TASK_CHECK(got.size() == ref.size());
  for (size_t i = 0; i < ref.size(); ++i) {
    TASK_CHECK(got[i].pts == ref[i].pts);
    TASK_CHECK(got[i].user_data == ref[i].user_data);
    for (int c = 0; c < 3; ++c) {
      TASK_CHECK(got[i].plane_hash[c] == ref[i].plane_hash[c]);
    }
  }
}

// ---------------------------------------------------------------------------
// queue group: ordering of NALs and boundaries under every input style
// ---------------------------------------------------------------------------

int case_annexb_bytewise()
{
  Stream s = make_stream();
  std::vector<FrameInfo> ref = reference_decode(s, 0);

  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx);
  for (size_t au = 0; au < s.aus.size(); ++au) {
    int pic = picture_number_of_au(s, au);
    int64_t pts = pic;  // 0 for the parameter-set AU, 1..75 for pictures
    push_au_annexb_bytewise(ctx, s, au, pts, (intptr_t)pts);
    push_au_boundary(ctx, s, au);
  }
  PumpResult r = pump_until_waiting(ctx);
  TASK_CHECK(de265_flush_data(ctx) == DE265_OK);
  {
    PumpResult tail = pump_until_waiting(ctx);
    r.frames.insert(r.frames.end(), tail.frames.begin(), tail.frames.end());
    r.tags.insert(r.tags.end(), tail.tags.begin(), tail.tags.end());
  }
  TASK_CHECK(de265_free_decoder(ctx) == DE265_OK);

  expect_tag_sequence_1_to_n(r.tags, 75);
  expect_frames_match_reference(r.frames, ref);
  return 0;
}

int case_full_nal()
{
  Stream s = make_stream();
  std::vector<FrameInfo> ref = reference_decode(s, 0);

  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx);
  for (size_t au = 0; au < s.aus.size(); ++au) {
    int pic = picture_number_of_au(s, au);
    push_au_full_nal(ctx, s, au, pic, (intptr_t)pic, nullptr);
    push_au_boundary(ctx, s, au);
  }
  PumpResult r = pump_until_waiting(ctx);
  TASK_CHECK(de265_flush_data(ctx) == DE265_OK);
  {
    PumpResult tail = pump_until_waiting(ctx);
    r.frames.insert(r.frames.end(), tail.frames.begin(), tail.frames.end());
    r.tags.insert(r.tags.end(), tail.tags.begin(), tail.tags.end());
  }
  TASK_CHECK(de265_free_decoder(ctx) == DE265_OK);

  expect_tag_sequence_1_to_n(r.tags, 75);
  expect_frames_match_reference(r.frames, ref);
  return 0;
}

int case_backlog_first()
{
  // Queue every picture and boundary before any decoding happens.
  Stream s = make_stream();
  std::vector<FrameInfo> ref = reference_decode(s, 0);

  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx);
  for (size_t au = 0; au < s.aus.size(); ++au) {
    int pic = picture_number_of_au(s, au);
    push_au_full_nal(ctx, s, au, pic, (intptr_t)pic, nullptr);
    push_au_boundary(ctx, s, au);
  }
  TASK_CHECK(de265_get_number_of_NAL_units_pending(ctx) >= 75);

  PumpResult r = pump_until_waiting(ctx);
  TASK_CHECK(de265_flush_data(ctx) == DE265_OK);
  {
    PumpResult tail = pump_until_waiting(ctx);
    r.frames.insert(r.frames.end(), tail.frames.begin(), tail.frames.end());
    r.tags.insert(r.tags.end(), tail.tags.begin(), tail.tags.end());
  }
  TASK_CHECK(de265_free_decoder(ctx) == DE265_OK);

  expect_tag_sequence_1_to_n(r.tags, 75);
  expect_frames_match_reference(r.frames, ref);
  return 0;
}

int case_empty_boundaries()
{
  Stream s = make_stream();

  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx);

  // Two boundaries with no NAL at all between them: both must stay silent.
  TASK_CHECK(de265_push_picture_boundary(ctx, 1001) == DE265_OK);
  TASK_CHECK(de265_push_picture_boundary(ctx, 1002) == DE265_OK);

  for (size_t au = 0; au < s.aus.size(); ++au) {
    int pic = picture_number_of_au(s, au);
    push_au_full_nal(ctx, s, au, pic, (intptr_t)pic, nullptr);
    push_au_boundary(ctx, s, au);
    // An extra empty boundary after every fifth picture must not notify.
    if (pic && pic % 5 == 0) {
      TASK_CHECK(de265_push_picture_boundary(ctx, (uint64_t)pic + 5000) == DE265_OK);
    }
  }
  PumpResult r = pump_until_waiting(ctx);
  TASK_CHECK(de265_flush_data(ctx) == DE265_OK);
  {
    PumpResult tail = pump_until_waiting(ctx);
    r.tags.insert(r.tags.end(), tail.tags.begin(), tail.tags.end());
  }
  TASK_CHECK(de265_free_decoder(ctx) == DE265_OK);

  expect_tag_sequence_1_to_n(r.tags, 75);
  return 0;
}

int case_repeated_and_zero_tags()
{
  // Tags may repeat and may be zero; notifications still arrive once per
  // non-empty boundary, in decoding order.
  Stream s = make_stream();

  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx);
  const uint64_t shared_tag = 777;
  for (size_t au = 0; au < s.aus.size(); ++au) {
    int pic = picture_number_of_au(s, au);
    int64_t pts = pic;
    push_au_full_nal(ctx, s, au, pts, (intptr_t)pts, nullptr);
    uint64_t tag = (pic == 0) ? 999 : (pic % 3 == 0 ? 0 : shared_tag);
    TASK_CHECK(de265_push_picture_boundary(ctx, tag) == DE265_OK);
  }
  PumpResult r = pump_until_waiting(ctx);
  TASK_CHECK(de265_flush_data(ctx) == DE265_OK);
  {
    PumpResult tail = pump_until_waiting(ctx);
    r.tags.insert(r.tags.end(), tail.tags.begin(), tail.tags.end());
  }
  TASK_CHECK(de265_free_decoder(ctx) == DE265_OK);

  TASK_CHECK(r.tags.size() == 75);
  size_t zeros = 0, shared = 0;
  int pic = 0;
  for (uint64_t tag : r.tags) {
    ++pic;
    uint64_t expected = (pic % 3 == 0) ? 0 : shared_tag;
    TASK_CHECK(tag == expected);
    if (tag == 0) { ++zeros; }
    if (tag == shared_tag) { ++shared; }
  }
  TASK_CHECK(zeros == 25);
  TASK_CHECK(shared == 50);
  return 0;
}

int case_duplicate_pts()
{
  // Every picture carries the same PTS and user_data; completion order must
  // still follow decoding order (driven by the tags), while all displayed
  // pictures report the shared PTS and the pixels stay unchanged.
  Stream s = make_stream();
  std::vector<FrameInfo> ref = reference_decode(s, 0);

  const int64_t shared_pts = 4242;
  const intptr_t shared_ud = 9090;

  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx);
  for (size_t au = 0; au < s.aus.size(); ++au) {
    int pic = picture_number_of_au(s, au);
    push_au_full_nal(ctx, s, au, shared_pts, shared_ud, nullptr);
    TASK_CHECK(de265_push_picture_boundary(ctx, (uint64_t)pic) == DE265_OK);
  }
  PumpResult r = pump_until_waiting(ctx);
  TASK_CHECK(de265_flush_data(ctx) == DE265_OK);
  {
    PumpResult tail = pump_until_waiting(ctx);
    r.frames.insert(r.frames.end(), tail.frames.begin(), tail.frames.end());
    r.tags.insert(r.tags.end(), tail.tags.begin(), tail.tags.end());
  }
  TASK_CHECK(de265_free_decoder(ctx) == DE265_OK);

  // Notifications remain a strict 1..75 decoding-order sequence.
  expect_tag_sequence_1_to_n(r.tags, 75);

  // Every output picture carries the shared PTS/user_data, but the pixels are
  // picture-specific and match the reference plane for plane.
  TASK_CHECK(r.frames.size() == 75);
  for (size_t i = 0; i < r.frames.size(); ++i) {
    TASK_CHECK(r.frames[i].pts == shared_pts);
    TASK_CHECK(r.frames[i].user_data == shared_ud);
    for (int c = 0; c < 3; ++c) {
      TASK_CHECK(r.frames[i].plane_hash[c] == ref[i].plane_hash[c]);
    }
  }
  return 0;
}

int case_suffix_sei()
{
  // A suffix SEI NAL after the slice is part of the picture and must be
  // processed before the tag is published; pixels are unchanged because the
  // user_data SEI is informational.
  Stream s = make_stream();
  std::vector<FrameInfo> ref = reference_decode(s, 0);
  std::vector<uint8_t> sei = make_suffix_sei_nal();

  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx);
  for (size_t au = 0; au < s.aus.size(); ++au) {
    int pic = picture_number_of_au(s, au);
    const std::vector<uint8_t>* extra = (pic && (pic == 1 || pic % 10 == 0 || pic == 75))
                                        ? &sei : nullptr;
    push_au_full_nal(ctx, s, au, pic, (intptr_t)pic, extra);
    push_au_boundary(ctx, s, au);
  }
  PumpResult r = pump_until_waiting(ctx);
  TASK_CHECK(de265_flush_data(ctx) == DE265_OK);
  {
    PumpResult tail = pump_until_waiting(ctx);
    r.frames.insert(r.frames.end(), tail.frames.begin(), tail.frames.end());
    r.tags.insert(r.tags.end(), tail.tags.begin(), tail.tags.end());
  }
  TASK_CHECK(de265_free_decoder(ctx) == DE265_OK);

  expect_tag_sequence_1_to_n(r.tags, 75);
  expect_frames_match_reference(r.frames, ref);
  return 0;
}

// ---------------------------------------------------------------------------
// reorder group: decoding-order notifications vs POC display order
// ---------------------------------------------------------------------------

int case_decode_order_vs_display_order()
{
  Stream s = make_stream();

  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx);
  for (size_t au = 0; au < s.aus.size(); ++au) {
    int pic = picture_number_of_au(s, au);
    push_au_full_nal(ctx, s, au, pic, (intptr_t)pic, nullptr);
    push_au_boundary(ctx, s, au);
  }
  PumpResult r = pump_until_waiting(ctx);
  TASK_CHECK(de265_flush_data(ctx) == DE265_OK);
  {
    PumpResult tail = pump_until_waiting(ctx);
    r.frames.insert(r.frames.end(), tail.frames.begin(), tail.frames.end());
    r.tags.insert(r.tags.end(), tail.tags.begin(), tail.tags.end());
  }
  TASK_CHECK(de265_free_decoder(ctx) == DE265_OK);

  // Notifications: strict decoding order 1..75.
  expect_tag_sequence_1_to_n(r.tags, 75);

  // Display output: 75 pictures whose PTS sequence is reordered by B frames.
  TASK_CHECK(r.frames.size() == 75);
  int out_of_order = 0;
  for (size_t i = 1; i < r.frames.size(); ++i) {
    if (r.frames[i].pts < r.frames[i - 1].pts) { ++out_of_order; }
  }
  TASK_CHECK(out_of_order > 10);  // girlshy keeps many B pictures reordered

  // The PTS order must differ from the tag order while containing the same
  // picture numbers exactly once.
  std::vector<int64_t> display_pts;
  for (const FrameInfo& f : r.frames) { display_pts.push_back(f.pts); }
  bool differs_from_decode_order = false;
  for (size_t i = 0; i < display_pts.size(); ++i) {
    if (display_pts[i] != (int64_t)(i + 1)) { differs_from_decode_order = true; }
  }
  TASK_CHECK(differs_from_decode_order);

  std::vector<int> seen(76, 0);
  for (int64_t pts : display_pts) {
    TASK_CHECK(pts >= 1 && pts <= 75);
    ++seen[(size_t)pts];
  }
  for (int p = 1; p <= 75; ++p) { TASK_CHECK(seen[p] == 1); }
  return 0;
}

int case_last_picture_completes_alone()
{
  // The final picture must publish its tag without any following frame
  // arriving and without flushing the DPB.
  Stream s = make_stream();
  size_t last = s.aus.size() - 1;
  int last_pic = picture_number_of_au(s, last);
  TASK_CHECK(last_pic == 75);

  // Legacy-API reference for the identical minimal input (parameter sets plus
  // the last picture only). The last frame references earlier pictures that
  // were never sent, so it cannot be compared against a full-stream decode;
  // it must instead match a non-boundary decode of exactly these NALs.
  de265_decoder_context* refctx = de265_new_decoder();
  TASK_CHECK(refctx);
  push_au_full_nal(refctx, s, 0, 0, 0, nullptr);
  push_au_full_nal(refctx, s, last, last_pic, (intptr_t)last_pic, nullptr);
  TASK_CHECK(de265_flush_data(refctx) == DE265_OK);
  PumpResult rr = pump_until_waiting(refctx);
  TASK_CHECK(de265_free_decoder(refctx) == DE265_OK);
  TASK_CHECK(rr.frames.size() == 1);

  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx);

  // Parameter sets first (empty boundary), then only the last picture.
  push_au_full_nal(ctx, s, 0, 0, 0, nullptr);
  TASK_CHECK(de265_push_picture_boundary(ctx, 0) == DE265_OK);

  push_au_full_nal(ctx, s, last, last_pic, (intptr_t)last_pic, nullptr);
  TASK_CHECK(de265_push_picture_boundary(ctx, (uint64_t)last_pic) == DE265_OK);

  PumpResult r = pump_until_waiting(ctx);

  // Exactly one notification, available before any flush / next frame.
  TASK_CHECK(r.tags.size() == 1);
  TASK_CHECK(r.tags[0] == (uint64_t)last_pic);

  // Flushing only drains the display queue; the tag must not be reported again.
  TASK_CHECK(de265_flush_data(ctx) == DE265_OK);
  PumpResult tail = pump_until_waiting(ctx);
  TASK_CHECK(tail.tags.empty());
  TASK_CHECK(de265_get_completed_picture(ctx, nullptr) == 0);

  std::vector<FrameInfo> frames = r.frames;
  frames.insert(frames.end(), tail.frames.begin(), tail.frames.end());
  TASK_CHECK(frames.size() == 1);
  TASK_CHECK(frames[0].pts == last_pic);
  const FrameInfo& expected = rr.frames[0];
  for (int c = 0; c < 3; ++c) {
    TASK_CHECK(frames[0].plane_hash[c] == expected.plane_hash[c]);
  }
  TASK_CHECK(de265_free_decoder(ctx) == DE265_OK);
  return 0;
}

int case_partial_then_continue()
{
  Stream s = make_stream();
  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx);

  const size_t split = 30;  // pictures 1..30, then the remainder
  for (size_t au = 0; au < s.aus.size(); ++au) {
    int pic = picture_number_of_au(s, au);
    if (pic > (int)split) { break; }
    push_au_full_nal(ctx, s, au, pic, (intptr_t)pic, nullptr);
    push_au_boundary(ctx, s, au);
  }
  PumpResult first = pump_until_waiting(ctx);
  TASK_CHECK(first.tags.size() == split);
  expect_tag_sequence_1_to_n(first.tags, split);

  for (size_t au = 0; au < s.aus.size(); ++au) {
    int pic = picture_number_of_au(s, au);
    if (pic <= (int)split) { continue; }
    push_au_full_nal(ctx, s, au, pic, (intptr_t)pic, nullptr);
    push_au_boundary(ctx, s, au);
  }
  PumpResult second = pump_until_waiting(ctx);
  TASK_CHECK(de265_flush_data(ctx) == DE265_OK);
  PumpResult tail = pump_until_waiting(ctx);
  TASK_CHECK(de265_free_decoder(ctx) == DE265_OK);

  std::vector<uint64_t> all = first.tags;
  all.insert(all.end(), second.tags.begin(), second.tags.end());
  all.insert(all.end(), tail.tags.begin(), tail.tags.end());
  expect_tag_sequence_1_to_n(all, 75);
  return 0;
}

// ---------------------------------------------------------------------------
// resume group: DPB stalls, reset, strict per-picture pumping, workers
// ---------------------------------------------------------------------------

int case_output_stall_resume()
{
  // Feed the whole stream, then decode while holding every output picture
  // until the DPB reports IMAGE_BUFFER_FULL. Consuming the held outputs must
  // resume decoding; pending boundaries survive the stall and every tag is
  // eventually reported exactly once, in order.
  Stream s = make_stream();
  std::vector<FrameInfo> ref = reference_decode(s, 0);

  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx);
  for (size_t au = 0; au < s.aus.size(); ++au) {
    int pic = picture_number_of_au(s, au);
    push_au_full_nal(ctx, s, au, pic, (intptr_t)pic, nullptr);
    push_au_boundary(ctx, s, au);
  }

  std::vector<FrameInfo> frames;
  std::vector<uint64_t> tags;
  bool stalled_once = false;
  for (int iter = 0; iter < 200000; ++iter) {
    int more = 0;
    de265_error err = de265_decode(ctx, &more);

    // Tags can always be consumed; they are independent of the DPB. At every
    // checkpoint the tags seen so far must be an exact prefix 1..k: no
    // duplicates, no gaps, nothing lost across a stall.
    auto fresh_tags = drain_tags(ctx);
    for (uint64_t t : fresh_tags) {
      TASK_CHECK(t == (uint64_t)(tags.size() + 1));
      tags.push_back(t);
    }

    if (err == DE265_ERROR_IMAGE_BUFFER_FULL) {
      stalled_once = true;
      // Hold the output queue across one more identical call to prove the
      // stall is sticky and does not corrupt pending boundary state.
      int more2 = 0;
      TASK_CHECK(de265_decode(ctx, &more2) == DE265_ERROR_IMAGE_BUFFER_FULL);

      auto released = drain_outputs(ctx);  // make DPB slots available
      frames.insert(frames.end(), released.begin(), released.end());
      continue;
    }

    if (err == DE265_ERROR_WAITING_FOR_INPUT_DATA) { break; }
    TASK_CHECK(err == DE265_OK);
    if (!more) { break; }
  }
  TASK_CHECK(stalled_once);

  // Drain whatever the decoder still holds, then flush display output.
  auto rest = drain_outputs(ctx);
  frames.insert(frames.end(), rest.begin(), rest.end());
  TASK_CHECK(de265_flush_data(ctx) == DE265_OK);
  PumpResult tail = pump_until_waiting(ctx);
  frames.insert(frames.end(), tail.frames.begin(), tail.frames.end());
  tags.insert(tags.end(), tail.tags.begin(), tail.tags.end());
  TASK_CHECK(de265_free_decoder(ctx) == DE265_OK);

  expect_tag_sequence_1_to_n(tags, 75);
  expect_frames_match_reference(frames, ref);
  return 0;
}

int case_reset_replay()
{
  Stream s = make_stream();
  std::vector<FrameInfo> ref = reference_decode(s, 0);

  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx);

  // Decode a prefix, then reset.
  for (size_t au = 0; au < s.aus.size(); ++au) {
    int pic = picture_number_of_au(s, au);
    if (pic > 12) { break; }
    push_au_full_nal(ctx, s, au, pic, (intptr_t)pic, nullptr);
    push_au_boundary(ctx, s, au);
  }
  PumpResult before = pump_until_waiting(ctx);
  TASK_CHECK(!before.tags.empty());

  de265_reset(ctx);

  // reset() must remove queued boundaries and all unconsumed notifications.
  TASK_CHECK(de265_get_completed_picture(ctx, nullptr) == 0);
  TASK_CHECK(de265_get_number_of_NAL_units_pending(ctx) == 0);
  TASK_CHECK(de265_peek_next_picture(ctx) == nullptr);

  // Replay the whole stream on the same context; the bookkeeping starts at
  // zero again so the full 1..75 notification sequence is reproduced.
  for (size_t au = 0; au < s.aus.size(); ++au) {
    int pic = picture_number_of_au(s, au);
    push_au_full_nal(ctx, s, au, pic, (intptr_t)pic, nullptr);
    push_au_boundary(ctx, s, au);
  }
  PumpResult r = pump_until_waiting(ctx);
  TASK_CHECK(de265_flush_data(ctx) == DE265_OK);
  {
    PumpResult tail = pump_until_waiting(ctx);
    r.frames.insert(r.frames.end(), tail.frames.begin(), tail.frames.end());
    r.tags.insert(r.tags.end(), tail.tags.begin(), tail.tags.end());
  }
  TASK_CHECK(de265_free_decoder(ctx) == DE265_OK);

  expect_tag_sequence_1_to_n(r.tags, 75);
  expect_frames_match_reference(r.frames, ref);
  return 0;
}

int case_picture_by_picture()
{
  // Strict producer/consumer: one picture plus boundary, then pump until the
  // decoder waits. Each tag must be available as soon as its picture is done,
  // independently of when display pictures are released.
  Stream s = make_stream();
  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx);

  std::vector<uint64_t> tags;
  for (size_t au = 0; au < s.aus.size(); ++au) {
    int pic = picture_number_of_au(s, au);
    push_au_full_nal(ctx, s, au, pic, (intptr_t)pic, nullptr);
    TASK_CHECK(de265_push_picture_boundary(ctx, (uint64_t)pic) == DE265_OK);

    PumpResult r = pump_until_waiting(ctx);
    // The just-pushed picture (if any) completes without a following frame.
    if (pic > 0) {
      TASK_CHECK(!r.tags.empty());
    }
    tags.insert(tags.end(), r.tags.begin(), r.tags.end());
    // Output pictures are intentionally not released here; release them only
    // periodically to prove notifications do not depend on display draining.
    if (pic % 20 == 0) {
      while (de265_peek_next_picture(ctx)) {
        de265_release_next_picture(ctx);
      }
    }
  }
  // Release everything still held, then flush the remaining display output.
  while (de265_peek_next_picture(ctx)) {
    de265_release_next_picture(ctx);
  }
  TASK_CHECK(de265_flush_data(ctx) == DE265_OK);
  PumpResult tail = pump_until_waiting(ctx);
  tags.insert(tags.end(), tail.tags.begin(), tail.tags.end());
  TASK_CHECK(de265_free_decoder(ctx) == DE265_OK);

  expect_tag_sequence_1_to_n(tags, 75);
  return 0;
}

int run_worker_scenario(int workers)
{
  Stream s = make_stream();
  std::vector<FrameInfo> ref = reference_decode(s, 0);

  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx);
  if (workers > 0) {
    TASK_CHECK(de265_start_worker_threads(ctx, workers) == DE265_OK);
  }
  for (size_t au = 0; au < s.aus.size(); ++au) {
    int pic = picture_number_of_au(s, au);
    push_au_annexb_bytewise(ctx, s, au, pic, (intptr_t)pic);
    push_au_boundary(ctx, s, au);
  }
  PumpResult r = pump_until_waiting(ctx);
  TASK_CHECK(de265_flush_data(ctx) == DE265_OK);
  {
    PumpResult tail = pump_until_waiting(ctx);
    r.frames.insert(r.frames.end(), tail.frames.begin(), tail.frames.end());
    r.tags.insert(r.tags.end(), tail.tags.begin(), tail.tags.end());
  }
  TASK_CHECK(de265_free_decoder(ctx) == DE265_OK);

  expect_tag_sequence_1_to_n(r.tags, 75);
  expect_frames_match_reference(r.frames, ref);
  return 0;
}

int case_workers_zero()
{
  return run_worker_scenario(0);
}

int case_workers_two()
{
  return run_worker_scenario(2);
}

// Convert a TASK_CHECK failure (thrown from any helper) back into the int
// return code expected by task_support/driver.c.
template <int (*Fn)()>
int run_thunk()
{
  try {
    return Fn();
  } catch (const TaskCheckFailure&) {
    return 1;
  }
}

}  // namespace

// C linkage so task_support/driver.c (compiled as C) can resolve the symbols.
// 'extern' is required because a namespace-scope const has internal linkage
// by default in C++.
extern "C" {

extern const task_case task_cases[] = {
  // queue group
  {"queue", "annexb_bytewise", run_thunk<case_annexb_bytewise>},
  {"queue", "full_nal", run_thunk<case_full_nal>},
  {"queue", "backlog_first", run_thunk<case_backlog_first>},
  {"queue", "empty_boundaries", run_thunk<case_empty_boundaries>},
  {"queue", "repeated_and_zero_tags", run_thunk<case_repeated_and_zero_tags>},
  {"queue", "duplicate_pts", run_thunk<case_duplicate_pts>},
  {"queue", "suffix_sei", run_thunk<case_suffix_sei>},
  // reorder group
  {"reorder", "decode_order_vs_display_order", run_thunk<case_decode_order_vs_display_order>},
  {"reorder", "last_picture_completes_alone", run_thunk<case_last_picture_completes_alone>},
  {"reorder", "partial_then_continue", run_thunk<case_partial_then_continue>},
  // resume group
  {"resume", "output_stall_resume", run_thunk<case_output_stall_resume>},
  {"resume", "reset_replay", run_thunk<case_reset_replay>},
  {"resume", "picture_by_picture", run_thunk<case_picture_by_picture>},
  {"resume", "workers_zero", run_thunk<case_workers_zero>},
  {"resume", "workers_two", run_thunk<case_workers_two>},
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

}  // extern "C"
