/***************************************************************************
    copyright            : (C) 2026 by the TagLib authors

    Tests for rewriting the Vorbis comment of a single logical stream in a
    multiplexed Ogg file, selected by its serial number.

    Page headers, lacing values, CRCs and packet sequences are interpreted
    independently of the production code, foreign pages and audio packets
    are compared byte for byte, and durations are computed by hand.
 *                                                                         *
 *   This library is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Lesser General Public License version   *
 *   2.1 as published by the Free Software Foundation.                     *
 ***************************************************************************/

#include "task_support/check.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <tbytevector.h>
#include <tbytevectorlist.h>
#include <tbytevectorstream.h>
#include <tstring.h>
#include <oggpageheader.h>
#include <vorbisfile.h>

using namespace TagLib;

namespace
{
  ////////////////////////////////////////////////////////////////////////////
  // Independent Ogg framing (the tests never trust the production parser to
  // verify the production parser).
  ////////////////////////////////////////////////////////////////////////////

  const unsigned int OggCrcPoly = 0x04c11db7U;

  unsigned int oggCrcTable[256];
  bool oggCrcTableReady = false;

  void initCrcTable()
  {
    if(oggCrcTableReady)
      return;

    for(unsigned int i = 0; i < 256; ++i) {
      unsigned int crc = i << 24;
      for(int j = 0; j < 8; ++j)
        crc = (crc & 0x80000000U) ? (crc << 1) ^ OggCrcPoly : (crc << 1);
      oggCrcTable[i] = crc;
    }
    oggCrcTableReady = true;
  }

  unsigned int oggCrc(const ByteVector &data)
  {
    initCrcTable();
    unsigned int crc = 0;
    for(unsigned int i = 0; i < data.size(); ++i)
      crc = (crc << 8) ^ oggCrcTable[((crc >> 24) & 0xff)
                                    ^ static_cast<unsigned char>(data[i])];
    return crc;
  }

  // A page interpreted straight from the bytes.
  struct RawPage
  {
    long offset;
    unsigned char flags;
    long long granule;
    unsigned int serial;
    unsigned int sequence;
    unsigned int crc;
    ByteVector segs;
    ByteVector body;

    bool continued() const { return (flags & 0x01) != 0; }
    bool bos() const { return (flags & 0x02) != 0; }
    bool eos() const { return (flags & 0x04) != 0; }
    bool lastCompleted() const
    {
      return segs.isEmpty() ||
             static_cast<unsigned char>(segs[segs.size() - 1]) != 255;
    }
    int size() const
    {
      return 27 + static_cast<int>(segs.size()) + body.size();
    }

    ByteVector render() const
    {
      ByteVector out;
      out.append(ByteVector("OggS", 4));
      out.append(static_cast<char>(0));
      out.append(static_cast<char>(flags));
      out.append(ByteVector::fromLongLong(granule, false));
      out.append(ByteVector::fromUInt(serial, false));
      out.append(ByteVector::fromUInt(sequence, false));
      out.append(ByteVector(4, 0));  // checksum slot, bytes 22..25
      out.append(static_cast<unsigned char>(segs.size()));
      out.append(segs);
      out.append(body);
      const ByteVector checksum = ByteVector::fromUInt(oggCrc(out), false);
      std::memcpy(out.data() + 22, checksum.data(), 4);
      return out;
    }
  };

  // Parses every physical page.  Fails on any framing error, a bad capture
  // pattern or a mismatching CRC.
  bool parsePages(const ByteVector &file, std::vector<RawPage> &pages)
  {
    initCrcTable();
    pages.clear();
    long offset = 0;
    const long n = static_cast<long>(file.size());

    while(offset < n) {
      if(file.mid(offset, 4) != ByteVector("OggS", 4))
        return false;
      if(offset + 27 > n)
        return false;

      RawPage page;
      page.offset = offset;
      page.flags = static_cast<unsigned char>(file[offset + 5]);
      page.granule = file.toLongLong(offset + 6, false);
      page.serial = file.toUInt(offset + 14, false);
      page.sequence = file.toUInt(offset + 18, false);
      page.crc = file.toUInt(offset + 22, false);

      const int nseg = static_cast<unsigned char>(file[offset + 26]);
      if(nseg < 1 || offset + 27 + nseg > n)
        return false;
      page.segs = file.mid(offset + 27, nseg);

      int dataSize = 0;
      for(int i = 0; i < nseg; ++i)
        dataSize += static_cast<unsigned char>(page.segs[i]);
      if(offset + 27 + nseg + dataSize > n)
        return false;
      page.body = file.mid(offset + 27 + nseg, dataSize);

      // The CRC is computed over the whole page with its CRC field zeroed.
      ByteVector check = file.mid(offset, 27 + nseg + dataSize);
      std::memset(check.data() + 22, 0, 4);
      if(oggCrc(check) != page.crc)
        return false;

      pages.push_back(page);
      offset += page.size();
    }
    return true;
  }

  ByteVector concatenatePages(const std::vector<RawPage> &pages)
  {
    ByteVector out;
    for(std::size_t i = 0; i < pages.size(); ++i)
      out.append(pages[i].render());
    return out;
  }

  // Reassembles the packets of one logical stream purely from its lacing
  // values, checking continuation flags along the way.
  bool extractPackets(const std::vector<RawPage> &pages, unsigned int serial,
                      std::vector<ByteVector> &packets)
  {
    packets.clear();
    bool open = false;

    for(std::size_t pi = 0; pi < pages.size(); ++pi) {
      const RawPage &page = pages[pi];
      if(page.serial != serial)
        continue;

      if(page.continued() != open)
        return false;

      if(!page.continued())
        packets.push_back(ByteVector());

      int bodyPos = 0;
      int segmentBytes = 0;
      for(unsigned int s = 0; s < page.segs.size(); ++s) {
        const int value = static_cast<unsigned char>(page.segs[s]);
        segmentBytes += value;
        if(value < 255) {
          packets.back().append(page.body.mid(bodyPos, segmentBytes));
          bodyPos += segmentBytes;
          segmentBytes = 0;
          if(s + 1 < page.segs.size())
            packets.push_back(ByteVector());
        }
      }
      if(segmentBytes > 0) {
        packets.back().append(page.body.mid(bodyPos, segmentBytes));
        open = true;
      }
      else
        open = false;
    }
    return !open;
  }

  ////////////////////////////////////////////////////////////////////////////
  // Page builder used to construct the multiplexed fixtures.
  ////////////////////////////////////////////////////////////////////////////

  struct BuiltPage
  {
    RawPage page;
    int lastCompletedPacket;  // -1 if no packet completes on this page
  };

