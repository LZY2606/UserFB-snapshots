import assert from 'node:assert/strict';
import { test } from 'node:test';
import VideoContext from '../src/videocontext.js';
import VideoNode from '../src/SourceNodes/videonode.js';
import AudioNode from '../src/SourceNodes/audionode.js';

globalThis.window = {};
globalThis.HTMLMediaElement = { HAVE_FUTURE_DATA: 3 };

// Minimal WebGL stand-in, taken from devcheck/baseline.mjs.
const makeGL = () =>
    new Proxy(
        { canvas: { width: 320, height: 180 } },
        {
            get(o, k) {
                if (k in o) return o[k];
                if (/^[A-Z_0-9]+$/.test(k)) return 1;
                if (k === 'getParameter') return () => 8;
                if (k === 'getShaderParameter' || k === 'getProgramParameter') return () => true;
                if (k.startsWith('create')) return () => ({});
                return () => {};
            }
        }
    );

// Fake media element: only records currentTime/playbackRate/play/pause.
// All timeline<->media time conversion is done by the real library.
const makeMediaElement = (duration = 10) => ({
    duration,
    readyState: 4,
    seeking: false,
    ended: false,
    currentTime: 0,
    playbackRate: 1,
    volume: 1,
    playCount: 0,
    pauseCount: 0,
    pause() {
        this.pauseCount++;
    },
    play() {
        this.playCount++;
        return Promise.resolve();
    }
});

// Elements created by the library itself (url based nodes) need a few more
// DOM-ish methods, but still only record the same playback state.
const libraryElements = [];
globalThis.document = {
    createElement() {
        const el = makeMediaElement();
        el.attributes = {};
        el.setAttribute = (k, v) => {
            el.attributes[k] = v;
        };
        el.removeAttribute = k => {
            delete el.attributes[k];
        };
        el.load = () => {};
        libraryElements.push(el);
        return el;
    }
};

let contextCount = 0;
const makeContext = () =>
    new VideoContext({ id: `timeline-rate-${contextCount++}`, getContext: () => makeGL() }, null, {
        manualUpdate: true,
        useVideoElementCache: false
    });

// A 10s clip scheduled at timeline 10 with sourceOffset 1 and local rate 2.
const makeScheduledPair = ctx => {
    const ve = makeMediaElement();
    const ae = makeMediaElement();
    const v = ctx.video(ve, 1);
    const a = ctx.audio(ae, 1);
    for (const n of [v, a]) {
        n.startAt(10);
        n.playbackRate = 2;
    }
    return { v, a, ve, ae };
};

test('video and audio map timeline positions to the same source position', () => {
    const ctx = makeContext();
    const { v, a, ve, ae } = makeScheduledPair(ctx);
    for (const n of [v, a]) {
        n._seek(12);
        n._update(12);
    }
    // timeline 12 -> sourceOffset 1 + (12 - 10) * 2 = 5
    assert.equal(ve.currentTime, 5);
    assert.equal(ae.currentTime, 5);
    // elements actually got played
    assert.ok(ve.playCount > 0);
    assert.ok(ae.playCount > 0);
    // automatic stop time: 10 + (10 - 1) / 2 = 14.5
    assert.equal(v.stopTime, 14.5);
    assert.equal(a.stopTime, 14.5);
});

test('context playbackRate only scales wall-clock advance; element rate is the product', () => {
    const ctx = makeContext();
    const { v, a, ve, ae } = makeScheduledPair(ctx);
    for (const n of [v, a]) {
        n._seek(12);
        n._update(12);
    }
    ctx.playbackRate = 3;
    v._update(12);
    a._update(12);
    // both element types get global * local
    assert.equal(ve.playbackRate, 6);
    assert.equal(ae.playbackRate, 6);
    // the media position mapping is not rescaled by the context rate
    v._seek(14);
    a._seek(14);
    assert.equal(ve.currentTime, 9); // 1 + (14 - 10) * 2
    assert.equal(ae.currentTime, 9);
    // nor is the segment length on the timeline
    assert.equal(v.stopTime, 14.5);
    assert.equal(a.stopTime, 14.5);
});

