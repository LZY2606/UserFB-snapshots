import { test } from 'node:test'
import assert from 'node:assert'
import convert from './index.js'

// All fixtures are hand-written byte sequences (or Node's own Buffer writers — never this library encoding its
// own expectations). Physical byte order is asserted with Buffer.readXxxBE/LE and literal hex, so a big-endian
// result that is merely read back with a native-order TypedArray cannot pass by accident.

// === The reported failure ===

test('BE int16 Buffer → LE float64 decodes values and signs', () => {
	// 0x4000 = +16384 → +0.5; 0xc000 as int16 = -16384 → -0.5
	let out = convert(Buffer.from([0x40, 0x00, 0xc0, 0x00]),
		{ dtype: 'int16', endianness: 'be' },
		{ dtype: 'float64', endianness: 'le' })
	assert.ok(out instanceof Float64Array)
	assert.deepStrictEqual([...out], [0.5, -0.5])
})

// === LE happy path ===

test('LE raw input decodes with native values', () => {
	// Same samples as the BE case, little-endian on the wire: 0x0040, 0xc000(-16384)
	let out = convert(Buffer.from([0x00, 0x40, 0x00, 0xc0]),
		{ dtype: 'int16', endianness: 'le' }, 'float64')
	assert.deepStrictEqual([...out], [0.5, -0.5])
})

test('LE output physical bytes are little-endian', () => {
	let out = convert(new Int16Array([1, -2]), 'int16', { dtype: 'int16', endianness: 'le' })
	let raw = Buffer.from(out.buffer, out.byteOffset, out.byteLength)
	assert.equal(raw.toString('hex'), '0100feff')
	assert.equal(raw.readInt16LE(0), 1)
	assert.equal(raw.readInt16LE(2), -2)
})

test('no source endianness on raw bytes means platform-native order', () => {
	// This suite runs on little-endian Node (arm64/x64); document rather than assume silently.
	assert.equal(new Uint8Array(new Uint16Array([1]).buffer)[0], 1, 'fixture assumes a little-endian host')
	let out = convert(Buffer.from([0x00, 0x40, 0x00, 0xc0]), { dtype: 'int16' }, 'float64')
	assert.deepStrictEqual([...out], [0.5, -0.5])
})

// === BE input across every supported multi-byte dtype ===

test('BE int32 input', () => {
	// 0x40000000 = 2^30 → 0.5 ; 0xc0000000 signed = -2^30 → -0.5
	let out = convert(Buffer.from([0x40, 0, 0, 0, 0xc0, 0, 0, 0]),
		{ dtype: 'int32', endianness: 'be' }, 'float64')
	assert.deepStrictEqual([...out], [0.5, -0.5])
})

test('BE uint16 input is unsigned with the 2^15 midpoint', () => {
	// 0x4000 = 16384 → -0.5 ; 0xc000 = 49152 → +0.5
	let out = convert(Buffer.from([0x40, 0x00, 0xc0, 0x00]),
		{ dtype: 'uint16', endianness: 'be' }, 'float64')
	assert.deepStrictEqual([...out], [-0.5, 0.5])
})

test('BE uint32 input is unsigned with the 2^31 midpoint', () => {
	// 0x40000000 = 2^30 → -0.5 ; 0xc0000000 = 3·2^30 → +0.5
	let out = convert(Buffer.from([0x40, 0, 0, 0, 0xc0, 0, 0, 0]),
		{ dtype: 'uint32', endianness: 'be' }, 'float64')
	assert.deepStrictEqual([...out], [-0.5, 0.5])
})

test('BE float32 input', () => {
	let raw = Buffer.alloc(8)
	raw.writeFloatBE(0.25, 0)
	raw.writeFloatBE(-0.75, 4)
	let out = convert(raw, { dtype: 'float32', endianness: 'be' }, 'float64')
	assert.deepStrictEqual([...out], [0.25, -0.75])
})

test('BE float64 input', () => {
	let raw = Buffer.alloc(16)
	raw.writeDoubleBE(0.125, 0)
	raw.writeDoubleBE(-0.625, 8)
	let out = convert(raw, { dtype: 'float64', endianness: 'be' }, 'float32')
	assert.deepStrictEqual([...out], [0.125, -0.625])
})

test('ArrayBuffer raw source decodes BE the same as Buffer', () => {
	let ab = new Uint8Array([0x40, 0x00, 0xc0, 0x00]).buffer
	let out = convert(ab, { dtype: 'int16', endianness: 'be' }, 'float64')
	assert.deepStrictEqual([...out], [0.5, -0.5])
})