  // Paginates packets following the Ogg lacing rules.  maxSegments limits the
  // segment table of a single page; small values force extra page breaks.
  // Granule positions follow the header rule (0 on completion, -1 while a
  // packet is unfinished); callers overwrite them for audio pages.
  std::vector<BuiltPage> buildPages(const std::vector<ByteVector> &packets,
                                    unsigned int serial,
                                    unsigned int firstSequence,
                                    int maxSegments)
  {
    std::vector<BuiltPage> out;
    ByteVector curSegs;
    ByteVector curBody;
    bool firstContinued = false;
    unsigned sequence = firstSequence;
    int lastCompleted = -1;

    for(std::size_t pi = 0; pi < packets.size(); ++pi) {
      const ByteVector &packet = packets[pi];
      unsigned int pos = 0;
      unsigned int remaining = packet.size();

      while(remaining >= 255) {
        if(static_cast<int>(curSegs.size()) >= maxSegments) {
          BuiltPage b;
          b.page.flags = firstContinued ? 0x01 : 0x00;
          b.page.granule = -1;
          b.page.serial = serial;
          b.page.sequence = sequence++;
          b.page.crc = 0;
          b.page.segs = curSegs;
          b.page.body = curBody;
          b.lastCompletedPacket = -1;
          out.push_back(b);
          curSegs.clear();
          curBody.clear();
          firstContinued = true;
          lastCompleted = -1;
        }
        curSegs.append(static_cast<char>(0xff));
        curBody.append(packet.mid(pos, 255));
        pos += 255;
        remaining -= 255;
      }

      if(static_cast<int>(curSegs.size()) >= maxSegments) {
        BuiltPage b;
        b.page.flags = firstContinued ? 0x01 : 0x00;
        b.page.granule = -1;
        b.page.serial = serial;
        b.page.sequence = sequence++;
        b.page.crc = 0;
        b.page.segs = curSegs;
        b.page.body = curBody;
        b.lastCompletedPacket = -1;
        out.push_back(b);
        curSegs.clear();
        curBody.clear();
        firstContinued = true;
        lastCompleted = -1;
      }
      curSegs.append(static_cast<unsigned char>(remaining));
      curBody.append(packet.mid(pos, remaining));
      lastCompleted = static_cast<int>(pi);
    }

    BuiltPage b;
    b.page.flags = firstContinued ? 0x01 : 0x00;
    b.page.granule = 0;
    b.page.serial = serial;
    b.page.sequence = sequence;
    b.page.crc = 0;
    b.page.segs = curSegs;
    b.page.body = curBody;
    b.lastCompletedPacket = lastCompleted;
    out.push_back(b);

    return out;
  }

  ////////////////////////////////////////////////////////////////////////////
  // Fixtures built from the real Vorbis headers and audio packets of the
  // repository's tests/data/empty.ogg.
  ////////////////////////////////////////////////////////////////////////////

  const unsigned int SerialA = 0x41000001U;
  const unsigned int SerialB = 0x42000002U;
  const unsigned int SerialX = 0x58000058U;  // opaque side stream

  struct StreamSource
  {
    std::vector<ByteVector> packets;  // all packets, three headers first
    long long finalGranule;
    unsigned int sampleRate;
    unsigned int channels;
  };

  // Reads the packets of the single stream empty.ogg with the independent
  // parser, so the fixtures carry genuine codec payload.
  bool loadSingleStream(const ByteVector &file, StreamSource &source)
  {
    std::vector<RawPage> pages;
    if(!parsePages(file, pages) || pages.empty())
      return false;

    const unsigned int serial = pages.front().serial;
    if(!extractPackets(pages, serial, source.packets))
      return false;
    if(source.packets.size() < 4)
      return false;

    source.finalGranule = 0;
    for(std::size_t i = 0; i < pages.size(); ++i) {
      if(pages[i].eos())
        source.finalGranule = pages[i].granule;
    }

    if(source.packets[0].size() < 16)
      return false;
    source.sampleRate = source.packets[0].toUInt(12, false);
    source.channels =
      static_cast<unsigned char>(source.packets[0][11]);
    return true;
  }

  void appendLacing(ByteVector &segs, unsigned int size)
  {
    while(size >= 255) {
      segs.append(static_cast<char>(0xff));
      size -= 255;
    }
    segs.append(static_cast<unsigned char>(size));
  }

  // Renders a complete logical stream: identification alone on BOS, comment
  // and setup paginated with headerSegmentLimit, then audio on an EOS page.
  // With splitAudio, one extra audio packet is prepended and split over a
  // continued page.  With sharedSetupAudio, the first audio packet shares the
  // page on which the setup header completes (an intentionally invalid
  // layout, used by a rejection case).
  std::vector<RawPage> renderStream(const StreamSource &source,
                                    unsigned int serial,
                                    int headerSegmentLimit,
                                    bool splitAudio,
                                    bool sharedSetupAudio = false)
  {
    std::vector<RawPage> out;

    // Identification header alone on the BOS page.
    {
      std::vector<ByteVector> id;
      id.push_back(source.packets[0]);
      std::vector<BuiltPage> p = buildPages(id, serial, 0, 255);
      p[0].page.flags |= 0x02;  // BOS
      out.push_back(p[0].page);
    }

    // Comment and setup, possibly spanning pages and sharing the last one.
    std::vector<ByteVector> headers;
    headers.push_back(source.packets[1]);
    headers.push_back(source.packets[2]);
    if(sharedSetupAudio)
      headers.push_back(source.packets[3]);
    std::vector<BuiltPage> hp =
      buildPages(headers, serial, 1, headerSegmentLimit);
    for(std::size_t i = 0; i < hp.size(); ++i) {
      RawPage page = hp[i].page;
      // A header page has granule 0 whenever a packet finishes on it (any
      // lacing value below 255), and -1 only while every packet it carries
      // is still unfinished.
      bool finishedHere = false;
      for(unsigned int s = 0; s < page.segs.size(); ++s) {
        if(static_cast<unsigned char>(page.segs[s]) < 255) {
          finishedHere = true;
          break;
        }
      }
      page.granule = finishedHere ? 0LL : -1LL;
      out.push_back(page);
    }

    unsigned sequence = static_cast<unsigned>(out.size());
    std::size_t firstAudio = sharedSetupAudio ? 4 : 3;

    if(splitAudio) {
      // A 300 byte audio packet (fill byte taken from the real first audio
      // packet) spans two pages, exercising audio continuation.  The first
      // page has to end on a 255 lacing value for the packet to stay open.
      ByteVector big(300, source.packets[3][0]);
      const unsigned int cut = 255;

      RawPage partial;
      partial.flags = 0x00;
      partial.granule = -1;
      partial.serial = serial;
      partial.sequence = sequence++;
      partial.crc = 0;
      // An open packet ends on a run of 255 lacing values with no
      // terminating value, so write the segment table directly.
      for(unsigned int left = cut; left > 0; left -= 255)
        partial.segs.append(static_cast<char>(0xff));
      partial.body = big.mid(0, cut);
      out.push_back(partial);

      RawPage tail;
      tail.flags = 0x01;  // continued
      tail.granule = source.finalGranule;
      tail.serial = serial;
      tail.sequence = sequence++;
      tail.crc = 0;
      appendLacing(tail.segs, big.size() - cut);
      tail.body.append(big.mid(cut));
      for(std::size_t k = firstAudio; k < source.packets.size(); ++k) {
        appendLacing(tail.segs, source.packets[k].size());
        tail.body.append(source.packets[k]);
      }
      tail.flags |= 0x04;  // EOS
      out.push_back(tail);
    }
    else {
      RawPage audio;
      audio.flags = 0x04;  // EOS
      audio.granule = source.finalGranule;
      audio.serial = serial;
      audio.sequence = sequence;
      audio.crc = 0;
      for(std::size_t k = firstAudio; k < source.packets.size(); ++k) {
        appendLacing(audio.segs, source.packets[k].size());
        audio.body.append(source.packets[k]);
      }
      out.push_back(audio);
    }

    return out;
  }

