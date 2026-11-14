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

// The public header only uses freestanding headers; this translation unit
// is built by CMake with an absolute include path, so the full STL is
// available here for the internal interpretation model. The model is
// frozen into the header's owning arrays only after interpretation
// succeeds, which keeps every failure free of partially published state.

#include <pe-parse/relocation_preview.h>

#include <pe-parse/nt-headers.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace peparse {
namespace {

constexpr std::uint32_t kBaseRelocDirectory = 5;
constexpr std::uint32_t kPeSignature = 0x00004550;
constexpr std::uint64_t kAbsInt64Min = 0x8000000000000000ULL;

struct RawSection {
  std::uint16_t index;
  std::uint32_t virtualAddress;
  std::uint32_t rawSize;
  std::uint32_t rawPointer;
};

struct FragModel {
  std::uint16_t sectionIndex;
  std::uint32_t fileOffset;
  std::vector<std::uint8_t> bytes;
};

struct OpModel {
  std::size_t ordinal;
  std::size_t entryIndex;
  RelocPreviewKind kind;
  RelocPreviewWidth width;

  std::uint16_t sectionIndex;
  std::uint32_t entryFileOffset;
  std::uint16_t rawEntry;
  std::vector<FragModel> entryFragments;

  std::uint32_t targetRva;
  std::vector<std::uint8_t> before;
  std::vector<std::uint8_t> after;
  std::vector<FragModel> targetFragments;

  bool hasPair;
  std::uint16_t pairSectionIndex;
  std::uint32_t pairFileOffset;
  std::uint16_t pairRawValue;
  std::vector<FragModel> pairFragments;

  OpModel()
      : ordinal(0),
        entryIndex(0),
        kind(RelocPreviewKind_Absolute),
        width(RelocPreviewWidth_None),
        sectionIndex(0),
        entryFileOffset(0),
        rawEntry(0),
        targetRva(0),
        hasPair(false),
        pairSectionIndex(0),
        pairFileOffset(0),
        pairRawValue(0) {
  }
};

struct BlockModel {
  std::uint32_t pageRva;
  std::uint32_t blockSize;
  std::uint32_t directoryOffset;
  std::uint32_t headerFileOffset;
  std::vector<FragModel> headerFragments;
  std::vector<OpModel> operations;

  BlockModel()
      : pageRva(0),
        blockSize(0),
        directoryOffset(0),
        headerFileOffset(0) {
  }
};

struct IntervalModel {
  std::uint32_t firstRva;
  std::uint32_t lastRva;
  std::vector<std::uint8_t> original;
  std::vector<std::uint8_t> finalBytes;
  std::vector<std::size_t> ordinals;
};

struct Model {
  bool pe32Plus;
  std::uint64_t oldImageBase;
  std::uint64_t newImageBase;
  std::int64_t baseDelta;
  bool directoryPresent;
  std::uint32_t directoryRva;
  std::uint32_t directorySize;
  std::vector<BlockModel> blocks;
  std::vector<IntervalModel> intervals;

  Model()
      : pe32Plus(false),
        oldImageBase(0),
        newImageBase(0),
        baseDelta(0),
        directoryPresent(false),
        directoryRva(0),
        directorySize(0) {
  }
};

struct PreviewFailure {
  RelocPreviewErrorCategory category;
  std::string message;
  bool hasFileOffset;
  std::uint64_t fileOffset;
  bool hasRva;
  std::uint32_t rva;

  PreviewFailure(RelocPreviewErrorCategory c, const std::string &m)
      : category(c),
        message(m),
        hasFileOffset(false),
        fileOffset(0),
        hasRva(false),
        rva(0) {
  }
};

static void failAt(RelocPreviewErrorCategory category,
                   const std::string &message) {
  throw PreviewFailure(category, message);
}

static void failAtFile(RelocPreviewErrorCategory category,
                       std::uint64_t fileOffset,
                       const std::string &message) {
  PreviewFailure f(category, message);
  f.hasFileOffset = true;
  f.fileOffset = fileOffset;
  throw f;
}

static void failAtRva(RelocPreviewErrorCategory category,
                      std::uint32_t rva,
                      const std::string &message) {
  PreviewFailure f(category, message);
  f.hasRva = true;
  f.rva = rva;
  throw f;
}

// Bounded, little-endian reader over the frozen input bytes.
class ByteReader {
public:
  ByteReader(const std::uint8_t *data, std::size_t size)
      : data_(data), size_(size) {
  }

  std::uint16_t u16(std::uint64_t offset) const {
    require(offset, 2);
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(data_[offset]) |
        (static_cast<std::uint16_t>(data_[offset + 1]) << 8));
  }

  std::uint32_t u32(std::uint64_t offset) const {
    require(offset, 4);
    std::uint32_t v = 0;
    for (std::uint64_t i = 0; i < 4; ++i) {
      v |= static_cast<std::uint32_t>(data_[offset + i])
           << static_cast<unsigned>(8 * i);
    }
    return v;
  }

  std::uint64_t u64(std::uint64_t offset) const {
    require(offset, 8);
    std::uint64_t v = 0;
    for (std::uint64_t i = 0; i < 8; ++i) {
      v |= static_cast<std::uint64_t>(data_[offset + i])
           << static_cast<unsigned>(8 * i);
    }
    return v;
  }

