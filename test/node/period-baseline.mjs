import assert from 'node:assert/strict';
import {parse} from './load-source.mjs';
const xml = `<MPD type="static" mediaPresentationDuration="PT12S" xmlns="urn:mpeg:dash:schema:mpd:2011">
<Period id="p0" start="PT0S" duration="PT6S"><AdaptationSet mimeType="video/mp4" codecs="avc1.42E01E">
<SegmentTemplate timescale="10" presentationTimeOffset="100" media="a-$Time$.m4s" initialization="a-init.mp4"><SegmentTimeline><S t="100" d="20" r="-1"/></SegmentTimeline></SegmentTemplate>
<Representation id="v" bandwidth="1000" width="640" height="360"/></AdaptationSet></Period>
<Period id="p1" start="PT6S" duration="PT6S"><AdaptationSet mimeType="video/mp4" codecs="avc1.42E01E">
<SegmentTemplate timescale="10" presentationTimeOffset="200" media="b-$Time$.m4s" initialization="b-init.mp4"><SegmentTimeline><S t="200" d="20" r="-1"/></SegmentTimeline></SegmentTemplate>
<Representation id="v" bandwidth="1000" width="640" height="360"/></AdaptationSet></Period></MPD>`;
const result = parse(xml, {manifestUri: 'https://media.invalid/demo.mpd'});
const segments = result.playlists.flatMap(p => p.segments);
assert.deepEqual(segments.map(s => s.uri), ['a-100.m4s']);
assert.equal(segments[0].presentationTime, 0);
console.log('baseline: 12s/2-period MPD expands only a-100.m4s; expected media needs 6 segments');
