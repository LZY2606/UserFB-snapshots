const assert = require('node:assert/strict');
const create = require('../texture');
const ndarray = require('ndarray');
const calls = [];
const gl = {MAX_TEXTURE_SIZE:3379, ACTIVE_TEXTURE:34016, TEXTURE0:33984,
 TEXTURE_2D:3553, RGBA:6408, UNSIGNED_BYTE:5121, FLOAT:5126,
 NEAREST:9728, CLAMP_TO_EDGE:33071,
 getParameter(p) { return p === this.MAX_TEXTURE_SIZE ? 1024 : this.TEXTURE0; },
 getExtension() { return null; }, createTexture() { return {}; },
 bindTexture() {}, texParameteri() {},
 texImage2D(...a) { calls.push(['image', ...a]); },
 texSubImage2D(...a) { calls.push(['sub', ...a]); }};
const t = create(gl, [8,4]);
const pixel = ndarray(new Uint8Array([11,22,33,44]),[1,1,4],[4,4,1]);
assert.deepEqual(t.shape.slice(),[8,4]);
t.setPixels(pixel,[0,0],0);
assert.equal(calls.at(-1)[0],'sub');
t.setPixels(pixel,[0,0],1);
assert.deepEqual(calls.at(-1).slice(0,6),['image',3553,1,6408,1,1]);
console.log('existing first mip allocation: level=1 width=1 height=1');
assert.throws(()=>t.setPixels(pixel,[2,0],1),/out of bounds/);
console.log('existing non-square mip rejects x=2 although level width should be 4');
t.setPixels({raw:{width:1,height:1},width:1,height:1},[0,0],2);
assert.equal(calls.at(-1)[2],0);
console.log('existing raw first mip writes level=0; base-level subimage passed');