  std::vector<std::uint8_t> bytes(std::uint64_t offset,
                                  std::uint32_t length) const {
    require(offset, length);
    return std::vector<std::uint8_t>(data_ + offset,
                                     data_ + offset + length);
  }

  std::size_t size() const {
    return size_;
  }

private:
  void require(std::uint64_t offset, std::uint64_t length) const {
    if (offset > size_ || length > size_ - offset) {
      failAtFile(RelocPreviewError_Mapping, offset,
                 "read exceeds the bounds of the input file");
    }
  }

  const std::uint8_t *data_;
  std::size_t size_;
};

// Maps RVAs to unique, raw-backed section bytes, one byte at a time.
class SectionMapper {
public:
  SectionMapper(const ByteReader &reader,
                std::vector<RawSection> sections,
                std::uint32_t sizeOfHeaders)
      : reader_(reader),
        sections_(std::move(sections)),
        sizeOfHeaders_(sizeOfHeaders) {
    // Reject overlapping RVA ownership between raw-backed sections.
    for (std::size_t i = 0; i < sections_.size(); ++i) {
      const RawSection &a = sections_[i];
      if (a.rawSize == 0) {
        continue;
      }
      for (std::size_t j = i + 1; j < sections_.size(); ++j) {
        const RawSection &b = sections_[j];
        if (b.rawSize == 0) {
          continue;
        }
        bool disjoint =
            (a.virtualAddress >= b.virtualAddress + b.rawSize) ||
            (b.virtualAddress >= a.virtualAddress + a.rawSize);
        if (!disjoint) {
          failAtRva(RelocPreviewError_Mapping,
                    std::max(a.virtualAddress, b.virtualAddress),
                    "two sections claim ownership of the same RVA byte");
        }
      }
    }
  }

  // Resolve [rva, rva+length) into per-section fragments holding the
  // original input bytes. Gaps, virtual-only tails, wraps and header
  // resident bytes are all rejected.
  std::vector<FragModel> mapRange(std::uint32_t rva,
                                  std::uint32_t length) const {
    std::vector<FragModel> out;
    std::uint64_t pos = rva;
    std::uint64_t remaining = length;

    while (remaining != 0) {
      if (pos > std::numeric_limits<std::uint32_t>::max()) {
        failAtRva(RelocPreviewError_Mapping, rva,
                  "RVA range wraps past the 32-bit RVA space");
      }

      const RawSection *owner = nullptr;
      for (const RawSection &s : sections_) {
        if (s.rawSize == 0) {
          continue;
        }
        if (pos >= s.virtualAddress &&
            pos < static_cast<std::uint64_t>(s.virtualAddress) +
                      s.rawSize) {
          if (owner != nullptr) {
            failAtRva(RelocPreviewError_Mapping,
                      static_cast<std::uint32_t>(pos),
                      "RVA byte has more than one owning section");
          }
          owner = &s;
        }
      }

      if (owner == nullptr) {
        failAtRva(RelocPreviewError_Mapping,
                  static_cast<std::uint32_t>(pos),
                  "RVA byte is not backed by raw section data");
      }

      std::uint64_t sectionLeft =
          static_cast<std::uint64_t>(owner->virtualAddress) +
          owner->rawSize - pos;
      std::uint64_t chunk = std::min(remaining, sectionLeft);
      std::uint64_t fileOffset =
          static_cast<std::uint64_t>(owner->rawPointer) +
          (pos - owner->virtualAddress);

      if (fileOffset < sizeOfHeaders_) {
        failAtRva(RelocPreviewError_Mapping,
                  static_cast<std::uint32_t>(pos),
                  "target byte resides in the PE headers, which is not "
                  "supported");
      }

      FragModel fragment;
      fragment.sectionIndex = owner->index;
      fragment.fileOffset = static_cast<std::uint32_t>(fileOffset);
      fragment.bytes =
          reader_.bytes(fileOffset, static_cast<std::uint32_t>(chunk));
      out.push_back(std::move(fragment));

      pos += chunk;
      remaining -= chunk;
    }

    return out;
  }

private:
  const ByteReader &reader_;
  std::vector<RawSection> sections_;
  std::uint32_t sizeOfHeaders_;
};

static std::uint16_t le16(const std::vector<std::uint8_t> &b,
                          std::size_t off) {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(b[off]) |
      (static_cast<std::uint16_t>(b[off + 1]) << 8));
}

static std::uint32_t le32(const std::vector<std::uint8_t> &b,
                          std::size_t off) {
  std::uint32_t v = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    v |= static_cast<std::uint32_t>(b[off + i])
         << static_cast<unsigned>(8 * i);
  }
  return v;
}

static std::uint64_t le64(const std::vector<std::uint8_t> &b,
                          std::size_t off) {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(b[off + i])
         << static_cast<unsigned>(8 * i);
  }
  return v;
}

static void putLe16(std::vector<std::uint8_t> &b, std::size_t off,
                    std::uint16_t v) {
  b[off] = static_cast<std::uint8_t>(v);
  b[off + 1] = static_cast<std::uint8_t>(v >> 8);
}

static void putLe32(std::vector<std::uint8_t> &b, std::size_t off,
                    std::uint32_t v) {
  for (std::size_t i = 0; i < 4; ++i) {
    b[off + i] =
        static_cast<std::uint8_t>(v >> static_cast<unsigned>(8 * i));
  }
}

