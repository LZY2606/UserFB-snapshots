import assert from 'node:assert/strict';
import {parse} from './load-source.mjs';

const BASE = 'https://media.invalid/demo.mpd';
const wrap = (body, attrs = 'type="static" mediaPresentationDuration="PT12S"') =>
  `<MPD ${attrs} xmlns="urn:mpeg:dash:schema:mpd:2011">${body}</MPD>`;

const onlyPlaylist = (mpd) => {
  const result = parse(mpd, {manifestUri: BASE});

  assert.equal(result.playlists.length, 1, 'expected a single merged playlist');
  return result.playlists[0];
};

const summarize = (segment) => ({
  uri: segment.uri,
  number: segment.number,
  presentationTime: segment.presentationTime,
  duration: segment.duration,
  map: segment.map && segment.map.uri,
  discontinuity: !!segment.discontinuity
});

// 1. The 12s two-Period sample from period-baseline.mjs, run through the
//    public parse API. Media origins (100 / 200) must be preserved.
const baselineXml = wrap(`
<Period id="p0" start="PT0S" duration="PT6S"><AdaptationSet mimeType="video/mp4" codecs="avc1.42E01E">
<SegmentTemplate timescale="10" presentationTimeOffset="100" media="a-$Time$.m4s" initialization="a-init.mp4"><SegmentTimeline><S t="100" d="20" r="-1"/></SegmentTimeline></SegmentTemplate>
<Representation id="v" bandwidth="1000" width="640" height="360"/></AdaptationSet></Period>
<Period id="p1" start="PT6S" duration="PT6S"><AdaptationSet mimeType="video/mp4" codecs="avc1.42E01E">
<SegmentTemplate timescale="10" presentationTimeOffset="200" media="b-$Time$.m4s" initialization="b-init.mp4"><SegmentTimeline><S t="200" d="20" r="-1"/></SegmentTimeline></SegmentTemplate>
<Representation id="v" bandwidth="1000" width="640" height="360"/></AdaptationSet></Period>`);

const merged = onlyPlaylist(baselineXml);

assert.deepEqual(
  merged.segments.map(s => s.uri),
  ['a-100.m4s', 'a-120.m4s', 'a-140.m4s', 'b-200.m4s', 'b-220.m4s', 'b-240.m4s'],
  'trailing r=-1 expands both Periods using the media clock'
);
assert.deepEqual(
  merged.segments.map(s => s.presentationTime),
  [0, 2, 4, 6, 8, 10],
  'presentation times are derived from PTO, not used inside URLs'
);
assert.deepEqual(
  merged.segments.map(s => s.duration),
  [2, 2, 2, 2, 2, 2],
  'every segment keeps its full original media duration'
);
assert.deepEqual(
  merged.segments.map(s => s.map.uri),
  ['a-init.mp4', 'a-init.mp4', 'a-init.mp4', 'b-init.mp4', 'b-init.mp4', 'b-init.mp4'],
  'each segment carries its Period initialization resource'
);
// merged playlists are resequenced as a contiguous media sequence, regardless
// of DASH startNumber, PTO or the Period boundary
assert.deepEqual(merged.segments.map(s => s.number), [0, 1, 2, 3, 4, 5]);
assert.deepEqual(merged.discontinuityStarts, [3], 'Period boundary marks the break');
assert.deepEqual(
  merged.timelineStarts,
  [{start: 0, timeline: 0}, {start: 6, timeline: 6}],
  'merged playlist keeps both Period timeline starts'
);
assert.equal(merged.segments[3].discontinuity, true);
assert.ok(!merged.segments[0].discontinuity);

// 2. Period without an explicit duration: its end is derived from
//    mediaPresentationDuration minus its own start.
const noDurationXml = wrap(`
<Period id="p0" start="PT0S" duration="PT6S"><AdaptationSet mimeType="video/mp4">
<SegmentTemplate timescale="10" presentationTimeOffset="100" media="a-$Time$.m4s" initialization="a-init.mp4"><SegmentTimeline><S t="100" d="20" r="-1"/></SegmentTimeline></SegmentTemplate>
<Representation id="v" bandwidth="1000" width="640" height="360"/></AdaptationSet></Period>
<Period id="p1" start="PT6S"><AdaptationSet mimeType="video/mp4">
<SegmentTemplate timescale="10" presentationTimeOffset="200" media="b-$Time$.m4s" initialization="b-init.mp4"><SegmentTimeline><S t="200" d="20" r="-1"/></SegmentTimeline></SegmentTemplate>
<Representation id="v" bandwidth="1000" width="640" height="360"/></AdaptationSet></Period>`);

assert.deepEqual(
  onlyPlaylist(noDurationXml).segments.map(summarize),
  merged.segments.map(summarize),
  'omitting the second Period duration yields the same six segments'
);