test('Buffer source with non-zero byteOffset decodes its own region only', () => {
	let carrier = Buffer.concat([
		Buffer.from([0xaa, 0xbb]),                       // unrelated prefix
		Buffer.from([0x40, 0x00, 0xc0, 0x00]),           // BE int16 +16384, -16384
		Buffer.from([0xcc])                              // unrelated suffix
	])
	let slice = carrier.subarray(2, 6)
	assert.ok(slice.byteOffset > 0, 'fixture: the view sits at a non-zero pool offset')
	assert.equal(slice.byteLength, 4)
	let out = convert(slice, { dtype: 'int16', endianness: 'be' }, 'float64')
	assert.deepStrictEqual([...out], [0.5, -0.5])
})

// === Numeric inputs are never reinterpreted by from.endianness ===

test('TypedArray input keeps its numeric values regardless of from.endianness', () => {
	let out = convert(new Int16Array([16384, -16384]),
		{ dtype: 'int16', endianness: 'be' }, 'float64')
	assert.deepStrictEqual([...out], [0.5, -0.5])
})

test('plain Array input/output carries JS numbers; endianness never reinterprets it', () => {
	// A numeric array source is a sequence of samples, not bytes: a from.endianness token changes nothing.
	let fromBE = convert([0.5, -0.5], { dtype: 'float32', endianness: 'be' }, 'int16')
	let fromNone = convert([0.5, -0.5], 'float32', 'int16')
	assert.deepStrictEqual([...fromBE], [...fromNone])
	assert.deepStrictEqual([...fromBE], [16384, -16384])

	// An 'array' output is normalized to the float -1..1 range and has no physical byte order at all:
	// the result must be identical with and without a from.endianness token.
	let a = convert(new Int16Array([1, -2]), { dtype: 'int16', endianness: 'be' }, 'array')
	let b = convert(new Int16Array([1, -2]), { dtype: 'int16' }, 'array')
	assert.deepStrictEqual(a, b)
	assert.deepStrictEqual(a, [1 / 32768, -2 / 32768])
})

// === BE output: physical encoding in the valid region ===

test('BE int16 output physical bytes', () => {
	let out = convert(new Int16Array([1, -2]), 'int16', { dtype: 'int16', endianness: 'be' })
	let raw = Buffer.from(out.buffer, out.byteOffset, out.byteLength)
	assert.equal(raw.toString('hex'), '0001fffe')
	assert.equal(raw.readInt16BE(0), 1)
	assert.equal(raw.readInt16BE(2), -2)
})

test('BE int32 / uint16 / uint32 output physical bytes', () => {
	let i32 = convert(new Int32Array([1, -2]), 'int32', { dtype: 'int32', endianness: 'be' })
	let i32raw = Buffer.from(i32.buffer)
	assert.equal(i32raw.toString('hex'), '00000001fffffffe')
	assert.equal(i32raw.readInt32BE(0), 1)
	assert.equal(i32raw.readInt32BE(4), -2)

	let u16 = convert(new Uint16Array([1, 2]), 'uint16', { dtype: 'uint16', endianness: 'be' })
	assert.equal(Buffer.from(u16.buffer).toString('hex'), '00010002')

	let u32 = convert(new Uint32Array([1, 2]), 'uint32', { dtype: 'uint32', endianness: 'be' })
	let u32raw = Buffer.from(u32.buffer)
	assert.equal(u32raw.toString('hex'), '0000000100000002')
	assert.equal(u32raw.readUInt32BE(0), 1)
	assert.equal(u32raw.readUInt32BE(4), 2)
})

test('BE float32 / float64 output physical bytes', () => {
	let f32 = convert(new Float32Array([0.25, -0.75]), 'float32', { dtype: 'float32', endianness: 'be' })
	let f32raw = Buffer.from(f32.buffer)
	assert.equal(f32raw.readFloatBE(0), 0.25)
	assert.equal(f32raw.readFloatBE(4), -0.75)

	let f64 = convert(new Float64Array([0.125, -0.625]), 'float64', { dtype: 'float64', endianness: 'be' })
	let f64raw = Buffer.from(f64.buffer)
	assert.equal(f64raw.readDoubleBE(0), 0.125)
	assert.equal(f64raw.readDoubleBE(8), -0.625)
})

test('to.endianness takes effect even when from declares no endianness', () => {
	let out = convert(new Int16Array([1, -2]), { dtype: 'int16' }, { dtype: 'int16', endianness: 'be' })
	assert.equal(Buffer.from(out.buffer).toString('hex'), '0001fffe')
})

