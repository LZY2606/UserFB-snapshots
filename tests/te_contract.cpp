// TE (UEFI PI Terse Executable) contract tests.
//
// The images built here are synthetic but internally consistent: the TE
// header is followed directly by the section table, and the raw section
// headers keep the file offsets of the original (pre-stripping) PE file.
// A PE file offset X lives at TE offset X - (StrippedSize - 40).
//
// Test cases register themselves into task_cases for task_support/driver.c;
// this file deliberately does not define main().

#include <pe-parse/parse.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "task_support/check.h"
#include "task_support/fixtures.h"

namespace tec {

using Bytes = std::vector<std::uint8_t>;

inline void w16(Bytes &b, std::uint32_t o, std::uint16_t v) {
  b.at(o) = static_cast<std::uint8_t>(v);
  b.at(o + 1) = static_cast<std::uint8_t>(v >> 8);
}

inline void w32(Bytes &b, std::uint32_t o, std::uint32_t v) {
  w16(b, o, static_cast<std::uint16_t>(v));
  w16(b, o + 2, static_cast<std::uint16_t>(v >> 16));
}

inline void w64(Bytes &b, std::uint32_t o, std::uint64_t v) {
  w32(b, o, static_cast<std::uint32_t>(v));
  w32(b, o + 4, static_cast<std::uint32_t>(v >> 32));
}

// Original-file layout, before any stripping.
constexpr std::uint32_t kTextRVA = 0x1000;
constexpr std::uint32_t kTextRaw = 0x200;
constexpr std::uint32_t kTextSize = 0x100;
constexpr std::uint32_t kDataRVA = 0x2000;
constexpr std::uint32_t kDataRaw = 0x300;
constexpr std::uint32_t kDataSize = 0x100;
constexpr std::uint32_t kOriginalEnd = 0x400;

constexpr std::uint32_t kRelocDirRVA = 0x2010; // 0x10 into .rdata
constexpr std::uint32_t kRelocDirSize = 10;
constexpr std::uint32_t kRelocPageRVA = 0x1000;
constexpr std::uint32_t kRelocOffset = 0x20;
constexpr std::uint16_t kRelocType32 = 3;  // IMAGE_REL_BASED_HIGHLOW
constexpr std::uint16_t kRelocType64 = 10; // IMAGE_REL_BASED_DIR64

constexpr std::uint32_t kDebugDirRVA = 0x2030; // 0x30 into .rdata
constexpr std::uint32_t kDebugDirSize = 2 * 28;
constexpr std::uint32_t kDebugPayloadRVA = 0x1040; // 0x40 into .text
constexpr std::uint32_t kDebugPayloadSize = 16;
constexpr std::uint32_t kDebugPayloadRaw = 0x320; // .rdata, original coords
constexpr std::uint32_t kDebugPointerSize = 8;

constexpr std::uint32_t kEntryRVA = 0x1000;
constexpr std::uint16_t kSubsystem = 10; // EFI application

struct Image {
  Bytes bytes;
  bool wide = false;
  std::uint16_t stripped = 0;
  std::uint32_t bias = 0;
  std::uint64_t imageBase = 0;

  std::uint32_t teOffset(std::uint32_t peOffset) const {
    return peOffset - bias;
  }

  std::uint32_t machine() const {
    return wide ? peparse::IMAGE_FILE_MACHINE_AMD64
                : peparse::IMAGE_FILE_MACHINE_I386;
  }

  std::uint16_t relocType() const {
    return wide ? kRelocType64 : kRelocType32;
  }

  std::uint64_t relocTargetVA() const {
    return imageBase + kRelocPageRVA + kRelocOffset;
  }

  std::uint64_t entryVA() const { return imageBase + kEntryRVA; }

