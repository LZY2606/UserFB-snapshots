/* Standalone geometry tests for SPS display information: cropping,
 * sample aspect ratio, and display aspect ratio.
 * Run directly with:  node tests/sps_geometry.mjs
 * No jest or other dev dependencies required. */
import assert from 'node:assert/strict'
import { SPS, AvcC, RawBitstream } from '../index.js'

let passed = 0
let failed = 0

function ok (name, fn) {
  try {
    fn()
    passed++
    console.log('ok   ' + name)
  } catch (ex) {
    failed++
    console.log('FAIL ' + name + '\n     ' + ex.message)
  }
}

function throwsRange (name, fn) {
  ok(name, () => {
    assert.throws(fn, RangeError)
  })
}

function describe (sps) {
  const dar = sps.displayAspectRatio
    ? `${sps.displayAspectRatio.num}:${sps.displayAspectRatio.den}`
    : 'unspecified'
  console.log(
    `     coded ${sps.picWidth}x${sps.picHeight}` +
    `  crop ${sps.cropRect.x},${sps.cropRect.y} ${sps.cropRect.width}x${sps.cropRect.height}` +
    `  displayAspectRatio ${dar}`)
}

/* Build a minimal SPS NALU for the given parameters. */
function buildSPS (opts = {}) {
  const {
    profileIdc = 66,
    levelIdc = 30,
    seqId = 0,
    chromaFormatIdc = 1,
    separateColourPlaneFlag = 0,
    widthMbsMinus1 = 1,
    heightMapUnitsMinus1 = 1,
    frameMbsOnly = 1,
    crop, /* [left, right, top, bottom] or undefined */
    aspectRatioIdc,
    sarWidth = 0,
    sarHeight = 0,
    videoSignal, /* { videoFormat, fullRange } or undefined */
    timing /* { numUnitsInTick, timeScale, fixedFrameRate } or undefined */
  } = opts
  const bs = new RawBitstream(4096)
  bs.put_u8(0x67) /* nal header: forbidden 0, nal_ref_idc 3, type 7 */
  bs.put_u8(profileIdc)
  bs.put_u8(0) /* constraint flags + reserved */
  bs.put_u8(levelIdc)
  bs.put_ue_v(seqId)
  const frext = !(profileIdc === 66 || profileIdc === 77 || profileIdc === 88)
  if (frext) {
    bs.put_ue_v(chromaFormatIdc)
    if (chromaFormatIdc === 3) bs.put_u_1(separateColourPlaneFlag)
    bs.put_ue_v(0) /* bit_depth_luma_minus8 */
    bs.put_ue_v(0) /* bit_depth_chroma_minus8 */
    bs.put_u_1(0) /* qpprime_y_zero_transform_bypass_flag */
    bs.put_u_1(0) /* seq_scaling_matrix_present_flag */
  }
  bs.put_ue_v(0) /* log2_max_frame_num_minus4 */
  bs.put_ue_v(0) /* pic_order_cnt_type */
  bs.put_ue_v(0) /* log2_max_pic_order_cnt_lsb_minus4 */
  bs.put_ue_v(1) /* max_num_ref_frames */
  bs.put_u_1(0) /* gaps_in_frame_num_value_allowed_flag */
  bs.put_ue_v(widthMbsMinus1)
  bs.put_ue_v(heightMapUnitsMinus1)
  bs.put_u_1(frameMbsOnly)
  if (!frameMbsOnly) bs.put_u_1(0) /* mb_adaptive_frame_field_flag */
  bs.put_u_1(1) /* direct_8x8_inference_flag */
  bs.put_u_1(crop ? 1 : 0)
  if (crop) for (const v of crop) bs.put_ue_v(v)
  const vui = aspectRatioIdc !== undefined || videoSignal || timing
  bs.put_u_1(vui ? 1 : 0)
  if (vui) {
    bs.put_u_1(aspectRatioIdc !== undefined ? 1 : 0)
    if (aspectRatioIdc !== undefined) {
      bs.put_u(aspectRatioIdc, 8)
      if (aspectRatioIdc === 255) {
        bs.put_u(sarWidth, 16)
        bs.put_u(sarHeight, 16)
      }
    }
    bs.put_u_1(0) /* overscan_info_present_flag */
    bs.put_u_1(videoSignal ? 1 : 0)
    if (videoSignal) {
      bs.put_u(videoSignal.videoFormat ?? 5, 3)
      bs.put_u_1(videoSignal.fullRange ? 1 : 0)
      bs.put_u_1(0) /* colour_description_present_flag */
    }
    bs.put_u_1(0) /* chroma_loc_info_present_flag */
    bs.put_u_1(timing ? 1 : 0)
    if (timing) {
      bs.put_u(timing.numUnitsInTick, 32)
      bs.put_u(timing.timeScale, 32)
      bs.put_u_1(timing.fixedFrameRate ? 1 : 0)
    }
    bs.put_u_1(0) /* nal_hrd_parameters_present_flag */
  }
  bs.put_u_1(1) /* rbsp_stop_one_bit */
  bs.put_complete()
  return bs.buffer
}