  // Opaque two page side stream: its payload is never interpreted as Vorbis
  // and its EOS page is meant to land physically after the Vorbis streams.
  std::vector<RawPage> renderSideStream(unsigned int serial)
  {
    const ByteVector payload(300, '\x7e');
    std::vector<RawPage> out;

    RawPage bos;
    bos.flags = 0x02;
    bos.granule = 0;
    bos.serial = serial;
    bos.sequence = 0;
    bos.crc = 0;
    bos.segs.append(static_cast<char>(0xff));
    bos.body = payload.mid(0, 255);
    out.push_back(bos);

    RawPage rest;
    rest.flags = 0x01 | 0x04;
    rest.granule = 123456789LL;
    rest.serial = serial;
    rest.sequence = 1;
    rest.crc = 0;
    rest.segs.append(static_cast<unsigned char>(payload.size() - 255));
    rest.body = payload.mid(255);
    out.push_back(rest);

    return out;
  }

  // Standard multiplexed fixture: BOS pages of all three streams first, then
  // the remaining Vorbis pages interleaved (a B page before each A page),
  // and the side stream EOS page last.  Stream B is the first stream, so
  // selecting A exercises "target is not the first stream".
  ByteVector buildMuxFixture(const StreamSource &sourceA,
                             const StreamSource &sourceB,
                             int headerSegmentLimit,
                             bool splitAudioA,
                             bool sharedSetupAudio = false)
  {
    std::vector<RawPage> a = renderStream(sourceA, SerialA,
                                          headerSegmentLimit, splitAudioA,
                                          sharedSetupAudio);
    std::vector<RawPage> b = renderStream(sourceB, SerialB,
                                          headerSegmentLimit, false);
    std::vector<RawPage> x = renderSideStream(SerialX);

    std::vector<RawPage> ordered;
    ordered.push_back(b[0]);  // first stream BOS
    ordered.push_back(a[0]);  // target stream BOS, second position
    ordered.push_back(x[0]);  // side stream BOS

    const std::size_t maxRest =
      std::max(a.size(), b.size()) - 1;
    for(std::size_t k = 1; k <= maxRest; ++k) {
      if(k < b.size())
        ordered.push_back(b[k]);
      if(k < a.size())
        ordered.push_back(a[k]);
    }
    ordered.push_back(x[1]);  // foreign EOS page later than every Vorbis EOS

    return concatenatePages(ordered);
  }

  ////////////////////////////////////////////////////////////////////////////
  // Shared helpers.
  ////////////////////////////////////////////////////////////////////////////