// 2b. Middle Period without a duration derives its end from the next Period start.
const middleNoDurationXml = wrap(`
<Period id="p0" start="PT0S" duration="PT4S"><AdaptationSet mimeType="video/mp4">
<SegmentTemplate timescale="10" media="a-$Time$.m4s" initialization="a-init.mp4"><SegmentTimeline><S t="0" d="20" r="-1"/></SegmentTimeline></SegmentTemplate>
<Representation id="v" bandwidth="1000" width="640" height="360"/></AdaptationSet></Period>
<Period id="p1" start="PT4S"><AdaptationSet mimeType="video/mp4">
<SegmentTemplate timescale="10" media="b-$Time$.m4s" initialization="b-init.mp4"><SegmentTimeline><S t="0" d="20" r="-1"/></SegmentTimeline></SegmentTemplate>
<Representation id="v" bandwidth="1000" width="640" height="360"/></AdaptationSet></Period>
<Period id="p2" start="PT8S" duration="PT4S"><AdaptationSet mimeType="video/mp4">
<SegmentTemplate timescale="10" media="c-$Time$.m4s" initialization="c-init.mp4"><SegmentTimeline><S t="0" d="20" r="-1"/></SegmentTimeline></SegmentTemplate>
<Representation id="v" bandwidth="1000" width="640" height="360"/></AdaptationSet></Period>`);

const middle = onlyPlaylist(middleNoDurationXml);
assert.deepEqual(
  middle.segments.map(s => s.uri),
  ['a-0.m4s', 'a-20.m4s', 'b-0.m4s', 'b-20.m4s', 'c-0.m4s', 'c-20.m4s'],
  'the duration-less middle Period ends where the next Period starts'
);
assert.deepEqual(middle.discontinuityStarts, [2, 4]);

// 3. Representation-level timescale/PTO override while inheriting the parent
//    SegmentTimeline. The Period end is converted using the Representation's
//    own media clock.
const overrideXml = wrap(`
<Period start="PT0S" duration="PT6S"><AdaptationSet mimeType="video/mp4">
<SegmentTemplate timescale="20" presentationTimeOffset="200" media="parent-$Time$.m4s" initialization="init.mp4">
<SegmentTimeline><S t="50" d="20" r="-1"/></SegmentTimeline></SegmentTemplate>
<Representation id="v" bandwidth="1000" width="640" height="360">
<SegmentTemplate timescale="10" presentationTimeOffset="50" media="rep-$Time$.m4s"/>
</Representation></AdaptationSet></Period>`);

const override = onlyPlaylist(overrideXml);
assert.deepEqual(
  override.segments.map(s => s.uri),
  ['rep-50.m4s', 'rep-70.m4s', 'rep-90.m4s'],
  '$Time$ stays on the inherited media clock; only the unit conversion changes'
);
assert.deepEqual(
  override.segments.map(s => s.presentationTime),
  [0, 2, 4],
  'PTO and timescale from the Representation level drive presentationTime'
);
assert.deepEqual(override.segments.map(s => s.duration), [2, 2, 2]);
assert.deepEqual(override.segments.map(s => s.map.uri), ['init.mp4', 'init.mp4', 'init.mp4']);

// 4. r="-1" followed by another S with an explicit t: the group ends at that
//    t, not at the Period end.
const boundedRepeatXml = wrap(`
<Period start="PT0S" duration="PT5S"><AdaptationSet mimeType="video/mp4">
<SegmentTemplate timescale="10" media="s-$Time$.m4s" initialization="init.mp4">
<SegmentTimeline><S t="0" d="10" r="-1"/><S t="30" d="5"/><S d="5"/></SegmentTimeline>
</SegmentTemplate><Representation id="v" bandwidth="1000" width="640" height="360"/></AdaptationSet></Period>`);

const bounded = onlyPlaylist(boundedRepeatXml);
assert.deepEqual(bounded.segments.map(s => s.uri), ['s-0.m4s', 's-10.m4s', 's-20.m4s', 's-30.m4s', 's-35.m4s']);
assert.deepEqual(bounded.segments.map(s => s.presentationTime), [0, 1, 2, 3, 3.5]);
assert.deepEqual(bounded.segments.map(s => s.duration), [1, 1, 1, 0.5, 0.5]);
assert.deepEqual(bounded.segments.map(s => s.number), [0, 1, 2, 3, 4]);

// 4b. Explicit time gaps and positive repeats keep their existing behavior.
const gapsXml = wrap(`
<Period start="PT0S" duration="PT6S"><AdaptationSet mimeType="video/mp4">
<SegmentTemplate timescale="10" media="g-$Time$.m4s" initialization="init.mp4">
<SegmentTimeline><S t="0" d="10" r="2"/><S t="50" d="10"/></SegmentTimeline>
</SegmentTemplate><Representation id="v" bandwidth="1000" width="640" height="360"/></AdaptationSet></Period>`);

