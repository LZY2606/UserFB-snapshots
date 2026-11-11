'use strict';
//
// cancelAndHoldAtTime demo/verification.
//
// Wraps real impl nodes (ConstantSourceNode -> GainNode -> destination) with
// src/api/AudioParam, renders actual PCM and compares every sample against
// independent closed-form formulas (absolute tolerance 2e-5).
//
// Run: NODE_PATH=offline/node_modules node offline/hold_demo.js
//
const assert = require('node:assert/strict');
const Context = require('../src/impl/AudioContext');
const Source = require('../src/impl/ConstantSourceNode');
const Gain = require('../src/impl/GainNode');
const Param = require('../src/api/AudioParam');

const SR = 8000;
const FRAMES = 96;
const TOL = 2e-5;

function makeGraph(blockSize) {
  const context = new Context({ sampleRate: SR, blockSize, numberOfChannels: 1 });
  const source = new Source(context, { offset: 1 });
  const gain = new Gain(context);

  source.connect(gain);
  gain.connect(context.getDestination());
  source.start(0);
  context.resume();

  const param = new Param(context, gain.getGain());

  return { context, gain, param };
}

function processBlocks(context, channelData, fromFrame, toFrame) {
  for (let n = fromFrame; n < toFrame; n += context.blockSize) {
    context.process(channelData, n);
  }
}

function render(context, frames) {
  const out = new Float32Array(frames);

  processBlocks(context, [ out ], 0, frames);

  return out;
}

function checkClose(actual, expected, label) {
  assert.equal(actual.length, expected.length, `${label}: length`);
  for (let n = 0; n < expected.length; n++) {
    const diff = Math.abs(actual[n] - expected[n]);

    assert.ok(diff <= TOL, `${label}: frame ${n} got ${actual[n]}, expected ${expected[n]}`);
  }
}

function expected(fn, frames) {
  const out = new Float64Array(frames);

  for (let n = 0; n < frames; n++) {
    out[n] = fn(n);
  }

  return out;
}

// case 1: linear ramp 0 -> 1 over 64 frames, held at frame 23.
// frames 0..22 keep the original slope, frame 23 on is 23/64.
// the same sequence is rendered with 16- and 32-frame blocks and the
// common sample range is cross-compared.
{
  const holdValue = 23 / 64;
  const formula = n => (n < 23 ? n / 64 : holdValue);
  const runs = [ 16, 32 ].map((blockSize) => {
    const { context, param } = makeGraph(blockSize);
    const chained = param
      .setValueAtTime(0, 0)
      .linearRampToValueAtTime(1, 64 / SR)
      .cancelAndHoldAtTime(23 / SR);

    assert.equal(chained, param, 'cancelAndHoldAtTime must be chainable');

    return render(context, FRAMES);
  });

  checkClose(runs[0], expected(formula, FRAMES), 'case1 linear hold@23 block=16');
  checkClose(runs[1], expected(formula, FRAMES), 'case1 linear hold@23 block=32');
  checkClose(runs[0], runs[1], 'case1 block=16 vs block=32');
  console.log('case 1 ok: linear ramp held at frame 23 (block 16/32 cross-checked)');
}

// case 2: exponential ramp 1 -> 4 over 64 frames, held at the midpoint
// frame 32, where the value is exactly 2.
{
  const { context, param } = makeGraph(16);

  param.setValueAtTime(1, 0);
  param.exponentialRampToValueAtTime(4, 64 / SR);
  param.cancelAndHoldAtTime(32 / SR);

  const out = render(context, FRAMES);

  checkClose(out, expected(n => (n < 32 ? Math.pow(4, n / 64) : 2), FRAMES), 'case2 exponential hold@32');
  console.log('case 2 ok: exponential ramp held at midpoint (value 2)');
}

