// Contract tests for UEFI TE (Terse Executable) support in pe-parse.
//
// Every fixture here is synthesized in memory. Nothing is executed, loaded
// into a process, or relocated. The checked values (file offsets, entry
// points and relocation VAs) are computed by hand from the fixture layout,
// independently of the parser under test.

#include <pe-parse/parse.h>

#include "task_support/check.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace peparse;

using Bytes = std::vector<std::uint8_t>;

void w16(Bytes &b, std::size_t o, std::uint16_t v) {
  b.at(o) = static_cast<std::uint8_t>(v);
  b.at(o + 1) = static_cast<std::uint8_t>(v >> 8);
}

void w32(Bytes &b, std::size_t o, std::uint32_t v) {
  w16(b, o, static_cast<std::uint16_t>(v));
  w16(b, o + 2, static_cast<std::uint16_t>(v >> 16));
}

void w64(Bytes &b, std::size_t o, std::uint64_t v) {
  w32(b, o, static_cast<std::uint32_t>(v));
  w32(b, o + 4, static_cast<std::uint32_t>(v >> 32));
}

// A synthesized TE image whose section table follows the 40 byte header.
//
// Pre-stripping layout (every "raw" value stays in that coordinate space):
//   headers : file [0x000, 0x200)
//   .text   : RVA 0x1000, raw 0x200, size 0x200  (holds the entry code)
//   .rdata  : RVA 0x2000, raw 0x400, size 0x200  (reloc + debug directory)
//   .data   : RVA 0x3000, raw 0x600, size 0x200  (debug payload)
//
// In the TE file the first StrippedSize bytes are gone, so the pre-strip file
// offset P appears at P - bias where bias = StrippedSize - 40.
struct Fixture {
  Bytes bytes;
  std::uint32_t strippedSize;
  std::uint32_t bias;
  std::uint64_t imageBase;
  std::uint32_t entryRva;
  std::uint32_t textFileOff;
  std::uint32_t rdataFileOff;
  std::uint32_t debugDirRva;
  std::uint32_t debugDirOff;
  std::uint32_t relocDirRva;
  std::uint32_t relocDirOff;
  std::uint32_t debugPayloadRva;
  std::uint32_t debugPayloadFileOff;
  std::uint32_t filePtrDebugPayload;
};