static void putLe64(std::vector<std::uint8_t> &b, std::size_t off,
                    std::uint64_t v) {
  for (std::size_t i = 0; i < 8; ++i) {
    b[off + i] =
        static_cast<std::uint8_t>(v >> static_cast<unsigned>(8 * i));
  }
}

static std::uint16_t addMod16(std::uint16_t a, std::uint16_t b) {
  return static_cast<std::uint16_t>(static_cast<std::uint32_t>(a) +
                                    static_cast<std::uint32_t>(b));
}

static std::uint32_t addMod32(std::uint32_t a, std::int64_t delta) {
  return a + static_cast<std::uint32_t>(static_cast<std::uint64_t>(delta));
}

static std::uint64_t addMod64(std::uint64_t a, std::int64_t delta) {
  return a + static_cast<std::uint64_t>(delta);
}

static std::int64_t checkedAdd(std::int64_t a, std::int64_t b,
                               std::uint32_t targetRva) {
  if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) {
    failAtRva(RelocPreviewError_Base, targetRva,
              "signed base relocation arithmetic would overflow "
              "int64_t");
  }
  if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) {
    failAtRva(RelocPreviewError_Base, targetRva,
              "signed base relocation arithmetic would underflow "
              "int64_t");
  }
  return a + b;
}

// Floor division by 2^16 without right-shifting negatives or negating
// INT64_MIN. Intermediate work stays in well-defined unsigned types.
static std::int64_t floorDivPow2_16(std::int64_t x) {
  if (x >= 0) {
    return x >> 16;
  }
  // |x| as an unsigned magnitude (also correct for INT64_MIN), then the
  // standard ceiling adjustment before truncating the positive value.
  std::uint64_t magnitude = 0ULL - static_cast<std::uint64_t>(x);
  std::uint64_t adjusted = magnitude + 0xFFFFULL;
  return -static_cast<std::int64_t>(adjusted >> 16);
}

static std::uint16_t highAdjResult(std::uint16_t h, std::int16_t low,
                                   std::int64_t delta,
                                   std::uint32_t targetRva) {
  std::int64_t x = static_cast<std::int64_t>(h) << 16;
  x = checkedAdd(x, static_cast<std::int64_t>(low), targetRva);
  x = checkedAdd(x, delta, targetRva);
  x = checkedAdd(x, 0x8000, targetRva);
  return static_cast<std::uint16_t>(
      static_cast<std::uint64_t>(floorDivPow2_16(x)) & 0xFFFFULL);
}

struct HeaderInfo {
  bool pe32Plus;
  std::uint16_t machine;
  std::uint16_t characteristics;
  std::uint16_t sectionCount;
  std::uint32_t sizeOfHeaders;
  std::uint64_t imageBase;
  std::uint32_t numberOfRvaAndSizes;
  std::uint64_t sectionTableOffset;
  std::uint32_t peOffset;
  std::uint32_t machineFileOffset;
  std::uint32_t characteristicsFileOffset;
  std::uint32_t imageBaseFileOffset;
};

static HeaderInfo parseHeaders(const ByteReader &r) {
  if (r.size() < 0x40) {
    failAtFile(RelocPreviewError_Header, r.size(),
               "file too small for a DOS header");
  }
  if (r.u16(0) != MZ_MAGIC) {
    failAtFile(RelocPreviewError_Header, 0, "missing MZ magic");
  }

  std::uint32_t peOffset = r.u32(0x3C);
  if (peOffset < 0x40 ||
      static_cast<std::uint64_t>(peOffset) + 24 > r.size()) {
    failAtFile(RelocPreviewError_Header, 0x3C,
               "e_lfanew does not point at a complete PE file header");
  }

  if (r.u32(peOffset) != kPeSignature) {
    failAtFile(RelocPreviewError_Header, peOffset,
               "missing PE\\0\\0 signature");
  }

  HeaderInfo h;
  h.peOffset = peOffset;
  h.machineFileOffset = peOffset + 4;
  h.characteristicsFileOffset = peOffset + 22;
  h.machine = r.u16(peOffset + 4);
  h.sectionCount = r.u16(peOffset + 6);
  std::uint16_t optionalSize = r.u16(peOffset + 20);
  h.characteristics = r.u16(peOffset + 22);

  std::uint64_t opt = peOffset + 24ULL;
  if (optionalSize < 2 || opt + optionalSize > r.size()) {
    failAtFile(RelocPreviewError_Header, opt,
               "optional header extends past the end of the file");
  }

  std::uint16_t magic = r.u16(opt);
  if (magic == NT_OPTIONAL_32_MAGIC) {
    h.pe32Plus = false;
    if (optionalSize < 104) {
      failAtFile(RelocPreviewError_Header, opt,
                 "PE32 optional header is too small for the base "
                 "relocation directory");
    }
    h.imageBaseFileOffset = static_cast<std::uint32_t>(opt + 28);
    h.imageBase = r.u32(opt + 28);
    h.sizeOfHeaders = r.u32(opt + 60);
    h.numberOfRvaAndSizes = r.u32(opt + 92);
  } else if (magic == NT_OPTIONAL_64_MAGIC) {
    h.pe32Plus = true;
    if (optionalSize < 120) {
      failAtFile(RelocPreviewError_Header, opt,
                 "PE32+ optional header is too small for the base "
                 "relocation directory");
    }
    h.imageBaseFileOffset = static_cast<std::uint32_t>(opt + 24);
    h.imageBase = r.u64(opt + 24);
    h.sizeOfHeaders = r.u32(opt + 60);
    h.numberOfRvaAndSizes = r.u32(opt + 108);
  } else {
    failAtFile(RelocPreviewError_Header, opt,
               "unknown optional header magic (neither PE32 nor PE32+)");
  }

  h.sectionTableOffset = opt + optionalSize;
  std::uint64_t tableEnd = h.sectionTableOffset +
                           static_cast<std::uint64_t>(h.sectionCount) *
                               40ULL;
  if (tableEnd > r.size()) {
    failAtFile(RelocPreviewError_Header, h.sectionTableOffset,
               "section table extends past the end of the file");
  }

  return h;
}

