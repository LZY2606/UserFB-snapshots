import assert from 'node:assert/strict';
import {Parser} from './load-source.mjs';

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

const parse = (text, previousManifest, chunk = (t) => [t]) => {
  const parser = new Parser(previousManifest ? {previousManifest} : {});
  const errors = [];

  parser.on('error', (e) => errors.push(e.message));
  for (const piece of chunk(text)) {
    parser.push(piece);
  }
  parser.end();
  return {parser, manifest: parser.manifest, errors};
};

const parseWhole = (text, previousManifest) => parse(text, previousManifest).manifest;

// every way the spec allows the text to arrive over the wire
const chunkings = [
  (t) => [t],
  (t) => t.split(''),
  (t) => {
    const chunks = [];

    for (let i = 0; i < t.length; i += 3) {
      chunks.push(t.slice(i, i + 3));
    }
    return chunks;
  },
  (t) => {
    const chunks = [];

    for (let i = 0; i < t.length; i += 13) {
      chunks.push(t.slice(i, i + 13));
    }
    return chunks;
  }
];

// split the text in two at every possible position, including positions that
// cut across a line boundary
const allTwoChunkSplits = (t) => {
  const out = [];

  for (let i = 0; i <= t.length; i++) {
    out.push([t.slice(0, i), t.slice(i)]);
  }
  return out;
};

// ---------------------------------------------------------------------------
// fixtures
// ---------------------------------------------------------------------------

const oldText = `#EXTM3U
#EXT-X-VERSION:9
#EXT-X-TARGETDURATION:4
#EXT-X-MEDIA-SEQUENCE:41
#EXT-X-DISCONTINUITY-SEQUENCE:7
#EXT-X-KEY:METHOD=AES-128,URI="key-a.bin",IV=0x00000000000000000000000000000041
#EXT-X-MAP:URI="init-a.mp4"
#EXTINF:4,
41.m4s
#EXT-X-DISCONTINUITY
#EXTINF:4,
42.m4s
#EXTINF:4,
43.m4s
#EXTINF:4,
44.m4s
#EXT-X-PART:DURATION=0.5,URI="45.0.old.m4s"
#EXT-X-PRELOAD-HINT:TYPE=PART,URI="45.1.old.m4s"
`;

// M=42, K=2: segments 42 and 43 are omitted, 44 is listed again with a new MAP
// and METHOD=NONE, 45 carries a discontinuity, and 46 only has a trailing PART
// plus a preload hint.
const deltaText = `#EXTM3U
#EXT-X-VERSION:9
#EXT-X-TARGETDURATION:4
#EXT-X-MEDIA-SEQUENCE:42
#EXT-X-DISCONTINUITY-SEQUENCE:7
#EXT-X-SKIP:SKIPPED-SEGMENTS=2
#EXT-X-MAP:URI="init-b.mp4"
#EXT-X-KEY:METHOD=NONE
#EXTINF:4,
44.m4s
#EXT-X-DISCONTINUITY
#EXTINF:4,
45.m4s
#EXT-X-PART:DURATION=0.5,URI="46.0.m4s"
#EXT-X-PRELOAD-HINT:TYPE=PART,URI="46.1.m4s"
`;

const oldManifest = parseWhole(oldText);

// ---------------------------------------------------------------------------
// main scenario: one continuous delta update, field by field
// ---------------------------------------------------------------------------

const merged = parseWhole(deltaText, oldManifest);

assert.equal(merged.mediaSequence, 42);
assert.equal(merged.skip.skippedSegments, 2);
assert.deepEqual(merged.discontinuityStarts, [0, 3]);
assert.equal(merged.segments.length, 4);

const byIndex = (i) => merged.segments[i];

// segment 42: restored from the snapshot, discontinuity opens a new timeline
let seg = byIndex(0);
assert.equal(seg.uri, '42.m4s');
assert.equal(seg.duration, 4);
assert.equal(seg.discontinuity, true);
assert.equal(seg.timeline, 8);
assert.equal(seg.key.method, 'AES-128');
assert.equal(seg.key.uri, 'key-a.bin');
assert.ok(seg.key.iv instanceof Uint32Array, 'IV is kept as a Uint32Array');
assert.deepEqual(seg.key.iv, oldManifest.segments[1].key.iv);
assert.equal(seg.map.uri, 'init-a.mp4');

// segment 43: restored, same inherited key/map/timeline, no discontinuity
seg = byIndex(1);
assert.equal(seg.uri, '43.m4s');
assert.equal(seg.discontinuity, undefined);
assert.equal(seg.timeline, 8);
assert.equal(seg.key.uri, 'key-a.bin');
assert.equal(seg.map.uri, 'init-a.mp4');

