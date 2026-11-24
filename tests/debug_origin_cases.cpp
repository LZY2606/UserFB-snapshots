// Debug record origin and identity regression cases.
//
// Expected offsets, owners and identities below were derived by reading
// the sample bytes directly (see docs/debug-origin-analysis.md), never
// by running the new API. Several cases mutate copies of the samples in
// memory to prove the results track the bytes, not the file names.

#include <pe-parse/debug_records.h>
#include <pe-parse/parse.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "task_support/check.h"
#include "task_support/fixture.hpp"

namespace {

const char *kCedar32 = "samples/cedar32.pe";
const char *kCedar64 = "samples/cedar64.pe";

std::vector<std::uint8_t> load_file(const char *path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f),
                                   std::istreambuf_iterator<char>());
}

std::uint32_t get32le(const std::vector<std::uint8_t> &b, std::size_t off) {
  return static_cast<std::uint32_t>(b[off]) |
         (static_cast<std::uint32_t>(b[off + 1]) << 8) |
         (static_cast<std::uint32_t>(b[off + 2]) << 16) |
         (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

void put32le(std::vector<std::uint8_t> &b, std::size_t off, std::uint32_t v) {
  b[off] = static_cast<std::uint8_t>(v);
  b[off + 1] = static_cast<std::uint8_t>(v >> 8);
  b[off + 2] = static_cast<std::uint8_t>(v >> 16);
  b[off + 3] = static_cast<std::uint8_t>(v >> 24);
}

std::string hex8(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08x", v);
  return buf;
}

// Identity key rule mirrored from the report format: format kind plus
// every identity field, never the path.
std::string identity_of(const peparse::codeview_record &cv) {
  if (cv.format == peparse::codeview_format::rsds) {
    return "RSDS:" + cv.guid_text + ":" + hex8(cv.age);
  }
  if (cv.format == peparse::codeview_format::nb10) {
    return "NB10:" + hex8(cv.offset) + ":" + hex8(cv.signature) + ":" +
           hex8(cv.age);
  }
  return "-";
}

struct ParsedBytes {
  std::vector<std::uint8_t> bytes;
  peparse::parsed_pe *pe = nullptr;
  peparse::debug_records records;
  ~ParsedBytes() {
    if (pe != nullptr) {
      peparse::DestructParsedPE(pe);
    }
  }
};

bool parse_bytes(std::vector<std::uint8_t> bytes, ParsedBytes &out) {
  out.bytes = std::move(bytes);
  out.pe = peparse::ParsePEFromPointer(out.bytes.data(),
                                       static_cast<std::uint32_t>(out.bytes.size()));
  if (out.pe == nullptr) {
    return false;
  }
  return peparse::ReadDebugRecords(out.pe, out.records);
}

bool load_records(const char *path, ParsedBytes &out) {
  return parse_bytes(load_file(path), out);
}

bool bytes_at(const std::vector<std::uint8_t> &file,
              std::uint32_t off,
              const std::vector<std::uint8_t> &expect) {
  if (static_cast<std::uint64_t>(off) + expect.size() > file.size()) {
    return false;
  }
  return std::equal(expect.begin(), expect.end(), file.begin() + off);
}

const peparse::debug_record *record_at(const peparse::debug_records &recs,
                                       std::uint32_t ordinal) {
  if (ordinal >= recs.records.size()) {
    return nullptr;
  }
  return &recs.records[ordinal];
}

// ---------------------------------------------------------------- origins

int case_cedar32_origins() {
  std::vector<std::uint8_t> file = load_file(kCedar32);
  TASK_CHECK(!file.empty());

  ParsedBytes s;
  TASK_CHECK(load_records(kCedar32, s));
  const peparse::debug_records &r = s.records;

  // The directory holds 7 records at 0x240..0x304 inside section 0;
  // the zero-length marker at ordinal 0 does not stop the walk.
  TASK_CHECK(r.records.size() == 7);
  for (std::uint32_t i = 0; i < 7; i++) {
    TASK_CHECK(r.records[i].ordinal == i);
    TASK_CHECK(r.records[i].entry_file_offset == 0x240 + 28 * i);
    TASK_CHECK(r.records[i].directory_section_index == 0);
    std::vector<std::uint8_t> raw(r.records[i].raw_entry.begin(),
                                  r.records[i].raw_entry.end());
    TASK_CHECK(bytes_at(file, 0x240 + 28 * i, raw));
  }

  // ordinal 0: zero-length marker, kept, nothing dereferenced
  const auto *rec = record_at(r, 0);
  TASK_CHECK(rec != nullptr);
  TASK_CHECK(rec->type == 16);
  TASK_CHECK(rec->timestamp == 0x65748321);
  TASK_CHECK(rec->relation == peparse::debug_relation::none);
  TASK_CHECK(rec->candidates.empty());

  // ordinal 1: both coordinates land on file offset 0x640
  rec = record_at(r, 1);
  TASK_CHECK(rec != nullptr);
  TASK_CHECK(rec->address_of_raw_data == 0x3040);
  TASK_CHECK(rec->pointer_to_raw_data == 0x640);
  TASK_CHECK(rec->relation == peparse::debug_relation::same);
  TASK_CHECK(rec->candidates.size() == 2);
  TASK_CHECK(rec->candidates[0].source ==
             peparse::debug_candidate_source::file_pointer);
  TASK_CHECK(rec->candidates[1].source == peparse::debug_candidate_source::rva);
  TASK_CHECK(rec->candidates[0].file_offset == 0x640);
  TASK_CHECK(rec->candidates[1].file_offset == 0x640);
  TASK_CHECK(rec->candidates[0].owner == peparse::debug_owner_kind::section);
  TASK_CHECK(rec->candidates[0].owner_section == 1);
  TASK_CHECK(bytes_at(file, 0x640, rec->candidates[0].bytes));

  // ordinal 2: file pointer only, payload sits in the overlay
  rec = record_at(r, 2);
  TASK_CHECK(rec != nullptr);
  TASK_CHECK(rec->relation == peparse::debug_relation::file_only);
  TASK_CHECK(rec->candidates.size() == 1);
  TASK_CHECK(rec->candidates[0].file_offset == 0x820);
  TASK_CHECK(rec->candidates[0].owner == peparse::debug_owner_kind::overlay);
  TASK_CHECK(bytes_at(file, 0x820, rec->candidates[0].bytes));

  // ordinal 3: the two coordinates point at different bytes
  rec = record_at(r, 3);
  TASK_CHECK(rec != nullptr);
  TASK_CHECK(rec->relation == peparse::debug_relation::conflict);
  TASK_CHECK(rec->candidates.size() == 2);
  TASK_CHECK(rec->candidates[0].file_offset == 0x900);
  TASK_CHECK(rec->candidates[0].owner == peparse::debug_owner_kind::overlay);
  TASK_CHECK(rec->candidates[1].file_offset == 0x6a0);
  TASK_CHECK(rec->candidates[1].owner == peparse::debug_owner_kind::section);
  TASK_CHECK(rec->candidates[1].owner_section == 1);
  TASK_CHECK(rec->candidates[0].bytes != rec->candidates[1].bytes);
  TASK_CHECK(bytes_at(file, 0x900, rec->candidates[0].bytes));
  TASK_CHECK(bytes_at(file, 0x6a0, rec->candidates[1].bytes));

  // ordinal 4: RVA only, mapped through section 1 to 0x700
  rec = record_at(r, 4);
  TASK_CHECK(rec != nullptr);
  TASK_CHECK(rec->relation == peparse::debug_relation::rva_only);
  TASK_CHECK(rec->candidates.size() == 1);
  TASK_CHECK(rec->candidates[0].source == peparse::debug_candidate_source::rva);
  TASK_CHECK(rec->candidates[0].file_offset == 0x700);
  TASK_CHECK(rec->candidates[0].owner_section == 1);
  TASK_CHECK(bytes_at(file, 0x700, rec->candidates[0].bytes));

  // ordinal 5: distinct offsets, identical bytes
  rec = record_at(r, 5);
  TASK_CHECK(rec != nullptr);
  TASK_CHECK(rec->relation == peparse::debug_relation::equal_distinct);
  TASK_CHECK(rec->candidates.size() == 2);
  TASK_CHECK(rec->candidates[0].file_offset == 0x960);
  TASK_CHECK(rec->candidates[0].owner == peparse::debug_owner_kind::overlay);
  TASK_CHECK(rec->candidates[1].file_offset == 0x740);
  TASK_CHECK(rec->candidates[1].owner_section == 1);
  TASK_CHECK(rec->candidates[0].bytes == rec->candidates[1].bytes);

  // ordinal 6: 8-byte opaque payload in the overlay
  rec = record_at(r, 6);
  TASK_CHECK(rec != nullptr);
  TASK_CHECK(rec->relation == peparse::debug_relation::file_only);
  TASK_CHECK(rec->candidates.size() == 1);
  TASK_CHECK(rec->candidates[0].file_offset == 0x9c0);
  TASK_CHECK(rec->candidates[0].bytes.size() == 8);
  TASK_CHECK(rec->candidates[0].codeview.format ==
             peparse::codeview_format::opaque);
  TASK_CHECK(bytes_at(file, 0x9c0, rec->candidates[0].bytes));

  return 0;
}

int case_cedar64_origins() {
  std::vector<std::uint8_t> file = load_file(kCedar64);
  TASK_CHECK(!file.empty());

  ParsedBytes s;
  TASK_CHECK(load_records(kCedar64, s));
  const peparse::debug_records &r = s.records;

  // PE32+ directory: 4 records at 0x260..0x2d0 in section 0, the
  // zero-length marker is the last one.
  TASK_CHECK(r.records.size() == 4);
  for (std::uint32_t i = 0; i < 4; i++) {
    TASK_CHECK(r.records[i].entry_file_offset == 0x260 + 28 * i);
    TASK_CHECK(r.records[i].directory_section_index == 0);
  }

  const auto *rec = record_at(r, 0);
  TASK_CHECK(rec != nullptr);
  TASK_CHECK(rec->relation == peparse::debug_relation::file_only);
  TASK_CHECK(rec->size_of_data == 0x1f);
  TASK_CHECK(rec->candidates.size() == 1);
  TASK_CHECK(rec->candidates[0].file_offset == 0x820);
  TASK_CHECK(rec->candidates[0].owner == peparse::debug_owner_kind::overlay);
  TASK_CHECK(bytes_at(file, 0x820, rec->candidates[0].bytes));

  rec = record_at(r, 1);
  TASK_CHECK(rec != nullptr);
  TASK_CHECK(rec->relation == peparse::debug_relation::same);
  TASK_CHECK(rec->candidates.size() == 2);
  TASK_CHECK(rec->candidates[0].file_offset == 0x640);
  TASK_CHECK(rec->candidates[1].file_offset == 0x640);
  TASK_CHECK(rec->candidates[0].owner_section == 1);

  rec = record_at(r, 2);
  TASK_CHECK(rec != nullptr);
  TASK_CHECK(rec->relation == peparse::debug_relation::same);
  TASK_CHECK(rec->candidates[0].file_offset == 0x6a0);
  TASK_CHECK(bytes_at(file, 0x6a0, rec->candidates[0].bytes));

  rec = record_at(r, 3);
  TASK_CHECK(rec != nullptr);
  TASK_CHECK(rec->type == 16);
  TASK_CHECK(rec->relation == peparse::debug_relation::none);
  TASK_CHECK(rec->candidates.empty());

  return 0;
}

int case_overlay_relocation() {
  // Move the overlay payload of cedar32 entry 2 (0x2b bytes at 0x820)
  // to freshly appended bytes at 0xa00 and repoint PointerToRawData.
  std::vector<std::uint8_t> b = load_file(kCedar32);
  TASK_CHECK(b.size() == 0xa00);
  std::vector<std::uint8_t> payload(b.begin() + 0x820, b.begin() + 0x820 + 0x2b);
  b.insert(b.end(), payload.begin(), payload.end());
  put32le(b, 0x278 + 24, 0xa00); // entry 2 PointerToRawData field

  ParsedBytes s;
  TASK_CHECK(parse_bytes(std::move(b), s));
  const auto *rec = record_at(s.records, 2);
  TASK_CHECK(rec != nullptr);
  TASK_CHECK(rec->relation == peparse::debug_relation::file_only);
  TASK_CHECK(rec->candidates.size() == 1);
  TASK_CHECK(rec->candidates[0].file_offset == 0xa00);
  TASK_CHECK(rec->candidates[0].owner == peparse::debug_owner_kind::overlay);
  TASK_CHECK(rec->candidates[0].bytes == payload);
  // the identity travels with the bytes
  TASK_CHECK(identity_of(rec->candidates[0].codeview) ==
             "RSDS:04030201-0605-0807-090a-0b0c0d0e0f10:00000003");
  return 0;
}

int case_path_mutation() {
  // cedar32 entry 1 payload at 0x640: rewrite the path drive letter.
  // The path is not part of the identity, so the key must not move.
  std::vector<std::uint8_t> b = load_file(kCedar32);
  TASK_CHECK(b[0x658] == 'C'); // path starts at payload + 24
  b[0x658] = 'E';

  ParsedBytes s;
  TASK_CHECK(parse_bytes(std::move(b), s));
  const auto *rec = record_at(s.records, 1);
  TASK_CHECK(rec != nullptr);
  TASK_CHECK(rec->candidates.size() == 2);
  TASK_CHECK(rec->candidates[0].codeview.path == "E:\\build\\cedar.pdb");
  TASK_CHECK(identity_of(rec->candidates[0].codeview) ==
             "RSDS:04030201-0605-0807-090a-0b0c0d0e0f10:00000003");
  return 0;
}

int case_age_mutation() {
  // Bump the RSDS age of cedar32 entry 1 payload: the identity must
  // change even though GUID and path stay put.
  std::vector<std::uint8_t> b = load_file(kCedar32);
  TASK_CHECK(get32le(b, 0x654) == 3); // age at payload + 20
  put32le(b, 0x654, 9);

  ParsedBytes s;
  TASK_CHECK(parse_bytes(std::move(b), s));
  const auto *rec = record_at(s.records, 1);
  TASK_CHECK(rec != nullptr);
  TASK_CHECK(rec->candidates[0].codeview.age == 9);
  TASK_CHECK(rec->candidates[0].codeview.path == "C:\\build\\cedar.pdb");
  TASK_CHECK(identity_of(rec->candidates[0].codeview) ==
             "RSDS:04030201-0605-0807-090a-0b0c0d0e0f10:00000009");
  return 0;
}

// ------------------------------------------------------------- identities

int case_cedar32_codeview_identities() {
  ParsedBytes s;
  TASK_CHECK(load_records(kCedar32, s));

  // ordinal 1: RSDS with raw GUID bytes 01..10, age 3
  const auto *rec = record_at(s.records, 1);
  TASK_CHECK(rec != nullptr);
  const auto &cv = rec->candidates[0].codeview;
  TASK_CHECK(cv.format == peparse::codeview_format::rsds);
  for (std::uint32_t i = 0; i < 16; i++) {
    TASK_CHECK(cv.guid[i] == i + 1);
  }
  TASK_CHECK(cv.guid_text == "04030201-0605-0807-090a-0b0c0d0e0f10");
  TASK_CHECK(cv.age == 3);
  TASK_CHECK(cv.path == "C:\\build\\cedar.pdb");

  // ordinal 3: both candidates of the conflict record decode separately
  rec = record_at(s.records, 3);
  TASK_CHECK(rec != nullptr);
  TASK_CHECK(rec->candidates[0].codeview.age == 5); // file pointer bytes
  TASK_CHECK(rec->candidates[1].codeview.age == 4); // RVA bytes
  TASK_CHECK(identity_of(rec->candidates[0].codeview) !=
             identity_of(rec->candidates[1].codeview));

  // ordinal 4: NB10, signature is a scalar, not a GUID
  rec = record_at(s.records, 4);
  TASK_CHECK(rec != nullptr);
  const auto &nb = rec->candidates[0].codeview;
  TASK_CHECK(nb.format == peparse::codeview_format::nb10);
  TASK_CHECK(nb.offset == 0);
  TASK_CHECK(nb.signature == 0x1234abcd);
  TASK_CHECK(nb.age == 7);
  TASK_CHECK(nb.path == "legacy.pdb");
  TASK_CHECK(nb.guid_text.empty());

  // ordinal 6: unknown CodeView signature stays opaque
  rec = record_at(s.records, 6);
  TASK_CHECK(rec != nullptr);
  TASK_CHECK(rec->candidates[0].codeview.format ==
             peparse::codeview_format::opaque);
  TASK_CHECK(identity_of(rec->candidates[0].codeview) == "-");

  return 0;
}

int case_cross_file_identities() {
  ParsedBytes s32, s64;
  TASK_CHECK(load_records(kCedar32, s32));
  TASK_CHECK(load_records(kCedar64, s64));

  const std::string shared =
      "RSDS:04030201-0605-0807-090a-0b0c0d0e0f10:00000003";

  // cedar32 records 1, 2, 5 and cedar64 record 1 share one RSDS
  // identity, even though record 2 names D:\cache\cedar.pdb and the
  // cedar64 record names remote\cedar.pdb.
  TASK_CHECK(identity_of(record_at(s32.records, 1)->candidates[0].codeview) ==
             shared);
  TASK_CHECK(identity_of(record_at(s32.records, 2)->candidates[0].codeview) ==
             shared);
  TASK_CHECK(identity_of(record_at(s32.records, 5)->candidates[0].codeview) ==
             shared);
  TASK_CHECK(identity_of(record_at(s64.records, 1)->candidates[0].codeview) ==
             shared);
  TASK_CHECK(record_at(s32.records, 2)->candidates[0].codeview.path ==
             "D:\\cache\\cedar.pdb");
  TASK_CHECK(record_at(s64.records, 1)->candidates[0].codeview.path ==
             "remote\\cedar.pdb");

  // cedar64 record 2 spells the same path as cedar32 record 1 but its
  // GUID differs in the first byte: same path, different identity.
  const auto &other = record_at(s64.records, 2)->candidates[0].codeview;
  TASK_CHECK(other.path == "C:\\build\\cedar.pdb");
  TASK_CHECK(other.guid[0] == 0x99);
  TASK_CHECK(other.guid_text == "04030299-0605-0807-090a-0b0c0d0e0f10");
  TASK_CHECK(identity_of(other) != shared);

  // the NB10 records of the two files share one identity while naming
  // different paths
  const auto &nb32 = record_at(s32.records, 4)->candidates[0].codeview;
  const auto &nb64 = record_at(s64.records, 0)->candidates[0].codeview;
  TASK_CHECK(identity_of(nb32) == "NB10:00000000:1234abcd:00000007");
  TASK_CHECK(identity_of(nb64) == identity_of(nb32));
  TASK_CHECK(nb32.path == "legacy.pdb");
  TASK_CHECK(nb64.path == "alt\\legacy.pdb");

  // the conflict record of cedar32 contributes no certain identity:
  // its two candidates decode to different keys
  const auto *conflict = record_at(s32.records, 3);
  TASK_CHECK(conflict->relation == peparse::debug_relation::conflict);
  TASK_CHECK(identity_of(conflict->candidates[0].codeview) ==
             "RSDS:04030201-0605-0807-090a-0b0c0d0e0f10:00000005");
  TASK_CHECK(identity_of(conflict->candidates[1].codeview) ==
             "RSDS:04030201-0605-0807-090a-0b0c0d0e0f10:00000004");

  return 0;
}

int case_nb10_identity_fields() {
  // NB10 offset/signature/age are independent scalars: mutating the
  // signature moves the identity, mutating the path does not.
  std::vector<std::uint8_t> b = load_file(kCedar32);
  TASK_CHECK(get32le(b, 0x700 + 8) == 0x1234abcd);
  put32le(b, 0x700 + 8, 0x1234abce);

  ParsedBytes s;
  TASK_CHECK(parse_bytes(std::move(b), s));
  const auto &cv = record_at(s.records, 4)->candidates[0].codeview;
  TASK_CHECK(cv.format == peparse::codeview_format::nb10);
  TASK_CHECK(cv.signature == 0x1234abce);
  TASK_CHECK(cv.age == 7);
  TASK_CHECK(identity_of(cv) == "NB10:00000000:1234abce:00000007");
  TASK_CHECK(cv.guid_text.empty());
  return 0;
}

// ------------------------------------------------------------ integration

struct IterCount {
  unsigned count = 0;
  std::uint32_t lastType = 0;
  std::vector<std::uint8_t> lastBytes;
};

int iter_count_cb(void *cbd,
                  const std::uint32_t &type,
                  const peparse::bounded_buffer *data) {
  auto *st = static_cast<IterCount *>(cbd);
  st->count++;
  st->lastType = type;
  st->lastBytes.assign(data->buf, data->buf + data->bufLen);
  return 0;
}

int case_iter_debugs_compat() {
  // the pre-existing fixture payloads still iterate, both widths
  for (bool wide : {false, true}) {
    std::vector<std::uint8_t> b = basic_debug_pe(wide, false);
    peparse::parsed_pe *pe = peparse::ParsePEFromPointer(
        b.data(), static_cast<std::uint32_t>(b.size()));
    TASK_CHECK(pe != nullptr);
    IterCount st;
    peparse::IterDebugs(pe, iter_count_cb, &st);
    TASK_CHECK(st.count == 1);
    TASK_CHECK(st.lastType == 2);
    TASK_CHECK(st.lastBytes.size() == 33);
    TASK_CHECK(std::equal(st.lastBytes.begin(), st.lastBytes.begin() + 4, "RSDS"));
    peparse::DestructParsedPE(pe);

    // the overlay variant used to be rejected; the file pointer now
    // locates the payload on its own
    std::vector<std::uint8_t> o = basic_debug_pe(wide, true);
    peparse::parsed_pe *ope = peparse::ParsePEFromPointer(
        o.data(), static_cast<std::uint32_t>(o.size()));
    TASK_CHECK(ope != nullptr);
    IterCount ost;
    peparse::IterDebugs(ope, iter_count_cb, &ost);
    TASK_CHECK(ost.count == 1);
    TASK_CHECK(ost.lastBytes.size() == 33);
    TASK_CHECK(std::equal(ost.lastBytes.begin(), ost.lastBytes.begin() + 4, "RSDS"));
    peparse::DestructParsedPE(ope);
  }

  // cedar32: six non-empty records iterate once each, in directory
  // order; the conflict record surfaces its file-pointer bytes
  {
    ParsedBytes s;
    TASK_CHECK(load_records(kCedar32, s));
    IterCount st;
    peparse::IterDebugs(s.pe, iter_count_cb, &st);
    TASK_CHECK(st.count == 6);
    TASK_CHECK(st.lastType == 2);
    TASK_CHECK(st.lastBytes.size() == 8); // ordinal 6, the opaque payload
  }

  // cedar64: three callbacks, zero-length tail marker skipped
  {
    ParsedBytes s;
    TASK_CHECK(load_records(kCedar64, s));
    IterCount st;
    peparse::IterDebugs(s.pe, iter_count_cb, &st);
    TASK_CHECK(st.count == 3);
  }

  return 0;
}

int case_results_outlive_parse() {
  peparse::debug_records records;
  {
    std::vector<std::uint8_t> bytes = load_file(kCedar32);
    TASK_CHECK(!bytes.empty());
    peparse::parsed_pe *pe = peparse::ParsePEFromPointer(
        bytes.data(), static_cast<std::uint32_t>(bytes.size()));
    TASK_CHECK(pe != nullptr);
    TASK_CHECK(peparse::ReadDebugRecords(pe, records));
    peparse::DestructParsedPE(pe);
    // bytes and the parsed PE are both gone here
  }

  TASK_CHECK(records.records.size() == 7);
  const auto *rec = record_at(records, 1);
  TASK_CHECK(rec != nullptr);
  TASK_CHECK(rec->entry_file_offset == 0x25c);
  TASK_CHECK(rec->candidates[0].file_offset == 0x640);
  TASK_CHECK(rec->candidates[0].bytes.size() == 0x2b);
  TASK_CHECK(std::equal(rec->candidates[0].bytes.begin(),
                        rec->candidates[0].bytes.begin() + 4,
                        "RSDS"));
  TASK_CHECK(identity_of(rec->candidates[0].codeview) ==
             "RSDS:04030201-0605-0807-090a-0b0c0d0e0f10:00000003");
  rec = record_at(records, 4);
  TASK_CHECK(rec != nullptr);
  TASK_CHECK(identity_of(rec->candidates[0].codeview) ==
             "NB10:00000000:1234abcd:00000007");
  return 0;
}

int case_corrupt_then_good() {
  // 1. recognized RSDS payload truncated below its 24-byte header
  {
    std::vector<std::uint8_t> b = load_file(kCedar32);
    put32le(b, 0x25c + 16, 20); // entry 1 SizeOfData
    ParsedBytes s;
    s.bytes = std::move(b);
    s.pe = peparse::ParsePEFromPointer(
        s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    TASK_CHECK(s.pe != nullptr); // parse itself still succeeds
    TASK_CHECK(!peparse::ReadDebugRecords(s.pe, s.records));
    TASK_CHECK(s.records.has_error);
    TASK_CHECK(s.records.error_entry_file_offset == 0x25c);
    TASK_CHECK(!s.records.error_message.empty());
  }

  // 2. RSDS path missing its NUL terminator inside the payload
  {
    std::vector<std::uint8_t> b = load_file(kCedar32);
    TASK_CHECK(b[0x66a] == 0); // path NUL of entry 1 payload
    b[0x66a] = 'X';
    ParsedBytes s;
    s.bytes = std::move(b);
    s.pe = peparse::ParsePEFromPointer(
        s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    TASK_CHECK(s.pe != nullptr);
    TASK_CHECK(!peparse::ReadDebugRecords(s.pe, s.records));
    TASK_CHECK(s.records.error_entry_file_offset == 0x25c);
  }

  // 3. PointerToRawData range runs past the end of the file
  {
    std::vector<std::uint8_t> b = load_file(kCedar32);
    put32le(b, 0x278 + 24, 0xffff0000U); // entry 2 PointerToRawData
    ParsedBytes s;
    s.bytes = std::move(b);
    s.pe = peparse::ParsePEFromPointer(
        s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    TASK_CHECK(s.pe != nullptr);
    TASK_CHECK(!peparse::ReadDebugRecords(s.pe, s.records));
    TASK_CHECK(s.records.error_entry_file_offset == 0x278);
  }

  // 4. directory size is not a whole number of 28-byte entries
  {
    std::vector<std::uint8_t> b = load_file(kCedar32);
    TASK_CHECK(get32le(b, 0x12c) == 0xc4); // debug data directory Size
    put32le(b, 0x12c, 0xc5);
    ParsedBytes s;
    s.bytes = std::move(b);
    s.pe = peparse::ParsePEFromPointer(
        s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    TASK_CHECK(s.pe != nullptr);
    TASK_CHECK(!peparse::ReadDebugRecords(s.pe, s.records));
    TASK_CHECK(s.records.error_entry_file_offset == 0xffffffffU);
  }

  // a good file still reads cleanly after the damaged ones
  {
    ParsedBytes s;
    TASK_CHECK(load_records(kCedar32, s));
    TASK_CHECK(!s.records.has_error);
    TASK_CHECK(s.records.records.size() == 7);
    TASK_CHECK(identity_of(record_at(s.records, 1)->candidates[0].codeview) ==
               "RSDS:04030201-0605-0807-090a-0b0c0d0e0f10:00000003");
  }

  return 0;
}

} // namespace

const task_case task_cases[] = {
    {"origins", "cedar32_origins", case_cedar32_origins},
    {"origins", "cedar64_origins", case_cedar64_origins},
    {"origins", "overlay_relocation", case_overlay_relocation},
    {"origins", "path_mutation", case_path_mutation},
    {"origins", "age_mutation", case_age_mutation},
    {"identities", "cedar32_codeview_identities", case_cedar32_codeview_identities},
    {"identities", "cross_file_identities", case_cross_file_identities},
    {"identities", "nb10_identity_fields", case_nb10_identity_fields},
    {"integration", "iter_debugs_compat", case_iter_debugs_compat},
    {"integration", "results_outlive_parse", case_results_outlive_parse},
    {"integration", "corrupt_then_good", case_corrupt_then_good},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
