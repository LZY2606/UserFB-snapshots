'use strict';

// A tiny, deliberately readable MPEG-1 video elementary stream builder.
//
// It only implements the handful of grammar rules the motion-grid tests need
// (sequence/picture/slice/macroblock layers); everything is written through a
// bit-level writer so the produced bytes are easy to audit against the
// MPEG-1 (ISO/IEC 11172-2) syntax. Variable-length codes are looked up in the
// decoder's own Huffman tables via buildVlcMap(), so no code word has to be
// transcribed by hand here.

// Walk a JSMpeg Huffman table (triplets [stateFor0, stateFor1, leafValue])
// and return decodedValue -> bit string.
function buildVlcMap(table) {
	var out = new Map();
	var queue = [[0, '']];
	while (queue.length) {
		var item = queue.shift();
		var row = item[0], prefix = item[1];
		var left = table[row * 3];
		var right = table[row * 3 + 1];
		if (left === 0 && right === 0) {
			out.set(table[row * 3 + 2], prefix); // leaf row
			continue;
		}
		if (left >= 0) {
			queue.push([left / 3, prefix + '0']);
		}
		if (right >= 0) {
			queue.push([right / 3, prefix + '1']);
		}
	}
	return out;
}

function BitWriter() {
	this.bytes = [];
	this.current = 0;
	this.bitsInCurrent = 0;
}

BitWriter.prototype.writeBits = function(value, count) {
	for (var i = count - 1; i >= 0; i--) {
		this.current = (this.current << 1) | ((value >> i) & 1);
		if (++this.bitsInCurrent === 8) {
			this.bytes.push(this.current);
			this.current = 0;
			this.bitsInCurrent = 0;
		}
	}
};

BitWriter.prototype.writeString = function(bits) {
	for (var i = 0; i < bits.length; i++) {
		this.writeBits(bits.charCodeAt(i) === 49 ? 1 : 0, 1);
	}
};

BitWriter.prototype.align = function() {
	if (this.bitsInCurrent) {
		this.writeBits(0, 8 - this.bitsInCurrent);
	}
};

BitWriter.prototype.startCode = function(code) {
	this.align();
	this.bytes.push(0x00, 0x00, 0x01, code);
};

BitWriter.prototype.toUint8Array = function() {
	this.align();
	return new Uint8Array(this.bytes);
};

// Shared grammar pieces -----------------------------------------------------

function writeSequenceHeader(w, width, height) {
	w.startCode(0xb3); // sequence_header_code
	w.writeBits(width, 12);
	w.writeBits(height, 12);
	w.writeBits(0, 4); // pixel aspect ratio
	w.writeBits(5, 4); // frame_rate_code: 5 == 30fps
	w.writeBits(1, 18); // bit_rate (low bit is the marker bit, must be 1)
	w.writeBits(1, 1); // marker bit
	w.writeBits(0, 10); // vbv_buffer_size
	w.writeBits(0, 1); // constrained_parameters_flag
	w.writeBits(0, 1); // no custom intra quant matrix
	w.writeBits(0, 1); // no custom non-intra quant matrix
}

function writePictureHeader(w, temporalReference, pictureType, fullPel, fCode) {
	w.startCode(0x00); // picture_start_code
	w.writeBits(temporalReference, 10);
	w.writeBits(pictureType, 3); // 1=I, 2=P, 3=B, 4=D
	w.writeBits(0xffff, 16); // vbv_delay
	if (pictureType === 2) {
		w.writeBits(fullPel ? 1 : 0, 1); // full_pel_forward
		w.writeBits(fCode, 3); // forward_f_code
	}
}

function writeSliceHeader(w, slice, quantizerScale) {
	w.startCode(slice); // slice_start_code (1..0xAF)
	w.writeBits(quantizerScale, 5);
	w.writeBits(0, 1); // extra_bit_slice == 0
}

// One zero-valued coefficient followed by end_of_block.
//
// Intra blocks: dct_dc_size 0 (the DC predictor is copied through) followed
// directly by EOB, which the decoder accepts because n == 1 after the DC
// coefficient. The macroblock reconstructs as a flat, deterministic area.
//
// Inter blocks: EOB is only legal after at least one coefficient, so emit an
// escape-coded run=0/level=0 coefficient first. It dequantizes to zero and
// adds nothing to the predicted pixels.
function writeZeroBlock(w, vlc, intra, chromaDc) {
	if (intra) {
		// dct_dc_size == 0: luma code "100", chroma code "00"
		w.writeString(chromaDc ? vlc.dcChroma.get(0) : vlc.dcLuma.get(0));
		w.writeString(vlc.endOfBlock);
		return;
	}
	w.writeString(vlc.escape); // DCT escape
	w.writeBits(0, 6); // run
	w.writeBits(0, 8); // level == 0 -> one more level byte follows
	w.writeBits(0, 8); // (signed) level = 0
	w.writeString(vlc.endOfBlock); // "1" + following 0 bit
}

