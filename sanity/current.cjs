const assert = require('node:assert/strict');
const bundle = require('../');
const shader = [
 {id:19,file:'entry.frag',entry:true,deps:{'./gain':7},source:[
  'precision mediump float;',
  '#pragma glslify: cold = require(./gain, factor=2.0)',
  '#pragma glslify: warm = require(./gain, factor=3.0)',
  'void main(){gl_FragColor=vec4(cold(1.0),warm(1.0),0.0,1.0);}'
 ].join('\n')},
 {id:7,file:'gain.glsl',deps:{},source:[
  'float gain(float x) { return factor*x; }',
  '#pragma glslify: export(gain)'
 ].join('\n')}
];
const before=JSON.stringify(shader),src=bundle(shader);
assert.equal(JSON.stringify(shader),before);
assert.match(src,/#define GLSLIFY 1/);
assert.match(src,/return 2\.0\*x/);assert.match(src,/return 3\.0\*x/);
assert.equal((src.match(/float gain_/g)||[]).length,2);
assert.equal(bundle(shader),src);
console.log('GLSL_BUNDLE_BASELINE_OK specializations=2 deterministic=true');
console.log(src);