test('context playbackRate reaches subclasses of VideoNode and AudioNode', () => {
    class CustomVideoNode extends VideoNode {}
    class CustomAudioNode extends AudioNode {}
    const ctx = makeContext();
    const ve = makeMediaElement();
    const ae = makeMediaElement();
    const v = new CustomVideoNode(ve, ctx._gl, ctx._renderGraph, 0, 1, 1, 4, undefined, {});
    const a = new CustomAudioNode(ae, ctx._gl, ctx._renderGraph, 0, 1, 1, 4, undefined, {});
    ctx._sourceNodes.push(v, a);
    ctx.playbackRate = 3;
    for (const n of [v, a]) {
        n.startAt(10);
        n.playbackRate = 2;
        n._seek(12);
        n._update(12);
    }
    assert.equal(ve.playbackRate, 6);
    assert.equal(ae.playbackRate, 6);
    assert.equal(ve.currentTime, 5);
    assert.equal(ae.currentTime, 5);
});

test('image and canvas nodes are not affected by either rate layer', () => {
    const ctx = makeContext();
    const imageNode = ctx.image({});
    const canvasNode = ctx.canvas({});
    ctx.playbackRate = 3;
    assert.equal(imageNode._globalPlaybackRate, undefined);
    assert.equal(canvasNode._globalPlaybackRate, undefined);
    assert.equal(imageNode.playbackRate, undefined);
    assert.equal(canvasNode.playbackRate, undefined);
});

test('continuous local rate changes, backward seeks and replacement of later changes', () => {
    const ctx = makeContext();
    const ve = makeMediaElement();
    const v = ctx.video(ve, 1);
    v.startAt(10);
    v.playbackRate = 2;
    v._seek(12);
    v._update(12);
    assert.equal(ve.currentTime, 5);

    // change to 0.5 at timeline 12: the frame must not jump
    v.playbackRate = 0.5;
    assert.equal(ve.currentTime, 5);
    // stop time is re-derived from the remaining media and recorded rates
    assert.equal(v.stopTime, 22); // 12 + (10 - 5) / 0.5

    v._seek(14);
    assert.equal(ve.currentTime, 6); // 5 + (14 - 12) * 0.5

    // seeking back uses the previously recorded intervals
    v._seek(11);
    assert.equal(ve.currentTime, 3); // 1 + (11 - 10) * 2

    // changing the rate at 11 replaces the change recorded at 12
    v.playbackRate = 1;
    v._seek(14);
    assert.equal(ve.currentTime, 6); // 3 + (14 - 11) * 1
    assert.equal(v.stopTime, 18); // 11 + (10 - 3) / 1
});

test('rate set before scheduling is the initial rate for the whole segment', () => {
    const ctx = makeContext();
    const ve = makeMediaElement();
    const v = ctx.video(ve, 1);
    v.playbackRate = 4; // node not sequenced yet
    v.startAt(10);
    v._seek(11);
    v._update(11);
    assert.equal(ve.currentTime, 5); // 1 + (11 - 10) * 4
    assert.equal(v.stopTime, 12.25); // 10 + (10 - 1) / 4
});

test('rate can be adjusted while the node is paused', () => {
    const ctx = makeContext();
    const ve = makeMediaElement();
    const v = ctx.video(ve, 1);
    v.startAt(10);
    v.playbackRate = 2;
    v._seek(12);
    v._update(12);
    v._pause();
    v._update(12);
    assert.equal(ve.pauseCount, 1);
    // rate change while paused is recorded at the frozen timeline position
    v.playbackRate = 0.5;
    assert.equal(ve.currentTime, 5);
    assert.equal(v.stopTime, 22);
    // seeking while paused uses the new recorded interval
    v._seek(14);
    assert.equal(ve.currentTime, 6);
    assert.equal(v.stopTime, 22);
});

test('repeated rate sets at the same timeline position: last one wins', () => {
    const ctx = makeContext();
    const ve = makeMediaElement();
    const v = ctx.video(ve, 1);
    v.startAt(10);
    v._seek(12);
    v._update(12);
    v.playbackRate = 3;
    v.playbackRate = 0.25; // same timeline position, replaces the previous set
    v._seek(14);
    assert.equal(ve.currentTime, 3.5); // 3 + (14 - 12) * 0.25
});