// Intra macroblocks use a different type table depending on the picture
// coding type: an intra in a P picture is code 0x01 of the P table.
function writeIntraMacroblock(w, vlc, pictureType) {
	w.writeString(vlc.address.get(1));
	if (pictureType === 2) {
		w.writeString(vlc.mbPredictive.get(0x01));
	}
	else {
		w.writeString(vlc.mbIntra.get(0x01));
	}
	for (var block = 0; block < 6; block++) {
		writeZeroBlock(w, vlc, true, block >= 4);
	}
}

// Inter macroblock. motionH/motionV are arrays of motion VLC code values with
// optional residual bits (only used with f_code > 1). null means "no forward
// motion coded"; then no motion vectors are transmitted at all.
function writeInterMacroblock(w, vlc, addressIncrement, motion) {
	w.writeString(vlc.address.get(addressIncrement));
	if (motion === null) {
		// pattern-only type 0x02: no motion forward, coded pattern follows
		w.writeString(vlc.mbPredictive.get(0x02));
		// cbp value 1 -> only block 5 present, as a zero block
		w.writeString(vlc.cbp.get(1));
		writeZeroBlock(w, vlc, false, true);
	}
	else {
		// type 0x08: motion forward, no coded pattern
		w.writeString(vlc.mbPredictive.get(0x08));
		for (var axis = 0; axis < 2; axis++) {
			var spec = motion[axis];
			w.writeString(vlc.motion.get(spec.code));
			if (spec.residual !== undefined) {
				w.writeBits(spec.residual, spec.residualBits || 1);
			}
		}
	}
}

// Stream A: f_code 1, half-pel vectors, two slices, P-frame intra, skipped
// MBs and an inter MB without motion.
function buildHalfPelStream(JSMpeg) {
	var vlc = makeVlc(JSMpeg);
	var w = new BitWriter();
	writeSequenceHeader(w, 48, 32);

	// I picture: all six macroblocks intra
	writePictureHeader(w, 0, 1, false, 0);
	writeSliceHeader(w, 1, 8);
	for (var i = 0; i < 6; i++) {
		writeIntraMacroblock(w, vlc, 1);
	}

	// P picture, f_code 1, full_pel_forward = 0
	writePictureHeader(w, 1, 2, false, 1);

	// Slice 1 (macroblock row 0): predictors start at zero
	writeSliceHeader(w, 1, 8);
	writeInterMacroblock(w, vlc, 1, [
		{code: 1}, {code: -1} // -> (+1, -1) half pixels
	]);
	writeInterMacroblock(w, vlc, 1, [
		{code: 2}, {code: 0} // accumulate -> (+3, -1)
	]);
	writeIntraMacroblock(w, vlc, 2); // P-frame intra: vectors reset to 0

	// Slice 2 (macroblock row 1): motion predictors are reset by the slice
	writeSliceHeader(w, 2, 8);
	writeInterMacroblock(w, vlc, 1, null); // inter without motion -> (0, 0)
	// Next macroblock layer uses increment 2: mb 4 is skipped, mb 5 explicit
	writeInterMacroblock(w, vlc, 2, [
		{code: -1}, {code: 1} // -> (-1, +1)
	]);

	w.startCode(0xb7); // sequence_end_code
	return w.toUint8Array();
}

