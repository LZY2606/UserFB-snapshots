/**
 * Regression tests for the unified clip clock, CPU preview compositing,
 * and transition rendering.  Registered into task_support/check.mjs groups
 * and executed by `node task_support/run.mjs target`.
 */

import { group, check } from '../task_support/check.mjs';
import {
  Project,
  PreviewRenderer,
  CpuBackend,
  Transition,
} from '../src/index.js';

// ─── Helpers ────────────────────────────────────────────────────────────────

/** Build a solid-colour frame. */
function solid(width, height, [r, g, b, a]) {
  const data = new Uint8ClampedArray(width * height * 4);
  for (let i = 0; i < data.length; i += 4) {
    data[i] = r; data[i + 1] = g; data[i + 2] = b; data[i + 3] = a;
  }
  return { width, height, data };
}

/** Flatten a frame's pixels to a comparable string. */
function pixels(frame) {
  return Array.from(frame.data).join(',');
}

/** Run an async fn, return the thrown error or null. */
async function catchError(fn) {
  try { await fn(); } catch (err) { return err; }
  return null;
}

// ─── clock ──────────────────────────────────────────────────────────────────

group('clock', () => {
  // Duration follows the playback rate; track length, auto-append and the
  // active interval all use the rate-adjusted duration.
  const p = new Project({ fps: 30, width: 2, height: 2 });
  const track = p.addTrack('video');
  const fast = track.addVideo('memory:fast', { inPoint: 2, outPoint: 10, startTime: 0 });
  fast.speed(2);
  check(fast.duration === 4, 'duration = (outPoint - inPoint) / rate');
  check(fast.endTime === 4, 'endTime uses rate-adjusted duration');
  check(track.getDuration() === 4, 'track length uses rate-adjusted duration');

  const appended = track.addVideo('memory:appended', { inPoint: 0, outPoint: 1 });
  check(appended.startTime === 4, 'auto-append starts after the rate-adjusted end');

  check(p.timeline.getClipsAtTime(3.999).includes(fast), 'clip active just before end');
  check(!p.timeline.getClipsAtTime(4).includes(fast), 'active interval is half-open [start, end)');

  // Forward sampling: inPoint + timeline offset × rate.
  check(fast.getSourceTimeAt(0) === 2, 'forward sampling starts at inPoint');
  check(fast.getSourceTimeAt(1.5) === 5, 'forward sampling scales by rate');

  // AudioClip shares the same clock rules.
  const audioProject = new Project({ fps: 30 });
  const audioTrack = audioProject.addTrack('audio');
  const audio = audioTrack.addAudio('memory:audio', { inPoint: 0, outPoint: 8, startTime: 0 });
  audio.speed(4);
  check(audio.duration === 2, 'audio duration = trim / rate');
  check(audioTrack.getDuration() === 2, 'audio track length uses rate-adjusted duration');
  check(audio.getSourceTimeAt(1) === 4, 'audio sampling scales by rate');

  // Reversed sampling: outPoint − timeline offset × rate; the first
  // timeline instant maps to exactly outPoint.
  const rev = track.addVideo('memory:rev', { inPoint: 2, outPoint: 10, startTime: 10 });
  rev.speed(2).reverse(true);
  check(rev.duration === 4, 'reversed duration = trim / rate');
  check(rev.endTime === 14, 'reversed endTime uses rate-adjusted duration');
  check(rev.getSourceTimeAt(10) === 10, 'reversed sampling starts exactly at outPoint');
  check(rev.getSourceTimeAt(11) === 8, 'reversed sampling walks backwards by rate');
  check(Math.abs(rev.getSourceTimeAt(13.999) - 2.002) < 1e-9, 'reversed sampling approaches inPoint');

  // Split at 1× keeps the historical behaviour exactly.
  const normal = track.addVideo('memory:normal', { inPoint: 4, outPoint: 8, startTime: 20 });
  const { head: nHead, tail: nTail } = normal.split(22);
  check(nHead.inPoint === 4 && nHead.outPoint === 6, '1× split head keeps source start');
  check(nTail.inPoint === 6 && nTail.outPoint === 8, '1× split tail starts at the cut');
  check(nTail.startTime === 22, '1× split tail starts at the split time');

  // Split at 2× forward: the cut lands at inPoint + offset × rate and
  // sampling is identical before and after, for every active instant.
  const sp = new Project({ fps: 30 });
  const sTrack = sp.addTrack('video');
  const c = sTrack.addVideo('memory:sp', { inPoint: 1, outPoint: 9, startTime: 2 });
  c.speed(2); // active [2, 6)
  const sampleTimes = [2, 2.5, 3, 3.5, 4, 4.5, 5, 5.5, 5.999];
  const before = sampleTimes.map((t) => c.getSourceTimeAt(t));
  const { head, tail } = c.split(4);
  check(head.inPoint === 1 && head.outPoint === 5, '2× split head source range');
  check(tail.inPoint === 5 && tail.outPoint === 9, '2× split tail source range');
  check(tail.speed() === 2, 'tail keeps the playback rate');
  check(head.duration === 2 && tail.duration === 2, 'split halves the rate-adjusted duration');
  sampleTimes.forEach((t, i) => {
    const piece = t < 4 ? head : tail;
    check(piece.getSourceTimeAt(t) === before[i], `sampling continuous across 2× split at t=${t}`);
  });

  // Splitting the tail again stays consistent (repeated splits).
  const before2 = [4.5, 5, 5.5].map((t) => tail.getSourceTimeAt(t));
  const { head: tailHead, tail: tailTail } = tail.split(5);
  check(tailHead.inPoint === 5 && tailHead.outPoint === 7, 'second split head source range');
  check(tailTail.inPoint === 7 && tailTail.outPoint === 9, 'second split tail source range');
  [4.5, 5, 5.5].forEach((t, i) => {
    const piece = t < 5 ? tailHead : tailTail;
    check(piece.getSourceTimeAt(t) === before2[i], `sampling continuous across repeated split at t=${t}`);
  });

  // Split a reversed 2× clip: the head keeps the out-point side, the tail
  // keeps the in-point side, and sampling is continuous across the cut.
  const rp = new Project({ fps: 30 });
  const rTrack = rp.addTrack('video');
  const rc = rTrack.addVideo('memory:rsplit', { inPoint: 2, outPoint: 10, startTime: 0 });
  rc.speed(2).reverse(true); // active [0, 4)
  const rTimes = [0, 0.5, 1, 1.5, 2, 2.5, 3, 3.5, 3.999];
  const rBefore = rTimes.map((t) => rc.getSourceTimeAt(t));
  const { head: rHead, tail: rTail } = rc.split(1);
  check(rHead.inPoint === 8 && rHead.outPoint === 10, 'reversed split head keeps out-point side');
  check(rTail.inPoint === 2 && rTail.outPoint === 8, 'reversed split tail keeps in-point side');
  check(rHead.isReversed && rTail.isReversed, 'both halves keep the direction');
  check(rTail.getSourceTimeAt(1) === 8, 'reversed tail starts sampling at the cut');
  rTimes.forEach((t, i) => {
    const piece = t < 1 ? rHead : rTail;
    check(piece.getSourceTimeAt(t) === rBefore[i], `reversed sampling continuous at t=${t}`);
  });

  // Non-video clip types keep the original 1:1 split behaviour.
  const ip = new Project({ fps: 30 });
  const img = ip.addTrack('image').addImage('memory:img', { inPoint: 0, outPoint: 10, startTime: 0 });
  const { head: iHead, tail: iTail } = img.split(3);
  check(iHead.outPoint === 3 && iTail.inPoint === 3, 'image clip split unchanged');
});