  bool readFile(const char *path, ByteVector &out)
  {
    FILE *f = std::fopen(path, "rb");
    if(!f)
      return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<unsigned int>(size));
    const std::size_t read = std::fread(out.data(), 1, size, f);
    std::fclose(f);
    return read == static_cast<std::size_t>(size);
  }

  bool writeFile(const char *path, const ByteVector &data)
  {
    FILE *f = std::fopen(path, "wb");
    if(!f)
      return false;
    const std::size_t written =
      std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return written == data.size();
  }

  // Raw rendered bytes of every page belonging to a serial, in order.
  std::vector<ByteVector> pagesOf(const std::vector<RawPage> &pages,
                                  unsigned int serial)
  {
    std::vector<ByteVector> out;
    for(std::size_t i = 0; i < pages.size(); ++i)
      if(pages[i].serial == serial)
        out.push_back(pages[i].render());
    return out;
  }

  bool sameByteVectors(const std::vector<ByteVector> &a,
                       const std::vector<ByteVector> &b)
  {
    if(a.size() != b.size())
      return false;
    for(std::size_t i = 0; i < a.size(); ++i)
      if(a[i] != b[i])
        return false;
    return true;
  }

  // Hand computed Vorbis duration: final granule PCM samples over the sample
  // rate taken from the identification header.
  int durationMs(const ByteVector &identification, long long lastGranule)
  {
    const unsigned int sampleRate = identification.toUInt(12, false);
    return static_cast<int>(lastGranule * 1000.0 / sampleRate + 0.5);
  }

  long long targetPageBytes(const std::vector<RawPage> &pages,
                            unsigned int serial)
  {
    long long total = 0;
    for(std::size_t i = 0; i < pages.size(); ++i)
      if(pages[i].serial == serial)
        total += pages[i].size();
    return total;
  }

  const char *TempFileName = ".task-build/mux_comment_case.ogg";

  // Snapshot of everything that must remain untouched while one stream is
  // rewritten.
  struct Snapshot
  {
    std::vector<ByteVector> foreignB;
    std::vector<ByteVector> foreignX;
    std::vector<ByteVector> targetAudioPackets;
    ByteVector identification;
    ByteVector setup;
  };

  Snapshot takeSnapshot(const ByteVector &file, unsigned int target,
                        const StreamSource &source)
  {
    std::vector<RawPage> pages;
    parsePages(file, pages);
    Snapshot s;
    s.foreignB = pagesOf(pages, SerialB == target ? SerialA : SerialB);
    s.foreignX = pagesOf(pages, SerialX);
    s.identification = source.packets[0];
    s.setup = source.packets[2];
    std::vector<ByteVector> all;
    extractPackets(pages, target, all);
    for(std::size_t i = 3; i < all.size(); ++i)
      s.targetAudioPackets.push_back(all[i]);
    return s;
  }

  // Verifies the untouched parts after a rewrite: foreign page bytes and
  // their relative order, target identification/setup and audio packets.
  bool verifyUntouched(const ByteVector &file, unsigned int target,
                       const Snapshot &s)
  {
    std::vector<RawPage> pages;
    if(!parsePages(file, pages))
      return false;

    const unsigned int otherSerial =
      SerialB == target ? SerialA : SerialB;
    if(!sameByteVectors(pagesOf(pages, otherSerial), s.foreignB))
      return false;
    if(!sameByteVectors(pagesOf(pages, SerialX), s.foreignX))
      return false;

    std::vector<ByteVector> packets;
    if(!extractPackets(pages, target, packets) || packets.size() < 4)
      return false;
    if(packets[0] != s.identification || packets[2] != s.setup)
      return false;
    if(packets.size() - 3 != s.targetAudioPackets.size())
      return false;
    for(std::size_t i = 0; i < s.targetAudioPackets.size(); ++i) {
      if(packets[3 + i] != s.targetAudioPackets[i])
        return false;
    }
    return true;
  }

  // Every page must carry a valid CRC and the selected stream's page numbers
  // must be continuous from zero to its EOS page.
  bool verifyPageNumbering(const ByteVector &file, unsigned int target)
  {
    std::vector<RawPage> pages;
    if(!parsePages(file, pages))
      return false;

    unsigned int expected = 0;
    bool sawEos = false;
    bool bosFirst = false;
    for(std::size_t i = 0; i < pages.size(); ++i) {
      if(pages[i].serial != target)
        continue;
      if(pages[i].sequence != expected)
        return false;
      if(expected == 0) {
        if(!pages[i].bos() || pages[i].continued())
          return false;
        bosFirst = true;
      }
      if(sawEos)
        return false;
      sawEos = pages[i].eos();
      ++expected;
    }
    return bosFirst && sawEos;
  }

  bool loadSources(StreamSource &a, StreamSource &b)
  {
    ByteVector fixture;
    if(!readFile("tests/data/empty.ogg", fixture))
      return false;
    if(!loadSingleStream(fixture, a))
      return false;
    b = a;
    return true;
  }

  // A ByteVectorStream keeps its own copy of the bytes; after a save the
  // live content is reached through the stream, not the original vector.
  const ByteVector &liveBytes(ByteVectorStream &stream)
  {
    return *stream.data();
  }

  // Hand built Vorbis comment packet (0x03vorbis prefix plus a one field
  // Xiph style comment block).
  ByteVector makeCommentPacket(const std::string &vendor,
                               const std::string &field)
  {
    ByteVector block;
    block.append(ByteVector::fromUInt(static_cast<unsigned int>(vendor.size()),
                                      false));
    block.append(ByteVector(vendor.data(),
                            static_cast<unsigned int>(vendor.size())));
    block.append(ByteVector::fromUInt(1, false));
    block.append(ByteVector::fromUInt(static_cast<unsigned int>(field.size()),
                                      false));
    block.append(ByteVector(field.data(),
                            static_cast<unsigned int>(field.size())));
    block.append(static_cast<char>(1));  // framing bit

    ByteVector packet;
    packet.append(static_cast<char>(0x03));
    packet.append("vorbis");
    packet.append(block);
    return packet;
  }

  // Replaces the comment packet of a source while keeping the other packets.
  StreamSource withComment(const StreamSource &source,
                           const std::string &vendor,
                           const std::string &field)
  {
    StreamSource out = source;
    out.packets[1] = makeCommentPacket(vendor, field);
    return out;
  }

  // Expected average bitrate over the selected stream, using only the bytes
  // of its own pages and its current header packet sizes, mirroring
  // Vorbis::Properties (which reports kbps by dividing by the length in ms).
  int expectedBitrate(const std::vector<RawPage> &pages,
                      unsigned int serial,
                      unsigned int sampleRate,
                      long long finalGranule)
  {
    std::vector<ByteVector> packets;
    if(!extractPackets(pages, serial, packets) || packets.size() < 3)
      return -1;
    long long bytes = targetPageBytes(pages, serial);
    for(int i = 0; i < 3; ++i)
      bytes -= packets[i].size();
    const double lengthMs = finalGranule * 1000.0 / sampleRate;
    return static_cast<int>(bytes * 8.0 / lengthMs + 0.5);
  }
}  // namespace

// ============================================================================
// selection group
// ============================================================================

static int selection_second_stream_iostream()
{
  StreamSource a, b;
  TASK_CHECK(loadSources(a, b));
  a = withComment(a, "vendor-a", "ARTIST=artist-A");
  b = withComment(b, "vendor-b", "ARTIST=artist-B");

  const ByteVector file = buildMuxFixture(a, b, 16, false);
  std::vector<RawPage> pages;
  TASK_CHECK(parsePages(file, pages));

  ByteVectorStream stream(file);
  Vorbis::File f(&stream, true, Vorbis::Properties::Average, SerialA);
  TASK_CHECK(f.isValid());
  TASK_CHECK(f.tag() != 0);
  TASK_CHECK(f.tag()->artist() == String("artist-A"));
  TASK_CHECK(f.packet(0) == a.packets[0]);
  TASK_CHECK(f.packet(2) == a.packets[2]);
  TASK_CHECK(f.firstPageHeader()->streamSerialNumber() == SerialA);
  TASK_CHECK(f.lastPageHeader()->streamSerialNumber() == SerialA);
  TASK_CHECK(f.lastPageHeader()->absoluteGranularPosition() == a.finalGranule);
  TASK_CHECK(f.audioProperties() != 0);
  TASK_CHECK(f.audioProperties()->sampleRate() ==
             static_cast<int>(a.sampleRate));
  TASK_CHECK(f.audioProperties()->channels() ==
             static_cast<int>(a.channels));
  TASK_CHECK(f.audioProperties()->lengthInMilliseconds() ==
             durationMs(a.packets[0], a.finalGranule));
  TASK_CHECK(f.audioProperties()->bitrate() ==
             expectedBitrate(pages, SerialA, a.sampleRate, a.finalGranule));

  // The other stream is independently selectable and reports its own tag.
  ByteVectorStream streamB(file);
  Vorbis::File fb(&streamB, true, Vorbis::Properties::Average, SerialB);
  TASK_CHECK(fb.isValid());
  TASK_CHECK(fb.tag()->artist() == String("artist-B"));
  TASK_CHECK(fb.firstPageHeader()->streamSerialNumber() == SerialB);
  return 0;
}

