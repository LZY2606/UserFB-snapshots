/*
 * Hand-checked content tests for BuildBaseRelocationPreview.
 *
 * The minimal PE images below are constructed directly in memory; no
 * host file is ever read. Every expected value was computed by hand.
 *
 * This translation unit is compiled with the repository root on its
 * include path, so it deliberately avoids C++ container/string headers
 * (a file named "version" in the root shadows the standard one); fixed
 * buffers and C arrays are enough for these tiny images.
 */

#include "task_support/check.h"

#include <pe-parse/relocation_preview.h>

#include <cstdint>
#include <cstring>

using namespace peparse;

namespace {

constexpr std::uint16_t kMachineI386 = 0x014C;
constexpr std::uint16_t kMachineAmd64 = 0x8664;
constexpr std::uint16_t kMagicPe32 = 0x010B;
constexpr std::uint16_t kMagicPe32Plus = 0x020B;
constexpr std::size_t kImageCapacity = 0x1000;
constexpr std::size_t kMaxSections = 4;

void put16(std::uint8_t *b, std::size_t off, std::uint16_t v) {
  b[off] = static_cast<std::uint8_t>(v);
  b[off + 1] = static_cast<std::uint8_t>(v >> 8);
}

void put32(std::uint8_t *b, std::size_t off, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    b[off + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>(v >> (8 * i));
  }
}

void put64(std::uint8_t *b, std::size_t off, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    b[off + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>(v >> (8 * i));
  }
}

std::uint16_t get16(const std::uint8_t *b, std::size_t off) {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(b[off]) |
      (static_cast<std::uint16_t>(b[off + 1]) << 8));
}

std::uint32_t get32(const std::uint8_t *b, std::size_t off) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<std::uint32_t>(b[off + static_cast<std::size_t>(i)])
         << (8 * i);
  }
  return v;
}

std::uint64_t get64(const std::uint8_t *b, std::size_t off) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(b[off + static_cast<std::size_t>(i)])
         << (8 * i);
  }
  return v;
}

bool bytesEqual(const std::uint8_t *a, const std::uint8_t *b,
                std::uint32_t n) {
  return std::memcmp(a, b, n) == 0;
}

std::uint16_t relocWord(std::uint16_t type, std::uint16_t offset) {
  return static_cast<std::uint16_t>((type << 12) | (offset & 0x0FFF));
}

struct SectionSpec {
  const char *name;
  std::uint32_t rva;
  std::uint32_t virtualSize;
  std::uint32_t rawPointer;
  std::uint32_t rawSize;
};

struct PeOptions {
  std::uint16_t machine;
  std::uint64_t imageBase;
  std::uint16_t characteristics;
  std::uint32_t sizeOfHeaders;
  std::uint32_t relocRva;
  std::uint32_t relocSize;
  SectionSpec sections[kMaxSections];
  std::uint16_t sectionCount;

  PeOptions()
      : machine(kMachineI386),
        imageBase(0x00400000),
        characteristics(0x0102),
        sizeOfHeaders(0x200),
        relocRva(0),
        relocSize(0),
        sectionCount(0) {
    std::memset(sections, 0, sizeof(sections));
  }
};

// A zero-filled, fixed-capacity in-memory PE image.
struct Image {
  std::uint8_t bytes[kImageCapacity];

  Image() {
    std::memset(bytes, 0, sizeof(bytes));
  }
};

void copySections(PeOptions &dst, const SectionSpec *src,
                  std::uint16_t count) {
  dst.sectionCount = count;
  for (std::uint16_t i = 0; i < count; ++i) {
    dst.sections[i] = src[i];
  }
}

// Construct a minimal, little-endian PE image in a zero-filled buffer.
Image buildPe(const PeOptions &opts) {
  const bool plus = (opts.machine == kMachineAmd64);
  const std::uint16_t optSize = plus ? 0x00F0 : 0x00E0;

  Image img;
  std::uint8_t *b = img.bytes;

  put16(b, 0, 0x5A4D);       // DOS magic
  put32(b, 0x3C, 0x80);      // e_lfanew
  put32(b, 0x80, 0x00004550); // PE signature
  put16(b, 0x84, opts.machine);
  put16(b, 0x86, opts.sectionCount);
  put16(b, 0x94, optSize);
  put16(b, 0x96, opts.characteristics);

  const std::size_t opt = 0x98;
  put16(b, opt, plus ? kMagicPe32Plus : kMagicPe32);
  put32(b, opt + 32, 0x1000); // SectionAlignment
  put32(b, opt + 36, 0x200);  // FileAlignment
  put32(b, opt + 56, 0x5000); // SizeOfImage
  put32(b, opt + 60, opts.sizeOfHeaders);
  put32(b, opt + (plus ? 108 : 92), 16); // NumberOfRvaAndSizes

  if (plus) {
    put64(b, opt + 24, opts.imageBase);
  } else {
    put32(b, opt + 28, static_cast<std::uint32_t>(opts.imageBase));
  }

  const std::size_t dirBase = opt + (plus ? 112 : 96);
  put32(b, dirBase + 5 * 8, opts.relocRva);
  put32(b, dirBase + 5 * 8 + 4, opts.relocSize);

  std::size_t table = opt + optSize;
  for (std::uint16_t i = 0; i < opts.sectionCount; ++i) {
    std::size_t s = table + static_cast<std::size_t>(i) * 40;
    std::memcpy(&b[s], opts.sections[i].name,
                std::strlen(opts.sections[i].name));
    put32(b, s + 8, opts.sections[i].virtualSize);
    put32(b, s + 12, opts.sections[i].rva);
    put32(b, s + 20, opts.sections[i].rawSize);
    put32(b, s + 24, opts.sections[i].rawPointer);
    put32(b, s + 36, 0xC0000040);
  }

  return img;
}

// Write one relocation block; return its total byte length.
std::uint32_t writeBlock(std::uint8_t *b, std::size_t rawOffset,
                         std::uint32_t pageRva,
                         const std::uint16_t *entries,
                         std::uint32_t entryCount) {
  std::uint32_t blockSize = 8 + 2 * entryCount;
  put32(b, rawOffset, pageRva);
  put32(b, rawOffset + 4, blockSize);
  for (std::uint32_t i = 0; i < entryCount; ++i) {
    put16(b, rawOffset + 8 + 2 * i, entries[i]);
  }
  return blockSize;
}

