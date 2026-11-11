const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const test = require('node:test');
const loadSource = require('./helpers/load-source.cjs');

const chrome = 'Mozilla/5.0 (Windows NT 10.0) AppleWebKit/537.36 Chrome/120.0.0.0 Safari/537.36';
const safari = 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 Version/17.0 Safari/605.1.15';

const AVC = 'avc1.42E01E';
const HEVC = 'hvc1.1.6.L93.B0';
const AV1 = 'av01.0.04M.08';
const AAC = 'mp4a.40.2';
const AC3 = 'ac-3';

const AVC_MIME = `video/mp4;codecs=${AVC}`;
const HEVC_MIME = `video/mp4;codecs=${HEVC}`;
const AV1_MIME = `video/mp4;codecs=${AV1}`;
const AAC_MIME = `audio/mp4;codecs=${AAC}`;
const AC3_MIME = `audio/mp4;codecs=${AC3}`;

function initSegment(codec, byte, type = 'video') {
    return { type, container: `${type}/mp4`, codec, data: new Uint8Array([byte]) };
}

function mediaSegment(byte, type = 'video') {
    return { type, data: new Uint8Array([byte]) };
}

// Test doubles. The SourceBuffer strictly refuses changeType()/appendBuffer() while
// updating (violations are recorded and asserted empty), supports manually ending an
// update via finish(), and can be armed to fail the next appendBuffer() once with a
// QuotaExceededError (code 22).
function createController(options = {}) {
    const ops = [];
    const violations = [];
    const codecChanges = [];
    const errors = [];
    const bufferFullEvents = [];

    const sbOptions = options.sourceBuffer || {};

    class FakeSourceBuffer {
        constructor(mimeType) {
            this.mimeType = mimeType;
            this.type = mimeType.split('/')[0];
            this.updating = false;
            this.timestampOffset = 0;
            this.listeners = {};
            this.removed = [];
            this.bufferedRanges = options.bufferedRanges || [];
            this.quotaPending = false;
            this.rejectChangeType = !!sbOptions.rejectChangeType;
            if (sbOptions.noChangeType) {
                this.changeType = undefined;
            }
        }

        addEventListener(name, listener) {
            this.listeners[name] = listener;
        }

        removeEventListener() {}

        appendBuffer(data) {
            if (this.updating) {
                violations.push(['append-while-updating', this.type]);
            }
            if (this.quotaPending) {
                this.quotaPending = false;
                const error = new Error('quota exceeded');
                error.code = 22;  // QuotaExceededError
                throw error;
            }
            ops.push(['append', this.type, data[0]]);
            this.updating = true;
        }

        changeType(mimeType) {
            if (this.updating) {
                violations.push(['changeType-while-updating', this.type]);
            }
            if (this.rejectChangeType) {
                const error = new Error('mime type is not supported');
                error.name = 'NotSupportedError';
                throw error;
            }
            ops.push(['changeType', this.type, this.mimeType, mimeType]);
            this.mimeType = mimeType;
        }

        abort() {}

        remove(start, end) {
            this.removed.push([start, end]);
            this.updating = true;
        }

        get buffered() {
            const ranges = this.bufferedRanges;
            return {
                length: ranges.length,
                start: (index) => ranges[index][0],
                end: (index) => ranges[index][1]
            };
        }

        finish() {
            this.updating = false;
            const listener = this.listeners.updateend;
            if (listener) {
                listener();
            }
        }
    }

    class FakeMediaSource {
        constructor() {
            this.readyState = options.readyState || 'open';
            if (options.streaming !== undefined) {
                this.streaming = options.streaming;
            }
            this.handle = {};
            this.buffers = [];
            this.listeners = {};
        }

        addEventListener(name, listener) {
            this.listeners[name] = listener;
        }

        removeEventListener(name) {
            delete this.listeners[name];
        }

        addSourceBuffer(mimeType) {
            ops.push(['create', mimeType]);
            const sourceBuffer = new FakeSourceBuffer(mimeType);
            this.buffers.push(sourceBuffer);
            return sourceBuffer;
        }

        endOfStream() {
            this.readyState = 'ended';
        }

        open() {
            this.readyState = 'open';
            const listener = this.listeners.sourceopen;
            if (listener) {
                listener();
            }
        }

        startStreaming() {
            this.streaming = true;
            const listener = this.listeners.startstreaming;
            if (listener) {
                listener();
            }
        }
    }

    const globals = {
        navigator: { userAgent: options.userAgent || chrome },
        self: {}
    };
    if (options.managed) {
        globals.self.ManagedMediaSource = FakeMediaSource;
    } else {
        globals.self.MediaSource = FakeMediaSource;
    }

    const load = loadSource(globals);
    const MSEController = load('core/mse-controller.js').default;
    const MSEEvents = load('core/mse-events.ts').default;

    const controller = new MSEController(options.config || {});
    controller.on(MSEEvents.CODEC_CHANGE, (info) => {
        // Spread into this realm: the payload is created inside the module's vm context
        codecChanges.push({ info: { ...info }, logIndex: ops.length });
    });
    controller.on(MSEEvents.ERROR, (info) => errors.push(info));
    controller.on(MSEEvents.BUFFER_FULL, () => bufferFullEvents.push(true));
    controller.initialize({ getCurrentTime: () => 0, getReadyState: () => 1 });

    const mediaSource = controller.getObject();

    return {
        controller,
        mediaSource,
        ops,
        violations,
        codecChanges,
        errors,
        bufferFullEvents,
        video: () => mediaSource.buffers.find((buffer) => buffer.type === 'video'),
        audio: () => mediaSource.buffers.find((buffer) => buffer.type === 'audio')
    };
}