test('explicit stopAt always wins over inferred stop times', () => {
    const ctx = makeContext();
    const ve = makeMediaElement();
    const v = ctx.video(ve, 1);
    v.startAt(10);
    v.playbackRate = 2;
    v.stopAt(15);
    // loading metadata must not override the explicit stop
    v._seek(12);
    v._update(12);
    assert.equal(v.stopTime, 15);
    // neither must later rate changes
    v.playbackRate = 0.5;
    assert.equal(v.stopTime, 15);
    // relative stop() is explicit too
    const v2 = ctx.video(makeMediaElement(), 1);
    v2.startAt(10);
    assert.ok(v2.stop(18)); // relative to the context timeline position (0)
    v2._seek(12);
    v2._update(12);
    assert.equal(v2.stopTime, 18);
});

test('preload before the segment start parks the element at sourceOffset', () => {
    const ctx = makeContext();
    const ve = makeMediaElement();
    const v = ctx.video(ve, 1);
    v.playbackRate = 2;
    v.startAt(10);
    v._update(7); // inside the 4s preload window
    assert.equal(ve.currentTime, 1);
});

test('invalid rates throw RangeError and preserve previous rate and mapping', () => {
    const ctx = makeContext();
    const ve = makeMediaElement();
    const v = ctx.video(ve, 1);
    v.startAt(10);
    v.playbackRate = 2;
    v._seek(12);
    v._update(12);

    for (const bad of [0, -1, NaN, Infinity, -Infinity, '2', undefined, null]) {
        assert.throws(() => {
            v.playbackRate = bad;
        }, RangeError);
        assert.throws(() => {
            ctx.playbackRate = bad;
        }, RangeError);
    }
    // previous rates retained
    assert.equal(v.playbackRate, 2);
    assert.equal(ctx.playbackRate, 1);
    // positioning records retained: mapping and stop time unchanged
    v._seek(14);
    assert.equal(ve.currentTime, 9);
    assert.equal(v.stopTime, 14.5);
});

test('library-created elements share the mapping across unload and reload', () => {
    const ctx = makeContext();
    const v = ctx.video('clip.mp4', 1);
    v.startAt(10);
    v.playbackRate = 2;
    v._seek(12);
    v._update(12);
    const first = v.element;
    assert.ok(first);
    assert.equal(first.currentTime, 5);

    v._unload();
    assert.equal(v.element, undefined);

    // reload: _triggerLoad must land on the same source position as _seek
    v._load();
    const second = v.element;
    assert.ok(second);
    assert.notEqual(second, first);
    assert.equal(second.currentTime, 5);

    // and the mapping keeps advancing consistently afterwards
    v._seek(13);
    assert.equal(second.currentTime, 7);
});

test('clearing the timeline drops recorded knots and rebuilds from current rate', () => {
    const ctx = makeContext();
    const ve = makeMediaElement();
    const v = ctx.video(ve, 1);
    v.startAt(10);
    v.playbackRate = 2;
    v._seek(12);
    v._update(12);
    v.playbackRate = 0.5;

    v.clearTimelineState();
    v.startAt(20);
    v._seek(22);
    v._update(22);
    // rebuilt from the node's current rate (0.5), old knots discarded
    assert.equal(ve.currentTime, 2); // 1 + (22 - 20) * 0.5
    assert.equal(v.stopTime, 38); // 20 + (10 - 1) / 0.5
});

test('looping media keeps the legacy 1:1 mapping', () => {
    const ctx = makeContext();
    const ve = makeMediaElement();
    const v = ctx.video(ve, 1, 4, { loop: true });
    v.startAt(10);
    v.playbackRate = 2;
    v._seek(12);
    v._update(12);
    // legacy path: sourceOffset + raw timeline delta
    assert.equal(ve.currentTime, 3);
});

// Only report success once the whole run has completed without failures.
process.on('exit', code => {
    if (code === 0) console.log('TIMELINE_RATE_OK');
});
