import assert from 'node:assert/strict';
import '../src/main/js/animation.js';
import {parse,wrap,find,generateISD,ns} from './xml.mjs';
const d=parse("<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:tts=\"http://www.w3.org/ns/ttml#styling\"><head><layout><region xml:id=\"r\" dur=\"6s\"><animate dur=\"2s\" repeatCount=\"2.25\" fill=\"freeze\" tts:opacity=\"0;1\"/></region></layout></head><body><div region=\"r\"><p dur=\"6s\">A</p></div></body></tt>");
assert.equal(find(generateISD(d,1),'r').styleAttrs[ns+'opacity'],0.5,'ANIMATE_CLOCK halfway');
assert.equal(find(generateISD(d,5),'r').styleAttrs[ns+'opacity'],0.25,'ANIMATE_CLOCK partial freeze');
assert.deepEqual(d.getContinuousAnimationRanges(),[[0,4.5]],'ANIMATE_RANGES');
console.log('ANIMATION_API_OK');
