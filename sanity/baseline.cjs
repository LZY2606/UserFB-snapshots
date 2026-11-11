const assert = require('node:assert/strict');
const {WebVTT, VTTCue, VTTRegion} = require('../lib');
function observe(parts) {
  const parser = new WebVTT.Parser({VTTCue, VTTRegion}, new TextDecoder());
  const cues = [], errors = [], regions = [];
  let flushes = 0;
  parser.oncue = cue => cues.push(cue);
  parser.onparsingerror = error => errors.push(error.code);
  parser.onregion = region => regions.push(region);
  parser.onflush = () => flushes++;
  for (const part of parts) parser.parse(part);
  const before = cues.map(c => c.text);
  parser.flush();
  return {cues, errors, regions, flushes, before};
}
const normal = Buffer.from('WEBVTT\nRegion: id=r width=30%\nRegion: id=r width=40% lines=2\n\nNOTE ignored\nnot a cue\n\nbad\ninvalid --> timing\nskip\n\nid\n00:01.000 --> 00:02.000 region:r line:25% position:30% size:70% align:start\n中🙂\n\n');
for (const parts of [[normal], [...normal].map(b => Buffer.from([b]))]) {
  const r = observe(parts);
  assert.equal(r.cues.length, 1); assert.deepEqual(r.errors, [1]);
  assert.equal(r.regions.length, 2); assert.equal(r.flushes, 1);
  assert.equal(r.cues[0].region, r.regions[1]);
  assert.equal(r.regions[0].width, 30); assert.equal(r.regions[1].width, 40);
  assert.equal(r.cues[0].text, '中🙂'); assert.equal(r.cues[0].line, 25);
  assert.equal(r.cues[0].snapToLines, false);
}
console.log('BASELINE_NORMAL_OK utf8=whole,bytes region=last errors=1');
const source = 'WEBVTT\n\n00:01.000 --> 00:02.000\nfirst\n00:03.000 --> 00:04.000\nsecond\n\n';
const bytes = Buffer.from(source), cut = source.indexOf('second');
const whole = observe([bytes]);
assert.deepEqual(whole.cues.map(c => [c.startTime, c.endTime, c.text]), [[1,2,'first'],[3,4,'second']]);
assert.deepEqual(whole.errors, []);
console.log('BASELINE_WHOLE_OK cues=first,second errors=none');
for (const [name, parts] of [['line-end', [bytes.subarray(0,cut), bytes.subarray(cut)]], ['bytes', [...bytes].map(b => Buffer.from([b]))]]) {
  const r = observe(parts);
  assert.deepEqual(r.cues.map(c => c.text), ['first']); assert.deepEqual(r.errors, [1]);
  console.log(`BASELINE_DEFECT replay=${name} cues=first errors=1`);
}
const cr = observe([Buffer.from(source.replaceAll('\n', '\r'))]);
assert.deepEqual(cr.cues, []); assert.deepEqual(cr.errors, [0]);
console.log('BASELINE_DEFECT bare-CR cues=none errors=0');
const tail = observe([Buffer.concat([Buffer.from('WEBVTT\n\n00:01.000 --> 00:02.000\nx'), Buffer.from([0xf0,0x9f])])]);
assert.deepEqual(tail.before, []); assert.equal(tail.cues[0].text, 'x\ufffd');
assert.deepEqual(tail.errors, []); assert.equal(tail.flushes, 1);
console.log('BASELINE_EOF_OK decoder-tail=replacement cue-on-flush=1');
