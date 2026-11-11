'use strict';

// Tests for the optional onVideoMotion(decoder, info) callback on the
// JavaScript MPEG-1 video decoder. Everything goes through the public
// MPEG1Video.write()/decode() entry points; the motion VLC decoder is never
// invoked by hand.
//
// Run with: node --test tests/motion-grid.test.cjs

const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');
const crypto = require('crypto');
const test = require('node:test');

const SRC = path.join(__dirname, '..', 'src');
const bitstream = require('./motion-bitstream.cjs');

const BASE_FILES = ['jsmpeg.js', 'buffer.js', 'decoder.js', 'mpeg1.js'];
const PIXEL_BASELINE = [
	'99c64e421c64', '5b4edfc57d16', '60757c9b8f64',
	'1318db64e1a4', '7c5a2b2492ac', 'f0833fad2e58'
];

function makeContext(webAssemblySupported) {
	const ctx = {
		window: {performance: {now: () => 0}},
		document: {
			readyState: 'loading',
			addEventListener() {},
			createElement() {
				return {getContext: () => ({})};
			}
		}
	};
	if (webAssemblySupported) {
		ctx.window.WebAssembly = {};
	}
	vm.createContext(ctx);
	return ctx;
}

function load(ctx, files) {
	for (const f of files) {
		vm.runInContext(fs.readFileSync(path.join(SRC, f), 'utf8'), ctx);
	}
	return ctx.JSMpeg;
}

function makeDecoder(JSMpeg, options) {
	return new JSMpeg.Decoder.MPEG1Video(Object.assign(
		{decodeFirstFrame: false}, options
	));
}

// Feeds a whole elementary stream and pumps decode() until it reports that
// no more pictures are available.
function decodeAll(decoder, data) {
	decoder.write(0, [data]);
	let guard = 0;
	while (decoder.decode()) {
		if (++guard > 1000) {
			throw new Error('decode() did not terminate');
		}
	}
}

function collect(JSMpeg, data, withRenderer) {
	const infos = [];
	const hashes = [];
	const decoder = makeDecoder(JSMpeg, {
		onVideoMotion: (d, info) => infos.push(info)
	});
	if (withRenderer) {
		decoder.connect({
			resize() {},
			render(y) {
				hashes.push(
					crypto.createHash('sha256').update(y).digest('hex').slice(0, 12)
				);
			}
		});
	}
	// Without a renderer the callback must still fire after each rebuilt
	// picture; pixel hashing is only meaningful on the renderer path.
	decodeAll(decoder, data);
	return {decoder, infos, hashes};
}

// Wrap copyMacroblock so every motion vector actually handed to motion
// compensation is recorded against the current macroblock address.
function instrumentCopyMacroblock(decoder) {
	const calls = [];
	const original = decoder.copyMacroblock;
	decoder.copyMacroblock = function(motionH, motionV) {
		calls.push({address: this.macroblockAddress, h: motionH, v: motionV});
		return original.apply(this, arguments);
	};
	return calls;
}

// Flatten an expected grid written as rows of [kind, dx, dy] triples.
function expectedVectors(rows) {
	const vectors = new Int16Array(rows.length * rows[0].length * 2);
	const kinds = new Uint8Array(rows.length * rows[0].length);
	rows.forEach((row, r) => {
		row.forEach((cell, c) => {
			const i = r * row.length + c;
			kinds[i] = cell[0];
			vectors[i * 2] = cell[1];
			vectors[i * 2 + 1] = cell[2];
		});
	});
	return {vectors, kinds};
}

function assertGrid(info, rows) {
	const expected = expectedVectors(rows);
	assert.deepEqual(Array.from(info.kinds), Array.from(expected.kinds), 'kinds');
	assert.deepEqual(Array.from(info.vectors), Array.from(expected.vectors), 'vectors');
}

// Independent cross-check: the values copyMacroblock was actually called with
// must match the published grid, and the call pattern must agree with kinds
// (intra blocks never call it; explicit inter and skipped blocks do, once).
function assertCopyMacroblockCrossCheck(info, calls) {
	const byAddress = new Map();
	for (const call of calls) {
		assert.ok(!byAddress.has(call.address), 'duplicate copyMacroblock for mb');
		byAddress.set(call.address, call);
	}
	for (let i = 0; i < info.kinds.length; i++) {
		const kind = info.kinds[i];
		const call = byAddress.get(i);
		if (kind === 1) {
			assert.equal(call, undefined, 'intra mb must not run motion compensation');
		}
		else if (kind === 2 || kind === 3) {
			assert.ok(call, `kind ${kind} mb must run motion compensation`);
			assert.equal(call.h, info.vectors[i * 2], 'copyMacroblock dx mismatch');
			assert.equal(call.v, info.vectors[i * 2 + 1], 'copyMacroblock dy mismatch');
		}
		else {
			assert.equal(kind, 0);
			assert.equal(call, undefined, 'uncovered mb must not be compensated');
		}
	}
}

