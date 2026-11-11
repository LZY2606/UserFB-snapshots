import assert from 'node:assert/strict';
import {Parser} from './load-source.mjs';
const text = `#EXTM3U
#EXT-X-VERSION:9
#EXT-X-TARGETDURATION:4
#EXT-X-MEDIA-SEQUENCE:41
#EXT-X-DISCONTINUITY-SEQUENCE:7
#EXT-X-SKIP:SKIPPED-SEGMENTS=2
#EXT-X-MAP:URI="init-b.mp4"
#EXTINF:4,
43.m4s
#EXT-X-PART:DURATION=0.5,URI="44.0.m4s"
#EXT-X-PRELOAD-HINT:TYPE=PART,URI="44.1.m4s"
`;
const p = new Parser();
for (let i = 0; i < text.length; i += 7) p.push(text.slice(i, i + 7));
p.end();
assert.equal(p.manifest.skip.skippedSegments, 2);
assert.deepEqual(p.manifest.segments.map(s => s.uri), ['43.m4s']);
assert.equal(p.manifest.segments[0].map.uri, 'init-b.mp4');
assert.equal(p.manifest.preloadSegment.parts[0].uri, '44.0.m4s');
console.log('baseline: delta declares 2 skipped segments; parsed=43.m4s preload=44.0.m4s');
