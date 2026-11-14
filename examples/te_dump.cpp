// Minimal example: read a UEFI TE image and walk its entry point, sections,
// relocations and debug records through the ordinary pe-parse API.
//
// Build (from the repository root):
//   c++ -std=c++17 -Ipe-parser-library/include \
//       pe-parser-library/src/buffer.cpp pe-parser-library/src/parse.cpp \
//       pe-parser-library/src/unicode_codecvt.cpp examples/te_dump.cpp \
//       -o te_dump
//   ./te_dump image.te

#include <pe-parse/parse.h>

#include <cstdio>

static int onSection(void *,
                     const peparse::VA &base,
                     const std::string &name,
                     const peparse::image_section_header &hdr,
                     const peparse::bounded_buffer *) {
  std::printf("  section %-8s RVA=0x%08x base=0x%llx raw=0x%08x size=0x%x\n",
              name.c_str(),
              hdr.VirtualAddress,
              static_cast<unsigned long long>(base),
              hdr.PointerToRawData,
              hdr.SizeOfRawData);
  return 0;
}

static int onReloc(void *, const peparse::VA &va,
                   const peparse::reloc_type &type) {
  std::printf("  reloc VA=0x%llx type=%d\n",
              static_cast<unsigned long long>(va),
              static_cast<int>(type));
  return 0;
}

static int onDebug(void *,
                   const std::uint32_t &type,
                   const peparse::bounded_buffer *data) {
  std::printf("  debug type=%u size=%llu\n", type,
              static_cast<unsigned long long>(data != nullptr ? data->bufLen
                                                              : 0));
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <image.te>\n", argv[0]);
    return 2;
  }

  peparse::parsed_pe *pe = peparse::ParsePEFromFile(argv[1]);
  if (pe == nullptr) {
    std::fprintf(stderr, "parse failed: %s (%s)\n",
                 peparse::GetPEErrString().c_str(),
                 peparse::GetPEErrLoc().c_str());
    return 1;
  }

  peparse::te_header te{};
  if (peparse::GetTEHeader(pe, te)) {
    std::printf("TE machine=0x%04x sections=%u stripped=0x%x "
                "entry=0x%llx imageBase=0x%llx\n",
                te.Machine, te.NumberOfSections, te.StrippedSize,
                static_cast<unsigned long long>(te.AddressOfEntryPoint),
                static_cast<unsigned long long>(te.ImageBase));
  } else {
    std::printf("regular PE machine=%s\n", peparse::GetMachineAsString(pe));
  }

  peparse::VA entry = 0;
  if (peparse::GetEntryPoint(pe, entry)) {
    std::printf("entry VA=0x%llx\n", static_cast<unsigned long long>(entry));
  }

  peparse::IterSec(pe, onSection, nullptr);
  peparse::IterRelocs(pe, onReloc, nullptr);
  peparse::IterDebugs(pe, onDebug, nullptr);

  peparse::DestructParsedPE(pe);
  return 0;
}
