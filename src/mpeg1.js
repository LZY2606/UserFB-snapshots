JSMpeg.Decoder.MPEG1Video = (function(){ "use strict";

// Inspired by Java MPEG-1 Video Decoder and Player by Zoltan Korandi 
// https://sourceforge.net/projects/javampeg1video/

var MPEG1 = function(options) {
	JSMpeg.Decoder.Base.call(this, options);

	this.onDecodeCallback = options.onVideoDecode;
	this.incremental = !!options.incremental;

	// In incremental mode the BitBuffer runs in EVICT mode with a fixed,
	// small backing store; consumed prefixes are reclaimed instead of
	// accumulating a whole-picture copy.
	var bufferSize = this.incremental
		? MPEG1.INCREMENTAL_BUFFER_SIZE
		: (options.videoBufferSize || 512*1024);
	var bufferMode = (this.incremental || options.streaming)
		? JSMpeg.BitBuffer.MODE.EVICT
		: JSMpeg.BitBuffer.MODE.EXPAND;

	this.bits = new JSMpeg.BitBuffer(bufferSize, bufferMode);

	this.customIntraQuantMatrix = new Uint8Array(64);
	this.customNonIntraQuantMatrix = new Uint8Array(64);
	this.blockData = new Int32Array(64);

	this.currentFrame = 0;
	this.decodeFirstFrame = options.decodeFirstFrame !== false;

	if (this.incremental) {
		// No per-chunk timestamp accumulation; the first pts is the origin.
		this.collectTimestamps = false;

		// Incremental decoding state
		this.incFinished = false;
		this.incTimeInitialized = false;
		this.incPictureActive = false;
		this.incPictureLocated = false;
		this.incHeaderBaseDone = false;
		this.incSkipBody = false;
		this.incAfterPictureHeader = false;
		this.incScanningForSlice = false;
		this.incInSlice = false;
		this.incPictureType = 0;
		this.incMacroblocks = 0;
		this.incPhase = MPEG1.INC_PHASE.IDLE;
		this.incCheckpoint = 0;
		this.incCbp = 0;
		this.incBlock = 0;
		this.incMask = 0x20;
		this.sliceBegin = false;
	}
};

// Fixed backing store for incremental mode. Callers write at most 1024
// bytes at a time and drain decode() to false, so the retained compressed
// data (a partial start code, the unfinished syntax and a little slack)
// always fits well within this capacity.
MPEG1.INCREMENTAL_BUFFER_SIZE = 16384;

// Status codes for the incremental picture/slice state machine
MPEG1.INC_NEED_MORE = -1;
MPEG1.INC_SKIP_PICTURE = -2;
MPEG1.INC_PICTURE_READY = 0;
MPEG1.INC_NO_PREFIX = -3;
MPEG1.INC_NOT_A_START_CODE = -4;

// Checkpoints for resuming a half-parsed macroblock/block without replaying
// already committed work.
MPEG1.INC_PHASE = {
	IDLE:         0, // between macroblocks, reading the address VLC
	SKIPS_DONE:   1, // address applied (skipped MBs built once), MB type next
	TYPE_DONE:    2, // MB type parsed, quant/motion/cbp not done yet
	MOTION_DONE:  3, // motion vectors parsed, prediction copy pending
	COPY_DONE:    4  // prediction written, block loop in progress
};

MPEG1.prototype = Object.create(JSMpeg.Decoder.Base.prototype);
MPEG1.prototype.constructor = MPEG1;

MPEG1.prototype.write = function(pts, buffers) {
	if (this.incremental) {
		return this.incWrite(pts, buffers);
	}

	JSMpeg.Decoder.Base.prototype.write.call(this, pts, buffers);

	if (!this.hasSequenceHeader) {
		if (this.bits.findStartCode(MPEG1.START.SEQUENCE) === -1) {
			return false;
		}
		this.decodeSequenceHeader();

		if (this.decodeFirstFrame) {
			this.decode();
		}
	}
};

MPEG1.prototype.decode = function() {
	if (this.incremental) {
		return this.incDecode();
	}

	var startTime = JSMpeg.Now();

	if (!this.hasSequenceHeader) {
		return false;
	}

	if (this.bits.findStartCode(MPEG1.START.PICTURE) === -1) {
		var bufferedBytes = this.bits.byteLength - (this.bits.index >> 3);
		return false;
	}

	this.decodePicture();
	this.advanceDecodedTime(1/this.frameRate);

	var elapsedTime = JSMpeg.Now() - startTime;
	if (this.onDecodeCallback) {
		this.onDecodeCallback(this, elapsedTime);
	}
	return true;
};

// Marks the incremental input as ended. Does not render anything by
// itself; a following decode() may still complete the last picture if its
// final slice is already buffered. Repeated calls have no effect.
MPEG1.prototype.finish = function() {
	if (!this.incremental) {
		return;
	}
	this.incFinished = true;
};

// Progress of the picture currently being assembled as a plain object.
// pictureType: 1 (I) or 2 (P) while a picture is open, 0 between pictures.
// macroblocks: fully reconstructed MBs of the current picture (incl. skipped).
// bufferedBytes: compressed bytes still retained, including bytes the
// suspended parser may have to re-read.
MPEG1.prototype.getDecodeProgress = function() {
	if (!this.incremental) {
		return {pictureType: 0, macroblocks: 0, bufferedBytes: 0};
	}
	return {
		pictureType: this.incPictureActive ? this.incPictureType : 0,
		macroblocks: this.incMacroblocks,
		bufferedBytes: this.bits.byteLength - (this.bits.index >> 3)
	};
};


// Incremental entry point and resumable decode machinery

MPEG1.prototype.incWrite = function(pts, buffers) {
	if (this.incFinished) {
		throw new Error('JSMpeg: write() after finish() on incremental decoder');
	}

	// The first pts anchors the timeline; later chunks carry no new time.
	if (!this.incTimeInitialized) {
		this.startTime = pts;
		this.decodedTime = pts;
		this.incTimeInitialized = true;
	}

	this.bytesWritten += this.bits.write(buffers);
	this.canPlay = true;

	if (!this.hasSequenceHeader) {
		if (!this.incFindSequenceHeader()) {
			return false;
		}
		this.decodeSequenceHeader();
	}
	return true;
};

// Finds the sequence header while keeping any trailing 00 00 01 prefix that
// may be completed by the next chunk.
MPEG1.prototype.incFindSequenceHeader = function() {
	var bits = this.bits;
	var i = bits.index >> 3;
	var bytes = bits.bytes;
	var limit = bits.byteLength;
	while (i + 3 < limit) {
		if (
			bytes[i] === 0x00 &&
			bytes[i+1] === 0x00 &&
			bytes[i+2] === 0x01 &&
			bytes[i+3] === MPEG1.START.SEQUENCE
		) {
			// The fixed-size part of the sequence header (including the two
			// quant-matrix flags) is 65 bits = 9 bytes.
			if (limit - (i + 4) < 9) {
				bits.index = i << 3;
				return false;
			}
			bits.index = (i + 4) << 3;
			return true;
		}
		i++;
	}
	bits.index = Math.max(bits.index, (limit - 3) << 3);
	return false;
};

MPEG1.prototype.incDecode = function() {
	if (!this.hasSequenceHeader) {
		return false;
	}

	var startTime = JSMpeg.Now();
	var completed = false;

	while (true) {
		if (this.incSkipBody) {
			// Picture header of an unsupported type was consumed; skip the
			// body up to the next start code.
			if (!this.incSkipToNextStart()) {
				break;
			}
			this.incSkipBody = false;
		}

		if (!this.incPictureActive) {
			// The picture start code is located at most once: a truncated
			// picture header must not make us scan past it on retry.
			if (!this.incPictureLocated && !this.incFindPictureStart()) {
				break;
			}
			this.incPictureLocated = true;
			var status = this.incBeginPicture();
			if (status === MPEG1.INC_NEED_MORE) {
				break;
			}
			if (status === MPEG1.INC_SKIP_PICTURE) {
				this.incPictureLocated = false;
				this.incHeaderBaseDone = false;
				this.incAfterPictureHeader = false;
				this.incSkipBody = true;
				continue;
			}
		}

		if (!this.incDecodeSlices()) {
			break;
		}

		// Whole picture assembled: publish exactly once.
		this.incPublishPicture();
		this.advanceDecodedTime(1/this.frameRate);
		completed = true;

		var elapsedTime = JSMpeg.Now() - startTime;
		if (this.onDecodeCallback) {
			this.onDecodeCallback(this, elapsedTime);
		}
	}

	return completed;
};

// Locates the next picture start code in the retained bytes. A code split
// across chunks (00 00 01 ...) stays buffered until its fourth byte arrives.
MPEG1.prototype.incFindPictureStart = function() {
	var i = this.bits.index >> 3;
	var bytes = this.bits.bytes;
	var limit = this.bits.byteLength;
	while (i + 3 < limit) {
		if (
			bytes[i] === 0x00 &&
			bytes[i+1] === 0x00 &&
			bytes[i+2] === 0x01 &&
			bytes[i+3] === MPEG1.START.PICTURE
		) {
			this.bits.index = (i + 4) << 3;
			return true;
		}
		i++;
	}
	// Park at the last three bytes: a start code prefix may straddle the
	// chunk boundary, so those bytes must be retained.
	this.bits.index = Math.max(this.bits.index, (limit - 3) << 3);
	return false;
};

MPEG1.prototype.incBeginPicture = function() {
	var bits = this.bits;

	if (!this.incHeaderBaseDone) {
		if (!bits.has(10 + 3 + 16)) {
			return MPEG1.INC_NEED_MORE;
		}

		this.currentFrame++;
		bits.skip(10); // temporal_reference
		this.pictureType = bits.read(3);
		bits.skip(16); // vbv_delay

		// Unsupported picture types (B/D) are not part of the incremental
		// contract; the caller skips their bodies.
		if (this.pictureType <= 0 || this.pictureType >= MPEG1.PICTURE_TYPE.B) {
			this.pictureType = 0;
			return MPEG1.INC_SKIP_PICTURE;
		}
		this.incHeaderBaseDone = true;
	}

	if (!this.incAfterPictureHeader) {
		if (this.pictureType === MPEG1.PICTURE_TYPE.PREDICTIVE) {
			if (!bits.has(1 + 3)) {
				return MPEG1.INC_NEED_MORE;
			}
			this.fullPelForward = bits.read(1);
			this.forwardFCode = bits.read(3);
			if (this.forwardFCode === 0) {
				// Ignore picture with zero forward_f_code
				this.pictureType = 0;
				return MPEG1.INC_SKIP_PICTURE;
			}
			this.forwardRSize = this.forwardFCode - 1;
			this.forwardF = 1 << this.forwardRSize;
		}
		this.incAfterPictureHeader = true;
		this.incScanningForSlice = false;
	}

	// Skip extension/user_data start codes between the picture header and
	// the first slice, surviving truncated chunks.
	while (true) {
		if (this.incScanningForSlice) {
			var found = this.incFindNextStartCode();
			if (found === MPEG1.INC_NO_PREFIX) {
				return MPEG1.INC_NEED_MORE;
			}
			this.incScanningForSlice = false;
		}

		var code = this.incPeekStartCode();
		if (code === MPEG1.INC_NO_PREFIX) {
			return MPEG1.INC_NEED_MORE;
		}
		if (code === MPEG1.START.EXTENSION || code === MPEG1.START.USER_DATA) {
			bits.index += 32;
			this.incScanningForSlice = true;
			continue;
		}
		break;
	}

	this.incAfterPictureHeader = false;
	this.incPictureActive = true;
	this.incPictureType = this.pictureType;
	this.incMacroblocks = 0;
	this.incPhase = MPEG1.INC_PHASE.IDLE;
	this.incCheckpoint = bits.index;
	this.incInSlice = false;
	this.sliceBegin = false;
	return MPEG1.INC_PICTURE_READY;
};

MPEG1.prototype.incSkipToNextStart = function() {
	return this.incFindNextStartCode() !== MPEG1.INC_NO_PREFIX;
};

// Prefix-aware start-code scan for skip paths. On failure parks at the last
// three bytes (a prefix may be completed by the next chunk) and returns
// INC_NO_PREFIX; otherwise returns the byte after the 00 00 01 prefix and
// leaves the index at the start of that four-byte code.
MPEG1.prototype.incFindNextStartCode = function() {
	var bits = this.bits;
	var i = (bits.index + 7) >> 3;
	var bytes = bits.bytes;
	var limit = bits.byteLength;
	while (i + 3 < limit) {
		if (
			bytes[i] === 0x00 &&
			bytes[i+1] === 0x00 &&
			bytes[i+2] === 0x01
		) {
			bits.index = i << 3;
			return bytes[i+3];
		}
		i++;
	}
	bits.index = Math.max(bits.index, (limit - 3) << 3);
	return MPEG1.INC_NO_PREFIX;
};

// Classifies the bytes at the current byte-aligned position:
//  INC_NO_PREFIX: fewer than four bytes buffered, a prefix may still arrive
//  INC_NOT_A_START_CODE: ordinary slice/macroblock payload
//  otherwise: the byte following a 00 00 01 start-code prefix
MPEG1.prototype.incPeekStartCode = function() {
	var i = (this.bits.index + 7) >> 3;
	if (i + 3 >= this.bits.byteLength) {
		return MPEG1.INC_NO_PREFIX;
	}
	var bytes = this.bits.bytes;
	if (
		bytes[i] === 0x00 &&
		bytes[i+1] === 0x00 &&
		bytes[i+2] === 0x01
	) {
		return bytes[i+3];
	}
	return MPEG1.INC_NOT_A_START_CODE;
};

MPEG1.prototype.incDecodeSlices = function() {
	while (true) {
		if (this.incInSlice) {
			// Resume a slice header whose quantizer/extra bits were truncated.
			if (!this.incBeginSlice(0)) {
				return this.incAtCleanBoundary() && this.incFinished;
			}
		}

		var code = this.incPeekStartCode();
		if (code === MPEG1.INC_NO_PREFIX) {
			// No complete start code ahead. If a macroblock is still open,
			// let it consume the remaining bytes first; only a clean boundary
			// (nothing half-parsed) may close the picture after finish().
			if (this.incAtCleanBoundary()) {
				return this.incFinished;
			}
			if (!this.incDecodeMacroblock()) {
				return false;
			}
			continue;
		}

		if (code === MPEG1.INC_NOT_A_START_CODE) {
			if (!this.incDecodeMacroblocks()) {
				return this.incAtCleanBoundary() && this.incFinished;
			}
			continue;
		}

		if (code >= MPEG1.START.SLICE_FIRST && code <= MPEG1.START.SLICE_LAST) {
			if (!this.incBeginSlice(code & 0xff)) {
				return false;
			}
			if (!this.incDecodeMacroblocks()) {
				return this.incAtCleanBoundary() && this.incFinished;
			}
			continue;
		}

		// Next picture, sequence end or another code ends the picture;
		// leave the 00 00 01 xx in place for the next decode() round.
		return true;
	}
};

// True only when no slice header or macroblock is half-parsed, so finish()
// can legitimately close the picture at the actual input boundary.
MPEG1.prototype.incAtCleanBoundary = function() {
	return !this.incInSlice &&
		this.incPhase === MPEG1.INC_PHASE.IDLE;
};

MPEG1.prototype.incBeginSlice = function(slice) {
	var bits = this.bits;

	if (!this.incInSlice) {
		// Re-anchor to the byte-aligned start code: the preceding picture or
		// macroblock syntax may have left the bit index mid-byte, so adding
		// 32 bits directly would consume the code at a wrong offset.
		var startByte = (bits.index + 7) >> 3;

		// Need the full start code plus quantizer_scale and extra_bit_slice.
		if (!bits.has(((startByte << 3) - bits.index) + 32 + 5 + 1)) {
			return false;
		}
		bits.index = (startByte + 4) << 3; // consume 00 00 01 xx
		this.incInSlice = true;

		this.sliceBegin = true;
		this.macroblockAddress = (slice - 1) * this.mbWidth - 1;

		// Reset motion vectors and DC predictors at every slice boundary
		this.motionFwH = this.motionFwHPrev = 0;
		this.motionFwV = this.motionFwVPrev = 0;
		this.dcPredictorY  = 128;
		this.dcPredictorCr = 128;
		this.dcPredictorCb = 128;
	}

	if (!bits.has(5)) {
		return false;
	}
	this.quantizerScale = bits.read(5);

	// extra_bit_slice / extra_information_slice
	while (true) {
		if (!bits.has(1)) {
			return false;
		}
		if (bits.read(1) === 0) {
			break;
		}
		if (!bits.has(8)) {
			return false;
		}
		bits.skip(8);
	}

	this.incInSlice = false;
	this.incPhase = MPEG1.INC_PHASE.IDLE;
	this.incCheckpoint = bits.index;
	return true;
};

// Drives macroblocks until a start code is reached or bytes run out.
MPEG1.prototype.incDecodeMacroblocks = function() {
	while (true) {
		var code = this.incPeekStartCode();
		if (code === MPEG1.INC_NO_PREFIX) {
			return false;
		}
		if (code !== MPEG1.INC_NOT_A_START_CODE) {
			// A real start code: this slice is finished.
			return true;
		}

		if (!this.incDecodeMacroblock()) {
			return false;
		}
	}
};

MPEG1.prototype.incDecodeMacroblock = function() {
	var bits = this.bits;
	var INC = MPEG1.INC_PHASE;

	if (this.incPhase === INC.IDLE) {
		// Parse macroblock_address_increment from a resume point taken
		// before any of its VLC bits; truncated VLCs roll back to it.
		this.incCheckpoint = bits.index;
		var increment = 0;
		var t;
		while (true) {
			t = this.incReadHuffman(MPEG1.MACROBLOCK_ADDRESS_INCREMENT);
			if (t === null) {
				bits.index = this.incCheckpoint;
				return false;
			}
			if (t === 34) { // macroblock_stuffing
				continue;
			}
			break;
		}
		while (t === 35) { // macroblock_escape
			increment += 33;
			var again = this.incReadHuffman(MPEG1.MACROBLOCK_ADDRESS_INCREMENT);
			if (again === null) {
				bits.index = this.incCheckpoint;
				return false;
			}
			t = again;
		}
		increment += t;

		// Apply the increment once, including every skipped macroblock.
		// The phase switch below guarantees this whole block (and thus the
		// skipped-MB reconstruction) never runs again on resume.
		if (this.sliceBegin) {
			// The first increment of each slice is relative to the start of
			// the previous row, not the previous macroblock.
			this.sliceBegin = false;
			this.macroblockAddress += increment;
		}
		else {
			if (this.macroblockAddress + increment >= this.mbSize) {
				// Illegal (too large) macroblock_address_increment
				this.incPhase = INC.IDLE;
				return true;
			}
			if (increment > 1) {
				// Skipped macroblocks reset DC predictors
				this.dcPredictorY  = 128;
				this.dcPredictorCr = 128;
				this.dcPredictorCb = 128;

				// Skipped macroblocks in P-pictures reset motion vectors
				if (this.pictureType === MPEG1.PICTURE_TYPE.PREDICTIVE) {
					this.motionFwH = this.motionFwHPrev = 0;
					this.motionFwV = this.motionFwVPrev = 0;
				}
			}
			while (increment > 1) {
				this.macroblockAddress++;
				this.mbRow = (this.macroblockAddress / this.mbWidth)|0;
				this.mbCol = this.macroblockAddress % this.mbWidth;
				this.copyMacroblock(
					this.motionFwH, this.motionFwV,
					this.forwardY, this.forwardCr, this.forwardCb
				);
				this.incMacroblocks++;
				increment--;
			}
			this.macroblockAddress++;
		}
		this.mbRow = (this.macroblockAddress / this.mbWidth)|0;
		this.mbCol = this.macroblockAddress % this.mbWidth;

		this.incPhase = INC.SKIPS_DONE;
	}

	if (this.incPhase === INC.SKIPS_DONE) {
		// Explicit macroblock header
		this.incCheckpoint = bits.index;
		var mbType = this.incReadHuffman(MPEG1.MACROBLOCK_TYPE[this.pictureType]);
		if (mbType === null) {
			bits.index = this.incCheckpoint;
			return false;
		}
		this.macroblockType = mbType;
		this.macroblockIntra = (mbType & 0x01);
		this.macroblockMotFw = (mbType & 0x08);
		this.incPhase = INC.TYPE_DONE;
	}

	if (this.incPhase === INC.TYPE_DONE) {
		// Quantizer scale
		if ((this.macroblockType & 0x10) !== 0) {
			if (!bits.has(5)) {
				return false;
			}
			this.quantizerScale = bits.read(5);
		}

		if (this.macroblockIntra) {
			// Intra-coded macroblocks reset motion vectors
			this.motionFwH = this.motionFwHPrev = 0;
			this.motionFwV = this.motionFwVPrev = 0;
		}
		else {
			// Non-intra macroblocks reset DC predictors
			this.dcPredictorY = 128;
			this.dcPredictorCr = 128;
			this.dcPredictorCb = 128;

			this.incCheckpoint = bits.index;
			if (!this.incDecodeMotionVectors()) {
				bits.index = this.incCheckpoint;
				return false;
			}
		}
		this.incPhase = INC.MOTION_DONE;
	}

	if (this.incPhase === INC.MOTION_DONE) {
		// Resolve the coded block pattern before touching the planes, so a
		// truncated CBP VLC cannot cause a double prediction copy on resume.
		var cbp;
		if ((this.macroblockType & 0x02) !== 0) {
			this.incCheckpoint = bits.index;
			cbp = this.incReadHuffman(MPEG1.CODE_BLOCK_PATTERN);
			if (cbp === null) {
				bits.index = this.incCheckpoint;
				return false;
			}
		}
		else {
			cbp = this.macroblockIntra ? 0x3f : 0;
		}

		if (!this.macroblockIntra) {
			this.copyMacroblock(
				this.motionFwH, this.motionFwV,
				this.forwardY, this.forwardCr, this.forwardCb
			);
		}

		this.incCbp = cbp;
		this.incBlock = 0;
		this.incMask = 0x20;
		this.incPhase = INC.COPY_DONE;
	}

	if (this.incPhase === INC.COPY_DONE) {
		while (this.incBlock < 6) {
			if ((this.incCbp & this.incMask) !== 0) {
				this.incCheckpoint = bits.index;
				if (!this.incDecodeBlock(this.incBlock)) {
					// Roll the bitstream back to the block start; the block
					// is rebuilt from scratch on the next decode() call.
					bits.index = this.incCheckpoint;
					return false;
				}
			}
			this.incBlock++;
			this.incMask >>= 1;
		}
		this.incMacroblocks++;
		this.incPhase = INC.IDLE;
	}

	return true;
};

// Huffman read that fails cleanly on truncated input instead of walking off
// the buffer. Returns null when not enough bits are buffered; the caller is
// responsible for restoring the saved checkpoint.
MPEG1.prototype.incReadHuffman = function(codeTable) {
	var state = 0;
	var bits = this.bits;
	do {
		if (!bits.has(1)) {
			return null;
		}
		state = codeTable[state + bits.read(1)];
		if (state < 0) {
			return null;
		}
	} while (codeTable[state] !== 0);
	return codeTable[state+2];
};

MPEG1.prototype.incDecodeMotionVectors = function() {
	var code, d, r = 0;

	// Compute into locals and commit only once the full syntax is present,
	// so a truncated residual cannot leave an applied delta behind.
	var newHPrev = this.motionFwHPrev;
	var newVPrev = this.motionFwVPrev;

	if (this.macroblockMotFw) {
		// Horizontal forward
		code = this.incReadHuffman(MPEG1.MOTION);
		if (code === null) {
			return false;
		}
		if ((code !== 0) && (this.forwardF !== 1)) {
			if (!this.bits.has(this.forwardRSize)) {
				return false;
			}
			r = this.bits.read(this.forwardRSize);
			d = ((Math.abs(code) - 1) << this.forwardRSize) + r + 1;
			if (code < 0) {
				d = -d;
			}
		}
		else {
			d = code;
		}

		newHPrev += d;
		if (newHPrev > (this.forwardF << 4) - 1) {
			newHPrev -= this.forwardF << 5;
		}
		else if (newHPrev < ((-this.forwardF) << 4)) {
			newHPrev += this.forwardF << 5;
		}

		// Vertical forward
		code = this.incReadHuffman(MPEG1.MOTION);
		if (code === null) {
			return false;
		}
		if ((code !== 0) && (this.forwardF !== 1)) {
			if (!this.bits.has(this.forwardRSize)) {
				return false;
			}
			r = this.bits.read(this.forwardRSize);
			d = ((Math.abs(code) - 1) << this.forwardRSize) + r + 1;
			if (code < 0) {
				d = -d;
			}
		}
		else {
			d = code;
		}

		newVPrev += d;
		if (newVPrev > (this.forwardF << 4) - 1) {
			newVPrev -= this.forwardF << 5;
		}
		else if (newVPrev < ((-this.forwardF) << 4)) {
			newVPrev += this.forwardF << 5;
		}

		this.motionFwHPrev = newHPrev;
		this.motionFwVPrev = newVPrev;
		this.motionFwH = this.fullPelForward ? newHPrev << 1 : newHPrev;
		this.motionFwV = this.fullPelForward ? newVPrev << 1 : newVPrev;
	}
	else if (this.pictureType === MPEG1.PICTURE_TYPE.PREDICTIVE) {
		// No motion information in P-picture, reset vectors
		this.motionFwH = this.motionFwHPrev = 0;
		this.motionFwV = this.motionFwVPrev = 0;
	}
	return true;
};

// Resumable block decode. On entry the bit position is either the start of
// the block (fresh call) or the saved checkpoint (resume). blockData holds
// only this one block's partial coefficients; planes stay untouched until
// the whole block is known to be complete, so IDCT and pixel writes run
// exactly once per block.
MPEG1.prototype.incDecodeBlock = function(block) {
	var bits = this.bits;
	var n = 0, quantMatrix;
	var newDcPredictor = 0;

	JSMpeg.Fill(this.blockData, 0);

	if (this.macroblockIntra) {
		var predictor, dctSize;

		// DC prediction

		if (block < 4) {
			predictor = this.dcPredictorY;
			dctSize = this.incReadHuffman(MPEG1.DCT_DC_SIZE_LUMINANCE);
		}
		else {
			predictor = (block === 4 ? this.dcPredictorCr : this.dcPredictorCb);
			dctSize = this.incReadHuffman(MPEG1.DCT_DC_SIZE_CHROMINANCE);
		}
		if (dctSize === null) {
			return false;
		}

		// Read DC coeff
		if (dctSize > 0) {
			if (!bits.has(dctSize)) {
				return false;
			}
			var differential = bits.read(dctSize);
			if ((differential & (1 << (dctSize - 1))) !== 0) {
				newDcPredictor = predictor + differential;
			}
			else {
				newDcPredictor = predictor + ((-1 << dctSize)|(differential+1));
			}
		}
		else {
			newDcPredictor = predictor;
		}

		// Keep the slice predictor in a local until the whole block is known
		// to be complete; committing early would double-apply the differential
		// if a later coefficient turns out to be truncated.
		this.blockData[0] = newDcPredictor << (3 + 5);
		quantMatrix = this.intraQuantMatrix;
		n = 1;
	}
	else {
		quantMatrix = this.nonIntraQuantMatrix;
	}

	// Decode AC coefficients (+DC for non-intra)
	var level = 0;
	while (true) {
		var
			run = 0,
			coeff = this.incReadHuffman(MPEG1.DCT_COEFF);

		if (coeff === null) {
			return false;
		}

		if ((coeff === 0x0001) && (n > 0)) {
			if (!bits.has(1)) {
				return false;
			}
			if (bits.read(1) === 0) {
				// end_of_block
				break;
			}
			// Otherwise run=0, level=1, fall through
		}

		if (coeff === 0xffff) {
			// escape
			if (!bits.has(6 + 8)) {
				return false;
			}
			run = bits.read(6);
			level = bits.read(8);
			if (level === 0) {
				if (!bits.has(8)) {
					return false;
				}
				level = bits.read(8);
			}
			else if (level === 128) {
				if (!bits.has(8)) {
					return false;
				}
				level = bits.read(8) - 256;
			}
			else if (level > 128) {
				level = level - 256;
			}
		}
		else {
			run = coeff >> 8;
			level = coeff & 0xff;
			if (!bits.has(1)) {
				return false;
			}
			if (bits.read(1)) {
				level = -level;
			}
		}

		n += run;
		var dezigZagged = MPEG1.ZIG_ZAG[n];
		n++;

		// Dequantize, oddify, clip
		level <<= 1;
		if (!this.macroblockIntra) {
			level += (level < 0 ? -1 : 1);
		}
		level = (level * this.quantizerScale * quantMatrix[dezigZagged]) >> 4;
		if ((level & 1) === 0) {
			level -= level > 0 ? 1 : -1;
		}
		if (level > 2047) {
			level = 2047;
		}
		else if (level < -2048) {
			level = -2048;
		}

		// Save premultiplied coefficient
		this.blockData[dezigZagged] = level * MPEG1.PREMULTIPLIER_MATRIX[dezigZagged];
	}

	// The whole coefficient sequence parsed: commit the DC predictor once.
	if (this.macroblockIntra) {
		if (block < 4) {
			this.dcPredictorY = newDcPredictor;
		}
		else if (block === 4) {
			this.dcPredictorCr = newDcPredictor;
		}
		else {
			this.dcPredictorCb = newDcPredictor;
		}
	}

	// Move block to its place
	var
		destArray,
		destIndex,
		scan;

	if (block < 4) {
		destArray = this.currentY;
		scan = this.codedWidth - 8;
		destIndex = (this.mbRow * this.codedWidth + this.mbCol) << 4;
		if ((block & 1) !== 0) {
			destIndex += 8;
		}
		if ((block & 2) !== 0) {
			destIndex += this.codedWidth << 3;
		}
	}
	else {
		destArray = (block === 4) ? this.currentCb : this.currentCr;
		scan = (this.codedWidth >> 1) - 8;
		destIndex = ((this.mbRow * this.codedWidth) << 2) + (this.mbCol << 3);
	}

	if (this.macroblockIntra) {
		// Overwrite (no prediction)
		if (n === 1) {
			MPEG1.CopyValueToDestination((this.blockData[0] + 128) >> 8, destArray, destIndex, scan);
			this.blockData[0] = 0;
		}
		else {
			MPEG1.IDCT(this.blockData);
			MPEG1.CopyBlockToDestination(this.blockData, destArray, destIndex, scan);
			JSMpeg.Fill(this.blockData, 0);
		}
	}
	else {
		// Add data to the predicted macroblock
		if (n === 1) {
			MPEG1.AddValueToDestination((this.blockData[0] + 128) >> 8, destArray, destIndex, scan);
			this.blockData[0] = 0;
		}
		else {
			MPEG1.IDCT(this.blockData);
			MPEG1.AddBlockToDestination(this.blockData, destArray, destIndex, scan);
			JSMpeg.Fill(this.blockData, 0);
		}
	}

	return true;
};

MPEG1.prototype.incPublishPicture = function() {
	if (this.destination) {
		this.destination.render(this.currentY, this.currentCr, this.currentCb, true);
	}

	// If this is a reference picture then rotate the prediction pointers
	if (
		this.pictureType === MPEG1.PICTURE_TYPE.INTRA ||
		this.pictureType === MPEG1.PICTURE_TYPE.PREDICTIVE
	) {
		var
			tmpY = this.forwardY,
			tmpY32 = this.forwardY32,
			tmpCr = this.forwardCr,
			tmpCr32 = this.forwardCr32,
			tmpCb = this.forwardCb,
			tmpCb32 = this.forwardCb32;

		this.forwardY = this.currentY;
		this.forwardY32 = this.currentY32;
		this.forwardCr = this.currentCr;
		this.forwardCr32 = this.currentCr32;
		this.forwardCb = this.currentCb;
		this.forwardCb32 = this.currentCb32;

		this.currentY = tmpY;
		this.currentY32 = tmpY32;
		this.currentCr = tmpCr;
		this.currentCr32 = tmpCr32;
		this.currentCb = tmpCb;
		this.currentCb32 = tmpCb32;
	}

	this.incPictureActive = false;
	this.incPictureLocated = false;
	this.incHeaderBaseDone = false;
	this.incPictureType = 0;
	this.incMacroblocks = 0;
	this.incPhase = MPEG1.INC_PHASE.IDLE;
	this.incInSlice = false;
	this.incAfterPictureHeader = false;
	this.incScanningForSlice = false;
	this.sliceBegin = false;
	this.pictureType = 0;
};

MPEG1.prototype.readHuffman = function(codeTable) {
	var state = 0;
	do {
		state = codeTable[state + this.bits.read(1)];
	} while (state >= 0 && codeTable[state] !== 0);
	return codeTable[state+2];
};


// Sequence Layer

MPEG1.prototype.frameRate = 30;
MPEG1.prototype.decodeSequenceHeader = function() {
	var newWidth = this.bits.read(12),
		newHeight = this.bits.read(12);

	// skip pixel aspect ratio
	this.bits.skip(4);

	this.frameRate = MPEG1.PICTURE_RATE[this.bits.read(4)];

	// skip bitRate, marker, bufferSize and constrained bit
	this.bits.skip(18 + 1 + 10 + 1);

	if (newWidth !== this.width || newHeight !== this.height) {
		this.width = newWidth;
		this.height = newHeight;

		this.initBuffers();

		if (this.destination) {
			this.destination.resize(newWidth, newHeight);
		}
	}

	if (this.bits.read(1)) { // load custom intra quant matrix?
		for (var i = 0; i < 64; i++) {
			this.customIntraQuantMatrix[MPEG1.ZIG_ZAG[i]] = this.bits.read(8);
		}
		this.intraQuantMatrix = this.customIntraQuantMatrix;
	}

	if (this.bits.read(1)) { // load custom non intra quant matrix?
		for (var i = 0; i < 64; i++) {
			var idx = MPEG1.ZIG_ZAG[i];
			this.customNonIntraQuantMatrix[idx] = this.bits.read(8);
		}
		this.nonIntraQuantMatrix = this.customNonIntraQuantMatrix;
	}

	this.hasSequenceHeader = true;
};

MPEG1.prototype.initBuffers = function() {
	this.intraQuantMatrix = MPEG1.DEFAULT_INTRA_QUANT_MATRIX;
	this.nonIntraQuantMatrix = MPEG1.DEFAULT_NON_INTRA_QUANT_MATRIX;

	this.mbWidth = (this.width + 15) >> 4;
	this.mbHeight = (this.height + 15) >> 4;
	this.mbSize = this.mbWidth * this.mbHeight;

	this.codedWidth = this.mbWidth << 4;
	this.codedHeight = this.mbHeight << 4;
	this.codedSize = this.codedWidth * this.codedHeight;

	this.halfWidth = this.mbWidth << 3;
	this.halfHeight = this.mbHeight << 3;

	// Allocated buffers and resize the canvas
	this.currentY = new Uint8ClampedArray(this.codedSize);
	this.currentY32 = new Uint32Array(this.currentY.buffer);

	this.currentCr = new Uint8ClampedArray(this.codedSize >> 2);
	this.currentCr32 = new Uint32Array(this.currentCr.buffer);

	this.currentCb = new Uint8ClampedArray(this.codedSize >> 2);
	this.currentCb32 = new Uint32Array(this.currentCb.buffer);


	this.forwardY = new Uint8ClampedArray(this.codedSize);
	this.forwardY32 = new Uint32Array(this.forwardY.buffer);

	this.forwardCr = new Uint8ClampedArray(this.codedSize >> 2);
	this.forwardCr32 = new Uint32Array(this.forwardCr.buffer);

	this.forwardCb = new Uint8ClampedArray(this.codedSize >> 2);
	this.forwardCb32 = new Uint32Array(this.forwardCb.buffer);
};


// Picture Layer

MPEG1.prototype.currentY = null;
MPEG1.prototype.currentCr = null;
MPEG1.prototype.currentCb = null;

MPEG1.prototype.pictureType = 0;

// Buffers for motion compensation
MPEG1.prototype.forwardY = null;
MPEG1.prototype.forwardCr = null;
MPEG1.prototype.forwardCb = null;

MPEG1.prototype.fullPelForward = false;
MPEG1.prototype.forwardFCode = 0;
MPEG1.prototype.forwardRSize = 0;
MPEG1.prototype.forwardF = 0;

MPEG1.prototype.decodePicture = function(skipOutput) {
	this.currentFrame++;

	this.bits.skip(10); // skip temporalReference
	this.pictureType = this.bits.read(3);
	this.bits.skip(16); // skip vbv_delay

	// Skip B and D frames or unknown coding type
	if (this.pictureType <= 0 || this.pictureType >= MPEG1.PICTURE_TYPE.B) {
		return;
	}

	// full_pel_forward, forward_f_code
	if (this.pictureType === MPEG1.PICTURE_TYPE.PREDICTIVE) {
		this.fullPelForward = this.bits.read(1);
		this.forwardFCode = this.bits.read(3);
		if (this.forwardFCode === 0) {
			// Ignore picture with zero forward_f_code
			return;
		}
		this.forwardRSize = this.forwardFCode - 1;
		this.forwardF = 1 << this.forwardRSize;
	}

	var code = 0;
	do {
		code = this.bits.findNextStartCode();
	} while (code === MPEG1.START.EXTENSION || code === MPEG1.START.USER_DATA );


	while (code >= MPEG1.START.SLICE_FIRST && code <= MPEG1.START.SLICE_LAST) {
		this.decodeSlice(code & 0x000000FF);
		code = this.bits.findNextStartCode();
	}

	if (code !== -1) {
		// We found the next start code; rewind 32bits and let the main loop
		// handle it.
		this.bits.rewind(32);
	}

	// Invoke decode callbacks
	if (this.destination) {
		this.destination.render(this.currentY, this.currentCr, this.currentCb, true);
	}

	// If this is a reference picutre then rotate the prediction pointers
	if (
		this.pictureType === MPEG1.PICTURE_TYPE.INTRA ||
		this.pictureType === MPEG1.PICTURE_TYPE.PREDICTIVE
	) {
		var
			tmpY = this.forwardY,
			tmpY32 = this.forwardY32,
			tmpCr = this.forwardCr,
			tmpCr32 = this.forwardCr32,
			tmpCb = this.forwardCb,
			tmpCb32 = this.forwardCb32;

		this.forwardY = this.currentY;
		this.forwardY32 = this.currentY32;
		this.forwardCr = this.currentCr;
		this.forwardCr32 = this.currentCr32;
		this.forwardCb = this.currentCb;
		this.forwardCb32 = this.currentCb32;

		this.currentY = tmpY;
		this.currentY32 = tmpY32;
		this.currentCr = tmpCr;
		this.currentCr32 = tmpCr32;
		this.currentCb = tmpCb;
		this.currentCb32 = tmpCb32;
	}
};


// Slice Layer

MPEG1.prototype.quantizerScale = 0;
MPEG1.prototype.sliceBegin = false;

MPEG1.prototype.decodeSlice = function(slice) {
	this.sliceBegin = true;
	this.macroblockAddress = (slice - 1) * this.mbWidth - 1;

	// Reset motion vectors and DC predictors
	this.motionFwH = this.motionFwHPrev = 0;
	this.motionFwV = this.motionFwVPrev = 0;
	this.dcPredictorY  = 128;
	this.dcPredictorCr = 128;
	this.dcPredictorCb = 128;

	this.quantizerScale = this.bits.read(5);

	// skip extra bits
	while (this.bits.read(1)) {
		this.bits.skip(8);
	}

	do {
		this.decodeMacroblock();
	} while (!this.bits.nextBytesAreStartCode());
};


// Macroblock Layer

MPEG1.prototype.macroblockAddress = 0;
MPEG1.prototype.mbRow = 0;
MPEG1.prototype.mbCol = 0;

MPEG1.prototype.macroblockType = 0;
MPEG1.prototype.macroblockIntra = false;
MPEG1.prototype.macroblockMotFw = false;

MPEG1.prototype.motionFwH = 0;
MPEG1.prototype.motionFwV = 0;
MPEG1.prototype.motionFwHPrev = 0;
MPEG1.prototype.motionFwVPrev = 0;

MPEG1.prototype.decodeMacroblock = function() {
	// Decode macroblock_address_increment
	var
		increment = 0,
		t = this.readHuffman(MPEG1.MACROBLOCK_ADDRESS_INCREMENT);

	while (t === 34) {
		// macroblock_stuffing
		t = this.readHuffman(MPEG1.MACROBLOCK_ADDRESS_INCREMENT);
	}
	while (t === 35) {
		// macroblock_escape
		increment += 33;
		t = this.readHuffman(MPEG1.MACROBLOCK_ADDRESS_INCREMENT);
	}
	increment += t;

	// Process any skipped macroblocks
	if (this.sliceBegin) {
		// The first macroblock_address_increment of each slice is relative
		// to beginning of the preverious row, not the preverious macroblock
		this.sliceBegin = false;
		this.macroblockAddress += increment;
	}
	else {
		if (this.macroblockAddress + increment >= this.mbSize) {
			// Illegal (too large) macroblock_address_increment
			return;
		}
		if (increment > 1) {
			// Skipped macroblocks reset DC predictors
			this.dcPredictorY  = 128;
			this.dcPredictorCr = 128;
			this.dcPredictorCb = 128;

			// Skipped macroblocks in P-pictures reset motion vectors
			if (this.pictureType === MPEG1.PICTURE_TYPE.PREDICTIVE) {
				this.motionFwH = this.motionFwHPrev = 0;
				this.motionFwV = this.motionFwVPrev = 0;
			}
		}

		// Predict skipped macroblocks
		while (increment > 1) {
			this.macroblockAddress++;
			this.mbRow = (this.macroblockAddress / this.mbWidth)|0;
			this.mbCol = this.macroblockAddress % this.mbWidth;
			this.copyMacroblock(
				this.motionFwH, this.motionFwV,
				this.forwardY, this.forwardCr, this.forwardCb
			);
			increment--;
		}
		this.macroblockAddress++;
	}
	this.mbRow = (this.macroblockAddress / this.mbWidth)|0;
	this.mbCol = this.macroblockAddress % this.mbWidth;

	// Process the current macroblock
	var mbTable = MPEG1.MACROBLOCK_TYPE[this.pictureType];
	this.macroblockType = this.readHuffman(mbTable);
	this.macroblockIntra = (this.macroblockType & 0x01);
	this.macroblockMotFw = (this.macroblockType & 0x08);

	// Quantizer scale
	if ((this.macroblockType & 0x10) !== 0) {
		this.quantizerScale = this.bits.read(5);
	}

	if (this.macroblockIntra) {
		// Intra-coded macroblocks reset motion vectors
		this.motionFwH = this.motionFwHPrev = 0;
		this.motionFwV = this.motionFwVPrev = 0;
	}
	else {
		// Non-intra macroblocks reset DC predictors
		this.dcPredictorY = 128;
		this.dcPredictorCr = 128;
		this.dcPredictorCb = 128;

		this.decodeMotionVectors();
		this.copyMacroblock(
			this.motionFwH, this.motionFwV,
			this.forwardY, this.forwardCr, this.forwardCb
		);
	}

	// Decode blocks
	var cbp = ((this.macroblockType & 0x02) !== 0)
		? this.readHuffman(MPEG1.CODE_BLOCK_PATTERN)
		: (this.macroblockIntra ? 0x3f : 0);

	for (var block = 0, mask = 0x20; block < 6; block++) {
		if ((cbp & mask) !== 0) {
			this.decodeBlock(block);
		}
		mask >>= 1;
	}
};


MPEG1.prototype.decodeMotionVectors = function() {
	var code, d, r = 0;

	// Forward
	if (this.macroblockMotFw) {
		// Horizontal forward
		code = this.readHuffman(MPEG1.MOTION);
		if ((code !== 0) && (this.forwardF !== 1)) {
			r = this.bits.read(this.forwardRSize);
			d = ((Math.abs(code) - 1) << this.forwardRSize) + r + 1;
			if (code < 0) {
				d = -d;
			}
		}
		else {
			d = code;
		}

		this.motionFwHPrev += d;
		if (this.motionFwHPrev > (this.forwardF << 4) - 1) {
			this.motionFwHPrev -= this.forwardF << 5;
		}
		else if (this.motionFwHPrev < ((-this.forwardF) << 4)) {
			this.motionFwHPrev += this.forwardF << 5;
		}

		this.motionFwH = this.motionFwHPrev;
		if (this.fullPelForward) {
			this.motionFwH <<= 1;
		}

		// Vertical forward
		code = this.readHuffman(MPEG1.MOTION);
		if ((code !== 0) && (this.forwardF !== 1)) {
			r = this.bits.read(this.forwardRSize);
			d = ((Math.abs(code) - 1) << this.forwardRSize) + r + 1;
			if (code < 0) {
				d = -d;
			}
		}
		else {
			d = code;
		}

		this.motionFwVPrev += d;
		if (this.motionFwVPrev > (this.forwardF << 4) - 1) {
			this.motionFwVPrev -= this.forwardF << 5;
		}
		else if (this.motionFwVPrev < ((-this.forwardF) << 4)) {
			this.motionFwVPrev += this.forwardF << 5;
		}

		this.motionFwV = this.motionFwVPrev;
		if (this.fullPelForward) {
			this.motionFwV <<= 1;
		}
	}
	else if (this.pictureType === MPEG1.PICTURE_TYPE.PREDICTIVE) {
		// No motion information in P-picture, reset vectors
		this.motionFwH = this.motionFwHPrev = 0;
		this.motionFwV = this.motionFwVPrev = 0;
	}
};

MPEG1.prototype.copyMacroblock = function(motionH, motionV, sY, sCr, sCb) {
	var
		width, scan,
		H, V, oddH, oddV,
		src, dest, last;

	// We use 32bit writes here
	var dY = this.currentY32,
		dCb = this.currentCb32,
		dCr = this.currentCr32;

	// Luminance
	width = this.codedWidth;
	scan = width - 16;

	H = motionH >> 1;
	V = motionV >> 1;
	oddH = (motionH & 1) === 1;
	oddV = (motionV & 1) === 1;

	src = ((this.mbRow << 4) + V) * width + (this.mbCol << 4) + H;
	dest = (this.mbRow * width + this.mbCol) << 2;
	last = dest + (width << 2);

	var x, y1, y2, y;
	if (oddH) {
		if (oddV) {
			while (dest < last) {
				y1 = sY[src] + sY[src+width]; src++;
				for (x = 0; x < 4; x++) {
					y2 = sY[src] + sY[src+width]; src++;
					y = (((y1 + y2 + 2) >> 2) & 0xff);

					y1 = sY[src] + sY[src+width]; src++;
					y |= (((y1 + y2 + 2) << 6) & 0xff00);

					y2 = sY[src] + sY[src+width]; src++;
					y |= (((y1 + y2 + 2) << 14) & 0xff0000);

					y1 = sY[src] + sY[src+width]; src++;
					y |= (((y1 + y2 + 2) << 22) & 0xff000000);

					dY[dest++] = y;
				}
				dest += scan >> 2; src += scan-1;
			}
		}
		else {
			while (dest < last) {
				y1 = sY[src++];
				for (x = 0; x < 4; x++) {
					y2 = sY[src++];
					y = (((y1 + y2 + 1) >> 1) & 0xff);

					y1 = sY[src++];
					y |= (((y1 + y2 + 1) << 7) & 0xff00);

					y2 = sY[src++];
					y |= (((y1 + y2 + 1) << 15) & 0xff0000);

					y1 = sY[src++];
					y |= (((y1 + y2 + 1) << 23) & 0xff000000);

					dY[dest++] = y;
				}
				dest += scan >> 2; src += scan-1;
			}
		}
	}
	else {
		if (oddV) {
			while (dest < last) {
				for (x = 0; x < 4; x++) {
					y = (((sY[src] + sY[src+width] + 1) >> 1) & 0xff); src++;
					y |= (((sY[src] + sY[src+width] + 1) << 7) & 0xff00); src++;
					y |= (((sY[src] + sY[src+width] + 1) << 15) & 0xff0000); src++;
					y |= (((sY[src] + sY[src+width] + 1) << 23) & 0xff000000); src++;

					dY[dest++] = y;
				}
				dest += scan >> 2; src += scan;
			}
		}
		else {
			while (dest < last) {
				for (x = 0; x < 4; x++) {
					y = sY[src]; src++;
					y |= sY[src] << 8; src++;
					y |= sY[src] << 16; src++;
					y |= sY[src] << 24; src++;

					dY[dest++] = y;
				}
				dest += scan >> 2; src += scan;
			}
		}
	}

	// Chrominance

	width = this.halfWidth;
	scan = width - 8;

	H = (motionH/2) >> 1;
	V = (motionV/2) >> 1;
	oddH = ((motionH/2) & 1) === 1;
	oddV = ((motionV/2) & 1) === 1;

	src = ((this.mbRow << 3) + V) * width + (this.mbCol << 3) + H;
	dest = (this.mbRow * width + this.mbCol) << 1;
	last = dest + (width << 1);

	var cr1, cr2, cr,
		cb1, cb2, cb;
	if (oddH) {
		if (oddV) {
			while (dest < last) {
				cr1 = sCr[src] + sCr[src+width];
				cb1 = sCb[src] + sCb[src+width];
				src++;
				for (x = 0; x < 2; x++) {
					cr2 = sCr[src] + sCr[src+width];
					cb2 = sCb[src] + sCb[src+width]; src++;
					cr = (((cr1 + cr2 + 2) >> 2) & 0xff);
					cb = (((cb1 + cb2 + 2) >> 2) & 0xff);

					cr1 = sCr[src] + sCr[src+width];
					cb1 = sCb[src] + sCb[src+width]; src++;
					cr |= (((cr1 + cr2 + 2) << 6) & 0xff00);
					cb |= (((cb1 + cb2 + 2) << 6) & 0xff00);

					cr2 = sCr[src] + sCr[src+width];
					cb2 = sCb[src] + sCb[src+width]; src++;
					cr |= (((cr1 + cr2 + 2) << 14) & 0xff0000);
					cb |= (((cb1 + cb2 + 2) << 14) & 0xff0000);

					cr1 = sCr[src] + sCr[src+width];
					cb1 = sCb[src] + sCb[src+width]; src++;
					cr |= (((cr1 + cr2 + 2) << 22) & 0xff000000);
					cb |= (((cb1 + cb2 + 2) << 22) & 0xff000000);

					dCr[dest] = cr;
					dCb[dest] = cb;
					dest++;
				}
				dest += scan >> 2; src += scan-1;
			}
		}
		else {
			while (dest < last) {
				cr1 = sCr[src];
				cb1 = sCb[src];
				src++;
				for (x = 0; x < 2; x++) {
					cr2 = sCr[src];
					cb2 = sCb[src++];
					cr = (((cr1 + cr2 + 1) >> 1) & 0xff);
					cb = (((cb1 + cb2 + 1) >> 1) & 0xff);

					cr1 = sCr[src];
					cb1 = sCb[src++];
					cr |= (((cr1 + cr2 + 1) << 7) & 0xff00);
					cb |= (((cb1 + cb2 + 1) << 7) & 0xff00);

					cr2 = sCr[src];
					cb2 = sCb[src++];
					cr |= (((cr1 + cr2 + 1) << 15) & 0xff0000);
					cb |= (((cb1 + cb2 + 1) << 15) & 0xff0000);

					cr1 = sCr[src];
					cb1 = sCb[src++];
					cr |= (((cr1 + cr2 + 1) << 23) & 0xff000000);
					cb |= (((cb1 + cb2 + 1) << 23) & 0xff000000);

					dCr[dest] = cr;
					dCb[dest] = cb;
					dest++;
				}
				dest += scan >> 2; src += scan-1;
			}
		}
	}
	else {
		if (oddV) {
			while (dest < last) {
				for (x = 0; x < 2; x++) {
					cr = (((sCr[src] + sCr[src+width] + 1) >> 1) & 0xff);
					cb = (((sCb[src] + sCb[src+width] + 1) >> 1) & 0xff); src++;

					cr |= (((sCr[src] + sCr[src+width] + 1) << 7) & 0xff00);
					cb |= (((sCb[src] + sCb[src+width] + 1) << 7) & 0xff00); src++;

					cr |= (((sCr[src] + sCr[src+width] + 1) << 15) & 0xff0000);
					cb |= (((sCb[src] + sCb[src+width] + 1) << 15) & 0xff0000); src++;

					cr |= (((sCr[src] + sCr[src+width] + 1) << 23) & 0xff000000);
					cb |= (((sCb[src] + sCb[src+width] + 1) << 23) & 0xff000000); src++;

					dCr[dest] = cr;
					dCb[dest] = cb;
					dest++;
				}
				dest += scan >> 2; src += scan;
			}
		}
		else {
			while (dest < last) {
				for (x = 0; x < 2; x++) {
					cr = sCr[src];
					cb = sCb[src]; src++;

					cr |= sCr[src] << 8;
					cb |= sCb[src] << 8; src++;

					cr |= sCr[src] << 16;
					cb |= sCb[src] << 16; src++;

					cr |= sCr[src] << 24;
					cb |= sCb[src] << 24; src++;

					dCr[dest] = cr;
					dCb[dest] = cb;
					dest++;
				}
				dest += scan >> 2; src += scan;
			}
		}
	}
};


// Block layer

MPEG1.prototype.dcPredictorY = 0;
MPEG1.prototype.dcPredictorCr = 0;
MPEG1.prototype.dcPredictorCb = 0;

MPEG1.prototype.blockData = null;

MPEG1.prototype.decodeBlock = function(block) {

	var
		n = 0,
		quantMatrix;

	// Decode DC coefficient of intra-coded blocks
	if (this.macroblockIntra) {
		var
			predictor,
			dctSize;

		// DC prediction

		if (block < 4) {
			predictor = this.dcPredictorY;
			dctSize = this.readHuffman(MPEG1.DCT_DC_SIZE_LUMINANCE);
		}
		else {
			predictor = (block === 4 ? this.dcPredictorCr : this.dcPredictorCb);
			dctSize = this.readHuffman(MPEG1.DCT_DC_SIZE_CHROMINANCE);
		}

		// Read DC coeff
		if (dctSize > 0) {
			var differential = this.bits.read(dctSize);
			if ((differential & (1 << (dctSize - 1))) !== 0) {
				this.blockData[0] = predictor + differential;
			}
			else {
				this.blockData[0] = predictor + ((-1 << dctSize)|(differential+1));
			}
		}
		else {
			this.blockData[0] = predictor;
		}

		// Save predictor value
		if (block < 4) {
			this.dcPredictorY = this.blockData[0];
		}
		else if (block === 4) {
			this.dcPredictorCr = this.blockData[0];
		}
		else {
			this.dcPredictorCb = this.blockData[0];
		}

		// Dequantize + premultiply
		this.blockData[0] <<= (3 + 5);

		quantMatrix = this.intraQuantMatrix;
		n = 1;
	}
	else {
		quantMatrix = this.nonIntraQuantMatrix;
	}

	// Decode AC coefficients (+DC for non-intra)
	var level = 0;
	while (true) {
		var
			run = 0,
			coeff = this.readHuffman(MPEG1.DCT_COEFF);

		if ((coeff === 0x0001) && (n > 0) && (this.bits.read(1) === 0)) {
			// end_of_block
			break;
		}
		if (coeff === 0xffff) {
			// escape
			run = this.bits.read(6);
			level = this.bits.read(8);
			if (level === 0) {
				level = this.bits.read(8);
			}
			else if (level === 128) {
				level = this.bits.read(8) - 256;
			}
			else if (level > 128) {
				level = level - 256;
			}
		}
		else {
			run = coeff >> 8;
			level = coeff & 0xff;
			if (this.bits.read(1)) {
				level = -level;
			}
		}

		n += run;
		var dezigZagged = MPEG1.ZIG_ZAG[n];
		n++;

		// Dequantize, oddify, clip
		level <<= 1;
		if (!this.macroblockIntra) {
			level += (level < 0 ? -1 : 1);
		}
		level = (level * this.quantizerScale * quantMatrix[dezigZagged]) >> 4;
		if ((level & 1) === 0) {
			level -= level > 0 ? 1 : -1;
		}
		if (level > 2047) {
			level = 2047;
		}
		else if (level < -2048) {
			level = -2048;
		}

		// Save premultiplied coefficient
		this.blockData[dezigZagged] = level * MPEG1.PREMULTIPLIER_MATRIX[dezigZagged];
	}

	// Move block to its place
	var
		destArray,
		destIndex,
		scan;

	if (block < 4) {
		destArray = this.currentY;
		scan = this.codedWidth - 8;
		destIndex = (this.mbRow * this.codedWidth + this.mbCol) << 4;
		if ((block & 1) !== 0) {
			destIndex += 8;
		}
		if ((block & 2) !== 0) {
			destIndex += this.codedWidth << 3;
		}
	}
	else {
		destArray = (block === 4) ? this.currentCb : this.currentCr;
		scan = (this.codedWidth >> 1) - 8;
		destIndex = ((this.mbRow * this.codedWidth) << 2) + (this.mbCol << 3);
	}

	if (this.macroblockIntra) {
		// Overwrite (no prediction)
		if (n === 1) {
			MPEG1.CopyValueToDestination((this.blockData[0] + 128) >> 8, destArray, destIndex, scan);
			this.blockData[0] = 0;
		}
		else {
			MPEG1.IDCT(this.blockData);
			MPEG1.CopyBlockToDestination(this.blockData, destArray, destIndex, scan);
			JSMpeg.Fill(this.blockData, 0);
		}
	}
	else {
		// Add data to the predicted macroblock
		if (n === 1) {
			MPEG1.AddValueToDestination((this.blockData[0] + 128) >> 8, destArray, destIndex, scan);
			this.blockData[0] = 0;
		}
		else {
			MPEG1.IDCT(this.blockData);
			MPEG1.AddBlockToDestination(this.blockData, destArray, destIndex, scan);
			JSMpeg.Fill(this.blockData, 0);
		}
	}

	n = 0;
};

MPEG1.CopyBlockToDestination = function(block, dest, index, scan) {
	for (var n = 0; n < 64; n += 8, index += scan+8) {
		dest[index+0] = block[n+0];
		dest[index+1] = block[n+1];
		dest[index+2] = block[n+2];
		dest[index+3] = block[n+3];
		dest[index+4] = block[n+4];
		dest[index+5] = block[n+5];
		dest[index+6] = block[n+6];
		dest[index+7] = block[n+7];
	}
};

MPEG1.AddBlockToDestination = function(block, dest, index, scan) {
	for (var n = 0; n < 64; n += 8, index += scan+8) {
		dest[index+0] += block[n+0];
		dest[index+1] += block[n+1];
		dest[index+2] += block[n+2];
		dest[index+3] += block[n+3];
		dest[index+4] += block[n+4];
		dest[index+5] += block[n+5];
		dest[index+6] += block[n+6];
		dest[index+7] += block[n+7];
	}
};

MPEG1.CopyValueToDestination = function(value, dest, index, scan) {
	for (var n = 0; n < 64; n += 8, index += scan+8) {
		dest[index+0] = value;
		dest[index+1] = value;
		dest[index+2] = value;
		dest[index+3] = value;
		dest[index+4] = value;
		dest[index+5] = value;
		dest[index+6] = value;
		dest[index+7] = value;
	}
};

MPEG1.AddValueToDestination = function(value, dest, index, scan) {
	for (var n = 0; n < 64; n += 8, index += scan+8) {
		dest[index+0] += value;
		dest[index+1] += value;
		dest[index+2] += value;
		dest[index+3] += value;
		dest[index+4] += value;
		dest[index+5] += value;
		dest[index+6] += value;
		dest[index+7] += value;
	}
};

MPEG1.IDCT = function(block) {
	// See http://vsr.informatik.tu-chemnitz.de/~jan/MPEG/HTML/IDCT.html
	// for more info.

	var
		b1, b3, b4, b6, b7, tmp1, tmp2, m0,
		x0, x1, x2, x3, x4, y3, y4, y5, y6, y7;

	// Transform columns
	for (var i = 0; i < 8; ++i) {
		b1 = block[4*8+i];
		b3 = block[2*8+i] + block[6*8+i];
		b4 = block[5*8+i] - block[3*8+i];
		tmp1 = block[1*8+i] + block[7*8+i];
		tmp2 = block[3*8+i] + block[5*8+i];
		b6 = block[1*8+i] - block[7*8+i];
		b7 = tmp1 + tmp2;
		m0 = block[0*8+i];
		x4 = ((b6*473 - b4*196 + 128) >> 8) - b7;
		x0 = x4 - (((tmp1 - tmp2)*362 + 128) >> 8);
		x1 = m0 - b1;
		x2 = (((block[2*8+i] - block[6*8+i])*362 + 128) >> 8) - b3;
		x3 = m0 + b1;
		y3 = x1 + x2;
		y4 = x3 + b3;
		y5 = x1 - x2;
		y6 = x3 - b3;
		y7 = -x0 - ((b4*473 + b6*196 + 128) >> 8);
		block[0*8+i] = b7 + y4;
		block[1*8+i] = x4 + y3;
		block[2*8+i] = y5 - x0;
		block[3*8+i] = y6 - y7;
		block[4*8+i] = y6 + y7;
		block[5*8+i] = x0 + y5;
		block[6*8+i] = y3 - x4;
		block[7*8+i] = y4 - b7;
	}

	// Transform rows
	for (var i = 0; i < 64; i += 8) {
		b1 = block[4+i];
		b3 = block[2+i] + block[6+i];
		b4 = block[5+i] - block[3+i];
		tmp1 = block[1+i] + block[7+i];
		tmp2 = block[3+i] + block[5+i];
		b6 = block[1+i] - block[7+i];
		b7 = tmp1 + tmp2;
		m0 = block[0+i];
		x4 = ((b6*473 - b4*196 + 128) >> 8) - b7;
		x0 = x4 - (((tmp1 - tmp2)*362 + 128) >> 8);
		x1 = m0 - b1;
		x2 = (((block[2+i] - block[6+i])*362 + 128) >> 8) - b3;
		x3 = m0 + b1;
		y3 = x1 + x2;
		y4 = x3 + b3;
		y5 = x1 - x2;
		y6 = x3 - b3;
		y7 = -x0 - ((b4*473 + b6*196 + 128) >> 8);
		block[0+i] = (b7 + y4 + 128) >> 8;
		block[1+i] = (x4 + y3 + 128) >> 8;
		block[2+i] = (y5 - x0 + 128) >> 8;
		block[3+i] = (y6 - y7 + 128) >> 8;
		block[4+i] = (y6 + y7 + 128) >> 8;
		block[5+i] = (x0 + y5 + 128) >> 8;
		block[6+i] = (y3 - x4 + 128) >> 8;
		block[7+i] = (y4 - b7 + 128) >> 8;
	}
};


// VLC Tables and Constants

MPEG1.PICTURE_RATE = [
	0.000, 23.976, 24.000, 25.000, 29.970, 30.000, 50.000, 59.940,
	60.000, 0.000, 0.000, 0.000, 0.000, 0.000, 0.000, 0.000
];

MPEG1.ZIG_ZAG = new Uint8Array([
	 0,  1,  8, 16,  9,  2,  3, 10,
	17, 24, 32, 25, 18, 11,  4,  5,
	12, 19, 26, 33, 40, 48, 41, 34,
	27, 20, 13,  6,  7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36,
	29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46,
	53, 60, 61, 54, 47, 55, 62, 63
]);

MPEG1.DEFAULT_INTRA_QUANT_MATRIX = new Uint8Array([
	 8, 16, 19, 22, 26, 27, 29, 34,
	16, 16, 22, 24, 27, 29, 34, 37,
	19, 22, 26, 27, 29, 34, 34, 38,
	22, 22, 26, 27, 29, 34, 37, 40,
	22, 26, 27, 29, 32, 35, 40, 48,
	26, 27, 29, 32, 35, 40, 48, 58,
	26, 27, 29, 34, 38, 46, 56, 69,
	27, 29, 35, 38, 46, 56, 69, 83
]);

MPEG1.DEFAULT_NON_INTRA_QUANT_MATRIX = new Uint8Array([
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16
]);

MPEG1.PREMULTIPLIER_MATRIX = new Uint8Array([
	32, 44, 42, 38, 32, 25, 17,  9,
	44, 62, 58, 52, 44, 35, 24, 12,
	42, 58, 55, 49, 42, 33, 23, 12,
	38, 52, 49, 44, 38, 30, 20, 10,
	32, 44, 42, 38, 32, 25, 17,  9,
	25, 35, 33, 30, 25, 20, 14,  7,
	17, 24, 23, 20, 17, 14,  9,  5,
	 9, 12, 12, 10,  9,  7,  5,  2
]);

// MPEG-1 VLC

//  macroblock_stuffing decodes as 34.
//  macroblock_escape decodes as 35.

MPEG1.MACROBLOCK_ADDRESS_INCREMENT = new Int16Array([
	 1*3,  2*3,  0, //   0
	 3*3,  4*3,  0, //   1  0
	   0,    0,  1, //   2  1.
	 5*3,  6*3,  0, //   3  00
	 7*3,  8*3,  0, //   4  01
	 9*3, 10*3,  0, //   5  000
	11*3, 12*3,  0, //   6  001
	   0,    0,  3, //   7  010.
	   0,    0,  2, //   8  011.
	13*3, 14*3,  0, //   9  0000
	15*3, 16*3,  0, //  10  0001
	   0,    0,  5, //  11  0010.
	   0,    0,  4, //  12  0011.
	17*3, 18*3,  0, //  13  0000 0
	19*3, 20*3,  0, //  14  0000 1
	   0,    0,  7, //  15  0001 0.
	   0,    0,  6, //  16  0001 1.
	21*3, 22*3,  0, //  17  0000 00
	23*3, 24*3,  0, //  18  0000 01
	25*3, 26*3,  0, //  19  0000 10
	27*3, 28*3,  0, //  20  0000 11
	  -1, 29*3,  0, //  21  0000 000
	  -1, 30*3,  0, //  22  0000 001
	31*3, 32*3,  0, //  23  0000 010
	33*3, 34*3,  0, //  24  0000 011
	35*3, 36*3,  0, //  25  0000 100
	37*3, 38*3,  0, //  26  0000 101
	   0,    0,  9, //  27  0000 110.
	   0,    0,  8, //  28  0000 111.
	39*3, 40*3,  0, //  29  0000 0001
	41*3, 42*3,  0, //  30  0000 0011
	43*3, 44*3,  0, //  31  0000 0100
	45*3, 46*3,  0, //  32  0000 0101
	   0,    0, 15, //  33  0000 0110.
	   0,    0, 14, //  34  0000 0111.
	   0,    0, 13, //  35  0000 1000.
	   0,    0, 12, //  36  0000 1001.
	   0,    0, 11, //  37  0000 1010.
	   0,    0, 10, //  38  0000 1011.
	47*3,   -1,  0, //  39  0000 0001 0
	  -1, 48*3,  0, //  40  0000 0001 1
	49*3, 50*3,  0, //  41  0000 0011 0
	51*3, 52*3,  0, //  42  0000 0011 1
	53*3, 54*3,  0, //  43  0000 0100 0
	55*3, 56*3,  0, //  44  0000 0100 1
	57*3, 58*3,  0, //  45  0000 0101 0
	59*3, 60*3,  0, //  46  0000 0101 1
	61*3,   -1,  0, //  47  0000 0001 00
	  -1, 62*3,  0, //  48  0000 0001 11
	63*3, 64*3,  0, //  49  0000 0011 00
	65*3, 66*3,  0, //  50  0000 0011 01
	67*3, 68*3,  0, //  51  0000 0011 10
	69*3, 70*3,  0, //  52  0000 0011 11
	71*3, 72*3,  0, //  53  0000 0100 00
	73*3, 74*3,  0, //  54  0000 0100 01
	   0,    0, 21, //  55  0000 0100 10.
	   0,    0, 20, //  56  0000 0100 11.
	   0,    0, 19, //  57  0000 0101 00.
	   0,    0, 18, //  58  0000 0101 01.
	   0,    0, 17, //  59  0000 0101 10.
	   0,    0, 16, //  60  0000 0101 11.
	   0,    0, 35, //  61  0000 0001 000. -- macroblock_escape
	   0,    0, 34, //  62  0000 0001 111. -- macroblock_stuffing
	   0,    0, 33, //  63  0000 0011 000.
	   0,    0, 32, //  64  0000 0011 001.
	   0,    0, 31, //  65  0000 0011 010.
	   0,    0, 30, //  66  0000 0011 011.
	   0,    0, 29, //  67  0000 0011 100.
	   0,    0, 28, //  68  0000 0011 101.
	   0,    0, 27, //  69  0000 0011 110.
	   0,    0, 26, //  70  0000 0011 111.
	   0,    0, 25, //  71  0000 0100 000.
	   0,    0, 24, //  72  0000 0100 001.
	   0,    0, 23, //  73  0000 0100 010.
	   0,    0, 22  //  74  0000 0100 011.
]);

//  macroblock_type bitmap:
//    0x10  macroblock_quant
//    0x08  macroblock_motion_forward
//    0x04  macroblock_motion_backward
//    0x02  macrobkock_pattern
//    0x01  macroblock_intra
//

MPEG1.MACROBLOCK_TYPE_INTRA = new Int8Array([
	 1*3,  2*3,     0, //   0
	  -1,  3*3,     0, //   1  0
	   0,    0,  0x01, //   2  1.
	   0,    0,  0x11  //   3  01.
]);

MPEG1.MACROBLOCK_TYPE_PREDICTIVE = new Int8Array([
	 1*3,  2*3,     0, //  0
	 3*3,  4*3,     0, //  1  0
	   0,    0,  0x0a, //  2  1.
	 5*3,  6*3,     0, //  3  00
	   0,    0,  0x02, //  4  01.
	 7*3,  8*3,     0, //  5  000
	   0,    0,  0x08, //  6  001.
	 9*3, 10*3,     0, //  7  0000
	11*3, 12*3,     0, //  8  0001
	  -1, 13*3,     0, //  9  00000
	   0,    0,  0x12, // 10  00001.
	   0,    0,  0x1a, // 11  00010.
	   0,    0,  0x01, // 12  00011.
	   0,    0,  0x11  // 13  000001.
]);

MPEG1.MACROBLOCK_TYPE_B = new Int8Array([
	 1*3,  2*3,     0,  //  0
	 3*3,  5*3,     0,  //  1  0
	 4*3,  6*3,     0,  //  2  1
	 8*3,  7*3,     0,  //  3  00
	   0,    0,  0x0c,  //  4  10.
	 9*3, 10*3,     0,  //  5  01
	   0,    0,  0x0e,  //  6  11.
	13*3, 14*3,     0,  //  7  001
	12*3, 11*3,     0,  //  8  000
	   0,    0,  0x04,  //  9  010.
	   0,    0,  0x06,  // 10  011.
	18*3, 16*3,     0,  // 11  0001
	15*3, 17*3,     0,  // 12  0000
	   0,    0,  0x08,  // 13  0010.
	   0,    0,  0x0a,  // 14  0011.
	  -1, 19*3,     0,  // 15  00000
	   0,    0,  0x01,  // 16  00011.
	20*3, 21*3,     0,  // 17  00001
	   0,    0,  0x1e,  // 18  00010.
	   0,    0,  0x11,  // 19  000001.
	   0,    0,  0x16,  // 20  000010.
	   0,    0,  0x1a   // 21  000011.
]);

MPEG1.MACROBLOCK_TYPE = [
	null,
	MPEG1.MACROBLOCK_TYPE_INTRA,
	MPEG1.MACROBLOCK_TYPE_PREDICTIVE,
	MPEG1.MACROBLOCK_TYPE_B
];

MPEG1.CODE_BLOCK_PATTERN = new Int16Array([
	  2*3,   1*3,   0,  //   0
	  3*3,   6*3,   0,  //   1  1
	  4*3,   5*3,   0,  //   2  0
	  8*3,  11*3,   0,  //   3  10
	 12*3,  13*3,   0,  //   4  00
	  9*3,   7*3,   0,  //   5  01
	 10*3,  14*3,   0,  //   6  11
	 20*3,  19*3,   0,  //   7  011
	 18*3,  16*3,   0,  //   8  100
	 23*3,  17*3,   0,  //   9  010
	 27*3,  25*3,   0,  //  10  110
	 21*3,  28*3,   0,  //  11  101
	 15*3,  22*3,   0,  //  12  000
	 24*3,  26*3,   0,  //  13  001
	    0,     0,  60,  //  14  111.
	 35*3,  40*3,   0,  //  15  0000
	 44*3,  48*3,   0,  //  16  1001
	 38*3,  36*3,   0,  //  17  0101
	 42*3,  47*3,   0,  //  18  1000
	 29*3,  31*3,   0,  //  19  0111
	 39*3,  32*3,   0,  //  20  0110
	    0,     0,  32,  //  21  1010.
	 45*3,  46*3,   0,  //  22  0001
	 33*3,  41*3,   0,  //  23  0100
	 43*3,  34*3,   0,  //  24  0010
	    0,     0,   4,  //  25  1101.
	 30*3,  37*3,   0,  //  26  0011
	    0,     0,   8,  //  27  1100.
	    0,     0,  16,  //  28  1011.
	    0,     0,  44,  //  29  0111 0.
	 50*3,  56*3,   0,  //  30  0011 0
	    0,     0,  28,  //  31  0111 1.
	    0,     0,  52,  //  32  0110 1.
	    0,     0,  62,  //  33  0100 0.
	 61*3,  59*3,   0,  //  34  0010 1
	 52*3,  60*3,   0,  //  35  0000 0
	    0,     0,   1,  //  36  0101 1.
	 55*3,  54*3,   0,  //  37  0011 1
	    0,     0,  61,  //  38  0101 0.
	    0,     0,  56,  //  39  0110 0.
	 57*3,  58*3,   0,  //  40  0000 1
	    0,     0,   2,  //  41  0100 1.
	    0,     0,  40,  //  42  1000 0.
	 51*3,  62*3,   0,  //  43  0010 0
	    0,     0,  48,  //  44  1001 0.
	 64*3,  63*3,   0,  //  45  0001 0
	 49*3,  53*3,   0,  //  46  0001 1
	    0,     0,  20,  //  47  1000 1.
	    0,     0,  12,  //  48  1001 1.
	 80*3,  83*3,   0,  //  49  0001 10
	    0,     0,  63,  //  50  0011 00.
	 77*3,  75*3,   0,  //  51  0010 00
	 65*3,  73*3,   0,  //  52  0000 00
	 84*3,  66*3,   0,  //  53  0001 11
	    0,     0,  24,  //  54  0011 11.
	    0,     0,  36,  //  55  0011 10.
	    0,     0,   3,  //  56  0011 01.
	 69*3,  87*3,   0,  //  57  0000 10
	 81*3,  79*3,   0,  //  58  0000 11
	 68*3,  71*3,   0,  //  59  0010 11
	 70*3,  78*3,   0,  //  60  0000 01
	 67*3,  76*3,   0,  //  61  0010 10
	 72*3,  74*3,   0,  //  62  0010 01
	 86*3,  85*3,   0,  //  63  0001 01
	 88*3,  82*3,   0,  //  64  0001 00
	   -1,  94*3,   0,  //  65  0000 000
	 95*3,  97*3,   0,  //  66  0001 111
	    0,     0,  33,  //  67  0010 100.
	    0,     0,   9,  //  68  0010 110.
	106*3, 110*3,   0,  //  69  0000 100
	102*3, 116*3,   0,  //  70  0000 010
	    0,     0,   5,  //  71  0010 111.
	    0,     0,  10,  //  72  0010 010.
	 93*3,  89*3,   0,  //  73  0000 001
	    0,     0,   6,  //  74  0010 011.
	    0,     0,  18,  //  75  0010 001.
	    0,     0,  17,  //  76  0010 101.
	    0,     0,  34,  //  77  0010 000.
	113*3, 119*3,   0,  //  78  0000 011
	103*3, 104*3,   0,  //  79  0000 111
	 90*3,  92*3,   0,  //  80  0001 100
	109*3, 107*3,   0,  //  81  0000 110
	117*3, 118*3,   0,  //  82  0001 001
	101*3,  99*3,   0,  //  83  0001 101
	 98*3,  96*3,   0,  //  84  0001 110
	100*3,  91*3,   0,  //  85  0001 011
	114*3, 115*3,   0,  //  86  0001 010
	105*3, 108*3,   0,  //  87  0000 101
	112*3, 111*3,   0,  //  88  0001 000
	121*3, 125*3,   0,  //  89  0000 0011
	    0,     0,  41,  //  90  0001 1000.
	    0,     0,  14,  //  91  0001 0111.
	    0,     0,  21,  //  92  0001 1001.
	124*3, 122*3,   0,  //  93  0000 0010
	120*3, 123*3,   0,  //  94  0000 0001
	    0,     0,  11,  //  95  0001 1110.
	    0,     0,  19,  //  96  0001 1101.
	    0,     0,   7,  //  97  0001 1111.
	    0,     0,  35,  //  98  0001 1100.
	    0,     0,  13,  //  99  0001 1011.
	    0,     0,  50,  // 100  0001 0110.
	    0,     0,  49,  // 101  0001 1010.
	    0,     0,  58,  // 102  0000 0100.
	    0,     0,  37,  // 103  0000 1110.
	    0,     0,  25,  // 104  0000 1111.
	    0,     0,  45,  // 105  0000 1010.
	    0,     0,  57,  // 106  0000 1000.
	    0,     0,  26,  // 107  0000 1101.
	    0,     0,  29,  // 108  0000 1011.
	    0,     0,  38,  // 109  0000 1100.
	    0,     0,  53,  // 110  0000 1001.
	    0,     0,  23,  // 111  0001 0001.
	    0,     0,  43,  // 112  0001 0000.
	    0,     0,  46,  // 113  0000 0110.
	    0,     0,  42,  // 114  0001 0100.
	    0,     0,  22,  // 115  0001 0101.
	    0,     0,  54,  // 116  0000 0101.
	    0,     0,  51,  // 117  0001 0010.
	    0,     0,  15,  // 118  0001 0011.
	    0,     0,  30,  // 119  0000 0111.
	    0,     0,  39,  // 120  0000 0001 0.
	    0,     0,  47,  // 121  0000 0011 0.
	    0,     0,  55,  // 122  0000 0010 1.
	    0,     0,  27,  // 123  0000 0001 1.
	    0,     0,  59,  // 124  0000 0010 0.
	    0,     0,  31   // 125  0000 0011 1.
]);

MPEG1.MOTION = new Int16Array([
	  1*3,   2*3,   0,  //   0
	  4*3,   3*3,   0,  //   1  0
	    0,     0,   0,  //   2  1.
	  6*3,   5*3,   0,  //   3  01
	  8*3,   7*3,   0,  //   4  00
	    0,     0,  -1,  //   5  011.
	    0,     0,   1,  //   6  010.
	  9*3,  10*3,   0,  //   7  001
	 12*3,  11*3,   0,  //   8  000
	    0,     0,   2,  //   9  0010.
	    0,     0,  -2,  //  10  0011.
	 14*3,  15*3,   0,  //  11  0001
	 16*3,  13*3,   0,  //  12  0000
	 20*3,  18*3,   0,  //  13  0000 1
	    0,     0,   3,  //  14  0001 0.
	    0,     0,  -3,  //  15  0001 1.
	 17*3,  19*3,   0,  //  16  0000 0
	   -1,  23*3,   0,  //  17  0000 00
	 27*3,  25*3,   0,  //  18  0000 11
	 26*3,  21*3,   0,  //  19  0000 01
	 24*3,  22*3,   0,  //  20  0000 10
	 32*3,  28*3,   0,  //  21  0000 011
	 29*3,  31*3,   0,  //  22  0000 101
	   -1,  33*3,   0,  //  23  0000 001
	 36*3,  35*3,   0,  //  24  0000 100
	    0,     0,  -4,  //  25  0000 111.
	 30*3,  34*3,   0,  //  26  0000 010
	    0,     0,   4,  //  27  0000 110.
	    0,     0,  -7,  //  28  0000 0111.
	    0,     0,   5,  //  29  0000 1010.
	 37*3,  41*3,   0,  //  30  0000 0100
	    0,     0,  -5,  //  31  0000 1011.
	    0,     0,   7,  //  32  0000 0110.
	 38*3,  40*3,   0,  //  33  0000 0011
	 42*3,  39*3,   0,  //  34  0000 0101
	    0,     0,  -6,  //  35  0000 1001.
	    0,     0,   6,  //  36  0000 1000.
	 51*3,  54*3,   0,  //  37  0000 0100 0
	 50*3,  49*3,   0,  //  38  0000 0011 0
	 45*3,  46*3,   0,  //  39  0000 0101 1
	 52*3,  47*3,   0,  //  40  0000 0011 1
	 43*3,  53*3,   0,  //  41  0000 0100 1
	 44*3,  48*3,   0,  //  42  0000 0101 0
	    0,     0,  10,  //  43  0000 0100 10.
	    0,     0,   9,  //  44  0000 0101 00.
	    0,     0,   8,  //  45  0000 0101 10.
	    0,     0,  -8,  //  46  0000 0101 11.
	 57*3,  66*3,   0,  //  47  0000 0011 11
	    0,     0,  -9,  //  48  0000 0101 01.
	 60*3,  64*3,   0,  //  49  0000 0011 01
	 56*3,  61*3,   0,  //  50  0000 0011 00
	 55*3,  62*3,   0,  //  51  0000 0100 00
	 58*3,  63*3,   0,  //  52  0000 0011 10
	    0,     0, -10,  //  53  0000 0100 11.
	 59*3,  65*3,   0,  //  54  0000 0100 01
	    0,     0,  12,  //  55  0000 0100 000.
	    0,     0,  16,  //  56  0000 0011 000.
	    0,     0,  13,  //  57  0000 0011 110.
	    0,     0,  14,  //  58  0000 0011 100.
	    0,     0,  11,  //  59  0000 0100 010.
	    0,     0,  15,  //  60  0000 0011 010.
	    0,     0, -16,  //  61  0000 0011 001.
	    0,     0, -12,  //  62  0000 0100 001.
	    0,     0, -14,  //  63  0000 0011 101.
	    0,     0, -15,  //  64  0000 0011 011.
	    0,     0, -11,  //  65  0000 0100 011.
	    0,     0, -13   //  66  0000 0011 111.
]);

MPEG1.DCT_DC_SIZE_LUMINANCE = new Int8Array([
	  2*3,   1*3, 0,  //   0
	  6*3,   5*3, 0,  //   1  1
	  3*3,   4*3, 0,  //   2  0
	    0,     0, 1,  //   3  00.
	    0,     0, 2,  //   4  01.
	  9*3,   8*3, 0,  //   5  11
	  7*3,  10*3, 0,  //   6  10
	    0,     0, 0,  //   7  100.
	 12*3,  11*3, 0,  //   8  111
	    0,     0, 4,  //   9  110.
	    0,     0, 3,  //  10  101.
	 13*3,  14*3, 0,  //  11  1111
	    0,     0, 5,  //  12  1110.
	    0,     0, 6,  //  13  1111 0.
	 16*3,  15*3, 0,  //  14  1111 1
	 17*3,    -1, 0,  //  15  1111 11
	    0,     0, 7,  //  16  1111 10.
	    0,     0, 8   //  17  1111 110.
]);

MPEG1.DCT_DC_SIZE_CHROMINANCE = new Int8Array([
	  2*3,   1*3, 0,  //   0
	  4*3,   3*3, 0,  //   1  1
	  6*3,   5*3, 0,  //   2  0
	  8*3,   7*3, 0,  //   3  11
	    0,     0, 2,  //   4  10.
	    0,     0, 1,  //   5  01.
	    0,     0, 0,  //   6  00.
	 10*3,   9*3, 0,  //   7  111
	    0,     0, 3,  //   8  110.
	 12*3,  11*3, 0,  //   9  1111
	    0,     0, 4,  //  10  1110.
	 14*3,  13*3, 0,  //  11  1111 1
	    0,     0, 5,  //  12  1111 0.
	 16*3,  15*3, 0,  //  13  1111 11
	    0,     0, 6,  //  14  1111 10.
	 17*3,    -1, 0,  //  15  1111 111
	    0,     0, 7,  //  16  1111 110.
	    0,     0, 8   //  17  1111 1110.
]);

//  dct_coeff bitmap:
//    0xff00  run
//    0x00ff  level

//  Decoded values are unsigned. Sign bit follows in the stream.

//  Interpretation of the value 0x0001
//    for dc_coeff_first:  run=0, level=1
//    for dc_coeff_next:   If the next bit is 1: run=0, level=1
//                         If the next bit is 0: end_of_block

//  escape decodes as 0xffff.

MPEG1.DCT_COEFF = new Int32Array([
	  1*3,   2*3,      0,  //   0
	  4*3,   3*3,      0,  //   1  0
	    0,     0, 0x0001,  //   2  1.
	  7*3,   8*3,      0,  //   3  01
	  6*3,   5*3,      0,  //   4  00
	 13*3,   9*3,      0,  //   5  001
	 11*3,  10*3,      0,  //   6  000
	 14*3,  12*3,      0,  //   7  010
	    0,     0, 0x0101,  //   8  011.
	 20*3,  22*3,      0,  //   9  0011
	 18*3,  21*3,      0,  //  10  0001
	 16*3,  19*3,      0,  //  11  0000
	    0,     0, 0x0201,  //  12  0101.
	 17*3,  15*3,      0,  //  13  0010
	    0,     0, 0x0002,  //  14  0100.
	    0,     0, 0x0003,  //  15  0010 1.
	 27*3,  25*3,      0,  //  16  0000 0
	 29*3,  31*3,      0,  //  17  0010 0
	 24*3,  26*3,      0,  //  18  0001 0
	 32*3,  30*3,      0,  //  19  0000 1
	    0,     0, 0x0401,  //  20  0011 0.
	 23*3,  28*3,      0,  //  21  0001 1
	    0,     0, 0x0301,  //  22  0011 1.
	    0,     0, 0x0102,  //  23  0001 10.
	    0,     0, 0x0701,  //  24  0001 00.
	    0,     0, 0xffff,  //  25  0000 01. -- escape
	    0,     0, 0x0601,  //  26  0001 01.
	 37*3,  36*3,      0,  //  27  0000 00
	    0,     0, 0x0501,  //  28  0001 11.
	 35*3,  34*3,      0,  //  29  0010 00
	 39*3,  38*3,      0,  //  30  0000 11
	 33*3,  42*3,      0,  //  31  0010 01
	 40*3,  41*3,      0,  //  32  0000 10
	 52*3,  50*3,      0,  //  33  0010 010
	 54*3,  53*3,      0,  //  34  0010 001
	 48*3,  49*3,      0,  //  35  0010 000
	 43*3,  45*3,      0,  //  36  0000 001
	 46*3,  44*3,      0,  //  37  0000 000
	    0,     0, 0x0801,  //  38  0000 111.
	    0,     0, 0x0004,  //  39  0000 110.
	    0,     0, 0x0202,  //  40  0000 100.
	    0,     0, 0x0901,  //  41  0000 101.
	 51*3,  47*3,      0,  //  42  0010 011
	 55*3,  57*3,      0,  //  43  0000 0010
	 60*3,  56*3,      0,  //  44  0000 0001
	 59*3,  58*3,      0,  //  45  0000 0011
	 61*3,  62*3,      0,  //  46  0000 0000
	    0,     0, 0x0a01,  //  47  0010 0111.
	    0,     0, 0x0d01,  //  48  0010 0000.
	    0,     0, 0x0006,  //  49  0010 0001.
	    0,     0, 0x0103,  //  50  0010 0101.
	    0,     0, 0x0005,  //  51  0010 0110.
	    0,     0, 0x0302,  //  52  0010 0100.
	    0,     0, 0x0b01,  //  53  0010 0011.
	    0,     0, 0x0c01,  //  54  0010 0010.
	 76*3,  75*3,      0,  //  55  0000 0010 0
	 67*3,  70*3,      0,  //  56  0000 0001 1
	 73*3,  71*3,      0,  //  57  0000 0010 1
	 78*3,  74*3,      0,  //  58  0000 0011 1
	 72*3,  77*3,      0,  //  59  0000 0011 0
	 69*3,  64*3,      0,  //  60  0000 0001 0
	 68*3,  63*3,      0,  //  61  0000 0000 0
	 66*3,  65*3,      0,  //  62  0000 0000 1
	 81*3,  87*3,      0,  //  63  0000 0000 01
	 91*3,  80*3,      0,  //  64  0000 0001 01
	 82*3,  79*3,      0,  //  65  0000 0000 11
	 83*3,  86*3,      0,  //  66  0000 0000 10
	 93*3,  92*3,      0,  //  67  0000 0001 10
	 84*3,  85*3,      0,  //  68  0000 0000 00
	 90*3,  94*3,      0,  //  69  0000 0001 00
	 88*3,  89*3,      0,  //  70  0000 0001 11
	    0,     0, 0x0203,  //  71  0000 0010 11.
	    0,     0, 0x0104,  //  72  0000 0011 00.
	    0,     0, 0x0007,  //  73  0000 0010 10.
	    0,     0, 0x0402,  //  74  0000 0011 11.
	    0,     0, 0x0502,  //  75  0000 0010 01.
	    0,     0, 0x1001,  //  76  0000 0010 00.
	    0,     0, 0x0f01,  //  77  0000 0011 01.
	    0,     0, 0x0e01,  //  78  0000 0011 10.
	105*3, 107*3,      0,  //  79  0000 0000 111
	111*3, 114*3,      0,  //  80  0000 0001 011
	104*3,  97*3,      0,  //  81  0000 0000 010
	125*3, 119*3,      0,  //  82  0000 0000 110
	 96*3,  98*3,      0,  //  83  0000 0000 100
	   -1, 123*3,      0,  //  84  0000 0000 000
	 95*3, 101*3,      0,  //  85  0000 0000 001
	106*3, 121*3,      0,  //  86  0000 0000 101
	 99*3, 102*3,      0,  //  87  0000 0000 011
	113*3, 103*3,      0,  //  88  0000 0001 110
	112*3, 116*3,      0,  //  89  0000 0001 111
	110*3, 100*3,      0,  //  90  0000 0001 000
	124*3, 115*3,      0,  //  91  0000 0001 010
	117*3, 122*3,      0,  //  92  0000 0001 101
	109*3, 118*3,      0,  //  93  0000 0001 100
	120*3, 108*3,      0,  //  94  0000 0001 001
	127*3, 136*3,      0,  //  95  0000 0000 0010
	139*3, 140*3,      0,  //  96  0000 0000 1000
	130*3, 126*3,      0,  //  97  0000 0000 0101
	145*3, 146*3,      0,  //  98  0000 0000 1001
	128*3, 129*3,      0,  //  99  0000 0000 0110
	    0,     0, 0x0802,  // 100  0000 0001 0001.
	132*3, 134*3,      0,  // 101  0000 0000 0011
	155*3, 154*3,      0,  // 102  0000 0000 0111
	    0,     0, 0x0008,  // 103  0000 0001 1101.
	137*3, 133*3,      0,  // 104  0000 0000 0100
	143*3, 144*3,      0,  // 105  0000 0000 1110
	151*3, 138*3,      0,  // 106  0000 0000 1010
	142*3, 141*3,      0,  // 107  0000 0000 1111
	    0,     0, 0x000a,  // 108  0000 0001 0011.
	    0,     0, 0x0009,  // 109  0000 0001 1000.
	    0,     0, 0x000b,  // 110  0000 0001 0000.
	    0,     0, 0x1501,  // 111  0000 0001 0110.
	    0,     0, 0x0602,  // 112  0000 0001 1110.
	    0,     0, 0x0303,  // 113  0000 0001 1100.
	    0,     0, 0x1401,  // 114  0000 0001 0111.
	    0,     0, 0x0702,  // 115  0000 0001 0101.
	    0,     0, 0x1101,  // 116  0000 0001 1111.
	    0,     0, 0x1201,  // 117  0000 0001 1010.
	    0,     0, 0x1301,  // 118  0000 0001 1001.
	148*3, 152*3,      0,  // 119  0000 0000 1101
	    0,     0, 0x0403,  // 120  0000 0001 0010.
	153*3, 150*3,      0,  // 121  0000 0000 1011
	    0,     0, 0x0105,  // 122  0000 0001 1011.
	131*3, 135*3,      0,  // 123  0000 0000 0001
	    0,     0, 0x0204,  // 124  0000 0001 0100.
	149*3, 147*3,      0,  // 125  0000 0000 1100
	172*3, 173*3,      0,  // 126  0000 0000 0101 1
	162*3, 158*3,      0,  // 127  0000 0000 0010 0
	170*3, 161*3,      0,  // 128  0000 0000 0110 0
	168*3, 166*3,      0,  // 129  0000 0000 0110 1
	157*3, 179*3,      0,  // 130  0000 0000 0101 0
	169*3, 167*3,      0,  // 131  0000 0000 0001 0
	174*3, 171*3,      0,  // 132  0000 0000 0011 0
	178*3, 177*3,      0,  // 133  0000 0000 0100 1
	156*3, 159*3,      0,  // 134  0000 0000 0011 1
	164*3, 165*3,      0,  // 135  0000 0000 0001 1
	183*3, 182*3,      0,  // 136  0000 0000 0010 1
	175*3, 176*3,      0,  // 137  0000 0000 0100 0
	    0,     0, 0x0107,  // 138  0000 0000 1010 1.
	    0,     0, 0x0a02,  // 139  0000 0000 1000 0.
	    0,     0, 0x0902,  // 140  0000 0000 1000 1.
	    0,     0, 0x1601,  // 141  0000 0000 1111 1.
	    0,     0, 0x1701,  // 142  0000 0000 1111 0.
	    0,     0, 0x1901,  // 143  0000 0000 1110 0.
	    0,     0, 0x1801,  // 144  0000 0000 1110 1.
	    0,     0, 0x0503,  // 145  0000 0000 1001 0.
	    0,     0, 0x0304,  // 146  0000 0000 1001 1.
	    0,     0, 0x000d,  // 147  0000 0000 1100 1.
	    0,     0, 0x000c,  // 148  0000 0000 1101 0.
	    0,     0, 0x000e,  // 149  0000 0000 1100 0.
	    0,     0, 0x000f,  // 150  0000 0000 1011 1.
	    0,     0, 0x0205,  // 151  0000 0000 1010 0.
	    0,     0, 0x1a01,  // 152  0000 0000 1101 1.
	    0,     0, 0x0106,  // 153  0000 0000 1011 0.
	180*3, 181*3,      0,  // 154  0000 0000 0111 1
	160*3, 163*3,      0,  // 155  0000 0000 0111 0
	196*3, 199*3,      0,  // 156  0000 0000 0011 10
	    0,     0, 0x001b,  // 157  0000 0000 0101 00.
	203*3, 185*3,      0,  // 158  0000 0000 0010 01
	202*3, 201*3,      0,  // 159  0000 0000 0011 11
	    0,     0, 0x0013,  // 160  0000 0000 0111 00.
	    0,     0, 0x0016,  // 161  0000 0000 0110 01.
	197*3, 207*3,      0,  // 162  0000 0000 0010 00
	    0,     0, 0x0012,  // 163  0000 0000 0111 01.
	191*3, 192*3,      0,  // 164  0000 0000 0001 10
	188*3, 190*3,      0,  // 165  0000 0000 0001 11
	    0,     0, 0x0014,  // 166  0000 0000 0110 11.
	184*3, 194*3,      0,  // 167  0000 0000 0001 01
	    0,     0, 0x0015,  // 168  0000 0000 0110 10.
	186*3, 193*3,      0,  // 169  0000 0000 0001 00
	    0,     0, 0x0017,  // 170  0000 0000 0110 00.
	204*3, 198*3,      0,  // 171  0000 0000 0011 01
	    0,     0, 0x0019,  // 172  0000 0000 0101 10.
	    0,     0, 0x0018,  // 173  0000 0000 0101 11.
	200*3, 205*3,      0,  // 174  0000 0000 0011 00
	    0,     0, 0x001f,  // 175  0000 0000 0100 00.
	    0,     0, 0x001e,  // 176  0000 0000 0100 01.
	    0,     0, 0x001c,  // 177  0000 0000 0100 11.
	    0,     0, 0x001d,  // 178  0000 0000 0100 10.
	    0,     0, 0x001a,  // 179  0000 0000 0101 01.
	    0,     0, 0x0011,  // 180  0000 0000 0111 10.
	    0,     0, 0x0010,  // 181  0000 0000 0111 11.
	189*3, 206*3,      0,  // 182  0000 0000 0010 11
	187*3, 195*3,      0,  // 183  0000 0000 0010 10
	218*3, 211*3,      0,  // 184  0000 0000 0001 010
	    0,     0, 0x0025,  // 185  0000 0000 0010 011.
	215*3, 216*3,      0,  // 186  0000 0000 0001 000
	    0,     0, 0x0024,  // 187  0000 0000 0010 100.
	210*3, 212*3,      0,  // 188  0000 0000 0001 110
	    0,     0, 0x0022,  // 189  0000 0000 0010 110.
	213*3, 209*3,      0,  // 190  0000 0000 0001 111
	221*3, 222*3,      0,  // 191  0000 0000 0001 100
	219*3, 208*3,      0,  // 192  0000 0000 0001 101
	217*3, 214*3,      0,  // 193  0000 0000 0001 001
	223*3, 220*3,      0,  // 194  0000 0000 0001 011
	    0,     0, 0x0023,  // 195  0000 0000 0010 101.
	    0,     0, 0x010b,  // 196  0000 0000 0011 100.
	    0,     0, 0x0028,  // 197  0000 0000 0010 000.
	    0,     0, 0x010c,  // 198  0000 0000 0011 011.
	    0,     0, 0x010a,  // 199  0000 0000 0011 101.
	    0,     0, 0x0020,  // 200  0000 0000 0011 000.
	    0,     0, 0x0108,  // 201  0000 0000 0011 111.
	    0,     0, 0x0109,  // 202  0000 0000 0011 110.
	    0,     0, 0x0026,  // 203  0000 0000 0010 010.
	    0,     0, 0x010d,  // 204  0000 0000 0011 010.
	    0,     0, 0x010e,  // 205  0000 0000 0011 001.
	    0,     0, 0x0021,  // 206  0000 0000 0010 111.
	    0,     0, 0x0027,  // 207  0000 0000 0010 001.
	    0,     0, 0x1f01,  // 208  0000 0000 0001 1011.
	    0,     0, 0x1b01,  // 209  0000 0000 0001 1111.
	    0,     0, 0x1e01,  // 210  0000 0000 0001 1100.
	    0,     0, 0x1002,  // 211  0000 0000 0001 0101.
	    0,     0, 0x1d01,  // 212  0000 0000 0001 1101.
	    0,     0, 0x1c01,  // 213  0000 0000 0001 1110.
	    0,     0, 0x010f,  // 214  0000 0000 0001 0011.
	    0,     0, 0x0112,  // 215  0000 0000 0001 0000.
	    0,     0, 0x0111,  // 216  0000 0000 0001 0001.
	    0,     0, 0x0110,  // 217  0000 0000 0001 0010.
	    0,     0, 0x0603,  // 218  0000 0000 0001 0100.
	    0,     0, 0x0b02,  // 219  0000 0000 0001 1010.
	    0,     0, 0x0e02,  // 220  0000 0000 0001 0111.
	    0,     0, 0x0d02,  // 221  0000 0000 0001 1000.
	    0,     0, 0x0c02,  // 222  0000 0000 0001 1001.
	    0,     0, 0x0f02   // 223  0000 0000 0001 0110.
]);

MPEG1.PICTURE_TYPE = {
	INTRA: 1,
	PREDICTIVE: 2,
	B: 3
};

MPEG1.START = {
	SEQUENCE: 0xB3,
	SLICE_FIRST: 0x01,
	SLICE_LAST: 0xAF,
	PICTURE: 0x00,
	EXTENSION: 0xB5,
	USER_DATA: 0xB2
};

return MPEG1;

})();