// Drives the fake SourceBuffer through its update cycles until no append is in flight
function settle(sourceBuffer) {
    let guard = 0;
    while (sourceBuffer.updating && guard++ < 50) {
        sourceBuffer.finish();
    }
}

test('MSE codec transition', (t) => {
    t.test('switches codecs on one track with ordered changeType calls (A/B/A)', () => {
        const h = createController();
        const c = h.controller;

        c.appendInitSegment(initSegment(AVC, 1));
        c.appendMediaSegment(mediaSegment(2));
        settle(h.video());

        c.appendInitSegment(initSegment(HEVC, 3));
        c.appendMediaSegment(mediaSegment(4));
        settle(h.video());

        c.appendInitSegment(initSegment(AVC, 5));
        c.appendMediaSegment(mediaSegment(6));
        settle(h.video());

        // A repeated init segment of the effective type must not trigger changeType
        c.appendInitSegment(initSegment(AVC, 7));
        settle(h.video());

        assert.deepEqual(h.ops, [
            ['create', AVC_MIME],
            ['append', 'video', 1],
            ['append', 'video', 2],
            ['changeType', 'video', AVC_MIME, HEVC_MIME],
            ['append', 'video', 3],
            ['append', 'video', 4],
            ['changeType', 'video', HEVC_MIME, AVC_MIME],
            ['append', 'video', 5],
            ['append', 'video', 6],
            ['append', 'video', 7]
        ]);
        assert.deepEqual(h.codecChanges.map((entry) => entry.info), [
            { type: 'video', oldMimeType: AVC_MIME, newMimeType: HEVC_MIME },
            { type: 'video', oldMimeType: HEVC_MIME, newMimeType: AVC_MIME }
        ]);
        // Each CODEC_CHANGE event fired right before the new init segment append
        assert.deepEqual(h.ops[h.codecChanges[0].logIndex], ['append', 'video', 3]);
        assert.deepEqual(h.ops[h.codecChanges[1].logIndex], ['append', 'video', 5]);
        assert.equal(h.errors.length, 0);
        assert.deepEqual(h.violations, []);
        assert.equal(c._mimeTypes.video, AVC_MIME);
    });

    t.test('replays segments accumulated before the first sourceopen in order', () => {
        const h = createController({ readyState: 'closed' });
        const c = h.controller;

        c.appendInitSegment(initSegment(AVC, 1));
        c.appendMediaSegment(mediaSegment(2));
        c.appendInitSegment(initSegment(HEVC, 3));
        c.appendMediaSegment(mediaSegment(4));

        // Nothing may happen before the MediaSource opens, and no type is effective yet
        assert.deepEqual(h.ops, []);
        assert.equal(c._mimeTypes.video, null);

        h.mediaSource.open();
        settle(h.video());

        // The interleaved codec switch in the middle of the queue is not collapsed away
        assert.deepEqual(h.ops, [
            ['create', AVC_MIME],
            ['append', 'video', 1],
            ['append', 'video', 2],
            ['changeType', 'video', AVC_MIME, HEVC_MIME],
            ['append', 'video', 3],
            ['append', 'video', 4]
        ]);
        assert.deepEqual(h.codecChanges.map((entry) => entry.info), [
            { type: 'video', oldMimeType: AVC_MIME, newMimeType: HEVC_MIME }
        ]);
        assert.equal(h.errors.length, 0);
        assert.deepEqual(h.violations, []);
    });

    t.test('keeps an independent audio track appending while video waits', () => {
        const h = createController();
        const c = h.controller;

        c.appendInitSegment(initSegment(AVC, 1, 'video'));
        c.appendInitSegment(initSegment(AAC, 2, 'audio'));
        c.appendMediaSegment(mediaSegment(3, 'video'));
        c.appendMediaSegment(mediaSegment(4, 'audio'));

        // The video codec switch is requested while the video SourceBuffer is updating
        c.appendInitSegment(initSegment(HEVC, 5, 'video'));
        assert.deepEqual(h.ops.filter((op) => op[0] === 'changeType'), []);
        // The requested-but-not-effective type must not become the current type
        assert.equal(c._mimeTypes.video, AVC_MIME);

        // Audio finishes its pending update and keeps appending independently
        h.audio().finish();
        h.audio().finish();
        c.appendMediaSegment(mediaSegment(6, 'audio'));
        assert.deepEqual(h.ops.filter((op) => op[0] === 'changeType'), []);
        assert.equal(c._mimeTypes.video, AVC_MIME);

        // Now the video track drains: switch first, then the queued media
        settle(h.video());

        // An audio codec switch is handled independently on its own track
        c.appendInitSegment(initSegment(AC3, 7, 'audio'));
        settle(h.audio());

        assert.deepEqual(h.ops, [
            ['create', AVC_MIME],
            ['append', 'video', 1],
            ['create', AAC_MIME],
            ['append', 'audio', 2],
            ['append', 'audio', 4],
            ['append', 'audio', 6],
            ['append', 'video', 3],
            ['changeType', 'video', AVC_MIME, HEVC_MIME],
            ['append', 'video', 5],
            ['changeType', 'audio', AAC_MIME, AC3_MIME],
            ['append', 'audio', 7]
        ]);
        assert.deepEqual(h.codecChanges.map((entry) => entry.info), [
            { type: 'video', oldMimeType: AVC_MIME, newMimeType: HEVC_MIME },
            { type: 'audio', oldMimeType: AAC_MIME, newMimeType: AC3_MIME }
        ]);
        assert.equal(h.errors.length, 0);
        assert.deepEqual(h.violations, []);
    });

    t.test('retries quota-failed appends without resubmitting codec switches', () => {
        const h = createController();
        const c = h.controller;

        c.appendInitSegment(initSegment(AVC, 1));
        settle(h.video());

        c.appendInitSegment(initSegment(HEVC, 2));
        c.appendMediaSegment(mediaSegment(3));

        // The next appendBuffer fails once with QuotaExceededError
        h.video().quotaPending = true;
        h.video().finish();
        assert.equal(h.bufferFullEvents.length, 1);
        assert.deepEqual(h.ops.filter((op) => op[0] === 'append'), [
            ['append', 'video', 1],
            ['append', 'video', 2]
        ]);

        // A further codec switch must not overtake the still-pending media segment
        c.appendInitSegment(initSegment(AV1, 4));
        c.appendMediaSegment(mediaSegment(5));

        settle(h.video());

        assert.deepEqual(h.ops, [
            ['create', AVC_MIME],
            ['append', 'video', 1],
            ['changeType', 'video', AVC_MIME, HEVC_MIME],
            ['append', 'video', 2],
            ['append', 'video', 3],
            ['changeType', 'video', HEVC_MIME, AV1_MIME],
            ['append', 'video', 4],
            ['append', 'video', 5]
        ]);
        // The completed switch was submitted exactly once; the retried media was not lost
        assert.deepEqual(h.codecChanges.map((entry) => entry.info), [
            { type: 'video', oldMimeType: AVC_MIME, newMimeType: HEVC_MIME },
            { type: 'video', oldMimeType: HEVC_MIME, newMimeType: AV1_MIME }
        ]);
        assert.equal(h.errors.length, 0);
        assert.deepEqual(h.violations, []);
    });

    t.test('reports unsupported codec transitions and parks only the affected track', () => {
        const h = createController({ sourceBuffer: { rejectChangeType: true } });
        const c = h.controller;

        c.appendInitSegment(initSegment(AVC, 1, 'video'));
        c.appendMediaSegment(mediaSegment(2, 'video'));
        c.appendInitSegment(initSegment(AAC, 3, 'audio'));
        settle(h.video());
        settle(h.audio());

        c.appendInitSegment(initSegment(HEVC, 4, 'video'));
        c.appendMediaSegment(mediaSegment(5, 'video'));   // parked track: dropped
        c.appendMediaSegment(mediaSegment(6, 'audio'));   // other track keeps working
        settle(h.audio());

        assert.equal(h.errors.length, 1);
        assert.equal(h.errors[0].reason, 'codec_change_unsupported');
        assert.equal(h.errors[0].type, 'video');
        assert.equal(h.errors[0].oldMimeType, AVC_MIME);
        assert.equal(h.errors[0].newMimeType, HEVC_MIME);
        assert.equal(h.codecChanges.length, 0);
        assert.deepEqual(h.ops, [
            ['create', AVC_MIME],
            ['append', 'video', 1],
            ['create', AAC_MIME],
            ['append', 'audio', 3],
            ['append', 'video', 2],
            ['append', 'audio', 6]
        ]);
        // The effective type is untouched and the other track was never flushed away
        assert.equal(c._mimeTypes.video, AVC_MIME);

        // While parked, further init segments for that track are not accepted
        c.appendInitSegment(initSegment(HEVC, 7, 'video'));
        assert.equal(h.errors.length, 1);

        // flush() clears the failure state; the resubmitted transition is attempted once
        c.flush();
        c.appendInitSegment(initSegment(HEVC, 8, 'video'));
        assert.equal(h.errors.length, 2);
        assert.equal(h.errors[1].reason, 'codec_change_unsupported');

        // No infinite retry: repeated update cycles and media do not re-attempt
        h.video().finish();
        h.video().finish();
        c.appendMediaSegment(mediaSegment(9, 'video'));
        assert.equal(h.errors.length, 2);
        assert.deepEqual(h.violations, []);
    });

    t.test('reports codec transitions on browsers without changeType support', () => {
        const h = createController({ sourceBuffer: { noChangeType: true } });
        const c = h.controller;

        c.appendInitSegment(initSegment(AVC, 1));
        c.appendMediaSegment(mediaSegment(2));
        settle(h.video());

        c.appendInitSegment(initSegment(HEVC, 3));
        c.appendMediaSegment(mediaSegment(4));

        assert.equal(h.errors.length, 1);
        assert.equal(h.errors[0].reason, 'codec_change_unsupported');
        assert.equal(h.errors[0].oldMimeType, AVC_MIME);
        assert.equal(h.errors[0].newMimeType, HEVC_MIME);
        assert.deepEqual(h.ops, [
            ['create', AVC_MIME],
            ['append', 'video', 1],
            ['append', 'video', 2]
        ]);
        assert.deepEqual(h.violations, []);
    });

    t.test('appends as before on browsers without changeType when the codec is unchanged', () => {
        const h = createController({ sourceBuffer: { noChangeType: true } });
        const c = h.controller;

        c.appendInitSegment(initSegment(AVC, 1));
        c.appendMediaSegment(mediaSegment(2));
        c.appendInitSegment(initSegment(AVC, 3));
        c.appendMediaSegment(mediaSegment(4));
        settle(h.video());

        assert.deepEqual(h.ops, [
            ['create', AVC_MIME],
            ['append', 'video', 1],
            ['append', 'video', 2],
            ['append', 'video', 3],
            ['append', 'video', 4]
        ]);
        assert.equal(h.errors.length, 0);
        assert.equal(h.codecChanges.length, 0);
        assert.deepEqual(h.violations, []);
    });

    t.test('flush discards a pending switch and keeps the effective codec', () => {
        const h = createController({ bufferedRanges: [[1, 2]] });
        const c = h.controller;

        c.appendInitSegment(initSegment(AVC, 1));
        c.appendMediaSegment(mediaSegment(2));
        // Received but still queued behind the media segment: never takes effect
        c.appendInitSegment(initSegment(HEVC, 3));

        c.flush();
        assert.equal(c._mimeTypes.video, AVC_MIME);
        assert.equal(h.codecChanges.length, 0);
        assert.deepEqual(h.ops, [
            ['create', AVC_MIME],
            ['append', 'video', 1]
        ]);

        settle(h.video());
        assert.deepEqual(h.video().removed, [[1, 2]]);

        // After the flush the track accepts a fresh transition normally
        c.appendInitSegment(initSegment(HEVC, 4));
        settle(h.video());
        assert.deepEqual(h.ops, [
            ['create', AVC_MIME],
            ['append', 'video', 1],
            ['changeType', 'video', AVC_MIME, HEVC_MIME],
            ['append', 'video', 4]
        ]);
        assert.deepEqual(h.codecChanges.map((entry) => entry.info), [
            { type: 'video', oldMimeType: AVC_MIME, newMimeType: HEVC_MIME }
        ]);
        assert.deepEqual(h.violations, []);
    });

    t.test('safari flush re-appends the effective init segment, not the pending one', () => {
        const h = createController({ userAgent: safari, bufferedRanges: [[1, 2]] });
        const c = h.controller;

        c.appendInitSegment(initSegment(AVC, 1));
        c.appendMediaSegment(mediaSegment(2));
        // HEVC init segment is received but never becomes effective before the flush
        c.appendInitSegment(initSegment(HEVC, 3));

        c.flush();
        settle(h.video());

        // Safari's init-segment re-append uses the effective AVC init segment
        assert.deepEqual(h.ops, [
            ['create', AVC_MIME],
            ['append', 'video', 1],
            ['append', 'video', 1]
        ]);
        assert.deepEqual(h.video().removed, [[1, 2]]);
        assert.equal(c._mimeTypes.video, AVC_MIME);
        assert.equal(h.codecChanges.length, 0);
        assert.deepEqual(h.violations, []);
    });

    t.test('defers appends while managed streaming is suspended and resumes in order', () => {
        const h = createController({ managed: true });
        const c = h.controller;

        c.appendInitSegment(initSegment(AVC, 1));
        settle(h.video());

        h.mediaSource.streaming = false;
        c.appendMediaSegment(mediaSegment(2));
        c.appendInitSegment(initSegment(HEVC, 3));
        c.appendMediaSegment(mediaSegment(4));
        assert.deepEqual(h.ops, [
            ['create', AVC_MIME],
            ['append', 'video', 1]
        ]);

        // Resuming streaming continues the same queued order, switch included
        h.mediaSource.startStreaming();
        settle(h.video());
        assert.deepEqual(h.ops, [
            ['create', AVC_MIME],
            ['append', 'video', 1],
            ['append', 'video', 2],
            ['changeType', 'video', AVC_MIME, HEVC_MIME],
            ['append', 'video', 3],
            ['append', 'video', 4]
        ]);
        assert.deepEqual(h.codecChanges.map((entry) => entry.info), [
            { type: 'video', oldMimeType: AVC_MIME, newMimeType: HEVC_MIME }
        ]);
        assert.deepEqual(h.violations, []);
    });

    t.test('forwards codec change events through the main thread engine', () => {
        const load = loadSource({
            navigator: { userAgent: chrome },
            self: { Promise, setInterval, clearInterval }
        });
        const PlayerEngineMainThread = load('player/player-engine-main-thread.ts').default;
        const PlayerEvents = load('player/player-events.ts').default;

        const engine = new PlayerEngineMainThread({ url: 'unused' }, {});
        const received = [];
        engine.on(PlayerEvents.CODEC_CHANGE, (info) => received.push(info));

        const payload = { type: 'video', oldMimeType: AVC_MIME, newMimeType: HEVC_MIME };
        engine._onMSECodecChange(payload);
        assert.deepEqual(received, [payload]);
        engine.destroy();
    });

    t.test('forwards codec change events through the worker engine', () => {
        const posted = [];
        const listeners = {};
        const self = {
            Promise,
            MediaSource: class {
                constructor() {
                    this.handle = {};
                    this.readyState = 'open';
                }
                addEventListener() {}
                removeEventListener() {}
            },
            addEventListener(name, listener) {
                listeners[name] = listener;
            },
            postMessage(message) {
                posted.push(message);
            }
        };
        const load = loadSource({ self, navigator: { userAgent: chrome } });

        // The worker creates its own MSEController from this same module instance;
        // capture the listener it registers for the real MSE codec_change event
        const MSEController = load('core/mse-controller.js').default;
        const MSEEvents = load('core/mse-events.ts').default;
        const codecChangeListeners = [];
        const originalOn = MSEController.prototype.on;
        MSEController.prototype.on = function (event, listener) {
            if (event === MSEEvents.CODEC_CHANGE) {
                codecChangeListeners.push(listener);
            }
            return originalOn.call(this, event, listener);
        };

        const PlayerEngineWorker = load('player/player-engine-worker.ts').default;
        PlayerEngineWorker(self);
        listeners.message({ data: { cmd: 'init', media_data_source: { url: 'unused' }, config: {} } });
        listeners.message({ data: { cmd: 'initialize_mse' } });

        assert.equal(codecChangeListeners.length, 1);
        assert.ok(posted.some((message) => message.msg === 'mse_init'));

        const payload = { type: 'video', oldMimeType: AVC_MIME, newMimeType: HEVC_MIME };
        codecChangeListeners[0](payload);

        const packet = posted.find((message) => message.msg === 'codec_change');
        // Spread into this realm: packets are created inside the module's vm context
        assert.deepEqual({ ...packet }, {
            msg: 'codec_change',
            type: 'video',
            oldMimeType: AVC_MIME,
            newMimeType: HEVC_MIME
        });
    });

    t.test('forwards codec change events through the dedicated thread engine', () => {
        const load = loadSource({ navigator: { userAgent: chrome }, self: {} });
        const PlayerEngineDedicatedThread = load('player/player-engine-dedicated-thread.ts').default;
        const PlayerEvents = load('player/player-events.ts').default;

        const engine = Object.create(PlayerEngineDedicatedThread.prototype);
        engine._emitter = new EventEmitter();
        const received = [];
        engine._emitter.on(PlayerEvents.CODEC_CHANGE, (info) => received.push(info));

        engine._onWorkerMessage({
            data: {
                msg: 'codec_change',
                type: 'video',
                oldMimeType: AVC_MIME,
                newMimeType: HEVC_MIME
            }
        });
        // Spread into this realm: the payload is created inside the module's vm context
        assert.deepEqual(received.map((info) => ({ ...info })), [
            { type: 'video', oldMimeType: AVC_MIME, newMimeType: HEVC_MIME }
        ]);
    });
});
