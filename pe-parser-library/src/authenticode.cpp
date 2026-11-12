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

#include <pe-parse/authenticode.h>
#include <pe-parse/parse.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <string>

namespace peparse {

namespace {

// ---------------------------------------------------------------------------
// On-disk layout constants. These are the wire sizes/offsets of the PE
// format; the in-memory aggregate structures in nt-headers.h must not be used
// as a substitute, since their sizeof/offsetof are host-layout dependent.
// ---------------------------------------------------------------------------

const std::uint32_t DOS_E_LFANEW_OFFSET = 0x3C;

const std::uint32_t NT_SIGNATURE_SIZE = 4;
const std::uint32_t FILE_HEADER_SIZE = 20;
const std::uint16_t FILE_HEADER_NUMBER_OF_SECTIONS_OFFSET = 2;
const std::uint16_t FILE_HEADER_SIZE_OF_OPTIONAL_HEADER_OFFSET = 16;

const std::uint32_t SECTION_HEADER_FILE_SIZE = 40;
const std::uint32_t SECTION_SIZE_OF_RAW_DATA_OFFSET = 16;
const std::uint32_t SECTION_POINTER_TO_RAW_DATA_OFFSET = 20;

const std::uint16_t OPTIONAL_MAGIC_PE32 = 0x010B;
const std::uint16_t OPTIONAL_MAGIC_PE32_PLUS = 0x020B;

const std::uint32_t OPTIONAL_SIZE_OF_HEADERS_OFFSET = 60;
const std::uint32_t OPTIONAL_CHECKSUM_OFFSET = 64;
const std::uint32_t PE32_NUMBER_OF_RVA_AND_SIZES_OFFSET = 92;
const std::uint32_t PE32_DATA_DIRECTORY_OFFSET = 96;
const std::uint32_t PE32PLUS_NUMBER_OF_RVA_AND_SIZES_OFFSET = 108;
const std::uint32_t PE32PLUS_DATA_DIRECTORY_OFFSET = 112;

const std::uint32_t DATA_DIRECTORY_ENTRY_SIZE = 8;
const std::uint32_t SECURITY_DIRECTORY_INDEX = 4;
const std::uint32_t MAX_NUMBER_OF_RVA_AND_SIZES = 16;

const std::uint32_t WIN_CERTIFICATE_HEADER_SIZE = 8;
const std::uint32_t WIN_CERTIFICATE_LENGTH_OFFSET = 0;
const std::uint32_t WIN_CERTIFICATE_REVISION_OFFSET = 4;
const std::uint32_t WIN_CERTIFICATE_TYPE_OFFSET = 6;

thread_local std::string g_plan_error;

void setPlanError(const char *category,
                  std::uint64_t offset,
                  const std::string &detail) {
  std::ostringstream oss;
  oss << "authenticode plan rejected (" << category << ") at file offset 0x"
      << std::hex << offset << std::dec << ": " << detail;
  g_plan_error = oss.str();
}

// Bounds-checked little-endian readers over a raw byte range.
class RawImage {
public:
  RawImage(const std::uint8_t *data, std::uint64_t size)
      : data_(data), size_(size) {
  }

  bool readU16(std::uint64_t offset, std::uint16_t &out) const {
    if (offset > size_ || size_ - offset < 2) {
      return false;
    }
    out = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(data_[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(
                                       data_[offset + 1])
                                   << 8));
    return true;
  }

  bool readU32(std::uint64_t offset, std::uint32_t &out) const {
    if (offset > size_ || size_ - offset < 4) {
      return false;
    }
    out = static_cast<std::uint32_t>(data_[offset]) |
          (static_cast<std::uint32_t>(data_[offset + 1]) << 8) |
          (static_cast<std::uint32_t>(data_[offset + 2]) << 16) |
          (static_cast<std::uint32_t>(data_[offset + 3]) << 24);
    return true;
  }

