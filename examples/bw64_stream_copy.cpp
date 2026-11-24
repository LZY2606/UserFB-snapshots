#include <bw64/bw64.hpp>
#include <iostream>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

// Copies a RIFF/BW64/RF64 WAVE stream from stdin to stdout in blocks,
// without seeking or creating temporary files, using the forward streaming
// API. This can be used to chain processing stages together with pipes, e.g.
//
//   ./bw64_create_sine - | ./bw64_stream_copy | ./bw64_info -

using namespace bw64;

const uint64_t BLOCK_SIZE = 4096;

int main() {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  auto inStream = readStream(std::cin);

  // chunks before the data chunk are known right after opening; chunks
  // stored after the audio data can not be known up-front on a stream and
  // are therefore not propagated here
  auto outStream =
      writeStream(std::cout, inStream->numberOfFrames(), inStream->channels(),
                  inStream->sampleRate(), inStream->bitDepth(),
                  inStream->chnaChunk(), inStream->axmlChunk());

  std::vector<float> buffer(BLOCK_SIZE * inStream->channels());
  while (!inStream->eof()) {
    auto readFrames = inStream->read(buffer.data(), BLOCK_SIZE);
    outStream->write(buffer.data(), readFrames);
  }

  inStream->finish();  // consume any chunks after the data chunk
  outStream->close();

  return 0;
}
