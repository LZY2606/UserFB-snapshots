'use strict'

// Tests for streaming mip updates in gl-texture2d.
//
// The GL double below only records what the library actually emits:
// the mip level, storage dimensions, upload region and pixel bytes.
// It never picks a level for the library and never clamps or corrects
// a region, so every assertion here reflects real library behavior.
// No browser or GPU is required.

const assert = require('node:assert/strict')
const ndarray = require('ndarray')
const createTexture = require('../texture')

function pixelBytes(pixels) {
  if (pixels == null) {
    return null
  }
  if (ArrayBuffer.isView(pixels)) {
    return Array.from(pixels)
  }
  // rawObject / DOM source: keep an identifying marker instead of bytes
  return { raw: pixels.tag }
}

function createGL() {
  const calls = []
  const gl = {
    TEXTURE_2D: 0x0de1,
    TEXTURE0: 0x84c0,
    ACTIVE_TEXTURE: 0x84e0,
    RGBA: 0x1908,
    RGB: 0x1907,
    ALPHA: 0x1906,
    LUMINANCE: 0x1909,
    LUMINANCE_ALPHA: 0x190a,
    UNSIGNED_BYTE: 0x1401,
    FLOAT: 0x1406,
    NEAREST: 0x2600,
    LINEAR: 0x2601,
    NEAREST_MIPMAP_NEAREST: 0x2700,
    LINEAR_MIPMAP_NEAREST: 0x2701,
    NEAREST_MIPMAP_LINEAR: 0x2702,
    LINEAR_MIPMAP_LINEAR: 0x2703,
    CLAMP_TO_EDGE: 0x812f,
    REPEAT: 0x2901,
    MIRRORED_REPEAT: 0x8370,
    MAX_TEXTURE_SIZE: 0x0d33,
    // When set, the next texImage2D at this non-zero level throws once.
    failOnceAtLevel: null,
    getParameter(p) {
      if (p === this.MAX_TEXTURE_SIZE) return 4096
      if (p === this.ACTIVE_TEXTURE) return this.TEXTURE0
      return 0
    },
    getExtension() {
      return null
    },
    createTexture() {
      return {}
    },
    deleteTexture(handle) {
      calls.push({ op: 'deleteTexture', handle })
    },
    activeTexture() {},
    bindTexture() {},
    texParameteri() {},
    texParameterf() {},
    generateMipmap(target) {
      calls.push({ op: 'generateMipmap', target })
    },
    texImage2D(target, level, internalformat, width, height, border, format, type, pixels) {
      if (level !== 0 && this.failOnceAtLevel === level) {
        this.failOnceAtLevel = null
        throw new Error('simulated storage allocation failure')
      }
      calls.push({
        op: 'image',
        level,
        width,
        height,
        pixels: pixelBytes(pixels)
      })
    },
    texSubImage2D(target, level, xoff, yoff, width, height, format, type, pixels) {
      if (arguments.length === 9) {
        // Typed-array overload: region is given explicitly
        calls.push({
          op: 'sub',
          level,
          xoff,
          yoff,
          width,
          height,
          raw: false,
          pixels: pixelBytes(pixels)
        })
      } else {
        // DOM/rawObject overload: (target, level, xoff, yoff, format, type, source)
        const source = arguments[6]
        calls.push({
          op: 'sub',
          level,
          xoff,
          yoff,
          width: source.width,
          height: source.height,
          raw: true,
          pixels: pixelBytes(source)
        })
      }
    }
  }
  gl.calls = calls
  return gl
}

function rgbaPixel(r, g, b, a) {
  return ndarray(new Uint8Array([r, g, b, a]), [1, 1, 4], [4, 4, 1])
}

function rgbaBlock(w, h, fill) {
  const data = new Uint8Array(w * h * 4)
  for (let i = 0; i < data.length; i++) {
    data[i] = fill
  }
  return ndarray(data, [w, h, 4], [4, 4 * w, 1])
}

function imageCalls(gl, level) {
  return gl.calls.filter((c) => c.op === 'image' && c.level === level)
}

function subCalls(gl, level) {
  return gl.calls.filter((c) => c.op === 'sub' && c.level === level)
}

let pass = 0
let fail = 0