// segment 44: listed in the new text; METHOD=NONE revokes the old key and the
// new MAP only applies from here on
seg = byIndex(2);
assert.equal(seg.uri, '44.m4s');
assert.equal(seg.timeline, 8);
assert.equal('key' in seg, false, 'METHOD=NONE must not be undone by the restored key');
assert.equal(seg.map.uri, 'init-b.mp4');

// segment 45: new discontinuity continues the timeline counting
seg = byIndex(3);
assert.equal(seg.uri, '45.m4s');
assert.equal(seg.discontinuity, true);
assert.equal(seg.timeline, 9);
assert.equal('key' in seg, false);
assert.equal(seg.map.uri, 'init-b.mp4');

// preload segment: only this response's PART/hint, inherited map, no key,
// timeline continuing past the last full segment
const preload = merged.preloadSegment;
assert.equal(preload.uri, undefined);
assert.equal(preload.parts.length, 1);
assert.equal(preload.parts[0].uri, '46.0.m4s');
assert.equal(preload.preloadHints.length, 1);
assert.equal(preload.preloadHints[0].uri, '46.1.m4s');
assert.equal(preload.map.uri, 'init-b.mp4');
assert.equal('key' in preload, false);
assert.equal(preload.timeline, 9);

// the old preload PART must not be stacked onto the new one
assert.ok(!preload.parts.some((p) => p.uri === '45.0.old.m4s'));
assert.ok(!preload.preloadHints.some((h) => h.uri === '45.1.old.m4s'));

// ---------------------------------------------------------------------------
// the snapshot and the merged result never share mutable references
// ---------------------------------------------------------------------------

const restored42 = merged.segments[0];
const old42 = oldManifest.segments[1];

assert.notEqual(restored42, old42);
assert.notEqual(restored42.key, old42.key);
assert.notEqual(restored42.key.iv, old42.key.iv);
assert.notEqual(restored42.map, old42.map);

// ---------------------------------------------------------------------------
// chunking: whole string, per character and arbitrary cross-line splits all
// produce the same manifest (runs before the snapshot is mutated below)
// ---------------------------------------------------------------------------

for (const chunk of chunkings) {
  const {manifest, errors} = parse(deltaText, oldManifest, chunk);

  assert.deepEqual(errors, []);
  assert.deepEqual(manifest, merged);
}
for (const pieces of allTwoChunkSplits(deltaText)) {
  const parser = new Parser({previousManifest: oldManifest});
  const errors = [];

  parser.on('error', (e) => errors.push(e.message));
  parser.push(pieces[0]);
  parser.push(pieces[1]);
  parser.end();
  assert.deepEqual(errors, []);
  assert.deepEqual(parser.manifest, merged);
}

// mutating the snapshot after parsing must not change the result. use a fresh
// parse so the shared oldManifest stays pristine for the later scenarios.
const mutableOld = parseWhole(oldText);
const mutableMerged = parseWhole(deltaText, mutableOld);

mutableOld.segments[1].uri = 'mutated.m4s';
mutableOld.segments[1].key.uri = 'mutated-key';
mutableOld.segments[1].map.uri = 'mutated-map';
mutableOld.preloadSegment.parts[0].uri = 'mutated-part';
assert.equal(mutableMerged.segments[0].uri, '42.m4s');
assert.equal(mutableMerged.segments[0].key.uri, 'key-a.bin');
assert.equal(mutableMerged.segments[0].map.uri, 'init-a.mp4');
assert.equal(mutableMerged.preloadSegment.parts[0].uri, '46.0.m4s');

// mutating the result must not reach back into the snapshot
mutableMerged.segments[0].uri = 'result-mutated.m4s';
assert.equal(mutableOld.segments[1].uri, 'mutated.m4s');

// ---------------------------------------------------------------------------
// old window sliding out: index based lookup, not array-tail slicing
// ---------------------------------------------------------------------------

// M=43, K=2 restores 43/44 even though 41/42 have already left the window;
// segment 45 is new.
const slideText = `#EXTM3U
#EXT-X-VERSION:9
#EXT-X-TARGETDURATION:4
#EXT-X-MEDIA-SEQUENCE:43
#EXT-X-DISCONTINUITY-SEQUENCE:7
#EXT-X-SKIP:SKIPPED-SEGMENTS=2
#EXT-X-MAP:URI="init-b.mp4"
#EXT-X-KEY:METHOD=NONE
#EXTINF:4,
45.m4s
`;
const slide = parseWhole(slideText, oldManifest);
assert.equal(slide.segments.length, 3);
assert.equal(slide.segments[0].uri, '43.m4s');
assert.equal(slide.segments[0].map.uri, 'init-a.mp4');
assert.equal(slide.segments[1].uri, '44.m4s');
assert.equal(slide.segments[2].uri, '45.m4s');
assert.equal(slide.segments[2].map.uri, 'init-b.mp4');
assert.equal('key' in slide.segments[2], false);

