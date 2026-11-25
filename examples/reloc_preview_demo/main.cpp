/*
 * relocation preview demo
 *
 * Builds a minimal I386 PE32 image entirely in memory and previews what its
 * pointer fields would look like after moving ImageBase. No host file is
 * opened or written; only the C++ standard library is used besides pe-parse.
 */

#include <pe-parse/nt-headers.h>
#include <pe-parse/relocation_preview.h>

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {

typedef std::vector<std::uint8_t> byte_vec;

void put16(byte_vec &b, std::uint32_t off, std::uint16_t v) {
  b[off] = static_cast<std::uint8_t>(v);
  b[off + 1] = static_cast<std::uint8_t>(v >> 8);
}

void put32(byte_vec &b, std::uint32_t off, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    b[off + static_cast<std::uint32_t>(i)] =
        static_cast<std::uint8_t>(v >> (8 * i));
  }
}

const char *kind_name(peparse::relocation_preview_kind kind) {
  switch (kind) {
  case peparse::RPK_ABSOLUTE:
    return "ABSOLUTE";
  case peparse::RPK_HIGH:
    return "HIGH";
  case peparse::RPK_LOW:
    return "LOW";
  case peparse::RPK_HIGHLOW:
    return "HIGHLOW";
  case peparse::RPK_HIGHADJ:
    return "HIGHADJ";
  case peparse::RPK_DIR64:
    return "DIR64";
  }
  return "?";
}

// A two-section I386 PE32 image: .text at RVA 0x1000 / file 0x200 and
// .reloc at RVA 0x2000 / file 0x400.
byte_vec make_image() {
  byte_vec b(0x600U, 0U);

  put16(b, 0x00U, peparse::MZ_MAGIC);
  put32(b, 0x3CU, 0x80U);
  put32(b, 0x80U, peparse::NT_MAGIC);
  put16(b, 0x84U, peparse::IMAGE_FILE_MACHINE_I386);
  put16(b, 0x86U, 2U);               // NumberOfSections
  put16(b, 0x94U, 0xE0U);            // SizeOfOptionalHeader
  put16(b, 0x96U, 0x0102U);          // Characteristics
  put16(b, 0x98U, peparse::NT_OPTIONAL_32_MAGIC);
  put32(b, 0xB4U, 0x00400000U);      // ImageBase
  put32(b, 0xB8U, 0x1000U);          // SectionAlignment
  put32(b, 0xBCU, 0x200U);           // FileAlignment
  put32(b, 0xD0U, 0x4000U);          // SizeOfImage
  put32(b, 0xD4U, 0x200U);           // SizeOfHeaders
  put32(b, 0xF4U, 16U);              // NumberOfRvaAndSizes
  // Base relocation data directory (index 5): RVA 0x2000, size 16.
  put32(b, 0x98U + 96U + 5U * 8U, 0x2000U);
  put32(b, 0x98U + 96U + 5U * 8U + 4U, 16U);

  // Section table at 0x178.
  std::memcpy(&b[0x178U], ".text", 5);
  put32(b, 0x180U, 0x200U);          // VirtualSize
  put32(b, 0x184U, 0x1000U);         // VirtualAddress
  put32(b, 0x188U, 0x200U);          // SizeOfRawData
  put32(b, 0x18CU, 0x200U);          // PointerToRawData
  put32(b, 0x19CU, 0x40000040U);     // Characteristics

  std::memcpy(&b[0x1A0U], ".reloc", 6);
  put32(b, 0x1A8U, 0x200U);
  put32(b, 0x1ACU, 0x2000U);
  put32(b, 0x1B0U, 0x200U);
  put32(b, 0x1B4U, 0x400U);
  put32(b, 0x1C4U, 0x40000040U);

  // A 32-bit pointer field at RVA 0x1000 (file 0x200).
  put32(b, 0x200U, 0x00401234U);

  // One block: ABSOLUTE padding, HIGHLOW at offset 0, ABSOLUTE padding.
  put32(b, 0x400U, 0x1000U);         // PageRVA
  put32(b, 0x404U, 16U);             // BlockSize
  put16(b, 0x408U, 0x0000U);         // ABSOLUTE
  put16(b, 0x40AU, 0x3000U);         // HIGHLOW offset 0
  put16(b, 0x40CU, 0x0000U);         // ABSOLUTE
  put16(b, 0x40EU, 0x0000U);         // ABSOLUTE

  return b;
}

void print_location(const peparse::relocation_preview_error &err) {
  std::cerr << "  category=" << static_cast<int>(err.category);
  if (err.location_is_rva) {
    std::cerr << " rva=0x" << std::hex << err.rva;
  } else {
    std::cerr << " file_offset=0x" << std::hex << err.file_offset;
  }
  std::cerr << " " << err.what << std::dec << "\n";
}

} // namespace

int main() {
  const byte_vec image = make_image();
  const std::uint64_t new_base = 0x00500000ULL;

  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  if (!peparse::BuildBaseRelocationPreview(image.data(), image.size(),
                                           new_base, report, err)) {
    std::cerr << "preview failed\n";
    print_location(err);
    return 1;
  }

  std::cout << std::hex << std::setfill('0');
  std::cout << "machine=0x" << report.machine
            << (report.is_pe32_plus ? " PE32+\n" : " PE32\n");
  std::cout << "old ImageBase=0x" << report.old_image_base
            << " new ImageBase=0x" << report.new_image_base
            << " delta=0x" << report.delta << "\n";
  std::cout << "directory rva=0x" << report.directory_rva
            << " size=" << std::dec << report.directory_size
            << " file_offset=0x" << std::hex
            << report.directory_file_offset << "\n";

  for (std::size_t i = 0; i < report.blocks.size(); ++i) {
    const peparse::relocation_preview_block &blk = report.blocks[i];
    std::cout << "block page_rva=0x" << blk.page_rva
              << " size=" << std::dec << blk.block_size
              << " ops=" << blk.ops.size() << std::hex << "\n";
    for (std::size_t j = 0; j < blk.ops.size(); ++j) {
      const peparse::relocation_preview_op &op = blk.ops[j];
      std::cout << "  [" << std::dec << j << "] "
                << kind_name(op.kind) << " raw=0x" << std::hex
                << op.raw_entry << " entry_off=0x" << op.entry_file_offset;
      if (op.kind != peparse::RPK_ABSOLUTE) {
        std::cout << " target_rva=0x" << op.target_rva;
      }
      if (op.has_pair) {
        std::cout << " pair_off=0x" << op.pair_file_offset
                  << " pair_raw=0x" << op.pair_raw;
      }
      std::cout << "\n";
    }
  }

  for (std::size_t i = 0; i < report.intervals.size(); ++i) {
    const peparse::relocation_preview_interval &iv = report.intervals[i];
    std::cout << "interval rva=0x" << iv.rva << " bytes=" << std::dec
              << iv.original_bytes.size() << std::hex << " original=";
    for (std::size_t k = 0; k < iv.original_bytes.size(); ++k) {
      std::cout << std::setw(2)
                << static_cast<unsigned>(iv.original_bytes[k]);
    }
    std::cout << " preview=";
    for (std::size_t k = 0; k < iv.preview_bytes.size(); ++k) {
      std::cout << std::setw(2)
                << static_cast<unsigned>(iv.preview_bytes[k]);
    }
    std::cout << "\n";
  }
  return 0;
}