Fixture makeFixture(bool wide, std::uint32_t strippedSize) {
  Fixture f{};
  f.strippedSize = strippedSize;
  f.bias = strippedSize - TE_HEADER_SIZE;
  f.imageBase = wide ? 0x140000000ULL : 0x400000ULL;
  f.entryRva = 0x1000;
  f.textFileOff = 0x200 - f.bias;
  f.rdataFileOff = 0x400 - f.bias;
  f.debugDirRva = 0x2040;  // 0x40 bytes into .rdata, not at its start
  f.debugDirOff = 0x40;
  f.relocDirRva = 0x2080;  // 0x80 bytes into .rdata
  f.relocDirOff = 0x80;
  f.debugPayloadRva = 0x3010;
  f.debugPayloadFileOff = 0x610 - f.bias;
  f.filePtrDebugPayload = 0x610;  // pre-stripping file pointer

  const std::size_t preStrip = 0x800;
  f.bytes.assign(preStrip - f.bias, 0);

  Bytes &b = f.bytes;

  // --- TE header (40 bytes) ---
  w16(b, 0, TE_MAGIC);
  w16(b, 2, wide ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386);
  b[4] = 3;   // NumberOfSections
  b[5] = 10;  // Subsystem: EFI_APPLICATION
  w16(b, 6, static_cast<std::uint16_t>(strippedSize));
  w32(b, 8, f.entryRva);
  w32(b, 12, 0x1000);  // BaseOfCode
  w64(b, 16, f.imageBase);
  w32(b, 24, f.relocDirRva);  // TE directory 0: base relocations
  w32(b, 28, 10);             // one 8 byte block header + one 2 byte entry
  w32(b, 32, f.debugDirRva);  // TE directory 1: debug
  w32(b, 36, 28);             // sizeof(debug_dir_entry)

  // --- Section table, immediately after the TE header ---
  auto section = [&b](std::size_t o, const char *name, std::uint32_t rva,
                      std::uint32_t raw, std::uint32_t size) {
    std::memcpy(b.data() + o, name, std::strlen(name));
    w32(b, o + 8, size);   // VirtualSize
    w32(b, o + 12, rva);   // VirtualAddress
    w32(b, o + 16, size);  // SizeOfRawData
    w32(b, o + 20, raw);   // PointerToRawData (pre-stripping coordinate)
    w32(b, o + 36, 0x60000020);
  };
  section(40, ".text", 0x1000, 0x200, 0x200);
  section(80, ".rdata", 0x2000, 0x400, 0x200);
  section(120, ".data", 0x3000, 0x600, 0x200);

  // Byte at the entry point.
  b.at(f.textFileOff) = 0xC3;

  // --- Base relocation directory inside .rdata, at +0x80 ---
  {
    std::size_t r = f.rdataFileOff + f.relocDirOff;
    w32(b, r, 0x2000);  // PageRVA
    w32(b, r + 4, 10);  // BlockSize
    w16(b, r + 8, wide ? 0xA008 : 0x3008);  // DIR64 / HIGHLOW, offset 8
  }

  // --- Debug directory inside .rdata, at +0x40 ---
  {
    std::size_t d = f.rdataFileOff + f.debugDirOff;
    w32(b, d + 12, 2);                  // Type: CODEVIEW
    w32(b, d + 16, 0x18);               // SizeOfData
    w32(b, d + 20, f.debugPayloadRva);  // AddressOfRawData
    w32(b, d + 24, 0);                  // PointerToRawData unused
  }

  // --- Debug payload in .data, at RVA 0x3010 ---
  {
    std::size_t p = f.debugPayloadFileOff;
    std::memcpy(b.data() + p, "RSDS", 4);
    w32(b, p + 4, 0x11223344);
    w32(b, p + 20, 7);  // age
  }

  return f;
}

// A TE whose debug record has no RVA, only a PointerToRawData that is still
// expressed in pre-stripping file coordinates.
Fixture makeFilePointerDebugFixture(std::uint32_t strippedSize) {
  Fixture f = makeFixture(false, strippedSize);
  f.debugPayloadFileOff = f.filePtrDebugPayload - f.bias;

  std::size_t d = f.rdataFileOff + f.debugDirOff;
  w32(f.bytes, d + 20, 0);                   // AddressOfRawData
  w32(f.bytes, d + 24, f.filePtrDebugPayload);

  std::memcpy(f.bytes.data() + f.debugPayloadFileOff, "NB10", 4);
  w32(f.bytes, f.debugPayloadFileOff + 4, 0xAABBCCDD);
  return f;
}

// A TE with both optional directories absent.
Fixture makeNoDirectoryFixture(bool wide) {
  Fixture f = makeFixture(wide, 0x180);
  w32(f.bytes, 24, 0);
  w32(f.bytes, 28, 0);
  w32(f.bytes, 32, 0);
  w32(f.bytes, 36, 0);
  return f;
}

struct SecView {
  std::string name;
  VA base;
  image_section_header hdr;
  const bounded_buffer *data;
};

std::vector<SecView> collectSections(parsed_pe *p) {
  std::vector<SecView> out;
  IterSec(
      p,
      [](void *ctx, const VA &base, const std::string &name,
         const image_section_header &hdr, const bounded_buffer *data) -> int {
        SecView s;
        s.name = name;
        s.base = base;
        s.hdr = hdr;
        s.data = data;
        static_cast<std::vector<SecView> *>(ctx)->push_back(s);
        return 0;
      },
      &out);
  return out;
}

struct RelocView {
  VA va;
  reloc_type type;
};

std::vector<RelocView> collectRelocs(parsed_pe *p) {
  std::vector<RelocView> out;
  IterRelocs(
      p,
      [](void *ctx, const VA &va, const reloc_type &t) -> int {
        static_cast<std::vector<RelocView> *>(ctx)->push_back({va, t});
        return 0;
      },
      &out);
  return out;
}