/* Build a minimal PPS NALU. */
function buildPPS () {
  const bs = new RawBitstream(256)
  bs.put_u8(0x68) /* nal header, type 8 */
  bs.put_ue_v(0) /* pic_parameter_set_id */
  bs.put_ue_v(0) /* seq_parameter_set_id */
  bs.put_u_1(0) /* entropy_coding_mode_flag */
  bs.put_u_1(0) /* bottom_field_pic_order_in_frame_present_flag */
  bs.put_ue_v(0) /* num_slice_groups_minus1 */
  bs.put_ue_v(0) /* num_ref_idx_l0_active_minus1 */
  bs.put_ue_v(0) /* num_ref_idx_l1_active_minus1 */
  bs.put_u_1(0) /* weighted_pred_flag */
  bs.put_u(0, 2) /* weighted_bipred_idc */
  bs.put_se_v(0) /* pic_init_qp_minus26 */
  bs.put_se_v(0) /* pic_init_qs_minus26 */
  bs.put_u_1(0) /* deblocking_filter_control_present_flag */
  bs.put_u_1(0) /* constrained_intra_pred_flag */
  bs.put_u_1(0) /* redundant_pic_cnt_present_flag */
  bs.put_u_1(1) /* rbsp_stop_one_bit */
  bs.put_complete()
  return bs.buffer
}

function fromHex (hex) {
  return Uint8Array.from(Buffer.from(hex, 'hex'))
}

console.log('--- reported problem cases ---')

ok('32x32 4:2:0 progressive, crop offsets are chroma units', () => {
  const sps = new SPS(fromHex('6742001eda25d34d'))
  describe(sps)
  assert.equal(sps.picWidth, 32)
  assert.equal(sps.picHeight, 32)
  assert.deepEqual(sps.cropRect, { x: 2, y: 2, width: 26, height: 26 })
  assert.equal(sps.displayAspectRatio, undefined)
})

ok('same stream with VUI aspect_ratio_idc 1 (no 32-bit extended SAR read)', () => {
  const sps = new SPS(fromHex('6742001eda25d34f0104'))
  describe(sps)
  assert.equal(sps.aspect_ratio_idc, 1)
  assert.equal(sps.sar_width, 1)
  assert.equal(sps.sar_height, 1)
  assert.deepEqual(sps.cropRect, { x: 2, y: 2, width: 26, height: 26 })
  assert.deepEqual(sps.displayAspectRatio, { num: 1, den: 1 })
})

console.log('--- cropping units ---')

ok('no cropping flag yields the full coded rectangle', () => {
  const sps = new SPS(buildSPS())
  describe(sps)
  assert.deepEqual(sps.cropRect, { x: 0, y: 0, width: 32, height: 32 })
})

ok('4:2:0 progressive: crop unit is 2x2 luma pixels', () => {
  const sps = new SPS(buildSPS({ profileIdc: 100, chromaFormatIdc: 1, crop: [1, 1, 1, 1] }))
  describe(sps)
  assert.deepEqual(sps.cropRect, { x: 2, y: 2, width: 28, height: 28 })
})

ok('4:2:2 progressive: crop unit is 2x1 luma pixels', () => {
  const sps = new SPS(buildSPS({ profileIdc: 110, chromaFormatIdc: 2, crop: [1, 1, 1, 1] }))
  describe(sps)
  assert.deepEqual(sps.cropRect, { x: 2, y: 1, width: 28, height: 30 })
})

ok('4:4:4 progressive: crop unit is 1x1 luma pixels', () => {
  const sps = new SPS(buildSPS({ profileIdc: 244, chromaFormatIdc: 3, crop: [1, 1, 1, 1] }))
  describe(sps)
  assert.deepEqual(sps.cropRect, { x: 1, y: 1, width: 30, height: 30 })
})

ok('monochrome: crop unit is 1x1 luma pixels', () => {
  const sps = new SPS(buildSPS({ profileIdc: 100, chromaFormatIdc: 0, crop: [1, 1, 1, 1] }))
  describe(sps)
  assert.equal(sps.chromaArrayType, 0)
  assert.deepEqual(sps.cropRect, { x: 1, y: 1, width: 30, height: 30 })
})

