/**
 * @file metadata_edit.cpp
 *
 * Example: render a small multi-channel PCM file with a reserved metadata
 * arena, close it, then rewrite the ADM (axml) and channel allocation (chna)
 * in place twice without touching the PCM payload.
 *
 * Usage: metadata_edit_example [OUTPUT_FILE]
 */
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <bw64/bw64.hpp>

using namespace bw64;

namespace {

  const uint16_t CHANNELS = 3;
  const uint32_t SAMPLE_RATE = 48000;
  const uint16_t BIT_DEPTH = 24;
  const uint64_t FRAMES = 16;
  // enough room for several growing axml/chna versions
  const uint32_t ARENA_BYTES = 4096;

  // one distinctive PCM level per channel so the payload can be checked
  // independently of the metadata
  std::vector<float> renderPcm() {
    const float levels[CHANNELS] = {0.25f, -0.5f, 0.75f};
    std::vector<float> samples(static_cast<size_t>(FRAMES) * CHANNELS);
    for (uint64_t frame = 0; frame < FRAMES; ++frame) {
      for (uint16_t ch = 0; ch < CHANNELS; ++ch) {
        samples[frame * CHANNELS + ch] = levels[ch];
      }
    }
    return samples;
  }

  std::shared_ptr<ChnaChunk> makeChna(uint16_t channels,
                                      const std::string& uidSuffix) {
    auto chna = std::make_shared<ChnaChunk>();
    for (uint16_t ch = 1; ch <= channels; ++ch) {
      const std::string digit(1, static_cast<char>('0' + ch));
      const std::string uid =
          std::string("ATU_000000") + uidSuffix + digit;
      const std::string trackRef =
          std::string("AT_0001000") + digit + "_01";
      const std::string packRef = std::string("AP_0001000") + digit;
      chna->addAudioId(AudioId(ch, uid, trackRef, packRef));
    }
    return chna;
  }

  std::shared_ptr<AxmlChunk> makeAxml(const std::string& label) {
    // treated as an exact byte string, not parsed
    std::string xml = "<audioProgramme label=\"" + label + "\">";
    for (uint16_t ch = 1; ch <= CHANNELS; ++ch) {
      xml += "<audioTrackFormat channel=\"" +
             std::to_string(static_cast<int>(ch)) + "\"/>";
    }
    xml += "</audioProgramme>";
    return std::make_shared<AxmlChunk>(xml);
  }

  void verifyWithReader(const std::string& path,
                        const std::vector<float>& expectedPcm,
                        unsigned expectedUids) {
    auto reader = readFile(path);
    if (reader->channels() != CHANNELS ||
        reader->numberOfFrames() != FRAMES) {
      throw std::runtime_error("format mismatch on reopen");
    }
    auto chna = reader->chnaChunk();
    if (expectedUids == 0u) {
      if (chna) throw std::runtime_error("chna should have been deleted");
    } else {
      if (!chna || chna->numUids() != expectedUids) {
        throw std::runtime_error("chna UID count mismatch on reopen");
      }
    }
    std::vector<float> pcm(static_cast<size_t>(FRAMES) * CHANNELS);
    const uint64_t readFrames = reader->read(pcm.data(), FRAMES);
    if (readFrames != FRAMES) {
      throw std::runtime_error("could not read all frames on reopen");
    }
    for (size_t i = 0; i < pcm.size(); ++i) {
      const float diff = pcm[i] - expectedPcm[i];
      if (diff > 1e-6f || diff < -1e-6f) {
        throw std::runtime_error("PCM payload changed during metadata edit");
      }
    }
    reader->close();
  }

}  // namespace

int main(int argc, char const* argv[]) {
  const std::string path =
      (argc >= 2) ? std::string(argv[1]) : std::string("metadata_edit.wav");
  const std::vector<float> pcm = renderPcm();

  // 1. render the file, reserve the arena before any audio, then close
  {
    auto initialChna = makeChna(CHANNELS, "1");
    auto initialAxml = makeAxml("initial");
    auto writer = writeFile(path, CHANNELS, SAMPLE_RATE, BIT_DEPTH,
                            initialChna, initialAxml);
    writer->reserveMetadata(ARENA_BYTES);
    writer->write(const_cast<float*>(pcm.data()), FRAMES);
    writer->close();
  }
  std::cout << "rendered " << FRAMES << " frames x " << CHANNELS
            << " channels with a " << ARENA_BYTES
            << "-byte reserved metadata arena" << std::endl;
  verifyWithReader(path, pcm, CHANNELS);

  // 2. first in-place update: replace both chna and axml on the closed file
  {
    auto chna = makeChna(CHANNELS, "2");
    auto axml = makeAxml("first-update");
    updateMetadataInPlace(path, chna, axml);
  }
  std::cout << "update 1: replaced chna and axml in place" << std::endl;
  {
    auto reader = readFile(path);
    auto axml = reader->axmlChunk();
    if (!axml || axml->data().find("first-update") == std::string::npos) {
      throw std::runtime_error("first axml update not observed");
    }
    if (!reader->chnaChunk() ||
        reader->chnaChunk()->numUids() != CHANNELS) {
      throw std::runtime_error("first chna update not observed");
    }
    reader->close();
  }
  verifyWithReader(path, pcm, CHANNELS);

  // 3. second in-place update: delete the chna, replace the axml again
  {
    auto axml = makeAxml("second-update");
    updateMetadataInPlace(path, nullptr, axml);
  }
  std::cout << "update 2: deleted chna and replaced axml in place" << std::endl;
  verifyWithReader(path, pcm, 0u);
  {
    auto reader = readFile(path);
    auto axml = reader->axmlChunk();
    if (!axml || axml->data().find("second-update") == std::string::npos) {
      throw std::runtime_error("second axml update not observed");
    }
    reader->close();
  }

  std::cout << "metadata edits completed without rewriting the PCM payload"
            << std::endl;
  return 0;
}