struct DebugView {
  std::uint32_t type;
  bool present;
  std::vector<std::uint8_t> data;
  const std::uint8_t *src;  // start of the payload in the source buffer
};

std::vector<DebugView> collectDebugs(parsed_pe *p) {
  std::vector<DebugView> out;
  IterDebugs(
      p,
      [](void *ctx, const std::uint32_t &type,
         const bounded_buffer *data) -> int {
        DebugView d;
        d.type = type;
        d.present = data != nullptr;
        d.src = data != nullptr ? data->buf : nullptr;
        if (data != nullptr) {
          d.data.assign(data->buf, data->buf + data->bufLen);
        }
        static_cast<std::vector<DebugView> *>(ctx)->push_back(d);
        return 0;
      },
      &out);
  return out;
}

bool bytesEqual(const Bytes &a, const Bytes &b) {
  return a.size() == b.size() &&
         std::memcmp(a.data(), b.data(), a.size()) == 0;
}

Bytes buildPlainPE() {
  Bytes b(0x400, 0);
  w16(b, 0, 0x5A4D);
  w32(b, 0x3C, 0x80);
  w32(b, 0x80, NT_MAGIC);
  w16(b, 0x84, IMAGE_FILE_MACHINE_I386);
  w16(b, 0x86, 1);    // NumberOfSections
  w16(b, 0x94, 224);  // SizeOfOptionalHeader
  w16(b, 0x96, 0x102);
  std::size_t o = 0x98;
  w16(b, o, NT_OPTIONAL_32_MAGIC);
  w32(b, o + 16, 0x1000);  // AddressOfEntryPoint
  w32(b, o + 20, 0x1000);  // BaseOfCode
  w32(b, o + 28, 0x400000);
  w32(b, o + 32, 0x1000);
  w32(b, o + 36, 0x200);
  w32(b, o + 56, 0x2000);
  w32(b, o + 60, 0x200);
  w16(b, o + 68, IMAGE_SUBSYSTEM_WINDOWS_CUI);
  w32(b, o + 92, NUM_DIR_ENTRIES);
  std::size_t s = 0x178;
  std::memcpy(b.data() + s, ".text", 5);
  w32(b, s + 8, 0x200);   // VirtualSize
  w32(b, s + 12, 0x1000); // VirtualAddress
  w32(b, s + 16, 0x200);  // SizeOfRawData
  w32(b, s + 20, 0x200);  // PointerToRawData
  w32(b, s + 36, 0x60000020);
  b[0x200] = 0xC3;
  return b;
}

// ---------------------------------------------------------------------------
// Group: coordinates
// ---------------------------------------------------------------------------