void twoSections(PeOptions &opts) {
  SectionSpec secs[2] = {
      {".text", 0x1000, 0x200, 0x200, 0x200},
      {".reloc", 0x2000, 0x200, 0x400, 0x200}};
  copySections(opts, secs, 2);
}

} // namespace

// ---------------------------------------------------------------------------
// pairs group: type/value pairing semantics, including negative deltas and
// the HIGHADJ paired word.
// ---------------------------------------------------------------------------

static int pairs_i386_negative_delta(void) {
  // delta = 0x003F0000 - 0x00400000 = -0x10000 (delta32 = 0xFFFF0000).
  PeOptions opts;
  twoSections(opts);
  opts.relocRva = 0x2000;
  opts.relocSize = 20;
  Image img = buildPe(opts);
  std::uint8_t *b = img.bytes;

  put32(b, 0x200, 0x00401234); // HIGHLOW target @ RVA 0x1000
  put16(b, 0x204, 0x1234);     // HIGH target    @ RVA 0x1004
  put16(b, 0x206, 0x5678);     // LOW target     @ RVA 0x1006
  put16(b, 0x208, 0x1234);     // HIGHADJ target @ RVA 0x1008

  std::uint16_t entries[6];
  entries[0] = relocWord(3, 0x000);
  entries[1] = relocWord(1, 0x004);
  entries[2] = relocWord(2, 0x006);
  entries[3] = relocWord(4, 0x008);
  entries[4] = 0x8001; // paired word: int16 -32767
  entries[5] = 0x0000; // ABSOLUTE padding
  writeBlock(b, 0x400, 0x1000, entries, 6);

  RelocPreviewResult r =
      BuildBaseRelocationPreview(b, kImageCapacity, 0x003F0000);
  TASK_CHECK(r.ok);
  TASK_CHECK(r.report != nullptr);
  TASK_CHECK(r.report->baseDelta == -0x10000);
  TASK_CHECK(r.report->oldImageBase == 0x00400000);
  TASK_CHECK(r.report->newImageBase == 0x003F0000);
  TASK_CHECK(r.report->blockCount == 1);
  TASK_CHECK(r.report->blocks[0].operationCount == 5);

  const peparse::RelocPreviewOperation *ops =
      r.report->blocks[0].operations;

  TASK_CHECK(ops[0].kind == RelocPreviewKind_HighLow);
  TASK_CHECK(ops[0].width == RelocPreviewWidth_Dword32);
  TASK_CHECK(ops[0].targetRva == 0x1000);
  TASK_CHECK(get32(ops[0].beforeBytes, 0) == 0x00401234);
  TASK_CHECK(get32(ops[0].afterBytes, 0) == 0x003F1234);
  TASK_CHECK(ops[0].entryFileOffset == 0x408);
  TASK_CHECK(ops[0].rawEntry == relocWord(3, 0x000));
  TASK_CHECK(ops[0].targetFragmentCount == 1);
  TASK_CHECK(ops[0].targetFragments[0].sectionIndex == 0);
  TASK_CHECK(ops[0].targetFragments[0].fileOffset == 0x200);
  TASK_CHECK(ops[0].entryFragmentCount == 1);
  TASK_CHECK(ops[0].entryFragments[0].fileOffset == 0x408);

  TASK_CHECK(ops[1].kind == RelocPreviewKind_High);
  TASK_CHECK(get16(ops[1].beforeBytes, 0) == 0x1234);
  TASK_CHECK(get16(ops[1].afterBytes, 0) == 0x1233);

  TASK_CHECK(ops[2].kind == RelocPreviewKind_Low);
  TASK_CHECK(get16(ops[2].beforeBytes, 0) == 0x5678);
  TASK_CHECK(get16(ops[2].afterBytes, 0) == 0x5678);

  TASK_CHECK(ops[3].kind == RelocPreviewKind_HighAdj);
  TASK_CHECK(ops[3].hasPair);
  TASK_CHECK(ops[3].pairRawValue == 0x8001);
  TASK_CHECK(ops[3].pairFileOffset == 0x410);
  TASK_CHECK(ops[3].pairFragmentCount == 1);
  TASK_CHECK(ops[3].pairFragments[0].fileOffset == 0x410);
  // ((0x1234<<16) + (-32767) + (-0x10000) + 0x8000) >> 16 == 0x1233
  TASK_CHECK(get16(ops[3].beforeBytes, 0) == 0x1234);
  TASK_CHECK(get16(ops[3].afterBytes, 0) == 0x1233);

  TASK_CHECK(ops[4].kind == RelocPreviewKind_Absolute);
  TASK_CHECK(ops[4].ordinal == 4);
  TASK_CHECK(ops[4].entryIndex == 5);

  TASK_CHECK(get32(b, 0x200) == 0x00401234);
  TASK_CHECK(get16(b, 0x208) == 0x1234);
  return 0;
}

static int pairs_highadj_rounding(void) {
  PeOptions opts;
  twoSections(opts);
  opts.relocRva = 0x2000;
  opts.relocSize = 20;
  Image img = buildPe(opts);
  std::uint8_t *b = img.bytes;

  put16(b, 0x200, 0x0000); // h0 @ RVA 0x1000
  put16(b, 0x202, 0x0000); // h1 @ RVA 0x1002
  put16(b, 0x204, 0x0001); // h2 @ RVA 0x1004

  std::uint16_t entries[6];
  entries[0] = relocWord(4, 0x000);
  entries[1] = 0x7FFF; // l0 = +32767
  entries[2] = relocWord(4, 0x002);
  entries[3] = 0x8000; // l1 = -32768
  entries[4] = relocWord(4, 0x004);
  entries[5] = 0xFFFF; // l2 = -1
  writeBlock(b, 0x400, 0x1000, entries, 6);

  RelocPreviewResult r =
      BuildBaseRelocationPreview(b, kImageCapacity, 0x00400001);
  TASK_CHECK(r.ok);
  TASK_CHECK(r.report->baseDelta == 1);
  const peparse::RelocPreviewOperation *ops =
      r.report->blocks[0].operations;
  TASK_CHECK(r.report->blocks[0].operationCount == 3);

  // h0=0, l0=+32767, delta=+1: (32767+1+32768)>>16 = 1
  TASK_CHECK(ops[0].kind == RelocPreviewKind_HighAdj);
  TASK_CHECK(ops[0].pairRawValue == 0x7FFF);
  TASK_CHECK(get16(ops[0].afterBytes, 0) == 0x0001);

  // Negative delta: x = 0 - 32768 - 1 + 32768 = -1;
  // floor(-1 / 65536) = -1 -> 0xFFFF.
  RelocPreviewResult r2 =
      BuildBaseRelocationPreview(b, kImageCapacity, 0x003FFFFF);
  TASK_CHECK(r2.ok);
  TASK_CHECK(r2.report->baseDelta == -1);
  const peparse::RelocPreviewOperation *ops2 =
      r2.report->blocks[0].operations;
  TASK_CHECK(get16(ops2[1].afterBytes, 0) == 0xFFFF);
  // h2=1, l2=-1, delta=-1: (0x10000 - 1 - 1 + 0x8000) = 0x17FFE -> 1
  TASK_CHECK(get16(ops2[2].afterBytes, 0) == 0x0001);
  // h0=0, l0=+32767, delta=-1: (32767 - 1 + 32768) = 0xFFFF -> 0
  TASK_CHECK(get16(ops2[0].afterBytes, 0) == 0x0000);
  return 0;
}