static int selection_filename_entry()
{
  StreamSource a, b;
  TASK_CHECK(loadSources(a, b));
  a = withComment(a, "vendor-a", "ARTIST=file-A");
  const ByteVector file = buildMuxFixture(a, b, 16, false);
  TASK_CHECK(writeFile(TempFileName, file));

  {
    Vorbis::File f(TempFileName, true, Vorbis::Properties::Average, SerialA);
    TASK_CHECK(f.isValid());
    TASK_CHECK(f.tag()->artist() == String("file-A"));
    f.tag()->setArtist("file-A-saved");
    TASK_CHECK(f.save());
  }
  {
    Vorbis::File f(TempFileName, true, Vorbis::Properties::Average, SerialA);
    TASK_CHECK(f.isValid());
    TASK_CHECK(f.tag()->artist() == String("file-A-saved"));
  }
  ByteVector rewritten;
  TASK_CHECK(readFile(TempFileName, rewritten));
  std::vector<RawPage> pages;
  TASK_CHECK(parsePages(rewritten, pages));
  return 0;
}

static int selection_unknown_serial_invalid()
{
  StreamSource a, b;
  TASK_CHECK(loadSources(a, b));
  const ByteVector file = buildMuxFixture(a, b, 16, false);

  ByteVectorStream stream(file);
  Vorbis::File f(&stream, true, Vorbis::Properties::Average, 0xdeadbeefU);
  TASK_CHECK(!f.isValid());
  TASK_CHECK(f.firstPageHeader() == 0);
  TASK_CHECK(f.lastPageHeader() == 0);

  // The opaque side stream has a serial but no Vorbis BOS.
  ByteVectorStream streamX(file);
  Vorbis::File fx(&streamX, true, Vorbis::Properties::Average, SerialX);
  TASK_CHECK(!fx.isValid());
  return 0;
}

static int selection_legacy_single_stream()
{
  ByteVector fixture;
  TASK_CHECK(readFile("tests/data/empty.ogg", fixture));
  ByteVectorStream stream(fixture);
  Vorbis::File f(&stream);
  TASK_CHECK(f.isValid());
  TASK_CHECK(f.packet(0).size() == 30);
  TASK_CHECK(f.audioProperties() != 0);
  TASK_CHECK(f.audioProperties()->lengthInMilliseconds() == 3685);
  TASK_CHECK(f.firstPageHeader() != 0);
  TASK_CHECK(f.lastPageHeader() != 0);
  return 0;
}

static int selection_skip_audio_properties()
{
  StreamSource a, b;
  TASK_CHECK(loadSources(a, b));
  const ByteVector file = buildMuxFixture(a, b, 16, false);

  // With readProperties disabled no audio properties are built, but the tag
  // and packet access still work for the selected logical stream.
  ByteVectorStream stream(file);
  Vorbis::File f(&stream, false, Vorbis::Properties::Average, SerialA);
  TASK_CHECK(f.isValid());
  TASK_CHECK(f.audioProperties() == 0);
  TASK_CHECK(f.tag() != 0);
  TASK_CHECK(f.packet(0) == a.packets[0]);
  TASK_CHECK(f.firstPageHeader() != 0);
  TASK_CHECK(f.lastPageHeader() != 0);

  // Saving remains possible and keeps the structure guarantees.
  const Snapshot snapshot = takeSnapshot(file, SerialA, a);
  f.tag()->setArtist("no-properties-save");
  TASK_CHECK(f.save());
  TASK_CHECK(verifyUntouched(liveBytes(stream), SerialA, snapshot));
  return 0;
}

// ============================================================================
// rewrite group
// ============================================================================

static int rewrite_short_comment_preserves_everything()
{
  StreamSource a, b;
  TASK_CHECK(loadSources(a, b));
  a = withComment(a, "vendor-a", "ARTIST=before-A");
  const ByteVector original = buildMuxFixture(a, b, 16, false);
  const Snapshot snapshot = takeSnapshot(original, SerialA, a);

  ByteVector data = original;
  ByteVectorStream stream(data);
  Vorbis::File f(&stream, true, Vorbis::Properties::Average, SerialA);
  TASK_CHECK(f.isValid());
  f.tag()->setArtist("after-A");
  TASK_CHECK(f.save());

  const ByteVector &written = liveBytes(stream);
  TASK_CHECK(verifyUntouched(written, SerialA, snapshot));
  TASK_CHECK(verifyPageNumbering(written, SerialA));

  std::vector<RawPage> pages;
  TASK_CHECK(parsePages(written, pages));
  std::vector<ByteVector> packets;
  TASK_CHECK(extractPackets(pages, SerialA, packets));
  TASK_CHECK(packets[1].mid(0, 7) ==
             ByteVector("\x03vorbis", 7));
  ByteVectorStream reopenStream(written);
  Vorbis::File reopened(&reopenStream, true,
                        Vorbis::Properties::Average, SerialA);
  TASK_CHECK(reopened.isValid());
  TASK_CHECK(reopened.tag()->artist() == String("after-A"));
  TASK_CHECK(reopened.audioProperties()->lengthInMilliseconds() ==
             durationMs(a.packets[0], a.finalGranule));
  TASK_CHECK(reopened.audioProperties()->bitrate() ==
             expectedBitrate(pages, SerialA, a.sampleRate, a.finalGranule));
  return 0;
}

static int rewrite_three_consecutive_saves()
{
  StreamSource a, b;
  TASK_CHECK(loadSources(a, b));
  const ByteVector original = buildMuxFixture(a, b, 16, false);
  const Snapshot snapshot = takeSnapshot(original, SerialA, a);

  ByteVector data = original;
  ByteVectorStream stream(data);

  // All three saves run through a single File object, proving that an object
  // can keep reading the original packets and saving again after each write.
  Vorbis::File f(&stream, true, Vorbis::Properties::Average, SerialA);
  TASK_CHECK(f.isValid());

  // Save one: short comment.
  f.tag()->setComment("short");
  TASK_CHECK(f.save());

  // The same object still reads the original codec packets after saving.
  TASK_CHECK(f.packet(0) == a.packets[0]);
  TASK_CHECK(f.packet(2) == a.packets[2]);
  TASK_CHECK(f.packet(3) == a.packets[3]);

  // Save two: a comment spanning multiple pages and crossing the 65025 byte
  // lacing boundary on the same object.
  {
    const String huge(ByteVector(70000, 'L'), String::Latin1);
    f.tag()->setComment(huge);
    TASK_CHECK(f.save());
    TASK_CHECK(verifyPageNumbering(liveBytes(stream), SerialA));
    TASK_CHECK(verifyUntouched(liveBytes(stream), SerialA, snapshot));

    std::vector<RawPage> pages;
    TASK_CHECK(parsePages(liveBytes(stream), pages));
    std::vector<ByteVector> packets;
    TASK_CHECK(extractPackets(pages, SerialA, packets));
    TASK_CHECK(packets[1].size() > 65025);
    TASK_CHECK(packets[1].startsWith(ByteVector("\x03vorbis", 7)));
  }

  // Save three: shrink the comment again, still on the same object.
  f.tag()->setComment("short again");
  TASK_CHECK(f.save());

  // The same object keeps serving the untouched packets after every save.
  TASK_CHECK(f.packet(0) == a.packets[0]);
  TASK_CHECK(f.packet(2) == a.packets[2]);
  TASK_CHECK(f.packet(3) == a.packets[3]);

  // Reopen from scratch and check the final state and all immutable bytes.
  const ByteVector &written = liveBytes(stream);
  TASK_CHECK(verifyUntouched(written, SerialA, snapshot));
  TASK_CHECK(verifyPageNumbering(written, SerialA));
  ByteVectorStream reopenStream(written);
  Vorbis::File reopened(&reopenStream, true,
                        Vorbis::Properties::Average, SerialA);
  TASK_CHECK(reopened.isValid());
  TASK_CHECK(reopened.tag()->comment() == String("short again"));
  TASK_CHECK(reopened.audioProperties()->lengthInMilliseconds() ==
             durationMs(a.packets[0], a.finalGranule));
  return 0;
}