int coordinates_32bit_entry_and_sections() {
  Fixture f = makeFixture(false, 0x180);
  Bytes original = f.bytes;

  parsed_pe *p = ParsePEFromPointer(
      f.bytes.data(), static_cast<std::uint32_t>(f.bytes.size()));
  TASK_CHECK(p != nullptr);

  // The original TE file is still the backing buffer, unchanged.
  TASK_CHECK(p->fileBuffer != nullptr);
  TASK_CHECK(p->fileBuffer->bufLen == original.size());
  TASK_CHECK(std::memcmp(p->fileBuffer->buf, original.data(),
                         original.size()) == 0);
  TASK_CHECK(bytesEqual(f.bytes, original));

  // Machine and subsystem are the raw TE values.
  TASK_CHECK(p->peHeader.nt.FileHeader.Machine == IMAGE_FILE_MACHINE_I386);
  TASK_CHECK(std::string(GetMachineAsString(p)) == "x86");
  TASK_CHECK(std::string(GetSubsystemAsString(p)) == "EFI_APPLICATION");

  // Entry point = original ImageBase + original entry RVA.
  VA ep = 0;
  TASK_CHECK(GetEntryPoint(p, ep));
  TASK_CHECK(ep == f.imageBase + f.entryRva);
  TASK_CHECK(ep == 0x401000ULL);

  std::uint8_t byte = 0;
  TASK_CHECK(ReadByteAtVA(p, ep, byte));
  TASK_CHECK(byte == 0xC3);

  // Section enumeration keeps original header fields, but each data slice
  // comes from the real TE file position.
  std::vector<SecView> secs = collectSections(p);
  TASK_CHECK(secs.size() == 3);
  TASK_CHECK(secs[0].name == ".text");
  TASK_CHECK(secs[0].base == f.imageBase + 0x1000);
  TASK_CHECK(secs[0].hdr.VirtualAddress == 0x1000);
  TASK_CHECK(secs[0].hdr.PointerToRawData == 0x200);  // not rewritten
  TASK_CHECK(secs[0].hdr.SizeOfRawData == 0x200);
  TASK_CHECK(secs[0].data != nullptr);
  TASK_CHECK(secs[0].data->buf == f.bytes.data() + f.textFileOff);
  TASK_CHECK(secs[0].data->bufLen == 0x200);
  TASK_CHECK(secs[0].data->buf[0] == 0xC3);

  TASK_CHECK(secs[1].name == ".rdata");
  TASK_CHECK(secs[1].base == f.imageBase + 0x2000);
  TASK_CHECK(secs[1].hdr.PointerToRawData == 0x400);
  TASK_CHECK(secs[1].data->buf == f.bytes.data() + f.rdataFileOff);

  TASK_CHECK(secs[2].name == ".data");
  TASK_CHECK(secs[2].base == f.imageBase + 0x3000);
  TASK_CHECK(secs[2].hdr.PointerToRawData == 0x600);
  TASK_CHECK(secs[2].data->buf == f.bytes.data() + (0x600 - f.bias));

  // GetTEHeader exposes the raw TE header.
  te_header te{};
  TASK_CHECK(GetTEHeader(p, te));
  TASK_CHECK(te.Signature == TE_MAGIC);
  TASK_CHECK(te.Machine == IMAGE_FILE_MACHINE_I386);
  TASK_CHECK(te.NumberOfSections == 3);
  TASK_CHECK(te.Subsystem == 10);
  TASK_CHECK(te.StrippedSize == 0x180);
  TASK_CHECK(te.AddressOfEntryPoint == 0x1000);
  TASK_CHECK(te.BaseOfCode == 0x1000);
  TASK_CHECK(te.ImageBase == 0x400000ULL);
  TASK_CHECK(te.DataDirectory[0].VirtualAddress == f.relocDirRva);
  TASK_CHECK(te.DataDirectory[1].VirtualAddress == f.debugDirRva);

  DestructParsedPE(p);
  return 0;
}

int coordinates_64bit_different_stripped_size() {
  // A different StrippedSize shifts every file position by a different
  // amount while RVAs and VAs stay identical.
  Fixture f = makeFixture(true, 0x170);
  TASK_CHECK(f.bias == 0x148);

  parsed_pe *p = ParsePEFromPointer(
      f.bytes.data(), static_cast<std::uint32_t>(f.bytes.size()));
  TASK_CHECK(p != nullptr);

  TASK_CHECK(p->peHeader.nt.OptionalMagic == NT_OPTIONAL_64_MAGIC);
  TASK_CHECK(std::string(GetMachineAsString(p)) == "x64");

  VA ep = 0;
  TASK_CHECK(GetEntryPoint(p, ep));
  TASK_CHECK(ep == 0x140001000ULL);

  std::uint8_t byte = 0;
  TASK_CHECK(ReadByteAtVA(p, ep, byte));
  TASK_CHECK(byte == 0xC3);

  std::vector<SecView> secs = collectSections(p);
  TASK_CHECK(secs.size() == 3);
  TASK_CHECK(secs[0].name == ".text");
  TASK_CHECK(secs[0].data->buf == f.bytes.data() + f.textFileOff);
  TASK_CHECK(secs[1].name == ".rdata");
  TASK_CHECK(secs[1].data->buf == f.bytes.data() + f.rdataFileOff);
  TASK_CHECK(secs[2].name == ".data");
  TASK_CHECK(secs[2].hdr.PointerToRawData == 0x600);

  te_header te{};
  TASK_CHECK(GetTEHeader(p, te));
  TASK_CHECK(te.Machine == IMAGE_FILE_MACHINE_AMD64);
  TASK_CHECK(te.ImageBase == 0x140000000ULL);
  TASK_CHECK(te.StrippedSize == 0x170);

  DestructParsedPE(p);
  return 0;
}

