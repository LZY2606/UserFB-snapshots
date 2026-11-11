'use strict';
/**
 * Regression coverage for the splitVideoConfig mode of VideoSegmentStream and
 * the video-only Transmuxer. Driven only through the public stream entry
 * points with Node assert; every structural claim is read back from the
 * generated initSegment, moof and mdat bytes.
 */
const assert = require('node:assert/strict');
const {VideoSegmentStream, Transmuxer} = require('../../lib/mp4/transmuxer');
const mp4 = require('../../lib/mp4/mp4-generator');

// ---------------------------------------------------------------------------
// small MP4 box reader (only what these assertions need)
// ---------------------------------------------------------------------------
function boxesOf(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const boxes = [];
  let offset = 0;

  while (offset + 8 <= bytes.byteLength) {
    const size = view.getUint32(offset);
    const type = String.fromCharCode(bytes[offset + 4], bytes[offset + 5],
      bytes[offset + 6], bytes[offset + 7]);

    if (size < 8 || offset + size > bytes.byteLength) {
      break;
    }
    boxes.push({type, start: offset, header: 8, payload: bytes.subarray(offset + 8, offset + size)});
    offset += size;
  }
  return boxes;
}

function findBox(bytes, path) {
  let current = bytes;
  let parentType = null;

  for (const wanted of path) {
    let searchable = current;

    if (parentType === 'stsd') {
      // stsd is a FullBox with an 8-byte version/flags/entry_count preamble
      searchable = current.subarray(8);
    } else if (parentType === 'avc1') {
      // an avc1 sample entry has 78 fixed bytes before its child boxes
      searchable = current.subarray(78);
    }
    const match = boxesOf(searchable).find((box) => box.type === wanted);

    assert.ok(match, 'expected box ' + wanted + ' under ' + path.join('/'));
    current = match.payload;
    parentType = wanted;
  }
  return current;
}

function readAvc1(initSegment) {
  const avc1 = findBox(initSegment, ['moov', 'trak', 'mdia', 'minf', 'stbl', 'stsd', 'avc1']);
  const avc1View = new DataView(avc1.buffer, avc1.byteOffset, avc1.byteLength);
  const avcC = findBox(initSegment,
    ['moov', 'trak', 'mdia', 'minf', 'stbl', 'stsd', 'avc1', 'avcC']);
  const avcCView = new DataView(avcC.buffer, avcC.byteOffset, avcC.byteLength);

  let offset = 6;
  const spsCount = avcC[5] & 0x1f;
  const sps = [];

  for (let i = 0; i < spsCount; i++) {
    const nalLen = avcCView.getUint16(offset);

    offset += 2;
    sps.push(Array.from(avcC.subarray(offset, offset + nalLen)));
    offset += nalLen;
  }

  const ppsCount = avcC[offset];
  offset += 1;
  const pps = [];

  for (let i = 0; i < ppsCount; i++) {
    const nalLen = avcCView.getUint16(offset);

    offset += 2;
    pps.push(Array.from(avcC.subarray(offset, offset + nalLen)));
    offset += nalLen;
  }

  return {
    width: avc1View.getUint16(24),
    height: avc1View.getUint16(26),
    sps,
    pps
  };
}

function readMoof(boxes) {
  const moofBox = boxesOf(boxes).find((box) => box.type === 'moof');

  assert.ok(moofBox, 'moof box missing');
  const mfhd = findBox(boxes, ['moof', 'mfhd']);
  const tfdt = findBox(boxes, ['moof', 'traf', 'tfdt']);
  const trun = findBox(boxes, ['moof', 'traf', 'trun']);
  const trunView = new DataView(trun.buffer, trun.byteOffset, trun.byteLength);
  const sampleCount = trunView.getUint32(4);
  const dataOffset = trunView.getInt32(8);
  const samples = [];

  // video trun entries are always duration + size + flags + composition offset
  for (let i = 0, entry = 12; i < sampleCount; i++, entry += 16) {
    samples.push({
      duration: trunView.getUint32(entry),
      size: trunView.getUint32(entry + 4),
      compositionTimeOffset: trunView.getInt32(entry + 12)
    });
  }

  const tfdtView = new DataView(tfdt.buffer, tfdt.byteOffset, tfdt.byteLength);
  const baseMediaDecodeTime = tfdt[0] === 1 ?
    tfdtView.getUint32(4) * 4294967296 + tfdtView.getUint32(8) :
    tfdtView.getUint32(4);

  return {
    sequenceNumber: new DataView(mfhd.buffer, mfhd.byteOffset, mfhd.byteLength).getUint32(4),
    baseMediaDecodeTime,
    dataOffset,
    samples
  };
}

