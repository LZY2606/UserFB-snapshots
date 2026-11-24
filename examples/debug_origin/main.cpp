// debug_origin: report where every debug directory record of the given
// PE files points, which bytes each candidate contributes, and which
// records share a debug identity across the inputs.
//
// Output follows task_support/report-format.md: TAB-separated R/C/G
// rows on stdout, diagnostics on stderr, non-zero exit on any damaged
// input. All structure comes from pe-parse (ParsePEFromFile +
// ReadDebugRecords); this file contains no PE parsing of its own.

#include <pe-parse/debug_records.h>
#include <pe-parse/parse.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

std::string baseName(const std::string &path) {
  auto pos = path.find_last_of("/\\");
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string hexByte(std::uint8_t b) {
  const char *digits = "0123456789abcdef";
  std::string s;
  s += digits[b >> 4];
  s += digits[b & 0xf];
  return s;
}

template <typename It>
std::string hexBytes(It begin, It end) {
  if (begin == end) {
    return "-";
  }
  std::string s;
  for (It it = begin; it != end; ++it) {
    s += hexByte(static_cast<std::uint8_t>(*it));
  }
  return s;
}

std::string hexU64(std::uint64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
  return buf;
}

std::string hex32(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08x", v);
  return buf;
}

const char *relationName(peparse::debug_relation rel) {
  switch (rel) {
    case peparse::debug_relation::none:
      return "none";
    case peparse::debug_relation::file_only:
      return "file_only";
    case peparse::debug_relation::rva_only:
      return "rva_only";
    case peparse::debug_relation::same:
      return "same";
    case peparse::debug_relation::equal_distinct:
      return "equal_distinct";
    case peparse::debug_relation::conflict:
      return "conflict";
  }
  return "none";
}

std::string ownerName(const peparse::debug_payload_candidate &cand) {
  switch (cand.owner) {
    case peparse::debug_owner_kind::headers:
      return "headers";
    case peparse::debug_owner_kind::section:
      return "section:" + std::to_string(cand.owner_section);
    case peparse::debug_owner_kind::overlay:
      return "overlay";
    case peparse::debug_owner_kind::gap:
      return "gap";
  }
  return "gap";
}

const char *formatName(peparse::codeview_format format) {
  switch (format) {
    case peparse::codeview_format::rsds:
      return "RSDS";
    case peparse::codeview_format::nb10:
      return "NB10";
    case peparse::codeview_format::opaque:
      return "opaque";
  }
  return "opaque";
}

// Display key for an identity: format kind plus every identity field,
// never the path. The library keeps the raw fields; this is only a
// rendering for grouping.
std::string identityKey(const peparse::codeview_record &cv) {
  switch (cv.format) {
    case peparse::codeview_format::rsds:
      return "RSDS:" + cv.guid_text + ":" + hex32(cv.age);
    case peparse::codeview_format::nb10:
      return "NB10:" + hex32(cv.offset) + ":" + hex32(cv.signature) + ":" +
             hex32(cv.age);
    case peparse::codeview_format::opaque:
      return "-";
  }
  return "-";
}

struct Member {
  std::string identity;
  std::size_t inputOrder;
  std::string sample;
  std::uint32_t ordinal;
};

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: " << argv[0] << " <pe-file> [pe-file ...]\n";
    return 2;
  }

  std::vector<std::string> inputs;
  std::set<std::string> names;
  for (int i = 1; i < argc; i++) {
    std::string path = argv[i];
    std::string name = baseName(path);
    if (name.empty()) {
      std::cerr << "error: input " << path << " has an empty basename\n";
      return 1;
    }
    if (!names.insert(name).second) {
      std::cerr << "error: duplicate input basename " << name << "\n";
      return 1;
    }
    inputs.push_back(path);
  }

  std::vector<Member> members;

  for (std::size_t order = 0; order < inputs.size(); order++) {
    const std::string &path = inputs[order];
    const std::string sample = baseName(path);

    peparse::parsed_pe *pe = peparse::ParsePEFromFile(path.c_str());
    if (pe == nullptr) {
      std::cerr << "error: " << path << ": cannot parse PE: "
                << peparse::GetPEErrString() << "\n";
      return 1;
    }

    peparse::debug_records records;
    bool ok = peparse::ReadDebugRecords(pe, records);
    if (!ok) {
      std::cerr << "error: " << path << ": debug directory unreadable";
      if (records.error_entry_file_offset != 0xffffffffU) {
        std::cerr << " at entry file offset "
                  << hexU64(records.error_entry_file_offset);
      }
      std::cerr << ": " << records.error_message << "\n";
      peparse::DestructParsedPE(pe);
      return 1;
    }

    for (const auto &rec : records.records) {
      std::cout << "R\t" << sample << "\t" << rec.ordinal << "\t"
                << hexU64(rec.entry_file_offset) << "\t"
                << rec.directory_section_index << "\t"
                << hexBytes(rec.raw_entry.begin(), rec.raw_entry.end()) << "\t"
                << relationName(rec.relation) << "\n";

      for (const auto &cand : rec.candidates) {
        const peparse::codeview_record &cv = cand.codeview;
        std::cout << "C\t" << sample << "\t" << rec.ordinal << "\t"
                  << (cand.source == peparse::debug_candidate_source::file_pointer
                          ? "file"
                          : "rva")
                  << "\t" << hexU64(cand.file_offset) << "\t"
                  << ownerName(cand) << "\t"
                  << hexBytes(cand.bytes.begin(), cand.bytes.end()) << "\t"
                  << formatName(cv.format) << "\t" << identityKey(cv) << "\t"
                  << hexBytes(cv.path.begin(), cv.path.end()) << "\n";
      }

      // Identity groups only take records that are identified and free
      // of source conflicts; conflicting records stay visible as
      // candidates only.
      if (rec.relation == peparse::debug_relation::conflict ||
          rec.candidates.empty()) {
        continue;
      }
      const peparse::codeview_record &cv = rec.candidates.front().codeview;
      if (cv.format == peparse::codeview_format::opaque) {
        continue;
      }
      members.push_back(Member{identityKey(cv), order, sample, rec.ordinal});
    }

    peparse::DestructParsedPE(pe);
  }

  std::stable_sort(members.begin(),
                   members.end(),
                   [](const Member &a, const Member &b) {
                     if (a.identity != b.identity) {
                       return a.identity < b.identity;
                     }
                     if (a.inputOrder != b.inputOrder) {
                       return a.inputOrder < b.inputOrder;
                     }
                     return a.ordinal < b.ordinal;
                   });

  for (const Member &m : members) {
    std::cout << "G\t" << m.identity << "\t" << m.sample << "\t" << m.ordinal
              << "\n";
  }

  return 0;
}