const fixtureData = Buffer.from(
	fs.readFileSync(path.join(__dirname, 'fixtures', 'motion.m1v.b64'), 'utf8'),
	'base64'
);

// Explicit grids for the six-frame 48x32 fixture. All its motion vectors are
// zero; kinds still distinguish explicit inter (2) from skipped (3).
const FIXTURE_GRIDS = [
	{type: 'I', rows: [
		[[1,0,0],[1,0,0],[1,0,0]],
		[[1,0,0],[1,0,0],[1,0,0]]
	]},
	{type: 'P', rows: [
		[[2,0,0],[2,0,0],[2,0,0]],
		[[2,0,0],[3,0,0],[2,0,0]]
	]},
	{type: 'P', rows: [
		[[2,0,0],[2,0,0],[2,0,0]],
		[[2,0,0],[3,0,0],[2,0,0]]
	]},
	{type: 'P', rows: [
		[[2,0,0],[3,0,0],[2,0,0]],
		[[2,0,0],[3,0,0],[2,0,0]]
	]},
	{type: 'P', rows: [
		[[2,0,0],[3,0,0],[2,0,0]],
		[[2,0,0],[3,0,0],[2,0,0]]
	]},
	{type: 'P', rows: [
		[[2,0,0],[3,0,0],[2,0,0]],
		[[2,0,0],[3,0,0],[2,0,0]]
	]}
];

for (const withRenderer of [false, true]) {
	test(`fixture: full grid, metadata and pixels (renderer=${withRenderer})`, () => {
		const JSMpeg = load(makeContext(false), BASE_FILES);
		const {infos, hashes} = collect(JSMpeg, fixtureData, withRenderer);

		assert.equal(infos.length, 6);
		infos.forEach((info, i) => {
			assert.equal(info.frameIndex, i, 'continuous frameIndex');
			assert.equal(info.pictureType, FIXTURE_GRIDS[i].type);
			assert.equal(info.width, 48);
			assert.equal(info.height, 32);
			assert.equal(info.mbWidth, 3);
			assert.equal(info.mbHeight, 2);
			// Typed arrays are constructed inside the decoder's VM realm, so
			// check their type tag instead of instanceof across realms.
			assert.equal(Object.prototype.toString.call(info.vectors), '[object Int16Array]');
			assert.equal(Object.prototype.toString.call(info.kinds), '[object Uint8Array]');
			assertGrid(info, FIXTURE_GRIDS[i].rows);
		});
		if (withRenderer) {
			assert.deepEqual(hashes, PIXEL_BASELINE);
		}
		else {
			assert.equal(hashes.length, 0);
		}
	});
}

for (const withRenderer of [false, true]) {
	test(`fixture: snapshots stay independent and survive later decoding (renderer=${withRenderer})`, () => {
		const JSMpeg = load(makeContext(false), BASE_FILES);
		const infos = [];
		const hashes = [];
		const decoder = makeDecoder(JSMpeg, {
			onVideoMotion: (d, info) => infos.push(info)
		});
		if (withRenderer) {
			decoder.connect({
				resize() {},
				render(y) {
					hashes.push(
						crypto.createHash('sha256').update(y).digest('hex').slice(0, 12)
					);
				}
			});
		}
		decoder.write(0, [fixtureData]);

		// Decode the first two pictures, keep both snapshots, then mutate the
		// arrays handed out with the first one before decoding the rest.
		let guard = 0;
		while (infos.length < 2 && decoder.decode()) {
			if (++guard > 1000) throw new Error('decode loop');
		}
		assert.equal(infos.length, 2);
		const first = infos[0];
		const secondBefore = {
			vectors: Array.from(infos[1].vectors),
			kinds: Array.from(infos[1].kinds)
		};
		first.vectors.fill(1234);
		first.kinds.fill(9);

		while (decoder.decode()) {
			if (++guard > 1000) throw new Error('decode loop');
		}

		// Every snapshot owns its own arrays; the mutation cannot leak into
		// the decoder, into later predictions, or into any other snapshot.
		assert.equal(infos.length, 6);
		assert.notEqual(infos[0].vectors, infos[1].vectors);
		assert.deepEqual(Array.from(infos[1].vectors), secondBefore.vectors);
		assert.deepEqual(Array.from(infos[1].kinds), secondBefore.kinds);
		assertGrid(infos[1], FIXTURE_GRIDS[1].rows);
		assertGrid(infos[5], FIXTURE_GRIDS[5].rows);
		assert.deepEqual(Array.from(infos[0].vectors), new Array(12).fill(1234));
		assert.deepEqual(Array.from(infos[0].kinds), new Array(6).fill(9));
		if (withRenderer) {
			assert.deepEqual(hashes, PIXEL_BASELINE);
		}
	});
}

