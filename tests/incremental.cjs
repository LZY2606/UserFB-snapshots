'use strict';
// Incremental-mode tests for JSMpeg.Decoder.MPEG1Video.
//
// The elementary streams below are built by hand from auditable bit-level
// helpers. Every variable-length code is derived from the decoder's own trie
// tables, so the encoder cannot silently drift from what the parser accepts.
// Tests always go through the public write()/decode() entry points and judge
// results through rendered planes, callback counts and timing - never by
// poking hand-filled internal structures alone.

const path = require('node:path');
const load = require(path.join(__dirname, '..', 'task_support', 'load.cjs'));
const J = load();
const M = J.Decoder.MPEG1Video;

// --------------------------------------------------------------------------
// Bit writer
// --------------------------------------------------------------------------

class BitWriter {
	constructor() { this.bits = []; }
	put(value, n) {
		for (let i = n - 1; i >= 0; i--) {
			this.bits.push((value >>> i) & 1);
		}
		return this;
	}
	str(s) {
		for (const c of s) this.bits.push(Number(c));
		return this;
	}
	align() {
		while (this.bits.length % 8) this.bits.push(0);
		return this;
	}
	start(code) {
		this.align();
		return this.put(1, 24).put(code, 8);
	}
	bytes() {
		this.align();
		const out = Buffer.alloc(this.bits.length / 8);
		this.bits.forEach((v, i) => { out[i >> 3] |= v << (7 - (i % 8)); });
		return out;
	}
}

// Walks one of the decoder's trie tables and returns leafValue -> bit string.
// Negative entries are parser error states, not reachable codes.
function vlcMap(table) {
	const out = new Map();
	const queue = [[0, '']];
	while (queue.length) {
		const [state, prefix] = queue.shift();
		if (state < 0 || state >= table.length) {
			continue;
		}
		if (table[state] === 0) {
			const value = table[state + 2];
			if (!out.has(value)) out.set(value, prefix);
			continue;
		}
		const s0 = table[state];
		const s1 = table[state + 1];
		if (s0 >= 0) queue.push([s0, prefix + '0']);
		if (s1 >= 0) queue.push([s1, prefix + '1']);
	}
	return out;
}

const addrMap = vlcMap(M.MACROBLOCK_ADDRESS_INCREMENT);
const mbTypeIMap = vlcMap(M.MACROBLOCK_TYPE_INTRA);
const mbTypePMap = vlcMap(M.MACROBLOCK_TYPE_PREDICTIVE);
const cbpMap = vlcMap(M.CODE_BLOCK_PATTERN);
const motionMap = vlcMap(M.MOTION);
const dcLumMap = vlcMap(M.DCT_DC_SIZE_LUMINANCE);
const dcChrMap = vlcMap(M.DCT_DC_SIZE_CHROMINANCE);
const coeffMap = vlcMap(M.DCT_COEFF);
const EOB = coeffMap.get(0x0001) + '0'; // 0x0001 followed by a 0 bit

function dcSize(diff) {
	const a = Math.abs(diff);
	let s = 0;
	while ((1 << s) <= a) s++;
	return s;
}

function writeDc(bw, diff, sizeMap) {
	const s = dcSize(diff);
	bw.str(sizeMap.get(s));
	if (s > 0) {
		// MPEG-1 two's complement: positive as-is, negative masked to s bits.
		bw.put(diff & ((1 << s) - 1), s);
	}
}