function test(name, fn) {
  try {
    fn()
    pass += 1
    console.log('ok - ' + name)
  } catch (err) {
    fail += 1
    console.log('not ok - ' + name)
    console.log('  ' + (err && err.stack ? err.stack.split('\n').slice(0, 2).join('\n  ') : err))
  }
}

// 1. First local write to an unallocated level builds the FULL level
//    storage, then uploads the block as a subimage.
test('ndarray first mip write allocates full 4x2 level 1 storage then subimages 1x1', () => {
  const gl = createGL()
  const tex = createTexture(gl, [8, 4])
  tex.setPixels(rgbaPixel(11, 22, 33, 44), [0, 0], 1)

  const img = imageCalls(gl, 1)
  assert.equal(img.length, 1)
  assert.deepEqual([img[0].width, img[0].height], [4, 2])
  assert.equal(img[0].pixels, null)

  const sub = subCalls(gl, 1)
  assert.equal(sub.length, 1)
  assert.deepEqual([sub[0].xoff, sub[0].yoff, sub[0].width, sub[0].height], [0, 0, 1, 1])
  assert.deepEqual(sub[0].pixels, [11, 22, 33, 44])
})

// 2. An existing level only accepts subimages; later (smaller) blocks must
//    never shrink the level. x=2 on a level 4 wide must be legal.
test('existing level only emits subimages and never shrinks', () => {
  const gl = createGL()
  const tex = createTexture(gl, [8, 4])
  tex.setPixels(rgbaPixel(1, 2, 3, 4), [0, 0], 1)
  tex.setPixels(rgbaPixel(5, 6, 7, 8), [2, 0], 1)

  assert.equal(imageCalls(gl, 1).length, 1)
  const subs = subCalls(gl, 1)
  assert.equal(subs.length, 2)
  assert.deepEqual([subs[1].xoff, subs[1].yoff, subs[1].width, subs[1].height], [2, 0, 1, 1])
  assert.deepEqual(subs[1].pixels, [5, 6, 7, 8])
  assert.deepEqual([imageCalls(gl, 1)[0].width, imageCalls(gl, 1)[0].height], [4, 2])
})

// 3. Zero offset and blocks flush against the right/bottom boundary are valid.
test('zero offset and boundary-flush regions are accepted', () => {
  const gl = createGL()
  const tex = createTexture(gl, [8, 4])
  tex.setPixels(rgbaBlock(2, 1, 9), [2, 1], 1) // 2+2 == 4, 1+1 == 2
  let sub = gl.calls.at(-1)
  assert.equal(sub.op, 'sub')
  assert.deepEqual([sub.xoff, sub.yoff, sub.width, sub.height], [2, 1, 2, 1])

  tex.setPixels(rgbaBlock(4, 2, 7), [0, 0], 1) // whole level
  sub = gl.calls.at(-1)
  assert.deepEqual([sub.xoff, sub.yoff, sub.width, sub.height], [0, 0, 4, 2])

  assert.throws(() => tex.setPixels(rgbaPixel(1, 1, 1, 1), [4, 1], 1), /out of bounds/)
  assert.throws(() => tex.setPixels(rgbaPixel(1, 1, 1, 1), [3, 2], 1), /out of bounds/)
})

// 4. Tail levels keep 1xN / Nx1 storage via the max(1, ...) rule.
test('Nx1 and 1x1 tail levels get correct full storage', () => {
  const gl = createGL()
  const tex = createTexture(gl, [8, 4]) // levels: 8x4, 4x2, 2x1, 1x1

  tex.setPixels(rgbaPixel(1, 0, 0, 1), [1, 0], 2)
  assert.deepEqual([imageCalls(gl, 2)[0].width, imageCalls(gl, 2)[0].height], [2, 1])
  assert.deepEqual([subCalls(gl, 2)[0].xoff, subCalls(gl, 2)[0].width], [1, 1])

  tex.setPixels(rgbaPixel(0, 1, 0, 1), [0, 0], 3)
  assert.deepEqual([imageCalls(gl, 3)[0].width, imageCalls(gl, 3)[0].height], [1, 1])
  assert.deepEqual([subCalls(gl, 3)[0].xoff, subCalls(gl, 3)[0].yoff], [0, 0])
})

