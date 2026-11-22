#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include "tbytevectorstream.h"
#include "vorbisfile.h"
#include "xiphcomment.h"
#include "oggpageheader.h"
static int fail(const char *s) { std::cerr << s << '\n'; return 1; }
int main() {
  using namespace TagLib;
  std::ifstream input("tests/data/empty.ogg", std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
  if(bytes.empty()) return fail("fixture missing");
  ByteVectorStream stream(ByteVector(bytes.data(), static_cast<unsigned int>(bytes.size())));
  ByteVector first,setup,audio; int duration=0; long long granule=0;
  {
    Vorbis::File file(&stream);
    if(!file.isValid() || !file.audioProperties() || !file.tag()) return fail("open failed");
    first=file.packet(0); setup=file.packet(2);
    audio=file.packet(3);
    const bool first_eos_fetch_empty=audio.isEmpty();
    if(first_eos_fetch_empty) audio=file.packet(3);
    std::cout << "FIRST_EOS_PACKET_FETCH_EMPTY=" << first_eos_fetch_empty << '\n';
    duration=file.audioProperties()->lengthInMilliseconds(); granule=file.lastPageHeader()->absoluteGranularPosition();
    if(first.size()!=30 || setup.isEmpty() || audio.isEmpty() || duration!=3685) return fail("audio mismatch");
    file.tag()->setArtist("single-stream-baseline");
    if(!file.save()) return fail("save failed");
  }
  {
    Vorbis::File reopened(&stream);
    ByteVector reopened_audio=reopened.packet(3);
    if(reopened_audio.isEmpty()) reopened_audio=reopened.packet(3);
    if(!reopened.isValid() || reopened.tag()->artist()!=String("single-stream-baseline") || reopened.packet(0)!=first || reopened.packet(2)!=setup || reopened_audio!=audio || reopened.lastPageHeader()->absoluteGranularPosition()!=granule || reopened.audioProperties()->lengthInMilliseconds()!=duration) return fail("roundtrip mismatch");
  }
  std::cout << "TAGLIB_SINGLE_READ_SAVE_OK duration_ms=" << duration << " granule=" << granule << " audio_packet_bytes=" << audio.size() << '\n';
}
