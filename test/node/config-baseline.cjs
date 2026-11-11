'use strict';
const assert = require('node:assert/strict');
const {VideoSegmentStream} = require('../../lib/mp4/transmuxer');
const track = {id: 1, type: 'video', timelineStartInfo: {dts: 0, pts: 0, baseMediaDecodeTime: 0}};
const stream = new VideoSegmentStream(track, {keepOriginalTimestamps: true});
const emitted = [];
stream.on('data', ({track: t, boxes}) => emitted.push({width: t.width, samples: t.samples.length, bytes: boxes.length}));
function frame(dts, width, tag) {
  const n = (type, bytes, config) => stream.push({nalUnitType: type, data: Uint8Array.from(bytes), dts, pts: dts, config});
  n('access_unit_delimiter_rbsp', [9, 0x10]);
  n('seq_parameter_set_rbsp', [0x67, tag], {width, height: 360, profileIdc: 66, levelIdc: 30, profileCompatibility: 0});
  n('pic_parameter_set_rbsp', [0x68, tag]);
  n('slice_layer_without_partitioning_rbsp_idr', [0x65, tag]);
}
frame(0, 640, 1);
frame(3000, 1280, 2);
stream.flush();
assert.equal(emitted.length, 1);
assert.equal(emitted[0].width, 640);
assert.equal(emitted[0].samples, 2);
assert.ok(emitted[0].bytes > 32);
console.log('baseline: two IDR configurations share one fragment; width=640 samples=2');