// 5. Illegal levels are rejected before any texture-mutating GL call.
test('negative, fractional and out-of-range levels rejected before GL writes', () => {
  const gl = createGL()
  const tex = createTexture(gl, [8, 4]) // max legal level is 3
  for (const badLevel of [-1, 1.5, 4]) {
    const before = gl.calls.length
    assert.throws(() => tex.setPixels(rgbaPixel(1, 2, 3, 4), [0, 0], badLevel))
    assert.equal(gl.calls.length, before)
  }
})

// 6. Illegal offsets/regions never reach GL and the base level stays untouched.
test('negative, fractional and overflowing regions rejected before GL writes', () => {
  const gl = createGL()
  const tex = createTexture(gl, [8, 4])

  const attempts = [
    () => tex.setPixels(rgbaPixel(1, 2, 3, 4), [-1, 0], 1),
    () => tex.setPixels(rgbaPixel(1, 2, 3, 4), [0.5, 0], 1),
    () => tex.setPixels(rgbaPixel(1, 2, 3, 4), [0, -1], 1),
    () => tex.setPixels({ raw: { tag: 'x', width: 1.5, height: 1 }, width: 1.5, height: 1 }, [0, 0], 1),
    () => tex.setPixels({ raw: { tag: 'y', width: 3, height: 1 }, width: 3, height: 1 }, [0, 0], 2) // level 2 is 2 wide
  ]
  for (const attempt of attempts) {
    const before = gl.calls.length
    assert.throws(attempt)
    assert.equal(gl.calls.length, before)
  }
  assert.equal(subCalls(gl, 0).length, 0)
})

// 7. A non-contiguous ndarray is repacked; bytes and region stay faithful.
test('non-contiguous ndarray is repacked with correct bytes and region', () => {
  const gl = createGL()
  const tex = createTexture(gl, [8, 4])
  const N = 4
  const data = new Uint8Array(N * N * 4)
  for (let i = 0; i < data.length; i++) {
    data[i] = (i % 251) + 1
  }
  const view = ndarray(data, [2, 2, 4], [N * 4, 4, 1], 0) // stride[1] != width*channels
  assert.equal(view.stride[1] !== view.shape[0] * view.shape[2], true)

  tex.setPixels(view, [1, 0], 1)

  assert.deepEqual([imageCalls(gl, 1)[0].width, imageCalls(gl, 1)[0].height], [4, 2])
  const sub = subCalls(gl, 1)[0]
  assert.deepEqual([sub.xoff, sub.yoff, sub.width, sub.height], [1, 0, 2, 2])

  const expected = []
  for (let y = 0; y < 2; y++) {
    for (let x = 0; x < 2; x++) {
      for (let c = 0; c < 4; c++) {
        expected.push(view.get(x, y, c))
      }
    }
  }
  assert.deepEqual(sub.pixels, expected)
})

// 8. Float ndarrays are converted to unsigned bytes through the existing rule.
//    On a freshly allocated level, full null storage is created before the
//    converted block is uploaded as a subimage.
test('float ndarray on a new level allocates storage then uploads converted bytes', () => {
  const gl = createGL()
  const tex = createTexture(gl, [8, 4])
  const floats = ndarray(new Float32Array([0.25, 0.5, 0.75, 1.0]), [1, 1, 4])
  tex.setPixels(floats, [0, 0], 1)

  assert.deepEqual([imageCalls(gl, 1)[0].width, imageCalls(gl, 1)[0].height], [4, 2])
  assert.equal(imageCalls(gl, 1)[0].pixels, null)
  const sub = subCalls(gl, 1)[0]
  assert.deepEqual(sub.pixels, [63, 127, 191, 255])
})

// 9. Packed ndarray with a nonzero array offset uploads exactly that window.
test('packed ndarray with nonzero offset uploads the correct subarray bytes', () => {
  const gl = createGL()
  const tex = createTexture(gl, [8, 4])
  const buf = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8])
  const view = ndarray(buf, [1, 1, 4], [4, 4, 1], 4)
  tex.setPixels(view, [3, 2], 0)

  const sub = gl.calls.at(-1)
  assert.equal(sub.op, 'sub')
  assert.deepEqual([sub.xoff, sub.yoff], [3, 2])
  assert.deepEqual(sub.pixels, [5, 6, 7, 8])
})