static std::vector<RawSection>
parseSections(const ByteReader &r, const HeaderInfo &h) {
  std::vector<RawSection> sections;
  for (std::uint16_t i = 0; i < h.sectionCount; ++i) {
    std::uint64_t off =
        h.sectionTableOffset + static_cast<std::uint64_t>(i) * 40ULL;
    std::uint32_t virtualSize = r.u32(off + 8);
    std::uint32_t virtualAddress = r.u32(off + 12);
    std::uint32_t rawSize = r.u32(off + 20);
    std::uint32_t rawPointer = r.u32(off + 24);

    RawSection s;
    s.index = i;
    s.virtualAddress = virtualAddress;
    s.rawSize = rawSize;
    s.rawPointer = rawPointer;

    if (rawSize != 0) {
      std::uint64_t rawEnd =
          static_cast<std::uint64_t>(rawPointer) + rawSize;
      if (rawEnd > r.size()) {
        failAtFile(RelocPreviewError_Mapping, rawPointer,
                   "section raw data is truncated by the end of the file");
      }
      std::uint64_t rvaEnd =
          static_cast<std::uint64_t>(virtualAddress) + rawSize;
      if (rvaEnd > std::numeric_limits<std::uint32_t>::max() + 1ULL) {
        failAtRva(RelocPreviewError_Mapping, virtualAddress,
                  "section RVA range wraps past the 32-bit RVA space");
      }
      if (rawPointer < h.sizeOfHeaders) {
        failAtFile(RelocPreviewError_Mapping, rawPointer,
                   "section raw data starts inside the PE headers or has "
                   "no raw data pointer");
      }
    } else if (virtualSize != 0) {
      // Virtual-only sections carry no raw bytes; nothing to validate
      // until an RVA inside one is actually referenced.
    }

    sections.push_back(s);
  }
  return sections;
}

struct DirectoryLocation {
  bool present;
  std::uint32_t rva;
  std::uint32_t size;
};

static DirectoryLocation readDirectory(const ByteReader &r,
                                       const HeaderInfo &h) {
  DirectoryLocation d;
  d.present = false;
  d.rva = 0;
  d.size = 0;

  if (h.numberOfRvaAndSizes <= kBaseRelocDirectory) {
    return d;
  }

  std::uint64_t peOffset = r.u32(0x3C);
  std::uint64_t opt = peOffset + 24ULL;
  std::uint64_t dirOff = opt + (h.pe32Plus ? 112ULL : 96ULL) +
                         kBaseRelocDirectory * 8ULL;

  d.rva = r.u32(dirOff);
  d.size = r.u32(dirOff + 4);

  if (d.rva == 0 && d.size == 0) {
    d.present = false;
    return d;
  }
  if (d.rva == 0 || d.size == 0) {
    failAtFile(RelocPreviewError_Directory, dirOff,
               "base relocation directory has a zero RVA or zero size");
  }
  d.present = true;
  return d;
}

static std::int64_t computeDelta(const HeaderInfo &h,
                                 std::uint64_t newBase) {
  if (!h.pe32Plus &&
      newBase > std::numeric_limits<std::uint32_t>::max()) {
    failAtFile(RelocPreviewError_Base, h.imageBaseFileOffset,
               "new ImageBase does not fit the 32-bit width of a PE32 "
               "image");
  }

  std::int64_t delta;
  if (newBase >= h.imageBase) {
    std::uint64_t positive = newBase - h.imageBase;
    if (positive >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
      failAtFile(RelocPreviewError_Base, h.imageBaseFileOffset,
                 "base difference is too large to represent in int64_t");
    }
    delta = static_cast<std::int64_t>(positive);
  } else {
    std::uint64_t negative = h.imageBase - newBase;
    if (negative > kAbsInt64Min) {
      failAtFile(RelocPreviewError_Base, h.imageBaseFileOffset,
                 "base difference is too small to represent in int64_t");
    }
    if (negative == kAbsInt64Min) {
      delta = std::numeric_limits<std::int64_t>::min();
    } else {
      delta = -static_cast<std::int64_t>(negative);
    }
  }
  return delta;
}

static std::vector<std::uint8_t>
flatten(const std::vector<FragModel> &fragments) {
  std::vector<std::uint8_t> out;
  for (const FragModel &f : fragments) {
    out.insert(out.end(), f.bytes.begin(), f.bytes.end());
  }
  return out;
}

struct PendingBuilder {
  Model *model;
  const SectionMapper *mapper;
  std::map<std::uint32_t, std::uint8_t> overlay;
  std::set<std::uint64_t> directoryFileBytes;
  std::size_t ordinal;

  PendingBuilder() : model(nullptr), mapper(nullptr), ordinal(0) {
  }

