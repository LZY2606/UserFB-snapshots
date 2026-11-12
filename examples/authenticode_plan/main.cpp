/*
 * examples/authenticode_plan/main.cpp
 *
 * Minimal demonstration of BuildAuthenticodePlan / FeedAuthenticodePlan.
 *
 * The library only describes and streams the bytes that enter the
 * Authenticode image hash; it never hashes or verifies a signature. To keep
 * this example dependency-free the callback below accumulates a non
 * cryptographic FNV-1a value purely to show how digest input is delivered.
 * A real caller would feed the same (offset, data, length) calls into its
 * own SHA-256 implementation and then validate the signature separately.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <pe-parse/authenticode.h>
#include <pe-parse/parse.h>

namespace {

const char *stageName(peparse::authenticode_span_stage stage) {
  switch (stage) {
    case peparse::AUTH_SPAN_HEADER:
      return "header";
    case peparse::AUTH_SPAN_SECTION:
      return "section";
    case peparse::AUTH_SPAN_TRAILING:
      return "trailing";
    default:
      return "unknown";
  }
}

struct Fnv1a {
  std::uint64_t state;
  std::uint64_t bytesFed;
};

int feedFnv(void *context,
            std::uint64_t offset,
            const std::uint8_t *data,
            std::uint64_t length,
            peparse::authenticode_span_stage stage,
            int sectionIndex) {
  Fnv1a *fnv = static_cast<Fnv1a *>(context);
  for (std::uint64_t i = 0; i < length; ++i) {
    fnv->state ^= static_cast<std::uint64_t>(data[i]);
    fnv->state *= 1099511628211ULL;
  }
  fnv->bytesFed += length;

  std::printf(
      "feed %-8s file_off=0x%-8llx len=0x%-8llx section=%d\n",
      stageName(stage),
      static_cast<unsigned long long>(offset),
      static_cast<unsigned long long>(length),
      sectionIndex);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <pe-file>\n", argv[0]);
    return 2;
  }

  peparse::parsed_pe *pe = peparse::ParsePEFromFile(argv[1]);
  if (pe == nullptr) {
    std::fprintf(stderr,
                 "ParsePEFromFile failed: %s (%s)\n",
                 peparse::GetPEErrString().c_str(),
                 peparse::GetPEErrLoc().c_str());
    return 1;
  }

  peparse::authenticode_plan plan;
  const peparse::authenticode_plan_status status =
      peparse::BuildAuthenticodePlan(pe, plan);
  if (status != peparse::AUTH_PLAN_OK) {
    std::fprintf(stderr,
                 "BuildAuthenticodePlan failed: %s\n",
                 peparse::GetAuthenticodePlanError().c_str());
    peparse::DestructParsedPE(pe);
    return 1;
  }

  // The plan is self-contained: the parsed PE can be destroyed before the
  // plan is inspected or fed.
  peparse::DestructParsedPE(pe);

  std::printf("file_size            = 0x%llx\n",
              static_cast<unsigned long long>(plan.file_size));
  std::printf("size_of_headers      = 0x%x\n", plan.size_of_headers);
  std::printf("sum_of_bytes_hashed  = 0x%llx\n",
              static_cast<unsigned long long>(plan.sum_of_bytes_hashed));
  std::printf("hash spans           = %u\n",
              static_cast<unsigned>(plan.spans.size()));
  std::printf("excluded fields      = %u\n",
              static_cast<unsigned>(plan.excluded.size()));
  std::printf("certificate entries  = %u\n",
              static_cast<unsigned>(plan.certificates.size()));

  for (std::size_t i = 0; i < plan.certificates.size(); ++i) {
    const peparse::authenticode_certificate_entry &entry =
        plan.certificates[i];
    std::printf(
        "  cert[%u] rev=0x%04x type=0x%04x entry=[0x%llx,+0x%llx) "
        "payload=[0x%llx,+0x%llx) storage=[0x%llx,+0x%llx)\n",
        static_cast<unsigned>(i),
        entry.revision,
        entry.type,
        static_cast<unsigned long long>(entry.entry_offset),
        static_cast<unsigned long long>(entry.entry_length),
        static_cast<unsigned long long>(entry.payload_offset),
        static_cast<unsigned long long>(entry.payload_length),
        static_cast<unsigned long long>(entry.storage_offset),
        static_cast<unsigned long long>(entry.storage_length));
  }

  // Re-open the raw bytes so the plan can be fed against the same file.
  // FeedAuthenticodePlan verifies the layout before invoking the callback.
  FILE *file = std::fopen(argv[1], "rb");
  if (file == nullptr) {
    std::fprintf(stderr, "cannot reopen %s\n", argv[1]);
    return 1;
  }
  std::fseek(file, 0, SEEK_END);
  const long fileSize = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize));
  if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
    std::fprintf(stderr, "short read\n");
    std::fclose(file);
    return 1;
  }
  std::fclose(file);

  Fnv1a fnv;
  fnv.state = 14695981039346656037ULL;
  fnv.bytesFed = 0;
  const peparse::authenticode_plan_status feedStatus =
      peparse::FeedAuthenticodePlan(
          plan, bytes.data(), bytes.size(), feedFnv, &fnv);
  if (feedStatus != peparse::AUTH_PLAN_OK) {
    std::fprintf(stderr,
                 "FeedAuthenticodePlan failed: %s\n",
                 peparse::GetAuthenticodePlanError().c_str());
    return 1;
  }

  std::printf("bytes fed (with ordered duplicates) = 0x%llx\n",
              static_cast<unsigned long long>(fnv.bytesFed));
  std::printf("illustrative FNV-1a (not a signature) = 0x%016llx\n",
              static_cast<unsigned long long>(fnv.state));
  return 0;
}