int coordinates_file_parsing_matches_pointer() {
  Fixture f = makeFixture(false, 0x180);
  const char *tmp = std::getenv("TMPDIR");
  std::string path =
      std::string(tmp != nullptr ? tmp : ".") + "/pe_parse_te_fixture.bin";
  {
    FILE *fp = std::fopen(path.c_str(), "wb");
    TASK_CHECK(fp != nullptr);
    TASK_CHECK(std::fwrite(f.bytes.data(), 1, f.bytes.size(), fp) ==
               f.bytes.size());
    std::fclose(fp);
  }

  parsed_pe *p = ParsePEFromFile(path.c_str());
  TASK_CHECK(p != nullptr);

  VA ep = 0;
  TASK_CHECK(GetEntryPoint(p, ep));
  TASK_CHECK(ep == 0x401000ULL);

  std::uint8_t byte = 0;
  TASK_CHECK(ReadByteAtVA(p, ep, byte));
  TASK_CHECK(byte == 0xC3);

  std::vector<SecView> secs = collectSections(p);
  TASK_CHECK(secs.size() == 3);
  TASK_CHECK(secs[0].data->buf[0] == 0xC3);
  TASK_CHECK(collectRelocs(p).size() == 1);
  TASK_CHECK(collectDebugs(p).size() == 1);

  DestructParsedPE(p);
  std::remove(path.c_str());
  return 0;
}

int coordinates_buffer_parsing_matches_pointer() {
  // ParsePEFromBuffer takes ownership of the bounded_buffer, so hand it a
  // split view and compare against the pointer entry point.
  Fixture f = makeFixture(true, 0x180);
  const std::uint32_t size = static_cast<std::uint32_t>(f.bytes.size());
  bounded_buffer *src = makeBufferFromPointer(f.bytes.data(), size);
  TASK_CHECK(src != nullptr);
  bounded_buffer *owned = splitBuffer(src, 0, size);
  deleteBuffer(src);
  TASK_CHECK(owned != nullptr);

  parsed_pe *p = ParsePEFromBuffer(owned);
  TASK_CHECK(p != nullptr);
  TASK_CHECK(p->fileBuffer == owned);

  VA ep = 0;
  TASK_CHECK(GetEntryPoint(p, ep));
  TASK_CHECK(ep == 0x140001000ULL);

  std::uint8_t byte = 0;
  TASK_CHECK(ReadByteAtVA(p, ep, byte));
  TASK_CHECK(byte == 0xC3);

  te_header te{};
  TASK_CHECK(GetTEHeader(p, te));
  TASK_CHECK(te.StrippedSize == 0x180);

  DestructParsedPE(p);
  return 0;
}

int coordinates_normal_pe_unaffected() {
  Bytes b = buildPlainPE();
  Bytes original = b;

  parsed_pe *p = ParsePEFromPointer(
      b.data(), static_cast<std::uint32_t>(b.size()));
  TASK_CHECK(p != nullptr);

  VA ep = 0;
  TASK_CHECK(GetEntryPoint(p, ep));
  TASK_CHECK(ep == 0x401000ULL);

  std::uint8_t byte = 0;
  TASK_CHECK(ReadByteAtVA(p, ep, byte));
  TASK_CHECK(byte == 0xC3);

  te_header te{};
  TASK_CHECK(!GetTEHeader(p, te));

  std::vector<SecView> secs = collectSections(p);
  TASK_CHECK(secs.size() == 1);
  TASK_CHECK(secs[0].name == ".text");
  TASK_CHECK(secs[0].data->buf == b.data() + 0x200);
  TASK_CHECK(bytesEqual(b, original));

  DestructParsedPE(p);
  return 0;
}

// ---------------------------------------------------------------------------
// Group: directories
// ---------------------------------------------------------------------------