function readMdatNals(boxes) {
  const mdatBox = boxesOf(boxes).find((box) => box.type === 'mdat');

  assert.ok(mdatBox, 'mdat box missing');
  const payload = mdatBox.payload;
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const nals = [];
  let offset = 0;

  while (offset + 4 <= payload.byteLength) {
    const length = view.getUint32(offset);

    offset += 4;
    if (!length || offset + length > payload.byteLength) {
      break;
    }
    const nal = payload.subarray(offset, offset + length);

    nals.push({typeCode: nal[0] & 0x1f, bytes: Array.from(nal)});
    offset += length;
  }
  return nals;
}

// ---------------------------------------------------------------------------
// NAL unit helpers for the direct VideoSegmentStream entry point
// ---------------------------------------------------------------------------
function nal(type, typeCode, dts, bytes, config) {
  return {
    nalUnitType: type,
    nalUnitTypeCode: typeCode,
    data: Uint8Array.from(bytes),
    dts,
    pts: dts,
    config
  };
}
function aud(dts) {
  return nal('access_unit_delimiter_rbsp', 9, dts, [9, 0x10]);
}
function spsNal(dts, tag, width, height) {
  return nal('seq_parameter_set_rbsp', 7, dts, [0x67].concat(tag),
    {width, height, profileIdc: 66, levelIdc: 30, profileCompatibility: 0});
}
function ppsNal(dts, tag) {
  return nal('pic_parameter_set_rbsp', 8, dts, [0x68].concat(tag));
}
function idrNal(dts, tag) {
  return nal('slice_layer_without_partitioning_rbsp_idr', 5, dts, [0x65].concat(tag));
}
function nonIdrNal(dts, tag) {
  // H264Stream leaves non-IDR slices unnamed, only the type code is reliable
  return nal(undefined, 1, dts, [0x61].concat(tag));
}

// a fully configured IDR access unit; `tag` identifies the SPS/PPS bytes
function idrAccessUnit(stream, dts, tag, width, height) {
  stream.push(aud(dts));
  stream.push(spsNal(dts, tag, width, height));
  stream.push(ppsNal(dts, tag));
  stream.push(idrNal(dts, tag));
}

function makeTrack(baseMediaDecodeTime) {
  return {
    id: 1,
    type: 'video',
    timelineStartInfo: {dts: 0, pts: 0, baseMediaDecodeTime: baseMediaDecodeTime || 0}
  };
}

function collect(stream) {
  const events = [];
  const errors = [];
  let doneCount = 0;

  stream.on('data', (event) => events.push(event));
  stream.on('error', (err) => errors.push(err));
  stream.on('done', () => doneCount++);
  return {events, errors, doneCount: () => doneCount};
}

// Read the init segment that CoalesceStream would build for this snapshot.
function initFor(event) {
  return mp4.initSegment([event.track]);
}

