// Brief example: read a UEFI TE image without reconstructing a PE.
//
// Build (from the repository root):
//   c++ -std=c++17 -Ipe-parser-library/include \
//       pe-parser-library/src/buffer.cpp pe-parser-library/src/parse.cpp \
//       pe-parser-library/src/unicode_codecvt.cpp examples/te_example.cpp \
//       -o te_example
//
// Run:
//   ./te_example path/to/image.te

#include <pe-parse/parse.h>

#include <cstdio>

namespace {

int onSection(void *cbd,
              const peparse::VA &va,
              const std::string &name,
              const peparse::image_section_header &hdr,
              const peparse::bounded_buffer *) {
  int *count = static_cast<int *>(cbd);
  std::printf("  section %-8s VA=%llx raw=%x size=%x\n",
              name.c_str(),
              static_cast<unsigned long long>(va),
              hdr.PointerToRawData,
              hdr.SizeOfRawData);
  ++*count;
  return 0;
}

int onReloc(void *cbd, const peparse::VA &va, const peparse::reloc_type &type) {
  int *count = static_cast<int *>(cbd);
  std::printf("  reloc   VA=%llx type=%u\n",
              static_cast<unsigned long long>(va),
              static_cast<unsigned>(type));
  ++*count;
  return 0;
}

int onDebug(void *cbd, const std::uint32_t &type, const peparse::bounded_buffer *data) {
  int *count = static_cast<int *>(cbd);
  std::printf("  debug   type=%u bytes=%llu\n",
              type,
              static_cast<unsigned long long>(data == nullptr ? 0 : data->bufLen));
  ++*count;
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <image.te>\n", argv[0]);
    return 2;
  }

  peparse::parsed_pe *pe = peparse::ParsePEFromFile(argv[1]);
  if (pe == nullptr) {
    std::fprintf(stderr, "parse failed: %s\n", peparse::GetPEErrString().c_str());
    return 1;
  }

  peparse::te_header te;
  if (!peparse::GetTEHeader(pe, te)) {
    std::fprintf(stderr, "%s is a PE image, not a TE image\n", argv[1]);
    peparse::DestructParsedPE(pe);
    return 1;
  }

  std::printf("TE machine=%s sections=%u subsystem=%s stripped=%x\n",
              peparse::GetMachineAsString(pe),
              te.NumberOfSections,
              peparse::GetSubsystemAsString(pe),
              te.StrippedSize);

  peparse::VA entry = 0;
  if (peparse::GetEntryPoint(pe, entry)) {
    std::printf("  entry   VA=%llx\n", static_cast<unsigned long long>(entry));
  }

  int sections = 0;
  peparse::IterSec(pe, onSection, &sections);

  int relocs = 0;
  peparse::IterRelocs(pe, onReloc, &relocs);

  int debugs = 0;
  peparse::IterDebugs(pe, onDebug, &debugs);

  std::printf("sections=%d relocs=%d debugs=%d\n", sections, relocs, debugs);
  peparse::DestructParsedPE(pe);
  return 0;
}