static int rewrite_comment_spanning_pages_sharing_setup()
{
  StreamSource a, b;
  TASK_CHECK(loadSources(a, b));
  // A long initial comment: with the 16 segment limit it spans pages and its
  // last page also starts the setup header.
  a = withComment(a, "vendor-a", std::string("ARTIST=") +
                                 std::string(6000, 'S'));
  const ByteVector original = buildMuxFixture(a, b, 16, false);
  {
    std::vector<RawPage> pages;
    TASK_CHECK(parsePages(original, pages));
    bool sawShared = false;
    for(std::size_t i = 0; i < pages.size(); ++i) {
      if(pages[i].serial != SerialA)
        continue;
      // A page that both completes a packet and keeps another one open is the
      // shared comment/setup page.
      if(!pages[i].lastCompleted() && pages[i].segs.size() > 1)
        sawShared = true;
    }
    TASK_CHECK(sawShared);
  }
  const Snapshot snapshot = takeSnapshot(original, SerialA, a);

  ByteVector data = original;
  ByteVectorStream stream(data);
  Vorbis::File f(&stream, true, Vorbis::Properties::Average, SerialA);
  TASK_CHECK(f.isValid());
  f.tag()->setArtist("compact");
  TASK_CHECK(f.save());

  const ByteVector &written = liveBytes(stream);
  TASK_CHECK(verifyUntouched(written, SerialA, snapshot));
  TASK_CHECK(verifyPageNumbering(written, SerialA));
  ByteVectorStream reopenStream(written);
  Vorbis::File reopened(&reopenStream, true,
                        Vorbis::Properties::Average, SerialA);
  TASK_CHECK(reopened.isValid());
  TASK_CHECK(reopened.tag()->artist() == String("compact"));
  return 0;
}

static int rewrite_audio_continuation_preserved()
{
  StreamSource a, b;
  TASK_CHECK(loadSources(a, b));
  const ByteVector original = buildMuxFixture(a, b, 16, true);
  const Snapshot snapshot = takeSnapshot(original, SerialA, a);

  // Remember the exact bytes of every target audio page.  Header pages always
  // carry granule 0, so audio pages are precisely the pages with a non zero
  // granule (-1 while a packet is open, the final PCM position on EOS).
  std::vector<RawPage> before;
  TASK_CHECK(parsePages(original, before));
  std::vector<ByteVector> audioPagesBefore;
  for(std::size_t i = 0; i < before.size(); ++i) {
    if(before[i].serial == SerialA && before[i].granule != 0)
      audioPagesBefore.push_back(before[i].render());
  }
  TASK_CHECK(audioPagesBefore.size() >= 2);

  ByteVector data = original;
  ByteVectorStream stream(data);
  Vorbis::File f(&stream, true, Vorbis::Properties::Average, SerialA);
  TASK_CHECK(f.isValid());
  f.tag()->setArtist("continued-audio");
  TASK_CHECK(f.save());

  std::vector<RawPage> after;
  TASK_CHECK(parsePages(liveBytes(stream), after));
  std::vector<ByteVector> audioPagesAfter;
  for(std::size_t i = 0; i < after.size(); ++i) {
    if(after[i].serial == SerialA && after[i].granule != 0)
      audioPagesAfter.push_back(after[i].render());
  }
  // Comment page count changes renumber the audio pages, but their rendered
  // bytes must only differ in the sequence number and CRC fields.
  TASK_CHECK(audioPagesAfter.size() == audioPagesBefore.size());
  for(std::size_t i = 0; i < audioPagesBefore.size(); ++i) {
    ByteVector p = audioPagesBefore[i];
    ByteVector q = audioPagesAfter[i];
    TASK_CHECK(p.size() == q.size());
    std::memset(p.data() + 18, 0, 4);
    std::memset(p.data() + 22, 0, 4);
    std::memset(q.data() + 18, 0, 4);
    std::memset(q.data() + 22, 0, 4);
    TASK_CHECK(p == q);
  }

  TASK_CHECK(verifyUntouched(liveBytes(stream), SerialA, snapshot));
  std::vector<ByteVector> packets;
  TASK_CHECK(extractPackets(after, SerialA, packets));
  TASK_CHECK(packets[3].size() == 300);
  TASK_CHECK(packets[3] == ByteVector(300, a.packets[3][0]));
  return 0;
}

static int rewrite_foreign_pages_unchanged_and_ordered()
{
  StreamSource a, b;
  TASK_CHECK(loadSources(a, b));
  const ByteVector original = buildMuxFixture(a, b, 16, false);

  // Foreign page byte sequence in physical order before the rewrite.
  std::vector<RawPage> before;
  TASK_CHECK(parsePages(original, before));
  std::vector<ByteVector> foreignBefore;
  for(std::size_t i = 0; i < before.size(); ++i) {
    if(before[i].serial != SerialA)
      foreignBefore.push_back(before[i].render());
  }

  ByteVector data = original;
  ByteVectorStream stream(data);
  Vorbis::File f(&stream, true, Vorbis::Properties::Average, SerialA);
  f.tag()->setComment(std::string(4000, 'Z'));
  TASK_CHECK(f.save());

  std::vector<RawPage> after;
  TASK_CHECK(parsePages(liveBytes(stream), after));
  std::vector<ByteVector> foreignAfter;
  for(std::size_t i = 0; i < after.size(); ++i) {
    if(after[i].serial != SerialA)
      foreignAfter.push_back(after[i].render());
  }
  TASK_CHECK(sameByteVectors(foreignBefore, foreignAfter));

  // The side stream EOS page remains the physical last page.
  TASK_CHECK(after.back().serial == SerialX);
  TASK_CHECK(after.back().eos());
  return 0;
}