static int pairs_pair_nibble_not_a_type(void) {
  // The paired word 0xF234 looks like type 15 in its high nibble, but
  // HIGHADJ owns the whole word; it is never decoded as an entry.
  PeOptions opts;
  twoSections(opts);
  opts.relocRva = 0x2000;
  opts.relocSize = 16;
  Image img = buildPe(opts);
  std::uint8_t *b = img.bytes;

  put16(b, 0x200, 0x4242);

  std::uint16_t entries[4];
  entries[0] = relocWord(4, 0x000);
  entries[1] = 0xF234; // paired word, nibble 0xF
  entries[2] = 0x0000; // ABSOLUTE
  entries[3] = 0x0000; // ABSOLUTE (4-byte padding)
  writeBlock(b, 0x400, 0x1000, entries, 4);

  RelocPreviewResult r =
      BuildBaseRelocationPreview(b, kImageCapacity, 0x00401000);
  TASK_CHECK(r.ok);
  const peparse::RelocPreviewOperation *ops =
      r.report->blocks[0].operations;
  TASK_CHECK(r.report->blocks[0].operationCount == 3);
  TASK_CHECK(ops[0].kind == RelocPreviewKind_HighAdj);
  TASK_CHECK(ops[0].hasPair);
  TASK_CHECK(ops[0].pairRawValue == 0xF234);
  TASK_CHECK(ops[0].pairFileOffset == 0x40A);
  TASK_CHECK(ops[1].kind == RelocPreviewKind_Absolute);
  TASK_CHECK(ops[1].entryIndex == 2);
  TASK_CHECK(ops[2].kind == RelocPreviewKind_Absolute);
  TASK_CHECK(ops[2].entryIndex == 3);
  return 0;
}

static int pairs_amd64_dir64(void) {
  PeOptions opts;
  opts.machine = kMachineAmd64;
  opts.imageBase = 0x0000000140000000ULL;
  twoSections(opts);
  opts.relocRva = 0x2000;
  opts.relocSize = 16;
  Image img = buildPe(opts);
  std::uint8_t *b = img.bytes;

  put64(b, 0x200, 0x0000000140001234ULL); // DIR64 @ RVA 0x1000
  put64(b, 0x208, 0x0000000140005678ULL); // DIR64 @ RVA 0x1008

  std::uint16_t entries[4];
  entries[0] = relocWord(10, 0x000);
  entries[1] = relocWord(10, 0x008);
  entries[2] = 0x0000;
  entries[3] = 0x0000;
  writeBlock(b, 0x400, 0x1000, entries, 4);

  RelocPreviewResult pos = BuildBaseRelocationPreview(
      b, kImageCapacity, 0x0000000180000000ULL);
  TASK_CHECK(pos.ok);
  TASK_CHECK(pos.report->pe32Plus);
  TASK_CHECK(pos.report->baseDelta == 0x40000000);
  const peparse::RelocPreviewOperation *pops =
      pos.report->blocks[0].operations;
  TASK_CHECK(pos.report->blocks[0].operationCount == 4);
  TASK_CHECK(pops[0].kind == RelocPreviewKind_Dir64);
  TASK_CHECK(pops[0].width == RelocPreviewWidth_Qword64);
  TASK_CHECK(get64(pops[0].beforeBytes, 0) == 0x0000000140001234ULL);
  TASK_CHECK(get64(pops[0].afterBytes, 0) == 0x0000000180001234ULL);
  TASK_CHECK(get64(pops[1].afterBytes, 0) == 0x0000000180005678ULL);

  RelocPreviewResult neg = BuildBaseRelocationPreview(
      b, kImageCapacity, 0x0000000100000000ULL);
  TASK_CHECK(neg.ok);
  TASK_CHECK(neg.report->baseDelta == -0x40000000LL);
  const peparse::RelocPreviewOperation *nops =
      neg.report->blocks[0].operations;
  TASK_CHECK(get64(nops[0].afterBytes, 0) == 0x0000000100001234ULL);
  TASK_CHECK(get64(nops[1].afterBytes, 0) == 0x0000000100005678ULL);
  return 0;
}

static int pairs_absolute_is_padding(void) {
  PeOptions opts;
  twoSections(opts);
  opts.relocRva = 0x2000;
  opts.relocSize = 16;
  Image img = buildPe(opts);
  std::uint8_t *b = img.bytes;

  std::uint16_t entries[4];
  entries[0] = 0x0000; // ABSOLUTE
  entries[1] = relocWord(3, 0x010); // HIGHLOW @ RVA 0x1010
  entries[2] = 0x0000; // ABSOLUTE
  entries[3] = 0x0000; // ABSOLUTE
  writeBlock(b, 0x400, 0x1000, entries, 4);
  put32(b, 0x210, 0xDEADBEEF);

  RelocPreviewResult r =
      BuildBaseRelocationPreview(b, kImageCapacity, 0x00400000);
  TASK_CHECK(r.ok);
  TASK_CHECK(r.report->baseDelta == 0);
  const peparse::RelocPreviewOperation *ops =
      r.report->blocks[0].operations;
  TASK_CHECK(r.report->blocks[0].operationCount == 4);

  TASK_CHECK(ops[0].kind == RelocPreviewKind_Absolute);
  TASK_CHECK(ops[0].width == RelocPreviewWidth_None);
  TASK_CHECK(ops[0].beforeByteCount == 0);
  TASK_CHECK(ops[0].afterByteCount == 0);
  TASK_CHECK(ops[0].targetFragmentCount == 0);
  TASK_CHECK(ops[0].hasPair == false);

  TASK_CHECK(ops[1].kind == RelocPreviewKind_HighLow);
  TASK_CHECK(get32(ops[1].beforeBytes, 0) == 0xDEADBEEF);
  TASK_CHECK(get32(ops[1].afterBytes, 0) == 0xDEADBEEF);

  TASK_CHECK(ops[2].kind == RelocPreviewKind_Absolute);
  TASK_CHECK(ops[3].kind == RelocPreviewKind_Absolute);
  return 0;
}

