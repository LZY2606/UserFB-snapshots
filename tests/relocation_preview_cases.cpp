/*
 * Relocation preview test cases.
 *
 * The minimal PE images below are constructed byte by byte in memory; no host
 * files are read. Every expected value is hand computed from the relocation
 * arithmetic documented in pe-parse/relocation_preview.h.
 */

#include <pe-parse/nt-headers.h>
#include <pe-parse/relocation_preview.h>

#include "task_support/check.h"

#include <cstdint>
#include <cstring>
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

void put64(byte_vec &b, std::uint32_t off, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    b[off + static_cast<std::uint32_t>(i)] =
        static_cast<std::uint8_t>(v >> (8 * i));
  }
}

std::uint16_t get16(const std::uint8_t *p) {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(p[0]) |
      (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t get32(const std::uint8_t *p) {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t get64(const std::uint8_t *p) {
  std::uint64_t v = 0U;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
  }
  return v;
}

struct section_spec {
  const char *name;
  std::uint32_t va;
  std::uint32_t virtual_size;
  std::uint32_t raw_ptr;
  std::uint32_t raw_size;
};

// A minimal, well formed little-endian PE image held entirely in memory.
struct minimal_pe {
  byte_vec bytes;
  bool plus; // PE32+ when true, PE32 otherwise

  minimal_pe(std::uint16_t machine,
             std::uint64_t image_base,
             const std::vector<section_spec> &sections,
             std::uint32_t reloc_rva,
             std::uint32_t reloc_size,
             std::uint16_t extra_characteristics = 0U) {
    plus = (machine == peparse::IMAGE_FILE_MACHINE_AMD64);
    std::uint32_t file_size = 0x600U;
    for (std::size_t i = 0; i < sections.size(); ++i) {
      std::uint64_t end = static_cast<std::uint64_t>(sections[i].raw_ptr) +
                          sections[i].raw_size;
      if (end > file_size) {
        file_size = static_cast<std::uint32_t>(end);
      }
    }
    bytes.assign(file_size, 0U);

    const std::uint32_t pe_off = 0x80U;
    const std::uint32_t fh = pe_off + 4U;
    const std::uint32_t opt = fh + 20U;
    const std::uint16_t opt_size = plus ? 0xF0U : 0xE0U;

    put16(bytes, 0x00U, peparse::MZ_MAGIC);
    put32(bytes, 0x3CU, pe_off);
    put32(bytes, pe_off, peparse::NT_MAGIC);

    put16(bytes, fh + 0U, machine);
    put16(bytes, fh + 2U, static_cast<std::uint16_t>(sections.size()));
    put16(bytes, fh + 16U, opt_size);
    std::uint16_t base_chars = plus ? 0x0022U : 0x0102U;
    put16(bytes, fh + 18U,
          static_cast<std::uint16_t>(base_chars | extra_characteristics));

    put16(bytes, opt + 0U,
          plus ? peparse::NT_OPTIONAL_64_MAGIC
               : peparse::NT_OPTIONAL_32_MAGIC);
    if (plus) {
      put64(bytes, opt + 24U, image_base);
    } else {
      put32(bytes, opt + 28U, static_cast<std::uint32_t>(image_base));
    }
    put32(bytes, opt + 32U, 0x1000U); // SectionAlignment
    put32(bytes, opt + 36U, 0x200U);  // FileAlignment
    put32(bytes, opt + 56U, 0x4000U); // SizeOfImage
    put32(bytes, opt + 60U, 0x200U);  // SizeOfHeaders
    const std::uint32_t dd_off = plus ? 112U : 96U;
    put32(bytes, opt + (plus ? 108U : 92U), 16U); // NumberOfRvaAndSizes
    const std::uint32_t reloc_dd =
        opt + dd_off + peparse::DIR_BASERELOC * 8U;
    put32(bytes, reloc_dd, reloc_rva);
    put32(bytes, reloc_dd + 4U, reloc_size);

    std::uint32_t table = opt + opt_size;
    for (std::size_t i = 0; i < sections.size(); ++i) {
      std::uint32_t s = table + static_cast<std::uint32_t>(i) * 40U;
      std::memcpy(&bytes[s], sections[i].name,
                  std::strlen(sections[i].name));
      put32(bytes, s + 8U, sections[i].virtual_size);
      put32(bytes, s + 12U, sections[i].va);
      put32(bytes, s + 16U, sections[i].raw_size);
      put32(bytes, s + 20U, sections[i].raw_ptr);
      put32(bytes, s + 36U, 0x40000040U);
    }
  }

  // Write one base relocation block (8-byte header followed by raw words).
  void reloc_block(std::uint32_t file_off,
                   std::uint32_t page_rva,
                   const std::vector<std::uint16_t> &words) {
    put32(bytes, file_off, page_rva);
    put32(bytes, file_off + 4U,
           8U + static_cast<std::uint32_t>(words.size()) * 2U);
    for (std::size_t i = 0; i < words.size(); ++i) {
      put16(bytes, file_off + 8U + static_cast<std::uint32_t>(i) * 2U,
            words[i]);
    }
  }

  bool build(std::uint64_t new_base,
             peparse::base_relocation_preview &report,
             peparse::relocation_preview_error &err) {
    return peparse::BuildBaseRelocationPreview(bytes.data(), bytes.size(),
                                                new_base, report, err);
  }
};

std::vector<section_spec> two_sections(std::uint32_t reloc_raw = 0x400U) {
  std::vector<section_spec> s;
  section_spec text = {".text", 0x1000U, 0x200U, 0x200U, 0x200U};
  section_spec reloc = {".reloc", 0x2000U, 0x200U, reloc_raw, 0x200U};
  s.push_back(text);
  s.push_back(reloc);
  return s;
}

// ---------------------------------------------------------------------------
// pairs group: HIGHADJ pairing, per-type arithmetic and type validation
// ---------------------------------------------------------------------------

// HIGHADJ with a negative signed low word; the paired word's high nibble is
// 0xF and must never be interpreted as a type-15 relocation.
int pairs_highadj_negative_lowword() {
  minimal_pe pe(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL,
                two_sections(), 0x2000U, 16U);
  // h = 0x1234 at RVA 0x1000 (file 0x200).
  put16(pe.bytes, 0x200U, 0x1234U);
  // Words: HIGHADJ off 0, pair 0xFFF6 (int16 = -10), two ABSOLUTE pads.
  pe.reloc_block(0x400U, 0x1000U,
                 std::vector<std::uint16_t>{0x4000U, 0xFFF6U, 0x0000U,
                                             0x0000U});

  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  TASK_CHECK(pe.build(0x003F0000ULL, report, err));
  TASK_CHECK(report.delta == -0x10000);
  TASK_CHECK(report.blocks.size() == 1U);
  TASK_CHECK(report.blocks[0].ops.size() == 3U);
  TASK_CHECK(report.total_op_count() == 3U);

  const peparse::relocation_preview_op &op = report.blocks[0].ops[0];
  TASK_CHECK(op.kind == peparse::RPK_HIGHADJ);
  TASK_CHECK(op.field_width == peparse::RPW_16);
  TASK_CHECK(op.raw_entry == 0x4000U);
  TASK_CHECK(op.raw_type == 4U);
  TASK_CHECK(op.page_offset == 0U);
  TASK_CHECK(op.target_rva == 0x1000U);
  TASK_CHECK(op.entry_file_offset == 0x408U);
  TASK_CHECK(op.has_pair);
  TASK_CHECK(op.pair_file_offset == 0x40AU);
  TASK_CHECK(op.pair_raw == 0xFFF6U);
  TASK_CHECK(op.pair_signed == -10);
  TASK_CHECK(op.target_spans.size() == 1U);
  TASK_CHECK(op.target_spans[0].section_index == 0U);
  TASK_CHECK(op.target_spans[0].file_offset == 0x200U);
  TASK_CHECK(get16(op.before.bytes) == 0x1234U);
  // ((0x1234 << 16) - 10 - 0x10000 + 0x8000) >> 16 == 0x1233
  TASK_CHECK(get16(op.after.bytes) == 0x1233U);

  // The paired word is consumed: it is not a fourth, type-15 operation.
  TASK_CHECK(report.blocks[0].ops[1].kind == peparse::RPK_ABSOLUTE);
  TASK_CHECK(report.blocks[0].ops[1].raw_entry == 0U);
  TASK_CHECK(report.blocks[0].ops[1].target_spans.empty());
  TASK_CHECK(report.blocks[0].ops[1].field_width == peparse::RPW_NONE);
  TASK_CHECK(report.blocks[0].ops[2].kind == peparse::RPK_ABSOLUTE);

  TASK_CHECK(report.intervals.size() == 1U);
  TASK_CHECK(report.intervals[0].rva == 0x1000U);
  TASK_CHECK(report.intervals[0].original_bytes.size() == 2U);
  TASK_CHECK(report.intervals[0].preview_bytes.size() == 2U);
  TASK_CHECK(get16(report.intervals[0].original_bytes.data()) == 0x1234U);
  TASK_CHECK(get16(report.intervals[0].preview_bytes.data()) == 0x1233U);
  return 0;
}

// HIGHADJ rounding carry with a positive low word.
int pairs_highadj_rounding_carry() {
  minimal_pe pe(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL,
                two_sections(), 0x2000U, 16U);
  put16(pe.bytes, 0x200U, 0x0001U); // h = 1
  // Pair 0x7FF0 -> int16 = 32752; its nibble 7 is also not a real entry.
  pe.reloc_block(0x400U, 0x1000U,
                 std::vector<std::uint16_t>{0x4000U, 0x7FF0U, 0x0000U,
                                             0x0000U});

  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  // delta = +0x20010, low32 high half 0x0002, low half 0x0010.
  TASK_CHECK(pe.build(0x00420010ULL, report, err));
  TASK_CHECK(report.delta == 0x20010);
  const peparse::relocation_preview_op &op = report.blocks[0].ops[0];
  TASK_CHECK(op.has_pair && op.pair_raw == 0x7FF0U);
  TASK_CHECK(op.pair_signed == 0x7FF0);
  // (0x10000 + 0x7FF0 + 0x20010 + 0x8000) = 0x40000; floored >>16 == 4.
  TASK_CHECK(get16(op.after.bytes) == 0x0004U);
  TASK_CHECK(report.blocks[0].ops.size() == 3U);
  return 0;
}

// A HIGHADJ that is the last word of its block has no paired word.
int pairs_highadj_missing_pair() {
  minimal_pe pe(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL,
                two_sections(), 0x2000U, 12U);
  pe.reloc_block(0x400U, 0x1000U,
                 std::vector<std::uint16_t>{0x0000U, 0x4000U});

  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  TASK_CHECK(!pe.build(0x00410000ULL, report, err));
  TASK_CHECK(err.category == peparse::RPE_TRUNCATION);
  TASK_CHECK(!err.location_is_rva);
  TASK_CHECK(err.file_offset == 0x40AU); // HIGHADJ entry itself
  return 0;
}

// An unknown 4-bit type is rejected with its raw entry location.
int pairs_unknown_type() {
  minimal_pe pe(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL,
                two_sections(), 0x2000U, 12U);
  pe.reloc_block(0x400U, 0x1000U,
                 std::vector<std::uint16_t>{0x5000U, 0x0000U});

  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  TASK_CHECK(!pe.build(0x00410000ULL, report, err));
  TASK_CHECK(err.category == peparse::RPE_UNKNOWN_TYPE);
  TASK_CHECK(!err.location_is_rva);
  TASK_CHECK(err.file_offset == 0x408U);
  return 0;
}

// Known types that do not belong to the target machine are distinguished
// from genuinely unknown type numbers.
int pairs_machine_mismatch() {
  // AMD64 image carrying an I386-only HIGHLOW entry.
  minimal_pe pe64(peparse::IMAGE_FILE_MACHINE_AMD64, 0x140000000ULL,
                  two_sections(), 0x2000U, 12U);
  pe64.reloc_block(0x400U, 0x1000U,
                   std::vector<std::uint16_t>{0x3000U, 0x0000U});
  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  TASK_CHECK(!pe64.build(0x150000000ULL, report, err));
  TASK_CHECK(err.category == peparse::RPE_MACHINE_MISMATCH);
  TASK_CHECK(err.file_offset == 0x408U);

  // I386 image carrying an AMD64-only DIR64 entry.
  minimal_pe pe32(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL,
                  two_sections(), 0x2000U, 12U);
  pe32.reloc_block(0x400U, 0x1000U,
                   std::vector<std::uint16_t>{0xA000U, 0x0000U});
  TASK_CHECK(!pe32.build(0x00410000ULL, report, err));
  TASK_CHECK(err.category == peparse::RPE_MACHINE_MISMATCH);
  return 0;
}

// HIGHLOW with a mathematically negative delta, modulo 2^32.
int pairs_negative_delta_highlow() {
  minimal_pe pe(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL,
                two_sections(), 0x2000U, 12U);
  put32(pe.bytes, 0x200U, 0x00403000U);
  pe.reloc_block(0x400U, 0x1000U,
                 std::vector<std::uint16_t>{0x3000U, 0x0000U});

  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  TASK_CHECK(pe.build(0x003F0000ULL, report, err)); // delta = -0x10000
  TASK_CHECK(report.delta == -0x10000);
  const peparse::relocation_preview_op &op = report.blocks[0].ops[0];
  TASK_CHECK(op.kind == peparse::RPK_HIGHLOW);
  TASK_CHECK(op.field_width == peparse::RPW_32);
  TASK_CHECK(get32(op.before.bytes) == 0x00403000U);
  TASK_CHECK(get32(op.after.bytes) == 0x003F3000U);
  return 0;
}

// HIGH and LOW take the high and low halves of delta's low 32 bits.
int pairs_high_low_halves() {
  minimal_pe pe(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL,
                two_sections(), 0x2000U, 12U);
  // HIGH target word 0xAAAA at 0x1000; LOW target word 0x1111 at 0x1002.
  put16(pe.bytes, 0x200U, 0xAAAAU);
  put16(pe.bytes, 0x202U, 0x1111U);
  // HIGH off 0, LOW off 2.
  pe.reloc_block(0x400U, 0x1000U,
                 std::vector<std::uint16_t>{0x1000U, 0x2002U});

  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  // delta = 0x12345678: high half 0x1234, low half 0x5678.
  TASK_CHECK(pe.build(0x12745678ULL, report, err));
  TASK_CHECK(report.delta == 0x12345678);
  const peparse::relocation_preview_block &blk = report.blocks[0];
  TASK_CHECK(blk.ops[0].kind == peparse::RPK_HIGH);
  TASK_CHECK(blk.ops[0].target_rva == 0x1000U);
  TASK_CHECK(get16(blk.ops[0].before.bytes) == 0xAAAAU);
  TASK_CHECK(get16(blk.ops[0].after.bytes) == 0xBCDEU);
  TASK_CHECK(blk.ops[1].kind == peparse::RPK_LOW);
  TASK_CHECK(blk.ops[1].target_rva == 0x1002U);
  TASK_CHECK(get16(blk.ops[1].before.bytes) == 0x1111U);
  TASK_CHECK(get16(blk.ops[1].after.bytes) == 0x6789U);

  // Merged interval spans all four bytes, in RVA order.
  TASK_CHECK(report.intervals.size() == 1U);
  TASK_CHECK(report.intervals[0].rva == 0x1000U);
  TASK_CHECK(report.intervals[0].original_bytes.size() == 4U);
  TASK_CHECK(get32(report.intervals[0].original_bytes.data()) ==
             0x1111AAAAU);
  TASK_CHECK(get32(report.intervals[0].preview_bytes.data()) ==
             0x6789BCDEU);
  return 0;
}

// A zero move still walks the whole directory; ABSOLUTE is sourced padding
// and never touches a target.
int pairs_absolute_and_zero_delta() {
  minimal_pe pe(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL,
                two_sections(), 0x2000U, 16U);
  put32(pe.bytes, 0x200U, 0xDEADBEEFU);
  pe.reloc_block(0x400U, 0x1000U,
                 std::vector<std::uint16_t>{0x0000U, 0x3000U, 0x0000U,
                                             0x0000U});

  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  TASK_CHECK(pe.build(0x00400000ULL, report, err));
  TASK_CHECK(report.delta == 0);
  TASK_CHECK(report.blocks.size() == 1U);
  TASK_CHECK(report.blocks[0].block_size == 16U);
  TASK_CHECK(report.blocks[0].ops.size() == 4U);
  TASK_CHECK(report.blocks[0].ops[0].kind == peparse::RPK_ABSOLUTE);
  TASK_CHECK(report.blocks[0].ops[0].target_spans.empty());
  TASK_CHECK(report.blocks[0].ops[0].field_width == peparse::RPW_NONE);

  const peparse::relocation_preview_op &hl = report.blocks[0].ops[1];
  TASK_CHECK(hl.kind == peparse::RPK_HIGHLOW);
  TASK_CHECK(get32(hl.before.bytes) == 0xDEADBEEFU);
  TASK_CHECK(get32(hl.after.bytes) == 0xDEADBEEFU);
  TASK_CHECK(report.intervals.size() == 1U);
  TASK_CHECK(report.intervals[0].preview_bytes ==
             report.intervals[0].original_bytes);
  return 0;
}

// RELOCS_STRIPPED rejects a non-zero move but allows a zero-move audit.
int pairs_stripped_handling() {
  minimal_pe stripped(
      peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL, two_sections(),
      0x2000U, 12U, peparse::IMAGE_FILE_RELOCS_STRIPPED);
  stripped.reloc_block(0x400U, 0x1000U,
                       std::vector<std::uint16_t>{0x3000U, 0x0000U});

  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  TASK_CHECK(!stripped.build(0x00410000ULL, report, err));
  TASK_CHECK(err.category == peparse::RPE_UNSUPPORTED);

  peparse::base_relocation_preview zero_report;
  TASK_CHECK(stripped.build(0x00400000ULL, zero_report, err));
  TASK_CHECK(zero_report.delta == 0);
  TASK_CHECK(zero_report.total_op_count() == 2U);
  return 0;
}

// The byte-reversed characteristics flags are an explicit refusal.
int pairs_reversed_bytes() {
  minimal_pe pe(
      peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL, two_sections(),
      0x2000U, 12U, peparse::IMAGE_FILE_BYTES_REVERSED_LO);
  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  TASK_CHECK(!pe.build(0x00410000ULL, report, err));
  TASK_CHECK(err.category == peparse::RPE_UNSUPPORTED);
  return 0;
}

// ---------------------------------------------------------------------------
// overlay group: sequential replay, overlaps, aliasing and report ownership
// ---------------------------------------------------------------------------

// The same target relocated twice: the second read sees the first preview.
int overlay_repeated_highlow() {
  minimal_pe pe(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL,
                two_sections(), 0x2000U, 12U);
  put32(pe.bytes, 0x200U, 0x00001000U);
  pe.reloc_block(0x400U, 0x1000U,
                 std::vector<std::uint16_t>{0x3000U, 0x3000U});

  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  TASK_CHECK(pe.build(0x00420000ULL, report, err)); // delta 0x20000
  const peparse::relocation_preview_block &blk = report.blocks[0];
  TASK_CHECK(blk.ops.size() == 2U);
  TASK_CHECK(blk.ops[0].target_rva == blk.ops[1].target_rva);
  TASK_CHECK(get32(blk.ops[0].before.bytes) == 0x00001000U);
  TASK_CHECK(get32(blk.ops[0].after.bytes) == 0x00021000U);
  TASK_CHECK(get32(blk.ops[1].before.bytes) == 0x00021000U);
  TASK_CHECK(get32(blk.ops[1].after.bytes) == 0x00041000U);

  TASK_CHECK(report.intervals.size() == 1U);
  const peparse::relocation_preview_interval &iv = report.intervals[0];
  TASK_CHECK(iv.rva == 0x1000U && iv.original_bytes.size() == 4U);
  TASK_CHECK(get32(iv.original_bytes.data()) == 0x00001000U);
  TASK_CHECK(get32(iv.preview_bytes.data()) == 0x00041000U);
  // Every physical byte records both writers, in application order.
  TASK_CHECK(iv.op_indices.size() == 8U);
  for (std::size_t i = 0; i < 4U; ++i) {
    TASK_CHECK(iv.op_indices[i * 2U] == 0U);
    TASK_CHECK(iv.op_indices[i * 2U + 1U] == 1U);
  }
  return 0;
}

// A 16-bit relocation partially overlaps the high half of a 32-bit one; it
// must read the previewed bytes, not the original file.
int overlay_partial_overlap() {
  minimal_pe pe(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL,
                two_sections(), 0x2000U, 12U);
  // Dword at RVA 0x1000 = 0x12345678, stored 78 56 34 12.
  put32(pe.bytes, 0x200U, 0x12345678U);
  // HIGHLOW off 0, then HIGH off 2 (bytes 0x1002..0x1003).
  pe.reloc_block(0x400U, 0x1000U,
                 std::vector<std::uint16_t>{0x3000U, 0x1002U});

  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  // delta = 0x00010001: high addend 1, low addend 1.
  TASK_CHECK(pe.build(0x00410001ULL, report, err));
  const peparse::relocation_preview_block &blk = report.blocks[0];
  TASK_CHECK(get32(blk.ops[0].after.bytes) == 0x12355679U);
  TASK_CHECK(blk.ops[1].kind == peparse::RPK_HIGH);
  TASK_CHECK(blk.ops[1].target_rva == 0x1002U);
  TASK_CHECK(get16(blk.ops[1].before.bytes) == 0x1235U);
  TASK_CHECK(get16(blk.ops[1].after.bytes) == 0x1236U);

  TASK_CHECK(report.intervals.size() == 1U);
  const peparse::relocation_preview_interval &iv = report.intervals[0];
  TASK_CHECK(iv.original_bytes.size() == 4U);
  // Final layout: 79 56 36 12.
  TASK_CHECK(iv.preview_bytes[0] == 0x79U);
  TASK_CHECK(iv.preview_bytes[1] == 0x56U);
  TASK_CHECK(iv.preview_bytes[2] == 0x36U);
  TASK_CHECK(iv.preview_bytes[3] == 0x12U);
  TASK_CHECK(iv.op_indices.size() == 6U);
  TASK_CHECK(iv.op_indices[0] == 0U && iv.op_indices[1] == 0U);
  TASK_CHECK(iv.op_indices[2] == 0U && iv.op_indices[3] == 1U);
  TASK_CHECK(iv.op_indices[4] == 0U && iv.op_indices[5] == 1U);
  return 0;
}

// Separate targets produce separate, RVA-sorted, merged intervals tied back
// to the operations that wrote them.
int overlay_intervals_sorted() {
  minimal_pe pe(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL,
                two_sections(), 0x2000U, 16U);
  put32(pe.bytes, 0x210U, 0x0000000AU); // RVA 0x1010
  put16(pe.bytes, 0x200U, 0x000BU);     // RVA 0x1000
  pe.reloc_block(0x400U, 0x1000U,
                 std::vector<std::uint16_t>{0x1000U, 0x3010U, 0x0000U,
                                             0x0000U});

  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  TASK_CHECK(pe.build(0x00410002ULL, report, err)); // delta 0x10002
  TASK_CHECK(report.intervals.size() == 2U);
  TASK_CHECK(report.intervals[0].rva == 0x1000U);
  TASK_CHECK(report.intervals[1].rva == 0x1010U);
  TASK_CHECK(report.intervals[0].original_bytes.size() == 2U);
  TASK_CHECK(get16(report.intervals[0].original_bytes.data()) == 0x000BU);
  // HIGH addend = high half of 0x10002 -> 0x0001.
  TASK_CHECK(get16(report.intervals[0].preview_bytes.data()) == 0x000CU);
  TASK_CHECK(report.intervals[1].original_bytes.size() == 4U);
  TASK_CHECK(get32(report.intervals[1].original_bytes.data()) == 0x0000000AU);
  // HIGHLOW adds the full delta 0x10002 modulo 2^32.
  TASK_CHECK(get32(report.intervals[1].preview_bytes.data()) ==
             0x0001000CU);
  TASK_CHECK(report.intervals[0].op_indices.size() == 2U);
  TASK_CHECK(report.intervals[1].op_indices.size() == 4U);
  return 0;
}

// Two sections claiming the same raw bytes give one physical byte two owners.
int overlay_physical_alias_rejected() {
  std::vector<section_spec> s;
  section_spec a = {".text", 0x1000U, 0x400U, 0x200U, 0x400U};
  section_spec b = {".data", 0x3000U, 0x100U, 0x400U, 0x100U};
  s.push_back(a);
  s.push_back(b);
  minimal_pe pe(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL, s,
                0x2000U, 12U);
  pe.reloc_block(0x400U, 0x3000U,
                 std::vector<std::uint16_t>{0x3000U, 0x0000U});

  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  TASK_CHECK(!pe.build(0x00410000ULL, report, err));
  TASK_CHECK(err.category == peparse::RPE_LAYOUT_CONFLICT);
  TASK_CHECK(!err.location_is_rva);
  TASK_CHECK(err.file_offset == 0x400U); // second section's raw pointer
  return 0;
}

// A target whose bytes land inside the relocation directory is refused.
int overlay_directory_write_rejected() {
  minimal_pe pe(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL,
                two_sections(), 0x2000U, 12U);
  // HIGHLOW targeting RVA 0x2000, the first directory byte (file 0x400).
  pe.reloc_block(0x400U, 0x2000U,
                 std::vector<std::uint16_t>{0x3000U, 0x0000U});

  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  TASK_CHECK(!pe.build(0x00410000ULL, report, err));
  TASK_CHECK(err.category == peparse::RPE_LAYOUT_CONFLICT);
  TASK_CHECK(err.file_offset == 0x400U);
  return 0;
}

// AMD64 DIR64 arithmetic, including a 64-bit wrap to zero.
int overlay_dir64_amd64() {
  minimal_pe pe(peparse::IMAGE_FILE_MACHINE_AMD64, 0x140000000ULL,
                two_sections(), 0x2000U, 12U);
  put64(pe.bytes, 0x200U, 0x0000000140001000ULL); // RVA 0x1000
  // -0x40000000 mod 2^64, so adding the delta wraps exactly to zero.
  put64(pe.bytes, 0x210U, 0xFFFFFFFFC0000000ULL); // RVA 0x1010
  pe.reloc_block(0x400U, 0x1000U,
                 std::vector<std::uint16_t>{0xA000U, 0xA010U});

  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  TASK_CHECK(pe.build(0x180000000ULL, report, err)); // delta 0x40000000
  TASK_CHECK(report.is_pe32_plus);
  TASK_CHECK(report.machine == peparse::IMAGE_FILE_MACHINE_AMD64);
  TASK_CHECK(report.delta == 0x40000000);
  const peparse::relocation_preview_block &blk = report.blocks[0];
  TASK_CHECK(blk.ops.size() == 2U);
  TASK_CHECK(blk.ops[0].field_width == peparse::RPW_64);
  TASK_CHECK(blk.ops[0].target_rva == 0x1000U);
  TASK_CHECK(get64(blk.ops[0].before.bytes) ==
             0x0000000140001000ULL);
  TASK_CHECK(get64(blk.ops[0].after.bytes) ==
             0x0000000180001000ULL);
  TASK_CHECK(blk.ops[1].target_rva == 0x1010U);
  TASK_CHECK(get64(blk.ops[1].before.bytes) ==
             0xFFFFFFFFC0000000ULL);
  TASK_CHECK(get64(blk.ops[1].after.bytes) == 0ULL);
  return 0;
}

// The report owns its storage: destroying the input leaves it readable.
int overlay_report_outlives_input() {
  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  {
    minimal_pe pe(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL,
                  two_sections(), 0x2000U, 12U);
    put32(pe.bytes, 0x200U, 0x00403000U);
    pe.reloc_block(0x400U, 0x1000U,
                   std::vector<std::uint16_t>{0x3000U, 0x0000U});
    TASK_CHECK(pe.build(0x003F0000ULL, report, err));
    // Scrub and release the input buffer on purpose.
    std::memset(pe.bytes.data(), 0xCC, pe.bytes.size());
  }

  TASK_CHECK(report.old_image_base == 0x00400000ULL);
  TASK_CHECK(report.new_image_base == 0x003F0000ULL);
  TASK_CHECK(report.blocks.size() == 1U);
  TASK_CHECK(report.blocks[0].header_file_offset == 0x400U);
  TASK_CHECK(get32(report.blocks[0].ops[0].after.bytes) ==
             0x003F3000U);
  TASK_CHECK(report.intervals.size() == 1U);
  TASK_CHECK(get32(report.intervals[0].preview_bytes.data()) ==
             0x003F3000U);
  TASK_CHECK(report.intervals[0].op_indices.size() == 4U);
  return 0;
}

// The input bytes are never modified by a preview.
int overlay_input_unchanged() {
  minimal_pe pe(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL,
                two_sections(), 0x2000U, 12U);
  put32(pe.bytes, 0x200U, 0x00403000U);
  pe.reloc_block(0x400U, 0x1000U,
                 std::vector<std::uint16_t>{0x3000U, 0x0000U});
  byte_vec snapshot = pe.bytes;

  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  TASK_CHECK(pe.build(0x003F0000ULL, report, err));
  TASK_CHECK(pe.bytes == snapshot);

  // A failed call must not mutate the input either.
  minimal_pe bad(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL,
                 two_sections(), 0x2000U, 8U);
  bad.reloc_block(0x400U, 0x1000U,
                  std::vector<std::uint16_t>{0x5000U});
  byte_vec bad_snapshot = bad.bytes;
  TASK_CHECK(!bad.build(0x00410000ULL, report, err));
  TASK_CHECK(bad.bytes == bad_snapshot);
  return 0;
}

// ---------------------------------------------------------------------------
// mapping group: RVA/offset translation, directory placement, malformed maps
// ---------------------------------------------------------------------------

// The directory may start at a non-zero offset inside its section.
int mapping_nonzero_directory_offset() {
  std::vector<section_spec> s;
  section_spec text = {".text", 0x1000U, 0x200U, 0x200U, 0x200U};
  section_spec reloc = {".reloc", 0x2000U, 0x200U, 0x400U, 0x200U};
  s.push_back(text);
  s.push_back(reloc);
  // Directory RVA 0x2040 -> file offset 0x440, exactly 12 bytes long.
  minimal_pe pe(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL, s,
                0x2040U, 12U);
  pe.reloc_block(0x440U, 0x1000U,
                 std::vector<std::uint16_t>{0x3000U, 0x0000U});
  put32(pe.bytes, 0x200U, 0x11223344U);

  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  TASK_CHECK(pe.build(0x00410000ULL, report, err));
  TASK_CHECK(report.directory_rva == 0x2040U);
  TASK_CHECK(report.directory_size == 12U);
  TASK_CHECK(report.directory_file_offset == 0x440U);
  TASK_CHECK(report.blocks[0].header_file_offset == 0x440U);
  TASK_CHECK(report.blocks[0].ops[0].entry_file_offset == 0x448U);
  TASK_CHECK(get32(report.blocks[0].ops[0].after.bytes) ==
             0x11233344U);
  return 0;
}

// A continuous 32-bit field straddles two adjacent sections; each byte is
// located independently through the section table.
int mapping_cross_section_field() {
  std::vector<section_spec> s;
  section_spec a = {".a", 0x1000U, 0x200U, 0x200U, 0x200U};
  section_spec b = {".b", 0x1200U, 0x200U, 0x400U, 0x200U};
  section_spec r = {".reloc", 0x2000U, 0x200U, 0x600U, 0x200U};
  s.push_back(a);
  s.push_back(b);
  s.push_back(r);
  minimal_pe pe(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL, s,
                0x2000U, 12U);
  // RVA 0x11FE: file 0x3FE/0x3FF in .a, then 0x400/0x401 in .b.
  pe.bytes[0x3FE] = 0x78U;
  pe.bytes[0x3FF] = 0x56U;
  pe.bytes[0x400] = 0x34U;
  pe.bytes[0x401] = 0x12U;
  pe.reloc_block(0x600U, 0x1000U,
                 std::vector<std::uint16_t>{0x31FEU, 0x0000U});

  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  TASK_CHECK(pe.build(0x00410001ULL, report, err));
  const peparse::relocation_preview_op &op =
      report.blocks[0].ops[0];
  TASK_CHECK(op.target_rva == 0x11FEU);
  TASK_CHECK(op.target_spans.size() == 2U);
  TASK_CHECK(op.target_spans[0].section_index == 0U);
  TASK_CHECK(op.target_spans[0].file_offset == 0x3FEU);
  TASK_CHECK(op.target_spans[0].size == 2U);
  TASK_CHECK(op.target_spans[1].section_index == 1U);
  TASK_CHECK(op.target_spans[1].file_offset == 0x400U);
  TASK_CHECK(op.target_spans[1].size == 2U);
  TASK_CHECK(get32(op.before.bytes) == 0x12345678U);
  TASK_CHECK(get32(op.after.bytes) == 0x12355679U);

  // The merged interval still presents one continuous RVA run.
  TASK_CHECK(report.intervals.size() == 1U);
  TASK_CHECK(report.intervals[0].rva == 0x11FEU);
  TASK_CHECK(report.intervals[0].original_bytes.size() == 4U);
  return 0;
}

// An RVA in a gap between sections cannot be located.
int mapping_gap_rejected() {
  std::vector<section_spec> s;
  section_spec text = {".text", 0x1000U, 0x200U, 0x200U, 0x200U};
  section_spec reloc = {".reloc", 0x2000U, 0x200U, 0x400U, 0x200U};
  s.push_back(text);
  s.push_back(reloc);
  minimal_pe pe(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL, s,
                0x2000U, 12U);
  pe.reloc_block(0x400U, 0x1000U,
                 std::vector<std::uint16_t>{0x3000U, 0x0000U});
  // Retarget the entry at RVA 0x1500, which no section owns.
  put16(pe.bytes, 0x408U, 0x3500U);

  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  TASK_CHECK(!pe.build(0x00410000ULL, report, err));
  TASK_CHECK(err.category == peparse::RPE_ADDRESS);
  TASK_CHECK(err.location_is_rva);
  TASK_CHECK(err.rva == 0x1500U);
  return 0;
}

// An RVA inside the uninitialized virtual tail has no raw bytes.
int mapping_virtual_tail_rejected() {
  std::vector<section_spec> s;
  section_spec text = {".text", 0x1000U, 0x400U, 0x200U, 0x200U};
  section_spec reloc = {".reloc", 0x2000U, 0x200U, 0x400U, 0x200U};
  s.push_back(text);
  s.push_back(reloc);
  minimal_pe pe(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL, s,
                0x2000U, 12U);
  // Page 0x1000, offset 0x300 -> RVA 0x1300, beyond the 0x200 raw bytes.
  pe.reloc_block(0x400U, 0x1000U,
                 std::vector<std::uint16_t>{0x3300U, 0x0000U});

  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  TASK_CHECK(!pe.build(0x00410000ULL, report, err));
  TASK_CHECK(err.category == peparse::RPE_ADDRESS);
  TASK_CHECK(err.location_is_rva);
  TASK_CHECK(err.rva == 0x1300U);
  return 0;
}

// Targets inside the headers are not supported.
int mapping_header_target_rejected() {
  minimal_pe pe(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL,
                two_sections(), 0x2000U, 12U);
  // Page 0, offset 0x100 -> header RVA 0x100 (< SizeOfHeaders 0x200).
  pe.reloc_block(0x400U, 0x0000U,
                 std::vector<std::uint16_t>{0x3100U, 0x0000U});

  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  TASK_CHECK(!pe.build(0x00410000ULL, report, err));
  TASK_CHECK(err.category == peparse::RPE_UNSUPPORTED);
  TASK_CHECK(err.location_is_rva);
  TASK_CHECK(err.rva == 0x100U);
  return 0;
}

// Blocks that do not fit the directory size or are too small are rejected.
int mapping_truncated_blocks() {
  // Directory claims 8 bytes but its block claims 12.
  minimal_pe pe(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL,
                two_sections(), 0x2000U, 8U);
  pe.reloc_block(0x400U, 0x1000U,
                 std::vector<std::uint16_t>{0x3000U, 0x0000U});
  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  TASK_CHECK(!pe.build(0x00410000ULL, report, err));
  TASK_CHECK(err.category == peparse::RPE_TRUNCATION);
  TASK_CHECK(err.location_is_rva);
  TASK_CHECK(err.rva == 0x2000U);

  // A block size below the eight-byte header is malformed.
  minimal_pe small(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL,
                   two_sections(), 0x2000U, 8U);
  put32(small.bytes, 0x400U, 0x1000U);
  put32(small.bytes, 0x404U, 4U);
  TASK_CHECK(!small.build(0x00410000ULL, report, err));
  TASK_CHECK(err.category == peparse::RPE_FORMAT);
  return 0;
}

// No directory descriptor: never guess pointers; an empty report comes back
// for both a zero and a non-zero move.
int mapping_missing_directory() {
  minimal_pe pe(peparse::IMAGE_FILE_MACHINE_I386, 0x00400000ULL,
                two_sections(), 0U, 0U);

  peparse::base_relocation_preview report;
  peparse::relocation_preview_error err;
  TASK_CHECK(pe.build(0x00400000ULL, report, err));
  TASK_CHECK(!report.directory_present);
  TASK_CHECK(report.blocks.empty());
  TASK_CHECK(report.intervals.empty());
  TASK_CHECK(report.total_op_count() == 0U);

  peparse::base_relocation_preview moved;
  TASK_CHECK(pe.build(0x00500000ULL, moved, err));
  TASK_CHECK(!moved.directory_present);
  TASK_CHECK(moved.blocks.empty());
  TASK_CHECK(moved.delta == 0x100000);
  return 0;
}

} // namespace

// External linkage: task_support/driver.c (compiled as C++) looks these up.
const task_case task_cases[] = {
    {"pairs", "highadj_negative_lowword", pairs_highadj_negative_lowword},
    {"pairs", "highadj_rounding_carry", pairs_highadj_rounding_carry},
    {"pairs", "highadj_missing_pair", pairs_highadj_missing_pair},
    {"pairs", "unknown_type", pairs_unknown_type},
    {"pairs", "machine_mismatch", pairs_machine_mismatch},
    {"pairs", "negative_delta_highlow", pairs_negative_delta_highlow},
    {"pairs", "high_low_halves", pairs_high_low_halves},
    {"pairs", "absolute_zero_delta", pairs_absolute_and_zero_delta},
    {"pairs", "stripped_handling", pairs_stripped_handling},
    {"pairs", "reversed_bytes", pairs_reversed_bytes},

    {"overlay", "repeated_highlow", overlay_repeated_highlow},
    {"overlay", "partial_overlap", overlay_partial_overlap},
    {"overlay", "intervals_sorted", overlay_intervals_sorted},
    {"overlay", "physical_alias", overlay_physical_alias_rejected},
    {"overlay", "directory_write", overlay_directory_write_rejected},
    {"overlay", "dir64_amd64", overlay_dir64_amd64},
    {"overlay", "outlives_input", overlay_report_outlives_input},
    {"overlay", "input_unchanged", overlay_input_unchanged},

    {"mapping", "nonzero_directory_offset", mapping_nonzero_directory_offset},
    {"mapping", "cross_section_field", mapping_cross_section_field},
    {"mapping", "gap_rejected", mapping_gap_rejected},
    {"mapping", "virtual_tail_rejected", mapping_virtual_tail_rejected},
    {"mapping", "header_target_rejected", mapping_header_target_rejected},
    {"mapping", "truncated_blocks", mapping_truncated_blocks},
    {"mapping", "missing_directory", mapping_missing_directory},
};

const std::size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