// ---------------------------------------------------------------------------
// 1. A/A/B/A ordering produces three fragments, each with matching config
// ---------------------------------------------------------------------------
(function aabaSplitsOnByteIdentity() {
  const stream = new VideoSegmentStream(makeTrack(),
    {keepOriginalTimestamps: true, splitVideoConfig: true});
  const out = collect(stream);

  idrAccessUnit(stream, 0, 1, 640, 360);      // A
  idrAccessUnit(stream, 3000, 1, 640, 360);   // A repeated: no new fragment
  idrAccessUnit(stream, 6000, 2, 1280, 720);  // B
  idrAccessUnit(stream, 9000, 1, 640, 360);   // back to A
  stream.flush();

  assert.equal(out.events.length, 3);
  assert.equal(out.doneCount(), 1);
  assert.equal(out.errors.length, 0);

  const expected = [
    {tag: [1], width: 640, height: 360, samples: 2, tfdt: 0, seq: 0},
    {tag: [2], width: 1280, height: 720, samples: 1, tfdt: 6000, seq: 1},
    {tag: [1], width: 640, height: 360, samples: 1, tfdt: 9000, seq: 2}
  ];

  expected.forEach((want, index) => {
    const event = out.events[index];
    const avc = readAvc1(initFor(event));
    const moof = readMoof(event.boxes);
    const mdatNals = readMdatNals(event.boxes);

    assert.deepEqual(avc.sps, [[0x67].concat(want.tag)], 'init SPS bytes fragment ' + index);
    assert.deepEqual(avc.pps, [[0x68].concat(want.tag)], 'init PPS bytes fragment ' + index);
    assert.equal(avc.width, want.width);
    assert.equal(avc.height, want.height);
    assert.equal(moof.sequenceNumber, want.seq);
    assert.equal(moof.baseMediaDecodeTime, want.tfdt);
    assert.equal(moof.samples.length, want.samples);

    // the parameter sets carried in mdat must all match this fragment's
    // init configuration (one inline pair per access unit/sample)
    const mdatSps = mdatNals.filter((n) => n.typeCode === 7).map((n) => n.bytes);
    const mdatPps = mdatNals.filter((n) => n.typeCode === 8).map((n) => n.bytes);

    assert.equal(mdatSps.length, want.samples);
    assert.equal(mdatPps.length, want.samples);
    assert.ok(mdatSps.every((sps) =>
      JSON.stringify(sps) === JSON.stringify([0x67].concat(want.tag))));
    assert.ok(mdatPps.every((pps) =>
      JSON.stringify(pps) === JSON.stringify([0x68].concat(want.tag))));
    // every fragment begins with an AUD and contains one IDR per sample
    assert.equal(mdatNals[0].typeCode, 9);
    assert.equal(mdatNals.filter((n) => n.typeCode === 5).length, want.samples);
  });

  // boundary timing: A's last frame duration comes from B's DTS, not reset
  assert.deepEqual(readMoof(out.events[0].boxes).samples.map((s) => s.duration), [3000, 3000]);
  assert.deepEqual(readMoof(out.events[1].boxes).samples.map((s) => s.duration), [3000]);
  assert.deepEqual(readMoof(out.events[2].boxes).samples.map((s) => s.duration), [3000]);
}());

// ---------------------------------------------------------------------------
// 2. A byte-level SPS/PPS change splits even when dimensions stay the same
// ---------------------------------------------------------------------------
(function byteChangeSameDimensionsStillSplits() {
  const stream = new VideoSegmentStream(makeTrack(),
    {keepOriginalTimestamps: true, splitVideoConfig: true});
  const out = collect(stream);

  idrAccessUnit(stream, 0, [1, 0xaa], 640, 360);
  idrAccessUnit(stream, 3000, [2, 0xbb], 640, 360);
  stream.flush();

  assert.equal(out.events.length, 2);
  const first = readAvc1(initFor(out.events[0]));
  const second = readAvc1(initFor(out.events[1]));

  assert.equal(first.width, second.width);
  assert.equal(first.height, second.height);
  assert.notDeepEqual(first.sps, second.sps);
  assert.notDeepEqual(first.pps, second.pps);
  assert.deepEqual(second.sps, [[0x67, 2, 0xbb]]);
}());

// ---------------------------------------------------------------------------
// 3. Different frame durations on either side of a switch
// ---------------------------------------------------------------------------
(function differingFrameDurations() {
  const stream = new VideoSegmentStream(makeTrack(),
    {keepOriginalTimestamps: true, splitVideoConfig: true});
  const out = collect(stream);

  // config A on a 3000-tick cadence
  idrAccessUnit(stream, 0, 1, 320, 240);
  idrAccessUnit(stream, 3000, 1, 320, 240);
  // config B starts on a 4500-tick cadence
  idrAccessUnit(stream, 6000, 2, 640, 480);
  idrAccessUnit(stream, 10500, 2, 640, 480);
  stream.flush();

  assert.equal(out.events.length, 2);
  const aFragment = readMoof(out.events[0].boxes);
  const bFragment = readMoof(out.events[1].boxes);

  // A's final frame still ends exactly where B begins
  assert.deepEqual(aFragment.samples.map((s) => s.duration), [3000, 3000]);
  assert.deepEqual(bFragment.samples.map((s) => s.duration), [4500, 4500]);
  assert.equal(bFragment.baseMediaDecodeTime, 6000);
  assert.equal(aFragment.baseMediaDecodeTime +
    aFragment.samples.reduce((sum, s) => sum + s.duration, 0), 6000);
}());