// when the needed sequences have fully slid out of the old window, the merge
// fails instead of handing back a truncated window
const slidAwayText = slideText.replace('MEDIA-SEQUENCE:43', 'MEDIA-SEQUENCE:45');
const slidAway = parse(slidAwayText, oldManifest);
assert.ok(slidAway.errors.length, 'missing skipped sequences must emit an error');
assert.deepEqual(slidAway.manifest.segments, []);
assert.equal(slidAway.manifest.preloadSegment, undefined);

// ---------------------------------------------------------------------------
// K=0: nothing is restored, the current text is a complete update
// ---------------------------------------------------------------------------

const k0Text = `#EXTM3U
#EXT-X-VERSION:9
#EXT-X-TARGETDURATION:4
#EXT-X-MEDIA-SEQUENCE:50
#EXT-X-DISCONTINUITY-SEQUENCE:7
#EXT-X-SKIP:SKIPPED-SEGMENTS=0
#EXT-X-DISCONTINUITY
#EXTINF:4,
50.m4s
#EXTINF:4,
51.m4s
`;
const k0 = parse(k0Text, oldManifest);
assert.deepEqual(k0.errors, []);
assert.equal(k0.manifest.segments.length, 2);
assert.deepEqual(k0.manifest.segments.map((s) => s.uri), ['50.m4s', '51.m4s']);
assert.deepEqual(k0.manifest.discontinuityStarts, [0]);
assert.equal(k0.manifest.segments[0].timeline, 8);
assert.equal(k0.manifest.segments[1].timeline, 8);

// ---------------------------------------------------------------------------
// inheritance with no KEY/MAP declarations in the delta text: the state
// carried through the restored segments flows into listed segments and the
// preload segment
// ---------------------------------------------------------------------------

const inheritText = `#EXTM3U
#EXT-X-VERSION:9
#EXT-X-TARGETDURATION:4
#EXT-X-MEDIA-SEQUENCE:43
#EXT-X-DISCONTINUITY-SEQUENCE:7
#EXT-X-SKIP:SKIPPED-SEGMENTS=2
#EXTINF:4,
45.m4s
#EXT-X-PART:DURATION=0.5,URI="46.0.m4s"
#EXT-X-PRELOAD-HINT:TYPE=PART,URI="46.1.m4s"
`;
const inherit = parseWhole(inheritText, oldManifest);
assert.equal(inherit.segments.length, 3);
assert.equal(inherit.segments[0].uri, '43.m4s');
assert.equal(inherit.segments[2].uri, '45.m4s');
assert.equal(inherit.segments[2].key.uri, 'key-a.bin', 'listed segment inherits the restored key');
assert.equal(inherit.segments[2].map.uri, 'init-a.mp4', 'listed segment inherits the restored map');
assert.equal(inherit.preloadSegment.map.uri, 'init-a.mp4', 'preload inherits the restored map');
assert.equal(inherit.preloadSegment.key.uri, 'key-a.bin', 'preload inherits the restored key');
assert.equal(inherit.preloadSegment.parts[0].uri, '46.0.m4s');

// ---------------------------------------------------------------------------
// identical URI with different byte ranges: identity is the sequence number,
// never the URI
// ---------------------------------------------------------------------------
const rangeOldText = `#EXTM3U
#EXT-X-VERSION:9
#EXT-X-TARGETDURATION:4
#EXT-X-MEDIA-SEQUENCE:41
#EXTINF:4,
#EXT-X-BYTERANGE:100@0
data.m4s
#EXTINF:4,
#EXT-X-BYTERANGE:100@100
data.m4s
#EXTINF:4,
#EXT-X-BYTERANGE:100@200
data.m4s
`;
const rangeOld = parseWhole(rangeOldText);
const rangeDeltaText = `#EXTM3U
#EXT-X-VERSION:9
#EXT-X-TARGETDURATION:4
#EXT-X-MEDIA-SEQUENCE:42
#EXT-X-SKIP:SKIPPED-SEGMENTS=1
#EXTINF:4,
#EXT-X-BYTERANGE:100@300
data.m4s
`;
const rangeMerged = parseWhole(rangeDeltaText, rangeOld);
assert.equal(rangeMerged.segments.length, 2);
assert.equal(rangeMerged.segments[0].uri, 'data.m4s');
assert.deepEqual(rangeMerged.segments[0].byterange, {length: 100, offset: 100});
assert.deepEqual(rangeMerged.segments[1].byterange, {length: 100, offset: 300});

// ---------------------------------------------------------------------------
// two consecutive deltas, including a key being re-enabled after METHOD=NONE
// ---------------------------------------------------------------------------

