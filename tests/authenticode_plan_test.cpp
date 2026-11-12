/*
 * tests/authenticode_plan_test.cpp
 *
 * Exercises the Authenticode image-hash planner. The expected feed order and
 * expected byte stream below are computed from the PE/Authenticode rules
 * directly (see independentExpectation), never from the planner under test,
 * and the important numeric results are additionally hard-coded.
 */

#include <pe-parse/authenticode.h>
#include <pe-parse/parse.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using peparse::authenticode_certificate_entry;
using peparse::authenticode_plan;
using peparse::authenticode_plan_status;
using peparse::authenticode_span;
using peparse::authenticode_span_stage;
using peparse::AUTH_PLAN_ERR_CERTIFICATE_ENTRY;
using peparse::AUTH_PLAN_ERR_CERTIFICATE_TABLE;
using peparse::AUTH_PLAN_ERR_HEADER;
using peparse::AUTH_PLAN_ERR_SECTION;
using peparse::AUTH_PLAN_ERR_SECTION_TABLE;
using peparse::AUTH_PLAN_ERR_SOURCE_MISMATCH;
using peparse::AUTH_PLAN_OK;
using peparse::AUTH_SPAN_HEADER;
using peparse::AUTH_SPAN_SECTION;
using peparse::AUTH_SPAN_TRAILING;
using peparse::parsed_pe;
using peparse::ParsePEFromPointer;
using peparse::DestructParsedPE;

// ---------------------------------------------------------------------------
// Tiny PE generator. All produced values are chosen by the test, so the test
// knows the on-disk layout without asking the parser or the planner.
// ---------------------------------------------------------------------------

const std::uint32_t E_LFANEW = 0x80;
const std::uint32_t OPTIONAL_HEADER_OFFSET = 0x98;
const std::uint32_t PE32_OPTIONAL_SIZE = 224;
const std::uint32_t PE32PLUS_OPTIONAL_SIZE = 240;
const std::uint32_t CHECKSUM_FIELD_OFFSET = OPTIONAL_HEADER_OFFSET + 64;
const std::uint32_t PE32_CERT_FIELD_OFFSET = OPTIONAL_HEADER_OFFSET + 96 + 32;
const std::uint32_t PE32PLUS_CERT_FIELD_OFFSET =
    OPTIONAL_HEADER_OFFSET + 112 + 32;