// Stream B: full_pel_forward = 1, f_code 2, differential wraparound,
// consecutive skipped MBs in one slice.
function buildFullPelWrapStream(JSMpeg) {
	var vlc = makeVlc(JSMpeg);
	var w = new BitWriter();
	writeSequenceHeader(w, 48, 32);

	writePictureHeader(w, 0, 1, false, 0);
	writeSliceHeader(w, 1, 8);
	for (var i = 0; i < 6; i++) {
		writeIntraMacroblock(w, vlc, 1);
	}

	// f_code 2 -> r_size 1, range -32..31 in (pre full_pel) half-pel units
	writePictureHeader(w, 1, 2, true, 2);
	writeSliceHeader(w, 1, 8);

	// code +16 with residual 1 => d = (15<<1)+1+1 = 32; 0+32 wraps to -32,
	// full_pel doubles it to -64. Vertical code +1, residual 0 => +1 -> +2.
	writeInterMacroblock(w, vlc, 1, [
		{code: 16, residual: 1}, {code: 1, residual: 0}
	]);
	// Horizontal code 0 keeps the wrapped predictor -32 -> -64; vertical
	// code -1 brings the predictor back to 0.
	writeInterMacroblock(w, vlc, 1, [
		{code: 0}, {code: -1, residual: 0}
	]);
	// Increment 3 skips macroblocks 2 and 3 (consecutive skips); the skip
	// rule zeroes the motion predictors before copying them.
	writeInterMacroblock(w, vlc, 3, [
		{code: 1, residual: 0}, {code: 0} // -> (+2, 0) after full_pel
	]);
	writeInterMacroblock(w, vlc, 1, null); // no motion coded -> (0, 0)

	w.startCode(0xb7);
	return w.toUint8Array();
}

// Stream C: I, B, D, P — the decoder skips B/D pictures entirely, so the two
// rebuilt reference pictures must get consecutive frameIndex values 0 and 1.
function buildSkippedPictureStream(JSMpeg) {
	var vlc = makeVlc(JSMpeg);
	var w = new BitWriter();
	writeSequenceHeader(w, 48, 32);

	writePictureHeader(w, 0, 1, false, 0);
	writeSliceHeader(w, 1, 8);
	for (var i = 0; i < 6; i++) {
		writeIntraMacroblock(w, vlc, 1);
	}

	// B and D picture headers only; the decoder returns before any slice.
	writePictureHeader(w, 1, 3, false, 0);
	writePictureHeader(w, 2, 4, false, 0);

	writePictureHeader(w, 3, 2, false, 1);
	writeSliceHeader(w, 1, 8);
	writeInterMacroblock(w, vlc, 1, [{code: 0}, {code: 0}]);
	// One increment of 5 skips macroblocks 1..5 in a single macroblock layer
	writeInterMacroblock(w, vlc, 5, [{code: 0}, {code: 0}]);

	w.startCode(0xb7);
	return w.toUint8Array();
}

// Stream D: 40x32 visible size still gives a 3x2 macroblock grid (coded
// width 48). Each picture only carries slice 1 (macroblock row 0) and the
// slice stops after two macroblocks, so the edge column and the whole second
// row stay uncovered (kind 0) instead of being trimmed from the grid.
function buildPartialCoverageStream(JSMpeg) {
	var vlc = makeVlc(JSMpeg);
	var w = new BitWriter();
	writeSequenceHeader(w, 40, 32);

	// I picture: only mb 0 and mb 1 of row 0 are coded
	writePictureHeader(w, 0, 1, false, 0);
	writeSliceHeader(w, 1, 8);
	writeIntraMacroblock(w, vlc, 1);
	writeIntraMacroblock(w, vlc, 1);

	// P picture: mb 0 explicit inter with (+1, +1), mb 1 intra; nothing else
	writePictureHeader(w, 1, 2, false, 1);
	writeSliceHeader(w, 1, 8);
	writeInterMacroblock(w, vlc, 1, [{code: 1}, {code: 1}]);
	writeIntraMacroblock(w, vlc, 2);

	w.startCode(0xb7);
	return w.toUint8Array();
}

function makeVlc(JSMpeg) {
	var M = JSMpeg.Decoder.MPEG1Video;
	return {
		address: buildVlcMap(M.MACROBLOCK_ADDRESS_INCREMENT),
		mbIntra: buildVlcMap(M.MACROBLOCK_TYPE_INTRA),
		mbPredictive: buildVlcMap(M.MACROBLOCK_TYPE_PREDICTIVE),
		motion: buildVlcMap(M.MOTION),
		cbp: buildVlcMap(M.CODE_BLOCK_PATTERN),
		dcLuma: buildVlcMap(M.DCT_DC_SIZE_LUMINANCE),
		dcChroma: buildVlcMap(M.DCT_DC_SIZE_CHROMINANCE),
		escape: buildVlcMap(M.DCT_COEFF).get(0xffff),
		// EOB is DCT_COEFF value 0x0001 followed by a 0 bit
		endOfBlock: buildVlcMap(M.DCT_COEFF).get(0x0001) + '0'
	};
}

module.exports = {
	buildVlcMap: buildVlcMap,
	buildHalfPelStream: buildHalfPelStream,
	buildFullPelWrapStream: buildFullPelWrapStream,
	buildSkippedPictureStream: buildSkippedPictureStream,
	buildPartialCoverageStream: buildPartialCoverageStream
};