// ---------------------------------------------------------------------------
// overlay group: sequential semantics, overlapping fields and physical
// byte aliasing.
// ---------------------------------------------------------------------------

static int overlay_repeat_target(void) {
  // The same RVA is relocated twice; the second read sees the bytes
  // previewed by the first.
  PeOptions opts;
  twoSections(opts);
  opts.relocRva = 0x2000;
  opts.relocSize = 16;
  Image img = buildPe(opts);
  std::uint8_t *b = img.bytes;

  put32(b, 0x200, 0x00000010);

  std::uint16_t entries[4];
  entries[0] = relocWord(3, 0x000);
  entries[1] = relocWord(3, 0x000);
  entries[2] = 0x0000;
  entries[3] = 0x0000;
  writeBlock(b, 0x400, 0x1000, entries, 4);

  RelocPreviewResult r =
      BuildBaseRelocationPreview(b, kImageCapacity, 0x00400020);
  TASK_CHECK(r.ok);
  TASK_CHECK(r.report->baseDelta == 0x20);
  const peparse::RelocPreviewOperation *ops =
      r.report->blocks[0].operations;
  TASK_CHECK(r.report->blocks[0].operationCount == 4);
  TASK_CHECK(get32(ops[0].beforeBytes, 0) == 0x10);
  TASK_CHECK(get32(ops[0].afterBytes, 0) == 0x30);
  TASK_CHECK(get32(ops[1].beforeBytes, 0) == 0x30);
  TASK_CHECK(get32(ops[1].afterBytes, 0) == 0x50);
  TASK_CHECK(ops[0].ordinal == 0);
  TASK_CHECK(ops[1].ordinal == 1);

  TASK_CHECK(r.report->intervalCount == 1);
  const peparse::RelocPreviewInterval &iv = r.report->intervals[0];
  TASK_CHECK(iv.firstRva == 0x1000);
  TASK_CHECK(iv.lastRva == 0x1003);
  TASK_CHECK(get32(iv.originalBytes, 0) == 0x10);
  TASK_CHECK(get32(iv.finalBytes, 0) == 0x50);
  return 0;
}

static int overlay_partial_overlap(void) {
  // delta32 = 0x00010002: low half 0x0002, high half 0x0001.
  PeOptions opts;
  twoSections(opts);
  opts.relocRva = 0x2000;
  opts.relocSize = 16;
  Image img = buildPe(opts);
  std::uint8_t *b = img.bytes;

  put32(b, 0x200, 0x11223344); // field @ RVA 0x1000..0x1003

  std::uint16_t entries[4];
  entries[0] = relocWord(3, 0x000); // HIGHLOW @ 0x1000
  entries[1] = relocWord(2, 0x002); // LOW     @ 0x1002
  entries[2] = relocWord(1, 0x002); // HIGH    @ 0x1002
  entries[3] = 0x0000;
  writeBlock(b, 0x400, 0x1000, entries, 4);

  RelocPreviewResult r =
      BuildBaseRelocationPreview(b, kImageCapacity, 0x00410002);
  TASK_CHECK(r.ok);
  const peparse::RelocPreviewOperation *ops =
      r.report->blocks[0].operations;
  TASK_CHECK(r.report->blocks[0].operationCount == 4);

  // HIGHLOW: 0x11223344 + 0x10002 = 0x11233346
  TASK_CHECK(get32(ops[0].afterBytes, 0) == 0x11233346);
  // LOW reads the previewed upper word 0x1123, adds 0x0002 -> 0x1125
  TASK_CHECK(get16(ops[1].beforeBytes, 0) == 0x1123);
  TASK_CHECK(get16(ops[1].afterBytes, 0) == 0x1125);
  // HIGH reads that result 0x1125, adds 0x0001 -> 0x1126
  TASK_CHECK(get16(ops[2].beforeBytes, 0) == 0x1125);
  TASK_CHECK(get16(ops[2].afterBytes, 0) == 0x1126);

  TASK_CHECK(r.report->intervalCount == 1);
  const peparse::RelocPreviewInterval &iv = r.report->intervals[0];
  TASK_CHECK(iv.firstRva == 0x1000);
  TASK_CHECK(iv.lastRva == 0x1003);
  TASK_CHECK(get32(iv.originalBytes, 0) == 0x11223344);
  TASK_CHECK(get32(iv.finalBytes, 0) == 0x11263346);
  TASK_CHECK(iv.operationOrdinalCount == 3);
  TASK_CHECK(iv.operationOrdinals[0] == 0);
  TASK_CHECK(iv.operationOrdinals[1] == 1);
  TASK_CHECK(iv.operationOrdinals[2] == 2);

  TASK_CHECK(get32(b, 0x200) == 0x11223344);
  return 0;
}