// ---------------------------------------------------------------------------
// 4. baseMediaDecodeTime / non-original timestamps stay continuous and are
//    never reset per configuration or double-counted across the switch
// ---------------------------------------------------------------------------
(function timestampsRemainContinuous() {
  const stream = new VideoSegmentStream(makeTrack(90000),
    {keepOriginalTimestamps: false, splitVideoConfig: true});
  const out = collect(stream);

  idrAccessUnit(stream, 0, 1, 320, 240);
  idrAccessUnit(stream, 3000, 2, 640, 480);
  stream.flush();

  const first = readMoof(out.events[0].boxes);
  const second = readMoof(out.events[1].boxes);

  assert.equal(first.baseMediaDecodeTime, 90000);
  assert.equal(second.baseMediaDecodeTime, 93000);
  assert.equal(second.sequenceNumber, first.sequenceNumber + 1);
}());

// ---------------------------------------------------------------------------
// 5. Cross-flush GOP fusion only joins identical configurations
// ---------------------------------------------------------------------------
(function crossFlushGopFusion() {
  const stream = new VideoSegmentStream(makeTrack(),
    {keepOriginalTimestamps: true, splitVideoConfig: true});
  const out = collect(stream);

  idrAccessUnit(stream, 0, 1, 320, 240);
  idrAccessUnit(stream, 3000, 1, 320, 240);
  stream.flush();
  assert.equal(out.events.length, 1);

  // leading non-IDR under config A: must fuse with the cached A GOP
  stream.push(aud(6000));
  stream.push(nonIdrNal(6000, 7));
  idrAccessUnit(stream, 9000, 1, 320, 240);
  stream.flush();

  assert.equal(out.events.length, 2);
  const fused = out.events[1];
  const moof = readMoof(fused.boxes);

  // cached GOP (1 frame) + non-IDR + following IDR = 3 samples, pulled back
  // to the cached GOP's DTS
  assert.equal(moof.samples.length, 3);
  assert.equal(moof.baseMediaDecodeTime, 3000);
  const avc = readAvc1(initFor(fused));

  assert.deepEqual(avc.sps, [[0x67, 1]]);

  // a new configuration must never borrow the A cache
  const switchStream = new VideoSegmentStream(makeTrack(),
    {keepOriginalTimestamps: true, splitVideoConfig: true});
  const switchOut = collect(switchStream);

  idrAccessUnit(switchStream, 0, 1, 320, 240);
  switchStream.flush();
  idrAccessUnit(switchStream, 3000, 2, 640, 480);
  switchStream.flush();

  const bBoxes = switchOut.events[1].boxes;
  const bNals = readMdatNals(bBoxes);

  assert.ok(!bNals.some((n) => n.typeCode === 5 && n.bytes[1] === 1),
    'the B fragment must not contain a cached A IDR');
  assert.equal(readMoof(bBoxes).samples.length, 1);
}());

// ---------------------------------------------------------------------------
// 6. Emitted snapshots are stable against later input and another flush
// ---------------------------------------------------------------------------
(function snapshotImmutability() {
  const stream = new VideoSegmentStream(makeTrack(),
    {keepOriginalTimestamps: true, splitVideoConfig: true});
  const out = collect(stream);

  idrAccessUnit(stream, 0, 1, 320, 240);
  stream.flush();

  const firstInit = new Uint8Array(initFor(out.events[0]));
  const firstSps = new Uint8Array(out.events[0].track.sps[0]);
  const firstBoxes = new Uint8Array(out.events[0].boxes);

  idrAccessUnit(stream, 3000, 2, 640, 480);
  stream.flush();

  assert.deepEqual(Array.from(new Uint8Array(initFor(out.events[0]))), Array.from(firstInit));
  assert.deepEqual(Array.from(out.events[0].track.sps[0]), Array.from(firstSps));
  assert.deepEqual(Array.from(out.events[0].boxes), Array.from(firstBoxes));
  assert.equal(readAvc1(initFor(out.events[1])).width, 640);
}());

// ---------------------------------------------------------------------------
// 7. Rejection paths: no data event may escape before the error
// ---------------------------------------------------------------------------
function expectRejection(name, pushBad, expectedFragment) {
  const stream = new VideoSegmentStream(makeTrack(),
    {keepOriginalTimestamps: true, splitVideoConfig: true});
  const out = collect(stream);

  pushBad(stream);
  stream.flush();

  assert.equal(out.events.length, 0, name + ' emitted data');
  assert.equal(out.errors.length, 1, name + ' did not error');
  assert.match(out.errors[0].message, expectedFragment, name + ' error message');
  assert.equal(out.doneCount(), 1, name + ' still signals done');

  // reset() clears the failure; legal input works on the same stream
  stream.reset();
  idrAccessUnit(stream, 0, 9, 320, 240);
  stream.flush();
  assert.equal(out.events.length, 1, name + ' did not recover after reset');
}