test('crafted half-pel stream: signs, accumulation, slice reset, skips, P-intra, inter without motion', () => {
	const JSMpeg = load(makeContext(false), BASE_FILES);
	const data = bitstream.buildHalfPelStream(JSMpeg);
	const infos = [];
	const decoder = makeDecoder(JSMpeg, {
		onVideoMotion: (d, info) => infos.push(info)
	});
	const calls = instrumentCopyMacroblock(decoder);
	decodeAll(decoder, data);

	assert.equal(infos.length, 2);
	assertGrid(infos[0], [
		[[1,0,0],[1,0,0],[1,0,0]],
		[[1,0,0],[1,0,0],[1,0,0]]
	]);
	// P picture, f_code 1, full_pel 0:
	//  mb0 (+1,-1); mb1 accumulates to (+3,-1); mb2 is a P-frame intra (0,0);
	//  slice 2 resets predictors, mb3 is inter without motion (0,0);
	//  mb4 is skipped via increment 2 (0,0); mb5 gets (-1,+1).
	assertGrid(infos[1], [
		[[2,1,-1],[2,3,-1],[1,0,0]],
		[[2,0,0],[3,0,0],[2,-1,1]]
	]);
	assertCopyMacroblockCrossCheck(infos[1], calls);
});

test('crafted full-pel stream: full_pel doubling, f_code wraparound, consecutive skips, zero inheritance', () => {
	const JSMpeg = load(makeContext(false), BASE_FILES);
	const data = bitstream.buildFullPelWrapStream(JSMpeg);
	const infos = [];
	const decoder = makeDecoder(JSMpeg, {
		onVideoMotion: (d, info) => infos.push(info)
	});
	const calls = instrumentCopyMacroblock(decoder);
	decodeAll(decoder, data);

	assert.equal(infos.length, 2);
	// full_pel_forward=1, f_code=2:
	//  mb0: differential +32 wraps the predictor to -32, doubled -> (-64,+2);
	//  mb1 keeps the wrapped horizontal predictor, vertical returns to 0;
	//  mb2/mb3 are consecutive skipped blocks from one increment of 3;
	//  mb4 gets (+2,0); mb5 codes no motion and must be (0,0), not inherited.
	assertGrid(infos[1], [
		[[2,-64,2],[2,-64,0],[3,0,0]],
		[[3,0,0],[2,2,0],[2,0,0]]
	]);
	assertCopyMacroblockCrossCheck(infos[1], calls);
});

test('crafted B/D stream: skipped pictures produce no callback and no frameIndex gap', () => {
	const JSMpeg = load(makeContext(false), BASE_FILES);
	const data = bitstream.buildSkippedPictureStream(JSMpeg);
	const {infos} = collect(JSMpeg, data, false);

	assert.deepEqual(infos.map(i => i.frameIndex), [0, 1]);
	assert.deepEqual(infos.map(i => i.pictureType), ['I', 'P']);
	assertGrid(infos[1], [
		[[2,0,0],[3,0,0],[3,0,0]],
		[[3,0,0],[3,0,0],[2,0,0]]
	]);
});