static int overlay_physical_alias(void) {
  // Two sections at different RVAs alias the same physical file bytes.
  // Preview writes are keyed by RVA, so the operations never cross.
  SectionSpec secs[3];
  secs[0] = {".text", 0x1000, 0x100, 0x200, 0x100};
  secs[1] = {".alias", 0x2000, 0x100, 0x200, 0x100};
  secs[2] = {".reloc", 0x3000, 0x100, 0x400, 0x100};

  PeOptions opts;
  copySections(opts, secs, 3);
  opts.relocRva = 0x3000;
  opts.relocSize = 32;
  Image img = buildPe(opts);
  std::uint8_t *b = img.bytes;

  put32(b, 0x200, 0xAABBCCDD); // physical bytes shared by both RVAs

  std::uint16_t first[4];
  first[0] = relocWord(3, 0x000); // RVA 0x1000
  first[1] = 0x0000;
  first[2] = 0x0000;
  first[3] = 0x0000;
  writeBlock(b, 0x400, 0x1000, first, 4);

  std::uint16_t second[4];
  second[0] = relocWord(3, 0x000); // RVA 0x2000
  second[1] = 0x0000;
  second[2] = 0x0000;
  second[3] = 0x0000;
  writeBlock(b, 0x410, 0x2000, second, 4);

  RelocPreviewResult r =
      BuildBaseRelocationPreview(b, kImageCapacity, 0x00400111);
  TASK_CHECK(r.ok);
  TASK_CHECK(r.report->blockCount == 2);

  const peparse::RelocPreviewOperation *firstOps =
      r.report->blocks[0].operations;
  const peparse::RelocPreviewOperation *secondOps =
      r.report->blocks[1].operations;
  TASK_CHECK(firstOps[0].targetRva == 0x1000);
  TASK_CHECK(secondOps[0].targetRva == 0x2000);

  TASK_CHECK(get32(firstOps[0].beforeBytes, 0) == 0xAABBCCDD);
  // 0xAABBCCDD + delta 0x111 = 0xAABBCDEE.
  TASK_CHECK(get32(firstOps[0].afterBytes, 0) == 0xAABBCDEE);
  // The aliased operation reads frozen original bytes, not the first
  // operation's preview.
  TASK_CHECK(get32(secondOps[0].beforeBytes, 0) == 0xAABBCCDD);
  TASK_CHECK(get32(secondOps[0].afterBytes, 0) == 0xAABBCDEE);

  TASK_CHECK(r.report->intervalCount == 2);
  TASK_CHECK(r.report->intervals[0].firstRva == 0x1000);
  TASK_CHECK(r.report->intervals[1].firstRva == 0x2000);
  TASK_CHECK(get32(r.report->intervals[0].originalBytes, 0) ==
             0xAABBCCDD);
  TASK_CHECK(get32(r.report->intervals[1].originalBytes, 0) ==
             0xAABBCCDD);
  TASK_CHECK(get32(r.report->intervals[0].finalBytes, 0) ==
             0xAABBCDEE);
  TASK_CHECK(get32(r.report->intervals[1].finalBytes, 0) ==
             0xAABBCDEE);
  TASK_CHECK(get32(b, 0x200) == 0xAABBCCDD);
  return 0;
}

static int overlay_interval_order(void) {
  // Blocks appear in directory order page 0x2000 then page 0x1000, but
  // merged intervals must be sorted by RVA.
  SectionSpec secs[3];
  secs[0] = {".text", 0x1000, 0x200, 0x200, 0x200};
  secs[1] = {".data", 0x2000, 0x200, 0x400, 0x200};
  secs[2] = {".reloc", 0x3000, 0x200, 0x600, 0x200};

  PeOptions opts;
  copySections(opts, secs, 3);
  opts.relocRva = 0x3000;
  opts.relocSize = 32;
  Image img = buildPe(opts);
  std::uint8_t *b = img.bytes;

  std::uint16_t blockA[4];
  blockA[0] = relocWord(3, 0x005); // RVA 0x2005
  blockA[1] = 0x0000;
  blockA[2] = 0x0000;
  blockA[3] = 0x0000;
  writeBlock(b, 0x600, 0x2000, blockA, 4);

  std::uint16_t blockB[4];
  blockB[0] = relocWord(3, 0x010); // RVA 0x1010
  blockB[1] = relocWord(3, 0x000); // RVA 0x1000
  blockB[2] = 0x0000;
  blockB[3] = 0x0000;
  writeBlock(b, 0x610, 0x1000, blockB, 4);

  RelocPreviewResult r =
      BuildBaseRelocationPreview(b, kImageCapacity, 0x00400001);
  TASK_CHECK(r.ok);
  TASK_CHECK(r.report->blockCount == 2);
  TASK_CHECK(r.report->blocks[0].pageRva == 0x2000);
  TASK_CHECK(r.report->blocks[1].pageRva == 0x1000);

  TASK_CHECK(r.report->intervalCount == 3);
  TASK_CHECK(r.report->intervals[0].firstRva == 0x1000);
  TASK_CHECK(r.report->intervals[1].firstRva == 0x1010);
  TASK_CHECK(r.report->intervals[2].firstRva == 0x2005);
  // Ordinals follow directory/block order; ABSOLUTE entries consume
  // ordinals too. Block A: HIGHLOW=0 (then three ABSOLUTE = 1,2,3),
  // block B: 0x1010 HIGHLOW=4, 0x1000 HIGHLOW=5.
  TASK_CHECK(r.report->intervals[0].operationOrdinals[0] == 5);
  TASK_CHECK(r.report->intervals[1].operationOrdinals[0] == 4);
  TASK_CHECK(r.report->intervals[2].operationOrdinals[0] == 0);
  return 0;
}

// ---------------------------------------------------------------------------
// mapping group: section resolution, directory offsets, rejection paths
// and report lifetime.
// ---------------------------------------------------------------------------

static int mapping_cross_section_dir64(void) {
  // A DIR64 field at RVA 0x10FC straddles two adjacent sections.
  SectionSpec secs[3];
  secs[0] = {".text", 0x1000, 0x100, 0x200, 0x100};
  secs[1] = {".data", 0x1100, 0x100, 0x300, 0x100};
  secs[2] = {".reloc", 0x3000, 0x100, 0x600, 0x100};

  PeOptions opts;
  opts.machine = kMachineAmd64;
  opts.imageBase = 0x0000000140000000ULL;
  copySections(opts, secs, 3);
  opts.relocRva = 0x3000;
  opts.relocSize = 16;
  Image img = buildPe(opts);
  std::uint8_t *b = img.bytes;

  put32(b, 0x2FC, 0x11111111); // low half in .text
  put32(b, 0x300, 0x22222222); // high half in .data

  std::uint16_t entries[4];
  entries[0] = relocWord(10, 0x0FC);
  entries[1] = 0x0000;
  entries[2] = 0x0000;
  entries[3] = 0x0000;
  writeBlock(b, 0x600, 0x1000, entries, 4);

  RelocPreviewResult r = BuildBaseRelocationPreview(
      b, kImageCapacity, 0x0000000140000100ULL);
  TASK_CHECK(r.ok);
  const peparse::RelocPreviewOperation &op =
      r.report->blocks[0].operations[0];
  TASK_CHECK(op.targetRva == 0x10FC);
  TASK_CHECK(op.targetFragmentCount == 2);
  TASK_CHECK(op.targetFragments[0].sectionIndex == 0);
  TASK_CHECK(op.targetFragments[0].fileOffset == 0x2FC);
  TASK_CHECK(op.targetFragments[0].byteCount == 4);
  TASK_CHECK(op.targetFragments[1].sectionIndex == 1);
  TASK_CHECK(op.targetFragments[1].fileOffset == 0x300);
  TASK_CHECK(op.targetFragments[1].byteCount == 4);

  TASK_CHECK(get64(op.beforeBytes, 0) == 0x2222222211111111ULL);
  TASK_CHECK(get64(op.afterBytes, 0) == 0x2222222211111211ULL);
  return 0;
}

