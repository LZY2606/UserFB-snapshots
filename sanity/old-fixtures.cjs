// Run existing parser fixture expectations through the public Node entry point.
// The historical PhantomJS serializer represents a null region as "".
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {WebVTT, VTTCue, VTTRegion} = require('../lib');
const fixtures = [
  ['file-layout/cue-spacing', 'file-layout/cue-spacing'],
  ['file-layout/many-comments', 'file-layout/many-comments'],
  ['file-layout/header-no-new-line', 'file-layout/no-output'],
  ['file-layout/webvtt-no-bom', 'file-layout/with-data'],
  ['cue-times/fraction-digits', 'cue-times/fraction-digits'],
  ['cue-settings/line/percent-value', 'cue-settings/line/percent-value'],
  ['cue-settings/line/integer-value', 'cue-settings/line/integer-value'],
  ['regions/no-line-break', 'regions/good-region'],
  ['regions/line-break', 'regions/bad-region'],
  ['integration/spec-example', 'integration/spec-example']
];
let runs = 0;
for (const [input, expected] of fixtures) {
  const raw = fs.readFileSync(path.join(__dirname, '../tests', input + '.vtt'));
  const want = JSON.parse(fs.readFileSync(path.join(__dirname, '../tests', expected + '.json')));
  for (const c of want.cues) if (c.region === '') c.region = null;
  for (const mode of ['utf8', 'string']) {
    const got = {cues: [], regions: []};
    const p = new WebVTT.Parser({VTTCue, VTTRegion}, mode === 'utf8' ? new TextDecoder() : WebVTT.StringDecoder());
    p.oncue = cue => got.cues.push(JSON.parse(JSON.stringify(cue.toJSON())));
    p.onregion = region => got.regions.push(JSON.parse(JSON.stringify(region)));
    p.onparsingerror = () => {}; // Existing JSON fixtures specify cues/regions, not errors.
    p.parse(mode === 'utf8' ? raw : raw.toString('utf8').replace(/^\ufeff/, ''));
    p.flush();
    assert.deepEqual(got, want, input + ':' + mode);
    console.log(`OLD_FIXTURE_OK ${input} ${mode}`);
    runs++;
  }
}
assert.equal(runs, 20);
console.log('OLD_FIXTURES_OK files=10 runs=20');
