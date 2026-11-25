/*
The MIT License (MIT)

Copyright (c) 2013 Andrew Ruef

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <pe-parse/relocation_preview.h>

#include <pe-parse/nt-headers.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace peparse {

namespace {

#define RP_FAIL(cat, is_rva, loc, msg)                                        \
  do {                                                                        \
    err.category = (cat);                                                     \
    err.location_is_rva = (is_rva);                                          \
    if (is_rva) {                                                             \
      err.rva = (loc);                                                        \
      err.file_offset = 0U;                                                   \
    } else {                                                                  \
      err.file_offset = (loc);                                                \
      err.rva = 0U;                                                           \
    }                                                                         \
    err.what = (msg);                                                         \
    return false;                                                             \
  } while (0)

std::uint16_t rd16(const std::uint8_t *p) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                    (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t rd32(const std::uint8_t *p) {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

bool add32(std::uint32_t a, std::uint32_t b, std::uint32_t &out) {
  std::uint64_t s = static_cast<std::uint64_t>(a) +
                    static_cast<std::uint64_t>(b);
  if (s > 0xFFFFFFFFULL) {
    return false;
  }
  out = static_cast<std::uint32_t>(s);
  return true;
}

// A section together with the byte ranges that are actually backed by raw
// file data.
struct section_model {
  std::size_t index;
  std::uint32_t va;          // VirtualAddress
  std::uint32_t mem_extent;  // bytes occupied in the RVA space
  std::uint32_t raw_ptr;     // PointerToRawData
  std::uint32_t raw_size;    // SizeOfRawData
  std::uint32_t backed_len;  // RVA bytes backed by raw data
};

// One byte of the previewed image. A physical byte can be touched by several
// operations in sequence (repeated or partially overlapping targets); every
// touching operation is recorded in application order.
struct preview_cell {
  std::uint8_t original;
  std::uint8_t preview;
  std::vector<std::size_t> writer_ops;
};

class preview_builder {
public:
  preview_builder(const std::uint8_t *data_,
                  std::size_t size_,
                  std::uint64_t new_base_,
                  base_relocation_preview &report_,
                  relocation_preview_error &err_)
      : data(data_),
        size(size_),
        new_base(new_base_),
        report(report_),
        err(err_) {
  }

  bool run() {
    if (!parse_headers()) {
      return false;
    }
    if (!build_sections()) {
      return false;
    }
    if (!prepare_directory()) {
      return false;
    }
    if (directory_present) {
      if (!parse_blocks()) {
        return false;
      }
      if (!build_intervals()) {
        return false;
      }
    }
    report.directory_present = directory_present;
    return true;
  }

private:
  const std::uint8_t *data;
  std::size_t size;
  std::uint64_t new_base;
  base_relocation_preview &report;
  relocation_preview_error &err;

  bool is_pe32_plus;
  std::uint16_t machine;
  std::uint16_t characteristics;
  std::uint16_t section_count;
  std::uint16_t opt_size;
  std::uint32_t size_of_headers;
  std::uint32_t opt_header_offset;
  std::uint32_t image_base_offset;
  std::uint64_t old_base;
  std::uint32_t dir_rva;
  std::uint32_t dir_size;
  bool directory_present;

  std::vector<section_model> sections;
  std::vector<relocation_preview_span> dir_spans;
  std::unordered_map<std::uint32_t, preview_cell> cells;
  std::size_t op_serial;

  bool need(std::size_t off, std::size_t len, std::uint32_t loc) {
    if (off > size || len > size - off) {
      RP_FAIL(RPE_TRUNCATION, false, loc,
              "structure extends past the end of the file");
    }
    return true;
  }

  bool parse_headers() {
    if (data == nullptr) {
      RP_FAIL(RPE_FORMAT, false, 0U, "null input buffer");
    }
    if (!need(0, sizeof(std::uint16_t) * 2, 0U)) {
      return false;
    }
    if (rd16(data) != MZ_MAGIC) {
      RP_FAIL(RPE_FORMAT, false, 0U, "missing MZ signature");
    }
    // e_lfanew lives at offset 0x3C of the DOS header.
    if (!need(0x3CU, sizeof(std::uint32_t), 0x3CU)) {
      return false;
    }
    std::uint32_t pe_off = rd32(data + 0x3CU);
    if (!need(pe_off, 4U + 20U, pe_off)) {
      return false;
    }
    if (rd32(data + pe_off) != NT_MAGIC) {
      RP_FAIL(RPE_FORMAT, false, pe_off, "missing PE signature");
    }
    std::uint32_t fh = pe_off + 4U;
    machine = rd16(data + fh);
    section_count = rd16(data + fh + 2U);
    opt_size = rd16(data + fh + 16U);
    characteristics = rd16(data + fh + 18U);

    if ((characteristics &
         (IMAGE_FILE_BYTES_REVERSED_LO | IMAGE_FILE_BYTES_REVERSED_HI)) != 0U) {
      RP_FAIL(RPE_UNSUPPORTED, false, fh + 18U,
              "byte-reversed images are not supported");
    }
    if (machine == IMAGE_FILE_MACHINE_I386) {
      is_pe32_plus = false;
    } else if (machine == IMAGE_FILE_MACHINE_AMD64) {
      is_pe32_plus = true;
    } else {
      RP_FAIL(RPE_UNSUPPORTED, false, fh,
              "only I386 PE32 and AMD64 PE32+ machines are supported");
    }
    if (section_count == 0U) {
      RP_FAIL(RPE_FORMAT, false, fh + 2U, "image has no sections");
    }
    opt_header_offset = fh + 20U;
    if (!need(opt_header_offset, std::size_t(opt_size), opt_header_offset)) {
      return false;
    }
    std::uint16_t opt_magic = rd16(data + opt_header_offset);
    if (is_pe32_plus) {
      if (opt_magic != NT_OPTIONAL_64_MAGIC) {
        RP_FAIL(RPE_FORMAT, false, opt_header_offset,
                "AMD64 image must carry a PE32+ optional header");
      }
      // The data directory slot we read ends at optional offset 120.
      if (opt_size < 120U) {
        RP_FAIL(RPE_FORMAT, false, opt_header_offset,
                "PE32+ optional header is too small for its data "
                "directories");
      }
      image_base_offset = opt_header_offset + 24U;
      // ImageBase is a 64-bit field at offset 24 of the PE32+ optional header.
      if (!need(image_base_offset, sizeof(std::uint64_t),
                image_base_offset)) {
        return false;
      }
      old_base = rd32(data + image_base_offset);
      old_base |= static_cast<std::uint64_t>(
                      rd32(data + image_base_offset + 4U))
                  << 32;
      size_of_headers = rd32(data + opt_header_offset + 60U);
      // Data directory starts at offset 112 of the PE32+ optional header.
      std::uint32_t dd = opt_header_offset + 112U;
      std::uint32_t num_dirs = rd32(data + opt_header_offset + 108U);
      if (num_dirs <= DIR_BASERELOC) {
        RP_FAIL(RPE_FORMAT, false, opt_header_offset + 108U,
                "optional header has no base relocation directory slot");
      }
      if (!need(dd + DIR_BASERELOC * 8U, 8U, dd + DIR_BASERELOC * 8U)) {
        return false;
      }
      dir_rva = rd32(data + dd + DIR_BASERELOC * 8U);
      dir_size = rd32(data + dd + DIR_BASERELOC * 8U + 4U);
    } else {
      if (opt_magic != NT_OPTIONAL_32_MAGIC) {
        RP_FAIL(RPE_FORMAT, false, opt_header_offset,
                "I386 image must carry a PE32 optional header");
      }
      // The data directory slot we read ends at optional offset 104.
      if (opt_size < 104U) {
        RP_FAIL(RPE_FORMAT, false, opt_header_offset,
                "PE32 optional header is too small for its data "
                "directories");
      }
      image_base_offset = opt_header_offset + 28U;
      // ImageBase is a 32-bit field at offset 28 of the PE32 optional header.
      old_base = rd32(data + image_base_offset);
      size_of_headers = rd32(data + opt_header_offset + 60U);
      // Data directory starts at offset 96 of the PE32 optional header.
      std::uint32_t dd = opt_header_offset + 96U;
      std::uint32_t num_dirs = rd32(data + opt_header_offset + 92U);
      if (num_dirs <= DIR_BASERELOC) {
        RP_FAIL(RPE_FORMAT, false, opt_header_offset + 92U,
                "optional header has no base relocation directory slot");
      }
      if (!need(dd + DIR_BASERELOC * 8U, 8U, dd + DIR_BASERELOC * 8U)) {
        return false;
      }
      dir_rva = rd32(data + dd + DIR_BASERELOC * 8U);
      dir_size = rd32(data + dd + DIR_BASERELOC * 8U + 4U);
    }

    if (!validate_bases()) {
      return false;
    }
    return true;
  }

  bool validate_bases() {
    if (!is_pe32_plus) {
      if (old_base > 0xFFFFFFFFULL || new_base > 0xFFFFFFFFULL) {
        RP_FAIL(RPE_OVERFLOW, false, image_base_offset,
                "image base must fit in 32 bits for PE32");
      }
    }
    // Mathematical subtraction new_base - old_base, required to fit in
    // int64_t. The unsigned differences below never overflow because both
    // operands are 64-bit values.
    std::int64_t delta;
    if (new_base >= old_base) {
      std::uint64_t d = new_base - old_base;
      if (d > static_cast<std::uint64_t>(
                  9223372036854775807LL)) { // INT64_MAX
        RP_FAIL(RPE_OVERFLOW, false, 0U,
                "base move does not fit in int64_t");
      }
      delta = static_cast<std::int64_t>(d);
    } else {
      std::uint64_t d = old_base - new_base;
      if (d > static_cast<std::uint64_t>(
                  9223372036854775807ULL)) { // magnitude of INT64_MIN
        RP_FAIL(RPE_OVERFLOW, false, 0U,
                "base move does not fit in int64_t");
      }
      // Negation stays in int64_t range because d <= INT64_MAX here.
      delta = -static_cast<std::int64_t>(d);
    }

    if ((characteristics & IMAGE_FILE_RELOCS_STRIPPED) != 0U &&
        delta != 0) {
      RP_FAIL(RPE_UNSUPPORTED, false, 0U,
              "IMAGE_FILE_RELOCS_STRIPPED is set but a non-zero move was "
              "requested");
    }

    report.is_pe32_plus = is_pe32_plus;
    report.machine = machine;
    report.old_image_base = old_base;
    report.new_image_base = new_base;
    report.delta = delta;
    return true;
  }

  bool build_sections() {
    // The section table immediately follows the optional header.
    std::uint32_t table = opt_header_offset + opt_size;
    if (!need(table, std::size_t(section_count) * 40U, table)) {
      return false;
    }
    for (std::uint16_t i = 0; i < section_count; ++i) {
      std::uint32_t s = table + static_cast<std::uint32_t>(i) * 40U;
      section_model m;
      m.index = static_cast<std::size_t>(i);
      m.va = rd32(data + s + 12U);
      std::uint32_t virtual_size = rd32(data + s + 8U);
      m.raw_size = rd32(data + s + 16U);
      m.raw_ptr = rd32(data + s + 20U);

      std::uint32_t extent = virtual_size != 0U ? virtual_size : m.raw_size;
      if (extent == 0U) {
        RP_FAIL(RPE_FORMAT, false, s + 8U,
                "section has neither virtual nor raw size");
      }
      std::uint32_t va_end;
      if (!add32(m.va, extent, va_end)) {
        RP_FAIL(RPE_OVERFLOW, true, m.va, "section RVA range wraps");
      }
      m.mem_extent = extent;

      if (m.raw_size != 0U) {
        if (m.raw_ptr < size_of_headers) {
          RP_FAIL(RPE_FORMAT, false, s + 20U,
                  "raw section data overlaps the headers");
        }
        std::uint64_t raw_end =
            static_cast<std::uint64_t>(m.raw_ptr) + m.raw_size;
        if (raw_end > size) {
          RP_FAIL(RPE_TRUNCATION, false, s + 16U,
                  "raw section data extends past the end of the file");
        }
        m.backed_len = m.raw_size < extent ? m.raw_size : extent;
      } else {
        m.backed_len = 0U;
      }
      sections.push_back(m);
    }

    // Every RVA byte and every raw file byte must have a unique owner.
    for (std::size_t i = 0; i < sections.size(); ++i) {
      for (std::size_t j = i + 1; j < sections.size(); ++j) {
        const section_model &a = sections[i];
        const section_model &b = sections[j];
        std::uint32_t a_end = a.va + a.mem_extent;
        std::uint32_t b_end = b.va + b.mem_extent;
        if (a.va < b_end && b.va < a_end) {
          RP_FAIL(RPE_LAYOUT_CONFLICT, true, b.va,
                  "sections overlap in the RVA space");
        }
        if (a.raw_size != 0U && b.raw_size != 0U) {
          std::uint64_t ar0 = a.raw_ptr;
          std::uint64_t ar1 = ar0 + a.raw_size;
          std::uint64_t br0 = b.raw_ptr;
          std::uint64_t br1 = br0 + b.raw_size;
          if (ar0 < br1 && br0 < ar1) {
            RP_FAIL(RPE_LAYOUT_CONFLICT, false, b.raw_ptr,
                    "sections overlap in the raw file space");
          }
        }
      }
    }
    return true;
  }

  // Locate every raw byte backing [rva, rva+len), producing one span per
  // contiguous run inside a single section. Continuous RVAs are allowed to
  // cross section boundaries; gaps, virtual tails and header targets are
  // rejected.
  bool map_rva(std::uint32_t rva,
               std::uint32_t len,
               std::vector<relocation_preview_span> &out_spans) {
    if (len == 0U) {
      RP_FAIL(RPE_FORMAT, true, rva, "zero-length RVA mapping requested");
    }
    std::uint64_t end64 = static_cast<std::uint64_t>(rva) + len;
    if (end64 > 0xFFFFFFFFULL) {
      RP_FAIL(RPE_OVERFLOW, true, rva, "RVA range wraps the 32-bit space");
    }
    for (std::uint32_t k = 0; k < len; ++k) {
      std::uint32_t cur = rva + k;
      const section_model *owner = nullptr;
      for (std::size_t i = 0; i < sections.size(); ++i) {
        const section_model &m = sections[i];
        if (cur >= m.va && cur < m.va + m.mem_extent) {
          owner = &m;
          break;
        }
      }
      if (owner == nullptr) {
        if (cur < size_of_headers) {
          RP_FAIL(RPE_UNSUPPORTED, true, cur,
                  "targets inside the headers are not supported");
        }
        RP_FAIL(RPE_ADDRESS, true, cur,
                "RVA is not backed by any section (gap)");
      }
      std::uint32_t in_sec = cur - owner->va;
      if (in_sec >= owner->backed_len) {
        RP_FAIL(RPE_ADDRESS, true, cur,
                "RVA falls in the uninitialized virtual tail of a section");
      }
      std::uint32_t file_off = owner->raw_ptr + in_sec;

      if (!out_spans.empty() &&
          out_spans.back().section_index == owner->index &&
          out_spans.back().file_offset + out_spans.back().size == file_off) {
        ++out_spans.back().size;
      } else {
        relocation_preview_span sp;
        sp.section_index = owner->index;
        sp.file_offset = file_off;
        sp.size = 1U;
        out_spans.push_back(sp);
      }
    }
    return true;
  }

  bool spans_intersect_directory(
      const std::vector<relocation_preview_span> &target_spans) {
    for (std::size_t i = 0; i < target_spans.size(); ++i) {
      std::uint64_t t0 = target_spans[i].file_offset;
      std::uint64_t t1 = t0 + target_spans[i].size;
      for (std::size_t j = 0; j < dir_spans.size(); ++j) {
        std::uint64_t d0 = dir_spans[j].file_offset;
        std::uint64_t d1 = d0 + dir_spans[j].size;
        if (t0 < d1 && d0 < t1) {
          return true;
        }
      }
    }
    return false;
  }

  bool prepare_directory() {
    directory_present = !(dir_rva == 0U && dir_size == 0U);
    if (!directory_present) {
      if (dir_rva != 0U || dir_size != 0U) {
        RP_FAIL(RPE_FORMAT, true, 0U,
                "partly zeroed base relocation directory descriptor");
      }
      // No directory: with no relocations there is nothing to preview. We
      // never guess which values look like pointers.
      report.directory_rva = 0U;
      report.directory_size = 0U;
      report.directory_file_offset = 0U;
      return true;
    }
    if (dir_size < 8U) {
      RP_FAIL(RPE_FORMAT, true, dir_rva,
              "base relocation directory is smaller than one block");
    }
    if (!map_rva(dir_rva, dir_size, dir_spans)) {
      return false;
    }
    report.directory_rva = dir_rva;
    report.directory_size = dir_size;
    report.directory_file_offset = dir_spans.front().file_offset;
    return true;
  }

  std::uint8_t read_current(std::uint32_t file_off) {
    std::unordered_map<std::uint32_t, preview_cell>::const_iterator it =
        cells.find(file_off);
    if (it != cells.end()) {
      return it->second.preview;
    }
    return data[file_off];
  }

  bool read_field(const std::vector<relocation_preview_span> &spans,
                  relocation_preview_width width,
                  std::uint8_t out[8]) {
    std::uint32_t total = 0U;
    for (std::size_t i = 0; i < spans.size(); ++i) {
      total += spans[i].size;
    }
    if (total != static_cast<std::uint32_t>(width)) {
      RP_FAIL(RPE_ADDRESS, false, spans.front().file_offset,
              "field spans do not cover the full field width");
    }
    std::uint32_t k = 0U;
    for (std::size_t i = 0; i < spans.size(); ++i) {
      for (std::uint32_t j = 0; j < spans[i].size; ++j) {
        out[k++] = read_current(spans[i].file_offset + j);
      }
    }
    return true;
  }

  bool write_field(const std::vector<relocation_preview_span> &spans,
                   const std::uint8_t bytes[8],
                   std::size_t writer) {
    std::uint32_t k = 0U;
    for (std::size_t i = 0; i < spans.size(); ++i) {
      for (std::uint32_t j = 0; j < spans[i].size; ++j) {
        std::uint32_t off = spans[i].file_offset + j;
        std::unordered_map<std::uint32_t, preview_cell>::iterator it =
            cells.find(off);
        if (it == cells.end()) {
          // First writer: the original byte stays frozen in the cell.
          preview_cell cell;
          cell.original = data[off];
          cell.preview = bytes[k];
          cell.writer_ops.push_back(writer);
          cells.insert(std::make_pair(off, cell));
        } else {
          // Later writer: read-after-write chaining. Every physical byte is
          // reached through exactly one RVA (verified by the section checks),
          // so distinct RVAs can never alias one another here.
          it->second.preview = bytes[k];
          it->second.writer_ops.push_back(writer);
        }
        ++k;
      }
    }
    return true;
  }

  static void load_value(const std::uint8_t bytes[8],
                         relocation_preview_width width,
                         relocation_preview_value &v) {
    v.width = width;
    for (std::size_t i = 0; i < 8U; ++i) {
      v.bytes[i] = 0U;
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(width); ++i) {
      v.bytes[i] = bytes[i];
    }
  }

  std::uint16_t u16_from(const std::uint8_t b[8]) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(b[0]) |
        (static_cast<std::uint16_t>(b[1]) << 8));
  }

  std::uint32_t u32_from(const std::uint8_t b[8]) {
    return static_cast<std::uint32_t>(b[0]) |
           (static_cast<std::uint32_t>(b[1]) << 8) |
           (static_cast<std::uint32_t>(b[2]) << 16) |
           (static_cast<std::uint32_t>(b[3]) << 24);
  }

  std::uint64_t u64_from(const std::uint8_t b[8]) {
    std::uint64_t v = 0U;
    for (std::size_t i = 0; i < 8U; ++i) {
      v |= static_cast<std::uint64_t>(b[i]) << (8U * i);
    }
    return v;
  }

  static void put16(std::uint8_t b[8], std::uint16_t v) {
    b[0] = static_cast<std::uint8_t>(v);
    b[1] = static_cast<std::uint8_t>(v >> 8);
  }

  static void put32(std::uint8_t b[8], std::uint32_t v) {
    b[0] = static_cast<std::uint8_t>(v);
    b[1] = static_cast<std::uint8_t>(v >> 8);
    b[2] = static_cast<std::uint8_t>(v >> 16);
    b[3] = static_cast<std::uint8_t>(v >> 24);
  }

  static void put64(std::uint8_t b[8], std::uint64_t v) {
    for (std::size_t i = 0; i < 8U; ++i) {
      b[i] = static_cast<std::uint8_t>(v >> (8U * i));
    }
  }

  // Arithmetic right shift of a signed 64-bit value by 16, implemented as
  // floor division by 2^16 so the result does not depend on the platform's
  // treatment of negative right shifts.
  static std::int64_t sar16(std::int64_t v) {
    const std::int64_t divisor = 65536;
    if (v >= 0) {
      return v / divisor;
    }
    std::int64_t mag = -v; // safe: callers keep v above INT64_MIN
    return -((mag + (divisor - 1)) / divisor);
  }

  bool type_supported(std::uint8_t type) {
    if (type == RPK_ABSOLUTE) {
      return true;
    }
    if (!is_pe32_plus) {
      return type == RPK_HIGH || type == RPK_LOW ||
             type == RPK_HIGHLOW || type == RPK_HIGHADJ;
    }
    return type == RPK_DIR64;
  }

  bool parse_blocks() {
    op_serial = 0U;
    std::uint32_t consumed = 0U; // bytes consumed inside the directory
    while (consumed < dir_size) {
      std::uint32_t block_rva = dir_rva + consumed;
      if ((block_rva & 0x3U) != 0U) {
        RP_FAIL(RPE_FORMAT, true, block_rva,
                "relocation block is not 4-byte aligned");
      }
      std::uint32_t remaining = dir_size - consumed;
      if (remaining < 8U) {
        RP_FAIL(RPE_TRUNCATION, true, block_rva,
                "trailing bytes in relocation directory cannot hold a block "
                "header");
      }
      std::vector<relocation_preview_span> head_spans;
      if (!map_rva(block_rva, 8U, head_spans)) {
        return false;
      }
      std::uint8_t hb[8];
      if (!read_field(head_spans, RPW_64, hb)) {
        return false;
      }
      std::uint32_t page_rva = u32_from(hb);
      std::uint32_t block_size = u32_from(hb + 4);
      if (block_size < 8U || (block_size & 0x3U) != 0U) {
        RP_FAIL(RPE_FORMAT, true, block_rva,
                "relocation block size must be a multiple of four and at "
                "least eight");
      }
      if (block_size > remaining) {
        RP_FAIL(RPE_TRUNCATION, true, block_rva,
                "relocation block runs past the directory size");
      }
      std::uint32_t entry_bytes = block_size - 8U;
      std::uint32_t entry_count = entry_bytes / 2U;

      relocation_preview_block block;
      block.header_file_offset = head_spans.front().file_offset;
      block.page_rva = page_rva;
      block.block_size = block_size;

      std::uint32_t idx = 0U;
      while (idx < entry_count) {
        std::uint32_t entry_rva = block_rva + 8U + idx * 2U;
        std::vector<relocation_preview_span> entry_spans;
        if (!map_rva(entry_rva, 2U, entry_spans)) {
          return false;
        }
        std::uint8_t eb[2];
        if (!read_field(entry_spans, RPW_16, eb)) {
          return false;
        }
        std::uint16_t raw = u16_from(eb);
        std::uint8_t type = static_cast<std::uint8_t>(raw >> 12);
        std::uint16_t page_off = static_cast<std::uint16_t>(raw & 0x0FFFU);

        relocation_preview_op op;
        op.entry_file_offset = entry_spans.front().file_offset;
        op.raw_entry = raw;
        op.raw_type = type;
        op.page_offset = page_off;
        op.has_pair = false;
        op.pair_file_offset = 0U;
        op.pair_raw = 0U;
        op.pair_signed = 0;

        if (!type_supported(type)) {
          // Distinguish an unknown type code from one that is known but not
          // valid on this machine.
          if (type == RPK_HIGH || type == RPK_LOW ||
              type == RPK_HIGHLOW || type == RPK_HIGHADJ ||
              type == RPK_DIR64) {
            RP_FAIL(RPE_MACHINE_MISMATCH, false, op.entry_file_offset,
                    "relocation type is not valid for this machine");
          }
          RP_FAIL(RPE_UNKNOWN_TYPE, false, op.entry_file_offset,
                  "unknown base relocation type");
        }
        op.kind = static_cast<relocation_preview_kind>(type);

        if (type == RPK_ABSOLUTE) {
          // Sourced padding: recorded, but its target is never read.
          std::uint8_t zero[8] = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
          op.field_width = RPW_NONE;
          op.target_rva = 0U;
          load_value(zero, RPW_NONE, op.before);
          load_value(zero, RPW_NONE, op.after);
          block.ops.push_back(op);
          ++op_serial;
          ++idx;
          continue;
        }

        std::uint32_t target_rva;
        if (!add32(page_rva, page_off, target_rva)) {
          RP_FAIL(RPE_OVERFLOW, true, page_rva,
                  "relocation target RVA wraps");
        }
        op.target_rva = target_rva;

        // HIGHADJ consumes the very next 16-bit word of the same block as
        // its signed low half-word. That word is not a relocation entry.
        if (type == RPK_HIGHADJ) {
          if (idx + 1U >= entry_count) {
            RP_FAIL(RPE_TRUNCATION, false, op.entry_file_offset,
                    "HIGHADJ relocation is missing its paired low word");
          }
          std::uint32_t pair_rva = entry_rva + 2U;
          std::vector<relocation_preview_span> pair_spans;
          if (!map_rva(pair_rva, 2U, pair_spans)) {
            return false;
          }
          std::uint8_t pb[2];
          if (!read_field(pair_spans, RPW_16, pb)) {
            return false;
          }
          std::uint16_t pair_raw = u16_from(pb);
          op.has_pair = true;
          op.pair_file_offset = pair_spans.front().file_offset;
          op.pair_raw = pair_raw;
          // Sign-extend the low half-word without relying on
          // implementation-defined narrowing.
          std::int32_t pair_s32 = static_cast<std::int32_t>(pair_raw);
          if (pair_s32 >= 0x8000) {
            pair_s32 -= 0x10000;
          }
          op.pair_signed = static_cast<std::int16_t>(pair_s32);
        }

        relocation_preview_width width = RPW_16;
        if (type == RPK_HIGHLOW) {
          width = RPW_32;
        } else if (type == RPK_DIR64) {
          width = RPW_64;
        }
        op.field_width = width;
        if (!map_rva(target_rva, static_cast<std::uint32_t>(width),
                     op.target_spans)) {
          return false;
        }
        if (spans_intersect_directory(op.target_spans)) {
          RP_FAIL(RPE_LAYOUT_CONFLICT, false,
                  op.target_spans.front().file_offset,
                  "relocation target writes into the relocation directory");
        }

        std::uint8_t before[8];
        if (!read_field(op.target_spans, width, before)) {
          return false;
        }
        load_value(before, width, op.before);

        std::uint8_t after[8];
        std::memcpy(after, before, 8);
        // Signed-to-unsigned conversion is defined modulo 2^N, so this also
        // recovers the two's complement encoding of a negative delta.
        std::uint64_t delta_u = static_cast<std::uint64_t>(report.delta);
        std::uint32_t delta32 = static_cast<std::uint32_t>(delta_u);

        if (type == RPK_HIGH || type == RPK_LOW) {
          std::uint32_t addend =
              type == RPK_HIGH ? (delta32 >> 16) : (delta32 & 0xFFFFU);
          std::uint32_t cur = u16_from(before);
          std::uint16_t nv =
              static_cast<std::uint16_t>((cur + addend) & 0xFFFFU);
          put16(after, nv);
        } else if (type == RPK_HIGHLOW) {
          std::uint32_t cur = u32_from(before);
          put32(after, cur + delta32); // unsigned wrap is well defined
        } else if (type == RPK_DIR64) {
          std::uint64_t cur = u64_from(before);
          put64(after, cur + delta_u); // unsigned wrap is well defined
        } else { // RPK_HIGHADJ
          std::int64_t h = static_cast<std::int64_t>(u16_from(before));
          std::int64_t l = static_cast<std::int64_t>(op.pair_signed);
          // Reduce delta to its unique representative in [-2^31, 2^31).
          // The HIGHADJ expression only depends on delta modulo 2^32, and
          // the reduced form keeps every signed addition inside int64_t.
          std::uint32_t delta_mod32 =
              static_cast<std::uint32_t>(static_cast<std::uint64_t>(
                  report.delta));
          std::int64_t delta_reduced;
          if (delta_mod32 >= 0x80000000U) {
            delta_reduced =
                static_cast<std::int64_t>(delta_mod32 - 0x80000000U) -
                static_cast<std::int64_t>(0x80000000U);
          } else {
            delta_reduced = static_cast<std::int64_t>(delta_mod32);
          }
          std::int64_t sum = (h << 16) + l + delta_reduced + 0x8000;
          std::int64_t shifted = sar16(sum);
          std::uint16_t nv =
              static_cast<std::uint16_t>(static_cast<std::uint64_t>(shifted));
          put16(after, nv);
        }

        load_value(after, width, op.after);
        if (!write_field(op.target_spans, after, op_serial)) {
          return false;
        }

        block.ops.push_back(op);
        ++op_serial;
        idx += (type == RPK_HIGHADJ) ? 2U : 1U;
      }

      report.blocks.push_back(block);
      consumed += block_size;
    }
    return true;
  }

  bool build_intervals() {
    struct item {
      std::uint32_t rva;
      std::uint8_t original;
      std::uint8_t preview;
      std::vector<std::size_t> writers;
    };
    std::vector<item> items;
    items.reserve(cells.size());

    std::unordered_map<std::uint32_t, preview_cell>::const_iterator it;
    for (it = cells.begin(); it != cells.end(); ++it) {
      std::uint32_t off = it->first;
      const preview_cell &cell = it->second;
      std::uint32_t rva = 0U;
      bool found = false;
      for (std::size_t i = 0; i < sections.size(); ++i) {
        const section_model &m = sections[i];
        if (m.raw_size != 0U && off >= m.raw_ptr &&
            off < m.raw_ptr + m.raw_size) {
          rva = m.va + (off - m.raw_ptr);
          found = true;
          break;
        }
      }
      if (!found) {
        RP_FAIL(RPE_ADDRESS, false, off,
                "previewed byte cannot be mapped back to an RVA");
      }
      item x;
      x.rva = rva;
      x.original = cell.original;
      x.preview = cell.preview;
      x.writers = cell.writer_ops;
      items.push_back(x);
    }

    std::sort(items.begin(), items.end(),
              [](const item &a, const item &b) { return a.rva < b.rva; });

    for (std::size_t i = 0; i < items.size();) {
      relocation_preview_interval interval;
      interval.rva = items[i].rva;
      std::size_t j = i;
      while (j < items.size() &&
             items[j].rva == interval.rva +
                                static_cast<std::uint32_t>(
                                    interval.original_bytes.size())) {
        interval.original_bytes.push_back(items[j].original);
        interval.preview_bytes.push_back(items[j].preview);
        for (std::size_t k = 0; k < items[j].writers.size(); ++k) {
          interval.op_indices.push_back(items[j].writers[k]);
        }
        ++j;
      }
      report.intervals.push_back(interval);
      i = j;
    }
    return true;
  }

#undef RP_FAIL
};

} // namespace

std::size_t base_relocation_preview::total_op_count() const {
  std::size_t n = 0;
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    n += blocks[i].ops.size();
  }
  return n;
}

bool BuildBaseRelocationPreview(const std::uint8_t *data,
                                std::size_t size,
                                std::uint64_t new_image_base,
                                base_relocation_preview &report_out,
                                relocation_preview_error &error_out) {
  error_out.category = RPE_NONE;
  error_out.location_is_rva = false;
  error_out.file_offset = 0U;
  error_out.rva = 0U;
  error_out.what = "";
  report_out = base_relocation_preview();

  preview_builder builder(data, size, new_image_base, report_out, error_out);
  if (builder.run()) {
    return true;
  }
  // Never publish a half-built report.
  report_out = base_relocation_preview();
  return false;
}

} // namespace peparse
