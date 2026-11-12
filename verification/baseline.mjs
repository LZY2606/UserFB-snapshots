import assert from 'node:assert/strict';
import {compile,decompile,parse} from '../src/index.js';
const input=String.raw`[Script Info]
PlayResX: 640
PlayResY: 360
[V4+ Styles]
Format: Name, Fontsize, PrimaryColour
Style: Default,20,&H00FFFFFF
[Events]
Format: Layer, Start, End, Style, Text
Dialogue: 0,0:00:01.00,0:00:03.00,Default,{\fs20\t(0,1000,\fs40)}A{\r\fs30}B`;
const c=compile(input); assert.equal(c.dialogues.length,1);
const d=c.dialogues[0];assert.equal(d.start,1);assert.equal(d.end,3);
assert.equal(d.slices[0].fragments[0].tag.fs,20);
assert.deepEqual(d.slices[0].fragments[0].tag.t,[{t1:0,t2:1000,accel:1,tag:{fs:40}}]);
assert.equal(d.slices[1].fragments[0].tag.fs,30);
const again=compile(decompile(c));assert.equal(again.dialogues[0].slices.map(s=>s.fragments.map(f=>f.text).join('')).join(''),'AB');
assert.equal(parse(input).events.dialogue.length,1);
console.log('ASS_COMPILE_TRANSFORM_PASS');console.log('ASS_DECOMPILE_ROUNDTRIP_PASS');