int directories_relocs_from_nonzero_section_offset() {
  // The relocation directory starts 0x80 bytes into .rdata. The walk must
  // bound itself by the directory length, not by that section-relative
  // starting offset.
  Fixture f = makeFixture(false, 0x180);
  parsed_pe *p = ParsePEFromPointer(
      f.bytes.data(), static_cast<std::uint32_t>(f.bytes.size()));
  TASK_CHECK(p != nullptr);

  std::vector<RelocView> relocs = collectRelocs(p);
  TASK_CHECK(relocs.size() == 1);
  TASK_CHECK(relocs[0].va == f.imageBase + 0x2000 + 0x8);
  TASK_CHECK(relocs[0].va == 0x402008ULL);
  TASK_CHECK(relocs[0].type == RELOC_HIGHLOW);

  DestructParsedPE(p);
  return 0;
}

int directories_relocs_64bit() {
  Fixture f = makeFixture(true, 0x180);
  parsed_pe *p = ParsePEFromPointer(
      f.bytes.data(), static_cast<std::uint32_t>(f.bytes.size()));
  TASK_CHECK(p != nullptr);

  std::vector<RelocView> relocs = collectRelocs(p);
  TASK_CHECK(relocs.size() == 1);
  TASK_CHECK(relocs[0].va == 0x140000000ULL + 0x2000 + 0x8);
  TASK_CHECK(relocs[0].type == RELOC_DIR64);

  DestructParsedPE(p);
  return 0;
}

int directories_debug_payload_in_other_section() {
  // Debug directory lives in .rdata; its payload lives in .data.
  Fixture f = makeFixture(false, 0x180);
  parsed_pe *p = ParsePEFromPointer(
      f.bytes.data(), static_cast<std::uint32_t>(f.bytes.size()));
  TASK_CHECK(p != nullptr);

  std::vector<DebugView> debugs = collectDebugs(p);
  TASK_CHECK(debugs.size() == 1);
  TASK_CHECK(debugs[0].type == 2);
  TASK_CHECK(debugs[0].present);
  TASK_CHECK(debugs[0].data.size() == 0x18);
  TASK_CHECK(std::memcmp(debugs[0].data.data(), "RSDS", 4) == 0);
  TASK_CHECK(std::memcmp(p->fileBuffer->buf + f.debugPayloadFileOff, "RSDS",
                         4) == 0);

  DestructParsedPE(p);
  return 0;
}

int directories_debug_file_pointer_only() {
  // The debug record has no RVA, only a PointerToRawData expressed in
  // pre-stripping file coordinates.
  Fixture f = makeFilePointerDebugFixture(0x160);
  parsed_pe *p = ParsePEFromPointer(
      f.bytes.data(), static_cast<std::uint32_t>(f.bytes.size()));
  TASK_CHECK(p != nullptr);

  std::vector<DebugView> debugs = collectDebugs(p);
  TASK_CHECK(debugs.size() == 1);
  TASK_CHECK(debugs[0].type == 2);
  TASK_CHECK(debugs[0].present);
  TASK_CHECK(debugs[0].data.size() == 0x18);
  TASK_CHECK(std::memcmp(debugs[0].data.data(), "NB10", 4) == 0);
  TASK_CHECK(debugs[0].src ==
             f.bytes.data() + (f.filePtrDebugPayload - f.bias));

  DestructParsedPE(p);
  return 0;
}

int directories_missing_yield_empty() {
  Fixture f = makeNoDirectoryFixture(true);
  parsed_pe *p = ParsePEFromPointer(
      f.bytes.data(), static_cast<std::uint32_t>(f.bytes.size()));
  TASK_CHECK(p != nullptr);

  TASK_CHECK(collectRelocs(p).empty());
  TASK_CHECK(collectDebugs(p).empty());

  VA ep = 0;
  TASK_CHECK(GetEntryPoint(p, ep));
  TASK_CHECK(ep == 0x140001000ULL);

  te_header te{};
  TASK_CHECK(GetTEHeader(p, te));
  TASK_CHECK(te.DataDirectory[0].Size == 0);
  TASK_CHECK(te.DataDirectory[1].Size == 0);

  DestructParsedPE(p);
  return 0;
}