test('crafted partial-coverage stream: edge macroblocks kept, uncovered cells are kind 0', () => {
	const JSMpeg = load(makeContext(false), BASE_FILES);
	const data = bitstream.buildPartialCoverageStream(JSMpeg);
	const {infos} = collect(JSMpeg, data, false);

	assert.equal(infos.length, 2);
	for (const info of infos) {
		assert.equal(info.width, 40); // visible width, not a multiple of 16
		assert.equal(info.height, 32);
		assert.equal(info.mbWidth, 3); // coded edge column is still included
		assert.equal(info.mbHeight, 2);
	}
	assertGrid(infos[0], [
		[[1,0,0],[1,0,0],[0,0,0]],
		[[0,0,0],[0,0,0],[0,0,0]]
	]);
	assertGrid(infos[1], [
		[[2,1,1],[1,0,0],[0,0,0]],
		[[0,0,0],[0,0,0],[0,0,0]]
	]);
});

test('WASM decoder rejects onVideoMotion with an explicit error', () => {
	const JSMpeg = load(makeContext(false), BASE_FILES.concat('mpeg1-wasm.js'));
	assert.throws(
		() => new JSMpeg.Decoder.MPEG1VideoWASM({onVideoMotion: () => {}}),
		/disableWebAssembly/
	);
	// Without the option, construction behaves exactly as before.
	assert.doesNotThrow(() => new JSMpeg.Decoder.MPEG1VideoWASM({}));
});

// Minimal Source stand-in: Player only needs connect/start/resume/destroy
// for a constructor-level test; no network is ever touched.
function FakeSource() {
	this.streaming = false;
}
FakeSource.prototype.connect = function() {};
FakeSource.prototype.start = function() {};
FakeSource.prototype.resume = function() {};
FakeSource.prototype.destroy = function() {};

test('Player passes onVideoMotion to the JS decoder when WebAssembly is disabled', () => {
	const files = BASE_FILES.concat(
		'wasm-module.js', 'ts.js', 'canvas2d.js', 'player.js'
	);
	const JSMpeg = load(makeContext(false), files);
	const callback = () => {};
	const player = new JSMpeg.Player('test://stream', {
		source: FakeSource,
		disableWebAssembly: true,
		disableGl: true,
		audio: false,
		autoplay: false,
		onVideoMotion: callback
	});
	assert.ok(player.video instanceof JSMpeg.Decoder.MPEG1Video);
	assert.equal(player.video.onMotionCallback, callback);
});

test('Player refuses onVideoMotion on the WASM path and never loads a binary', () => {
	const files = BASE_FILES.concat(
		'wasm-module.js', 'mpeg1-wasm.js', 'ts.js', 'canvas2d.js', 'player.js'
	);
	const JSMpeg = load(makeContext(true), files);
	assert.throws(
		() => new JSMpeg.Player('test://stream', {
			source: FakeSource,
			disableGl: true,
			audio: false,
			autoplay: false,
			onVideoMotion: () => {}
		}),
		/disableWebAssembly/
	);
	// Rejection happens at decoder construction. The shared WASM module may
	// have been created, but no binary download or instantiation may start.
	const moduleAfter = JSMpeg.WASMModule.CACHED_MODULE;
	assert.ok(moduleAfter, 'WASM module handle is created lazily before the decoder');
	assert.equal(
		moduleAfter.loadingFromFileStarted, false,
		'no jsmpeg.wasm download may be attempted'
	);
	assert.equal(
		moduleAfter.loadingFromBufferStarted, false,
		'no inlined wasm binary may be instantiated'
	);
});

test('omitting onVideoMotion leaves the decoder behavior unchanged', () => {
	const JSMpeg = load(makeContext(false), BASE_FILES);
	const decoder = makeDecoder(JSMpeg, {});
	assert.equal(decoder.onMotionCallback, null);
	const hashes = [];
	decoder.connect({
		resize() {},
		render(y) {
			hashes.push(
				crypto.createHash('sha256').update(y).digest('hex').slice(0, 12)
			);
		}
	});
	decodeAll(decoder, fixtureData);
	assert.deepEqual(hashes, PIXEL_BASELINE);
});

test('onVideoDecode still fires exactly once per rebuilt picture', () => {
	const JSMpeg = load(makeContext(false), BASE_FILES);
	const decodeEvents = [];
	const motionEvents = [];
	const decoder = makeDecoder(JSMpeg, {
		onVideoDecode: (d, elapsed) => decodeEvents.push(elapsed),
		onVideoMotion: (d, info) => motionEvents.push(info.frameIndex)
	});
	decodeAll(decoder, fixtureData);
	assert.equal(decodeEvents.length, 6);
	assert.deepEqual(motionEvents, [0, 1, 2, 3, 4, 5]);
	assert.ok(decodeEvents.every(t => typeof t === 'number'));
});