function writeCoeff(bw, run, level) {
	if (run < 0 || run > 63 || level === 0 || level < -255 || level > 255) {
		throw new Error('unencodable coefficient run=' + run + ' level=' + level);
	}
	// run=0/level=+-1 shares the 0x0001 VLC with end_of_block and needs the
	// decoder's extra discriminator/sign handling; the fixtures avoid it.
	if (run === 0 && Math.abs(level) === 1) {
		throw new Error('ambiguous run=0/level=+-1 coefficient; use another level');
	}
	const key = (run << 8) | Math.abs(level);
	const code = coeffMap.get(key);
	// Only the ordinary VLC path uses a table code. 0x0001 in this position
	// would be the EOB ambiguity rejected above; everything else escapes.
	if (code !== undefined && key !== 0xffff) {
		bw.str(code).put(level < 0 ? 1 : 0, 1);
		return;
	}
	// escape: 000001, 6-bit run, signed level (ordinary and extended forms)
	bw.str(coeffMap.get(0xffff)).put(run, 6);
	if (level >= 1 && level <= 127) {
		bw.put(level, 8);
	}
	else if (level >= -127 && level <= -1) {
		bw.put(256 + level, 8);
	}
	else if (level >= 128 && level <= 255) {
		bw.put(0, 8).put(level, 8);
	}
	else {
		// -255..-128: first byte 128, second byte level + 256
		bw.put(128, 8).put(level + 256, 8);
	}
}

function writeIntraBlock(bw, dcDiff, acs, chroma) {
	writeDc(bw, dcDiff, chroma ? dcChrMap : dcLumMap);
	for (const [run, level] of acs) writeCoeff(bw, run, level);
	bw.str(EOB);
}

function writeInterBlock(bw, acs) {
	for (const [run, level] of acs) writeCoeff(bw, run, level);
	bw.str(EOB);
}

// --------------------------------------------------------------------------
// Stream construction: 64x32 (4x2 macroblocks, two slices per picture)
// --------------------------------------------------------------------------

function sequenceHeader(width, height) {
	const bw = new BitWriter();
	bw.start(0xb3)
		.put(width, 12).put(height, 12)
		.put(1, 4)       // pixel aspect ratio
		.put(3, 4)       // frame rate code 3 -> 25 fps
		.put(100, 18).put(1, 1).put(20, 10).put(0, 1).put(0, 1).put(0, 1);
	return bw;
}

function pictureHeader(bw, tempRef, type) {
	bw.start(0x00).put(tempRef, 10).put(type, 3).put(0xffff, 16);
	if (type === M.PICTURE_TYPE.PREDICTIVE) {
		bw.put(0, 1)   // full_pel_forward
			.put(1, 3);  // forward_f_code = 1
	}
}

function sliceHeader(bw, id, quantizerScale) {
	bw.start(id).put(quantizerScale, 5).put(0, 1);
}