static int mapping_cross_section_highlow(void) {
  // A HIGHLOW field at RVA 0x10FE straddles .text and .data (PE32).
  SectionSpec secs[3];
  secs[0] = {".text", 0x1000, 0x100, 0x200, 0x100};
  secs[1] = {".data", 0x1100, 0x100, 0x300, 0x100};
  secs[2] = {".reloc", 0x3000, 0x100, 0x600, 0x100};

  PeOptions opts;
  copySections(opts, secs, 3);
  opts.relocRva = 0x3000;
  opts.relocSize = 16;
  Image img = buildPe(opts);
  std::uint8_t *b = img.bytes;

  put16(b, 0x2FE, 0x3344); // low half in .text
  put16(b, 0x300, 0x1122); // high half in .data

  std::uint16_t entries[4];
  entries[0] = relocWord(3, 0x0FE);
  entries[1] = 0x0000;
  entries[2] = 0x0000;
  entries[3] = 0x0000;
  writeBlock(b, 0x600, 0x1000, entries, 4);

  RelocPreviewResult r =
      BuildBaseRelocationPreview(b, kImageCapacity, 0x00410002);
  TASK_CHECK(r.ok);
  const peparse::RelocPreviewOperation &op =
      r.report->blocks[0].operations[0];
  TASK_CHECK(op.targetFragmentCount == 2);
  TASK_CHECK(op.targetFragments[0].sectionIndex == 0);
  TASK_CHECK(op.targetFragments[0].fileOffset == 0x2FE);
  TASK_CHECK(op.targetFragments[1].sectionIndex == 1);
  TASK_CHECK(op.targetFragments[1].fileOffset == 0x300);
  TASK_CHECK(get32(op.beforeBytes, 0) == 0x11223344);
  TASK_CHECK(get32(op.afterBytes, 0) == 0x11233346);
  return 0;
}

static int mapping_directory_offset(void) {
  // The directory starts 8 bytes inside .reloc and holds two blocks.
  SectionSpec secs[3];
  secs[0] = {".text", 0x1000, 0x200, 0x200, 0x200};
  secs[1] = {".data", 0x2000, 0x200, 0x400, 0x200};
  secs[2] = {".reloc", 0x3000, 0x200, 0x600, 0x200};

  PeOptions opts;
  copySections(opts, secs, 3);
  opts.relocRva = 0x3008;
  opts.relocSize = 40;
  Image img = buildPe(opts);
  std::uint8_t *b = img.bytes;

  std::uint16_t blockA[6];
  blockA[0] = relocWord(3, 0x000);
  blockA[1] = 0x0000;
  blockA[2] = 0x0000;
  blockA[3] = 0x0000;
  blockA[4] = 0x0000;
  blockA[5] = 0x0000;
  writeBlock(b, 0x608, 0x1000, blockA, 6);

  std::uint16_t blockB[6];
  blockB[0] = relocWord(3, 0x010);
  blockB[1] = 0x0000;
  blockB[2] = 0x0000;
  blockB[3] = 0x0000;
  blockB[4] = 0x0000;
  blockB[5] = 0x0000;
  writeBlock(b, 0x61C, 0x2000, blockB, 6);

  RelocPreviewResult r =
      BuildBaseRelocationPreview(b, kImageCapacity, 0x00400004);
  TASK_CHECK(r.ok);
  TASK_CHECK(r.report->directoryPresent);
  TASK_CHECK(r.report->directoryRva == 0x3008);
  TASK_CHECK(r.report->directorySize == 40);
  TASK_CHECK(r.report->blockCount == 2);

  TASK_CHECK(r.report->blocks[0].directoryOffset == 0);
  TASK_CHECK(r.report->blocks[0].headerFileOffset == 0x608);
  TASK_CHECK(r.report->blocks[0].headerFragmentCount == 1);
  TASK_CHECK(r.report->blocks[0].headerFragments[0].sectionIndex == 2);
  TASK_CHECK(r.report->blocks[0].pageRva == 0x1000);

  TASK_CHECK(r.report->blocks[1].directoryOffset == 20);
  TASK_CHECK(r.report->blocks[1].headerFileOffset == 0x61C);
  TASK_CHECK(r.report->blocks[1].pageRva == 0x2000);

  TASK_CHECK(r.report->blocks[0].operations[0].entryFileOffset == 0x610);
  TASK_CHECK(r.report->blocks[1].operations[0].targetRva == 0x2010);
  return 0;
}

static int mapping_gap_virtual_header(void) {
  // Gap between two raw-backed sections.
  SectionSpec gapSecs[3];
  gapSecs[0] = {".text", 0x1000, 0x100, 0x200, 0x100};
  gapSecs[1] = {".data", 0x1200, 0x100, 0x300, 0x100};
  gapSecs[2] = {".reloc", 0x3000, 0x100, 0x600, 0x100};
  PeOptions gap;
  copySections(gap, gapSecs, 3);
  gap.relocRva = 0x3000;
  gap.relocSize = 16;
  Image gimg = buildPe(gap);
  std::uint16_t e[4];
  e[0] = relocWord(3, 0x180); // target RVA 0x1180 is in the gap
  e[1] = 0x0000;
  e[2] = 0x0000;
  e[3] = 0x0000;
  writeBlock(gimg.bytes, 0x600, 0x1000, e, 4);
  RelocPreviewResult gr = BuildBaseRelocationPreview(
      gimg.bytes, kImageCapacity, 0x00400001);
  TASK_CHECK(!gr.ok);
  TASK_CHECK(gr.error.category == RelocPreviewError_Mapping);
  TASK_CHECK(gr.error.hasRva);
  TASK_CHECK(gr.error.rva == 0x1180);
  TASK_CHECK(gr.report == nullptr);

  // Virtual-only section tail.
  SectionSpec vsecs[3];
  vsecs[0] = {".text", 0x1000, 0x100, 0x200, 0x100};
  vsecs[1] = {".bss", 0x2000, 0x100, 0x000, 0x000};
  vsecs[2] = {".reloc", 0x3000, 0x100, 0x600, 0x100};
  PeOptions vo;
  copySections(vo, vsecs, 3);
  vo.relocRva = 0x3000;
  vo.relocSize = 16;
  Image vimg = buildPe(vo);
  writeBlock(vimg.bytes, 0x600, 0x2000, e, 4);
  RelocPreviewResult vr = BuildBaseRelocationPreview(
      vimg.bytes, kImageCapacity, 0x00400001);
  TASK_CHECK(!vr.ok);
  TASK_CHECK(vr.error.category == RelocPreviewError_Mapping);
  TASK_CHECK(vr.error.hasRva);
  TASK_CHECK(vr.error.rva == 0x2180);

  // Header-resident target: SizeOfHeaders reaches the section raw data.
  PeOptions ho;
  twoSections(ho);
  ho.sizeOfHeaders = 0x400;
  ho.relocRva = 0x2000;
  ho.relocSize = 16;
  Image himg = buildPe(ho);
  writeBlock(himg.bytes, 0x400, 0x1000, e, 4);
  RelocPreviewResult hr = BuildBaseRelocationPreview(
      himg.bytes, kImageCapacity, 0x00400001);
  TASK_CHECK(!hr.ok);
  TASK_CHECK(hr.error.category == RelocPreviewError_Mapping);
  return 0;
}