const delta2Text = `#EXTM3U
#EXT-X-VERSION:9
#EXT-X-TARGETDURATION:4
#EXT-X-MEDIA-SEQUENCE:44
#EXT-X-DISCONTINUITY-SEQUENCE:7
#EXT-X-SKIP:SKIPPED-SEGMENTS=2
#EXT-X-MAP:URI="init-c.mp4"
#EXT-X-KEY:METHOD=AES-128,URI="key-b.bin"
#EXTINF:4,
46.m4s
`;
const delta2 = parseWhole(delta2Text, merged);
assert.equal(delta2.segments.length, 3);
let s = delta2.segments[0];
assert.equal(s.uri, '44.m4s');
assert.equal('key' in s, false);
assert.equal(s.map.uri, 'init-b.mp4');
assert.equal(s.timeline, 8);
s = delta2.segments[1];
assert.equal(s.uri, '45.m4s');
assert.equal(s.discontinuity, true);
assert.equal('key' in s, false);
assert.equal(s.map.uri, 'init-b.mp4');
assert.equal(s.timeline, 9);
s = delta2.segments[2];
assert.equal(s.uri, '46.m4s');
assert.equal(s.key.uri, 'key-b.bin');
assert.equal(s.map.uri, 'init-c.mp4');
assert.equal(s.timeline, 9);
assert.deepEqual(delta2.discontinuityStarts, [1]);
// chain of merges still shares no references
assert.notEqual(delta2.segments[0], merged.segments[2]);
assert.notEqual(delta2.segments[0].map, merged.segments[2].map);

// ---------------------------------------------------------------------------
// ENDLIST delta updates follow the same sequence number rules
// ---------------------------------------------------------------------------

const endlistDeltaText = `#EXTM3U
#EXT-X-VERSION:9
#EXT-X-TARGETDURATION:4
#EXT-X-MEDIA-SEQUENCE:42
#EXT-X-SKIP:SKIPPED-SEGMENTS=1
#EXTINF:4,
43.m4s
#EXT-X-ENDLIST
`;
const endlist = parseWhole(endlistDeltaText, oldManifest);
assert.equal(endlist.endList, true);
assert.deepEqual(endlist.segments.map((x) => x.uri), ['42.m4s', '43.m4s']);

// ---------------------------------------------------------------------------
// error inputs emit an error event and never pollute the old snapshot
// ---------------------------------------------------------------------------

const expectError = (text, prev) => {
  const {manifest, errors} = parse(text, prev);

  assert.ok(errors.length, 'expected an error event');
  assert.deepEqual(manifest.segments, [], 'no seemingly complete merged result');
  assert.equal(manifest.preloadSegment, undefined);
  assert.deepEqual(manifest.discontinuityStarts, []);
  return errors;
};

const snapshot = JSON.stringify(oldManifest);

// needed sequences missing
expectError(slidAwayText, oldManifest);
assert.equal(JSON.stringify(oldManifest), snapshot, 'missing-sequence error leaves snapshot untouched');

// K is not a non-negative safe integer
const badKText = deltaText.replace('SKIPPED-SEGMENTS=2', 'SKIPPED-SEGMENTS=-1');
expectError(badKText, oldManifest);
assert.equal(JSON.stringify(oldManifest), snapshot);

const nanKText = deltaText.replace('SKIPPED-SEGMENTS=2', 'SKIPPED-SEGMENTS=abc');
expectError(nanKText, oldManifest);
assert.equal(JSON.stringify(oldManifest), snapshot);

// a master playlist cannot be used as the previous snapshot
const masterText = `#EXTM3U
#EXT-X-STREAM-INF:BANDWIDTH=1280000
high/main.m3u8
`;
const master = parseWhole(masterText);
expectError(deltaText, master);

// the failed parse did not touch the snapshot either
assert.equal(JSON.stringify(oldManifest), snapshot);

// ---------------------------------------------------------------------------
// original behavior is preserved when merging does not apply
// ---------------------------------------------------------------------------

// SKIP present but no previousManifest: only the listed segments are exposed
const noPrev = parseWhole(deltaText);
assert.deepEqual(noPrev.segments.map((x) => x.uri), ['44.m4s', '45.m4s']);
assert.equal(noPrev.preloadSegment.parts[0].uri, '46.0.m4s');

// previousManifest present but no SKIP: the current text is a full update
const noSkipText = deltaText
  .replace('#EXT-X-SKIP:SKIPPED-SEGMENTS=2\n', '')
  .replace('MEDIA-SEQUENCE:42', 'MEDIA-SEQUENCE:44');
const noSkip = parse(noSkipText, oldManifest);
assert.deepEqual(noSkip.errors, []);
assert.deepEqual(noSkip.manifest.segments.map((x) => x.uri), ['44.m4s', '45.m4s']);

console.log('delta merge: ok');