ok('4:4:4 with separate_colour_plane_flag is treated as ChromaArrayType 0', () => {
  const sps = new SPS(buildSPS({
    profileIdc: 244, chromaFormatIdc: 3, separateColourPlaneFlag: 1, crop: [1, 1, 1, 1]
  }))
  describe(sps)
  assert.equal(sps.chromaArrayType, 0)
  assert.deepEqual(sps.cropRect, { x: 1, y: 1, width: 30, height: 30 })
})

ok('4:2:0 interlaced: vertical crop unit is 4 luma pixels', () => {
  const sps = new SPS(buildSPS({
    profileIdc: 100, chromaFormatIdc: 1, frameMbsOnly: 0, crop: [1, 1, 1, 1]
  }))
  describe(sps)
  assert.equal(sps.interlaced, true)
  assert.equal(sps.picHeight, 64)
  assert.deepEqual(sps.cropRect, { x: 2, y: 4, width: 28, height: 56 })
})

console.log('--- sample aspect ratio ---')

ok('standard SAR idc 2 is 12:11 from the H.264 table', () => {
  const sps = new SPS(buildSPS({ crop: [1, 2, 1, 2], aspectRatioIdc: 2 }))
  describe(sps)
  assert.equal(sps.sar_width, 12)
  assert.equal(sps.sar_height, 11)
  assert.deepEqual(sps.cropRect, { x: 2, y: 2, width: 26, height: 26 })
  assert.deepEqual(sps.displayAspectRatio, { num: 12, den: 11 })
})

ok('extended SAR idc 255 reads 16-bit sar_width and sar_height', () => {
  const sps = new SPS(buildSPS({
    crop: [1, 2, 1, 2], aspectRatioIdc: 255, sarWidth: 40, sarHeight: 33
  }))
  describe(sps)
  assert.equal(sps.sar_width, 40)
  assert.equal(sps.sar_height, 33)
  assert.deepEqual(sps.displayAspectRatio, { num: 40, den: 33 })
})

ok('aspect_ratio_idc 0 means unspecified, no fabricated 1:1', () => {
  const sps = new SPS(buildSPS({ aspectRatioIdc: 0 }))
  describe(sps)
  assert.equal(sps.aspect_ratio_idc, 0)
  assert.equal(sps.sar_width, undefined)
  assert.equal(sps.sar_height, undefined)
  assert.equal(sps.displayAspectRatio, undefined)
})

ok('absent VUI means unspecified SAR', () => {
  const sps = new SPS(buildSPS())
  assert.equal(sps.sar_width, undefined)
  assert.equal(sps.displayAspectRatio, undefined)
})

ok('non-square cropped rectangle: DAR uses cropped, not coded, size', () => {
  /* crops to 28x26; SAR 1:1. An implementation using the coded 32x32
   * would wrongly report 1:1 here. */
  const sps = new SPS(buildSPS({ crop: [1, 1, 1, 2], aspectRatioIdc: 1 }))
  describe(sps)
  assert.deepEqual(sps.cropRect, { x: 2, y: 2, width: 28, height: 26 })
  assert.deepEqual(sps.displayAspectRatio, { num: 14, den: 13 })
})

ok('DAR is reduced to lowest terms', () => {
  /* 28x28 crop with SAR 12:11 -> 336:308 -> 12:11 */
  const sps = new SPS(buildSPS({ profileIdc: 100, crop: [1, 1, 1, 1], aspectRatioIdc: 2 }))
  describe(sps)
  assert.deepEqual(sps.displayAspectRatio, { num: 12, den: 11 })
})

ok('VUI fields after a standard SAR still land on the right bits', () => {
  const sps = new SPS(buildSPS({
    aspectRatioIdc: 2,
    videoSignal: { videoFormat: 5, fullRange: 1 },
    timing: { numUnitsInTick: 1000, timeScale: 60000, fixedFrameRate: 1 }
  }))
  describe(sps)
  assert.equal(sps.sar_width, 12)
  assert.equal(sps.sar_height, 11)
  assert.equal(sps.video_format, 5)
  assert.equal(sps.video_full_range_flag, 1)
  assert.equal(sps.num_units_in_tick, 1000)
  assert.equal(sps.time_scale, 60000)
  assert.equal(sps.fixed_frame_rate_flag, 1)
  assert.equal(sps.framesPerSecond, 60)
})