static int mapping_rejected_forms(void) {
  PeOptions base;
  twoSections(base);
  base.relocRva = 0x2000;
  base.relocSize = 16;

  // Unknown type on I386 (type 5).
  {
    Image img = buildPe(base);
    std::uint16_t e[4];
    e[0] = relocWord(5, 0x000);
    e[1] = 0x0000;
    e[2] = 0x0000;
    e[3] = 0x0000;
    writeBlock(img.bytes, 0x400, 0x1000, e, 4);
    RelocPreviewResult r = BuildBaseRelocationPreview(
        img.bytes, kImageCapacity, 0x00400001);
    TASK_CHECK(!r.ok);
    TASK_CHECK(r.error.category == RelocPreviewError_Entry);
    TASK_CHECK(r.error.hasFileOffset);
    TASK_CHECK(r.error.fileOffset == 0x408);
  }

  // DIR64 is a machine mismatch for I386.
  {
    Image img = buildPe(base);
    std::uint16_t e[4];
    e[0] = relocWord(10, 0x000);
    e[1] = 0x0000;
    e[2] = 0x0000;
    e[3] = 0x0000;
    writeBlock(img.bytes, 0x400, 0x1000, e, 4);
    RelocPreviewResult r = BuildBaseRelocationPreview(
        img.bytes, kImageCapacity, 0x00400001);
    TASK_CHECK(!r.ok);
    TASK_CHECK(r.error.category == RelocPreviewError_Entry);
  }

  // HIGH is a machine mismatch for AMD64.
  {
    PeOptions ao = base;
    ao.machine = kMachineAmd64;
    ao.imageBase = 0x0000000140000000ULL;
    Image img = buildPe(ao);
    std::uint16_t e[4];
    e[0] = relocWord(1, 0x000);
    e[1] = 0x0000;
    e[2] = 0x0000;
    e[3] = 0x0000;
    writeBlock(img.bytes, 0x400, 0x1000, e, 4);
    RelocPreviewResult r = BuildBaseRelocationPreview(
        img.bytes, kImageCapacity, 0x0000000140000001ULL);
    TASK_CHECK(!r.ok);
    TASK_CHECK(r.error.category == RelocPreviewError_Entry);
  }

  // HIGHADJ without a paired word.
  {
    PeOptions po = base;
    po.relocSize = 10;
    Image img = buildPe(po);
    std::uint16_t e[1];
    e[0] = relocWord(4, 0x000);
    writeBlock(img.bytes, 0x400, 0x1000, e, 1);
    RelocPreviewResult r = BuildBaseRelocationPreview(
        img.bytes, kImageCapacity, 0x00400001);
    TASK_CHECK(!r.ok);
    TASK_CHECK(r.error.category == RelocPreviewError_Entry);
    TASK_CHECK(r.error.hasFileOffset);
    TASK_CHECK(r.error.fileOffset == 0x408);
  }

  // Block size exceeds the directory size.
  {
    PeOptions bo = base;
    bo.relocSize = 12;
    Image img = buildPe(bo);
    std::uint16_t e[2];
    e[0] = 0x0000;
    e[1] = 0x0000;
    writeBlock(img.bytes, 0x400, 0x1000, e, 2); // BlockSize 12
    put32(img.bytes, 0x404, 24);                // lie: claim 24 bytes
    RelocPreviewResult r = BuildBaseRelocationPreview(
        img.bytes, kImageCapacity, 0x00400001);
    TASK_CHECK(!r.ok);
    TASK_CHECK(r.error.category == RelocPreviewError_Block);
  }

  // Byte-reversed characteristic.
  {
    PeOptions ro = base;
    ro.characteristics = 0x0080;
    Image img = buildPe(ro);
    RelocPreviewResult r = BuildBaseRelocationPreview(
        img.bytes, kImageCapacity, 0x00400001);
    TASK_CHECK(!r.ok);
    TASK_CHECK(r.error.category == RelocPreviewError_Header);
  }

  // New base wider than a PE32 image.
  {
    Image img = buildPe(base);
    RelocPreviewResult r = BuildBaseRelocationPreview(
        img.bytes, kImageCapacity, 0x100000000ULL);
    TASK_CHECK(!r.ok);
    TASK_CHECK(r.error.category == RelocPreviewError_Base);
  }

  // Target write interval intersects the relocation directory itself.
  {
    Image img = buildPe(base);
    std::uint16_t e[4];
    e[0] = relocWord(3, 0x000); // page 0x2000 -> RVA 0x2000
    e[1] = 0x0000;
    e[2] = 0x0000;
    e[3] = 0x0000;
    writeBlock(img.bytes, 0x400, 0x2000, e, 4);
    RelocPreviewResult r = BuildBaseRelocationPreview(
        img.bytes, kImageCapacity, 0x00400001);
    TASK_CHECK(!r.ok);
    TASK_CHECK(r.error.category == RelocPreviewError_Overlap);
    TASK_CHECK(r.error.hasFileOffset);
    TASK_CHECK(r.error.fileOffset == 0x400);
  }

  // RELOCS_STRIPPED with a non-zero move.
  {
    PeOptions so = base;
    so.characteristics = 0x0102 | 0x0001;
    Image img = buildPe(so);
    RelocPreviewResult r = BuildBaseRelocationPreview(
        img.bytes, kImageCapacity, 0x00400001);
    TASK_CHECK(!r.ok);
    TASK_CHECK(r.error.category == RelocPreviewError_RelocsStripped);
  }

  // RELOCS_STRIPPED with a zero move still walks the whole directory:
  // a malformed block is rejected, a well-formed one accepted.
  {
    PeOptions so = base;
    so.characteristics = 0x0102 | 0x0001;
    Image bad = buildPe(so);
    put32(bad.bytes, 0x404, 24); // directory size stays 16; block 24
    RelocPreviewResult br = BuildBaseRelocationPreview(
        bad.bytes, kImageCapacity, 0x00400000);
    TASK_CHECK(!br.ok);
    TASK_CHECK(br.error.category == RelocPreviewError_Block);

    Image good = buildPe(so);
    std::uint16_t e[4];
    e[0] = relocWord(3, 0x000);
    e[1] = 0x0000;
    e[2] = 0x0000;
    e[3] = 0x0000;
    writeBlock(good.bytes, 0x400, 0x1000, e, 4);
    put32(good.bytes, 0x200, 0x12345678);
    RelocPreviewResult gr = BuildBaseRelocationPreview(
        good.bytes, kImageCapacity, 0x00400000);
    TASK_CHECK(gr.ok);
    TASK_CHECK(get32(gr.report->blocks[0].operations[0].afterBytes, 0) ==
               0x12345678);
  }

  // Missing directory with a requested move: empty report, no guesses.
  {
    PeOptions mo;
    twoSections(mo);
    Image img = buildPe(mo);
    RelocPreviewResult r = BuildBaseRelocationPreview(
        img.bytes, kImageCapacity, 0x00500000);
    TASK_CHECK(r.ok);
    TASK_CHECK(!r.report->directoryPresent);
    TASK_CHECK(r.report->blockCount == 0);
    TASK_CHECK(r.report->intervalCount == 0);
    TASK_CHECK(r.report->baseDelta == 0x100000);
  }

  // A failure leaves the next, well-formed call fully usable.
  {
    Image img = buildPe(base);
    std::uint16_t bad[4];
    bad[0] = relocWord(15, 0x000);
    bad[1] = 0x0000;
    bad[2] = 0x0000;
    bad[3] = 0x0000;
    writeBlock(img.bytes, 0x400, 0x1000, bad, 4);
    RelocPreviewResult failed = BuildBaseRelocationPreview(
        img.bytes, kImageCapacity, 0x00400001);
    TASK_CHECK(!failed.ok);

    std::uint16_t ok[4];
    ok[0] = relocWord(3, 0x000);
    ok[1] = 0x0000;
    ok[2] = 0x0000;
    ok[3] = 0x0000;
    writeBlock(img.bytes, 0x400, 0x1000, ok, 4);
    put32(img.bytes, 0x200, 0x7);
    RelocPreviewResult good = BuildBaseRelocationPreview(
        img.bytes, kImageCapacity, 0x00400003);
    TASK_CHECK(good.ok);
    TASK_CHECK(get32(good.report->blocks[0].operations[0].afterBytes, 0) ==
               0xA);
  }
  return 0;
}