// ---------------------------------------------------------------------------
// Group: lifetime
// ---------------------------------------------------------------------------

int lifetime_rejects_truncated_header() {
  Fixture f = makeFixture(false, 0x180);
  Bytes truncated(f.bytes.begin(), f.bytes.begin() + 20);
  TASK_CHECK(ParsePEFromPointer(
                 truncated.data(),
                 static_cast<std::uint32_t>(truncated.size())) == nullptr);
  return 0;
}

int lifetime_rejects_short_section_table() {
  Fixture f = makeFixture(false, 0x180);
  // Header claims 3 sections, file only has room for one and part of a
  // second.
  Bytes truncated(f.bytes.begin(),
                  f.bytes.begin() + TE_HEADER_SIZE + 40 + 8);
  TASK_CHECK(ParsePEFromPointer(
                 truncated.data(),
                 static_cast<std::uint32_t>(truncated.size())) == nullptr);
  return 0;
}

int lifetime_rejects_bias_underflow() {
  Fixture f = makeFixture(false, 0x180);
  // .text PointerToRawData (0x200) is smaller than the bias, so the
  // translation would underflow.
  w32(f.bytes, 40 + 20, 0x10);
  TASK_CHECK(ParsePEFromPointer(
                 f.bytes.data(),
                 static_cast<std::uint32_t>(f.bytes.size())) == nullptr);
  return 0;
}

int lifetime_rejects_out_of_bounds_span() {
  Fixture f = makeFixture(false, 0x180);
  w32(f.bytes, 40 + 16, 0x10000);  // SizeOfRawData past end of file
  TASK_CHECK(ParsePEFromPointer(
                 f.bytes.data(),
                 static_cast<std::uint32_t>(f.bytes.size())) == nullptr);
  return 0;
}

int lifetime_rejects_stripped_size_below_header() {
  Fixture f = makeFixture(false, 0x180);
  w16(f.bytes, 6, 0x20);  // StrippedSize < 40 would underflow the bias
  TASK_CHECK(ParsePEFromPointer(
                 f.bytes.data(),
                 static_cast<std::uint32_t>(f.bytes.size())) == nullptr);
  return 0;
}

int lifetime_rejects_32bit_wide_image_base() {
  Fixture f = makeFixture(false, 0x180);
  w64(f.bytes, 16, 0x140000000ULL);  // I386 cannot expose a 64-bit base
  TASK_CHECK(ParsePEFromPointer(
                 f.bytes.data(),
                 static_cast<std::uint32_t>(f.bytes.size())) == nullptr);
  return 0;
}

int lifetime_rejects_unsupported_machine() {
  Fixture f = makeFixture(false, 0x180);
  w16(f.bytes, 2, IMAGE_FILE_MACHINE_ARM64);
  TASK_CHECK(ParsePEFromPointer(
                 f.bytes.data(),
                 static_cast<std::uint32_t>(f.bytes.size())) == nullptr);
  return 0;
}

int lifetime_failure_does_not_poison_next_parse() {
  Fixture bad = makeFixture(false, 0x180);
  w32(bad.bytes, 40 + 20, 0x10);  // underflowing section pointer
  TASK_CHECK(ParsePEFromPointer(
                 bad.bytes.data(),
                 static_cast<std::uint32_t>(bad.bytes.size())) == nullptr);

  Fixture good = makeFixture(false, 0x180);
  parsed_pe *p = ParsePEFromPointer(
      good.bytes.data(), static_cast<std::uint32_t>(good.bytes.size()));
  TASK_CHECK(p != nullptr);

  VA ep = 0;
  TASK_CHECK(GetEntryPoint(p, ep));
  TASK_CHECK(ep == 0x401000ULL);
  TASK_CHECK(collectSections(p).size() == 3);
  TASK_CHECK(collectRelocs(p).size() == 1);
  TASK_CHECK(collectDebugs(p).size() == 1);
  DestructParsedPE(p);

  // A failure inside the directory walk must not leak into the next parse.
  Fixture badDir = makeFixture(true, 0x170);
  w32(badDir.bytes, 28, 0xFFFFFFFF);  // absurd reloc directory size
  TASK_CHECK(ParsePEFromPointer(
                 badDir.bytes.data(),
                 static_cast<std::uint32_t>(badDir.bytes.size())) == nullptr);

  Fixture good64 = makeFixture(true, 0x170);
  p = ParsePEFromPointer(good64.bytes.data(),
                         static_cast<std::uint32_t>(good64.bytes.size()));
  TASK_CHECK(p != nullptr);
  TASK_CHECK(collectRelocs(p).size() == 1);
  TASK_CHECK(collectDebugs(p).size() == 1);
  DestructParsedPE(p);

  return 0;
}