// Rich stream exercising nonzero motion/residuals, positive and negative
// differences, all three escape level forms, slice resets, macroblock
// stuffing, runs of skipped MBs, intra MBs in P pictures and several pics.
function richStream() {
	const bw = sequenceHeader(64, 32);

	// --- I picture -------------------------------------------------------
	pictureHeader(bw, 0, M.PICTURE_TYPE.INTRA);
	sliceHeader(bw, 1, 8);

	// MB0: quantizer override, positive/negative DC, escape levels
	bw.str(addrMap.get(1)).str(mbTypeIMap.get(0x11)).put(12, 5);
	writeIntraBlock(bw, 17, [[0, 5], [1, -3]], false);
	writeIntraBlock(bw, -9, [[0, 200]], false);  // escape, extended positive
	writeIntraBlock(bw, 4, [[2, -200]], false);   // escape, extended negative
	writeIntraBlock(bw, 0, [[0, 7]], false);
	writeIntraBlock(bw, 3, [[0, -5]], true);
	writeIntraBlock(bw, -2, [[1, 9]], true);

	// Increment 2: MB1 is skipped, MB2 is coded with ordinary coefficients
	bw.str(addrMap.get(2)).str(mbTypeIMap.get(0x01));
	writeIntraBlock(bw, 11, [[0, 2], [0, -2]], false);
	writeIntraBlock(bw, 6, [[0, 40]], false);     // escape, ordinary positive
	writeIntraBlock(bw, -4, [[3, 2]], false);
	writeIntraBlock(bw, 2, [[0, -40]], false);    // escape, ordinary negative
	writeIntraBlock(bw, 0, [], true);
	writeIntraBlock(bw, 5, [[0, -7]], true);

	// MB3: one macroblock_stuffing code before the address increment
	bw.str(addrMap.get(34)).str(addrMap.get(1)).str(mbTypeIMap.get(0x01));
	for (let k = 0; k < 4; k++) {
		writeIntraBlock(bw, k - 1, [[0, k + 2]], false);
	}
	writeIntraBlock(bw, 2, [], true);
	writeIntraBlock(bw, -2, [], true);

	// Slice 2: DC predictors must reset here
	sliceHeader(bw, 2, 10);
	bw.str(addrMap.get(1)).str(mbTypeIMap.get(0x01));
	writeIntraBlock(bw, 30, [[0, 11]], false);
	writeIntraBlock(bw, -30, [[0, -11]], false);
	writeIntraBlock(bw, 1, [], false);
	writeIntraBlock(bw, -1, [], false);
	writeIntraBlock(bw, 8, [[0, 4]], true);
	writeIntraBlock(bw, -8, [[0, -4]], true);

	for (let mb = 5; mb < 8; mb++) {
		bw.str(addrMap.get(1)).str(mbTypeIMap.get(mb === 6 ? 0x11 : 0x01));
		if (mb === 6) bw.put(6, 5);
		for (let k = 0; k < 4; k++) {
			writeIntraBlock(bw, (mb % 2 ? -1 : 1) * (k + 2),
				[[0, k + 2]], false);
		}
		writeIntraBlock(bw, mb - 4, [], true);
		writeIntraBlock(bw, 4 - mb, [], true);
	}

	// --- P picture -------------------------------------------------------
	pictureHeader(bw, 1, M.PICTURE_TYPE.PREDICTIVE);
	sliceHeader(bw, 1, 8);

	// MB0: inter, motion (+3,-2), chroma-only coded block pattern
	bw.str(addrMap.get(1)).str(mbTypePMap.get(0x1a)); // quant+motion+pattern
	bw.put(9, 5);
	bw.str(motionMap.get(3)).str(motionMap.get(-2));
	bw.str(cbpMap.get(0x03)); // chroma blocks coded
	writeInterBlock(bw, [[0, 4], [2, -6]]);
	writeInterBlock(bw, [[0, -130], [1, 130]]); // extended escape both signs

	// Increment 3: MB1 and MB2 are consecutive skipped MBs, MB3 is the
	// explicit inter macroblock at the end of the row.
	bw.str(addrMap.get(3)).str(mbTypePMap.get(0x02));
	bw.str(cbpMap.get(0x07)); // three luma blocks coded on the explicit MB
	writeInterBlock(bw, [[0, 9]]);
	writeInterBlock(bw, [[0, -9]]);
	writeInterBlock(bw, [[0, 5], [1, -5]]);

	// Slice 2: motion/DC predictors reset
	sliceHeader(bw, 2, 7);
	// MB4: intra MB inside a P picture
	bw.str(addrMap.get(1)).str(mbTypePMap.get(0x01));
	writeIntraBlock(bw, 21, [[0, -8]], false);
	writeIntraBlock(bw, 0, [], false);
	writeIntraBlock(bw, 0, [], false);
	writeIntraBlock(bw, 0, [], false);
	writeIntraBlock(bw, 1, [], true);
	writeIntraBlock(bw, -1, [], true);

	// MB5: motion (-5,+4), all six blocks coded, escape ordinary levels
	bw.str(addrMap.get(1)).str(mbTypePMap.get(0x0a)); // motion + pattern
	bw.str(motionMap.get(-5)).str(motionMap.get(4));
	bw.str(cbpMap.get(0x3f));
	for (let k = 0; k < 4; k++) {
		writeInterBlock(bw, [[0, k % 2 ? -3 : 3], [1, -7]]);
	}
	writeInterBlock(bw, [[0, 40]]);
	writeInterBlock(bw, [[0, -40]]);

	// MB6 skipped, MB7 explicit inter without motion
	bw.str(addrMap.get(2)).str(mbTypePMap.get(0x02));
	bw.str(cbpMap.get(0x0c));
	writeInterBlock(bw, [[0, 6]]);
	writeInterBlock(bw, [[0, -6]]);

	// --- I picture again, so reference rotation is exercised ------------
	pictureHeader(bw, 2, M.PICTURE_TYPE.INTRA);
	sliceHeader(bw, 1, 8);
	for (let mb = 0; mb < 4; mb++) {
		bw.str(addrMap.get(mb === 0 ? 1 : 1)).str(mbTypeIMap.get(0x01));
		for (let k = 0; k < 4; k++) {
			writeIntraBlock(bw, 40 + mb + k, [[0, k + 2]], false);
		}
		writeIntraBlock(bw, 12, [], true);
		writeIntraBlock(bw, -12, [], true);
	}
	sliceHeader(bw, 2, 8);
	for (let mb = 0; mb < 4; mb++) {
		bw.str(addrMap.get(1)).str(mbTypeIMap.get(0x01));
		for (let k = 0; k < 4; k++) {
			writeIntraBlock(bw, -(40 + mb + k), [], false);
		}
		writeIntraBlock(bw, mb === 0 ? 7 : 0, [], true);
		writeIntraBlock(bw, mb === 0 ? -7 : 0, [], true);
	}

	return bw.bytes();
}

