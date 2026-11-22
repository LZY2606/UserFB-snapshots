// Validates sequence_header()/picture_header() parsing (mpg_headers.h)
// against a real ffmpeg-encoded stream, cross-checked via ffprobe - see
// docs/testing.md and docs/preparing-input-with-ffmpeg.md for how
// assets/small.m1v was produced (176x144, -g 0 -bf 0, I-pictures only).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "mpg_bitreader.h"
#include "mpg_headers.h"

using namespace tinympg;

static std::vector<uint8_t> readFile(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "cannot open %s (run from test/native/, see docs/testing.md)\n", path);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> data((size_t)size);
  size_t n = fread(data.data(), 1, (size_t)size, f);
  (void)n;
  fclose(f);
  return data;
}

// Test-only minimal start-code scan (the library's own scanner is an
// internal implementation detail of decoder/mpg_decoder.h) - just enough
// to hand each unit's payload to the parsers under test.
static bool findUnit(const std::vector<uint8_t>& data, size_t from, uint8_t type,
                      const uint8_t** payload, size_t* payloadSize) {
  for (size_t i = from; i + 3 < data.size(); i++) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && data[i + 3] == type) {
      size_t start = i + 4;
      size_t end = data.size();
      for (size_t j = start; j + 2 < data.size(); j++) {
        if (data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 1) {
          end = j;
          break;
        }
      }
      *payload = data.data() + start;
      *payloadSize = end - start;
      return true;
    }
  }
  return false;
}

int main() {
  std::vector<uint8_t> data = readFile("assets/small.m1v");

  const uint8_t* seqPayload = nullptr;
  size_t seqSize = 0;
  bool haveSeqUnit = findUnit(data, 0, 0xB3, &seqPayload, &seqSize);
  assert(haveSeqUnit);

  BitReader br(seqPayload, seqSize);
  SequenceInfo seq;
  bool seqOk = parseSequenceHeader(br, &seq);
  assert(seqOk);
  assert(seq.valid);
  assert(seq.width == 176);
  assert(seq.height == 144);
  assert(seq.codedWidth == 176);   // already a multiple of 16
  assert(seq.codedHeight == 144);  // already a multiple of 16
  assert(seq.mbWidth == 11);
  assert(seq.mbHeight == 9);
  // ffmpeg's mpeg1video encoder doesn't load a custom intra matrix by
  // default - confirm the default (JPEG-style) matrix was used.
  assert(seq.intraMatrix[0] == 8);
  assert(seq.intraMatrix[63] == 83);

  const uint8_t* picPayload = nullptr;
  size_t picSize = 0;
  bool havePicUnit = findUnit(data, 0, 0x00, &picPayload, &picSize);
  assert(havePicUnit);

  BitReader pbr(picPayload, picSize);
  PictureInfo pic;
  bool picOk = parsePictureHeader(pbr, &pic);
  assert(picOk);
  assert(pic.codingType == 1);  // I-picture - see docs/preparing-input-with-ffmpeg.md's -g 0

  printf("test_headers: OK\n");
  return 0;
}