int lifetime_original_bytes_and_headers_unchanged() {
  Fixture f = makeFixture(false, 0x180);
  Bytes original = f.bytes;

  parsed_pe *p = ParsePEFromPointer(
      f.bytes.data(), static_cast<std::uint32_t>(f.bytes.size()));
  TASK_CHECK(p != nullptr);

  // Walk everything; none of it may mutate the input.
  TASK_CHECK(collectSections(p).size() == 3);
  TASK_CHECK(collectRelocs(p).size() == 1);
  TASK_CHECK(collectDebugs(p).size() == 1);

  TASK_CHECK(bytesEqual(f.bytes, original));
  TASK_CHECK(p->fileBuffer != nullptr);
  TASK_CHECK(p->fileBuffer->bufLen == original.size());
  TASK_CHECK(std::memcmp(p->fileBuffer->buf, original.data(),
                         original.size()) == 0);

  // Original section header fields were preserved, not rewritten.
  std::vector<SecView> secs = collectSections(p);
  TASK_CHECK(secs.size() == 3);
  TASK_CHECK(secs[0].hdr.PointerToRawData == 0x200);
  TASK_CHECK(secs[1].hdr.PointerToRawData == 0x400);
  TASK_CHECK(secs[2].hdr.PointerToRawData == 0x600);
  TASK_CHECK(secs[0].hdr.Name[0] == '.');

  DestructParsedPE(p);
  return 0;
}

} // namespace

const task_case task_cases[] = {
    {"coordinates", "32bit_entry_and_sections",
     coordinates_32bit_entry_and_sections},
    {"coordinates", "64bit_different_stripped_size",
     coordinates_64bit_different_stripped_size},
    {"coordinates", "file_parsing_matches_pointer",
     coordinates_file_parsing_matches_pointer},
    {"coordinates", "buffer_parsing_matches_pointer",
     coordinates_buffer_parsing_matches_pointer},
    {"coordinates", "normal_pe_unaffected", coordinates_normal_pe_unaffected},
    {"directories", "relocs_from_nonzero_section_offset",
     directories_relocs_from_nonzero_section_offset},
    {"directories", "relocs_64bit", directories_relocs_64bit},
    {"directories", "debug_payload_in_other_section",
     directories_debug_payload_in_other_section},
    {"directories", "debug_file_pointer_only",
     directories_debug_file_pointer_only},
    {"directories", "missing_yield_empty", directories_missing_yield_empty},
    {"lifetime", "rejects_truncated_header", lifetime_rejects_truncated_header},
    {"lifetime", "rejects_short_section_table",
     lifetime_rejects_short_section_table},
    {"lifetime", "rejects_bias_underflow", lifetime_rejects_bias_underflow},
    {"lifetime", "rejects_out_of_bounds_span",
     lifetime_rejects_out_of_bounds_span},
    {"lifetime", "rejects_stripped_size_below_header",
     lifetime_rejects_stripped_size_below_header},
    {"lifetime", "rejects_32bit_wide_image_base",
     lifetime_rejects_32bit_wide_image_base},
    {"lifetime", "rejects_unsupported_machine",
     lifetime_rejects_unsupported_machine},
    {"lifetime", "failure_does_not_poison_next_parse",
     lifetime_failure_does_not_poison_next_parse},
    {"lifetime", "original_bytes_and_headers_unchanged",
     lifetime_original_bytes_and_headers_unchanged},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