// 10. 2D luminance ndarrays keep their existing format/type semantics.
test('2D luminance ndarray uploads through the LUMINANCE path', () => {
  const gl = createGL()
  const tex = createTexture(gl, [4, 4], gl.LUMINANCE, gl.UNSIGNED_BYTE)
  const lum = ndarray(new Uint8Array([10, 20, 30, 40]), [2, 2])
  tex.setPixels(lum, [1, 1], 0)

  const sub = gl.calls.at(-1)
  assert.equal(sub.op, 'sub')
  assert.deepEqual([sub.xoff, sub.yoff, sub.width, sub.height], [1, 1, 2, 2])
  // Repacked into packed row-major (x fastest) order, preserving each logical element
  const expectedPacked = []
  for (let y = 0; y < 2; y++) {
    for (let x = 0; x < 2; x++) {
      expectedPacked.push(lum.get(x, y))
    }
  }
  assert.deepEqual(sub.pixels, expectedPacked)
})

// 11. rawObject first write hits the requested level (never level 0),
//     allocates full storage first, then uses the DOM-style subimage overload.
test('rawObject first write allocates the requested level then subimages', () => {
  const gl = createGL()
  const tex = createTexture(gl, [8, 4])
  const source = { tag: 'A', width: 1, height: 1 }
  tex.setPixels({ raw: source, width: 1, height: 1 }, [0, 0], 2)

  const img = imageCalls(gl, 2)
  assert.equal(img.length, 1)
  assert.deepEqual([img[0].width, img[0].height], [2, 1])
  assert.equal(img[0].pixels, null)

  const sub = subCalls(gl, 2)
  assert.equal(sub.length, 1)
  assert.equal(sub[0].raw, true)
  assert.deepEqual([sub[0].xoff, sub[0].yoff], [0, 0])
  assert.deepEqual(sub[0].pixels, { raw: 'A' })
  assert.equal(subCalls(gl, 0).length, 0)
})

// 12. Later raw writes to a live level are sub-only; bad raw regions never reach GL.
test('rawObject updates existing level sub-only and rejects bad regions', () => {
  const gl = createGL()
  const tex = createTexture(gl, [8, 4])
  tex.setPixels({ raw: { tag: 'A', width: 1, height: 1 }, width: 1, height: 1 }, [0, 0], 2)
  tex.setPixels({ raw: { tag: 'B', width: 1, height: 1 }, width: 1, height: 1 }, [1, 0], 2)

  assert.equal(imageCalls(gl, 2).length, 1)
  const subs = subCalls(gl, 2)
  assert.equal(subs.length, 2)
  assert.deepEqual([subs[1].xoff, subs[1].yoff], [1, 0])
  assert.deepEqual(subs[1].pixels, { raw: 'B' })

  const before = gl.calls.length
  assert.throws(() =>
    tex.setPixels({ raw: { tag: 'C', width: 2, height: 2 }, width: 2, height: 2 }, [0, 0], 2)
  )
  assert.equal(gl.calls.length, before)
})

// 13. Streaming: several contiguous blocks fill a level; storage is allocated
//     once and every block lands at its own region with its own bytes.
test('contiguous multi-block streaming allocates once then subimages every block', () => {
  const gl = createGL()
  const tex = createTexture(gl, [8, 4])
  const regions = [
    [0, 0], [2, 0], [0, 1], [2, 1]
  ]
  regions.forEach(([x, y], i) => {
    tex.setPixels(rgbaBlock(2, 1, i + 10), [x, y], 1)
  })

  assert.equal(imageCalls(gl, 1).length, 1)
  const subs = subCalls(gl, 1)
  assert.equal(subs.length, 4)
  assert.deepEqual(subs.map((s) => [s.xoff, s.yoff, s.width, s.height]), [
    [0, 0, 2, 1],
    [2, 0, 2, 1],
    [0, 1, 2, 1],
    [2, 1, 2, 1]
  ])
  assert.deepEqual(subs[3].pixels, new Array(8).fill(13))
})