  std::uint64_t size() const {
    return size_;
  }

private:
  const std::uint8_t *data_;
  std::uint64_t size_;
};

std::uint64_t alignUp8(std::uint64_t value, bool *overflow) {
  *overflow = value > (0xFFFFFFFFFFFFFFFFULL - 7ULL);
  if (*overflow) {
    return 0;
  }
  return (value + 7ULL) & ~static_cast<std::uint64_t>(7);
}

struct OptionalHeaderLayout {
  std::uint32_t numberOfRvaAndSizesOffset;
  std::uint32_t dataDirectoryOffset;
};

OptionalHeaderLayout layoutForMagic(std::uint16_t magic) {
  OptionalHeaderLayout layout;
  if (magic == OPTIONAL_MAGIC_PE32) {
    layout.numberOfRvaAndSizesOffset = PE32_NUMBER_OF_RVA_AND_SIZES_OFFSET;
    layout.dataDirectoryOffset = PE32_DATA_DIRECTORY_OFFSET;
  } else {
    layout.numberOfRvaAndSizesOffset = PE32PLUS_NUMBER_OF_RVA_AND_SIZES_OFFSET;
    layout.dataDirectoryOffset = PE32PLUS_DATA_DIRECTORY_OFFSET;
  }
  return layout;
}

} // namespace

std::string GetAuthenticodePlanError() {
  return g_plan_error;
}

authenticode_plan_status
BuildAuthenticodePlan(parsed_pe *pe, authenticode_plan &plan_out) {
  plan_out = authenticode_plan();
  g_plan_error.clear();

  if (pe == nullptr || pe->fileBuffer == nullptr ||
      pe->fileBuffer->buf == nullptr) {
    setPlanError("argument", 0,
                 "null parsed_pe or file buffer");
    return AUTH_PLAN_ERR_ARGUMENT;
  }

  RawImage image(pe->fileBuffer->buf,
                 static_cast<std::uint64_t>(pe->fileBuffer->bufLen));
  const std::uint64_t fileSize = image.size();

  // --- DOS / NT headers ---------------------------------------------------
  std::uint32_t peOffset32 = 0;
  if (!image.readU32(DOS_E_LFANEW_OFFSET, peOffset32)) {
    setPlanError("dos header", DOS_E_LFANEW_OFFSET,
                 "e_lfanew is not readable");
    return AUTH_PLAN_ERR_HEADER;
  }
  const std::uint64_t peOffset = peOffset32;

  std::uint32_t signature = 0;
  if (!image.readU32(peOffset, signature) || signature != NT_MAGIC) {
    setPlanError("nt signature", peOffset,
                 "PE\\0\\0 signature missing");
    return AUTH_PLAN_ERR_HEADER;
  }

  const std::uint64_t fileHeaderOffset = peOffset + NT_SIGNATURE_SIZE;
  std::uint16_t numberOfSections = 0;
  std::uint16_t sizeOfOptionalHeader = 0;
  if (!image.readU16(
          fileHeaderOffset + FILE_HEADER_NUMBER_OF_SECTIONS_OFFSET,
          numberOfSections) ||
      !image.readU16(
          fileHeaderOffset + FILE_HEADER_SIZE_OF_OPTIONAL_HEADER_OFFSET,
          sizeOfOptionalHeader)) {
    setPlanError("file header", fileHeaderOffset,
                 "file header fields are not readable");
    return AUTH_PLAN_ERR_HEADER;
  }

  const std::uint64_t optionalHeaderOffset =
      fileHeaderOffset + FILE_HEADER_SIZE;
  std::uint16_t optionalMagic = 0;
  if (!image.readU16(optionalHeaderOffset, optionalMagic) ||
      (optionalMagic != OPTIONAL_MAGIC_PE32 &&
       optionalMagic != OPTIONAL_MAGIC_PE32_PLUS)) {
    setPlanError("optional header magic",
                 optionalHeaderOffset,
                 "optional header magic is neither PE32 nor PE32+");
    return AUTH_PLAN_ERR_HEADER;
  }

  const OptionalHeaderLayout ohLayout = layoutForMagic(optionalMagic);

  // The optional header must at least contain its fixed fields, and the
  // declared size must keep every data directory entry we touch on-disk.
  const std::uint64_t nrvaFieldOffset =
      optionalHeaderOffset + ohLayout.numberOfRvaAndSizesOffset;
  if (static_cast<std::uint64_t>(sizeOfOptionalHeader) <
      ohLayout.numberOfRvaAndSizesOffset + 4) {
    setPlanError("size of optional header",
                 fileHeaderOffset + FILE_HEADER_SIZE_OF_OPTIONAL_HEADER_OFFSET,
                 "optional header does not contain NumberOfRvaAndSizes");
    return AUTH_PLAN_ERR_HEADER;
  }

  std::uint32_t numberOfRvaAndSizes = 0;
  if (!image.readU32(nrvaFieldOffset, numberOfRvaAndSizes)) {
    setPlanError("number of rva and sizes",
                 nrvaFieldOffset, "field is not readable");
    return AUTH_PLAN_ERR_HEADER;
  }
  if (numberOfRvaAndSizes > MAX_NUMBER_OF_RVA_AND_SIZES) {
    setPlanError("number of rva and sizes",
                 nrvaFieldOffset, "value exceeds 16 directory entries");
    return AUTH_PLAN_ERR_HEADER;
  }

  const bool certificateDirectoryDeclared =
      numberOfRvaAndSizes > SECURITY_DIRECTORY_INDEX;
  const std::uint64_t certificateDirectoryFieldOffset =
      optionalHeaderOffset + ohLayout.dataDirectoryOffset +
      static_cast<std::uint64_t>(SECURITY_DIRECTORY_INDEX) *
          DATA_DIRECTORY_ENTRY_SIZE;
  if (certificateDirectoryDeclared &&
      static_cast<std::uint64_t>(sizeOfOptionalHeader) <
          ohLayout.dataDirectoryOffset +
              (SECURITY_DIRECTORY_INDEX + 1) * DATA_DIRECTORY_ENTRY_SIZE) {
    setPlanError("size of optional header",
                 fileHeaderOffset + FILE_HEADER_SIZE_OF_OPTIONAL_HEADER_OFFSET,
                 "declared certificate table entry runs past the optional "
                 "header");
    return AUTH_PLAN_ERR_HEADER;
  }

  const std::uint64_t sectionTableOffset =
      optionalHeaderOffset + sizeOfOptionalHeader;
  const std::uint64_t sectionTableEnd =
      sectionTableOffset +
      static_cast<std::uint64_t>(numberOfSections) * SECTION_HEADER_FILE_SIZE;
  if (sectionTableEnd > fileSize) {
    setPlanError("section table",
                 sectionTableOffset,
                 "section table runs past the end of the file");
    return AUTH_PLAN_ERR_SECTION_TABLE;
  }

  std::uint32_t sizeOfHeaders = 0;
  const std::uint64_t sizeOfHeadersOffset =
      optionalHeaderOffset + OPTIONAL_SIZE_OF_HEADERS_OFFSET;
  if (!image.readU32(sizeOfHeadersOffset, sizeOfHeaders)) {
    setPlanError("size of headers",
                 sizeOfHeadersOffset, "field is not readable");
    return AUTH_PLAN_ERR_HEADER;
  }
  if (static_cast<std::uint64_t>(sizeOfHeaders) < sectionTableEnd) {
    setPlanError("size of headers", sizeOfHeadersOffset,
                 "SizeOfHeaders does not cover the end of the section table");
    return AUTH_PLAN_ERR_HEADER;
  }
  if (static_cast<std::uint64_t>(sizeOfHeaders) > fileSize) {
    setPlanError("size of headers", sizeOfHeadersOffset,
                 "SizeOfHeaders runs past the end of the file");
    return AUTH_PLAN_ERR_HEADER;
  }

  const std::uint64_t checksumFieldOffset =
      optionalHeaderOffset + OPTIONAL_CHECKSUM_OFFSET;

  // --- Certificate Table data directory entry ----------------------------
  // Its VirtualAddress is a raw file offset, never an RVA: do not map it.
  std::uint32_t certDirectoryOffset = 0;
  std::uint32_t certDirectorySize = 0;
  std::uint64_t certificateTableOffset = 0;
  std::uint64_t certificateTableSize = 0;

  if (certificateDirectoryDeclared) {
    if (!image.readU32(certificateDirectoryFieldOffset,
                       certDirectoryOffset) ||
        !image.readU32(certificateDirectoryFieldOffset + 4,
                       certDirectorySize)) {
      setPlanError("certificate directory",
                   certificateDirectoryFieldOffset,
                   "directory entry is not readable");
      return AUTH_PLAN_ERR_CERTIFICATE_TABLE;
    }

    if ((certDirectoryOffset == 0) != (certDirectorySize == 0)) {
      setPlanError("certificate directory",
                   certificateDirectoryFieldOffset,
                   "exactly one of offset and size is zero");
      return AUTH_PLAN_ERR_CERTIFICATE_TABLE;
    }

    if (certDirectoryOffset != 0) {
      if ((certDirectoryOffset % 8) != 0 || (certDirectorySize % 8) != 0) {
        setPlanError(
                     "certificate table alignment",
                     certificateDirectoryFieldOffset,
                     "certificate table offset and size must be 8-byte "
                     "aligned");
        return AUTH_PLAN_ERR_CERTIFICATE_TABLE;
      }

      certificateTableOffset = certDirectoryOffset;
      certificateTableSize = certDirectorySize;
      const std::uint64_t certificateTableEnd =
          certificateTableOffset + certificateTableSize;
      if (certificateTableEnd > fileSize) {
        setPlanError("certificate table",
                     certificateTableOffset,
                     "certificate table runs past the end of the file");
        return AUTH_PLAN_ERR_CERTIFICATE_TABLE;
      }
      if (certificateTableEnd != fileSize) {
        setPlanError("certificate table",
                     certificateTableOffset,
                     "certificate table must be located at the end of the "
                     "file");
        return AUTH_PLAN_ERR_CERTIFICATE_TABLE;
      }
      if (static_cast<std::uint64_t>(sizeOfHeaders) >
          certificateTableOffset) {
        setPlanError("certificate table",
                     certificateTableOffset,
                     "certificate table overlaps the header range");
        return AUTH_PLAN_ERR_CERTIFICATE_TABLE;
      }
    }
  }

  // --- Sections ------------------------------------------------------------
  struct RawSection {
    std::uint32_t pointer;
    std::uint32_t size;
    int tableIndex;
  };

  std::vector<RawSection> rawSections;
  rawSections.reserve(numberOfSections);
  for (std::uint16_t i = 0; i < numberOfSections; ++i) {
    const std::uint64_t sectionHeaderOffset =
        sectionTableOffset +
        static_cast<std::uint64_t>(i) * SECTION_HEADER_FILE_SIZE;
    std::uint32_t sectionSize = 0;
    std::uint32_t sectionPointer = 0;
    if (!image.readU32(
            sectionHeaderOffset + SECTION_SIZE_OF_RAW_DATA_OFFSET,
            sectionSize) ||
        !image.readU32(
            sectionHeaderOffset + SECTION_POINTER_TO_RAW_DATA_OFFSET,
            sectionPointer)) {
      setPlanError("section header",
                   sectionHeaderOffset,
                   "section raw data fields are not readable");
      return AUTH_PLAN_ERR_SECTION_TABLE;
    }
    RawSection section;
    section.pointer = sectionPointer;
    section.size = sectionSize;
    section.tableIndex = static_cast<int>(i);
    rawSections.push_back(section);
  }

  std::vector<RawSection> nonEmptySections;
  for (std::vector<RawSection>::const_iterator it = rawSections.begin();
       it != rawSections.end();
       ++it) {
    if (it->size == 0) {
      // Zero raw-length sections contribute no digest input.
      continue;
    }

    const std::uint64_t sectionEnd =
        static_cast<std::uint64_t>(it->pointer) + it->size;
    if (sectionEnd > fileSize) {
      setPlanError("section raw data", it->pointer,
                   "section raw range runs past the end of the file");
      return AUTH_PLAN_ERR_SECTION;
    }
    if (static_cast<std::uint64_t>(it->pointer) < sizeOfHeaders) {
      setPlanError("section raw data", it->pointer,
                   "section raw range intrudes into the header range");
      return AUTH_PLAN_ERR_SECTION;
    }
    if (certificateTableSize != 0 &&
        sectionEnd > certificateTableOffset) {
      setPlanError("section raw data", it->pointer,
                   "section raw range intrudes into the certificate table");
      return AUTH_PLAN_ERR_SECTION;
    }
    nonEmptySections.push_back(*it);
  }

  std::sort(nonEmptySections.begin(),
            nonEmptySections.end(),
            [](const RawSection &lhs, const RawSection &rhs) {
              if (lhs.pointer != rhs.pointer) {
                return lhs.pointer < rhs.pointer;
              }
              return lhs.tableIndex < rhs.tableIndex;
            });

  for (std::size_t i = 1; i < nonEmptySections.size(); ++i) {
    const RawSection &previous = nonEmptySections[i - 1];
    const RawSection &current = nonEmptySections[i];
    const std::uint64_t previousEnd =
        static_cast<std::uint64_t>(previous.pointer) + previous.size;
    if (previousEnd > static_cast<std::uint64_t>(current.pointer)) {
      setPlanError("section raw data", current.pointer,
                   "section raw ranges overlap");
      return AUTH_PLAN_ERR_SECTION;
    }
  }

  // --- WIN_CERTIFICATE entries ---------------------------------------------
  std::vector<authenticode_certificate_entry> certificates;
  if (certificateTableSize != 0) {
    std::uint64_t cursor = certificateTableOffset;
    const std::uint64_t tableEnd =
        certificateTableOffset + certificateTableSize;

    while (cursor < tableEnd) {
      const std::uint64_t remaining = tableEnd - cursor;
      if (remaining < WIN_CERTIFICATE_HEADER_SIZE) {
        setPlanError(
                     "win_certificate header",
                     cursor,
                     "fewer than eight bytes remain for an entry header");
        return AUTH_PLAN_ERR_CERTIFICATE_ENTRY;
      }

      std::uint32_t dwLength = 0;
      std::uint16_t revision = 0;
      std::uint16_t certType = 0;
      if (!image.readU32(cursor + WIN_CERTIFICATE_LENGTH_OFFSET, dwLength) ||
          !image.readU16(cursor + WIN_CERTIFICATE_REVISION_OFFSET,
                         revision) ||
          !image.readU16(cursor + WIN_CERTIFICATE_TYPE_OFFSET, certType)) {
        setPlanError(
                     "win_certificate header",
                     cursor,
                     "entry header fields are not readable");
        return AUTH_PLAN_ERR_CERTIFICATE_ENTRY;
      }

      if (dwLength < WIN_CERTIFICATE_HEADER_SIZE) {
        setPlanError(
                     "win_certificate length",
                     cursor,
                     "dwLength is smaller than the eight-byte header");
        return AUTH_PLAN_ERR_CERTIFICATE_ENTRY;
      }
      if (static_cast<std::uint64_t>(dwLength) > remaining) {
        setPlanError(
                     "win_certificate length",
                     cursor,
                     "dwLength runs past the end of the certificate table");
        return AUTH_PLAN_ERR_CERTIFICATE_ENTRY;
      }

      bool alignOverflow = false;
      const std::uint64_t storageLength = alignUp8(dwLength, &alignOverflow);
      if (alignOverflow || cursor + storageLength > tableEnd) {
        setPlanError(
                     "win_certificate alignment",
                     cursor,
                     "8-byte rounded entry storage overflows or runs past "
                     "the table");
        return AUTH_PLAN_ERR_CERTIFICATE_ENTRY;
      }

      authenticode_certificate_entry entry;
      entry.dw_length = dwLength;
      entry.revision = revision;
      entry.type = certType;
      entry.entry_offset = cursor;
      entry.entry_length = dwLength;
      entry.payload_offset = cursor + WIN_CERTIFICATE_HEADER_SIZE;
      entry.payload_length =
          static_cast<std::uint64_t>(dwLength) - WIN_CERTIFICATE_HEADER_SIZE;
      entry.storage_offset = cursor;
      entry.storage_length = storageLength;
      certificates.push_back(entry);

      cursor += storageLength;
    }
  }

  // --- Excluded header fields ----------------------------------------------
  std::vector<authenticode_excluded_range> excluded;
  {
    authenticode_excluded_range checksum;
    checksum.offset = checksumFieldOffset;
    checksum.length = 4;
    checksum.field = AUTH_EXCLUDED_CHECKSUM;
    excluded.push_back(checksum);

    if (certificateDirectoryDeclared) {
      authenticode_excluded_range certificateDirectory;
      certificateDirectory.offset = certificateDirectoryFieldOffset;
      certificateDirectory.length = DATA_DIRECTORY_ENTRY_SIZE;
      certificateDirectory.field = AUTH_EXCLUDED_CERTIFICATE_DIRECTORY;
      excluded.push_back(certificateDirectory);
    }
  }
  std::sort(excluded.begin(),
            excluded.end(),
            [](const authenticode_excluded_range &lhs,
               const authenticode_excluded_range &rhs) {
              return lhs.offset < rhs.offset;
            });

  // --- Ordered digest spans ------------------------------------------------
  std::vector<authenticode_span> spans;

  // Stage 1: the header range, skipping the excluded fields.
  std::uint64_t position = 0;
  for (std::vector<authenticode_excluded_range>::const_iterator it =
           excluded.begin();
       it != excluded.end();
       ++it) {
    if (it->offset > position) {
      authenticode_span span;
      span.offset = position;
      span.length = it->offset - position;
      span.stage = AUTH_SPAN_HEADER;
      span.section_index = -1;
      spans.push_back(span);
    }
    position = it->offset + it->length;
  }
  if (static_cast<std::uint64_t>(sizeOfHeaders) > position) {
    authenticode_span span;
    span.offset = position;
    span.length = static_cast<std::uint64_t>(sizeOfHeaders) - position;
    span.stage = AUTH_SPAN_HEADER;
    span.section_index = -1;
    spans.push_back(span);
  }

  // Stage 2: non-empty sections in PointerToRawData order. The running
  // counter accumulates SizeOfRawData, including any inter-section gaps.
  std::uint64_t sumOfBytesHashed = sizeOfHeaders;
  for (std::vector<RawSection>::const_iterator it = nonEmptySections.begin();
       it != nonEmptySections.end();
       ++it) {
    if (sumOfBytesHashed >
        0xFFFFFFFFFFFFFFFFULL - static_cast<std::uint64_t>(it->size)) {
      setPlanError("sum of bytes hashed",
                   it->pointer, "SUM_OF_BYTES_HASHED overflow");
      return AUTH_PLAN_ERR_OVERFLOW;
    }
    sumOfBytesHashed += it->size;

    authenticode_span span;
    span.offset = it->pointer;
    span.length = it->size;
    span.stage = AUTH_SPAN_SECTION;
    span.section_index = it->tableIndex;
    spans.push_back(span);
  }

  // Stage 3: the trailing range starts at the accumulated counter, not at the
  // physical end of the last section, and may therefore re-cover gap bytes.
  const std::uint64_t trailingEnd = fileSize - certificateTableSize;
  if (sumOfBytesHashed > trailingEnd) {
    setPlanError("sum of bytes hashed",
                 sumOfBytesHashed,
                 "accumulated hash input exceeds the hashable file range");
    return AUTH_PLAN_ERR_SECTION;
  }
  if (sumOfBytesHashed < trailingEnd) {
    authenticode_span span;
    span.offset = sumOfBytesHashed;
    span.length = trailingEnd - sumOfBytesHashed;
    span.stage = AUTH_SPAN_TRAILING;
    span.section_index = -1;
    spans.push_back(span);
  }

  // --- Plan binding and result ---------------------------------------------
  authenticode_layout_binding binding;
  binding.pe_offset = static_cast<std::uint32_t>(peOffset);
  binding.optional_magic = optionalMagic;
  binding.size_of_optional_header = sizeOfOptionalHeader;
  binding.number_of_sections = numberOfSections;
  binding.number_of_rva_and_sizes = numberOfRvaAndSizes;
  binding.size_of_headers = sizeOfHeaders;
  binding.checksum_field_offset = checksumFieldOffset;
  binding.certificate_directory_field_offset = certificateDirectoryFieldOffset;
  binding.certificate_directory_declared = certificateDirectoryDeclared;
  binding.certificate_directory_offset = certDirectoryOffset;
  binding.certificate_directory_size = certDirectorySize;
  binding.sections.reserve(rawSections.size());
  for (std::vector<RawSection>::const_iterator it = rawSections.begin();
       it != rawSections.end();
       ++it) {
    authenticode_section_binding sectionBinding;
    sectionBinding.pointer_to_raw_data = it->pointer;
    sectionBinding.size_of_raw_data = it->size;
    binding.sections.push_back(sectionBinding);
  }

  plan_out.file_size = fileSize;
  plan_out.certificate_table_offset = certificateTableOffset;
  plan_out.certificate_table_size = certificateTableSize;
  plan_out.certificate_table_declared = certificateDirectoryDeclared;
  plan_out.size_of_headers = sizeOfHeaders;
  plan_out.sum_of_bytes_hashed = sumOfBytesHashed;
  plan_out.spans.swap(spans);
  plan_out.excluded.swap(excluded);
  plan_out.certificates.swap(certificates);
  plan_out.binding = binding;

  return AUTH_PLAN_OK;
}

namespace {

bool mismatch(const char *category,
              std::uint64_t offset,
              const std::string &detail) {
  setPlanError(category, offset, detail);
  return false;
}

bool verifyBinding(const authenticode_plan &plan,
                   const RawImage &image,
                   std::uint64_t sourceSize) {
  const authenticode_layout_binding &binding = plan.binding;

  if (sourceSize != plan.file_size) {
    return mismatch("file size", 0, "source length differs from the plan");
  }

  std::uint32_t peOffset = 0;
  if (!image.readU32(DOS_E_LFANEW_OFFSET, peOffset) ||
      peOffset != binding.pe_offset) {
    return mismatch("dos header", DOS_E_LFANEW_OFFSET,
                    "e_lfanew differs from the bound plan");
  }

  std::uint32_t signature = 0;
  if (!image.readU32(peOffset, signature) || signature != NT_MAGIC) {
    return mismatch("nt signature", peOffset,
                    "PE signature missing in the source");
  }

  const std::uint64_t fileHeaderOffset = peOffset + NT_SIGNATURE_SIZE;
  std::uint16_t numberOfSections = 0;
  std::uint16_t sizeOfOptionalHeader = 0;
  if (!image.readU16(
          fileHeaderOffset + FILE_HEADER_NUMBER_OF_SECTIONS_OFFSET,
          numberOfSections) ||
      numberOfSections != binding.number_of_sections) {
    return mismatch("file header",
                    fileHeaderOffset + FILE_HEADER_NUMBER_OF_SECTIONS_OFFSET,
                    "NumberOfSections differs from the bound plan");
  }
  if (!image.readU16(
          fileHeaderOffset + FILE_HEADER_SIZE_OF_OPTIONAL_HEADER_OFFSET,
          sizeOfOptionalHeader) ||
      sizeOfOptionalHeader != binding.size_of_optional_header) {
    return mismatch(
        "file header",
        fileHeaderOffset + FILE_HEADER_SIZE_OF_OPTIONAL_HEADER_OFFSET,
        "SizeOfOptionalHeader differs from the bound plan");
  }

  const std::uint64_t optionalHeaderOffset =
      fileHeaderOffset + FILE_HEADER_SIZE;
  std::uint16_t optionalMagic = 0;
  if (!image.readU16(optionalHeaderOffset, optionalMagic) ||
      optionalMagic != binding.optional_magic) {
    return mismatch("optional header magic", optionalHeaderOffset,
                    "optional header magic differs from the bound plan");
  }

  const OptionalHeaderLayout ohLayout = layoutForMagic(optionalMagic);
  std::uint32_t numberOfRvaAndSizes = 0;
  if (!image.readU32(
          optionalHeaderOffset + ohLayout.numberOfRvaAndSizesOffset,
          numberOfRvaAndSizes) ||
      numberOfRvaAndSizes != binding.number_of_rva_and_sizes) {
    return mismatch(
        "number of rva and sizes",
        optionalHeaderOffset + ohLayout.numberOfRvaAndSizesOffset,
        "NumberOfRvaAndSizes differs from the bound plan");
  }

  std::uint32_t sizeOfHeaders = 0;
  if (!image.readU32(
          optionalHeaderOffset + OPTIONAL_SIZE_OF_HEADERS_OFFSET,
          sizeOfHeaders) ||
      sizeOfHeaders != binding.size_of_headers) {
    return mismatch("size of headers",
                    optionalHeaderOffset + OPTIONAL_SIZE_OF_HEADERS_OFFSET,
                    "SizeOfHeaders differs from the bound plan");
  }

  if (optionalHeaderOffset + OPTIONAL_CHECKSUM_OFFSET !=
      binding.checksum_field_offset) {
    return mismatch("checksum field",
                    optionalHeaderOffset + OPTIONAL_CHECKSUM_OFFSET,
                    "CheckSum field location differs from the bound plan");
  }

  if (binding.certificate_directory_declared) {
    const std::uint64_t fieldOffset =
        optionalHeaderOffset + ohLayout.dataDirectoryOffset +
        static_cast<std::uint64_t>(SECURITY_DIRECTORY_INDEX) *
            DATA_DIRECTORY_ENTRY_SIZE;
    if (fieldOffset != binding.certificate_directory_field_offset) {
      return mismatch("certificate directory", fieldOffset,
                      "certificate directory field location differs from "
                      "the bound plan");
    }
    std::uint32_t directoryOffset = 0;
    std::uint32_t directorySize = 0;
    if (!image.readU32(fieldOffset, directoryOffset) ||
        !image.readU32(fieldOffset + 4, directorySize) ||
        directoryOffset != binding.certificate_directory_offset ||
        directorySize != binding.certificate_directory_size) {
      return mismatch("certificate directory", fieldOffset,
                      "certificate table offset or size differs from the "
                      "bound plan");
    }
  }

  const std::uint64_t sectionTableOffset =
      optionalHeaderOffset + sizeOfOptionalHeader;
  if (binding.sections.size() !=
      static_cast<std::size_t>(numberOfSections)) {
    return mismatch("section table", sectionTableOffset,
                    "section count differs from the bound plan");
  }
  for (std::size_t i = 0; i < binding.sections.size(); ++i) {
    const std::uint64_t sectionHeaderOffset =
        sectionTableOffset +
        static_cast<std::uint64_t>(i) * SECTION_HEADER_FILE_SIZE;
    std::uint32_t sectionSize = 0;
    std::uint32_t sectionPointer = 0;
    if (!image.readU32(
            sectionHeaderOffset + SECTION_SIZE_OF_RAW_DATA_OFFSET,
            sectionSize) ||
        !image.readU32(
            sectionHeaderOffset + SECTION_POINTER_TO_RAW_DATA_OFFSET,
            sectionPointer) ||
        sectionSize != binding.sections[i].size_of_raw_data ||
        sectionPointer != binding.sections[i].pointer_to_raw_data) {
      return mismatch("section table", sectionHeaderOffset,
                      "section PointerToRawData or SizeOfRawData differs "
                      "from the bound plan");
    }
  }

  // The whole plan must check out against the source before any callback is
  // allowed to run: every span has to stay inside the supplied buffer.
  for (std::vector<authenticode_span>::const_iterator it = plan.spans.begin();
       it != plan.spans.end();
       ++it) {
    if (it->offset > sourceSize ||
        sourceSize - it->offset < it->length) {
      return mismatch("hash span",
                      it->offset,
                      "plan span runs past the supplied source");
    }
  }

  return true;
}

} // namespace

authenticode_plan_status
FeedAuthenticodePlan(const authenticode_plan &plan,
                     const std::uint8_t *source,
                     std::uint64_t source_size,
                     authenticode_hash_callback cb,
                     void *context) {
  g_plan_error.clear();

  if (cb == nullptr) {
    setPlanError("argument", 0,
                 "null hash callback");
    return AUTH_PLAN_ERR_ARGUMENT;
  }
  if (source == nullptr && source_size != 0) {
    setPlanError("argument", 0,
                 "null source buffer with non-zero size");
    return AUTH_PLAN_ERR_ARGUMENT;
  }

  const RawImage image(source, source_size);

  // Verify the complete plan against the source before any callback runs.
  if (!verifyBinding(plan, image, source_size)) {
    return AUTH_PLAN_ERR_SOURCE_MISMATCH;
  }

  for (std::vector<authenticode_span>::const_iterator it = plan.spans.begin();
       it != plan.spans.end();
       ++it) {
    if (it->length == 0) {
      continue;
    }
    if (it->offset > source_size ||
        source_size - it->offset < it->length) {
      setPlanError(
                   "hash span",
                   it->offset,
                   "plan span runs past the supplied source");
      return AUTH_PLAN_ERR_SOURCE_MISMATCH;
    }

    const int callbackStatus =
        cb(context,
           it->offset,
           source + it->offset,
           it->length,
           it->stage,
           it->section_index);
    if (callbackStatus != 0) {
      return static_cast<authenticode_plan_status>(callbackStatus);
    }
  }

  return AUTH_PLAN_OK;
}

} // namespace peparse