  std::vector<std::uint8_t>
  currentField(std::uint32_t rva,
               const std::vector<std::uint8_t> &original) const {
    std::vector<std::uint8_t> out = original;
    for (std::size_t i = 0; i < out.size(); ++i) {
      std::uint32_t byteRva = static_cast<std::uint32_t>(
          static_cast<std::uint64_t>(rva) + i);
      std::map<std::uint32_t, std::uint8_t>::const_iterator it =
          overlay.find(byteRva);
      if (it != overlay.end()) {
        out[i] = it->second;
      }
    }
    return out;
  }

  void writeField(std::uint32_t rva,
                  const std::vector<std::uint8_t> &value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
      std::uint32_t byteRva = static_cast<std::uint32_t>(
          static_cast<std::uint64_t>(rva) + i);
      overlay[byteRva] = value[i];
    }
  }

  void rejectDirectoryOverlap(const std::vector<FragModel> &fragments)
      const {
    for (const FragModel &f : fragments) {
      for (std::uint32_t i = 0; i < f.bytes.size(); ++i) {
        std::uint64_t off =
            static_cast<std::uint64_t>(f.fileOffset) + i;
        if (directoryFileBytes.count(off) != 0) {
          failAtFile(RelocPreviewError_Overlap, off,
                     "relocation target write interval intersects the "
                     "relocation directory's own bytes");
        }
      }
    }
  }
};

static void buildIntervals(PendingBuilder &pb) {
  if (pb.overlay.empty()) {
    return;
  }

  std::vector<std::uint32_t> runStart;
  std::vector<std::uint32_t> runEnd; // inclusive
  for (std::map<std::uint32_t, std::uint8_t>::const_iterator it =
           pb.overlay.begin();
       it != pb.overlay.end(); ++it) {
    std::uint32_t rva = it->first;
    if (!runStart.empty() && rva == runEnd.back() + 1) {
      runEnd.back() = rva;
    } else {
      runStart.push_back(rva);
      runEnd.push_back(rva);
    }
  }

  for (std::size_t i = 0; i < runStart.size(); ++i) {
    std::uint32_t first = runStart[i];
    std::uint32_t length = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(runEnd[i]) - first + 1);

    IntervalModel interval;
    interval.firstRva = first;
    interval.lastRva = runEnd[i];
    interval.original = flatten(pb.mapper->mapRange(first, length));
    interval.finalBytes.resize(length);
    for (std::uint32_t k = 0; k < length; ++k) {
      std::uint32_t rva = static_cast<std::uint32_t>(
          static_cast<std::uint64_t>(first) + k);
      interval.finalBytes[k] = pb.overlay.find(rva)->second;
    }

    for (const BlockModel &block : pb.model->blocks) {
      for (const OpModel &op : block.operations) {
        if (op.width == RelocPreviewWidth_None) {
          continue;
        }
        std::uint32_t opEnd = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(op.targetRva) +
            static_cast<std::uint32_t>(op.width) - 1);
        bool intersects = !(opEnd < first || op.targetRva > runEnd[i]);
        if (intersects) {
          interval.ordinals.push_back(op.ordinal);
        }
      }
    }

    pb.model->intervals.push_back(std::move(interval));
  }
}

