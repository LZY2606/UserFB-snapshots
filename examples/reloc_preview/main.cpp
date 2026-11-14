// Static base-relocation preview example.
//
// This program never touches the host filesystem: the PE image is built
// entirely in memory and handed to BuildBaseRelocationPreview as a read
// only byte span. It prints the resulting block/operation/interval
// explanation, or a structured error.
//
// Only the C++ standard library and pe-parse are used.

#include <pe-parse/relocation_preview.h>

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {

void put16(std::vector<std::uint8_t> &b, std::size_t off,
           std::uint16_t v) {
  b[off] = static_cast<std::uint8_t>(v);
  b[off + 1] = static_cast<std::uint8_t>(v >> 8);
}

void put32(std::vector<std::uint8_t> &b, std::size_t off,
           std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    b[off + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>(v >> (8 * i));
  }
}

std::uint16_t relocWord(std::uint16_t type, std::uint16_t offset) {
  return static_cast<std::uint16_t>((type << 12) | (offset & 0x0FFF));
}

const char *kindName(peparse::RelocPreviewKind kind) {
  switch (kind) {
  case peparse::RelocPreviewKind_Absolute:
    return "ABSOLUTE";
  case peparse::RelocPreviewKind_High:
    return "HIGH";
  case peparse::RelocPreviewKind_Low:
    return "LOW";
  case peparse::RelocPreviewKind_HighLow:
    return "HIGHLOW";
  case peparse::RelocPreviewKind_HighAdj:
    return "HIGHADJ";
  case peparse::RelocPreviewKind_Dir64:
    return "DIR64";
  }
  return "?";
}

// Build a minimal little-endian I386 PE32 image with two sections:
// .text at RVA 0x1000 / raw 0x200 and .reloc at RVA 0x2000 / raw 0x400.
std::vector<std::uint8_t> buildImage() {
  std::vector<std::uint8_t> b(0x800, 0);

  put16(b, 0, 0x5A4D);        // MZ
  put32(b, 0x3C, 0x80);       // e_lfanew
  put32(b, 0x80, 0x00004550); // PE\0\0
  put16(b, 0x84, 0x014C);     // IMAGE_FILE_MACHINE_I386
  put16(b, 0x86, 2);          // NumberOfSections
  put16(b, 0x94, 0x00E0);     // SizeOfOptionalHeader
  put16(b, 0x96, 0x0102);     // Characteristics

  const std::size_t opt = 0x98;
  put16(b, opt, 0x010B);      // PE32 magic
  put32(b, opt + 28, 0x00400000); // ImageBase
  put32(b, opt + 32, 0x1000); // SectionAlignment
  put32(b, opt + 36, 0x200);  // FileAlignment
  put32(b, opt + 56, 0x5000); // SizeOfImage
  put32(b, opt + 60, 0x200);  // SizeOfHeaders
  put32(b, opt + 92, 16);     // NumberOfRvaAndSizes
  const std::size_t dirBase = opt + 96;
  put32(b, dirBase + 5 * 8, 0x2000);     // reloc directory RVA
  put32(b, dirBase + 5 * 8 + 4, 16);    // reloc directory size

  std::size_t table = opt + 0x00E0;
  std::memcpy(&b[table], ".text", 5);
  put32(b, table + 12, 0x1000);
  put32(b, table + 20, 0x200);
  put32(b, table + 24, 0x200);
  put32(b, table + 36, 0xC0000040);

  std::memcpy(&b[table + 40], ".reloc", 6);
  put32(b, table + 40 + 12, 0x2000);
  put32(b, table + 40 + 20, 0x200);
  put32(b, table + 40 + 24, 0x400);
  put32(b, table + 40 + 36, 0xC0000040);

  // A pointer-sized field that the relocation will adjust.
  put32(b, 0x200, 0x00401234);

  // One block: a HIGHLOW on RVA 0x1000 plus ABSOLUTE padding.
  put32(b, 0x400, 0x1000); // PageRVA
  put32(b, 0x404, 16);     // BlockSize
  put16(b, 0x408, relocWord(3, 0x000));
  put16(b, 0x40A, 0x0000);
  put16(b, 0x40C, 0x0000);
  put16(b, 0x40E, 0x0000);

  return b;
}

} // namespace

int main() {
  std::vector<std::uint8_t> image = buildImage();
  const std::uint64_t newImageBase = 0x00500000;

  peparse::RelocPreviewResult result =
      peparse::BuildBaseRelocationPreview(image.data(), image.size(),
                                          newImageBase);
  if (!result.ok) {
    const peparse::RelocPreviewError &e = result.error;
    std::cerr << "preview failed (category "
              << static_cast<int>(e.category) << "): " << e.message;
    if (e.hasFileOffset) {
      std::cerr << " at file offset 0x" << std::hex << e.fileOffset;
    }
    if (e.hasRva) {
      std::cerr << " at RVA 0x" << std::hex << e.rva;
    }
    std::cerr << "\n";
    return 1;
  }

  peparse::RelocPreviewReport *report = result.report;
  std::cout << std::hex << std::setfill('0');
  std::cout << "old ImageBase : 0x" << report->oldImageBase << "\n";
  std::cout << "new ImageBase : 0x" << report->newImageBase << "\n";
  std::cout << "base delta    : " << std::dec << report->baseDelta
            << "\n\n";

  for (std::uint32_t bi = 0; bi < report->blockCount; ++bi) {
    const peparse::RelocPreviewBlock &block = report->blocks[bi];
    std::cout << "block pageRVA=0x" << std::hex << block.pageRva
              << " blockSize=" << std::dec << block.blockSize << "\n";
    for (std::uint32_t oi = 0; oi < block.operationCount; ++oi) {
      const peparse::RelocPreviewOperation &op = block.operations[oi];
      std::cout << "  [" << op.ordinal << "] " << kindName(op.kind)
                << " targetRVA=0x" << std::hex << op.targetRva
                << " rawEntry=0x" << op.rawEntry
                << " entryFileOffset=0x" << op.entryFileOffset << "\n";
      if (op.width != peparse::RelocPreviewWidth_None) {
        std::cout << "      before=";
        for (std::uint32_t k = 0; k < op.beforeByteCount; ++k) {
          std::cout << std::hex << std::setw(2)
                    << static_cast<unsigned>(op.beforeBytes[k]);
        }
        std::cout << " after=";
        for (std::uint32_t k = 0; k < op.afterByteCount; ++k) {
          std::cout << std::hex << std::setw(2)
                    << static_cast<unsigned>(op.afterBytes[k]);
        }
        std::cout << "\n";
      }
    }
  }

  std::cout << "\nmerged intervals (RVA sorted):\n";
  for (std::uint32_t ii = 0; ii < report->intervalCount; ++ii) {
    const peparse::RelocPreviewInterval &iv = report->intervals[ii];
    std::cout << "  0x" << std::hex << iv.firstRva << "..0x"
              << iv.lastRva << " final=";
    for (std::uint32_t k = 0; k < iv.finalByteCount; ++k) {
      std::cout << std::setw(2)
                << static_cast<unsigned>(iv.finalBytes[k]);
    }
    std::cout << "\n";
  }

  // The input is untouched and the report is fully self-owned.
  std::cout << "\ninput byte 0x200 still 0x" << std::hex
            << static_cast<unsigned>(image[0x200]) << "..\n";

  peparse::DestroyBaseRelocationPreview(report);
  return 0;
}