// One very long single-slice I picture (> 64 KiB of slice payload),
// 4080x16 = 255 macroblocks, every block carrying escape coefficients.
function longSliceStream() {
	const bw = sequenceHeader(4080, 16);
	pictureHeader(bw, 0, M.PICTURE_TYPE.INTRA);
	sliceHeader(bw, 1, 8);
	for (let mb = 0; mb < 255; mb++) {
		bw.str(addrMap.get(1)).str(mbTypeIMap.get(0x01));
		for (let block = 0; block < 6; block++) {
			const acs = [];
			for (let k = 0; k < 30; k++) {
				acs.push([0, ((k + mb + block) % 2) ? 11 : -13]);
			}
			writeIntraBlock(bw, (mb % 7) - 3, acs, block >= 4);
		}
	}
	return bw.bytes();
}

// --------------------------------------------------------------------------
// Decoder driving helpers
// --------------------------------------------------------------------------

function makeDecoder(extra) {
	const renders = [];
	const callbacks = [];
	const progress = [];
	const capacity = [];
	const dec = new M(Object.assign({
		incremental: true,
		decodeFirstFrame: false,
		onVideoDecode: () => callbacks.push(dec.currentTime)
	}, extra || {}));
	dec.connect({
		resize() {},
		render(y, cr, cb) {
			renders.push({
				y: Uint8Array.from(y),
				cr: Uint8Array.from(cr),
				cb: Uint8Array.from(cb)
			});
		}
	});
	return {dec, renders, callbacks, progress, capacity};
}

function feed(handle, stream, chunkSize, ptsBase) {
	const {dec, progress, capacity} = handle;
	for (let off = 0; off < stream.length;) {
		const end = Math.min(off + chunkSize, stream.length);
		dec.write(ptsBase + off, stream.subarray(off, end));
		let guard = 0;
		while (dec.decode()) {
			if (++guard > 100000) throw new Error('decode loop did not settle');
		}
		progress.push(dec.getDecodeProgress());
		capacity.push(dec.bits.bytes.length);
		off = end;
	}
}

function feedArrayWrites(handle, stream, chunkSize, ptsBase) {
	const {dec} = handle;
	for (let off = 0; off < stream.length;) {
		const mid = Math.min(off + Math.max(1, chunkSize >> 1), stream.length);
		const end = Math.min(off + chunkSize, stream.length);
		dec.write(ptsBase, [
			stream.subarray(off, mid),
			stream.subarray(mid, end)
		]);
		while (dec.decode()) {}
		off = end;
	}
}

function baselineDecoder(stream) {
	const renders = [];
	const dec = new M({decodeFirstFrame: false});
	dec.connect({
		resize() {},
		render(y, cr, cb) {
			renders.push({
				y: Uint8Array.from(y),
				cr: Uint8Array.from(cr),
				cb: Uint8Array.from(cb)
			});
		}
	});
	dec.write(0, Buffer.concat([stream, Buffer.from([0, 0, 1, 0xb7])]));
	while (dec.decode()) {}
	return {dec, renders};
}