ok('VUI fields after an extended SAR still land on the right bits', () => {
  const sps = new SPS(buildSPS({
    aspectRatioIdc: 255,
    sarWidth: 1,
    sarHeight: 1,
    videoSignal: { videoFormat: 2, fullRange: 0 },
    timing: { numUnitsInTick: 1001, timeScale: 30000, fixedFrameRate: 0 }
  }))
  describe(sps)
  assert.equal(sps.sar_width, 1)
  assert.equal(sps.sar_height, 1)
  assert.equal(sps.video_format, 2)
  assert.equal(sps.video_full_range_flag, 0)
  assert.equal(sps.num_units_in_tick, 1001)
  assert.equal(sps.time_scale, 30000)
  assert.equal(sps.fixed_frame_rate_flag, 0)
})

console.log('--- invalid geometry ---')

throwsRange('reserved aspect_ratio_idc 17 throws RangeError', () => {
  // eslint-disable-next-line no-new
  new SPS(buildSPS({ aspectRatioIdc: 17 }))
})

throwsRange('reserved aspect_ratio_idc 254 throws RangeError', () => {
  // eslint-disable-next-line no-new
  new SPS(buildSPS({ aspectRatioIdc: 254 }))
})

throwsRange('extended SAR with zero numerator throws RangeError', () => {
  // eslint-disable-next-line no-new
  new SPS(buildSPS({ aspectRatioIdc: 255, sarWidth: 0, sarHeight: 3 }))
})

throwsRange('extended SAR with zero denominator throws RangeError', () => {
  // eslint-disable-next-line no-new
  new SPS(buildSPS({ aspectRatioIdc: 255, sarWidth: 4, sarHeight: 0 }))
})

throwsRange('cropping away the whole width throws RangeError', () => {
  // eslint-disable-next-line no-new
  new SPS(buildSPS({ crop: [8, 8, 0, 0] }))
})

throwsRange('cropping beyond the whole height throws RangeError', () => {
  // eslint-disable-next-line no-new
  new SPS(buildSPS({ crop: [0, 0, 10, 10] }))
})

console.log('--- AvcC consistency ---')

ok('AvcC exposes the same cropRect and displayAspectRatio as the SPS', () => {
  const spsNalu = buildSPS({ crop: [1, 2, 1, 2], aspectRatioIdc: 2 })
  const sps = new SPS(spsNalu)
  const avcc = new AvcC({ sps: spsNalu, pps: buildPPS() })
  describe(sps)
  assert.deepEqual(avcc.cropRect, sps.cropRect)
  assert.deepEqual(avcc.displayAspectRatio, { num: 12, den: 11 })
  assert.equal(avcc.picWidth, 32)
  assert.equal(avcc.picHeight, 32)
  assert.equal(avcc.MIME, sps.MIME)
  assert.equal(avcc.MIME, 'avc1.42001E')
  /* the avcC atom still packs with the original NAL bytes */
  const packed = avcc.avcC
  assert.ok(packed instanceof Uint8Array)
  assert.deepEqual(avcc.sps[0], spsNalu)
})

ok('a second SPS does not inherit SAR or crop from the first', () => {
  const avcc = new AvcC({ sps: buildSPS({ crop: [1, 2, 1, 2], aspectRatioIdc: 2 }), pps: buildPPS() })
  assert.deepEqual(avcc.displayAspectRatio, { num: 12, den: 11 })
  const second = avcc.unpackSps(buildSPS({ widthMbsMinus1: 3, heightMapUnitsMinus1: 2 }))
  describe(second)
  assert.deepEqual(avcc.cropRect, { x: 0, y: 0, width: 64, height: 48 })
  assert.equal(avcc.displayAspectRatio, undefined)
  assert.equal(second.displayAspectRatio, undefined)
})

console.log('--- robustness ---')

ok('parsing does not modify the input bytes', () => {
  const original = fromHex('6742001eda25d34f0104')
  const copy = original.slice()
  const sps = new SPS(original)
  assert.deepEqual(original, copy)
  assert.ok(sps.success)
})

ok('pre-existing sample SPS still parses (compatibility)', () => {
  /* kept from __tests__/sampledata.js */
  const hex = '67 42 c0 1e 95 a0 28 0f 68 40 00 00 03 00 40 00 00 0f 03 68 22 11 a8'
  const sps = new SPS(Uint8Array.from(hex.split(' ').map(x => parseInt(x, 16))))
  describe(sps)
  assert.ok(sps.success)
  assert.equal(sps.picWidth, 640)
  assert.equal(sps.picHeight, 480)
  assert.deepEqual(sps.cropRect, { x: 0, y: 0, width: 640, height: 480 })
  assert.equal(sps.displayAspectRatio, undefined)
  assert.equal(sps.MIME, 'avc1.42C01E')
})

console.log(`\n${passed} passed, ${failed} failed`)
process.exitCode = failed ? 1 : 0
