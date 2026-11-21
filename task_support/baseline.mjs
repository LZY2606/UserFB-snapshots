import assert from 'node:assert/strict';
import * as api from '../src/index.js';
const text = `[Script Info]
PlayResX: 640
PlayResY: 360
[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, MarginL, MarginR, MarginV
Style: Default,Arial,20,&H00FFFFFF,10,10,10
Style: Alt,Arial,30,&H000000FF,20,20,20
[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:03.00,Default,,10,0,0,,{\\fs+2}A{\\fs20}B
`;
const doc = api.compile(text);
assert.equal(doc.dialogues.length, 1);
const fragments = doc.dialogues[0].slices.flatMap(s=>s.fragments);
assert.deepEqual(fragments.map(f=>f.text), ['A','B']);
assert.deepEqual(fragments.map(f=>f.tag.fs), [24,20]);
assert.equal(doc.dialogues[0].margin.left,10);
const output=api.decompile(doc);
const again=api.compile(output);
assert.deepEqual(again.dialogues[0].slices.flatMap(s=>s.fragments).map(f=>f.tag.fs),[24,20]);
assert.equal(again.dialogues[0].start,1);
assert.equal(again.dialogues[0].end,3);
const relative=output.includes('\\fs+2');
const event=api.parse(output).events.dialogue[0];
console.log(`BASELINE_OK fs=24,20 text=AB relative_source=${relative?'retained':'flattened'} explicit_equal_margin=${event.MarginL} updateStyles=${typeof api.updateStyles}`);
