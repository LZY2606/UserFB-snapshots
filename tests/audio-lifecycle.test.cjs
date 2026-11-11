"use strict";

const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const test = require("node:test");
const assert = require("node:assert/strict");

const ROOT = path.join(__dirname, "..");

// A shared, deliberately simple event log. Both the fake AudioContext and the
// fake decoder append to it so tests can check ordering across components.
let events = [];
let nowTime = 10;
let timerQueue = [];

class FakeAudioContext {
	constructor() {
		this.currentTime = 10;
		this.destination = { kind: "destination" };
		this.gains = [];
		this.nodes = [];
		this.closed = false;
		this.closeCalls = 0;
		this._connections = 0;
		FakeAudioContext.instances.push(this);
	}

	createGain() {
		const gainNode = {
			gain: { value: 1 },
			connections: [],
			disconnectCalls: 0,
			connect(destination) {
				this.connections.push(destination);
			},
			disconnect() {
				this.disconnectCalls++;
				this.connections = [];
			}
		};
		this.gains.push(gainNode);
		return gainNode;
	}

	createBuffer(channels, length, sampleRate) {
		const channelData = [];
		for (let i = 0; i < channels; i++) {
			channelData.push(new Float32Array(length));
		}
		return {
			numberOfChannels: channels,
			length,
			sampleRate,
			duration: length / sampleRate,
			getChannelData(index) {
				return channelData[index];
			}
		};
	}

	createBufferSource() {
		const source = {
			buffer: null,
			onended: null,
			connectCalls: 0,
			startCalls: 0,
			stopCalls: 0,
			disconnectCalls: 0,
			started: false,
			stopped: false,
			disconnected: false,
			startedAt: null,
			PLAYING_STATE: "playing",
			FINISHED_STATE: "finished",
			playbackState: "playing",
			connect(destination) {
				this.connectCalls++;
				this.destination = destination;
			},
			start(at) {
				this.startCalls++;
				this.started = true;
				this.startedAt = at;
				events.push({ type: "start", node: this, time: at });
			},
			stop() {
				this.stopCalls++;
				this.stopped = true;
				events.push({ type: "stop", node: this });
			},
			disconnect() {
				this.disconnectCalls++;
				this.disconnected = true;
				events.push({ type: "disconnect", node: this });
			},
			// Let a test decide when a source ends "naturally".
			fireEnded() {
				const handler = this.onended;
				this.onended = null;
				if (handler) {
					handler.call(this);
				}
			},
			// Capture the handler for a delayed onended event.
			takeOnEnded() {
				const handler = this.onended;
				this.onended = null;
				return handler;
			}
		};
		this.nodes.push(source);
		return source;
	}

	close() {
		this.closed = true;
		this.closeCalls++;
	}
}
FakeAudioContext.instances = [];

// Provides finite PCM frames and bookkeeping, but no lifecycle behaviour: it
// always calls destination.play() and trusts the output to handle scheduling.
class FakeAudioDecoder {
	constructor(options) {
		options = options || {};
		this.destination = null;
		this.canPlay = false;
		this.startTime = 0;
		this.decodedTime = 0;
		this.frameIndex = 0;
		this.sampleRate = options.sampleRate || 100;
		this.samplesPerFrame = options.samplesPerFrame || 50;
		this.totalFrames = options.totalFrames === undefined
			? Infinity
			: options.totalFrames;

		Object.defineProperty(this, "currentTime", {
			get: () => this.decodedTime -
				(this.destination ? this.destination.enqueuedTime : 0)
		});
	}

	connect(destination) {
		this.destination = destination;
		this.canPlay = true;
	}

	destroy() {}

	addFrames(count) {
		this.totalFrames += count;
	}

	decode() {
		if (this.frameIndex >= this.totalFrames) {
			return false;
		}

		const left = new Float32Array(this.samplesPerFrame);
		const right = new Float32Array(this.samplesPerFrame);
		left.fill(0.25);
		right.fill(-0.25);
		this.destination.play(this.sampleRate, left, right);

		this.decodedTime += this.samplesPerFrame / this.sampleRate;
		this.frameIndex++;
		return true;
	}