expectRejection('missing PPS', (stream) => {
  stream.push(aud(0));
  stream.push(spsNal(0, 1, 320, 240));
  stream.push(idrNal(0, 1));
}, /incomplete parameter set/);

expectRejection('parameter set after a slice', (stream) => {
  idrAccessUnit(stream, 0, 1, 320, 240);
  stream.push(aud(3000));
  stream.push(ppsNal(3000, 2));
  stream.push(idrNal(3000, 2));
  stream.push(spsNal(3000, 2, 640, 480));
}, /start of an access unit/);

expectRejection('new configuration on a non-IDR', (stream) => {
  idrAccessUnit(stream, 0, 1, 320, 240);
  stream.push(aud(3000));
  stream.push(spsNal(3000, 2, 640, 480));
  stream.push(ppsNal(3000, 2));
  stream.push(nonIdrNal(3000, 2));
}, /IDR access unit/);

expectRejection('no parameter sets at all', (stream) => {
  stream.push(aud(0));
  stream.push(idrNal(0, 1));
}, /missing complete SPS\/PPS/);

// ---------------------------------------------------------------------------
// 8. An empty flush produces no media fragment
// ---------------------------------------------------------------------------
(function emptyFlush() {
  const stream = new VideoSegmentStream(makeTrack(), {splitVideoConfig: true});
  const out = collect(stream);

  stream.flush();
  assert.equal(out.events.length, 0);
  assert.equal(out.doneCount(), 1);
}());

// ---------------------------------------------------------------------------
// 8b. reset() drops the GOP cache and active configuration; pre-reset GOPs can
// never be fused into post-reset fragments
// ---------------------------------------------------------------------------
(function resetDropsGopCache() {
  const stream = new VideoSegmentStream(makeTrack(),
    {keepOriginalTimestamps: true, splitVideoConfig: true});
  const out = collect(stream);

  idrAccessUnit(stream, 0, 1, 320, 240);
  stream.flush();
  assert.equal(out.events.length, 1);

  stream.reset();
  assert.equal(stream.gopCache_.length, 0);

  // re-establish a configuration after reset at a much later DTS
  idrAccessUnit(stream, 6000, 1, 320, 240);
  stream.flush();
  assert.equal(out.events.length, 2);
  assert.equal(readMoof(out.events[1].boxes).baseMediaDecodeTime, 6000);

  // a leading non-IDR can only fuse with the post-reset GOP at 6000, never
  // the pre-reset GOP at 0
  stream.push(aud(9000));
  stream.push(nonIdrNal(9000, 7));
  idrAccessUnit(stream, 12000, 1, 320, 240);
  stream.flush();

  assert.equal(out.events.length, 3);
  const fused = readMoof(out.events[2].boxes);

  // the fused fragment starts at the post-reset GOP (6000), proving the
  // pre-reset GOP at 0 was discarded rather than reused from the cache
  assert.equal(fused.baseMediaDecodeTime, 6000);
  assert.equal(fused.samples.length, 3);
}());

// ---------------------------------------------------------------------------
// 9. The option defaults to off and keeps the legacy single-fragment behavior
// ---------------------------------------------------------------------------
(function disabledByDefault() {
  const stream = new VideoSegmentStream(makeTrack(),
    {keepOriginalTimestamps: true});
  const out = collect(stream);

  idrAccessUnit(stream, 0, 1, 640, 360);
  idrAccessUnit(stream, 3000, 2, 1280, 720);
  stream.flush();

  assert.equal(out.events.length, 1);
  assert.equal(out.events[0].track.samples.length, 2);
  assert.equal(readAvc1(initFor(out.events[0])).width, 640);
}());

// ---------------------------------------------------------------------------
// Minimal MPEG-TS muxer for the public Transmuxer entry point
// ---------------------------------------------------------------------------
const TS_PACKET_LENGTH = 188;
const PAT_PID = 0;
const PMT_PID = 256;
const VIDEO_PID = 257;
const AUDIO_PID = 258;

function writeCrc() { /* mux.js does not validate section CRCs */ }