static void processEntry(
    const SectionMapper &mapper, std::uint32_t pageRva,
    std::uint16_t rawEntry, std::size_t entryIndex, bool pe32Plus,
    std::int64_t delta, std::uint32_t pairRva,
    const std::vector<FragModel> &entryFragments, PendingBuilder &pb) {
  std::uint8_t type = static_cast<std::uint8_t>(rawEntry >> 12);
  std::uint16_t offset = static_cast<std::uint16_t>(rawEntry & 0x0FFF);
  std::uint32_t targetRva = static_cast<std::uint32_t>(
      static_cast<std::uint64_t>(pageRva) + offset);

  OpModel op;
  op.ordinal = pb.ordinal++;
  op.entryIndex = entryIndex;
  op.rawEntry = rawEntry;
  op.sectionIndex = entryFragments.front().sectionIndex;
  op.entryFileOffset = entryFragments.front().fileOffset;
  op.entryFragments = entryFragments;
  op.targetRva = targetRva;

  bool supported = pe32Plus ? (type == 0 || type == 10) : (type <= 4);
  if (!supported) {
    failAtFile(RelocPreviewError_Entry, op.entryFileOffset,
               "unknown relocation type or type unsupported for this "
               "machine");
  }

  if (type == 0) {
    op.kind = RelocPreviewKind_Absolute;
    op.width = RelocPreviewWidth_None;
    pb.model->blocks.back().operations.push_back(std::move(op));
    return;
  }

  switch (type) {
  case 1:
    op.kind = RelocPreviewKind_High;
    op.width = RelocPreviewWidth_Word16;
    break;
  case 2:
    op.kind = RelocPreviewKind_Low;
    op.width = RelocPreviewWidth_Word16;
    break;
  case 3:
    op.kind = RelocPreviewKind_HighLow;
    op.width = RelocPreviewWidth_Dword32;
    break;
  case 4:
    op.kind = RelocPreviewKind_HighAdj;
    op.width = RelocPreviewWidth_Word16;
    break;
  case 10:
    op.kind = RelocPreviewKind_Dir64;
    op.width = RelocPreviewWidth_Qword64;
    break;
  default:
    failAtFile(RelocPreviewError_Machine, op.entryFileOffset,
               "relocation type does not match the image machine");
    break;
  }

  std::uint32_t fieldWidth = static_cast<std::uint32_t>(op.width);
  if (static_cast<std::uint64_t>(targetRva) + fieldWidth >
      std::numeric_limits<std::uint32_t>::max() + 1ULL) {
    failAtRva(RelocPreviewError_Mapping, targetRva,
              "relocation target field wraps past the RVA space");
  }

  std::vector<FragModel> targetFragments =
      mapper.mapRange(targetRva, fieldWidth);
  pb.rejectDirectoryOverlap(targetFragments);
  op.targetFragments = targetFragments;

  std::vector<std::uint8_t> original = flatten(targetFragments);
  std::vector<std::uint8_t> before =
      pb.currentField(targetRva, original);
  std::vector<std::uint8_t> after = before;

  std::uint32_t delta32 =
      static_cast<std::uint32_t>(static_cast<std::uint64_t>(delta));

  switch (type) {
  case 1: { // HIGH
    std::uint16_t h = le16(before, 0);
    std::uint16_t deltaHi = static_cast<std::uint16_t>(delta32 >> 16);
    putLe16(after, 0, addMod16(h, deltaHi));
    break;
  }
  case 2: { // LOW
    std::uint16_t l = le16(before, 0);
    std::uint16_t deltaLo =
        static_cast<std::uint16_t>(delta32 & 0xFFFF);
    putLe16(after, 0, addMod16(l, deltaLo));
    break;
  }
  case 3: { // HIGHLOW
    std::uint32_t v = le32(before, 0);
    putLe32(after, 0, addMod32(v, delta));
    break;
  }
  case 4: { // HIGHADJ
    // The pair word is the next raw 16-bit word in this same block; its
    // high nibble is data, never another relocation type.
    std::vector<FragModel> pairFragments =
        mapper.mapRange(pairRva, 2);
    std::uint16_t pairRaw = le16(flatten(pairFragments), 0);
    std::int16_t pairSigned = static_cast<std::int16_t>(pairRaw);

    op.hasPair = true;
    op.pairSectionIndex = pairFragments.front().sectionIndex;
    op.pairFileOffset = pairFragments.front().fileOffset;
    op.pairRawValue = pairRaw;
    op.pairFragments = std::move(pairFragments);

    std::uint16_t h = le16(before, 0);
    putLe16(after, 0, highAdjResult(h, pairSigned, delta, targetRva));
    break;
  }
  case 10: { // DIR64
    std::uint64_t v = le64(before, 0);
    putLe64(after, 0, addMod64(v, delta));
    break;
  }
  default:
    break;
  }

  op.before = before;
  op.after = after;
  pb.writeField(targetRva, after);

  pb.model->blocks.back().operations.push_back(std::move(op));
}

// --- Freezing the internal STL model into the public owning arrays. ------

static std::uint8_t *freezeBytes(const std::vector<std::uint8_t> &v) {
  std::unique_ptr<std::uint8_t[]> p(new std::uint8_t[v.size()]);
  if (!v.empty()) {
    std::memcpy(p.get(), v.data(), v.size());
  }
  return p.release();
}

static RelocPreviewFragment *
freezeFragments(const std::vector<FragModel> &src) {
  std::unique_ptr<RelocPreviewFragment[]> arr(
      new RelocPreviewFragment[src.size()]);
  for (std::size_t i = 0; i < src.size(); ++i) {
    arr[i].sectionIndex = src[i].sectionIndex;
    arr[i].fileOffset = src[i].fileOffset;
    arr[i].byteCount =
        static_cast<std::uint32_t>(src[i].bytes.size());
    arr[i].bytes = freezeBytes(src[i].bytes);
  }
  return arr.release();
}

static RelocPreviewOperation *freezeOperations(
    const std::vector<OpModel> &src) {
  std::unique_ptr<RelocPreviewOperation[]> arr(
      new RelocPreviewOperation[src.size()]);
  for (std::size_t i = 0; i < src.size(); ++i) {
    const OpModel &s = src[i];
    RelocPreviewOperation &d = arr[i];
    d.ordinal = s.ordinal;
    d.entryIndex = s.entryIndex;
    d.kind = s.kind;
    d.width = s.width;
    d.sectionIndex = s.sectionIndex;
    d.entryFileOffset = s.entryFileOffset;
    d.rawEntry = s.rawEntry;
    d.entryFragmentCount =
        static_cast<std::uint32_t>(s.entryFragments.size());
    d.entryFragments = freezeFragments(s.entryFragments);
    d.targetRva = s.targetRva;
    d.beforeByteCount = static_cast<std::uint32_t>(s.before.size());
    d.beforeBytes = freezeBytes(s.before);
    d.afterByteCount = static_cast<std::uint32_t>(s.after.size());
    d.afterBytes = freezeBytes(s.after);
    d.targetFragmentCount =
        static_cast<std::uint32_t>(s.targetFragments.size());
    d.targetFragments = freezeFragments(s.targetFragments);
    d.hasPair = s.hasPair;
    d.pairSectionIndex = s.pairSectionIndex;
    d.pairFileOffset = s.pairFileOffset;
    d.pairRawValue = s.pairRawValue;
    d.pairFragmentCount =
        static_cast<std::uint32_t>(s.pairFragments.size());
    d.pairFragments = freezeFragments(s.pairFragments);
  }
  return arr.release();
}