// Builds a fixture where a foreign (B stream) page is physically sandwiched
// between the target's open ended comment page and the continued page on
// which the setup header finishes.  The long comment forces the comment to
// span pages in the first place.
static ByteVector buildHeaderInterleavedFixture(const StreamSource &srcA,
                                                 const StreamSource &srcB)
{
  StreamSource longCommentA = srcA;
  // A comment larger than 16 lacing values (16 * 255 = 4080 bytes) forces the
  // first comment page to end open with sixteen 255 values, so a later page
  // has to continue it.
  longCommentA.packets[1] =
    makeCommentPacket("vendor-a", std::string("ARTIST=") + std::string(6000, 'I'));

  std::vector<RawPage> a = renderStream(longCommentA, SerialA, 16, false);
  std::vector<RawPage> b = renderStream(srcB, SerialB, 16, false);
  std::vector<RawPage> x = renderSideStream(SerialX);

  // Locate the target page that opens the comment packet (open ended) and the
  // continued page that first follows it.
  std::size_t commentOpenIndex = 0;
  while(commentOpenIndex < a.size() && a[commentOpenIndex].lastCompleted())
    ++commentOpenIndex;
  if(commentOpenIndex >= a.size())
    return ByteVector();  // malformed fixture; the test will fail to parse

  std::vector<RawPage> ordered;
  ordered.push_back(b[0]);
  ordered.push_back(a[0]);
  ordered.push_back(x[0]);

  // Target pages up to and including the open ended comment page.
  for(std::size_t i = 1; i <= commentOpenIndex; ++i)
    ordered.push_back(a[i]);

  // A foreign page physically in the middle of the target's header pages.
  // Relabel it as a self contained B stream page (its own sequence).
  RawPage sandwiched = b[1];
  sandwiched.sequence = 1;
  ordered.push_back(sandwiched);

  // The rest of the target stream (continued setup page and audio).
  for(std::size_t i = commentOpenIndex + 1; i < a.size(); ++i)
    ordered.push_back(a[i]);

  // Remaining B stream pages after the borrowed one, renumbered so the B
  // stream still has continuous numbering from its BOS page.
  unsigned int bSeq = 2;
  for(std::size_t i = 2; i < b.size(); ++i) {
    b[i].sequence = bSeq++;
    ordered.push_back(b[i]);
  }
  ordered.push_back(x[1]);

  return concatenatePages(ordered);
}

static int rewrite_foreign_page_between_header_pages()
{
  StreamSource a, b;
  TASK_CHECK(loadSources(a, b));
  const ByteVector original = buildHeaderInterleavedFixture(a, b);

  std::vector<RawPage> before;
  TASK_CHECK(parsePages(original, before));

  // Prove the sandwich exists: an open ended target page, then a foreign page,
  // then the continued target page.
  bool sawOpen = false, sawForeign = false, sawContinued = false;
  for(std::size_t i = 0; i < before.size(); ++i) {
    const RawPage &p = before[i];
    if(p.serial == SerialA && !p.lastCompleted())
      sawOpen = true;
    else if(sawOpen && !sawContinued && p.serial != SerialA)
      sawForeign = true;
    else if(sawForeign && p.serial == SerialA && p.continued())
      sawContinued = true;
  }
  TASK_CHECK(sawOpen && sawForeign && sawContinued);

  const Snapshot snapshot = takeSnapshot(original, SerialA, a);

  std::vector<ByteVector> foreignBefore;
  for(std::size_t i = 0; i < before.size(); ++i)
    if(before[i].serial != SerialA)
      foreignBefore.push_back(before[i].render());

  ByteVector data = original;
  ByteVectorStream stream(data);
  Vorbis::File f(&stream, true, Vorbis::Properties::Average, SerialA);
  TASK_CHECK(f.isValid());
  f.tag()->setArtist("sandwiched");
  TASK_CHECK(f.save());

  const ByteVector &written = liveBytes(stream);
  std::vector<RawPage> after;
  TASK_CHECK(parsePages(written, after));

  // Every foreign page survives byte for byte and keeps its relative order;
  // none of them is swallowed by the header rewrite.
  std::vector<ByteVector> foreignAfter;
  for(std::size_t i = 0; i < after.size(); ++i)
    if(after[i].serial != SerialA)
      foreignAfter.push_back(after[i].render());
  TASK_CHECK(sameByteVectors(foreignBefore, foreignAfter));

  TASK_CHECK(verifyUntouched(written, SerialA, snapshot));
  TASK_CHECK(verifyPageNumbering(written, SerialA));

  ByteVectorStream reopen(written);
  Vorbis::File rf(&reopen, true, Vorbis::Properties::Average, SerialA);
  TASK_CHECK(rf.isValid());
  TASK_CHECK(rf.tag()->artist() == String("sandwiched"));
  return 0;
}

// ============================================================================
// boundaries group
// ============================================================================

static int boundaries_lacing_255_and_65025()
{
  StreamSource a, b;
  TASK_CHECK(loadSources(a, b));
  const ByteVector original = buildMuxFixture(a, b, 16, false);

  // Field lengths chosen so the comment packet repeatedly crosses multiples
  // of 255 and, for the large one, 65025 bytes.
  const unsigned int fieldLengths[] = {
    200, 240, 255, 256, 510, 1000, 65000, 70000
  };

  for(std::size_t k = 0; k < sizeof(fieldLengths) / sizeof(fieldLengths[0]);
      ++k) {
    ByteVector data = original;
    ByteVectorStream stream(data);
    Vorbis::File f(&stream, true, Vorbis::Properties::Average, SerialA);
    TASK_CHECK(f.isValid());
    f.tag()->setComment(String(ByteVector(fieldLengths[k], 'Q'),
                               String::Latin1));
    TASK_CHECK(f.save());

    const ByteVector &written = liveBytes(stream);
    std::vector<RawPage> pages;
    TASK_CHECK(parsePages(written, pages));
    std::vector<ByteVector> packets;
    TASK_CHECK(extractPackets(pages, SerialA, packets));
    TASK_CHECK(packets.size() >= 4);

    // Every reassembled packet ends properly: the final lacing value of its
    // last page must be below 255, which extractPackets already guarantees by
    // reporting success; additionally verify no target page dangles at EOF.
    bool targetEos = false;
    for(std::size_t i = 0; i < pages.size(); ++i) {
      if(pages[i].serial == SerialA && pages[i].eos()) {
        TASK_CHECK(pages[i].lastCompleted());
        targetEos = true;
      }
    }
    TASK_CHECK(targetEos);
    TASK_CHECK(verifyPageNumbering(written, SerialA));

    ByteVectorStream reopenStream(written);
    Vorbis::File reopened(&reopenStream, true,
                          Vorbis::Properties::Average, SerialA);
    TASK_CHECK(reopened.isValid());
    TASK_CHECK(static_cast<unsigned int>(reopened.tag()->comment().size()) ==
               fieldLengths[k]);
  }
  return 0;
}