	seek(time) {
		events.push({ type: "decoder-seek", time });
		this.frameIndex = Math.max(0,
			Math.round(time * this.sampleRate / this.samplesPerFrame));
		this.decodedTime = time;
	}
}

class FakeSource {
	constructor(url, options) {
		this.streaming = !!options.streaming;
		this.established = true;
		this.completed = false;
		this.progress = 1;
	}

	connect(demuxer) {
		this.demuxer = demuxer;
	}

	start() {
		this.startCalled = true;
	}

	resume() {}

	destroy() {
		this.destroyed = true;
	}
}

class FakeDemuxer {
	constructor() {
		this.currentTime = 0;
		this.startTime = 0;
		this.streams = {};
	}

	connect(stream, destination) {
		this.streams[stream] = destination;
	}

	write() {}
}
FakeDemuxer.STREAM = { VIDEO_1: 0, AUDIO_1: 1 };

function createEnvironment(options) {
	options = options || {};
	events = [];
	timerQueue = [];
	nowTime = 10;
	FakeAudioContext.instances.length = 0;

	const sandbox = {
		console,
		Float32Array,
		Uint8Array,
		Set,
		Array,
		Object,
		Math,
		window: {
			AudioContext: FakeAudioContext,
			performance: {
				now: () => nowTime * 1000
			}
		},
		navigator: {
			userAgent: options.userAgent || "desktop"
		},
		document: {
			readyState: "loading",
			visibilityState: "visible",
			addEventListener() {},
			querySelectorAll() {
				return [];
			}
		},
		setTimeout(fn, ms) {
			const timer = { fn, ms };
			timerQueue.push(timer);
			return timerQueue.length;
		},
		clearTimeout() {},
		requestAnimationFrame() {
			return 1;
		},
		cancelAnimationFrame() {}
	};
	vm.createContext(sandbox);

	for (const file of ["src/jsmpeg.js", "src/webaudio.js", "src/player.js"]) {
		const source = fs.readFileSync(path.join(ROOT, file), "utf8");
		vm.runInContext(source, sandbox, { filename: file });
	}

	sandbox.JSMpeg.Now = () => nowTime;
	sandbox.JSMpeg.Demuxer.TS = FakeDemuxer;
	sandbox.JSMpeg.Decoder.MP2Audio = FakeAudioDecoder;
	sandbox.JSMpeg.WASMModule = { IsSupported: () => false };

	return sandbox;
}

function advance(context, seconds) {
	context.currentTime += seconds;
	nowTime += seconds;
}

function assertClose(actual, expected, epsilon) {
	epsilon = epsilon || 1e-9;
	assert.ok(
		Math.abs(actual - expected) <= epsilon,
		`${actual} is not within ${epsilon} of ${expected}`
	);
}

function createPlayer(env, options) {
	const playerOptions = Object.assign({
		source: FakeSource,
		video: false,
		audio: true,
		disableWebAssembly: true,
		pauseWhenHidden: false,
		autoplay: false
	}, options || {});

	return new env.JSMpeg.Player("fake://stream", playerOptions);
}