static RelocPreviewBlock *
freezeBlocks(const std::vector<BlockModel> &src) {
  std::unique_ptr<RelocPreviewBlock[]> arr(
      new RelocPreviewBlock[src.size()]);
  for (std::size_t i = 0; i < src.size(); ++i) {
    const BlockModel &s = src[i];
    RelocPreviewBlock &d = arr[i];
    d.pageRva = s.pageRva;
    d.blockSize = s.blockSize;
    d.directoryOffset = s.directoryOffset;
    d.headerFileOffset = s.headerFileOffset;
    d.headerFragmentCount =
        static_cast<std::uint32_t>(s.headerFragments.size());
    d.headerFragments = freezeFragments(s.headerFragments);
    d.operationCount =
        static_cast<std::uint32_t>(s.operations.size());
    d.operations = freezeOperations(s.operations);
  }
  return arr.release();
}

static RelocPreviewInterval *
freezeIntervals(const std::vector<IntervalModel> &src) {
  std::unique_ptr<RelocPreviewInterval[]> arr(
      new RelocPreviewInterval[src.size()]);
  for (std::size_t i = 0; i < src.size(); ++i) {
    const IntervalModel &s = src[i];
    RelocPreviewInterval &d = arr[i];
    d.firstRva = s.firstRva;
    d.lastRva = s.lastRva;
    d.originalByteCount =
        static_cast<std::uint32_t>(s.original.size());
    d.originalBytes = freezeBytes(s.original);
    d.finalByteCount =
        static_cast<std::uint32_t>(s.finalBytes.size());
    d.finalBytes = freezeBytes(s.finalBytes);
    d.operationOrdinalCount =
        static_cast<std::uint32_t>(s.ordinals.size());
    std::unique_ptr<std::size_t[]> ord(
        new std::size_t[s.ordinals.size()]);
    for (std::size_t k = 0; k < s.ordinals.size(); ++k) {
      ord[k] = s.ordinals[k];
    }
    d.operationOrdinals = ord.release();
  }
  return arr.release();
}

static RelocPreviewReport *freezeReport(const Model &m) {
  std::unique_ptr<RelocPreviewReport> report(new RelocPreviewReport());
  report->pe32Plus = m.pe32Plus;
  report->oldImageBase = m.oldImageBase;
  report->newImageBase = m.newImageBase;
  report->baseDelta = m.baseDelta;
  report->directoryPresent = m.directoryPresent;
  report->directoryRva = m.directoryRva;
  report->directorySize = m.directorySize;
  report->blockCount = static_cast<std::uint32_t>(m.blocks.size());
  report->blocks = freezeBlocks(m.blocks);
  report->intervalCount =
      static_cast<std::uint32_t>(m.intervals.size());
  report->intervals = freezeIntervals(m.intervals);
  return report.release();
}