const gaps = onlyPlaylist(gapsXml);
assert.deepEqual(
  gaps.segments.map(s => s.uri),
  ['g-0.m4s', 'g-10.m4s', 'g-20.m4s', 'g-50.m4s'],
  'positive repeats expand as before and explicit gaps are preserved'
);
assert.deepEqual(gaps.segments.map(s => s.presentationTime), [0, 1, 2, 5]);

// 5. Non-divisible end: the last start before the boundary is retained even
//    though it does not evenly divide the Period, keeping its full duration,
//    and no segment starts at the boundary itself.
const nonDivisibleXml = wrap(
  `<Period start="PT0S" duration="PT5S"><AdaptationSet mimeType="video/mp4">
<SegmentTemplate timescale="10" media="n-$Time$.m4s" initialization="init.mp4">
<SegmentTimeline><S t="0" d="20" r="-1"/></SegmentTimeline>
</SegmentTemplate><Representation id="v" bandwidth="1000" width="640" height="360"/></AdaptationSet></Period>`,
  'type="static" mediaPresentationDuration="PT5S"'
);

const nonDivisible = onlyPlaylist(nonDivisibleXml);
assert.deepEqual(nonDivisible.segments.map(s => s.uri), ['n-0.m4s', 'n-20.m4s', 'n-40.m4s']);
assert.deepEqual(nonDivisible.segments.map(s => s.presentationTime), [0, 2, 4]);
assert.deepEqual(nonDivisible.segments.map(s => s.duration), [2, 2, 2], 'duration is not rewritten');
assert.ok(!nonDivisible.segments.some(s => s.uri === 'n-50.m4s'), 'no segment at the end boundary');

// 6. SegmentList with a SegmentTimeline trailing r="-1", finite SegmentURL
//    entries with byte ranges, and a ranged Initialization. URL/range/map
//    correspondence stays positional.
const listXml = wrap(
  `<Period start="PT0S" duration="PT5S"><AdaptationSet mimeType="video/mp4">
<SegmentList timescale="1000">
<Initialization sourceURL="init.mp4" range="0-99"/>
<SegmentTimeline><S t="0" d="2000" r="-1"/></SegmentTimeline>
<SegmentURL media="seg-1.ts" mediaRange="100-199"/>
<SegmentURL media="seg-2.ts" mediaRange="200-299"/>
<SegmentURL media="seg-3.ts" mediaRange="300-399"/>
</SegmentList>
<Representation id="v" bandwidth="1000" width="640" height="360"/></AdaptationSet></Period>`,
  'type="static" mediaPresentationDuration="PT5S"'
);

const list = onlyPlaylist(listXml);
assert.deepEqual(list.segments.map(s => s.uri), ['seg-1.ts', 'seg-2.ts', 'seg-3.ts']);
assert.deepEqual(list.segments.map(s => s.presentationTime), [0, 2, 4]);
assert.deepEqual(list.segments.map(s => s.duration), [2, 2, 2]);
assert.deepEqual(list.segments.map(s => s.number), [0, 1, 2]);
assert.deepEqual(
  list.segments.map(s => s.byterange),
  [{offset: 100, length: 100}, {offset: 200, length: 100}, {offset: 300, length: 100}],
  'each SegmentURL keeps its own byte range'
);
assert.deepEqual(
  list.segments.map(s => s.map.uri),
  ['init.mp4', 'init.mp4', 'init.mp4']
);
assert.deepEqual(list.segments[0].map.byterange, {offset: 0, length: 100});
assert.equal(list.segments[0].map.resolvedUri, 'https://media.invalid/init.mp4');
assert.equal(list.segments[0].resolvedUri, 'https://media.invalid/seg-1.ts');

// 7. Plain single-Period static manifest without negative repeats.
const singleXml = wrap(
  `<Period start="PT0S" duration="PT4S"><AdaptationSet mimeType="video/mp4">
<SegmentTemplate timescale="10" media="p-$Time$.m4s" initialization="init.mp4">
<SegmentTimeline><S t="0" d="20"/><S t="20" d="20"/></SegmentTimeline>
</SegmentTemplate><Representation id="v" bandwidth="1000" width="640" height="360"/></AdaptationSet></Period>`,
  'type="static" mediaPresentationDuration="PT4S"'
);

const single = onlyPlaylist(singleXml);
assert.deepEqual(single.segments.map(summarize), [
  {uri: 'p-0.m4s', number: 0, presentationTime: 0, duration: 2, map: 'init.mp4', discontinuity: false},
  {uri: 'p-20.m4s', number: 1, presentationTime: 2, duration: 2, map: 'init.mp4', discontinuity: false}
]);