function tsPacket(pid, payload, pusi, continuity, withAdaptation) {
  const packet = new Uint8Array(TS_PACKET_LENGTH);
  packet[0] = 0x47;
  packet[1] = (pusi ? 0x40 : 0) | ((pid >> 8) & 0x1f);
  packet[2] = pid & 0xff;
  let offset = 4;

  if (withAdaptation) {
    // adaptation field of a single stuffing byte
    packet[3] = 0x30 | (continuity & 0x0f);
    packet[4] = 1;
    packet[5] = 0;
    offset = 6;
  } else {
    packet[3] = 0x10 | (continuity & 0x0f);
  }
  packet.set(payload, offset);
  for (let i = offset + payload.length; i < TS_PACKET_LENGTH; i++) {
    packet[i] = 0xff;
  }
  return packet;
}

function patPacket() {
  const payload = new Uint8Array(184);
  payload[0] = 0; // pointer field
  payload[1] = 0x00;
  payload[2] = 0xb0;
  payload[3] = 13; // section length
  payload[4] = 0; payload[5] = 1; // transport stream id
  payload[6] = 0x01; // version 0, current
  payload[7] = 0; payload[8] = 0; // section numbers
  payload[9] = 0; payload[10] = 1; // program 1
  payload[11] = (PMT_PID >> 8) & 0x1f;
  payload[12] = PMT_PID & 0xff;
  writeCrc();
  return tsPacket(PAT_PID, payload, true, 0);
}

function pmtPacket(includeAudio) {
  const payload = new Uint8Array(184);
  // full TS payload: byte 0 is the pointer field, the PMT section starts at 1
  payload[0] = 0;
  payload[1] = 0x02; // table_id
  payload[2] = 0xb0; // section_syntax_indicator + reserved
  // section_length = 9 bytes of fixed fields + 5 per ES entry + 4 CRC bytes
  const sectionLength = includeAudio ? 23 : 18;

  payload[3] = sectionLength;
  payload[4] = 0; payload[5] = 1; // program number
  payload[6] = 0x01; // version 0, current_next_indicator
  payload[7] = 0; payload[8] = 0; // section numbers
  payload[9] = (PMT_PID >> 8) & 0x1f;
  payload[10] = PMT_PID & 0xff; // PCR PID
  payload[11] = 0xf0; payload[12] = 0x00; // program info length 0
  let offset = 13;
  payload[offset++] = 0x1b; // H.264
  payload[offset++] = (VIDEO_PID >> 8) & 0x1f;
  payload[offset++] = VIDEO_PID & 0xff;
  payload[offset++] = 0xf0; // ES info length high
  payload[offset++] = 0x00; // ES info length low
  if (includeAudio) {
    payload[offset++] = 0x0f; // ADTS
    payload[offset++] = (AUDIO_PID >> 8) & 0x1f;
    payload[offset++] = AUDIO_PID & 0xff;
    payload[offset++] = 0xf0;
    payload[offset++] = 0x00;
  }
  return tsPacket(PMT_PID, payload, true, 0);
}

// Exp-Golomb / bit writer for valid, minimal baseline SPS and PPS bytes
function bitWriter() {
  const bytes = [];
  let current = 0;
  let bits = 0;

  return {
    bits(value, count) {
      for (let i = count - 1; i >= 0; i--) {
        current = (current << 1) | ((value >> i) & 1);
        bits++;
        if (bits === 8) {
          bytes.push(current);
          current = 0;
          bits = 0;
        }
      }
    },
    ue(value) {
      const code = value + 1;
      const length = code.toString(2).length;

      this.bits(0, length - 1);
      this.bits(code, length);
    },
    se(value) {
      this.ue(value <= 0 ? 2 * (-value) : 2 * value - 1);
    },
    finish() {
      this.bits(1, 1); // rbsp_stop_bit
      while (bits) {
        this.bits(0, 1);
      }
      return bytes;
    }
  };
}

function buildSps(width, height, levelIdc) {
  const w = bitWriter();

  w.bits(66, 8); // profile_idc baseline
  w.bits(0, 8); // constraint flags
  w.bits(levelIdc, 8);
  w.ue(0); // seq_parameter_set_id
  w.ue(0); // log2_max_frame_num_minus4
  w.ue(0); // pic_order_cnt_type
  w.ue(0); // log2_max_pic_order_cnt_lsb_minus4
  w.ue(0); // max_num_ref_frames
  w.bits(0, 1); // gaps_in_frame_num_value_allowed_flag
  w.ue(width / 16 - 1);
  w.ue(height / 16 - 1);
  w.bits(1, 1); // frame_mbs_only_flag
  w.bits(1, 1); // direct_8x8_inference_flag
  w.bits(0, 1); // frame_cropping_flag
  w.bits(0, 1); // vui_parameters_present_flag
  return [0x67].concat(w.finish());
}