// case 3: setTargetAtTime held at frame 48. the held value follows the
// exponential approach formula, it does not jump to the target.
{
  const tc = 0.01;
  const k = SR * tc; // 80 frames
  const { context, param } = makeGraph(16);

  param.setValueAtTime(0, 0);
  param.setTargetAtTime(1, 0, tc);
  param.cancelAndHoldAtTime(48 / SR);

  const out = render(context, FRAMES);
  const holdValue = 1 - Math.exp(-48 / k);

  assert.ok(Math.abs(holdValue - 1) > 0.5, 'setTarget hold must not jump to the target');
  checkClose(out, expected(n => (n < 48 ? 1 - Math.exp(-n / k) : holdValue), FRAMES), 'case3 setTarget hold@48');
  console.log('case 3 ok: setTarget held on the approach curve');
}

// case 4: value curve [0,1,0] over 64 frames, held at frame 48 where the
// curve value is 0.5. the peak at frame 32 stays in place; the curve is
// not re-squeezed into the shorter duration.
// rendered with 16- and 32-frame blocks and cross-compared.
{
  const curve = new Float32Array([ 0, 1, 0 ]);
  const curveValue = (n) => {
    const ix = (n / 64) * (curve.length - 1);
    const i0 = Math.min(ix | 0, curve.length - 2);

    return curve[i0] + (ix % 1) * (curve[i0 + 1] - curve[i0]);
  };
  const formula = n => (n < 48 ? curveValue(n) : 0.5);
  const runs = [ 16, 32 ].map((blockSize) => {
    const { context, param } = makeGraph(blockSize);

    param.setValueCurveAtTime(curve, 0, 64 / SR);
    param.cancelAndHoldAtTime(48 / SR);

    return render(context, FRAMES);
  });

  checkClose(runs[0], expected(formula, FRAMES), 'case4 curve hold@48 block=16');
  checkClose(runs[1], expected(formula, FRAMES), 'case4 curve hold@48 block=32');
  checkClose(runs[0], runs[1], 'case4 block=16 vs block=32');
  assert.ok(Math.abs(runs[0][32] - 1) <= TOL, 'case4: curve peak stays at frame 32');
  console.log('case 4 ok: value curve held mid-way, peak intact (block 16/32 cross-checked)');
}

// case 5: scheduling between process calls. two blocks of a 0 -> 1 ramp
// over 128 frames are rendered first, then a future hold at frame 80 is
// scheduled (twice, to prove idempotency) and the rest is rendered.
{
  const { context, param } = makeGraph(16);
  const out = new Float32Array(FRAMES);
  const channelData = [ out ];

  param.setValueAtTime(0, 0);
  param.linearRampToValueAtTime(1, 128 / SR);

  processBlocks(context, channelData, 0, 32);

  param.cancelAndHoldAtTime(80 / SR);
  param.cancelAndHoldAtTime(80 / SR); // holding again at the same time changes nothing

  processBlocks(context, channelData, 32, FRAMES);

  checkClose(out, expected(n => (n < 80 ? n / 128 : 80 / 128), FRAMES), 'case5 mid-render hold@80');
  console.log('case 5 ok: future hold scheduled between process calls (idempotent)');
}

// case 6: external a-rate modulation. the held value is the automation
// value only; the modulation signal keeps being added per sample.
{
  const { context, gain, param } = makeGraph(16);
  const mod = new Source(context, { offset: 0.25 });

  mod.connect(gain.getGain());
  mod.start(0);

  param.setValueAtTime(0, 0);
  param.linearRampToValueAtTime(1, 64 / SR);
  param.cancelAndHoldAtTime(23 / SR);

  const out = render(context, FRAMES);

  checkClose(out, expected(n => (n < 23 ? n / 64 : 23 / 64) + 0.25, FRAMES), 'case6 hold with modulation');
  console.log('case 6 ok: external modulation still added after the hold');
}