test('BE output into an ArrayBuffer destination encodes inside that buffer', () => {
	let dst = new ArrayBuffer(4)
	let returned = convert(new Int16Array([1, -2]), 'int16', { dtype: 'int16', endianness: 'be' }, dst)
	assert.equal(returned, dst, 'the caller-provided ArrayBuffer is returned')
	let raw = Buffer.from(dst)
	assert.equal(raw.toString('hex'), '0001fffe')
})

test('float → int keeps rounding and full-scale clipping, including |x| > 1', () => {
	let out = convert(new Float32Array([1.5, -2, 0.5]), 'float32', { dtype: 'int16', endianness: 'be' })
	let raw = Buffer.from(out.buffer)
	assert.equal(raw.readInt16BE(0), 32767)
	assert.equal(raw.readInt16BE(2), -32768)
	assert.equal(raw.readInt16BE(4), 16384)
})

// === Single-byte dtypes are unaffected by endianness ===

test('uint8/int8 raw input ignores endianness', () => {
	let u8 = convert(Buffer.from([0x00, 0xff]), { dtype: 'uint8', endianness: 'be' }, 'float64')
	assert.deepStrictEqual([...u8], [-1, 127 / 128])

	let i8 = convert(Buffer.from([0x00, 0x7f]), { dtype: 'int8', endianness: 'be' }, 'float64')
	assert.deepStrictEqual([...i8], [0, 127 / 128])
})

test('uint8 output bytes are identical for either endianness token', () => {
	let be = convert(new Uint8Array([0, 255]), 'uint8', { dtype: 'uint8', endianness: 'be' })
	let le = convert(new Uint8Array([0, 255]), 'uint8', { dtype: 'uint8', endianness: 'le' })
	assert.equal(Buffer.from(be.buffer).toString('hex'), '00ff')
	assert.equal(Buffer.from(le.buffer).toString('hex'), '00ff')
})

// === Layout rearrangement keeps frame/channel/amplitude relations under endianness ===

test('BE interleaved stereo raw input → planar float preserves frames, channels and levels', () => {
	// frame 0: L = 0x4000 (+16384 → +0.5), R = 0xc000 (-16384 → -0.5)
	// frame 1: L = 0x7fff (+32767),        R = 0x8000 (-32768 → -1)
	let raw = Buffer.from([0x40, 0x00, 0xc0, 0x00, 0x7f, 0xff, 0x80, 0x00])
	let out = convert(raw,
		{ dtype: 'int16', endianness: 'be', channels: 2, interleaved: true },
		{ dtype: 'float64', channels: 2, interleaved: false })
	assert.deepStrictEqual([...out], [0.5, 32767 / 32768, -0.5, -1])
})

test('planar float → BE interleaved int16 writes channels back into the right frames', () => {
	let out = convert(new Float64Array([0.5, 32767 / 32768, -0.5, -1]),
		{ dtype: 'float64', channels: 2, interleaved: false },
		{ dtype: 'int16', endianness: 'be', channels: 2, interleaved: true })
	let raw = Buffer.from(out.buffer)
	// frame 0 L,R then frame 1 L,R, big-endian on the wire
	assert.equal(raw.toString('hex'), '4000c0007fff8000')
	assert.equal(raw.readInt16BE(0), 16384)
	assert.equal(raw.readInt16BE(2), -16384)
	assert.equal(raw.readInt16BE(4), 32767)
	assert.equal(raw.readInt16BE(6), -32768)
})

test('BE interleaved int16 → BE int32 keeps frame order and is consistent on both byte orders', () => {
	let raw = Buffer.from([0x40, 0x00, 0xc0, 0x00, 0x7f, 0xff, 0x80, 0x00])
	let native = convert(raw, { dtype: 'int16', endianness: 'be', channels: 2, interleaved: true }, 'int32')
	let be = convert(raw, { dtype: 'int16', endianness: 'be', channels: 2, interleaved: true },
		{ dtype: 'int32', endianness: 'be', channels: 2, interleaved: true })
	let beBytes = Buffer.from(be.buffer)
	for (let i = 0; i < native.length; i++) {
		// Physical BE bytes must decode (with BE readers) back to the same numeric samples.
		assert.equal(beBytes.readInt32BE(i * 4), native[i])
	}
})

// === Subview sentinels: only the view's valid region is written ===