function planesEqual(a, b) {
	return a.y.length === b.y.length &&
		Buffer.from(a.y).equals(Buffer.from(b.y)) &&
		Buffer.from(a.cr).equals(Buffer.from(b.cr)) &&
		Buffer.from(a.cb).equals(Buffer.from(b.cb));
}

function nthIndexOf(stream, needle, n) {
	let pos = -1;
	for (let i = 0; i <= n; i++) {
		pos = stream.indexOf(needle, pos + 1);
	}
	return pos;
}

// --------------------------------------------------------------------------
// Test cases
// --------------------------------------------------------------------------

const cases = [];
function add(group, name, run) {
	cases.push({group, name, run});
}

// ---------------- bitstream group ----------------------------------------

add('bitstream', 'feeding-strategies-match-baseline-planes', (check) => {
	const stream = richStream();
	const ref = baselineDecoder(stream);

	const oneShot = makeDecoder();
	oneShot.dec.write(1.5, stream);
	while (oneShot.dec.decode()) {}
	oneShot.dec.finish();
	while (oneShot.dec.decode()) {}

	const sizes = [1, 2, 3, 5, 7, 13, 31, 64, 100, 257, 1024];
	for (const size of sizes) {
		const h = makeDecoder();
		feed(h, stream, size, 1.5);
		h.dec.finish();
		while (h.dec.decode()) {}
		check(h.renders.length === ref.renders.length,
			'chunk size ' + size + ': render count ' + h.renders.length);
		for (let i = 0; i < ref.renders.length; i++) {
			check(planesEqual(h.renders[i], ref.renders[i]),
				'chunk size ' + size + ': picture ' + i + ' planes match baseline');
			check(planesEqual(h.renders[i], oneShot.renders[i]),
				'chunk size ' + size + ': picture ' + i + ' planes match one-shot');
		}
	}
	check(oneShot.renders.length === 3, 'one-shot rendered all three pictures');
});

add('bitstream', 'start-code-split-at-every-offset', (check) => {
	const stream = richStream();
	const ref = baselineDecoder(stream);
	// Every chunk size from 1 through 16 forces start codes to break at
	// different byte offsets.
	for (let size = 1; size <= 16; size++) {
		const h = makeDecoder();
		feed(h, stream, size, 0);
		h.dec.finish();
		while (h.dec.decode()) {}
		check(h.renders.length === ref.renders.length,
			'size ' + size + ': render count');
		check(h.renders.every((r, i) => planesEqual(r, ref.renders[i])),
			'size ' + size + ': planes match');
	}
});

add('bitstream', 'finish-completes-picture-without-trailing-start-code', (check) => {
	const stream = richStream();
	const ref = baselineDecoder(stream);

	// Feed only up to the middle of the last picture: decode must stay false
	// for that picture and nothing extra may render.
	const cut = stream.length - 40;
	const h = makeDecoder();
	feed(h, stream.subarray(0, cut), 3, 0);
	const rendersBefore = h.renders.length;
	check(rendersBefore === 2, 'first two pictures rendered before final bytes');
	check(h.dec.decode() === false, 'truncated tail cannot complete a picture');

	feed(h, stream.subarray(cut), 3, 0);
	check(h.dec.decode() === false, 'missing end marker keeps decode() false');
	h.dec.finish();
	check(h.dec.decode() === true, 'finish() lets the last complete picture flush');
	check(h.dec.decode() === false, 'no second completion after flush');
	check(h.dec.decode() === false, 'repeated decode() stays false');
	check(h.renders.length === 3, 'exactly three renders overall');
	check(planesEqual(h.renders[2], ref.renders[2]),
		'flushed picture planes match baseline');
});

add('bitstream', 'finish-idempotent-and-write-after-finish-throws', (check) => {
	const h = makeDecoder();
	h.dec.finish();
	h.dec.finish();
	check(h.dec.decode() === false, 'finish on empty input renders nothing');
	let threw = false;
	try { h.dec.write(0, Uint8Array.of(1)); }
	catch (e) { threw = true; }
	check(threw, 'write() after finish() must throw');
});

