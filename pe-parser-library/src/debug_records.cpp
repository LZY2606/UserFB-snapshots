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

#include <pe-parse/debug_records.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "parse_internal.h"

namespace peparse {

namespace {

constexpr std::uint32_t kDebugEntrySize = 28;
constexpr std::uint32_t kNoFileOffset = 0xffffffffU;
constexpr std::uint32_t kDebugTypeCodeView = 2;

// Little-endian scalar loads from the file buffer. All offsets and
// lengths have already been bounds-checked by the caller, so these
// never touch the global parser error state.
std::uint16_t loadWord(const std::uint8_t *p) {
  std::uint16_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

std::uint32_t loadDword(const std::uint8_t *p) {
  std::uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

std::string hexU64(std::uint64_t v) {
  std::ostringstream oss;
  oss << "0x" << std::hex << v;
  return oss.str();
}

void failAt(debug_records &out,
            std::uint32_t entryFileOffset,
            const std::string &message) {
  out.has_error = true;
  out.error_entry_file_offset = entryFileOffset;
  out.error_message = message;
}

// A section claims an RVA when it lies in the span the loader would
// map for it; the file-backed part of that span is SizeOfRawData.
bool sectionClaimsRva(const section &sec, std::uint32_t rva) {
  std::uint64_t begin = sec.sec.VirtualAddress;
  std::uint64_t span = sec.sec.Misc.VirtualSize;
  if (sec.sec.SizeOfRawData > span) {
    span = sec.sec.SizeOfRawData;
  }
  std::uint64_t end = begin + span;
  return rva >= begin && static_cast<std::uint64_t>(rva) < end;
}

// Map an RVA range of length len to a file offset. On success returns
// true and fills fileOffset and sectionIndex.
bool mapRvaRange(const std::vector<section> &secs,
                 std::uint64_t fileSize,
                 std::uint32_t rva,
                 std::uint32_t len,
                 std::uint32_t &fileOffset,
                 std::uint32_t &sectionIndex,
                 std::string &why) {
  const section *match = nullptr;
  bool ambiguous = false;
  for (const section &sec : secs) {
    if (!sectionClaimsRva(sec, rva)) {
      continue;
    }
    if (match != nullptr) {
      ambiguous = true;
      break;
    }
    match = &sec;
  }

  if (ambiguous) {
    why = "RVA " + hexU64(rva) + " is claimed by more than one section";
    return false;
  }
  if (match == nullptr) {
    why = "RVA " + hexU64(rva) + " is not covered by any section";
    return false;
  }

  std::uint64_t delta = static_cast<std::uint64_t>(rva) - match->sec.VirtualAddress;
  if (delta + len > match->sec.SizeOfRawData) {
    why = "range at RVA " + hexU64(rva) + " of length " + hexU64(len) +
          " crosses the raw data boundary of section " +
          std::to_string(match->sectionIndex);
    return false;
  }

  std::uint64_t off =
      static_cast<std::uint64_t>(match->sec.PointerToRawData) + delta;
  if (off + len > fileSize) {
    why = "range at RVA " + hexU64(rva) + " maps past the end of the file";
    return false;
  }

  fileOffset = static_cast<std::uint32_t>(off);
  sectionIndex = match->sectionIndex;
  return true;
}

// Classify a file range exactly one owner. Returns false when the
// range is not fully owned by a single region.
bool classifyFileRange(const std::vector<section> &secs,
                       std::uint64_t fileSize,
                       std::uint64_t sizeOfHeaders,
                       std::uint64_t overlayStart,
                       std::uint32_t off,
                       std::uint32_t len,
                       debug_owner_kind &owner,
                       std::uint32_t &ownerSection,
                       std::string &why) {
  std::uint64_t begin = off;
  std::uint64_t end = begin + len; // both operands are 32-bit: no overflow

  if (end > fileSize) {
    why = "range at file offset " + hexU64(begin) + " of length " +
          hexU64(len) + " extends past the end of the file";
    return false;
  }

  const section *inside = nullptr;
  bool ambiguous = false;
  for (const section &sec : secs) {
    if (sec.sec.SizeOfRawData == 0) {
      continue;
    }
    std::uint64_t secBegin = sec.sec.PointerToRawData;
    std::uint64_t secEnd = secBegin + sec.sec.SizeOfRawData;
    if (begin >= secBegin && end <= secEnd) {
      if (inside != nullptr) {
        ambiguous = true;
        break;
      }
      inside = &sec;
    } else if (begin < secEnd && end > secBegin) {
      why = "range at file offset " + hexU64(begin) + " of length " +
            hexU64(len) +
            " crosses the raw data boundary of section " +
            std::to_string(sec.sectionIndex);
      return false;
    }
  }

  if (ambiguous) {
    why = "file offset " + hexU64(begin) +
          " is owned by more than one section";
    return false;
  }
  if (inside != nullptr) {
    owner = debug_owner_kind::section;
    ownerSection = inside->sectionIndex;
    return true;
  }

  if (end <= sizeOfHeaders) {
    owner = debug_owner_kind::headers;
    return true;
  }
  if (begin >= overlayStart) {
    owner = debug_owner_kind::overlay;
    return true;
  }
  if (begin >= sizeOfHeaders && end <= overlayStart) {
    owner = debug_owner_kind::gap;
    return true;
  }

  why = "range at file offset " + hexU64(begin) + " of length " +
        hexU64(len) + " spans multiple file regions";
  return false;
}

bool readBytes(const bounded_buffer *file,
               std::uint32_t off,
               std::uint32_t len,
               std::vector<std::uint8_t> &out) {
  if (static_cast<std::uint64_t>(off) + len > file->bufLen) {
    return false;
  }
  out.assign(file->buf + off, file->buf + off + len);
  return true;
}

// Interpret one candidate's payload bytes. Recognized CodeView
// formats that are truncated or lack the NUL terminator are errors;
// everything else stays opaque.
bool interpretPayload(const debug_record &rec,
                      debug_payload_candidate &cand,
                      std::string &why) {
  codeview_record &cv = cand.codeview;
  const std::vector<std::uint8_t> &b = cand.bytes;

  if (rec.type != kDebugTypeCodeView || b.size() < 4) {
    cv.format = codeview_format::opaque;
    return true;
  }

  std::uint32_t sig = loadDword(b.data());
  if (sig == 0x53445352U) { // "RSDS"
    if (b.size() < 24) {
      why = "RSDS payload of " + std::to_string(b.size()) +
            " bytes is truncated below the 24-byte header";
      return false;
    }
    cv.format = codeview_format::rsds;
    std::copy_n(b.data() + 4, 16, cv.guid.begin());
    cv.age = loadDword(b.data() + 20);

    char text[37];
    std::snprintf(text,
                  sizeof(text),
                  "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  loadDword(b.data() + 4),
                  loadWord(b.data() + 8),
                  loadWord(b.data() + 10),
                  b[12],
                  b[13],
                  b[14],
                  b[15],
                  b[16],
                  b[17],
                  b[18],
                  b[19]);
    cv.guid_text = text;

    auto nul = std::find(b.begin() + 24, b.end(), 0);
    if (nul == b.end()) {
      why = "RSDS payload has no NUL-terminated path";
      return false;
    }
    cv.path.assign(b.begin() + 24, nul);
    return true;
  }

  if (sig == 0x3031424eU) { // "NB10"
    if (b.size() < 16) {
      why = "NB10 payload of " + std::to_string(b.size()) +
            " bytes is truncated below the 16-byte header";
      return false;
    }
    cv.format = codeview_format::nb10;
    cv.offset = loadDword(b.data() + 4);
    cv.signature = loadDword(b.data() + 8);
    cv.age = loadDword(b.data() + 12);

    auto nul = std::find(b.begin() + 16, b.end(), 0);
    if (nul == b.end()) {
      why = "NB10 payload has no NUL-terminated path";
      return false;
    }
    cv.path.assign(b.begin() + 16, nul);
    return true;
  }

  cv.format = codeview_format::opaque;
  return true;
}

} // anonymous namespace

bool ComputeDebugRecords(parsed_pe *pe, debug_records &out) {
  out = debug_records{};

  if (pe == nullptr || pe->fileBuffer == nullptr) {
    failAt(out, kNoFileOffset, "no parsed PE or file buffer");
    return false;
  }

  const nt_header_32 &nt = pe->peHeader.nt;
  data_directory debugDir{};
  std::uint64_t sizeOfHeaders = 0;
  if (nt.OptionalMagic == NT_OPTIONAL_32_MAGIC) {
    debugDir = nt.OptionalHeader.DataDirectory[DIR_DEBUG];
    sizeOfHeaders = nt.OptionalHeader.SizeOfHeaders;
  } else if (nt.OptionalMagic == NT_OPTIONAL_64_MAGIC) {
    debugDir = nt.OptionalHeader64.DataDirectory[DIR_DEBUG];
    sizeOfHeaders = nt.OptionalHeader64.SizeOfHeaders;
  } else {
    failAt(out, kNoFileOffset, "unknown optional header magic");
    return false;
  }

  if (debugDir.Size == 0) {
    return true; // no debug directory at all
  }
  if (debugDir.VirtualAddress == 0) {
    failAt(out,
           kNoFileOffset,
           "debug directory has a non-zero size but no RVA");
    return false;
  }
  if (debugDir.Size % kDebugEntrySize != 0) {
    failAt(out,
           kNoFileOffset,
           "debug directory size " + hexU64(debugDir.Size) +
             " is not a multiple of the 28-byte entry size");
    return false;
  }

  const std::vector<section> &secs = pe->internal->secs;
  const std::uint64_t fileSize = pe->fileBuffer->bufLen;

  // The overlay begins past the headers and the furthest non-empty
  // section raw range; holes between sections are gaps, not overlay.
  std::uint64_t overlayStart = sizeOfHeaders;
  for (const section &sec : secs) {
    if (sec.sec.SizeOfRawData == 0) {
      continue;
    }
    std::uint64_t secEnd =
        static_cast<std::uint64_t>(sec.sec.PointerToRawData) +
        sec.sec.SizeOfRawData;
    if (secEnd > overlayStart) {
      overlayStart = secEnd;
    }
  }

  // Locate the directory itself through the section table.
  std::uint32_t dirFileOffset = 0;
  std::uint32_t dirSectionIndex = 0;
  std::string why;
  if (!mapRvaRange(secs,
                   fileSize,
                   debugDir.VirtualAddress,
                   debugDir.Size,
                   dirFileOffset,
                   dirSectionIndex,
                   why)) {
    failAt(out, kNoFileOffset, "debug directory: " + why);
    return false;
  }

  const std::uint32_t count = debugDir.Size / kDebugEntrySize;
  const std::uint8_t *base = pe->fileBuffer->buf;

  for (std::uint32_t i = 0; i < count; i++) {
    std::uint64_t entryOff64 =
        static_cast<std::uint64_t>(dirFileOffset) +
        static_cast<std::uint64_t>(i) * kDebugEntrySize;
    // dirFileOffset + debugDir.Size was bounds-checked above
    std::uint32_t entryOff = static_cast<std::uint32_t>(entryOff64);
    const std::uint8_t *raw = base + entryOff;

    debug_record rec;
    rec.ordinal = i;
    rec.entry_file_offset = entryOff;
    rec.directory_section_index = dirSectionIndex;
    std::copy_n(raw, kDebugEntrySize, rec.raw_entry.begin());

    rec.characteristics = loadDword(raw + 0);
    rec.timestamp = loadDword(raw + 4);
    rec.major_version = loadWord(raw + 8);
    rec.minor_version = loadWord(raw + 10);
    rec.type = loadDword(raw + 12);
    rec.size_of_data = loadDword(raw + 16);
    rec.address_of_raw_data = loadDword(raw + 20);
    rec.pointer_to_raw_data = loadDword(raw + 24);

    if (rec.size_of_data == 0) {
      // Zero-length marker: keep the record, dereference nothing.
      rec.relation = debug_relation::none;
      out.records.push_back(std::move(rec));
      continue;
    }

    bool hasFile = rec.pointer_to_raw_data != 0;
    bool hasRva = rec.address_of_raw_data != 0;

    if (!hasFile && !hasRva) {
      failAt(out,
             entryOff,
             "entry " + std::to_string(i) + " at file offset " +
               hexU64(entryOff) + " has a non-empty payload but neither "
               "PointerToRawData nor AddressOfRawData");
      return false;
    }

    if (hasFile) {
      debug_payload_candidate cand;
      cand.source = debug_candidate_source::file_pointer;
      cand.file_offset = rec.pointer_to_raw_data;
      if (!classifyFileRange(secs,
                             fileSize,
                             sizeOfHeaders,
                             overlayStart,
                             cand.file_offset,
                             rec.size_of_data,
                             cand.owner,
                             cand.owner_section,
                             why)) {
        failAt(out,
               entryOff,
               "entry " + std::to_string(i) + " at file offset " +
                 hexU64(entryOff) + ": PointerToRawData: " + why);
        return false;
      }
      if (!readBytes(pe->fileBuffer,
                     cand.file_offset,
                     rec.size_of_data,
                     cand.bytes)) {
        failAt(out,
               entryOff,
               "entry " + std::to_string(i) + " at file offset " +
                 hexU64(entryOff) + ": PointerToRawData payload is unreadable");
        return false;
      }
      rec.candidates.push_back(std::move(cand));
    }

    if (hasRva) {
      debug_payload_candidate cand;
      cand.source = debug_candidate_source::rva;
      std::uint32_t sectionIndex = 0;
      if (!mapRvaRange(secs,
                       fileSize,
                       rec.address_of_raw_data,
                       rec.size_of_data,
                       cand.file_offset,
                       sectionIndex,
                       why)) {
        failAt(out,
               entryOff,
               "entry " + std::to_string(i) + " at file offset " +
                 hexU64(entryOff) + ": AddressOfRawData: " + why);
        return false;
      }
      cand.owner = debug_owner_kind::section;
      cand.owner_section = sectionIndex;
      if (!readBytes(pe->fileBuffer,
                     cand.file_offset,
                     rec.size_of_data,
                     cand.bytes)) {
        failAt(out,
               entryOff,
               "entry " + std::to_string(i) + " at file offset " +
                 hexU64(entryOff) + ": AddressOfRawData payload is unreadable");
        return false;
      }
      rec.candidates.push_back(std::move(cand));
    }

    if (hasFile && hasRva) {
      const auto &fileCand = rec.candidates.front();
      const auto &rvaCand = rec.candidates.back();
      if (fileCand.file_offset == rvaCand.file_offset) {
        rec.relation = debug_relation::same;
      } else if (fileCand.bytes == rvaCand.bytes) {
        rec.relation = debug_relation::equal_distinct;
      } else {
        rec.relation = debug_relation::conflict;
      }
    } else if (hasFile) {
      rec.relation = debug_relation::file_only;
    } else {
      rec.relation = debug_relation::rva_only;
    }

    for (auto &cand : rec.candidates) {
      if (!interpretPayload(rec, cand, why)) {
        failAt(out,
               entryOff,
               "entry " + std::to_string(i) + " at file offset " +
                 hexU64(entryOff) + ": " + why);
        return false;
      }
    }

    out.records.push_back(std::move(rec));
  }

  return true;
}

bool ReadDebugRecords(parsed_pe *pe, debug_records &out) {
  if (pe == nullptr || pe->internal == nullptr) {
    out = debug_records{};
    failAt(out, kNoFileOffset, "no parsed PE");
    return false;
  }

  // Deep copy: the result stays valid after DestructParsedPE.
  out = pe->internal->debugRecords;
  return !out.has_error;
}

} // namespace peparse