// Helper: parses a fixture, applies one page edit and re-renders it.
typedef void (*PageEdit)(std::vector<RawPage> &pages, unsigned int serial);

static void edit_gap(std::vector<RawPage> &pages, unsigned int serial)
{
  for(std::size_t i = 0; i < pages.size(); ++i) {
    if(pages[i].serial == serial && pages[i].sequence == 1) {
      pages[i].sequence = 2;  // page number 1 is now missing
      return;
    }
  }
}

static void edit_continuation_mismatch(std::vector<RawPage> &pages,
                                       unsigned int serial)
{
  for(std::size_t i = 0; i < pages.size(); ++i) {
    if(pages[i].serial == serial && pages[i].continued()) {
      pages[i].flags &= static_cast<unsigned char>(~0x01);
      return;
    }
  }
}

static void edit_missing_eos(std::vector<RawPage> &pages,
                             unsigned int serial)
{
  for(std::size_t i = pages.size(); i > 0; --i) {
    if(pages[i - 1].serial == serial) {
      pages[i - 1].flags &= static_cast<unsigned char>(~0x04);
      return;
    }
  }
}

static void edit_dangling_packet(std::vector<RawPage> &pages,
                                 unsigned int serial)
{
  for(std::size_t i = pages.size(); i > 0; --i) {
    if(pages[i - 1].serial == serial) {
      RawPage &p = pages[i - 1];
      p.segs.append(static_cast<char>(0xff));
      p.body.append(ByteVector(255, '\x55'));
      return;
    }
  }
}

static int run_rejection(const ByteVector &original, PageEdit edit)
{
  std::vector<RawPage> pages;
  TASK_CHECK(parsePages(original, pages));
  edit(pages, SerialA);
  const ByteVector damaged = concatenatePages(pages);
  TASK_CHECK(parsePages(damaged, pages));  // framing itself stays valid

  ByteVector data = damaged;
  ByteVectorStream stream(data);
  Vorbis::File f(&stream, true, Vorbis::Properties::Average, SerialA);
  TASK_CHECK(f.isValid());  // headers still parse; structure fails at save
  f.tag()->setArtist("must not be written");
  TASK_CHECK(!f.save());

  // Not a single byte of the file may have changed.
  TASK_CHECK(liveBytes(stream) == damaged);

  // A file name based entry must honor the same guarantee.
  TASK_CHECK(writeFile(TempFileName, damaged));
  {
    Vorbis::File nf(TempFileName, true, Vorbis::Properties::Average, SerialA);
    TASK_CHECK(nf.isValid());
    TASK_CHECK(!nf.save());
  }
  ByteVector onDisk;
  TASK_CHECK(readFile(TempFileName, onDisk));
  TASK_CHECK(onDisk == damaged);
  return 0;
}

static int boundaries_reject_gap()
{
  StreamSource a, b;
  TASK_CHECK(loadSources(a, b));
  return run_rejection(buildMuxFixture(a, b, 16, false), edit_gap);
}

static int boundaries_reject_continuation_mismatch()
{
  StreamSource a, b;
  TASK_CHECK(loadSources(a, b));
  return run_rejection(buildMuxFixture(a, b, 16, true),
                       edit_continuation_mismatch);
}

static int boundaries_reject_missing_eos()
{
  StreamSource a, b;
  TASK_CHECK(loadSources(a, b));
  return run_rejection(buildMuxFixture(a, b, 16, false), edit_missing_eos);
}

static int boundaries_reject_dangling_packet()
{
  StreamSource a, b;
  TASK_CHECK(loadSources(a, b));
  return run_rejection(buildMuxFixture(a, b, 16, false), edit_dangling_packet);
}

static int boundaries_reject_no_header_audio_boundary()
{
  StreamSource a, b;
  TASK_CHECK(loadSources(a, b));
  // The first audio packet shares the page on which the setup completes.
  const ByteVector damaged = buildMuxFixture(a, b, 16, false, true);
  std::vector<RawPage> pages;
  TASK_CHECK(parsePages(damaged, pages));

  ByteVector data = damaged;
  ByteVectorStream stream(data);
  Vorbis::File f(&stream, true, Vorbis::Properties::Average, SerialA);
  TASK_CHECK(f.isValid());
  TASK_CHECK(!f.save());
  TASK_CHECK(liveBytes(stream) == damaged);
  return 0;
}

// ============================================================================
// Registration
// ============================================================================

extern const struct task_case task_cases[] = {
  { "selection", "second_stream_iostream",
    selection_second_stream_iostream },
  { "selection", "filename_entry",
    selection_filename_entry },
  { "selection", "unknown_serial_invalid",
    selection_unknown_serial_invalid },
  { "selection", "legacy_single_stream",
    selection_legacy_single_stream },
  { "selection", "skip_audio_properties",
    selection_skip_audio_properties },

  { "rewrite", "short_comment_preserves_everything",
    rewrite_short_comment_preserves_everything },
  { "rewrite", "three_consecutive_saves",
    rewrite_three_consecutive_saves },
  { "rewrite", "comment_spanning_pages_sharing_setup",
    rewrite_comment_spanning_pages_sharing_setup },
  { "rewrite", "audio_continuation_preserved",
    rewrite_audio_continuation_preserved },
  { "rewrite", "foreign_pages_unchanged_and_ordered",
    rewrite_foreign_pages_unchanged_and_ordered },
  { "rewrite", "foreign_page_between_header_pages",
    rewrite_foreign_page_between_header_pages },

  { "boundaries", "lacing_255_and_65025",
    boundaries_lacing_255_and_65025 },
  { "boundaries", "reject_gap",
    boundaries_reject_gap },
  { "boundaries", "reject_continuation_mismatch",
    boundaries_reject_continuation_mismatch },
  { "boundaries", "reject_missing_eos",
    boundaries_reject_missing_eos },
  { "boundaries", "reject_dangling_packet",
    boundaries_reject_dangling_packet },
  { "boundaries", "reject_no_header_audio_boundary",
    boundaries_reject_no_header_audio_boundary },
};

extern const size_t task_case_count =
  sizeof(task_cases) / sizeof(task_cases[0]);