add('bitstream', 'only-first-pts-anchors-time', (check) => {
	const stream = richStream();
	const h = makeDecoder();
	feed(h, stream, 11, 100); // wildly increasing per-chunk pts values
	h.dec.finish();
	while (h.dec.decode()) {}
	check(h.dec.startTime === 100, 'start time is the first pts');
	check(h.dec.timestamps.length === 0, 'no per-chunk timestamps retained');
	check(Math.abs(h.dec.currentTime - (100 + 3 / 25)) < 1e-9,
		'decoded time advances by frame duration only');
	check(h.callbacks.length === 3, 'one callback per picture');
});

add('bitstream', 'write-accepts-array-of-buffers', (check) => {
	const stream = richStream();
	const ref = baselineDecoder(stream);
	const h = makeDecoder();
	feedArrayWrites(h, stream, 37, 0);
	h.dec.finish();
	while (h.dec.decode()) {}
	check(h.renders.length === ref.renders.length, 'array writes render count');
	check(h.renders.every((r, i) => planesEqual(r, ref.renders[i])),
		'array writes produce identical planes');
});

// ---------------- state group --------------------------------------------

add('state', 'progress-lifecycle-and-macroblock-counting', (check) => {
	const stream = richStream();
	const h = makeDecoder();
	const seenTypes = new Set();
	let prevType = 0;
	let prevMb = 0;
	let zeroResets = 0;

	feed(h, stream, 7, 0);
	check(h.progress.length > 0, 'progress sampled at every settle point');
	check(h.progress[0].pictureType === 0 && h.progress[0].macroblocks === 0,
		'before the first picture no picture is active');

	for (const p of h.progress) {
		check(p.bufferedBytes >= 0, 'bufferedBytes is non-negative');
		if (p.pictureType === 0) {
			check(p.macroblocks === 0, 'inactive picture carries no MB count');
			if (prevType !== 0) zeroResets++;
		}
		else {
			seenTypes.add(p.pictureType);
			check(p.macroblocks >= 0 && p.macroblocks <= 8,
				'count stays within the picture MB total');
			if (p.pictureType === prevType) {
				check(p.macroblocks >= prevMb,
					'macroblock count is monotonic within a picture');
			}
		}
		prevType = p.pictureType;
		prevMb = p.macroblocks;
	}
	check(seenTypes.has(1) && seenTypes.has(2), 'both I and P pictures observed');
	check(zeroResets >= 2, 'counter reset after every completed picture');
	check(h.renders.length + (h.dec.getDecodeProgress().pictureType ? 1 : 0) >= 2,
		'progress reported while pictures were still open');
});

add('state', 'skipped-macroblocks-counted-and-built-once', (check) => {
	const stream = richStream();

	// Count reconstruction primitives while feeding byte by byte.
	const hByte = makeDecoder();
	const copyByteOrig = hByte.dec.copyMacroblock.bind(hByte.dec);
	const idctOrig = M.IDCT;
	let copiesByte = 0, idctByte = 0;
	hByte.dec.copyMacroblock = function() {
		copiesByte++;
		return copyByteOrig.apply(null, arguments);
	};
	M.IDCT = function(block) { idctByte++; return idctOrig(block); };
	feed(hByte, stream, 1, 0);
	hByte.dec.finish();
	while (hByte.dec.decode()) {}
	M.IDCT = idctOrig;

	// Same stream in a single write.
	const hOne = makeDecoder();
	const copyOneOrig = hOne.dec.copyMacroblock.bind(hOne.dec);
	let copiesOne = 0, idctOne = 0;
	hOne.dec.copyMacroblock = function() {
		copiesOne++;
		return copyOneOrig.apply(null, arguments);
	};
	M.IDCT = function(block) { idctOne++; return idctOrig(block); };
	hOne.dec.write(0, stream);
	hOne.dec.finish();
	while (hOne.dec.decode()) {}
	M.IDCT = idctOrig;

	check(copiesByte === copiesOne,
		'prediction copies identical regardless of chunking (' + copiesByte + ')');
	check(idctByte === idctOne,
		'IDCT runs identical regardless of chunking (' + idctByte + ')');
	// I-picture skip (1) + P: explicit inter predictions MB0, MB3, MB5, MB7
	// (4) and skipped MB1, MB2, MB6 (3) = 8 prediction reconstructions.
	check(copiesByte === 8, 'exactly the designed eight copyMacroblock calls');
	check(hByte.renders.every((r, i) => planesEqual(r, hOne.renders[i])),
		'planes identical between feedings');

	// Feed everything up to (but not including) picture 3: the P picture is
	// fully decoded yet cannot close, so progress exposes its final count.
	const pic3 = nthIndexOf(stream, Buffer.from([0, 0, 1, 0x00]), 2);
	const hP = makeDecoder();
	feed(hP, stream.subarray(0, pic3), 17, 0);
	const p = hP.dec.getDecodeProgress();
	check(p.pictureType === 2, 'P picture stays open without a following code');
	check(p.macroblocks === 8, 'all eight P MBs counted, skipped ones included');
});