static RelocPreviewReport *
buildReport(const std::uint8_t *data, std::size_t size,
            std::uint64_t newImageBase) {
  if (data == nullptr) {
    failAt(RelocPreviewError_Header, "null input buffer");
  }

  Model model;
  ByteReader reader(data, size);
  HeaderInfo header = parseHeaders(reader);

  if ((header.characteristics & IMAGE_FILE_BYTES_REVERSED_LO) != 0 ||
      (header.characteristics & IMAGE_FILE_BYTES_REVERSED_HI) != 0) {
    failAtFile(RelocPreviewError_Header,
               header.characteristicsFileOffset,
               "byte-reversed PE images are not supported");
  }

  if (header.pe32Plus) {
    if (header.machine != IMAGE_FILE_MACHINE_AMD64) {
      failAtFile(RelocPreviewError_Machine, header.machineFileOffset,
                 "PE32+ images are only supported for "
                 "IMAGE_FILE_MACHINE_AMD64");
    }
  } else if (header.machine != IMAGE_FILE_MACHINE_I386) {
    failAtFile(RelocPreviewError_Machine, header.machineFileOffset,
               "PE32 images are only supported for "
               "IMAGE_FILE_MACHINE_I386");
  }

  std::int64_t delta = computeDelta(header, newImageBase);

  if ((header.characteristics & IMAGE_FILE_RELOCS_STRIPPED) != 0 &&
      delta != 0) {
    failAtFile(RelocPreviewError_RelocsStripped,
               header.characteristicsFileOffset,
               "IMAGE_FILE_RELOCS_STRIPPED is set but a non-zero base "
               "move was requested");
  }

  std::vector<RawSection> rawSections = parseSections(reader, header);
  SectionMapper mapper(reader, std::move(rawSections),
                       header.sizeOfHeaders);

  DirectoryLocation directory = readDirectory(reader, header);

  model.pe32Plus = header.pe32Plus;
  model.oldImageBase = header.imageBase;
  model.newImageBase = newImageBase;
  model.baseDelta = delta;
  model.directoryPresent = directory.present;
  model.directoryRva = directory.rva;
  model.directorySize = directory.size;

  if (!directory.present) {
    // A missing directory with a requested move yields an empty report;
    // no pointer-like values are guessed from raw bytes.
    return freezeReport(model);
  }

  std::uint64_t dirEnd =
      static_cast<std::uint64_t>(directory.rva) + directory.size;
  if (dirEnd > std::numeric_limits<std::uint32_t>::max() + 1ULL) {
    failAtRva(RelocPreviewError_Directory, directory.rva,
              "relocation directory wraps past the RVA space");
  }

  // Resolve the whole directory up front: every directory byte must have
  // a unique, raw-backed location, possibly across section boundaries.
  std::vector<FragModel> directoryFragments =
      mapper.mapRange(directory.rva, directory.size);
  PendingBuilder pb;
  pb.model = &model;
  pb.mapper = &mapper;
  for (const FragModel &f : directoryFragments) {
    for (std::uint32_t i = 0; i < f.bytes.size(); ++i) {
      pb.directoryFileBytes.insert(
          static_cast<std::uint64_t>(f.fileOffset) + i);
    }
  }

  std::uint32_t cursor = directory.rva;
  std::uint32_t remaining = directory.size;
  std::uint32_t directoryOffset = 0;

  while (remaining != 0) {
    if (remaining < 8) {
      failAtRva(RelocPreviewError_Block, cursor,
                "trailing relocation directory bytes cannot form a block "
                "(less than 8 bytes remain)");
    }
    if ((cursor & 0x3U) != 0) {
      failAtRva(RelocPreviewError_Block, cursor,
                "relocation block start is not 4-byte aligned");
    }

    std::vector<FragModel> headerFragments =
        mapper.mapRange(cursor, 8);
    std::vector<std::uint8_t> headerBytes = flatten(headerFragments);
    std::uint32_t pageRva = le32(headerBytes, 0);
    std::uint32_t blockSize = le32(headerBytes, 4);

    if (blockSize < 8) {
      failAtRva(RelocPreviewError_Block, cursor,
                "relocation block size is smaller than its 8-byte header");
    }
    if ((blockSize & 0x1U) != 0) {
      failAtRva(RelocPreviewError_Block, cursor,
                "relocation block size does not contain whole 16-bit "
                "entries");
    }
    if (blockSize > remaining) {
      failAtRva(RelocPreviewError_Block, cursor,
                "relocation block extends past the directory size");
    }

    BlockModel block;
    block.pageRva = pageRva;
    block.blockSize = blockSize;
    block.directoryOffset = directoryOffset;
    block.headerFileOffset = headerFragments.front().fileOffset;
    block.headerFragments = std::move(headerFragments);
    model.blocks.push_back(std::move(block));

    std::uint32_t entryWords = (blockSize - 8) / 2;
    std::uint32_t entryCursor = cursor + 8;

    // The HIGHADJ pair word's RVA; the pair is consumed instead of being
    // decoded as an entry.
    std::uint32_t pendingPairRva = 0;
    bool havePendingPair = false;

    for (std::uint32_t e = 0; e < entryWords; ++e) {
      std::uint32_t entryRva = static_cast<std::uint32_t>(
          static_cast<std::uint64_t>(entryCursor) +
          static_cast<std::uint64_t>(e) * 2ULL);
      std::vector<FragModel> entryFragments =
          mapper.mapRange(entryRva, 2);
      std::uint16_t raw = le16(flatten(entryFragments), 0);

      if (havePendingPair) {
        // This word belongs to the previous HIGHADJ; skip it as an entry
        // without ever examining its high nibble as a type.
        havePendingPair = false;
        continue;
      }

      std::uint8_t type = static_cast<std::uint8_t>(raw >> 12);

      if (type == 4) {
        if (e + 1 >= entryWords) {
          failAtFile(RelocPreviewError_Entry,
                     entryFragments.front().fileOffset,
                     "HIGHADJ relocation is missing its paired 16-bit "
                     "word in the same block");
        }
        pendingPairRva = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(entryRva) + 2);
        havePendingPair = true;
      }

      processEntry(mapper, pageRva, raw, e, header.pe32Plus, delta,
                   pendingPairRva, entryFragments, pb);
    }

    if (havePendingPair) {
      failAtRva(RelocPreviewError_Entry, entryCursor,
                "HIGHADJ pair word was not consumed");
    }

    cursor = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(cursor) + blockSize);
    remaining -= blockSize;
    directoryOffset += blockSize;
  }

  buildIntervals(pb);
  return freezeReport(model);
}

} // namespace

RelocPreviewResult
BuildBaseRelocationPreview(const std::uint8_t *data, std::size_t size,
                           std::uint64_t newImageBase) {
  RelocPreviewResult result;
  std::memset(&result, 0, sizeof(result));
  try {
    result.report = buildReport(data, size, newImageBase);
    result.ok = true;
  } catch (PreviewFailure &f) {
    result.ok = false;
    result.report = nullptr;
    result.error.category = f.category;
    result.error.hasFileOffset = f.hasFileOffset;
    result.error.fileOffset = f.fileOffset;
    result.error.hasRva = f.hasRva;
    result.error.rva = f.rva;
    std::strncpy(result.error.message, f.message.c_str(),
                 sizeof(result.error.message) - 1);
    result.error.message[sizeof(result.error.message) - 1] = '\0';
  } catch (const std::bad_alloc &) {
    result.ok = false;
    result.report = nullptr;
    result.error.category = RelocPreviewError_Memory;
    static const char kMsg[] =
        "failed to allocate the relocation preview report";
    std::strncpy(result.error.message, kMsg,
                 sizeof(result.error.message) - 1);
    result.error.message[sizeof(result.error.message) - 1] = '\0';
  }
  return result;
}

void DestroyBaseRelocationPreview(RelocPreviewReport *report) {
  delete report;
}

} // namespace peparse