void put16(std::vector<std::uint8_t> &b,
           std::uint32_t off,
           std::uint16_t v) {
  b[off] = static_cast<std::uint8_t>(v & 0xFF);
  b[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}

void put32(std::vector<std::uint8_t> &b,
           std::uint32_t off,
           std::uint32_t v) {
  b[off] = static_cast<std::uint8_t>(v & 0xFF);
  b[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
  b[off + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
  b[off + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}

void put64(std::vector<std::uint8_t> &b,
           std::uint32_t off,
           std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    b[off + static_cast<std::uint32_t>(i)] =
        static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
  }
}

struct SectionSpec {
  std::string name;
  std::uint32_t virtualAddress;
  std::uint32_t virtualSize;
  std::uint32_t rawPointer;
  std::uint32_t rawSize;
  std::uint8_t fill;
};

struct CertSpec {
  std::uint32_t dwLength;
  std::uint16_t revision;
  std::uint16_t type;
  std::uint8_t fill;
};

struct Options {
  bool plus;
  std::uint32_t sizeOfHeaders;
  std::uint32_t checksum;
  std::uint32_t numberOfRvaAndSizes;
  std::vector<SectionSpec> sections;
  std::vector<CertSpec> certs;
  // When non-zero, override the raw certificate directory entry values.
  std::uint32_t certDirOffsetOverride;
  std::uint32_t certDirSizeOverride;
  // Extra bytes after the last section when no certificate table is present.
  std::uint32_t extraTrailingBytes;

  Options()
      : plus(false),
        sizeOfHeaders(0x200),
        checksum(0x11223344),
        numberOfRvaAndSizes(16),
        certDirOffsetOverride(0),
        certDirSizeOverride(0),
        extraTrailingBytes(0) {
  }
};

struct BuiltPe {
  std::vector<std::uint8_t> bytes;
  std::uint32_t optionalHeaderSize;
  std::uint32_t sectionTableOffset;
  std::uint32_t certOffset;
  std::uint32_t certSize;
  std::uint64_t certFieldOffset;
};

std::uint32_t alignUp8(std::uint32_t v) {
  return (v + 7u) & ~static_cast<std::uint32_t>(7u);
}

BuiltPe buildPe(const Options &o) {
  BuiltPe out;
  out.optionalHeaderSize =
      o.plus ? PE32PLUS_OPTIONAL_SIZE : PE32_OPTIONAL_SIZE;
  out.sectionTableOffset = OPTIONAL_HEADER_OFFSET + out.optionalHeaderSize;
  out.certOffset = 0;
  out.certSize = 0;
  out.certFieldOffset =
      o.plus ? PE32PLUS_CERT_FIELD_OFFSET : PE32_CERT_FIELD_OFFSET;

  std::uint32_t endOfSections = o.sizeOfHeaders;
  for (std::size_t i = 0; i < o.sections.size(); ++i) {
    const SectionSpec &s = o.sections[i];
    if (s.rawSize != 0) {
      endOfSections = std::max(endOfSections, s.rawPointer + s.rawSize);
    }
  }

  std::uint32_t tableStorage = 0;
  for (std::size_t i = 0; i < o.certs.size(); ++i) {
    tableStorage += alignUp8(o.certs[i].dwLength);
  }

  std::uint32_t bodyEnd = endOfSections + o.extraTrailingBytes;
  std::uint32_t fileSize = bodyEnd;
  if (!o.certs.empty()) {
    out.certOffset = alignUp8(bodyEnd);
    out.certSize = tableStorage;
    fileSize = out.certOffset + out.certSize;
  }

  out.bytes.assign(fileSize, 0);

  // DOS header.
  put16(out.bytes, 0, 0x5A4D);
  put32(out.bytes, 0x3C, E_LFANEW);

  // NT signature + file header.
  put32(out.bytes, E_LFANEW, 0x00004550);
  put16(out.bytes, E_LFANEW + 4, o.plus ? 0x8664 : 0x014C);
  put16(out.bytes,
        E_LFANEW + 6,
        static_cast<std::uint16_t>(o.sections.size()));
  put32(out.bytes, E_LFANEW + 8, 0); // TimeDateStamp
  put32(out.bytes, E_LFANEW + 12, 0); // PointerToSymbolTable
  put32(out.bytes, E_LFANEW + 16, 0); // NumberOfSymbols
  put16(out.bytes, E_LFANEW + 20, out.optionalHeaderSize);
  put16(out.bytes, E_LFANEW + 22, 0x0102); // Characteristics

  // Optional header.
  const std::uint32_t oh = OPTIONAL_HEADER_OFFSET;
  put16(out.bytes, oh, o.plus ? 0x020B : 0x010B);
  put32(out.bytes, oh + 16, 0x1000); // AddressOfEntryPoint
  put32(out.bytes, oh + 20, 0x1000); // BaseOfCode
  if (!o.plus) {
    put32(out.bytes, oh + 24, 0x2000); // BaseOfData (PE32 only)
    put32(out.bytes, oh + 28, 0x00400000); // ImageBase
    put32(out.bytes, oh + 32, 0x1000); // SectionAlignment
    put32(out.bytes, oh + 36, 0x200); // FileAlignment
    put32(out.bytes, oh + 56, 0x4000); // SizeOfImage
    put32(out.bytes, oh + 60, o.sizeOfHeaders);
    put32(out.bytes, oh + 64, o.checksum);
    put16(out.bytes, oh + 68, 3); // Subsystem
    put32(out.bytes, oh + 92, o.numberOfRvaAndSizes);
  } else {
    put64(out.bytes, oh + 24, 0x0000000140000000ULL); // ImageBase
    put32(out.bytes, oh + 32, 0x1000);
    put32(out.bytes, oh + 36, 0x200);
    put32(out.bytes, oh + 56, 0x4000);
    put32(out.bytes, oh + 60, o.sizeOfHeaders);
    put32(out.bytes, oh + 64, o.checksum);
    put16(out.bytes, oh + 68, 3);
    put32(out.bytes, oh + 108, o.numberOfRvaAndSizes);
  }

  // Certificate Table data directory entry (index 4).
  std::uint32_t dirOffset = out.certOffset;
  std::uint32_t dirSize = out.certSize;
  if (o.certDirOffsetOverride != 0 || o.certDirSizeOverride != 0) {
    dirOffset = o.certDirOffsetOverride;
    dirSize = o.certDirSizeOverride;
  }
  if (o.numberOfRvaAndSizes > 4) {
    put32(out.bytes,
          static_cast<std::uint32_t>(out.certFieldOffset),
          dirOffset);
    put32(out.bytes,
          static_cast<std::uint32_t>(out.certFieldOffset) + 4,
          dirSize);
  }

  // Section headers (40 bytes each).
  for (std::size_t i = 0; i < o.sections.size(); ++i) {
    const SectionSpec &s = o.sections[i];
    const std::uint32_t sh =
        out.sectionTableOffset + static_cast<std::uint32_t>(i) * 40u;
    std::memcpy(&out.bytes[sh], s.name.data(), s.name.size());
    put32(out.bytes, sh + 8, s.virtualSize);
    put32(out.bytes, sh + 12, s.virtualAddress);
    put32(out.bytes, sh + 16, s.rawSize);
    put32(out.bytes, sh + 20, s.rawPointer);
    put32(out.bytes, sh + 36, 0x40000040);
    if (s.rawSize != 0) {
      for (std::uint32_t j = 0; j < s.rawSize; ++j) {
        out.bytes[s.rawPointer + j] = s.fill;
      }
    }
  }

  // Certificate table bytes.
  std::uint32_t cursor = out.certOffset;
  for (std::size_t i = 0; i < o.certs.size(); ++i) {
    const CertSpec &c = o.certs[i];
    put32(out.bytes, cursor, c.dwLength);
    put16(out.bytes, cursor + 4, c.revision);
    put16(out.bytes, cursor + 6, c.type);
    for (std::uint32_t j = 8; j < c.dwLength; ++j) {
      out.bytes[cursor + j] = c.fill;
    }
    cursor += alignUp8(c.dwLength);
  }

  return out;
}

// ---------------------------------------------------------------------------
// Independent expectation. This re-derives the Authenticode feed order
// straight from the specification, using none of the planner's types.
// ---------------------------------------------------------------------------

struct ExpectedSpan {
  std::uint64_t offset;
  std::uint64_t length;
  int stage; // 0 header, 1 section, 2 trailing
  int sectionIndex;
};

struct ExcludedPoint {
  std::uint64_t offset;
  std::uint64_t length;
};

std::vector<ExpectedSpan>
independentExpectation(const Options &o,
                       const BuiltPe &pe,
                       std::vector<ExcludedPoint> &excludedOut) {
  excludedOut.clear();
  std::vector<ExpectedSpan> spans;

  // Rule 1: [0, SizeOfHeaders), skipping CheckSum and the declared cert dir.
  excludedOut.push_back(ExcludedPoint{CHECKSUM_FIELD_OFFSET, 4});
  const bool certDeclared = o.numberOfRvaAndSizes > 4;
  if (certDeclared) {
    excludedOut.push_back(
        ExcludedPoint{pe.certFieldOffset, 8});
  }
  std::sort(excludedOut.begin(),
            excludedOut.end(),
            [](const ExcludedPoint &a, const ExcludedPoint &b) {
              return a.offset < b.offset;
            });

  std::uint64_t pos = 0;
  for (std::size_t i = 0; i < excludedOut.size(); ++i) {
    if (excludedOut[i].offset > pos) {
      spans.push_back(ExpectedSpan{
          pos, excludedOut[i].offset - pos, 0, -1});
    }
    pos = excludedOut[i].offset + excludedOut[i].length;
  }
  if (static_cast<std::uint64_t>(o.sizeOfHeaders) > pos) {
    spans.push_back(ExpectedSpan{
        pos, static_cast<std::uint64_t>(o.sizeOfHeaders) - pos, 0, -1});
  }

  // Rule 2: non-empty sections by PointerToRawData; SUM accumulates sizes.
  struct OrderEntry {
    std::uint32_t pointer;
    std::uint32_t size;
    int index;
  };
  std::vector<OrderEntry> ordered;
  for (std::size_t i = 0; i < o.sections.size(); ++i) {
    if (o.sections[i].rawSize != 0) {
      OrderEntry e;
      e.pointer = o.sections[i].rawPointer;
      e.size = o.sections[i].rawSize;
      e.index = static_cast<int>(i);
      ordered.push_back(e);
    }
  }
  std::sort(ordered.begin(),
            ordered.end(),
            [](const OrderEntry &a, const OrderEntry &b) {
              if (a.pointer != b.pointer) {
                return a.pointer < b.pointer;
              }
              return a.index < b.index;
            });

  std::uint64_t sum = o.sizeOfHeaders;
  for (std::size_t i = 0; i < ordered.size(); ++i) {
    sum += ordered[i].size;
    spans.push_back(ExpectedSpan{ordered[i].pointer, ordered[i].size, 1,
                                 ordered[i].index});
  }

  // Rule 3: [SUM, file_size - cert_size). Overlap with earlier spans stays.
  const std::uint64_t trailingEnd =
      static_cast<std::uint64_t>(pe.bytes.size()) - pe.certSize;
  if (sum < trailingEnd) {
    spans.push_back(ExpectedSpan{sum, trailingEnd - sum, 2, -1});
  }
  return spans;
}

std::vector<std::uint8_t>
materializeBytes(const std::vector<std::uint8_t> &file,
                 const std::vector<ExpectedSpan> &spans) {
  std::vector<std::uint8_t> out;
  for (std::size_t i = 0; i < spans.size(); ++i) {
    out.insert(out.end(),
               file.begin() +
                   static_cast<std::vector<std::uint8_t>::difference_type>(
                       spans[i].offset),
               file.begin() +
                   static_cast<std::vector<std::uint8_t>::difference_type>(
                       spans[i].offset + spans[i].length));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Planner plumbing
// ---------------------------------------------------------------------------

authenticode_plan_status buildForBytes(const std::vector<std::uint8_t> &bytes,
                                       parsed_pe *&peOut,
                                       authenticode_plan &plan) {
  peOut = ParsePEFromPointer(const_cast<std::uint8_t *>(bytes.data()),
                             static_cast<std::uint32_t>(bytes.size()));
  if (peOut == nullptr) {
    return static_cast<authenticode_plan_status>(0xFFFF);
  }
  return peparse::BuildAuthenticodePlan(peOut, plan);
}

// ---------------------------------------------------------------------------
// Feed callback collection
// ---------------------------------------------------------------------------

struct FeedRecord {
  std::uint64_t offset;
  std::vector<std::uint8_t> data;
  authenticode_span_stage stage;
  int sectionIndex;
};

struct FeedCollector {
  std::vector<FeedRecord> records;
  int failAtCall; // -1 never; otherwise the zero-based call that returns 42
  int calls;

  FeedCollector() : failAtCall(-1), calls(0) {
  }
};

int collectorCallback(void *context,
                      std::uint64_t offset,
                      const std::uint8_t *data,
                      std::uint64_t length,
                      authenticode_span_stage stage,
                      int sectionIndex) {
  FeedCollector *c = static_cast<FeedCollector *>(context);
  const int callIndex = c->calls++;
  if (c->failAtCall == callIndex) {
    return 42;
  }
  FeedRecord r;
  r.offset = offset;
  r.stage = stage;
  r.sectionIndex = sectionIndex;
  r.data.assign(data, data + length);
  c->records.push_back(r);
  return 0;
}

std::vector<std::uint8_t>
collectorBytes(const std::vector<FeedRecord> &records) {
  std::vector<std::uint8_t> out;
  for (std::size_t i = 0; i < records.size(); ++i) {
    out.insert(out.end(), records[i].data.begin(), records[i].data.end());
  }
  return out;
}

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++(*checks);                                                             \
    if (!(cond)) {                                                           \
      std::fprintf(stderr,                                                   \
                   "CHECK failed %s:%d: %s\n",                               \
                   __FILE__,                                                 \
                   __LINE__,                                                 \
                   #cond);                                                   \
      return 1;                                                              \
    }                                                                        \
  } while (0)

// ---------------------------------------------------------------------------
// Group: headers
// ---------------------------------------------------------------------------

int caseHeaders(unsigned *checks) {
  // PE32, certificate directory declared but zero/zero.
  {
    Options o;
    o.plus = false;
    SectionSpec s;
    s.name = ".text";
    s.virtualAddress = 0x1000;
    s.virtualSize = 0x200;
    s.rawPointer = 0x200;
    s.rawSize = 0x200;
    s.fill = 0xAA;
    o.sections.push_back(s);
    BuiltPe pe = buildPe(o);

    parsed_pe *p = nullptr;
    authenticode_plan plan;
    CHECK(buildForBytes(pe.bytes, p, plan) == AUTH_PLAN_OK);
    CHECK(plan.size_of_headers == 0x200);
    CHECK(plan.file_size == pe.bytes.size());
    CHECK(plan.certificate_table_declared);
    CHECK(plan.certificate_table_offset == 0);
    CHECK(plan.certificate_table_size == 0);
    CHECK(plan.excluded.size() == 2);
    CHECK(plan.excluded[0].offset == CHECKSUM_FIELD_OFFSET);
    CHECK(plan.excluded[0].length == 4);
    CHECK(plan.excluded[1].offset == PE32_CERT_FIELD_OFFSET);
    CHECK(plan.excluded[1].length == 8);

    // Header spans cover [0, 0x200) minus the two fields, in order.
    CHECK(plan.spans.size() >= 3);
    CHECK(plan.spans[0].stage == AUTH_SPAN_HEADER);
    CHECK(plan.spans[0].offset == 0);
    CHECK(plan.spans[0].length == CHECKSUM_FIELD_OFFSET);
    CHECK(plan.spans[1].stage == AUTH_SPAN_HEADER);
    CHECK(plan.spans[1].offset == CHECKSUM_FIELD_OFFSET + 4);
    CHECK(plan.spans[1].length ==
          PE32_CERT_FIELD_OFFSET - (CHECKSUM_FIELD_OFFSET + 4));
    CHECK(plan.spans[2].stage == AUTH_SPAN_HEADER);
    CHECK(plan.spans[2].offset == PE32_CERT_FIELD_OFFSET + 8);
    CHECK(plan.spans[2].offset + plan.spans[2].length == 0x200);

    // Metadata stays valid after the parsed PE is destroyed.
    DestructParsedPE(p);
    CHECK(plan.excluded.size() == 2);
    CHECK(plan.binding.size_of_headers == 0x200);
    CHECK(plan.binding.optional_magic == 0x010B);
  }

  // PE32+, different on-disk location for the certificate directory.
  {
    Options o;
    o.plus = true;
    SectionSpec s;
    s.name = ".text";
    s.virtualAddress = 0x1000;
    s.virtualSize = 0x200;
    s.rawPointer = 0x200;
    s.rawSize = 0x200;
    s.fill = 0xAA;
    o.sections.push_back(s);
    BuiltPe pe = buildPe(o);

    parsed_pe *p = nullptr;
    authenticode_plan plan;
    CHECK(buildForBytes(pe.bytes, p, plan) == AUTH_PLAN_OK);
    CHECK(plan.binding.optional_magic == 0x020B);
    CHECK(plan.excluded.size() == 2);
    CHECK(plan.excluded[1].offset == PE32PLUS_CERT_FIELD_OFFSET);
    CHECK(plan.excluded[0].offset == CHECKSUM_FIELD_OFFSET);
    DestructParsedPE(p);
  }

  // NumberOfRvaAndSizes == 4: the cert entry is not declared, so its eight
  // bytes are not excluded even though they exist in the file.
  {
    Options o;
    o.plus = false;
    o.numberOfRvaAndSizes = 4;
    SectionSpec s;
    s.name = ".text";
    s.virtualAddress = 0x1000;
    s.virtualSize = 0x100;
    s.rawPointer = 0x200;
    s.rawSize = 0x100;
    s.fill = 0x11;
    o.sections.push_back(s);
    BuiltPe pe = buildPe(o);

    parsed_pe *p = nullptr;
    authenticode_plan plan;
    CHECK(buildForBytes(pe.bytes, p, plan) == AUTH_PLAN_OK);
    CHECK(!plan.certificate_table_declared);
    CHECK(plan.certificate_table_offset == 0);
    CHECK(plan.certificate_table_size == 0);
    CHECK(plan.excluded.size() == 1);
    CHECK(plan.excluded[0].offset == CHECKSUM_FIELD_OFFSET);
    bool coversCertField = false;
    for (std::size_t i = 0; i < plan.spans.size(); ++i) {
      if (plan.spans[i].stage == AUTH_SPAN_HEADER &&
          plan.spans[i].offset <= PE32_CERT_FIELD_OFFSET &&
          plan.spans[i].offset + plan.spans[i].length >=
              PE32_CERT_FIELD_OFFSET + 8) {
        coversCertField = true;
      }
    }
    CHECK(coversCertField);
    DestructParsedPE(p);
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Group: sections
// ---------------------------------------------------------------------------

int caseSections(unsigned *checks) {
  // Reverse section table: table order C, A, B but raw pointers ascending.
  {
    Options o;
    const char *names[3] = {".zzz", ".aaa", ".mmm"};
    const std::uint32_t ptrs[3] = {0x600, 0x200, 0x400};
    const std::uint8_t fills[3] = {0x31, 0x32, 0x33};
    for (int i = 0; i < 3; ++i) {
      SectionSpec s;
      s.name = names[i];
      s.virtualAddress = 0x1000 + static_cast<std::uint32_t>(i) * 0x1000;
      s.virtualSize = 0x200;
      s.rawPointer = ptrs[i];
      s.rawSize = 0x200;
      s.fill = fills[i];
      o.sections.push_back(s);
    }
    BuiltPe pe = buildPe(o);

    parsed_pe *p = nullptr;
    authenticode_plan plan;
    CHECK(buildForBytes(pe.bytes, p, plan) == AUTH_PLAN_OK);

    std::vector<authenticode_span> sectionSpans;
    for (std::size_t i = 0; i < plan.spans.size(); ++i) {
      if (plan.spans[i].stage == AUTH_SPAN_SECTION) {
        sectionSpans.push_back(plan.spans[i]);
      }
    }
    CHECK(sectionSpans.size() == 3);
    CHECK(sectionSpans[0].offset == 0x200);
    CHECK(sectionSpans[0].section_index == 1);
    CHECK(sectionSpans[1].offset == 0x400);
    CHECK(sectionSpans[1].section_index == 2);
    CHECK(sectionSpans[2].offset == 0x600);
    CHECK(sectionSpans[2].section_index == 0);
    // SUM = 0x200 headers + 3 * 0x200.
    CHECK(plan.sum_of_bytes_hashed == 0x800);
    DestructParsedPE(p);
  }

  // Inter-section gap plus trailing bytes: the trailing span re-covers bytes
  // already fed by the second section, and the order must preserve that.
  {
    Options o;
    SectionSpec a;
    a.name = ".a";
    a.virtualAddress = 0x1000;
    a.virtualSize = 0x100;
    a.rawPointer = 0x200;
    a.rawSize = 0x100;
    a.fill = 0x41;
    SectionSpec b;
    b.name = ".b";
    b.virtualAddress = 0x2000;
    b.virtualSize = 0x100;
    b.rawPointer = 0x400;
    b.rawSize = 0x100;
    b.fill = 0x42;
    o.sections.push_back(a);
    o.sections.push_back(b);
    o.extraTrailingBytes = 0x100; // file ends at 0x600, sections at 0x500
    BuiltPe pe = buildPe(o);
    CHECK(pe.bytes.size() == 0x600);

    parsed_pe *p = nullptr;
    authenticode_plan plan;
    CHECK(buildForBytes(pe.bytes, p, plan) == AUTH_PLAN_OK);
    CHECK(plan.sum_of_bytes_hashed == 0x400); // 0x200 + 0x100 + 0x100

    const authenticode_span *trailing = nullptr;
    std::size_t sectionCount = 0;
    for (std::size_t i = 0; i < plan.spans.size(); ++i) {
      if (plan.spans[i].stage == AUTH_SPAN_SECTION) {
        ++sectionCount;
      }
      if (plan.spans[i].stage == AUTH_SPAN_TRAILING) {
        trailing = &plan.spans[i];
      }
    }
    CHECK(sectionCount == 2);
    CHECK(trailing != nullptr);
    CHECK(trailing->offset == 0x400);
    CHECK(trailing->length == 0x200); // [0x400, 0x600)

    // Feed and confirm the overlapping byte 0x400 is delivered twice, in
    // order, and that total delivered length counts the duplication.
    FeedCollector c;
    CHECK(peparse::FeedAuthenticodePlan(
              plan, pe.bytes.data(), pe.bytes.size(),
              collectorCallback, &c) == AUTH_PLAN_OK);
    std::uint64_t delivered = 0;
    int timesAt0x400 = 0;
    for (std::size_t i = 0; i < c.records.size(); ++i) {
      delivered += c.records[i].data.size();
      if (c.records[i].offset <= 0x400 &&
          c.records[i].offset + c.records[i].data.size() > 0x400) {
        ++timesAt0x400;
      }
    }
    CHECK(timesAt0x400 == 2);
    // Header feed contributes 0x200 - 4 (CheckSum) - 8 (cert directory),
    // then two 0x100 sections, then the 0x200 trailing range; the 0x100
    // bytes of section .b re-covered by the trailing range are counted twice.
    CHECK(delivered == (0x200ULL - 4 - 8) + 0x100ULL + 0x100ULL + 0x200ULL);
    DestructParsedPE(p);
  }

  // Zero raw-length sections are never fed, but stay in the binding.
  {
    Options o;
    SectionSpec a;
    a.name = ".a";
    a.virtualAddress = 0x1000;
    a.virtualSize = 0x100;
    a.rawPointer = 0x200;
    a.rawSize = 0x100;
    a.fill = 0x51;
    SectionSpec empty;
    empty.name = ".b";
    empty.virtualAddress = 0x2000;
    empty.virtualSize = 0x100;
    empty.rawPointer = 0;
    empty.rawSize = 0;
    empty.fill = 0;
    o.sections.push_back(a);
    o.sections.push_back(empty);
    BuiltPe pe = buildPe(o);

    parsed_pe *p = nullptr;
    authenticode_plan plan;
    CHECK(buildForBytes(pe.bytes, p, plan) == AUTH_PLAN_OK);
    CHECK(plan.binding.sections.size() == 2);
    for (std::size_t i = 0; i < plan.spans.size(); ++i) {
      CHECK(plan.spans[i].stage != AUTH_SPAN_SECTION ||
            plan.spans[i].length == 0x100);
    }
    CHECK(plan.sum_of_bytes_hashed == 0x300);
    DestructParsedPE(p);
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Group: certificates
// ---------------------------------------------------------------------------

int caseCertificates(unsigned *checks) {
  Options o;
  SectionSpec s;
  s.name = ".text";
  s.virtualAddress = 0x1000;
  s.virtualSize = 0x200;
  s.rawPointer = 0x200;
  s.rawSize = 0x200;
  s.fill = 0x77;
  o.sections.push_back(s);

  CertSpec c1;
  c1.dwLength = 12; // storage rounds to 16
  c1.revision = 0x0200;
  c1.type = 0x0002; // WIN_CERT_TYPE_PKCS_SIGNED_DATA
  c1.fill = 0xA1;
  CertSpec c2;
  c2.dwLength = 20; // storage rounds to 24
  c2.revision = 0x0100;
  c2.type = 0x00EF; // unknown type, must be retained opaquely
  c2.fill = 0xB2;
  o.certs.push_back(c1);
  o.certs.push_back(c2);

  BuiltPe pe = buildPe(o);
  CHECK(pe.certOffset == 0x400);
  CHECK(pe.certSize == 40); // 16 + 24
  CHECK(pe.bytes.size() == 0x428);

  parsed_pe *p = nullptr;
  authenticode_plan plan;
  CHECK(buildForBytes(pe.bytes, p, plan) == AUTH_PLAN_OK);
  CHECK(plan.certificate_table_offset == 0x400);
  CHECK(plan.certificate_table_size == 40);
  CHECK(plan.certificates.size() == 2);

  const authenticode_certificate_entry &e1 = plan.certificates[0];
  CHECK(e1.dw_length == 12);
  CHECK(e1.revision == 0x0200);
  CHECK(e1.type == 0x0002);
  CHECK(e1.entry_offset == 0x400);
  CHECK(e1.entry_length == 12);
  CHECK(e1.payload_offset == 0x408);
  CHECK(e1.payload_length == 4); // padding is not payload
  CHECK(e1.storage_offset == 0x400);
  CHECK(e1.storage_length == 16);

  const authenticode_certificate_entry &e2 = plan.certificates[1];
  CHECK(e2.dw_length == 20);
  CHECK(e2.revision == 0x0100);
  CHECK(e2.type == 0x00EF);
  CHECK(e2.entry_offset == 0x410); // starts after rounded storage
  CHECK(e2.entry_length == 20);
  CHECK(e2.payload_offset == 0x418);
  CHECK(e2.payload_length == 12);
  CHECK(e2.storage_offset == 0x410);
  CHECK(e2.storage_length == 24);
  CHECK(e2.storage_offset + e2.storage_length == 0x428);

  // Certificate bytes never appear in any hash span.
  for (std::size_t i = 0; i < plan.spans.size(); ++i) {
    CHECK(plan.spans[i].offset + plan.spans[i].length <= 0x400);
  }
  // The trailing span ends at file_size - cert_size.
  const authenticode_span *trailing = nullptr;
  for (std::size_t i = 0; i < plan.spans.size(); ++i) {
    if (plan.spans[i].stage == AUTH_SPAN_TRAILING) {
      trailing = &plan.spans[i];
    }
  }
  CHECK(trailing == nullptr || trailing->offset + trailing->length == 0x400);

  DestructParsedPE(p);

  // Declared zero/zero directory: no entries, but the field is excluded.
  {
    Options o2;
    o2.sections.push_back(s);
    BuiltPe pe2 = buildPe(o2);
    parsed_pe *p2 = nullptr;
    authenticode_plan plan2;
    CHECK(buildForBytes(pe2.bytes, p2, plan2) == AUTH_PLAN_OK);
    CHECK(plan2.certificate_table_declared);
    CHECK(plan2.certificates.empty());
    CHECK(plan2.certificate_table_offset == 0);
    CHECK(plan2.certificate_table_size == 0);
    bool hasCertExclusion = false;
    for (std::size_t i = 0; i < plan2.excluded.size(); ++i) {
      if (plan2.excluded[i].field ==
          peparse::AUTH_EXCLUDED_CERTIFICATE_DIRECTORY) {
        hasCertExclusion = true;
      }
    }
    CHECK(hasCertExclusion);
    DestructParsedPE(p2);
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Group: feeding
// ---------------------------------------------------------------------------

int caseFeeding(unsigned *checks) {
  Options o;
  SectionSpec a;
  a.name = ".a";
  a.virtualAddress = 0x1000;
  a.virtualSize = 0x100;
  a.rawPointer = 0x200;
  a.rawSize = 0x100;
  a.fill = 0x61;
  SectionSpec b;
  b.name = ".b";
  b.virtualAddress = 0x2000;
  b.virtualSize = 0x100;
  b.rawPointer = 0x400;
  b.rawSize = 0x100;
  b.fill = 0x62;
  o.sections.push_back(a);
  o.sections.push_back(b);
  CertSpec c;
  c.dwLength = 16;
  c.revision = 0x0200;
  c.type = 0x0002;
  c.fill = 0xC3;
  o.certs.push_back(c);
  BuiltPe pe = buildPe(o);

  std::vector<ExcludedPoint> expectedExcluded;
  std::vector<ExpectedSpan> expectedSpans =
      independentExpectation(o, pe, expectedExcluded);
  std::vector<std::uint8_t> expectedBytes =
      materializeBytes(pe.bytes, expectedSpans);

  parsed_pe *p = nullptr;
  authenticode_plan plan;
  CHECK(buildForBytes(pe.bytes, p, plan) == AUTH_PLAN_OK);
  DestructParsedPE(p); // feeding must work solely off the plan + bytes

  // Span-for-span agreement with the independent expectation.
  CHECK(plan.spans.size() == expectedSpans.size());
  for (std::size_t i = 0; i < plan.spans.size(); ++i) {
    CHECK(plan.spans[i].offset == expectedSpans[i].offset);
    CHECK(plan.spans[i].length == expectedSpans[i].length);
    CHECK(static_cast<int>(plan.spans[i].stage) == expectedSpans[i].stage);
    CHECK(plan.spans[i].section_index == expectedSpans[i].sectionIndex);
  }

  FeedCollector collector;
  CHECK(peparse::FeedAuthenticodePlan(plan,
                                      pe.bytes.data(),
                                      pe.bytes.size(),
                                      collectorCallback,
                                      &collector) == AUTH_PLAN_OK);
  CHECK(collector.records.size() == expectedSpans.size());
  CHECK(collectorBytes(collector.records) == expectedBytes);
  for (std::size_t i = 0; i < collector.records.size(); ++i) {
    CHECK(collector.records[i].offset == expectedSpans[i].offset);
    CHECK(collector.records[i].data.size() == expectedSpans[i].length);
    CHECK(static_cast<int>(collector.records[i].stage) ==
          expectedSpans[i].stage);
    CHECK(collector.records[i].sectionIndex ==
          expectedSpans[i].sectionIndex);
  }

  // Content change inside a section: same plan, changed stream at that byte.
  {
    std::vector<std::uint8_t> modified = pe.bytes;
    modified[0x210] = 0x99;
    FeedCollector c2;
    CHECK(peparse::FeedAuthenticodePlan(plan,
                                        modified.data(),
                                        modified.size(),
                                        collectorCallback,
                                        &c2) == AUTH_PLAN_OK);
    std::vector<std::uint8_t> modifiedExpected =
        materializeBytes(modified, expectedSpans);
    CHECK(collectorBytes(c2.records) == modifiedExpected);
    CHECK(collectorBytes(c2.records) != expectedBytes);
    CHECK(c2.records.size() == collector.records.size());
  }

  // CheckSum change: input byte stream is identical.
  {
    std::vector<std::uint8_t> modified = pe.bytes;
    put32(modified, CHECKSUM_FIELD_OFFSET, 0xDEADBEEF);
    FeedCollector c2;
    CHECK(peparse::FeedAuthenticodePlan(plan,
                                        modified.data(),
                                        modified.size(),
                                        collectorCallback,
                                        &c2) == AUTH_PLAN_OK);
    CHECK(collectorBytes(c2.records) == expectedBytes);
  }

  // Certificate payload change: input byte stream is identical.
  {
    std::vector<std::uint8_t> modified = pe.bytes;
    modified[pe.certOffset + 8] = 0x7E;
    FeedCollector c2;
    CHECK(peparse::FeedAuthenticodePlan(plan,
                                        modified.data(),
                                        modified.size(),
                                        collectorCallback,
                                        &c2) == AUTH_PLAN_OK);
    CHECK(collectorBytes(c2.records) == expectedBytes);
  }

  // Layout changes are refused before any callback runs.
  {
    std::vector<std::uint8_t> modified = pe.bytes;
    put32(modified, pe.sectionTableOffset + 16, 0x18); // SizeOfRawData
    FeedCollector c2;
    CHECK(peparse::FeedAuthenticodePlan(plan,
                                        modified.data(),
                                        modified.size(),
                                        collectorCallback,
                                        &c2) == AUTH_PLAN_ERR_SOURCE_MISMATCH);
    CHECK(c2.calls == 0);
  }
  {
    std::vector<std::uint8_t> modified = pe.bytes;
    put32(modified, OPTIONAL_HEADER_OFFSET + 60, 0x220); // SizeOfHeaders
    FeedCollector c2;
    CHECK(peparse::FeedAuthenticodePlan(plan,
                                        modified.data(),
                                        modified.size(),
                                        collectorCallback,
                                        &c2) == AUTH_PLAN_ERR_SOURCE_MISMATCH);
    CHECK(c2.calls == 0);
  }
  {
    std::vector<std::uint8_t> modified = pe.bytes;
    put32(modified,
          static_cast<std::uint32_t>(pe.certFieldOffset) + 4,
          pe.certSize + 8); // certificate directory size
    FeedCollector c2;
    CHECK(peparse::FeedAuthenticodePlan(plan,
                                        modified.data(),
                                        modified.size(),
                                        collectorCallback,
                                        &c2) == AUTH_PLAN_ERR_SOURCE_MISMATCH);
    CHECK(c2.calls == 0);
  }
  {
    FeedCollector c2;
    CHECK(peparse::FeedAuthenticodePlan(plan,
                                        pe.bytes.data(),
                                        pe.bytes.size() - 1,
                                        collectorCallback,
                                        &c2) == AUTH_PLAN_ERR_SOURCE_MISMATCH);
    CHECK(c2.calls == 0);
  }

  // Callback failure is passed through verbatim and stops the feed.
  {
    FeedCollector c2;
    c2.failAtCall = 1;
    const int rc = peparse::FeedAuthenticodePlan(plan,
                                                 pe.bytes.data(),
                                                 pe.bytes.size(),
                                                 collectorCallback,
                                                 &c2);
    CHECK(rc == static_cast<authenticode_plan_status>(42));
    CHECK(c2.records.size() == 1);
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Group: invalid layouts
// ---------------------------------------------------------------------------

int caseInvalid(unsigned *checks) {
  SectionSpec s;
  s.name = ".text";
  s.virtualAddress = 0x1000;
  s.virtualSize = 0x200;
  s.rawPointer = 0x200;
  s.rawSize = 0x200;
  s.fill = 0x77;

  // Exactly one of certificate offset/size zero.
  {
    Options o;
    o.sections.push_back(s);
    CertSpec c;
    c.dwLength = 16;
    c.revision = 0x0200;
    c.type = 0x0002;
    c.fill = 0x11;
    o.certs.push_back(c);
    BuiltPe pe = buildPe(o);
    put32(pe.bytes, static_cast<std::uint32_t>(pe.certFieldOffset), 0);
    put32(pe.bytes,
          static_cast<std::uint32_t>(pe.certFieldOffset) + 4,
          pe.certSize);

    parsed_pe *p = nullptr;
    authenticode_plan plan;
    CHECK(buildForBytes(pe.bytes, p, plan) ==
          AUTH_PLAN_ERR_CERTIFICATE_TABLE);
    CHECK(plan.file_size == 0);
    CHECK(plan.spans.empty());
    DestructParsedPE(p);
  }

  // Certificate table not at the end of the file.
  {
    Options o;
    o.sections.push_back(s);
    CertSpec c;
    c.dwLength = 16;
    c.revision = 0x0200;
    c.type = 0x0002;
    c.fill = 0x11;
    o.certs.push_back(c);
    BuiltPe pe = buildPe(o);
    pe.bytes.push_back(0xDD);
    pe.bytes.push_back(0xDD);

    parsed_pe *p = nullptr;
    authenticode_plan plan;
    CHECK(buildForBytes(pe.bytes, p, plan) ==
          AUTH_PLAN_ERR_CERTIFICATE_TABLE);
    DestructParsedPE(p);
  }

  // Misaligned certificate table offset.
  {
    Options o;
    o.sections.push_back(s);
    CertSpec c;
    c.dwLength = 16;
    c.revision = 0x0200;
    c.type = 0x0002;
    c.fill = 0x11;
    o.certs.push_back(c);
    BuiltPe pe = buildPe(o);
    put32(pe.bytes,
          static_cast<std::uint32_t>(pe.certFieldOffset),
          pe.certOffset + 2);
    put32(pe.bytes,
          static_cast<std::uint32_t>(pe.certFieldOffset) + 4,
          pe.certSize - 2);

    parsed_pe *p = nullptr;
    authenticode_plan plan;
    CHECK(buildForBytes(pe.bytes, p, plan) ==
          AUTH_PLAN_ERR_CERTIFICATE_TABLE);
    DestructParsedPE(p);
  }

  // dwLength smaller than the eight-byte WIN_CERTIFICATE header.
  {
    Options o;
    o.sections.push_back(s);
    CertSpec c;
    c.dwLength = 16;
    c.revision = 0x0200;
    c.type = 0x0002;
    c.fill = 0x11;
    o.certs.push_back(c);
    BuiltPe pe = buildPe(o);
    put32(pe.bytes, pe.certOffset, 4);

    parsed_pe *p = nullptr;
    authenticode_plan plan;
    CHECK(buildForBytes(pe.bytes, p, plan) ==
          AUTH_PLAN_ERR_CERTIFICATE_ENTRY);
    CHECK(plan.certificates.empty());
    DestructParsedPE(p);
  }

  // dwLength runs past the table, and a truncated trailing entry is refused.
  {
    Options o;
    o.sections.push_back(s);
    CertSpec c1;
    c1.dwLength = 16;
    c1.revision = 0x0200;
    c1.type = 0x0002;
    c1.fill = 0x11;
    CertSpec c2;
    c2.dwLength = 16;
    c2.revision = 0x0200;
    c2.type = 0x0002;
    c2.fill = 0x22;
    o.certs.push_back(c1);
    o.certs.push_back(c2);
    BuiltPe pe = buildPe(o); // table size 32
    // First entry claims the whole table plus more.
    put32(pe.bytes, pe.certOffset, 40);

    parsed_pe *p = nullptr;
    authenticode_plan plan;
    CHECK(buildForBytes(pe.bytes, p, plan) ==
          AUTH_PLAN_ERR_CERTIFICATE_ENTRY);
    DestructParsedPE(p);
  }

  // Trailing partial WIN_CERTIFICATE header whose dwLength exceeds the
  // remaining table bytes.
  {
    Options o;
    o.sections.push_back(s);
    CertSpec c1;
    c1.dwLength = 12; // storage 16
    c1.revision = 0x0200;
    c1.type = 0x0002;
    c1.fill = 0x11;
    CertSpec c2;
    c2.dwLength = 16;
    c2.revision = 0x0200;
    c2.type = 0x0002;
    c2.fill = 0x22;
    o.certs.push_back(c1);
    o.certs.push_back(c2);
    BuiltPe pe = buildPe(o); // 16 + 16 = 32 table bytes
    // Second header starts at 0x10 into the table; claim more than remains.
    put32(pe.bytes, pe.certOffset + 16, 24);

    parsed_pe *p = nullptr;
    authenticode_plan plan;
    CHECK(buildForBytes(pe.bytes, p, plan) ==
          AUTH_PLAN_ERR_CERTIFICATE_ENTRY);
    DestructParsedPE(p);
  }

  // Certificate table size that is not 8-byte aligned.
  {
    Options o;
    o.sections.push_back(s);
    CertSpec c;
    c.dwLength = 16;
    c.revision = 0x0200;
    c.type = 0x0002;
    c.fill = 0x11;
    o.certs.push_back(c);
    BuiltPe pe = buildPe(o);
    put32(pe.bytes,
          static_cast<std::uint32_t>(pe.certFieldOffset) + 4,
          pe.certSize - 1);

    parsed_pe *p = nullptr;
    authenticode_plan plan;
    CHECK(buildForBytes(pe.bytes, p, plan) ==
          AUTH_PLAN_ERR_CERTIFICATE_TABLE);
    DestructParsedPE(p);
  }

  // Overlapping section raw ranges.
  {
    Options o;
    SectionSpec a = s;
    a.name = ".a";
    a.rawPointer = 0x200;
    a.rawSize = 0x200;
    SectionSpec b = s;
    b.name = ".b";
    b.virtualAddress = 0x2000;
    b.rawPointer = 0x300;
    b.rawSize = 0x200;
    o.sections.push_back(a);
    o.sections.push_back(b);
    BuiltPe pe = buildPe(o);

    parsed_pe *p = nullptr;
    authenticode_plan plan;
    CHECK(buildForBytes(pe.bytes, p, plan) == AUTH_PLAN_ERR_SECTION);
    DestructParsedPE(p);
  }

  // Section intruding into the header range. The section sits in the padding
  // between the end of the (single-entry) section table at 0x1A0 and
  // SizeOfHeaders at 0x200, so the parser accepts the file while the planner
  // still rejects a raw pointer below SizeOfHeaders.
  {
    Options o;
    SectionSpec bad = s;
    bad.rawPointer = 0x1A0;
    bad.rawSize = 0x40;
    o.sections.push_back(bad);
    BuiltPe pe = buildPe(o);

    parsed_pe *p = nullptr;
    authenticode_plan plan;
    CHECK(buildForBytes(pe.bytes, p, plan) == AUTH_PLAN_ERR_SECTION);
    DestructParsedPE(p);
  }

  // Section whose declared raw range intrudes into the certificate table.
  // The file is built with a well-formed [0x200,0x400) section and a table
  // at [0x400,0x410); the section header's SizeOfRawData is then extended to
  // reach into the table. The parser accepted the original file, so the
  // planner must reject the section/certificate overlap itself.
  {
    Options o;
    o.sections.push_back(s); // [0x200, 0x400)
    CertSpec c;
    c.dwLength = 16;
    c.revision = 0x0200;
    c.type = 0x0002;
    c.fill = 0x11;
    o.certs.push_back(c);
    BuiltPe pe = buildPe(o);
    CHECK(pe.certOffset == 0x400);
    // Extend the single section's SizeOfRawData from 0x200 to 0x208.
    put32(pe.bytes, pe.sectionTableOffset + 16, 0x208);

    parsed_pe *p = nullptr;
    authenticode_plan plan;
    CHECK(buildForBytes(pe.bytes, p, plan) == AUTH_PLAN_ERR_SECTION);
    DestructParsedPE(p);
  }

  // SizeOfHeaders does not cover the end of the section table.
  {
    Options o;
    o.sections.push_back(s);
    o.sizeOfHeaders = 0x180; // PE32 section table ends at 0x1A0
    BuiltPe pe = buildPe(o);

    parsed_pe *p = nullptr;
    authenticode_plan plan;
    CHECK(buildForBytes(pe.bytes, p, plan) == AUTH_PLAN_ERR_HEADER);
    DestructParsedPE(p);
  }

  // NumberOfRvaAndSizes beyond the 16-entry limit.
  {
    Options o;
    o.sections.push_back(s);
    o.numberOfRvaAndSizes = 17;
    BuiltPe pe = buildPe(o);

    parsed_pe *p = nullptr;
    authenticode_plan plan;
    CHECK(buildForBytes(pe.bytes, p, plan) == AUTH_PLAN_ERR_HEADER);
    DestructParsedPE(p);
  }

  // SizeOfOptionalHeader that cannot contain NumberOfRvaAndSizes. The
  // parser tolerates the bad field (it positions from the fixed optional
  // header layout), so the planner gets to enforce the on-disk boundary.
  {
    Options o;
    o.sections.push_back(s);
    BuiltPe pe = buildPe(o);
    const std::uint16_t shrunkOptionalSize = 90;
    put16(pe.bytes, E_LFANEW + 20, shrunkOptionalSize);

    parsed_pe *p = nullptr;
    authenticode_plan plan;
    CHECK(buildForBytes(pe.bytes, p, plan) == AUTH_PLAN_ERR_HEADER);
    CHECK(plan.spans.empty());
    DestructParsedPE(p);
  }

  return 0;
}

} // namespace

extern int authenticode_plan_case(const char *group, unsigned *checks) {
  if (std::strcmp(group, "headers") == 0) {
    return caseHeaders(checks);
  }
  if (std::strcmp(group, "sections") == 0) {
    return caseSections(checks);
  }
  if (std::strcmp(group, "certificates") == 0) {
    return caseCertificates(checks);
  }
  if (std::strcmp(group, "feeding") == 0) {
    return caseFeeding(checks);
  }
  if (std::strcmp(group, "invalid") == 0) {
    return caseInvalid(checks);
  }
  std::fprintf(stderr, "unknown test group: %s\n", group);
  return 1;
}