// 14. Changing shape/width/height rebuilds from the new base size; old level
//     existence markers must not be reused.
test('reshape resets mip markers and levels use the new base size', () => {
  const gl = createGL()
  const tex = createTexture(gl, [8, 4])
  tex.setPixels(rgbaPixel(1, 2, 3, 4), [0, 0], 1)

  tex.shape = [16, 8]
  assert.deepEqual(tex._mipLevels, [0])
  tex.setPixels(rgbaBlock(2, 1, 4), [0, 0], 1)
  const img = imageCalls(gl, 1).at(-1)
  assert.deepEqual([img.width, img.height], [8, 4])

  tex.width = 32
  assert.deepEqual(tex._mipLevels, [0])
  tex.height = 16
  assert.deepEqual(tex._mipLevels, [0])
  assert.deepEqual([tex.width, tex.height], [32, 16])
})

// 15. After generateMipmap every level already exists, so updates are sub-only.
test('generateMipmap marks all levels and later updates go straight to subimage', () => {
  const gl = createGL()
  const tex = createTexture(gl, [8, 4])
  tex.generateMipmap()
  assert.deepEqual(tex._mipLevels.slice().sort(), [0, 1, 2, 3])

  const before = gl.calls.length
  tex.setPixels(rgbaPixel(9, 8, 7, 6), [0, 0], 2)
  assert.equal(gl.calls.length, before + 1)
  const call = gl.calls.at(-1)
  assert.equal(call.op, 'sub')
  assert.equal(call.level, 2)
})

// 16. A failing storage allocation on the ndarray path leaves the level
//     unmarked with no subimage; the next legal call allocates fresh.
test('ndarray allocation failure is retryable without stale level markers', () => {
  const gl = createGL()
  const tex = createTexture(gl, [8, 4])
  gl.failOnceAtLevel = 1

  assert.throws(() => tex.setPixels(rgbaPixel(1, 2, 3, 4), [0, 0], 1))
  assert.deepEqual(tex._mipLevels, [0])
  assert.equal(subCalls(gl, 1).length, 0)

  tex.setPixels(rgbaPixel(1, 2, 3, 4), [0, 0], 1)
  assert.deepEqual([imageCalls(gl, 1)[0].width, imageCalls(gl, 1)[0].height], [4, 2])
  assert.equal(subCalls(gl, 1).length, 1)
  assert.ok(tex._mipLevels.indexOf(1) >= 0)
})

// 17. Same retry guarantee on the rawObject/DOM path.
test('rawObject allocation failure is retryable', () => {
  const gl = createGL()
  const tex = createTexture(gl, [8, 4])
  gl.failOnceAtLevel = 2

  assert.throws(() =>
    tex.setPixels({ raw: { tag: 'R', width: 1, height: 1 }, width: 1, height: 1 }, [0, 0], 2)
  )
  assert.deepEqual(tex._mipLevels, [0])
  assert.equal(subCalls(gl, 2).length, 0)

  tex.setPixels({ raw: { tag: 'R', width: 1, height: 1 }, width: 1, height: 1 }, [0, 0], 2)
  assert.deepEqual([imageCalls(gl, 2)[0].width, imageCalls(gl, 2)[0].height], [2, 1])
  assert.equal(subCalls(gl, 2).length, 1)
})

// 18. Public bind / filter / wrap / dispose surface stays compatible.
test('bind, filters, wrap modes and dispose keep working', () => {
  const gl = createGL()
  const tex = createTexture(gl, [4, 4])

  assert.equal(tex.bind(2), 2)
  assert.equal(tex.bind(), 0)

  tex.wrapS = gl.REPEAT
  tex.wrapT = gl.MIRRORED_REPEAT
  assert.equal(tex.wrapS, gl.REPEAT)
  assert.equal(tex.wrapT, gl.MIRRORED_REPEAT)
  tex.wrap = [gl.CLAMP_TO_EDGE, gl.CLAMP_TO_EDGE]
  assert.deepEqual(tex.wrap.slice(), [gl.CLAMP_TO_EDGE, gl.CLAMP_TO_EDGE])

  tex.minFilter = gl.NEAREST
  tex.magFilter = gl.LINEAR
  assert.equal(tex.minFilter, gl.NEAREST)
  assert.equal(tex.magFilter, gl.LINEAR)
  assert.throws(() => { tex.minFilter = 99999 }, /Unknown filter/)

  tex.dispose()
  assert.equal(gl.calls.at(-1).op, 'deleteTexture')
})

console.log('\n' + pass + '/' + (pass + fail) + ' tests passed')
if (fail > 0) {
  process.exit(1)
}