test("audio queue lifecycle", async (t) => {
	await t.test("continuous playback keeps the original schedule and copies PCM", () => {
		const env = createEnvironment();
		const WebAudioOut = env.JSMpeg.AudioOutput.WebAudio;
		const out = new WebAudioOut({});
		const context = out.context;

		const left = new Float32Array(100);
		const right = new Float32Array(100);
		left.fill(0.25);
		right.fill(-0.25);

		out.play(100, left, right);

		assert.equal(context.nodes.length, 1);
		const first = context.nodes[0];
		assert.equal(first.startedAt, 10);
		assert.equal(out.enqueuedTime, 1);

		// The two channels are copied independently into their own buffer.
		const leftData = first.buffer.getChannelData(0);
		const rightData = first.buffer.getChannelData(1);
		assert.notEqual(leftData, rightData);
		assert.deepEqual(Array.from(leftData), Array.from(left));
		assert.deepEqual(Array.from(rightData), Array.from(right));

		advance(context, 1);
		out.play(100, new Float32Array(50), new Float32Array(50));

		assert.equal(context.nodes.length, 2);
		assert.equal(context.nodes[1].startedAt, 11);
		assert.equal(out.enqueuedTime, 0.5);

		// Nodes that end naturally are released, so the bookkeeping does not
		// grow by one entry for every frame ever played.
		first.fireEnded();
		assert.equal(out.activeSources.size, 1);
		context.nodes[1].fireEnded();
		assert.equal(out.activeSources.size, 0);

		// Explicit stops release everything as well.
		out.play(100, new Float32Array(10), new Float32Array(10));
		assert.equal(out.activeSources.size, 1);
		out.stop();
		assert.equal(out.activeSources.size, 0);
		assert.equal(out.enqueuedTime, 0);
		out.destroy();
	});

	await t.test("pause stores the audible position before clearing the queue", () => {
		const env = createEnvironment();
		const player = createPlayer(env, {
			sampleRate: 100,
			samplesPerFrame: 50,
			totalFrames: 4
		});
		const context = player.audioOut.context;

		player.play();
		player.audio.decode();
		player.audio.decode();
		assert.equal(player.audio.decodedTime, 1);
		assert.equal(player.audioOut.enqueuedTime, 1);

		// Half a second has actually been heard; the other half is enqueued.
		advance(context, 0.5);
		assert.equal(player.currentTime, 0.5);

		const oldSources = context.nodes.slice();
		player.pause();

		assert.equal(player.paused, true);
		for (const source of oldSources) {
			assert.ok(source.stopCalls >= 1, "old source must be stopped");
			assert.equal(source.disconnected, true);
		}
		assert.equal(player.audioOut.enqueuedTime, 0);

		// Crucially, pause must seek to 0.5, not to the 1.0 that had been
		// decoded ahead.
		assert.equal(player.audio.decodedTime, 0.5);
		assert.equal(player.currentTime, 0.5);
		assert.equal(
			player.currentTime,
			player.audio.decodedTime - player.audioOut.enqueuedTime -
				player.audio.startTime
		);

		// The old audio is removed before the decoder is repositioned.
		const stopIndex = events.findIndex((event) => event.type === "stop");
		const seekIndex = events.findIndex(
			(event) => event.type === "decoder-seek");
		assert.ok(stopIndex >= 0 && seekIndex >= 0);
		assert.ok(stopIndex < seekIndex);

		// Pausing an already paused player does not move the position again.
		const eventCount = events.length;
		player.pause();
		assert.equal(events.length, eventCount);
		assert.equal(player.currentTime, 0.5);

		// Resuming continues from the saved position and schedules the new
		// audio at the context's current playable time, not after the old
		// queue's end.
		player.play();
		const nodeCountBefore = context.nodes.length;
		player.audio.decode();
		assert.equal(context.nodes.length, nodeCountBefore + 1);
		const resumed = context.nodes[context.nodes.length - 1];
		assert.equal(resumed.startedAt, 10.5);
		assert.equal(player.audioOut.enqueuedTime, 0.5);
		assert.equal(player.currentTime, 0.5);

		// Player.stop() repositions to zero and the audio time relationship
		// stays consistent for the next scheduled audio.
		player.stop();
		assert.equal(player.audioOut.enqueuedTime, 0);
		player.audio.decode();
		assert.equal(player.currentTime, 0);
		assert.equal(
			player.currentTime,
			player.audio.decodedTime - player.audioOut.enqueuedTime -
				player.audio.startTime
		);
	});

	await t.test("direct seek removes old audio before moving the decoder", () => {
		const env = createEnvironment();
		const player = createPlayer(env, {
			sampleRate: 100,
			samplesPerFrame: 50,
			totalFrames: 4
		});
		const context = player.audioOut.context;

		player.play();
		player.audio.decode();
		player.audio.decode();
		advance(context, 0.5);

		const oldSources = context.nodes.slice();
		events = [];
		player.seek(0.3);

		for (const source of oldSources) {
			assert.ok(source.stopCalls >= 1, "old source must be stopped");
			assert.equal(source.disconnected, true);
		}
		assert.equal(player.audioOut.enqueuedTime, 0);
		assert.equal(player.audio.decodedTime, 0.3);

		const stopIndex = events.findIndex((event) => event.type === "stop");
		const seekIndex = events.findIndex(
			(event) => event.type === "decoder-seek");
		assert.ok(stopIndex >= 0 && seekIndex >= 0);
		assert.ok(stopIndex < seekIndex,
			"audio must be stopped before the decoder seek");

		// Newly decoded audio starts at the context position after the seek.
		player.audio.decode();
		const fresh = context.nodes[context.nodes.length - 1];
		assert.equal(fresh.startedAt, 10.5);
		assert.equal(player.audioOut.enqueuedTime, 0.5);
		assertClose(player.currentTime, 0.3);
	});

	await t.test("streaming catch-up discards late audio and resumes silently", () => {
		const env = createEnvironment();
		const player = createPlayer(env, {
			streaming: true,
			maxAudioLag: 0.1,
			sampleRate: 100,
			samplesPerFrame: 50,
			totalFrames: 8
		});
		const context = player.audioOut.context;

		player.play();
		player.audio.decode();
		player.audio.decode();
		assert.equal(player.audioOut.enqueuedTime, 1);

		const oldSources = context.nodes.slice();
		const nodesBeforeCatchUp = context.nodes.length;
		player.updateForStreaming();

		for (const source of oldSources) {
			assert.ok(source.stopCalls >= 1, "late source must be stopped");
			assert.equal(source.disconnected, true);
		}
		assert.equal(context.nodes.length, nodesBeforeCatchUp,
			"no audio is scheduled while catching up silently");
		assert.equal(player.audioOut.enqueuedTime, 0);
		assert.equal(player.audioOut.enabled, true);
		assert.equal(player.audioOut.activeSources.size, 0);

		// After catch-up, new audio starts at the current playable position
		// instead of being appended behind the discarded queue.
		player.audio.addFrames(1);
		player.audio.decode();
		const fresh = context.nodes[context.nodes.length - 1];
		assert.equal(fresh.startedAt, 10);
		assert.equal(player.audioOut.enqueuedTime, 0.5);
		assert.equal(
			player.currentTime,
			player.audio.decodedTime - player.audioOut.enqueuedTime -
				player.audio.startTime
		);
	});

	await t.test("outputs sharing a context clean up independently", () => {
		const env = createEnvironment();
		const WebAudioOut = env.JSMpeg.AudioOutput.WebAudio;
		const first = new WebAudioOut({});
		const second = new WebAudioOut({});
		const context = first.context;

		assert.equal(second.context, context);
		assert.equal(context._connections, 2);
		assert.notEqual(first.gain, second.gain);

		first.play(100, new Float32Array(100), new Float32Array(100));
		second.play(100, new Float32Array(100), new Float32Array(100));
		const firstNode = context.nodes[0];
		const secondNode = context.nodes[1];

		first.stop();

		assert.ok(firstNode.stopCalls >= 1);
		assert.equal(firstNode.disconnected, true);
		assert.equal(secondNode.stopCalls, 0,
			"the other output's source must not be stopped");
		assert.equal(secondNode.disconnected, false);
		assert.equal(first.enqueuedTime, 0);
		assert.equal(second.enqueuedTime, 1);
		assert.equal(second.gain.disconnectCalls, 0,
			"the other output's gain must not be disconnected");
		assert.equal(context.closed, false);

		first.destroy();
		assert.equal(first.gain.disconnectCalls, 1);
		assert.equal(second.gain.disconnectCalls, 0);
		assert.equal(context.closed, false);
		assert.equal(context._connections, 1);
		assert.equal(WebAudioOut.CachedContext, context);

		second.destroy();
		assert.equal(context.closed, true);
		assert.equal(context.closeCalls, 1);
		assert.equal(context._connections, 0);
		assert.equal(WebAudioOut.CachedContext, null);

		// Destroy is idempotent.
		second.destroy();
		assert.equal(context.closeCalls, 1);
		assert.equal(context._connections, 0);

		// A new output gets a fresh, usable context.
		const third = new WebAudioOut({});
		assert.notEqual(third.context, context);
		assert.equal(third.context.closed, false);
		third.play(100, new Float32Array(10), new Float32Array(10));
		assert.equal(third.context.nodes.length, 1);
		third.destroy();
		assert.equal(third.context.closeCalls, 1);
		assert.equal(WebAudioOut.CachedContext, null);
	});

	await t.test("a late onended cannot disturb newer audio or another output", () => {
		const env = createEnvironment();
		const WebAudioOut = env.JSMpeg.AudioOutput.WebAudio;
		const first = new WebAudioOut({});
		const second = new WebAudioOut({});
		const context = first.context;

		first.play(100, new Float32Array(100), new Float32Array(100));
		const oldNode = context.nodes[0];
		const lateHandler = oldNode.takeOnEnded();
		assert.equal(typeof lateHandler, "function");

		first.stop();
		first.play(100, new Float32Array(10), new Float32Array(10));
		const newNode = context.nodes[1];
		second.play(100, new Float32Array(100), new Float32Array(100));
		const otherNode = context.nodes[2];

		const firstEnqueued = first.enqueuedTime;
		assertClose(firstEnqueued, 0.1);

		// Deliver the old node's ended event much later.
		lateHandler.call(oldNode);

		assert.equal(newNode.stopCalls, 0,
			"a late callback must not stop newer audio");
		assert.equal(newNode.disconnected, false);
		assert.equal(otherNode.stopCalls, 0,
			"a late callback must not affect another output");
		assert.equal(first.enqueuedTime, firstEnqueued,
			"a late callback must not clear the new generation's timing");
		assert.equal(first.activeSources.size, 1);
		assert.equal(second.activeSources.size, 1);

		first.stop();
		assert.ok(newNode.stopCalls >= 1);
		assert.equal(otherNode.stopCalls, 0);
		second.stop();
		assert.ok(otherNode.stopCalls >= 1);
	});

	await t.test("locked devices keep silent timing but still clear on stop/destroy", () => {
		const env = createEnvironment({ userAgent: "iPhone" });
		const WebAudioOut = env.JSMpeg.AudioOutput.WebAudio;
		const out = new WebAudioOut({});
		const context = out.context;

		assert.equal(out.unlocked, false);
		out.play(100, new Float32Array(100), new Float32Array(100));
		assert.equal(context.nodes.length, 0);
		assert.equal(out.enqueuedTime, 1);

		out.stop();
		assert.equal(out.enqueuedTime, 0);

		out.play(100, new Float32Array(50), new Float32Array(50));
		assert.equal(out.enqueuedTime, 0.5);

		let callbackCalls = 0;
		out.unlock(() => {
			callbackCalls++;
		});
		assert.equal(context.nodes.length, 1);
		const probe = context.nodes[0];
		assert.equal(timerQueue.length, 1);

		out.destroy();
		assert.equal(out.destroyed, true);
		assert.ok(probe.stopCalls >= 1, "the unlock probe must be cleaned up");
		assert.equal(probe.disconnected, true);
		assert.equal(out.enqueuedTime, 0);
		assert.equal(out.unlockCallback, null);
		assert.equal(context.closed, true);
		assert.equal(context._connections, 0);

		// A poll that fires after destroy must not revive or notify.
		probe.playbackState = probe.FINISHED_STATE;
		timerQueue[0].fn();
		assert.equal(callbackCalls, 0);
		assert.equal(out.unlocked, false);
		assert.equal(context.nodes.length, 1);

		out.destroy();
		assert.equal(context.closeCalls, 1);
		assert.equal(context._connections, 0);
	});
});