// 7b. Original DASH startNumber drives $Number$ URLs and is not shifted by
//     PTO or Period switches.
const startNumberXml = wrap(`
<Period start="PT0S" duration="PT4S"><AdaptationSet mimeType="video/mp4">
<SegmentTemplate startNumber="1" timescale="10" presentationTimeOffset="100" media="a-$Number$.m4s" initialization="a-init.mp4"><SegmentTimeline><S t="100" d="20" r="1"/></SegmentTimeline></SegmentTemplate>
<Representation id="v" bandwidth="1000" width="640" height="360"/></AdaptationSet></Period>
<Period start="PT4S" duration="PT4S"><AdaptationSet mimeType="video/mp4">
<SegmentTemplate startNumber="100" timescale="10" presentationTimeOffset="200" media="b-$Number$.m4s" initialization="b-init.mp4"><SegmentTimeline><S t="200" d="20" r="1"/></SegmentTimeline></SegmentTemplate>
<Representation id="v" bandwidth="1000" width="640" height="360"/></AdaptationSet></Period>`);

const startNumber = onlyPlaylist(startNumberXml);
assert.deepEqual(
  startNumber.segments.map(s => s.uri),
  ['a-1.m4s', 'a-2.m4s', 'b-100.m4s', 'b-101.m4s'],
  'each Period keeps its own startNumber in its URLs'
);
assert.deepEqual(startNumber.segments.map(s => s.number), [0, 1, 2, 3], 'merged media sequence is contiguous');
assert.deepEqual(startNumber.segments.map(s => s.presentationTime), [0, 2, 4, 6]);
assert.deepEqual(startNumber.discontinuityStarts, [2]);

// 8. Rejection paths: explicit errors instead of empty arrays or endless loops.
const assertThrows = (mpd, code) => {
  assert.throws(() => parse(mpd, {manifestUri: BASE}), (err) => {
    assert.match(err.message, new RegExp(code));
    return true;
  }, `expected ${code}`);
};

// 8a. trailing r="-1" with no derivable Period end
assertThrows(wrap(
  `<Period start="PT0S"><AdaptationSet mimeType="video/mp4">
<SegmentTemplate timescale="10" media="x-$Time$.m4s" initialization="init.mp4"><SegmentTimeline><S t="0" d="20" r="-1"/></SegmentTimeline></SegmentTemplate>
<Representation id="v" bandwidth="1000" width="640" height="360"/></AdaptationSet></Period>`,
  'type="static"'
), 'PERIOD_END_UNSPECIFIED');

// 8b. non-positive d
assertThrows(wrap(
  `<Period start="PT0S" duration="PT6S"><AdaptationSet mimeType="video/mp4">
<SegmentTemplate timescale="10" media="x-$Time$.m4s" initialization="init.mp4"><SegmentTimeline><S t="0" d="0" r="-1"/></SegmentTimeline></SegmentTemplate>
<Representation id="v" bandwidth="1000" width="640" height="360"/></AdaptationSet></Period>`
), 'SEGMENT_DURATION_INVALID');

assertThrows(wrap(
  `<Period start="PT0S" duration="PT6S"><AdaptationSet mimeType="video/mp4">
<SegmentTemplate timescale="10" media="x-$Time$.m4s" initialization="init.mp4"><SegmentTimeline><S t="0" d="-5" r="-1"/></SegmentTimeline></SegmentTemplate>
<Representation id="v" bandwidth="1000" width="640" height="360"/></AdaptationSet></Period>`
), 'SEGMENT_DURATION_INVALID');

// 8c. zero timescale
assertThrows(wrap(
  `<Period start="PT0S" duration="PT6S"><AdaptationSet mimeType="video/mp4">
<SegmentTemplate timescale="0" media="x-$Time$.m4s" initialization="init.mp4"><SegmentTimeline><S t="0" d="20" r="-1"/></SegmentTimeline></SegmentTemplate>
<Representation id="v" bandwidth="1000" width="640" height="360"/></AdaptationSet></Period>`
), 'TIMESCALE_INVALID');

// 8d. integer that cannot be represented safely
assertThrows(wrap(
  `<Period start="PT0S" duration="PT6S"><AdaptationSet mimeType="video/mp4">
<SegmentTemplate timescale="10" media="x-$Time$.m4s" initialization="init.mp4"><SegmentTimeline><S t="99999999999999999999" d="20" r="-1"/></SegmentTimeline></SegmentTemplate>
<Representation id="v" bandwidth="1000" width="640" height="360"/></AdaptationSet></Period>`
), 'SEGMENT_TIME_UNSAFE_INTEGER');

console.log('period timeline: ok');