// ─── transition ─────────────────────────────────────────────────────────────

group('transition', async () => {
  // crossDissolve interpolates premultiplied colour and alpha, then
  // converts back to non-premultiplied bytes.
  const dissolve = new Transition('crossDissolve', 2, { easing: 'linear' });
  const from = solid(1, 1, [200, 100, 0, 128]);
  const to = solid(1, 1, [0, 100, 40, 255]);
  const fromBefore = pixels(from);
  const toBefore = pixels(to);
  const ctx = dissolve.apply({ time: 1, fromFrame: from, toFrame: to });
  const fa = 128 / 255;
  const outAlpha = fa * 0.5 + 1 * 0.5;
  check(ctx.frame.data[0] === Math.round((200 * fa * 0.5) / outAlpha), 'dissolve R from premult lerp');
  check(ctx.frame.data[1] === Math.round((100 * fa * 0.5 + 100 * 0.5) / outAlpha), 'dissolve G from premult lerp');
  check(ctx.frame.data[2] === Math.round((40 * 0.5) / outAlpha), 'dissolve B from premult lerp');
  check(ctx.frame.data[3] === Math.round(outAlpha * 255), 'dissolve alpha lerp');
  check(pixels(from) === fromBefore && pixels(to) === toBefore, 'apply does not mutate source frames');

  // Anchor case: fading from fully transparent yields the incoming colour
  // at half alpha.
  const half = new Transition('crossDissolve', 1, { easing: 'linear' }).apply({
    time: 0.5,
    fromFrame: solid(1, 1, [0, 0, 0, 0]),
    toFrame: solid(1, 1, [40, 80, 120, 255]),
  });
  check(pixels(half.frame) === '40,80,120,128', 'dissolve from transparent keeps incoming colour');

  // Fully transparent on both sides stays transparent with RGB = 0.
  const transparent = new Transition('crossDissolve', 1, { easing: 'linear' }).apply({
    time: 0.5,
    fromFrame: solid(1, 1, [0, 0, 0, 0]),
    toFrame: solid(1, 1, [0, 0, 0, 0]),
  });
  check(pixels(transparent.frame) === '0,0,0,0', 'transparent dissolve keeps RGB 0');

  // wipeLeft reveals the incoming clip from the left edge.
  const wipe = new Transition('wipeLeft', 2, { easing: 'linear' });
  const wipeFrom = solid(4, 1, [255, 0, 0, 255]);
  const wipeTo = solid(4, 1, [0, 0, 255, 255]);
  const wCtx = wipe.apply({ time: 1, fromFrame: wipeFrom, toFrame: wipeTo }); // progress 0.5
  check(pixels(wCtx.frame) === [
    '0,0,255,255', '0,0,255,255', '255,0,0,255', '255,0,0,255',
  ].join(','), 'wipeLeft reveals columns where x/width < progress');
  const wQuarter = wipe.apply({ time: 0.5, fromFrame: wipeFrom, toFrame: wipeTo }); // progress 0.25
  check(pixels(wQuarter.frame) === [
    '0,0,255,255', '255,0,0,255', '255,0,0,255', '255,0,0,255',
  ].join(','), 'wipeLeft quarter progress reveals only the first column');

  // Unsupported enabled types throw; disabled transitions are no-ops.
  const zoomErr = await catchError(async () =>
    new Transition('zoom', 1).apply({ time: 0, fromFrame: wipeFrom, toFrame: wipeTo }));
  check(/not supported/.test(zoomErr?.message ?? ''), 'unsupported transition type throws');
  const disabled = new Transition('crossDissolve', 1);
  disabled.disable();
  const disabledCtx = disabled.apply({ time: 0.5, fromFrame: wipeFrom, toFrame: wipeTo });
  check(disabledCtx.frame === undefined, 'disabled transition leaves context untouched');

  // Renderer integration: an enabled transition on the outgoing clip turns
  // the whole overlap into a single blended layer.
  const proj = new Project({ fps: 10, width: 2, height: 1 });
  const track = proj.addTrack('video');
  const a = track.addVideo('memory:a', { inPoint: 0, outPoint: 4, startTime: 0 });
  const b = track.addVideo('memory:b', { inPoint: 0, outPoint: 4, startTime: 3 });
  const colours = { 'memory:a': [255, 0, 0, 255], 'memory:b': [0, 0, 255, 255] };
  const calls = [];
  const backend = new CpuBackend({
    width: 2,
    height: 1,
    readFrame: (clip, sourceTime) => {
      calls.push({ path: clip.asset.path, sourceTime });
      return solid(2, 1, colours[clip.asset.path]);
    },
  });
  const renderer = new PreviewRenderer(proj.timeline, { backend });
  const transition = new Transition('crossDissolve', 1, { easing: 'linear' });
  transition.link(a, b);
  a.addEffect(transition);

  const blended = await renderer.render(3.5);
  check(pixels(blended) === '128,0,128,255,128,0,128,255', 'overlap renders as one blended layer');
  check(calls.length === 2, 'transition layer samples each clip exactly once');
  check(calls[0].path === 'memory:a' && calls[0].sourceTime === 3.5, 'outgoing clip sampled at its source time');
  check(calls[1].path === 'memory:b' && calls[1].sourceTime === 0.5, 'incoming clip sampled at its source time');

  // Outside the overlap the clips render individually.
  calls.length = 0;
  const solo = await renderer.render(2);
  check(pixels(solo) === '255,0,0,255,255,0,0,255', 'before the overlap only the outgoing clip draws');
  check(calls.length === 1 && calls[0].path === 'memory:a', 'single clip sampled once outside overlap');

  // Overlap length must equal the transition duration.
  transition.duration = 2;
  const overlapErr = await catchError(() => renderer.render(3.5));
  check(/does not match the.*overlap/i.test(overlapErr?.message ?? ''), 'mismatched overlap throws');
  transition.duration = 1;

  // Both clip ids must match the overlapping pair.
  transition.params.toClipId = 'clip_does_not_exist';
  const idErr = await catchError(() => renderer.render(3.5));
  check(/links \(.*\) but the overlapping clips/i.test(idErr?.message ?? ''), 'mismatched clip ids throw');
  transition.params.toClipId = b.id;

  // A disabled transition falls back to plain overlap stacking.
  transition.disable();
  calls.length = 0;
  const stacked = await renderer.render(3.5);
  check(pixels(stacked) === '0,0,255,255,0,0,255,255', 'disabled transition stacks both clips in order');
  check(calls.length === 2, 'disabled transition samples both clips');
  transition.enable();

  // An enabled but unsupported transition type throws during render.
  const weird = new Transition('wipeRight', 1);
  weird.link(a, b);
  a.removeEffect(transition.id);
  a.addEffect(weird);
  const unsupportedErr = await catchError(() => renderer.render(3.5));
  check(/not supported/.test(unsupportedErr?.message ?? ''), 'unsupported type throws during render');
});