// case 7: a ramp scheduled after the hold continues from the held
// point (frame 23, value 23/64), not from the removed ramp end (frame 64,
// value 1) and not from the default value.
{
  const { context, param } = makeGraph(16);

  param.setValueAtTime(0, 0);
  param.linearRampToValueAtTime(1, 64 / SR);
  param.cancelAndHoldAtTime(23 / SR);
  param.linearRampToValueAtTime(0, 55 / SR);

  const out = render(context, FRAMES);
  const holdValue = 23 / 64;

  checkClose(out, expected((n) => {
    if (n < 23) {
      return n / 64;
    }
    if (n < 55) {
      return holdValue + (n - 23) * (0 - holdValue) / 32;
    }
    return 0;
  }, FRAMES), 'case7 ramp continues from the hold point');
  assert.ok(Math.abs(out[23] - holdValue) <= TOL, 'case7: new ramp starts at the held value');
  console.log('case 7 ok: follow-up ramp starts from the hold point');
}

// case 8: empty timeline holds the current manually set value.
{
  const { context, param } = makeGraph(16);

  param.value = 0.7;
  param.cancelAndHoldAtTime(16 / SR);

  const out = render(context, FRAMES);

  checkClose(out, expected(() => 0.7, FRAMES), 'case8 empty timeline holds manual value');
  console.log('case 8 ok: empty timeline holds the manually set value');
}

// case 9: only future events. the future value must not be applied early;
// the future event is cancelled and the current value is held.
{
  const { context, param } = makeGraph(16);

  param.value = 0.5;
  param.setValueAtTime(1.0, 64 / SR);
  param.cancelAndHoldAtTime(32 / SR);

  const out = render(context, FRAMES);

  checkClose(out, expected(() => 0.5, FRAMES), 'case9 future event not applied early');
  console.log('case 9 ok: future events are not applied early');
}

// case 10: invalid times throw RangeError and leave the schedule and the
// rendered sound untouched.
{
  const schedule = (param) => {
    param.setValueAtTime(0, 0);
    param.linearRampToValueAtTime(1, 64 / SR);
  };
  const reference = (() => {
    const { context, param } = makeGraph(16);

    schedule(param);

    return render(context, FRAMES);
  })();
  const { context, param } = makeGraph(16);

  schedule(param);

  for (const badTime of [ NaN, -1, Infinity, -Infinity, undefined ]) {
    assert.throws(() => param.cancelAndHoldAtTime(badTime), RangeError, `cancelAndHoldAtTime(${badTime})`);
  }

  const out = render(context, FRAMES);

  checkClose(out, reference, 'case10 schedule untouched after RangeError');
  console.log('case 10 ok: invalid times throw RangeError, schedule untouched');
}

// case 11: a time in the past is treated as context.currentTime. after 32
// rendered frames, cancelAndHoldAtTime(0) holds at frame 32.
{
  const { context, param } = makeGraph(16);
  const out = new Float32Array(FRAMES);
  const channelData = [ out ];

  param.setValueAtTime(0, 0);
  param.linearRampToValueAtTime(1, 128 / SR);

  processBlocks(context, channelData, 0, 32);

  param.cancelAndHoldAtTime(0); // past time -> currentTime (frame 32)

  processBlocks(context, channelData, 32, FRAMES);

  checkClose(out, expected(n => (n < 32 ? n / 128 : 32 / 128), FRAMES), 'case11 past time holds at currentTime');
  console.log('case 11 ok: past time is treated as currentTime');
}

// case 12: cancelScheduledValues keeps working as before.
{
  const { context, param } = makeGraph(16);

  param.setValueAtTime(0, 0);
  param.linearRampToValueAtTime(1, 64 / SR);
  param.cancelScheduledValues(32 / SR);

  const out = render(context, FRAMES);

  checkClose(out, expected(() => 0, FRAMES), 'case12 cancelScheduledValues unchanged');
  console.log('case 12 ok: cancelScheduledValues behaves as before');
}

console.log('AUTOMATION_HOLD_OK');