static int mapping_input_lifetime(void) {
  PeOptions opts;
  twoSections(opts);
  opts.relocRva = 0x2000;
  opts.relocSize = 16;

  Image *heap = new Image();
  const PeOptions built = opts;
  *heap = buildPe(built);
  std::uint8_t *b = heap->bytes;
  put32(b, 0x200, 0x00000021);

  std::uint16_t entries[4];
  entries[0] = relocWord(3, 0x000);
  entries[1] = 0x0000;
  entries[2] = 0x0000;
  entries[3] = 0x0000;
  writeBlock(b, 0x400, 0x1000, entries, 4);

  RelocPreviewResult r =
      BuildBaseRelocationPreview(b, kImageCapacity, 0x0040000F);
  TASK_CHECK(r.ok);

  std::uint8_t savedAfter[4];
  std::uint8_t savedOrig[4];
  std::memcpy(savedAfter,
              r.report->blocks[0].operations[0].afterBytes, 4);
  std::memcpy(savedOrig, r.report->intervals[0].originalBytes, 4);
  const peparse::RelocPreviewFragment &frag =
      r.report->blocks[0].operations[0].targetFragments[0];
  TASK_CHECK(get32(savedAfter, 0) == 0x30);
  TASK_CHECK(frag.byteCount == 4);
  TASK_CHECK(frag.fileOffset == 0x200);

  // Release every byte of the input; the report must stay readable.
  delete heap;

  TASK_CHECK(r.report->oldImageBase == 0x00400000);
  TASK_CHECK(bytesEqual(r.report->blocks[0].operations[0].afterBytes,
                        savedAfter, 4));
  TASK_CHECK(bytesEqual(r.report->intervals[0].originalBytes,
                        savedOrig, 4));
  const peparse::RelocPreviewFragment &still =
      r.report->blocks[0].operations[0].targetFragments[0];
  TASK_CHECK(still.byteCount == 4);
  TASK_CHECK(still.fileOffset == 0x200);
  TASK_CHECK(r.report->blocks[0].operations[0].entryFragmentCount == 1);
  TASK_CHECK(get32(r.report->blocks[0].operations[0].afterBytes, 0) ==
             0x30);

  DestroyBaseRelocationPreview(r.report);
  return 0;
}

// ---------------------------------------------------------------------------

const task_case task_cases[] = {
    {"pairs", "i386_negative_delta", pairs_i386_negative_delta},
    {"pairs", "highadj_rounding", pairs_highadj_rounding},
    {"pairs", "pair_nibble_not_a_type", pairs_pair_nibble_not_a_type},
    {"pairs", "amd64_dir64", pairs_amd64_dir64},
    {"pairs", "absolute_is_padding", pairs_absolute_is_padding},

    {"overlay", "repeat_target", overlay_repeat_target},
    {"overlay", "partial_overlap", overlay_partial_overlap},
    {"overlay", "physical_alias", overlay_physical_alias},
    {"overlay", "interval_order", overlay_interval_order},

    {"mapping", "cross_section_dir64", mapping_cross_section_dir64},
    {"mapping", "cross_section_highlow", mapping_cross_section_highlow},
    {"mapping", "directory_offset", mapping_directory_offset},
    {"mapping", "gap_virtual_header", mapping_gap_virtual_header},
    {"mapping", "rejected_forms", mapping_rejected_forms},
    {"mapping", "input_lifetime", mapping_input_lifetime},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