add('state', 'half-picture-never-renders-or-rotates-reference', (check) => {
	const stream = richStream();
	// Cut just inside slice 2 of the P picture: slice 1 (four MBs) is done,
	// the slice-2 header and first macroblock bytes are partially present.
	const pSlice2 = nthIndexOf(stream, Buffer.from([0, 0, 1, 0x02]), 1);
	const cut = pSlice2 + 6;

	const h = makeDecoder();
	feed(h, stream.subarray(0, cut), 5, 0);
	check(h.renders.length === 1, 'only the completed I picture rendered');
	check(h.callbacks.length === 1, 'callback fires once for the complete pic');
	const p = h.dec.getDecodeProgress();
	check(p.pictureType === 2, 'P picture is still in progress');
	check(p.macroblocks >= 1 && p.macroblocks < 8,
		'some but not all P macroblocks complete (' + p.macroblocks + ')');
	const timeBefore = h.dec.currentTime;
	check(h.dec.decode() === false, 'partial picture cannot decode');
	check(h.dec.currentTime === timeBefore, 'time does not advance mid-picture');

	// Complete the stream; the half-parsed macroblock must rebuild correctly.
	feed(h, stream.subarray(cut), 5, 0);
	h.dec.finish();
	while (h.dec.decode()) {}
	const ref = baselineDecoder(stream);
	check(h.renders.length === 3, 'all three pictures render once data completes');
	check(planesEqual(h.renders[1], ref.renders[1]),
		'P picture matches baseline despite mid-macroblock interruption');
	check(planesEqual(h.renders[2], ref.renders[2]),
		'final picture matches baseline');
});

add('state', 'slice-boundary-resets-predictors', (check) => {
	const stream = richStream();
	// Prefix ends exactly at slice 2 of the first I picture: slice 1 has
	// committed all four row-0 macroblocks, slice 2 cannot start yet.
	const slice2 = nthIndexOf(stream, Buffer.from([0, 0, 1, 0x02]), 0);
	const h = makeDecoder();
	feed(h, stream.subarray(0, slice2), 4, 0);
	const p = h.dec.getDecodeProgress();
	check(p.pictureType === 1, 'picture stays open across slices');
	check(p.macroblocks === 4, 'first slice submitted its four MBs');

	feed(h, stream.subarray(slice2), 4, 0);
	h.dec.finish();
	while (h.dec.decode()) {}
	const ref = baselineDecoder(stream);
	check(planesEqual(h.renders[0], ref.renders[0]),
		'predictor reset at slice 2 produces baseline pixels');
});

add('state', 'render-and-callback-fire-exactly-once-per-picture', (check) => {
	const stream = richStream();
	const h = makeDecoder();
	feed(h, stream, 3, 0);
	h.dec.finish();
	while (h.dec.decode()) {}
	check(h.renders.length === 3, 'three renders for three pictures');
	check(h.callbacks.length === 3, 'three callbacks for three pictures');
});

// ---------------- buffer group --------------------------------------------

