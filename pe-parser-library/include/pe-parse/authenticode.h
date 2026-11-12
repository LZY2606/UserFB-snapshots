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

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <pe-parse/nt-headers.h>
#include <pe-parse/parse.h>

namespace peparse {

// ---------------------------------------------------------------------------
// Authenticode PE image-hash planning
//
// These types describe *which bytes* of an on-disk PE file feed the
// Authenticode image hash, in feed order. The library never hashes anything
// itself: signature trust verification and any cryptographic dependency stay
// with the caller. Nothing here loads or executes the image, and every
// address below is a raw file offset. In particular, the Certificate Table
// data directory entry is never interpreted as an RVA.
//
// The feed order follows the simplified PE image hash procedure from the
// Microsoft "Windows Authenticode Portable Executable Signature Format"
// specification:
//
//   1. The first SizeOfHeaders bytes (the header range), with the 4-byte
//      CheckSum field and, when the Certificate Table directory entry is
//      actually present, its 8 bytes skipped. SUM_OF_BYTES_HASHED starts at
//      SizeOfHeaders; the skipped field lengths are still counted in it.
//   2. Every section with a non-zero SizeOfRawData, ordered by
//      PointerToRawData, fed in full. Each section's SizeOfRawData is added
//      to SUM_OF_BYTES_HASHED.
//   3. A single trailing range [SUM_OF_BYTES_HASHED, file_size - cert_size).
//
// SUM_OF_BYTES_HASHED is an accumulated size, not the physical end of the
// last section. When sections leave inter-section gaps, the trailing range
// can therefore cover bytes that were already fed. That ordered overlap is
// intentional and is preserved: spans are never coalesced into a
// non-overlapping interval set, and duplicate bytes are delivered to the
// digest callback more than once.
// ---------------------------------------------------------------------------

// Which stage of the Authenticode image-hash procedure produced a span.
enum authenticode_span_stage {
  AUTH_SPAN_HEADER = 0,
  AUTH_SPAN_SECTION = 1,
  AUTH_SPAN_TRAILING = 2
};

// A contiguous run of file bytes that must be fed to the digest, in feed
// order. All values are file offsets / file lengths. section_index is the
// zero-based index of the span's section in the on-disk section table for
// AUTH_SPAN_SECTION spans, and -1 otherwise.
struct authenticode_span {
  std::uint64_t offset;
  std::uint64_t length;
  authenticode_span_stage stage;
  int section_index;
};

// A header field that is deliberately excluded from the digest input. The
// plan always excludes the 4-byte CheckSum field, and excludes the 8-byte
// Certificate Table data directory entry only when that entry is actually
// declared (NumberOfRvaAndSizes > 4). A declared but zero/zero certificate
// entry is still excluded; when the entry is not declared, its eight bytes
// may belong to other content and are not excluded.
enum authenticode_excluded_field {
  AUTH_EXCLUDED_CHECKSUM = 0,
  AUTH_EXCLUDED_CERTIFICATE_DIRECTORY = 1
};

struct authenticode_excluded_range {
  std::uint64_t offset;
  std::uint64_t length;
  authenticode_excluded_field field;
};

// One WIN_CERTIFICATE structure inside the Attribute Certificate Table.
//
//   entry_offset/entry_length  the raw on-disk entry as declared by
//                              dwLength (never includes alignment padding)
//   payload_offset/payload_length
//                              the bytes after the 8-byte WIN_CERTIFICATE
//                              header; padding is never part of the payload
//   storage_offset/storage_length
//                              the entry rounded up to 8-byte alignment; the
//                              next entry starts at storage end
//
// Unknown wRevision/wCertificateType values are retained as opaque entries.
struct authenticode_certificate_entry {
  std::uint32_t dw_length;
  std::uint16_t revision;
  std::uint16_t type;

  std::uint64_t entry_offset;
  std::uint64_t entry_length;

  std::uint64_t payload_offset;
  std::uint64_t payload_length;

  std::uint64_t storage_offset;
  std::uint64_t storage_length;
};

// Raw section-table descriptor captured at plan-build time, kept in
// section-table order (not in hashing order). FeedAuthenticodePlan re-reads
// these from the supplied source to prove that its layout still matches.
struct authenticode_section_binding {
  std::uint32_t pointer_to_raw_data;
  std::uint32_t size_of_raw_data;
};

// The layout descriptors that drive traversal. Content fields (section
// bytes, the CheckSum value, certificate payloads) are deliberately absent:
// changing them does not invalidate a plan, whereas changing anything here
// does.
struct authenticode_layout_binding {
  std::uint32_t pe_offset;
  std::uint16_t optional_magic;
  std::uint16_t size_of_optional_header;
  std::uint16_t number_of_sections;
  std::uint32_t number_of_rva_and_sizes;
  std::uint32_t size_of_headers;