test('BE int16 written into a subarray leaves the parent buffer sentinels intact', () => {
	let parent = new Int16Array([111, 222, 333, 444])
	let dst = parent.subarray(1, 3)
	let returned = convert(new Int16Array([1, 2]), 'int16', { dtype: 'int16', endianness: 'be' }, dst)

	assert.equal(returned, dst, 'the exact destination object is returned')
	// Native-order reads of BE bytes differ from the sample values — that is expected; check physical bytes.
	let viewBytes = Buffer.from(parent.buffer, dst.byteOffset, dst.byteLength)
	assert.equal(viewBytes.toString('hex'), '00010002')
	// Bytes outside the view's region are untouched.
	assert.equal(parent[0], 111)
	assert.equal(parent[3], 444)
	assert.equal(Buffer.from(parent.buffer, 0, 2).toString('hex'), '6f00')   // 111 little-endian
	assert.equal(Buffer.from(parent.buffer, 6, 2).readInt16LE(0), 444)
})

// === Overlapping storage ===

test('overlapping typed source and destination match an independent source copy', () => {
	let mem = new Int32Array([10, 20, 30, 40, 50, 60])
	let src = mem.subarray(0, 4)
	let dst = mem.subarray(2, 6)

	let reference = convert(Int32Array.from(src), 'int32', 'int32', new Int32Array(4))
	let returned = convert(src, 'int32', 'int32', dst)

	assert.equal(returned, dst)
	assert.deepStrictEqual([...dst], [...reference])
})

test('raw byte source sharing storage with the typed target matches a detached copy', () => {
	let backing = new ArrayBuffer(8)
	new Uint8Array(backing).set([0x40, 0x00, 0xc0, 0x00, 0, 0, 0, 0])
	let rawSrc = Buffer.from(backing, 0, 4)
	let dst = new Int16Array(backing, 4, 2)

	let independent = convert(Buffer.from([0x40, 0x00, 0xc0, 0x00]),
		{ dtype: 'int16', endianness: 'be' },
		{ dtype: 'int16', endianness: 'le' }, new Int16Array(2))
	let returned = convert(rawSrc, { dtype: 'int16', endianness: 'be' },
		{ dtype: 'int16', endianness: 'le' }, dst)

	assert.equal(returned, dst)
	assert.deepStrictEqual([...dst], [...independent])
	// The source region is read-only input and survives the overlapping write.
	assert.deepStrictEqual([...new Uint8Array(backing, 0, 4)], [0x40, 0x00, 0xc0, 0x00])
})

// === Failures happen before the destination is touched ===

test('undersized typed destination throws and keeps every sentinel byte', () => {
	let dst = new Int16Array([0x7777, 0x7777])
	let before = Buffer.from(dst.buffer).toString('hex')
	assert.throws(() => convert(new Int16Array([1, 2, 3]), 'int16',
		{ dtype: 'int16', endianness: 'be' }, dst), /Destination/)
	assert.equal(Buffer.from(dst.buffer).toString('hex'), before)
})

test('undersized ArrayBuffer destination throws without writing', () => {
	let dst = new ArrayBuffer(3)
	let before = Buffer.from(dst).toString('hex')
	assert.throws(() => convert(Buffer.from([0x40, 0x00, 0xc0, 0x00]),
		{ dtype: 'int16', endianness: 'be' },
		{ dtype: 'int16', endianness: 'be' }, dst), /Destination/)
	assert.equal(Buffer.from(dst).toString('hex'), before)
})

test('raw source that does not fill whole samples throws before writing', () => {
	let dst = new Int16Array([0x7777, 0x7777])
	let before = Buffer.from(dst.buffer).toString('hex')
	assert.throws(() => convert(Buffer.from([0x40, 0x00, 0xc0]),
		{ dtype: 'int16', endianness: 'be' },
		{ dtype: 'int16', endianness: 'be' }, dst), /whole number/)
	assert.equal(Buffer.from(dst.buffer).toString('hex'), before)
})

test('raw source that does not fill whole channel frames throws before writing', () => {
	let dst = new Float64Array(3)
	let before = Buffer.from(dst.buffer).toString('hex')
	// 3 int16 samples cannot make whole stereo frames.
	assert.throws(() => convert(Buffer.from([0x40, 0x00, 0xc0, 0x00, 0x7f, 0xff]),
		{ dtype: 'int16', endianness: 'be', channels: 2, interleaved: true },
		{ dtype: 'float64', channels: 2, interleaved: false }, dst), /frames/)
	assert.equal(Buffer.from(dst.buffer).toString('hex'), before)
})