function buildPps() {
  const w = bitWriter();

  w.ue(0); // pic_parameter_set_id
  w.ue(0); // seq_parameter_set_id
  w.bits(0, 1); // entropy_coding_mode_flag
  w.bits(0, 1); // bottom_field_pic_order_in_frame_present_flag
  w.ue(0); // num_slice_groups_minus1
  w.ue(0); // num_ref_idx_l0_default_active_minus1
  w.ue(0); // num_ref_idx_l1_default_active_minus1
  w.bits(0, 1); // weighted_pred_flag
  w.bits(0, 2); // weighted_bipred_idc
  w.se(0); // pic_init_qp_minus26
  w.se(0); // pic_init_qs_minus26
  w.se(0); // chroma_qp_index_offset
  w.bits(0, 1); // deblocking_filter_control_present_flag
  w.bits(0, 1); // constrained_intra_pred_flag
  w.bits(0, 1); // redundant_pic_cnt_present_flag
  return [0x68].concat(w.finish());
}

function encodeTs33(value, highMarker) {
  const bytes = [];
  bytes.push(highMarker | ((value >> 30) & 0x07) | 0x01);
  bytes.push((value >> 22) & 0xff);
  bytes.push(((value >> 14) & 0xfe) | 0x01);
  bytes.push((value >> 7) & 0xff);
  bytes.push(((value << 1) & 0xfe) | 0x01);
  return bytes;
}

function annexB(nals) {
  const out = [];

  for (const nal of nals) {
    out.push(0, 0, 1);
    for (const byte of nal) {
      out.push(byte);
    }
  }
  return out;
}

function videoPesPacket(dts, nals, continuity) {
  const payloadBytes = annexB(nals);
  const pts = encodeTs33(dts, 0x21);
  const dtsBytes = encodeTs33(dts, 0x11);
  const header = [0, 0, 1, 0xe0, 0, 0, 0x80, 0xc0, 10]
    .concat(pts)
    .concat(dtsBytes);
  const pes = Uint8Array.from(header.concat(payloadBytes));

  assert.ok(pes.byteLength <= 184, 'test PES must fit in one TS packet');
  return tsPacket(VIDEO_PID, pes, true, continuity);
}

function configSwitchTs() {
  const spsA = buildSps(320, 240, 30);
  const ppsA = buildPps();
  const spsB = buildSps(640, 480, 31);
  const ppsB = ppsA.concat([0x77]); // distinct PPS bytes for config B
  const packets = [patPacket(), pmtPacket(false)];
  const aud = [0x09, 0x10];

  packets.push(videoPesPacket(0, [aud, spsA, ppsA, [0x65, 0xaa]], 1));
  packets.push(videoPesPacket(3000, [aud, spsA, ppsA, [0x65, 0xab]], 2));
  packets.push(videoPesPacket(6000, [aud, spsB, ppsB, [0x65, 0xba]], 3));
  packets.push(videoPesPacket(9000, [aud, spsB, ppsB, [0x65, 0xbb]], 4));

  const bytes = new Uint8Array(packets.length * TS_PACKET_LENGTH);

  packets.forEach((packet, index) => bytes.set(packet, index * TS_PACKET_LENGTH));
  return {bytes, spsA, ppsA, spsB, ppsB};
}