add('buffer', 'long-slice-submits-macroblocks-before-slice-end', (check) => {
	const stream = longSliceStream();
	check(stream.length > 64 * 1024, 'fixture slice really exceeds 64 KiB');
	const h = makeDecoder();
	let sawProgressEarly = false;
	let peakMacroblocks = 0;
	for (let off = 0; off < stream.length;) {
		const end = Math.min(off + 1024, stream.length);
		h.dec.write(0, stream.subarray(off, end));
		while (h.dec.decode()) {}
		const p = h.dec.getDecodeProgress();
		peakMacroblocks = Math.max(peakMacroblocks, p.macroblocks);
		check(h.dec.bits.bytes.length === M.INCREMENTAL_BUFFER_SIZE,
			'backing capacity stays at 16384 bytes');
		check(h.dec.bits.byteLength <= M.INCREMENTAL_BUFFER_SIZE,
			'retained bytes never exceed backing capacity');
		if (p.pictureType === 1 && p.macroblocks > 0 && end < stream.length - 4096) {
			sawProgressEarly = true;
		}
		off = end;
	}
	check(sawProgressEarly, 'macroblocks were committed well before slice end');
	check(peakMacroblocks === 255, 'all 255 MBs accounted for before publish');
	h.dec.finish();
	check(h.dec.decode() === true, 'long picture completes after finish()');
	check(h.dec.getDecodeProgress().pictureType === 0,
		'picture closes and its counter resets after publish');

	const ref = baselineDecoder(stream);
	// A single write larger than the fixed backing store is outside the
	// incremental contract, so the "different chunking" reference also feeds
	// bounded writes, just with another size.
	const one = makeDecoder();
	feed(one, stream, 512, 0);
	one.dec.finish();
	while (one.dec.decode()) {}
	check(planesEqual(h.renders[0], ref.renders[0]), 'chunked output matches baseline');
	check(planesEqual(h.renders[0], one.renders[0]), 'chunked output matches one-shot');
});

add('buffer', 'consumed-prefixes-reclaimed-with-bounded-buffer', (check) => {
	const stream = longSliceStream();
	const h = makeDecoder();
	let maxBuffered = 0;
	for (let off = 0; off < stream.length;) {
		const end = Math.min(off + 1024, stream.length);
		h.dec.write(0, stream.subarray(off, end));
		while (h.dec.decode()) {}
		const p = h.dec.getDecodeProgress();
		maxBuffered = Math.max(maxBuffered, p.bufferedBytes);
		check(p.bufferedBytes < 4096, 'retained compressed bytes stay small');
		off = end;
	}
	check(maxBuffered < 4096, 'peak retained bytes ' + maxBuffered + ' stayed bounded');
	check(h.dec.bits.byteLength <= M.INCREMENTAL_BUFFER_SIZE, 'no buffer expansion');
});

add('buffer', 'no-full-stream-copy-retained-anywhere', (check) => {
	const stream = longSliceStream();
	const h = makeDecoder();
	feed(h, stream, 1024, 0);
	for (const key of Object.keys(h.dec)) {
		const v = h.dec[key];
		if (ArrayBuffer.isView(v)) {
			check(v.length < stream.length,
				'field "' + key + '" (' + v.length + ') is not a full-stream copy');
			check(v.length <= Math.max(h.dec.codedSize, M.INCREMENTAL_BUFFER_SIZE),
				'field "' + key + '" is a fixed-size plane or the bit buffer');
		}
	}
});

add('buffer', 'buffered-bytes-includes-uncommitted-syntax', (check) => {
	const stream = richStream();
	const h = makeDecoder();
	// Feed a single small chunk inside picture 1's slice payload.
	h.dec.write(0, stream.subarray(0, 60));
	while (h.dec.decode()) {}
	const p = h.dec.getDecodeProgress();
	check(p.bufferedBytes === h.dec.bits.byteLength - (h.dec.bits.index >> 3),
		'bufferedBytes reflects bytes the suspended parser still holds');
	check(p.bufferedBytes > 0, 'uncommitted syntax remains buffered');
	check(p.bufferedBytes <= 60, 'buffered count never exceeds bytes written');
});

module.exports = cases;