  std::uint64_t checksum_field_offset;
  // Zero when the Certificate Table entry is not declared.
  std::uint64_t certificate_directory_field_offset;
  bool certificate_directory_declared;
  std::uint32_t certificate_directory_offset;
  std::uint32_t certificate_directory_size;

  std::vector<authenticode_section_binding> sections;
};

// A complete, self-contained Authenticode hashing plan. A plan outlives its
// parsed_pe: DestructParsedPE may be called before the plan is inspected or
// fed, because the plan owns all of its metadata and a private copy of the
// layout descriptors it needs.
struct authenticode_plan {
  // Total length of the source file this plan was built for.
  std::uint64_t file_size;

  // File offset and size of the Attribute Certificate Table. Both are zero
  // when the file carries no embedded certificate table.
  std::uint64_t certificate_table_offset;
  std::uint64_t certificate_table_size;
  bool certificate_table_declared;

  // SizeOfHeaders from the optional header; the initial value of
  // SUM_OF_BYTES_HASHED.
  std::uint32_t size_of_headers;

  // The running SUM_OF_BYTES_HASHED counter after every section has been
  // accumulated (SizeOfHeaders plus every non-empty section's
  // SizeOfRawData). The trailing span starts at this file offset.
  std::uint64_t sum_of_bytes_hashed;

  // Ordered digest input. Overlaps caused by section gaps are preserved.
  std::vector<authenticode_span> spans;

  // Header fields skipped while feeding the header range.
  std::vector<authenticode_excluded_range> excluded;

  // WIN_CERTIFICATE entries in table order. Empty when no table is present.
  std::vector<authenticode_certificate_entry> certificates;

  // Snapshot of the layout this plan is bound to.
  authenticode_layout_binding binding;
};

// Why a plan could not be built or a plan could not be fed. Every code names
// the structural category of the rejection; GetAuthenticodePlanError() also
// reports the actual file position involved.
enum authenticode_plan_status {
  AUTH_PLAN_OK = 0,
  AUTH_PLAN_ERR_ARGUMENT = 1,
  AUTH_PLAN_ERR_HEADER = 2,
  AUTH_PLAN_ERR_SECTION_TABLE = 3,
  AUTH_PLAN_ERR_SECTION = 4,
  AUTH_PLAN_ERR_CERTIFICATE_TABLE = 5,
  AUTH_PLAN_ERR_CERTIFICATE_ENTRY = 6,
  AUTH_PLAN_ERR_OVERFLOW = 7,
  AUTH_PLAN_ERR_SOURCE_MISMATCH = 8
};

// Build an Authenticode hashing plan for pe. On success returns AUTH_PLAN_OK
// and fills plan_out (which is first cleared). On failure no partial plan is
// returned: plan_out is cleared and a non-zero status is returned, with a
// human-readable category and file-offset description available through
// GetAuthenticodePlanError().
authenticode_plan_status
BuildAuthenticodePlan(parsed_pe *pe, authenticode_plan &plan_out);

// Last error description produced by BuildAuthenticodePlan or
// FeedAuthenticodePlan in this thread, including the structural category and
// the actual file offset or range that triggered the rejection.
std::string GetAuthenticodePlanError();

// Callback invoked once per span, in plan order. offset is the span's
// position in the original file, data points at length bytes of that file,
// and section_index/stage come straight from the span. Returning non-zero
// aborts feeding and that value is returned verbatim to the caller.
typedef int (*authenticode_hash_callback)(void *context,
                                          std::uint64_t offset,
                                          const std::uint8_t *data,
                                          std::uint64_t length,
                                          authenticode_span_stage stage,
                                          int section_index);

// Verify that source is the same length and still has the layout (header,
// section table and certificate table descriptors) the plan was bound to,
// then feed every span to cb in order. Verification completes before any
// callback runs. Content-only changes (a section byte, the CheckSum value or
// certificate payload bytes) reuse the same plan; layout changes are
// rejected with AUTH_PLAN_ERR_SOURCE_MISMATCH. A non-zero callback return is
// passed through unchanged.
authenticode_plan_status
FeedAuthenticodePlan(const authenticode_plan &plan,
                     const std::uint8_t *source,
                     std::uint64_t source_size,
                     authenticode_hash_callback cb,
                     void *context);

} // namespace peparse