// ---------------------------------------------------------------------------
// 10. A real, audio-free TS stream drives the option through the Transmuxer
// ---------------------------------------------------------------------------
(function transmuxerTsConfigSwitch() {
  const fixture = configSwitchTs();
  const transmuxer = new Transmuxer({
    remux: false,
    keepOriginalTimestamps: true,
    splitVideoConfig: true
  });
  const segments = [];
  const errors = [];
  let doneCount = 0;

  transmuxer.on('data', (segment) => segments.push(segment));
  transmuxer.on('error', (err) => errors.push(err));
  transmuxer.on('done', () => doneCount++);

  transmuxer.push(fixture.bytes);
  transmuxer.flush();

  assert.equal(errors.length, 0, errors[0] && errors[0].message);
  assert.equal(doneCount, 1);
  assert.equal(segments.length, 2);

  const first = segments[0];
  const second = segments[1];

  assert.equal(first.type, 'video');
  const firstAvc = readAvc1(first.initSegment);
  const secondAvc = readAvc1(second.initSegment);

  assert.equal(firstAvc.width, 320);
  assert.equal(firstAvc.height, 240);
  assert.equal(secondAvc.width, 640);
  assert.equal(secondAvc.height, 480);
  assert.deepEqual(firstAvc.sps, [fixture.spsA]);
  assert.deepEqual(firstAvc.pps, [fixture.ppsA]);
  assert.deepEqual(secondAvc.sps, [fixture.spsB]);
  assert.deepEqual(secondAvc.pps, [fixture.ppsB]);

  // each data event's moof/mdat pair matches its own init segment
  const firstMoof = readMoof(first.data);
  const secondMoof = readMoof(second.data);

  assert.equal(firstMoof.samples.length, 2);
  assert.equal(secondMoof.samples.length, 2);
  assert.equal(firstMoof.baseMediaDecodeTime, 0);
  assert.equal(secondMoof.baseMediaDecodeTime, 6000);
  assert.ok(readMdatNals(first.data).some((n) => n.bytes[1] === 0xaa));
  assert.ok(!readMdatNals(second.data).some((n) => n.bytes[1] === 0xaa));
}());

// ---------------------------------------------------------------------------
// 11. Audio and remux:true are refused explicitly
// ---------------------------------------------------------------------------
(function unsupportedCombinations() {
  assert.throws(() => new Transmuxer({remux: true, splitVideoConfig: true}),
    /remux set to false/);
  // remux defaults to true, so it must be explicitly set to false
  assert.throws(() => new Transmuxer({splitVideoConfig: true}),
    /remux set to false/);

  const audioTs = (function() {
    const packets = [patPacket(), pmtPacket(true)];
    const sps = buildSps(320, 240, 30);
    const pps = buildPps();

    packets.push(videoPesPacket(0, [[0x09, 0x10], sps, pps, [0x65, 0xaa]], 1));
    const bytes = new Uint8Array(packets.length * TS_PACKET_LENGTH);

    packets.forEach((packet, index) => bytes.set(packet, index * TS_PACKET_LENGTH));
    return bytes;
  }());

  const transmuxer = new Transmuxer({remux: false, splitVideoConfig: true});
  const errors = [];
  const segments = [];

  transmuxer.on('error', (err) => errors.push(err));
  transmuxer.on('data', (segment) => segments.push(segment));
  transmuxer.push(audioTs);
  transmuxer.flush();

  assert.equal(segments.length, 0, 'audio must not be silently dropped into a fragment');
  assert.equal(errors.length, 1);
  assert.match(errors[0].message, /video-only/);
}());

// ---------------------------------------------------------------------------
// 12. Multiple outer flushes keep one done each and continuous sequence numbers
// ---------------------------------------------------------------------------
(function multipleFlushesThroughTransmuxer() {
  const transmuxer = new Transmuxer({
    remux: false,
    keepOriginalTimestamps: true,
    splitVideoConfig: true
  });
  const segments = [];
  const errors = [];
  let doneCount = 0;

  transmuxer.on('data', (segment) => segments.push(segment));
  transmuxer.on('error', (err) => errors.push(err));
  transmuxer.on('done', () => doneCount++);

  const first = configSwitchTs();

  transmuxer.push(first.bytes);
  transmuxer.flush();
  assert.equal(doneCount, 1);
  assert.equal(segments.length, 2);

  const second = configSwitchTs();

  transmuxer.push(second.bytes);
  transmuxer.flush();

  assert.equal(errors.length, 0, errors[0] && errors[0].message);
  assert.equal(doneCount, 2, 'one done per outer flush');
  assert.equal(segments.length, 4);

  const sequences = segments.map((segment) => readMoof(segment.data).sequenceNumber);

  assert.deepEqual(sequences, [0, 1, 2, 3], 'sequence numbers increase across flushes');

  // every emitted init segment stays internally consistent after later input
  segments.forEach((segment) => {
    const avc = readAvc1(segment.initSegment);
    const mdatSps = readMdatNals(segment.data)
      .filter((n) => n.typeCode === 7)
      .map((n) => n.bytes);

    assert.ok(mdatSps.every((sps) => JSON.stringify(sps) === JSON.stringify(avc.sps[0])));
  });
}());

console.log('config switch: ok');
