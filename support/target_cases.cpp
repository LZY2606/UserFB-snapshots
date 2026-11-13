// Regression material for the two ID3v2 unsynchronisation readers:
//
//   * ID3v2.3 restores unsynchronisation once, over the whole tag body.
//   * ID3v2.4 restores it per frame, after the on-disk frame length is known.
//
// Every byte checked below is produced by the small writers in this file.
// Neither ID3v2::Tag::render(), SynchData::fromUInt() nor SynchData::decode()
// are used to construct an expected value: the expectation is the frame body
// that was written before unsynchronisation was applied.

#include <tbytevector.h>
#include <tbytevectorstream.h>
#include <tstring.h>
#include <tstringlist.h>
#include <mpegfile.h>
#include <id3v2tag.h>
#include <id3v2frame.h>
#include <id3v2header.h>
#include <id3v2framefactory.h>
#include <privateframe.h>
#include <textidentificationframe.h>
#include <unknownframe.h>

using namespace TagLib;

namespace
{

// ---------------------------------------------------------------------------
// Independent integer writers.  No TagLib conversion helpers are used.
// ---------------------------------------------------------------------------

// Ordinary unsigned 32 bit big endian integer (ID3v2.3 frame lengths).
ByteVector beUInt(unsigned int value)
{
  ByteVector v(4, 0);
  for(unsigned int i = 0; i < 4; ++i)
    v[i] = static_cast<char>((value >> ((3 - i) * 8)) & 0xff);
  return v;
}

// 28 bit syncsafe integer (ID3v2 tag sizes, ID3v2.4 frame lengths, DLI).
ByteVector syncsafeUInt(unsigned int value)
{
  ByteVector v(4, 0);
  for(unsigned int i = 0; i < 4; ++i)
    v[i] = static_cast<char>((value >> ((3 - i) * 7)) & 0x7f);
  return v;
}

// ---------------------------------------------------------------------------
// Independent unsynchronisation encoder: a 0x00 byte is inserted after every
// 0xff byte, unconditionally.
// ---------------------------------------------------------------------------

ByteVector escapeFF(const ByteVector &in)
{
  ByteVector out;
  for(unsigned int i = 0; i < in.size(); ++i) {
    out.append(in[i]);
    if(in[i] == static_cast<char>(0xff))
      out.append(static_cast<char>(0x00));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Independent header / frame / tag writers.
// ---------------------------------------------------------------------------

ByteVector id3Header(unsigned int version, unsigned int tagSizeOnDisk,
                     bool unsynchronisation)
{
  ByteVector header("ID3", 3);
  header.append(static_cast<char>(version));
  header.append(static_cast<char>(0));
  header.append(static_cast<char>(unsynchronisation ? 0x80 : 0x00));
  header.append(syncsafeUInt(tagSizeOnDisk));
  return header;
}

ByteVector frameV3(const char *id, const ByteVector &content)
{
  ByteVector header(id, 4);
  header.append(beUInt(content.size()));
  header.append(ByteVector(2, 0));
  return header + content;
}

// Build one ID3v2.4 frame.  The frame length written to disk is the length of
// the data actually stored (after escaping).  When a data length indicator is
// requested, its 4 byte syncsafe prefix holds the length of the restored body
// (excluding the indicator itself).
ByteVector frameV4(const char *id, const ByteVector &content,
                   bool dataLengthIndicator, bool frameUnsynchronisation,
                   bool tagUnsynchronisation)
{
  ByteVector body;
  if(dataLengthIndicator)
    body.append(syncsafeUInt(content.size()));
  body.append(content);

  const bool encode = frameUnsynchronisation || tagUnsynchronisation;
  const ByteVector stored = encode ? escapeFF(body) : body;

  ByteVector header(id, 4);
  header.append(syncsafeUInt(stored.size()));
  header.append(static_cast<char>(0));
  header.append(static_cast<char>((dataLengthIndicator ? 0x01 : 0x00) |
                                  (frameUnsynchronisation ? 0x02 : 0x00)));
  return header + stored;
}

// Assemble a complete v2.3 tag.  Frame lengths inside the body are restored
// lengths; the whole body is escaped afterwards, so the tag header size counts
// the bytes that are really on disk.
ByteVector tagV3(const ByteVector &frames, bool unsynchronisation,
                 unsigned int paddingSize)
{
  ByteVector body = frames;
  body.append(ByteVector(paddingSize, 0));
  const ByteVector onDisk = unsynchronisation ? escapeFF(body) : body;
  return id3Header(3, onDisk.size(), unsynchronisation) + onDisk;
}

// Assemble a complete v2.4 tag.  Each frame carries its own stored length.
ByteVector tagV4(const ByteVector &frames, bool tagUnsynchronisation,
                 unsigned int paddingSize)
{
  ByteVector body = frames;
  body.append(ByteVector(paddingSize, 0));
  return id3Header(4, body.size(), tagUnsynchronisation) + body;
}

// ---------------------------------------------------------------------------
// Raw content material.
// ---------------------------------------------------------------------------

ByteVector ownerBytes()
{
  return ByteVector("fb-owner@example.test");
}

ByteVector privateContent(const ByteVector &payload)
{
  ByteVector content = ownerBytes();
  content.append(static_cast<char>(0x00));
  content.append(payload);
  return content;
}

ByteVector textContent(const char *text)
{
  ByteVector content(1, static_cast<char>(0x00)); // Latin1
  content.append(ByteVector(text));
  return content;
}

// Binary payload that contains every shape needed to tell a wrong decoder
// apart: 0xff 0xe1, a genuine 0xff 0x00, consecutive 0xff bytes, a trailing
// 0xff and ordinary zero bytes.  Filler is plain ASCII.
ByteVector specialPayload(unsigned int size)
{
  ByteVector payload(size, static_cast<char>(0x00));
  for(unsigned int i = 0; i < size; ++i)
    payload[i] = static_cast<char>('a' + (i % 26));

  payload[0] = static_cast<char>(0xff);
  payload[1] = static_cast<char>(0xe1);
  payload[2] = static_cast<char>(0xff);
  payload[3] = static_cast<char>(0x00); // genuine 0xff 0x00
  payload[4] = static_cast<char>(0xff);
  payload[5] = static_cast<char>(0xff); // consecutive 0xff
  payload[6] = static_cast<char>(0x00); // ordinary zero
  payload[7] = static_cast<char>(0x00); // ordinary zero
  payload[size - 1] = static_cast<char>(0xff); // trailing 0xff
  return payload;
}

// Length that the escaped form of \a in occupies on disk.
unsigned int escapedLength(const ByteVector &in)
{
  unsigned int length = in.size();
  for(unsigned int i = 0; i < in.size(); ++i)
    if(in[i] == static_cast<char>(0xff))
      ++length;
  return length;
}

// Payload with no byte that unsynchronisation would touch.
ByteVector plainPayload(unsigned int size)
{
  ByteVector payload(size, static_cast<char>(0x00));
  for(unsigned int i = 0; i < size; ++i)
    payload[i] = static_cast<char>(0x20 + (i % 0x5e));
  return payload;
}

// ---------------------------------------------------------------------------
// Full-tag read path: ByteVectorStream + MPEG::File.
// ---------------------------------------------------------------------------

struct TagReader
{
  ByteVectorStream stream;
  MPEG::File file;

  explicit TagReader(const ByteVector &data) :
    stream(data),
    file(&stream, ID3v2::FrameFactory::instance(), false)
  {
  }

  ID3v2::Tag *tag()
  {
    return file.ID3v2Tag();
  }
};

// ---------------------------------------------------------------------------
// Byte-exact assertions.
// ---------------------------------------------------------------------------

void checkPrivBytes(ID3v2::Tag *tag, const ByteVector &expectedPayload)
{
  FB_CHECK(tag != 0);
  const ID3v2::FrameList &list = tag->frameList("PRIV");
  FB_CHECK(list.size() == 1);
  const ID3v2::PrivateFrame *frame =
    dynamic_cast<const ID3v2::PrivateFrame *>(list.front());
  FB_CHECK(frame != 0);
  FB_CHECK(frame->owner().data(String::Latin1) == ownerBytes());
  FB_CHECK(frame->data() == expectedPayload);
}

void checkTextBytes(ID3v2::Tag *tag, const char *id, const char *value)
{
  FB_CHECK(tag != 0);
  const ID3v2::FrameList &list = tag->frameList(ByteVector(id));
  FB_CHECK(list.size() == 1);
  const ID3v2::TextIdentificationFrame *frame =
    dynamic_cast<const ID3v2::TextIdentificationFrame *>(list.front());
  FB_CHECK(frame != 0);
  const StringList fields = frame->fieldList();
  FB_CHECK(!fields.isEmpty());
  FB_CHECK(fields.front().data(String::Latin1) == ByteVector(value));
}

void checkFrameCount(ID3v2::Tag *tag, unsigned int expected)
{
  FB_CHECK(tag != 0);
  FB_CHECK(tag->frameList().size() == expected);
}

// Local comparison via the FrameFactory entry point.  This supplements the
// whole-tag paths, it does not replace them.
void checkFactoryPriv(const ByteVector &tagHeaderBytes,
                      const ByteVector &frameBytes,
                      const ByteVector &expectedPayload)
{
  ID3v2::Header tagHeader(tagHeaderBytes);
  ID3v2::Frame *frame =
    ID3v2::FrameFactory::instance()->createFrame(frameBytes, &tagHeader);
  FB_CHECK(frame != 0);
  const ID3v2::PrivateFrame *priv =
    dynamic_cast<const ID3v2::PrivateFrame *>(frame);
  FB_CHECK(priv != 0);
  FB_CHECK(priv->owner().data(String::Latin1) == ownerBytes());
  FB_CHECK(priv->data() == expectedPayload);
  delete frame;
}

// Build the simple two-frame body used by most cases: PRIV followed by a
// recognisable TIT2.
ByteVector privAndTitle(const ByteVector &payload, const char *title)
{
  return frameV3("PRIV", privateContent(payload)) +
         frameV3("TIT2", textContent(title));
}

// ===========================================================================
// Group tag23: ID3v2.3 restores unsynchronisation over the whole tag body.
// ===========================================================================

FB_TEST("tag23", tag23_whole_body_special_bytes)
{
  const ByteVector payload = specialPayload(100);
  const ByteVector frames = privAndTitle(payload, "AFTER-PRIV");
  const ByteVector file = tagV3(frames, true, 64);
  // The frame lengths inside the body are restored lengths, so the body on
  // disk is longer than frames + padding once 0xff bytes are escaped.
  FB_CHECK(file.size() == 10 + escapedLength(frames) + 64);
  FB_CHECK(file.size() > 10 + frames.size() + 64);
  TagReader reader(file);
  FB_CHECK(reader.file.hasID3v2Tag());
  ID3v2::Tag *tag = reader.tag();
  FB_CHECK(tag != 0);
  checkPrivBytes(tag, payload);
  checkTextBytes(tag, "TIT2", "AFTER-PRIV");
  checkFrameCount(tag, 2);
}

FB_TEST("tag23", tag23_payload_crosses_127)
{
  const ByteVector payload127 = specialPayload(127);
  const ByteVector payload128 = specialPayload(128);

  TagReader reader127(tagV3(privAndTitle(payload127, "NEXT-127"), true, 64));
  checkPrivBytes(reader127.tag(), payload127);
  checkTextBytes(reader127.tag(), "TIT2", "NEXT-127");
  checkFrameCount(reader127.tag(), 2);

  TagReader reader128(tagV3(privAndTitle(payload128, "NEXT-128"), true, 64));
  checkPrivBytes(reader128.tag(), payload128);
  checkTextBytes(reader128.tag(), "TIT2", "NEXT-128");
  checkFrameCount(reader128.tag(), 2);
}

FB_TEST("tag23", tag23_payload_crosses_16383)
{
  const ByteVector payload16383 = specialPayload(16383);
  const ByteVector payload16384 = specialPayload(16384);

  TagReader readerA(tagV3(privAndTitle(payload16383, "BIG-16383"), true, 64));
  checkPrivBytes(readerA.tag(), payload16383);
  checkTextBytes(readerA.tag(), "TIT2", "BIG-16383");
  checkFrameCount(readerA.tag(), 2);

  TagReader readerB(tagV3(privAndTitle(payload16384, "BIG-16384"), true, 64));
  checkPrivBytes(readerB.tag(), payload16384);
  checkTextBytes(readerB.tag(), "TIT2", "BIG-16384");
  checkFrameCount(readerB.tag(), 2);
}

FB_TEST("tag23", tag23_plain_tag_without_escapable_bytes)
{
  const ByteVector payload = plainPayload(200);
  const ByteVector file = tagV3(privAndTitle(payload, "PLAIN-TAG"), false, 64);
  TagReader reader(file);
  ID3v2::Tag *tag = reader.tag();
  FB_CHECK(tag != 0);
  checkPrivBytes(tag, payload);
  checkTextBytes(tag, "TIT2", "PLAIN-TAG");
  checkFrameCount(tag, 2);
  FB_CHECK(tag->header()->majorVersion() == 3);
}

// ===========================================================================
// Group frame24: ID3v2.4 restores unsynchronisation per frame.
// ===========================================================================

FB_TEST("frame24", frame24_tag_level_flag)
{
  const ByteVector payload = specialPayload(100);
  const ByteVector frames =
    frameV4("PRIV", privateContent(payload), false, false, true) +
    frameV4("TIT2", textContent("AFTER-PRIV"), false, false, true);
  TagReader reader(tagV4(frames, true, 64));
  ID3v2::Tag *tag = reader.tag();
  FB_CHECK(tag != 0);
  checkPrivBytes(tag, payload);
  checkTextBytes(tag, "TIT2", "AFTER-PRIV");
  checkFrameCount(tag, 2);
  FB_CHECK(tag->header()->majorVersion() == 4);
  FB_CHECK(tag->header()->unsynchronisation());
}

FB_TEST("frame24", frame24_frame_level_flag)
{
  const ByteVector payload = specialPayload(100);
  const ByteVector frames =
    frameV4("PRIV", privateContent(payload), false, true, false) +
    frameV4("TIT2", textContent("AFTER-PRIV"), false, false, false);
  TagReader reader(tagV4(frames, false, 64));
  ID3v2::Tag *tag = reader.tag();
  FB_CHECK(tag != 0);
  checkPrivBytes(tag, payload);
  checkTextBytes(tag, "TIT2", "AFTER-PRIV");
  checkFrameCount(tag, 2);
  FB_CHECK(!tag->header()->unsynchronisation());
}

FB_TEST("frame24", frame24_both_flags_restore_once)
{
  // Both the tag and the frame advertise unsynchronisation.  The body must be
  // restored exactly once; the genuine 0xff 0x00 in the payload would lose its
  // zero byte if it were restored a second time.
  const ByteVector payload = specialPayload(100);
  const ByteVector frames =
    frameV4("PRIV", privateContent(payload), false, true, true) +
    frameV4("TIT2", textContent("AFTER-PRIV"), false, false, true);
  TagReader reader(tagV4(frames, true, 64));
  ID3v2::Tag *tag = reader.tag();
  FB_CHECK(tag != 0);
  checkPrivBytes(tag, payload);
  checkTextBytes(tag, "TIT2", "AFTER-PRIV");
  checkFrameCount(tag, 2);
  FB_CHECK(tag->header()->unsynchronisation());
}

FB_TEST("frame24", frame24_dli_frame_level)
{
  const ByteVector payload = specialPayload(120);
  const ByteVector frames =
    frameV4("PRIV", privateContent(payload), true, true, false) +
    frameV4("TIT2", textContent("AFTER-DLI"), false, false, false);
  TagReader reader(tagV4(frames, false, 64));
  ID3v2::Tag *tag = reader.tag();
  FB_CHECK(tag != 0);
  checkPrivBytes(tag, payload);
  checkTextBytes(tag, "TIT2", "AFTER-DLI");
  checkFrameCount(tag, 2);
}

FB_TEST("frame24", frame24_dli_tag_level)
{
  const ByteVector payload = specialPayload(150);
  const ByteVector frames =
    frameV4("PRIV", privateContent(payload), true, false, true) +
    frameV4("TIT2", textContent("AFTER-DLI"), false, false, true);
  TagReader reader(tagV4(frames, true, 64));
  ID3v2::Tag *tag = reader.tag();
  FB_CHECK(tag != 0);
  checkPrivBytes(tag, payload);
  checkTextBytes(tag, "TIT2", "AFTER-DLI");
  checkFrameCount(tag, 2);
}

FB_TEST("frame24", frame24_plain_tag_without_escapable_bytes)
{
  const ByteVector payload = plainPayload(200);
  const ByteVector frames =
    frameV4("PRIV", privateContent(payload), false, false, false) +
    frameV4("TIT2", textContent("PLAIN-TAG"), false, false, false);
  TagReader reader(tagV4(frames, false, 64));
  ID3v2::Tag *tag = reader.tag();
  FB_CHECK(tag != 0);
  checkPrivBytes(tag, payload);
  checkTextBytes(tag, "TIT2", "PLAIN-TAG");
  checkFrameCount(tag, 2);
  FB_CHECK(!tag->header()->unsynchronisation());
}

FB_TEST("frame24", frame24_factory_tag_level)
{
  const ByteVector payload = specialPayload(100);
  const ByteVector frameBytes =
    frameV4("PRIV", privateContent(payload), false, false, true);
  checkFactoryPriv(id3Header(4, 0, true), frameBytes, payload);
}

FB_TEST("frame24", frame24_factory_frame_level)
{
  const ByteVector payload = specialPayload(130);
  const ByteVector frameBytes =
    frameV4("PRIV", privateContent(payload), false, true, false);
  checkFactoryPriv(id3Header(4, 0, false), frameBytes, payload);
}

FB_TEST("frame24", frame24_factory_dli)
{
  const ByteVector payload = specialPayload(160);
  const ByteVector frameBytes =
    frameV4("PRIV", privateContent(payload), true, true, false);
  checkFactoryPriv(id3Header(4, 0, false), frameBytes, payload);
}

// ===========================================================================
// Group lengths: the two different length conventions, size boundaries and
// the externally visible behaviour for malformed lengths.
// ===========================================================================

FB_TEST("lengths", lengths_dli_crosses_127)
{
  // The DLI prefix records the restored body length (owner + NUL + payload).
  // Choose those body lengths to be exactly 127 and 128.
  const ByteVector payload127 = specialPayload(105);
  const ByteVector payload128 = specialPayload(106);
  FB_CHECK(privateContent(payload127).size() == 127);
  FB_CHECK(privateContent(payload128).size() == 128);

  const ByteVector framesA =
    frameV4("PRIV", privateContent(payload127), true, true, false) +
    frameV4("TIT2", textContent("DLI-127"), false, false, false);
  TagReader readerA(tagV4(framesA, false, 64));
  checkPrivBytes(readerA.tag(), payload127);
  checkTextBytes(readerA.tag(), "TIT2", "DLI-127");
  checkFrameCount(readerA.tag(), 2);

  const ByteVector framesB =
    frameV4("PRIV", privateContent(payload128), true, true, false) +
    frameV4("TIT2", textContent("DLI-128"), false, false, false);
  TagReader readerB(tagV4(framesB, false, 64));
  checkPrivBytes(readerB.tag(), payload128);
  checkTextBytes(readerB.tag(), "TIT2", "DLI-128");
  checkFrameCount(readerB.tag(), 2);
}

FB_TEST("lengths", lengths_dli_crosses_16383)
{
  // Restored body lengths 16383 and 16384, encoded in the DLI prefix.
  const ByteVector payload16383 = specialPayload(16361);
  const ByteVector payload16384 = specialPayload(16362);
  FB_CHECK(privateContent(payload16383).size() == 16383);
  FB_CHECK(privateContent(payload16384).size() == 16384);

  const ByteVector framesA =
    frameV4("PRIV", privateContent(payload16383), true, true, false) +
    frameV4("TIT2", textContent("DLI-BIG-16383"), false, false, false);
  TagReader readerA(tagV4(framesA, false, 64));
  checkPrivBytes(readerA.tag(), payload16383);
  checkTextBytes(readerA.tag(), "TIT2", "DLI-BIG-16383");
  checkFrameCount(readerA.tag(), 2);

  const ByteVector framesB =
    frameV4("PRIV", privateContent(payload16384), true, true, false) +
    frameV4("TIT2", textContent("DLI-BIG-16384"), false, false, false);
  TagReader readerB(tagV4(framesB, false, 64));
  checkPrivBytes(readerB.tag(), payload16384);
  checkTextBytes(readerB.tag(), "TIT2", "DLI-BIG-16384");
  checkFrameCount(readerB.tag(), 2);
}

FB_TEST("lengths", lengths_no_dli_crosses_16383)
{
  // Without DLI the frame header length is the escaped on-disk length.  Choose
  // payloads so that this stored length is exactly 16383 and 16384, i.e. the
  // syncsafe frame length itself crosses the two-byte boundary.
  const ByteVector payload16383 = specialPayload(16356);
  const ByteVector payload16384 = specialPayload(16357);
  FB_CHECK(escapedLength(privateContent(payload16383)) == 16383);
  FB_CHECK(escapedLength(privateContent(payload16384)) == 16384);

  const ByteVector framesA =
    frameV4("PRIV", privateContent(payload16383), false, true, false) +
    frameV4("TIT2", textContent("STORED-BIG-16383"), false, false, false);
  TagReader readerA(tagV4(framesA, false, 64));
  checkPrivBytes(readerA.tag(), payload16383);
  checkTextBytes(readerA.tag(), "TIT2", "STORED-BIG-16383");
  checkFrameCount(readerA.tag(), 2);

  const ByteVector framesB =
    frameV4("PRIV", privateContent(payload16384), false, true, false) +
    frameV4("TIT2", textContent("STORED-BIG-16384"), false, false, false);
  TagReader readerB(tagV4(framesB, false, 64));
  checkPrivBytes(readerB.tag(), payload16384);
  checkTextBytes(readerB.tag(), "TIT2", "STORED-BIG-16384");
  checkFrameCount(readerB.tag(), 2);
}

FB_TEST("lengths", lengths_adjacent_frames_different_lengths)
{
  // Two PRIV frames of clearly different lengths back to back, mixing a DLI
  // frame with a plain one, followed by a text frame.
  const ByteVector shortPayload = specialPayload(60);
  const ByteVector longPayload = specialPayload(4200);
  const ByteVector frames =
    frameV4("PRIV", privateContent(shortPayload), true, true, false) +
    frameV4("PRIV", privateContent(longPayload), false, true, false) +
    frameV4("TIT2", textContent("AFTER-TWO-PRIV"), false, false, false);

  TagReader reader(tagV4(frames, false, 64));
  ID3v2::Tag *tag = reader.tag();
  FB_CHECK(tag != 0);
  const ID3v2::FrameList &list = tag->frameList("PRIV");
  FB_CHECK(list.size() == 2);

  const ID3v2::PrivateFrame *first =
    dynamic_cast<const ID3v2::PrivateFrame *>(list[0]);
  FB_CHECK(first != 0);
  FB_CHECK(first->owner().data(String::Latin1) == ownerBytes());
  FB_CHECK(first->data() == shortPayload);

  const ID3v2::PrivateFrame *second =
    dynamic_cast<const ID3v2::PrivateFrame *>(list[1]);
  FB_CHECK(second != 0);
  FB_CHECK(second->owner().data(String::Latin1) == ownerBytes());
  FB_CHECK(second->data() == longPayload);

  checkTextBytes(tag, "TIT2", "AFTER-TWO-PRIV");
  checkFrameCount(tag, 3);
}

FB_TEST("lengths", lengths_truncated_frame_stops_tag)
{
  // A frame header claims 500 bytes, far more than the rest of the tag body
  // (10 body bytes plus padding).  The current reader rejects this frame and
  // stops parsing the tag.
  ByteVector frame("PRIV", 4);
  frame.append(syncsafeUInt(500));
  frame.append(ByteVector(2, 0));
  frame.append(ByteVector(10, static_cast<char>(0x41)));
  TagReader reader(tagV4(frame, false, 80));
  checkFrameCount(reader.tag(), 0);
}

FB_TEST("lengths", lengths_dli_without_body_stops_tag)
{
  // A frame with the DLI flag and a frame length of exactly four: only the
  // indicator, no body.  The current reader rejects this frame.
  ByteVector frame("PRIV", 4);
  frame.append(syncsafeUInt(4));
  frame.append(static_cast<char>(0));
  frame.append(static_cast<char>(0x01));
  frame.append(syncsafeUInt(0));
  TagReader reader(tagV4(frame, false, 80));
  checkFrameCount(reader.tag(), 0);
}

FB_TEST("lengths", lengths_invalid_frame_id_stops_tag)
{
  const ByteVector payload = specialPayload(80);
  ByteVector frame("PR!V", 4);
  frame.append(syncsafeUInt(privateContent(payload).size()));
  frame.append(ByteVector(2, 0));
  frame.append(privateContent(payload));
  const ByteVector frames =
    frame + frameV4("TIT2", textContent("MUST-NOT-BE-READ"), false, false, false);
  TagReader reader(tagV4(frames, false, 80));
  checkFrameCount(reader.tag(), 0);
}

FB_TEST("lengths", lengths_stops_at_truncated_successor)
{
  // A valid PRIV frame, then a truncated successor, then a text frame that
  // must not be reached.  The tag keeps only the frame that parsed.
  const ByteVector payload = specialPayload(80);
  ByteVector truncated("PRIV", 4);
  truncated.append(syncsafeUInt(500));
  truncated.append(ByteVector(2, 0));
  truncated.append(ByteVector(10, static_cast<char>(0x42)));
  const ByteVector frames =
    frameV4("PRIV", privateContent(payload), false, true, false) +
    truncated +
    frameV4("TIT2", textContent("MUST-NOT-BE-READ"), false, false, false);

  TagReader reader(tagV4(frames, false, 80));
  ID3v2::Tag *tag = reader.tag();
  FB_CHECK(tag != 0);
  checkPrivBytes(tag, payload);
  FB_CHECK(tag->frameList("TIT2").isEmpty());
  checkFrameCount(tag, 1);
}

FB_TEST("lengths", lengths_stops_at_invalid_successor_id)
{
  const ByteVector payload = specialPayload(80);
  ByteVector invalid("PR!V", 4);
  invalid.append(syncsafeUInt(privateContent(payload).size()));
  invalid.append(ByteVector(2, 0));
  invalid.append(privateContent(payload));
  const ByteVector frames =
    frameV4("PRIV", privateContent(payload), false, true, false) +
    invalid +
    frameV4("TIT2", textContent("MUST-NOT-BE-READ"), false, false, false);

  TagReader reader(tagV4(frames, false, 80));
  ID3v2::Tag *tag = reader.tag();
  FB_CHECK(tag != 0);
  checkPrivBytes(tag, payload);
  FB_CHECK(tag->frameList("TIT2").isEmpty());
  checkFrameCount(tag, 1);
}

// ===========================================================================
// Group neighbors: the next frame must not be consumed or shifted by a
// wrongly restored predecessor, and rendered tags must keep binary meaning.
// ===========================================================================

FB_TEST("neighbors", neighbors_v3_whole_body)
{
  const ByteVector payload = specialPayload(100);
  const ByteVector frames =
    frameV3("PRIV", privateContent(payload)) +
    frameV3("TIT2", textContent("NEIGHBOR-TITLE")) +
    frameV3("TALB", textContent("NEIGHBOR-ALBUM"));
  TagReader reader(tagV3(frames, true, 64));
  ID3v2::Tag *tag = reader.tag();
  FB_CHECK(tag != 0);
  checkPrivBytes(tag, payload);
  checkTextBytes(tag, "TIT2", "NEIGHBOR-TITLE");
  checkTextBytes(tag, "TALB", "NEIGHBOR-ALBUM");
  checkFrameCount(tag, 3);
}

FB_TEST("neighbors", neighbors_v4_frame_level)
{
  const ByteVector payload = specialPayload(100);
  const ByteVector frames =
    frameV4("PRIV", privateContent(payload), false, true, false) +
    frameV4("TIT2", textContent("NEIGHBOR-TITLE"), false, false, false) +
    frameV4("TALB", textContent("NEIGHBOR-ALBUM"), false, false, false);
  TagReader reader(tagV4(frames, false, 64));
  ID3v2::Tag *tag = reader.tag();
  FB_CHECK(tag != 0);
  checkPrivBytes(tag, payload);
  checkTextBytes(tag, "TIT2", "NEIGHBOR-TITLE");
  checkTextBytes(tag, "TALB", "NEIGHBOR-ALBUM");
  checkFrameCount(tag, 3);
}

// Build a normal tag object (not handcrafted wire data) for the render checks.
ByteVector renderedTag(ID3v2::Version version, const ByteVector &unknownPayload,
                       const ByteVector &privatePayload, const char *title)
{
  ID3v2::Tag tag;

  // An unknown frame keeps arbitrary binary content.
  ByteVector unknownFrameData("XUNI", 4);
  unknownFrameData.append(syncsafeUInt(unknownPayload.size()));
  unknownFrameData.append(ByteVector(2, 0));
  unknownFrameData.append(unknownPayload);
  tag.addFrame(new ID3v2::UnknownFrame(unknownFrameData));

  ID3v2::PrivateFrame *priv = new ID3v2::PrivateFrame();
  priv->setOwner("fb-owner@example.test");
  priv->setData(privatePayload);
  tag.addFrame(priv);

  ID3v2::TextIdentificationFrame *text =
    new ID3v2::TextIdentificationFrame("TIT2", String::Latin1);
  text->setText(title);
  tag.addFrame(text);

  return tag.render(version);
}

void checkRenderedTag(const ByteVector &rendered, unsigned int majorVersion,
                      const ByteVector &unknownPayload,
                      const ByteVector &privatePayload, const char *title)
{
  TagReader reader(rendered);
  FB_CHECK(reader.file.hasID3v2Tag());
  ID3v2::Tag *tag = reader.tag();
  FB_CHECK(tag != 0);

  const ID3v2::FrameList &unknownList = tag->frameList("XUNI");
  FB_CHECK(unknownList.size() == 1);
  const ID3v2::UnknownFrame *unknown =
    dynamic_cast<const ID3v2::UnknownFrame *>(unknownList.front());
  FB_CHECK(unknown != 0);
  FB_CHECK(unknown->data() == unknownPayload);

  checkPrivBytes(tag, privatePayload);
  checkTextBytes(tag, "TIT2", title);
  checkFrameCount(tag, 3);
  FB_CHECK(tag->header()->majorVersion() == majorVersion);
}

FB_TEST("neighbors", neighbors_render_v3_keeps_content)
{
  const ByteVector unknownPayload = specialPayload(64);
  const ByteVector privatePayload = specialPayload(300);
  const ByteVector rendered =
    renderedTag(ID3v2::v3, unknownPayload, privatePayload, "RENDERED-NEXT");
  checkRenderedTag(rendered, 3, unknownPayload, privatePayload,
                   "RENDERED-NEXT");
}

FB_TEST("neighbors", neighbors_render_v4_keeps_content)
{
  const ByteVector unknownPayload = specialPayload(64);
  const ByteVector privatePayload = specialPayload(300);
  const ByteVector rendered =
    renderedTag(ID3v2::v4, unknownPayload, privatePayload, "RENDERED-NEXT");
  checkRenderedTag(rendered, 4, unknownPayload, privatePayload,
                   "RENDERED-NEXT");
}

}  // namespace