// ─── composition ────────────────────────────────────────────────────────────

group('composition', async () => {
  // Tracks stack bottom → top with source-over; results are rounded per
  // channel and fully transparent pixels keep RGB 0.
  const proj = new Project({ fps: 2, width: 2, height: 1 });
  const bottom = proj.addTrack('video');
  const top = proj.addTrack('video');
  bottom.addVideo('memory:bottom', { inPoint: 0, outPoint: 4, startTime: 0 });
  top.addVideo('memory:top', { inPoint: 0, outPoint: 4, startTime: 0 });
  const colours = { 'memory:bottom': [255, 0, 0, 255], 'memory:top': [0, 0, 255, 128] };
  const calls = [];
  const backend = new CpuBackend({
    width: 2,
    height: 1,
    readFrame: async (clip, sourceTime) => {
      calls.push({ path: clip.asset.path, sourceTime });
      return solid(2, 1, colours[clip.asset.path]);
    },
  });
  const renderer = new PreviewRenderer(proj.timeline, { backend });

  const frame = await renderer.render(1);
  check(frame.width === 2 && frame.height === 1, 'frame matches the backend size');
  check(pixels(frame) === '127,0,128,255,127,0,128,255', 'source-over across tracks, rounded per channel');
  check(calls[0].path === 'memory:bottom' && calls[1].path === 'memory:top', 'tracks drawn bottom to top');

  // Hidden tracks and hidden clips are never sampled.
  top.visible = false;
  calls.length = 0;
  const withoutTop = await renderer.render(1);
  check(pixels(withoutTop) === '255,0,0,255,255,0,0,255', 'hidden track contributes nothing');
  check(calls.length === 1 && calls[0].path === 'memory:bottom', 'hidden track is not sampled');
  top.visible = true;
  top.getClips()[0].visible = false;
  calls.length = 0;
  await renderer.render(1);
  check(calls.length === 1 && calls[0].path === 'memory:bottom', 'hidden clip is not sampled');
  top.getClips()[0].visible = true;

  // Non-video clips are skipped entirely.
  const mixed = new Project({ fps: 2, width: 1, height: 1 });
  const textTrack = mixed.addTrack('text');
  textTrack.addText('hello', { startTime: 0, inPoint: 0, outPoint: 2 });
  const mixedCalls = [];
  const mixedRenderer = new PreviewRenderer(mixed.timeline, {
    backend: new CpuBackend({
      width: 1,
      height: 1,
      readFrame: (clip, sourceTime) => { mixedCalls.push(sourceTime); return solid(1, 1, [1, 2, 3, 255]); },
    }),
  });
  const empty = await mixedRenderer.render(1);
  check(mixedCalls.length === 0, 'non-video clips are never sampled');
  check(pixels(empty) === '0,0,0,0', 'empty render is transparent with RGB 0');

  // Half-open interval: a clip is visible at startTime but not at endTime.
  calls.length = 0;
  const atStart = await renderer.render(0);
  check(pixels(atStart) === '127,0,128,255,127,0,128,255', 'clip visible at its startTime');
  const atEnd = await renderer.render(4);
  check(pixels(atEnd) === '0,0,0,0,0,0,0,0', 'clip gone at its endTime (left-closed, right-open)');

  // Overlapping clips without a transition stack in getClips() order.
  const ov = new Project({ fps: 2, width: 1, height: 1 });
  const ovTrack = ov.addTrack('video');
  ovTrack.addVideo('memory:first', { inPoint: 0, outPoint: 2, startTime: 0 });
  ovTrack.addVideo('memory:second', { inPoint: 0, outPoint: 2, startTime: 1 });
  const ovColours = { 'memory:first': [255, 0, 0, 255], 'memory:second': [0, 0, 255, 128] };
  const ovRenderer = new PreviewRenderer(ov.timeline, {
    backend: new CpuBackend({
      width: 1,
      height: 1,
      readFrame: (clip) => solid(1, 1, ovColours[clip.asset.path]),
    }),
  });
  const overlapFrame = await ovRenderer.render(1.5);
  check(pixels(overlapFrame) === '127,0,128,255', 'plain overlap stacks in getClips() order');

  // Returned frames own their storage: earlier frames are never mutated or
  // reused by later renders, and source frames are never written to.
  const seq = new Project({ fps: 2, width: 1, height: 1 });
  const seqTrack = seq.addTrack('video');
  seqTrack.addVideo('memory:red', { inPoint: 0, outPoint: 1, startTime: 0 });
  seqTrack.addVideo('memory:blue', { inPoint: 0, outPoint: 1, startTime: 1 });
  const sourceFrames = {
    'memory:red': solid(1, 1, [255, 0, 0, 255]),
    'memory:blue': solid(1, 1, [0, 0, 255, 255]),
  };
  const seqRenderer = new PreviewRenderer(seq.timeline, {
    backend: new CpuBackend({
      width: 1,
      height: 1,
      readFrame: (clip) => sourceFrames[clip.asset.path],
    }),
  });
  const frameA = await seqRenderer.render(0.25);
  check(pixels(frameA) === '255,0,0,255', 'first frame is the red clip');
  const frameB = await seqRenderer.render(0.75);
  const frameC = await seqRenderer.render(1.25);
  check(pixels(frameC) === '0,0,255,255', 'later frame is the blue clip');
  check(pixels(frameA) === '255,0,0,255' && pixels(frameB) === '255,0,0,255',
    'previously returned frames are not mutated by later renders');
  check(frameA.data !== frameB.data && frameA.data !== frameC.data, 'each frame owns fresh storage');
  check(pixels(sourceFrames['memory:red']) === '255,0,0,255', 'source frames are never mutated');

  // getFrame() returns a composited frame of the same size.
  const thumb = await seqRenderer.getFrame(1.25);
  check(thumb.width === 1 && thumb.height === 1 && pixels(thumb) === '0,0,255,255',
    'getFrame returns the composited frame');

  // renderRange walks consecutive frames through [startTime, endTime).
  const seen = [];
  await seqRenderer.renderRange(0, 1, async (f, frameNumber) => {
    seen.push({ frameNumber, px: pixels(f) });
  });
  check(seen.length === 2, 'renderRange covers [start, end) at the timeline fps');
  check(seen[0].frameNumber === 0 && seen[1].frameNumber === 1, 'frame numbers advance consecutively');
  check(seen[0].px === '255,0,0,255' && seen[1].px === '255,0,0,255', 'consecutive frames composited');
});