  std::uint64_t sectionVA(std::uint32_t rva) const {
    return imageBase + rva;
  }
};

inline Image make(bool wide, std::uint16_t stripped, bool withDirs = true) {
  Image img;
  img.wide = wide;
  img.stripped = stripped;
  img.bias = stripped - 40; // StrippedSize excludes the 40-byte TE header
  img.imageBase = wide ? 0x140000000ULL : 0x400000ULL;
  img.bytes.assign(kOriginalEnd - img.bias, 0);

  Bytes &b = img.bytes;

  // TE image header.
  w16(b, 0, 0x5A56);
  w16(b, 2, static_cast<std::uint16_t>(img.machine()));
  b.at(4) = 2; // NumberOfSections
  b.at(5) = kSubsystem;
  w16(b, 6, stripped);
  w32(b, 8, kEntryRVA);
  w32(b, 12, kTextRVA);
  w64(b, 16, img.imageBase);
  if (withDirs) {
    w32(b, 24, kRelocDirRVA);
    w32(b, 28, kRelocDirSize);
    w32(b, 32, kDebugDirRVA);
    w32(b, 36, kDebugDirSize);
  }

  // Section table, immediately after the 40-byte header.
  auto section = [&](std::uint32_t o, const char *name, std::uint32_t rva,
                     std::uint32_t raw, std::uint32_t size) {
    std::memcpy(b.data() + o, name, std::strlen(name));
    w32(b, o + 8, size);  // VirtualSize
    w32(b, o + 12, rva);  // VirtualAddress
    w32(b, o + 16, size); // SizeOfRawData
    w32(b, o + 20, raw);  // PointerToRawData (original-file coordinate)
    w32(b, o + 36, 0x60000020);
  };
  section(40, ".text", kTextRVA, kTextRaw, kTextSize);
  section(80, ".rdata", kDataRVA, kDataRaw, kDataSize);

  // Entry point byte: .text offset 0, original file offset 0x200.
  b.at(img.teOffset(kTextRaw)) = 0xC3;

  // Debug payload addressed by RVA: .text offset 0x40 (original 0x240).
  std::uint32_t rvaPayload = img.teOffset(kTextRaw + (kDebugPayloadRVA - kTextRVA));
  for (std::uint32_t i = 0; i < kDebugPayloadSize; i++) {
    b.at(rvaPayload + i) = static_cast<std::uint8_t>(0x41 + i);
  }

  // Debug payload addressed only by PointerToRawData: original 0x320.
  std::uint32_t ptrPayload = img.teOffset(kDebugPayloadRaw);
  for (std::uint32_t i = 0; i < kDebugPointerSize; i++) {
    b.at(ptrPayload + i) = static_cast<std::uint8_t>(0x61 + i);
  }

  if (withDirs) {
    // Base relocation directory: starts at .rdata offset 0x10, length 10
    // (block header 8 + one Type/Offset word). A parser that used the
    // directory start offset as its loop bound would read nothing here.
    std::uint32_t reloc = img.teOffset(kDataRaw + (kRelocDirRVA - kDataRVA));
    w32(b, reloc + 0, kRelocPageRVA);
    w32(b, reloc + 4, 10);
    w16(b, reloc + 8,
        static_cast<std::uint16_t>((img.relocType() << 12) | kRelocOffset));

    // Debug directory: two entries. The first resolves its payload through
    // a section RVA (crossing into .text); the second only carries the
    // original-file PointerToRawData.
    std::uint32_t dbg = img.teOffset(kDataRaw + (kDebugDirRVA - kDataRVA));
    w32(b, dbg + 4, 0x11111111); // TimeStamp
    w32(b, dbg + 12, 2);         // Type = CODEVIEW
    w32(b, dbg + 16, kDebugPayloadSize);
    w32(b, dbg + 20, kDebugPayloadRVA);
    w32(b, dbg + 24, 0); // PointerToRawData

    dbg += 28;
    w32(b, dbg + 4, 0x22222222);
    w32(b, dbg + 12, 1); // Type
    w32(b, dbg + 16, kDebugPointerSize);
    w32(b, dbg + 20, 0);               // AddressOfRawData absent
    w32(b, dbg + 24, kDebugPayloadRaw); // original-file coordinate
  }

  return img;
}

struct SecView {
  std::string name;
  peparse::image_section_header raw;
  peparse::VA base = 0;
  std::vector<std::uint8_t> data;
};

struct SecCtx {
  std::vector<SecView> secs;
};

static int collectSec(void *cbd,
                      const peparse::VA &va,
                      const std::string &name,
                      const peparse::image_section_header &raw,
                      const peparse::bounded_buffer *data) {
  SecCtx *ctx = static_cast<SecCtx *>(cbd);
  SecView v;
  v.name = name;
  v.raw = raw;
  v.base = va;
  if (data != nullptr) {
    v.data.assign(data->buf, data->buf + data->bufLen);
  }
  ctx->secs.push_back(v);
  return 0;
}

static const SecView *findSec(const SecCtx &ctx, const std::string &name) {
  for (const SecView &s : ctx.secs) {
    if (s.name == name) {
      return &s;
    }
  }
  return nullptr;
}

struct RelocView {
  peparse::VA va;
  peparse::reloc_type type;
};

struct RelocCtx {
  std::vector<RelocView> relocs;
};

static int collectReloc(void *cbd,
                        const peparse::VA &va,
                        const peparse::reloc_type &type) {
  RelocCtx *ctx = static_cast<RelocCtx *>(cbd);
  RelocView v;
  v.va = va;
  v.type = type;
  ctx->relocs.push_back(v);
  return 0;
}

struct DebugView {
  std::uint32_t type;
  std::vector<std::uint8_t> data;
};

struct DebugCtx {
  std::vector<DebugView> debugs;
};

static int collectDebug(void *cbd,
                        const std::uint32_t &type,
                        const peparse::bounded_buffer *data) {
  DebugCtx *ctx = static_cast<DebugCtx *>(cbd);
  DebugView v;
  v.type = type;
  if (data != nullptr) {
    v.data.assign(data->buf, data->buf + data->bufLen);
  }
  ctx->debugs.push_back(v);
  return 0;
}

// Parse through ParsePEFromPointer; the caller owns the returned object.
static peparse::parsed_pe *parsePointer(Image &img) {
  return peparse::ParsePEFromPointer(img.bytes.data(),
                                     static_cast<std::uint32_t>(img.bytes.size()));
}

static bool writeTemp(const char *path, const Bytes &b) {
  FILE *f = std::fopen(path, "wb");
  if (f == nullptr) {
    return false;
  }
  bool ok = std::fwrite(b.data(), 1, b.size(), f) == b.size();
  std::fclose(f);
  return ok;
}

static bool headerMatches(peparse::parsed_pe *p, const Image &img) {
  peparse::te_header te;
  return peparse::GetTEHeader(p, te) && te.Machine == img.machine() &&
         te.NumberOfSections == 2 && te.Subsystem == kSubsystem &&
         te.StrippedSize == img.stripped &&
         te.AddressOfEntryPoint == kEntryRVA && te.BaseOfCode == kTextRVA &&
         te.ImageBase == img.imageBase &&
         te.DataDirectory[0].VirtualAddress == kRelocDirRVA &&
         te.DataDirectory[0].Size == kRelocDirSize &&
         te.DataDirectory[1].VirtualAddress == kDebugDirRVA &&
         te.DataDirectory[1].Size == kDebugDirSize;
}

static bool parseFails(const Bytes &bytes) {
  peparse::parsed_pe *p = peparse::ParsePEFromPointer(
      const_cast<std::uint8_t *>(bytes.data()),
      static_cast<std::uint32_t>(bytes.size()));
  if (p != nullptr) {
    peparse::DestructParsedPE(p);
    return false;
  }
  return peparse::GetPEErr() != 0;
}

// ---------------------------------------------------------------------------
// coordinates
// ---------------------------------------------------------------------------

static int coordinates(void) {
  const std::uint16_t strips[] = {0x1a0, 0x1b0};
  for (int w = 0; w < 2; w++) {
    const bool wide = w != 0;
    for (std::uint16_t stripped : strips) {
      Image img = make(wide, stripped);
      peparse::parsed_pe *p = parsePointer(img);
      TASK_CHECK(p != nullptr);
      TASK_CHECK(headerMatches(p, img));

      peparse::VA ep = 0;
      TASK_CHECK(peparse::GetEntryPoint(p, ep) && ep == img.entryVA());

      std::uint8_t byte = 0;
      TASK_CHECK(peparse::ReadByteAtVA(p, ep, byte) && byte == 0xC3);

      const char *machine = peparse::GetMachineAsString(p);
      TASK_CHECK(machine != nullptr);
      TASK_CHECK(std::strcmp(machine, wide ? "x64" : "x86") == 0);
      const char *subsystem = peparse::GetSubsystemAsString(p);
      TASK_CHECK(subsystem != nullptr);
      TASK_CHECK(std::strcmp(subsystem, "EFI_APPLICATION") == 0);

      // parsed_pe::fileBuffer is the original TE file, length and contents.
      TASK_CHECK(p->fileBuffer != nullptr);
      TASK_CHECK(p->fileBuffer->bufLen == img.bytes.size());
      TASK_CHECK(std::memcmp(p->fileBuffer->buf, img.bytes.data(),
                             img.bytes.size()) == 0);

      peparse::DestructParsedPE(p);
    }
  }
  return 0;
}

static int sectionSlices(void) {
  const std::uint16_t strips[] = {0x1a0, 0x1b0};
  for (int w = 0; w < 2; w++) {
    const bool wide = w != 0;
    for (std::uint16_t stripped : strips) {
      Image img = make(wide, stripped);
      peparse::parsed_pe *p = parsePointer(img);
      TASK_CHECK(p != nullptr);

      SecCtx ctx;
      peparse::IterSec(p, collectSec, &ctx);
      TASK_CHECK(ctx.secs.size() == 2);

      const SecView *text = findSec(ctx, ".text");
      const SecView *data = findSec(ctx, ".rdata");
      TASK_CHECK(text != nullptr && data != nullptr);

      // VA convention: original ImageBase + original RVA.
      TASK_CHECK(text->base == img.sectionVA(kTextRVA));
      TASK_CHECK(data->base == img.sectionVA(kDataRVA));

      // Raw section headers are preserved exactly, including the stripped
      // file coordinate.
      TASK_CHECK(text->raw.VirtualAddress == kTextRVA);
      TASK_CHECK(text->raw.PointerToRawData == kTextRaw);
      TASK_CHECK(text->raw.SizeOfRawData == kTextSize);
      TASK_CHECK(data->raw.VirtualAddress == kDataRVA);
      TASK_CHECK(data->raw.PointerToRawData == kDataRaw);

      // The data slice must be the actual TE file range, not a rewritten
      // offset.
      TASK_CHECK(text->data.size() == kTextSize);
      TASK_CHECK(data->data.size() == kDataSize);
      std::uint32_t textStart = img.teOffset(kTextRaw);
      TASK_CHECK(std::memcmp(text->data.data(), img.bytes.data() + textStart,
                             kTextSize) == 0);
      TASK_CHECK(text->data[0] == 0xC3);

      std::uint32_t dataStart = img.teOffset(kDataRaw);
      TASK_CHECK(std::memcmp(data->data.data(), img.bytes.data() + dataStart,
                             kDataSize) == 0);

      // The entry byte read by VA shares the same slice.
      TASK_CHECK(text->data[kEntryRVA - kTextRVA] == 0xC3);

      peparse::DestructParsedPE(p);
    }
  }
  return 0;
}

// A section with SizeOfRawData == 0 needs no file data; real TE images often
// leave its PointerToRawData meaningless, so it must not break parsing.
static int zeroFillSection(void) {
  for (int w = 0; w < 2; w++) {
    const bool wide = w != 0;
    // Two sections so the table ends before the .text data starts.
    Image img = make(wide, 0x1a0, false);
    Bytes b = img.bytes;
    b.at(4) = 2; // NumberOfSections
    std::uint32_t o = 40 + 40;
    std::memset(b.data() + o, 0, 40);
    std::memcpy(b.data() + o, ".bss", 4);
    w32(b, o + 8, 0x100);  // VirtualSize
    w32(b, o + 12, 0x3000); // VirtualAddress
    w32(b, o + 16, 0);      // SizeOfRawData
    w32(b, o + 20, 0);      // PointerToRawData
    w32(b, o + 36, 0xC0000080);

    peparse::parsed_pe *p = peparse::ParsePEFromPointer(
        b.data(), static_cast<std::uint32_t>(b.size()));
    TASK_CHECK(p != nullptr);

    peparse::te_header te;
    TASK_CHECK(peparse::GetTEHeader(p, te) && te.NumberOfSections == 2);

    SecCtx ctx;
    peparse::IterSec(p, collectSec, &ctx);
    TASK_CHECK(ctx.secs.size() == 2);
    const SecView *bss = findSec(ctx, ".bss");
    TASK_CHECK(bss != nullptr);
    TASK_CHECK(bss->base == img.sectionVA(0x3000));
    TASK_CHECK(bss->raw.PointerToRawData == 0);
    TASK_CHECK(bss->raw.SizeOfRawData == 0);
    TASK_CHECK(bss->data.empty());

    // Other sections still slice the TE file correctly.
    const SecView *text = findSec(ctx, ".text");
    TASK_CHECK(text != nullptr && text->data.size() == kTextSize &&
               text->data[0] == 0xC3);

    peparse::DestructParsedPE(p);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// directories
// ---------------------------------------------------------------------------

static int relocations(void) {
  const std::uint16_t strips[] = {0x1a0, 0x1b0};
  for (int w = 0; w < 2; w++) {
    const bool wide = w != 0;
    for (std::uint16_t stripped : strips) {
      Image img = make(wide, stripped);
      peparse::parsed_pe *p = parsePointer(img);
      TASK_CHECK(p != nullptr);

      RelocCtx ctx;
      peparse::IterRelocs(p, collectReloc, &ctx);
      // A directory starting at section offset 0x10 with length 10 still
      // yields its single entry.
      TASK_CHECK(ctx.relocs.size() == 1);
      TASK_CHECK(ctx.relocs[0].va == img.relocTargetVA());
      TASK_CHECK(static_cast<std::uint16_t>(ctx.relocs[0].type) ==
                 img.relocType());

      peparse::DestructParsedPE(p);
    }
  }
  return 0;
}

static int debugPayloads(void) {
  const std::uint16_t strips[] = {0x1a0, 0x1b0};
  for (int w = 0; w < 2; w++) {
    const bool wide = w != 0;
    for (std::uint16_t stripped : strips) {
      Image img = make(wide, stripped);
      peparse::parsed_pe *p = parsePointer(img);
      TASK_CHECK(p != nullptr);

      DebugCtx ctx;
      peparse::IterDebugs(p, collectDebug, &ctx);
      TASK_CHECK(ctx.debugs.size() == 2);

      // Entry 0: payload in .text, located from the debug directory that
      // itself lives in .rdata (another section).
      TASK_CHECK(ctx.debugs[0].type == 2);
      TASK_CHECK(ctx.debugs[0].data.size() == kDebugPayloadSize);
      for (std::uint32_t i = 0; i < kDebugPayloadSize; i++) {
        TASK_CHECK(ctx.debugs[0].data[i] == 0x41 + i);
      }
      std::uint32_t rvaPayload =
          img.teOffset(kTextRaw + (kDebugPayloadRVA - kTextRVA));
      TASK_CHECK(std::memcmp(ctx.debugs[0].data.data(),
                             img.bytes.data() + rvaPayload,
                             kDebugPayloadSize) == 0);

      // Entry 1: no RVA, only a PointerToRawData in pre-stripping file
      // coordinates; the stripping relation locates it.
      TASK_CHECK(ctx.debugs[1].type == 1);
      TASK_CHECK(ctx.debugs[1].data.size() == kDebugPointerSize);
      for (std::uint32_t i = 0; i < kDebugPointerSize; i++) {
        TASK_CHECK(ctx.debugs[1].data[i] == 0x61 + i);
      }
      std::uint32_t ptrPayload = img.teOffset(kDebugPayloadRaw);
      TASK_CHECK(std::memcmp(ctx.debugs[1].data.data(),
                             img.bytes.data() + ptrPayload,
                             kDebugPointerSize) == 0);

      peparse::DestructParsedPE(p);
    }
  }
  return 0;
}

static int missingDirectories(void) {
  const std::uint16_t strips[] = {0x1a0, 0x1b0};
  for (int w = 0; w < 2; w++) {
    const bool wide = w != 0;
    for (std::uint16_t stripped : strips) {
      Image img = make(wide, stripped, false);
      peparse::parsed_pe *p = parsePointer(img);
      TASK_CHECK(p != nullptr);

      peparse::te_header te;
      TASK_CHECK(peparse::GetTEHeader(p, te));
      TASK_CHECK(te.DataDirectory[0].Size == 0);
      TASK_CHECK(te.DataDirectory[1].Size == 0);

      RelocCtx relocs;
      peparse::IterRelocs(p, collectReloc, &relocs);
      TASK_CHECK(relocs.relocs.empty());
      DebugCtx debugs;
      peparse::IterDebugs(p, collectDebug, &debugs);
      TASK_CHECK(debugs.debugs.empty());

      // Missing directories do not break the rest of the image.
      peparse::VA ep = 0;
      TASK_CHECK(peparse::GetEntryPoint(p, ep) && ep == img.entryVA());

      peparse::DestructParsedPE(p);
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// lifetime
// ---------------------------------------------------------------------------

static int threeEntryPoints(void) {
  Image img = make(false, 0x1a0);
  Bytes pristine = img.bytes;
  const char *path = "te_contract_io.tmp";

  // ParsePEFromPointer.
  peparse::parsed_pe *pp = parsePointer(img);
  TASK_CHECK(pp != nullptr && headerMatches(pp, img));
  peparse::VA ep = 0;
  TASK_CHECK(peparse::GetEntryPoint(pp, ep) && ep == img.entryVA());
  peparse::DestructParsedPE(pp);

  // ParsePEFromBuffer.
  peparse::bounded_buffer *buf = peparse::makeBufferFromPointer(
      img.bytes.data(), static_cast<std::uint32_t>(img.bytes.size()));
  TASK_CHECK(buf != nullptr);
  peparse::parsed_pe *pb = peparse::ParsePEFromBuffer(buf);
  TASK_CHECK(pb != nullptr && headerMatches(pb, img));
  ep = 0;
  TASK_CHECK(peparse::GetEntryPoint(pb, ep) && ep == img.entryVA());
  peparse::DestructParsedPE(pb);

  // ParsePEFromFile.
  TASK_CHECK(writeTemp(path, img.bytes));
  peparse::parsed_pe *pf = peparse::ParsePEFromFile(path);
  std::remove(path);
  TASK_CHECK(pf != nullptr && headerMatches(pf, img));
  ep = 0;
  TASK_CHECK(peparse::GetEntryPoint(pf, ep) && ep == img.entryVA());
  std::uint8_t byte = 0;
  TASK_CHECK(peparse::ReadByteAtVA(pf, ep, byte) && byte == 0xC3);
  peparse::DestructParsedPE(pf);

  // Parsing through any entry point leaves the caller's bytes untouched.
  TASK_CHECK(img.bytes.size() == pristine.size());
  TASK_CHECK(std::memcmp(img.bytes.data(), pristine.data(), pristine.size()) ==
             0);
  return 0;
}

static int failureThenSuccess(void) {
  Image img = make(true, 0x1b0);
  Bytes good = img.bytes;

  // Truncated TE header: magic present, fewer than 40 header bytes.
  Bytes truncated = good;
  truncated.resize(39);
  TASK_CHECK(parseFails(truncated));

  // Section table does not fit after the header.
  Bytes shortTable = good;
  shortTable.at(4) = 64; // NumberOfSections
  TASK_CHECK(parseFails(shortTable));

  // StrippedSize makes a raw file coordinate predate the stripping point.
  Bytes underflow = good;
  w16(underflow, 6, 0x230);
  TASK_CHECK(parseFails(underflow));

  // Section span runs past the TE file.
  Bytes overrun = good;
  w32(overrun, 40 + 16, 0x100000); // .text SizeOfRawData
  TASK_CHECK(parseFails(overrun));

  // Unsupported machine.
  Bytes badMachine = good;
  w16(badMachine, 2, 0x1234);
  TASK_CHECK(parseFails(badMachine));

  // A preceding failure must not affect a later normal parse.
  peparse::parsed_pe *p = parsePointer(img);
  TASK_CHECK(p != nullptr && headerMatches(p, img));
  TASK_CHECK(p->fileBuffer->bufLen == good.size());
  TASK_CHECK(std::memcmp(p->fileBuffer->buf, good.data(), good.size()) == 0);

  RelocCtx relocs;
  peparse::IterRelocs(p, collectReloc, &relocs);
  TASK_CHECK(relocs.relocs.size() == 1);
  DebugCtx debugs;
  peparse::IterDebugs(p, collectDebug, &debugs);
  TASK_CHECK(debugs.debugs.size() == 2);

  peparse::DestructParsedPE(p);

  // And again after destruction, to prove independence from stale state.
  p = parsePointer(img);
  TASK_CHECK(p != nullptr && headerMatches(p, img));
  peparse::DestructParsedPE(p);
  return 0;
}

static int noMutationAndPE(void) {
  Image img = make(false, 0x1a0);
  Bytes pristine = img.bytes;

  peparse::parsed_pe *p = parsePointer(img);
  TASK_CHECK(p != nullptr);

  // Raw section headers still carry the original coordinates after parsing.
  SecCtx ctx;
  peparse::IterSec(p, collectSec, &ctx);
  for (const SecView &s : ctx.secs) {
    if (s.name == ".text") {
      TASK_CHECK(s.raw.PointerToRawData == kTextRaw);
      TASK_CHECK(s.raw.VirtualAddress == kTextRVA);
    } else if (s.name == ".rdata") {
      TASK_CHECK(s.raw.PointerToRawData == kDataRaw);
      TASK_CHECK(s.raw.VirtualAddress == kDataRVA);
    }
  }
  TASK_CHECK(p->fileBuffer->bufLen == pristine.size());
  TASK_CHECK(std::memcmp(p->fileBuffer->buf, pristine.data(), pristine.size()) ==
             0);
  peparse::DestructParsedPE(p);

  // Original bytes are untouched even after destruction.
  TASK_CHECK(img.bytes.size() == pristine.size());
  TASK_CHECK(std::memcmp(img.bytes.data(), pristine.data(), pristine.size()) ==
             0);

  // A normal PE does not expose a TE header, and keeps its own behavior.
  auto pe = fixtures::pe();
  peparse::parsed_pe *pp = peparse::ParsePEFromPointer(
      pe.data(), static_cast<std::uint32_t>(pe.size()));
  TASK_CHECK(pp != nullptr);
  peparse::te_header te;
  TASK_CHECK(!peparse::GetTEHeader(pp, te));
  peparse::VA ep = 0;
  TASK_CHECK(peparse::GetEntryPoint(pp, ep) && ep == 0x401000);
  std::uint8_t byte = 0;
  TASK_CHECK(peparse::ReadByteAtVA(pp, ep, byte) && byte == 0xC3);
  peparse::DestructParsedPE(pp);
  return 0;
}

} // namespace tec

extern const task_case task_cases[] = {
    {"coordinates", "te_header_and_coordinates", tec::coordinates},
    {"coordinates", "section_data_from_te_offsets", tec::sectionSlices},
    {"coordinates", "zero_fill_section", tec::zeroFillSection},
    {"directories", "relocations_start_inside_section", tec::relocations},
    {"directories", "debug_payload_locations", tec::debugPayloads},
    {"directories", "missing_directories_are_empty", tec::missingDirectories},
    {"lifetime", "three_parse_entry_points", tec::threeEntryPoints},
    {"lifetime", "failures_then_success", tec::failureThenSuccess},
    {"lifetime", "no_mutation_and_plain_pe", tec::noMutationAndPE},
};

extern const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
