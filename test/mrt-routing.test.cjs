'use strict'

//Routing tests for per-FBO draw buffer write masks.  Uses a recording GL
//plus a locally synthesized texture stand-in; all draw buffer routing is
//performed by the library under test, the stand-ins only record.

const fs = require('node:fs')
const vm = require('node:vm')
const path = require('node:path')
const assert = require('node:assert')

const SRC = fs.readFileSync(path.join(__dirname, '../fbo.js'), 'utf8')

function loadFBO(createTexture) {
  const moduleBox = { exports: {} }
  vm.runInThisContext('(function(require,module,exports){' + SRC + '\n})')(
    name => {
      if(name === 'gl-texture2d') return createTexture
      throw Error(name)
    }, moduleBox, moduleBox.exports)
  return moduleBox.exports
}

//Texture stand-in: records allocation/release so tests can verify
//attachment identity and dimensions.  Never routes anything itself.
function makeTextureFactory(rec) {
  return function createTexture(gl, width, height, format, type) {
    const tex = {
      gl: gl,
      width: width,
      height: height,
      format: format,
      type: type,
      handle: { texture: ++rec.serial },
      disposed: false,
      magFilter: 0,
      minFilter: 0,
      mipSamples: 0,
      bind() {},
      dispose() {
        this.disposed = true
        rec.disposed.push(this)
      }
    }
    Object.defineProperty(tex, 'shape', {
      get() { return [this.width, this.height] },
      set(s) { this.width = s[0]; this.height = s[1] }
    })
    rec.allocated.push(tex)
    return tex
  }
}

const P = {
  FRAMEBUFFER_BINDING: 1,
  RENDERBUFFER_BINDING: 2,
  TEXTURE_BINDING_2D: 3,
  MAX_RENDERBUFFER_SIZE: 4,
  MAX_COLOR_ATTACHMENTS_WEBGL: 20,
  MAX_DRAW_BUFFERS_WEBGL: 21
}

function makeContext(opts) {
  opts = opts || {}
  const maxColorAttachments = opts.maxColorAttachments == null ? 4 : opts.maxColorAttachments
  const maxDrawBuffers = opts.maxDrawBuffers == null ? maxColorAttachments : opts.maxDrawBuffers
  const hasExt = opts.drawBuffersExt !== false

  const log = []
  const bindings = new Map()
  let serial = 0
  let framebuffersCreated = 0

  const ext = {
    MAX_COLOR_ATTACHMENTS_WEBGL: P.MAX_COLOR_ATTACHMENTS_WEBGL,
    MAX_DRAW_BUFFERS_WEBGL: P.MAX_DRAW_BUFFERS_WEBGL,
    drawBuffersWEBGL: a => log.push({
      name: 'drawBuffersWEBGL',
      buffers: a.slice(),
      framebuffer: bindings.get(P.FRAMEBUFFER_BINDING)
    })
  }

  const gl = {
    FRAMEBUFFER_BINDING: P.FRAMEBUFFER_BINDING,
    RENDERBUFFER_BINDING: P.RENDERBUFFER_BINDING,
    TEXTURE_BINDING_2D: P.TEXTURE_BINDING_2D,
    MAX_RENDERBUFFER_SIZE: P.MAX_RENDERBUFFER_SIZE,
    FRAMEBUFFER: 5,
    RENDERBUFFER: 6,
    TEXTURE_2D: 7,
    COLOR_ATTACHMENT0: 100,
    NONE: 0,
    RGBA: 11,
    RGBA4: 8,
    UNSIGNED_BYTE: 9,
    FRAMEBUFFER_COMPLETE: 10,
    FRAMEBUFFER_UNSUPPORTED: 30,
    FRAMEBUFFER_INCOMPLETE_ATTACHMENT: 31,
    FRAMEBUFFER_INCOMPLETE_DIMENSIONS: 32,
    FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: 33,
    DEPTH_COMPONENT16: 40,
    DEPTH_STENCIL: 41,
    STENCIL_INDEX: 42,
    DEPTH_ATTACHMENT: 50,
    STENCIL_ATTACHMENT: 51,
    DEPTH_STENCIL_ATTACHMENT: 52,
    NEAREST: 60,
    getExtension: n => n === 'WEBGL_draw_buffers' ? (hasExt ? ext : null) : null,
    getParameter: p => {
      if(p === P.MAX_COLOR_ATTACHMENTS_WEBGL) return maxColorAttachments
      if(p === P.MAX_DRAW_BUFFERS_WEBGL) return maxDrawBuffers
      if(p === P.MAX_RENDERBUFFER_SIZE) return 4096
      return bindings.get(p) || null
    },
    createFramebuffer: () => { framebuffersCreated++; return { fbo: ++serial } },
    deleteFramebuffer: () => {},
    createRenderbuffer: () => ({ rbo: ++serial }),
    deleteRenderbuffer: () => {},
    bindFramebuffer: (t, x) => { bindings.set(P.FRAMEBUFFER_BINDING, x) },
    bindRenderbuffer: (t, x) => { bindings.set(P.RENDERBUFFER_BINDING, x) },
    bindTexture: (t, x) => { bindings.set(P.TEXTURE_BINDING_2D, x) },
    renderbufferStorage: () => {},
    framebufferRenderbuffer: () => {},
    framebufferTexture2D: (target, attachment, textarget, texture, level) => log.push({
      name: 'framebufferTexture2D',
      attachment: attachment,
      texture: texture,
      framebuffer: bindings.get(P.FRAMEBUFFER_BINDING)
    }),
    checkFramebufferStatus: () => 10,
    viewport: (x, y, w, h) => log.push({ name: 'viewport', rect: [x, y, w, h] })
  }

  return {
    gl,
    log,
    drawBufferCalls: () => log.filter(e => e.name === 'drawBuffersWEBGL'),
    viewportCalls: () => log.filter(e => e.name === 'viewport'),
    framebuffersCreated: () => framebuffersCreated,
    currentFramebuffer: () => bindings.get(P.FRAMEBUFFER_BINDING) || null
  }
}

function makeKit(opts) {
  const rec = { allocated: [], disposed: [], serial: 0 }
  const createFBO = loadFBO(makeTextureFactory(rec))
  const ctx = makeContext(opts)
  return { rec, createFBO, ctx, gl: ctx.gl }
}

let failures = 0
function test(name, fn) {
  try {
    fn()
    console.log('ok - ' + name)
  } catch(e) {
    failures++
    console.error('not ok - ' + name)
    console.error(e && e.stack || e)
  }
}

const CA0 = 100
const NONE = 0

test('A/B targets interleave with independent masks and restored bindings', () => {
  const { createFBO, ctx, gl } = makeKit()
  const A = createFBO(gl, 8, 8, { color: 3, depth: false, drawBuffers: [true, false, true] })
  const B = createFBO(gl, 8, 8, { color: 2, depth: false, drawBuffers: [false, true] })

  //Creation routed each mask to its own framebuffer
  const created = ctx.drawBufferCalls()
  assert.strictEqual(created.length, 2)
  assert.deepStrictEqual(created[0].buffers, [CA0, NONE, CA0 + 2])
  assert.strictEqual(created[0].framebuffer, A.handle)
  assert.deepStrictEqual(created[1].buffers, [NONE, CA0 + 1])
  assert.strictEqual(created[1].framebuffer, B.handle)

  //Bind something unrelated, then interleave mask updates
  const external = { fbo: 999 }
  gl.bindFramebuffer(gl.FRAMEBUFFER, external)

  const viewportsBefore = ctx.viewportCalls().length
  A.setDrawBuffers([false, true, false])
  assert.strictEqual(ctx.currentFramebuffer(), external, 'A.setDrawBuffers must restore binding')
  B.setDrawBuffers([true, true])
  assert.strictEqual(ctx.currentFramebuffer(), external, 'B.setDrawBuffers must restore binding')
  assert.strictEqual(ctx.viewportCalls().length, viewportsBefore, 'setDrawBuffers must not touch viewport')

  const updates = ctx.drawBufferCalls().slice(2)
  assert.strictEqual(updates.length, 2)
  assert.deepStrictEqual(updates[0].buffers, [NONE, CA0 + 1])
  assert.strictEqual(updates[0].framebuffer, A.handle)
  assert.deepStrictEqual(updates[1].buffers, [CA0, CA0 + 1])
  assert.strictEqual(updates[1].framebuffer, B.handle)

  //Masks stay independent
  assert.deepStrictEqual(A.drawBuffers, [false, true, false])
  assert.deepStrictEqual(B.drawBuffers, [true, true])
})

test('middle attachment hole is not compacted (output 2 stays on attachment 2)', () => {
  const { createFBO, ctx, gl } = makeKit()
  const fbo = createFBO(gl, 4, 4, { color: 3, depth: false })
  fbo.setDrawBuffers([true, false, true])
  const call = ctx.drawBufferCalls().pop()
  assert.deepStrictEqual(call.buffers, [CA0, NONE, CA0 + 2])
  assert.strictEqual(call.buffers[2], CA0 + 2, 'fragment output 2 must not be routed to attachment 1')
  assert.strictEqual(call.buffers[1], NONE)

  //All disabled collapses to a single NONE
  fbo.setDrawBuffers([false, false, false])
  assert.deepStrictEqual(ctx.drawBufferCalls().pop().buffers, [NONE])
  assert.deepStrictEqual(fbo.drawBuffers, [false, false, false])
})

test('attachments keep identity and size; mask survives resize and bind', () => {
  const { rec, createFBO, ctx, gl } = makeKit()
  const fbo = createFBO(gl, 8, 4, { color: 3, depth: false, drawBuffers: [true, false, true] })

  //Texture stand-ins allocated per attachment with the right size
  assert.strictEqual(rec.allocated.length, 3)
  const attaches = ctx.log.filter(e => e.name === 'framebufferTexture2D')
  assert.strictEqual(attaches.length, 3)
  for(let i = 0; i < 3; ++i) {
    assert.strictEqual(attaches[i].attachment, CA0 + i)
    assert.strictEqual(attaches[i].texture, fbo.color[i].handle)
    assert.strictEqual(attaches[i].framebuffer, fbo.handle)
    assert.deepStrictEqual([fbo.color[i].width, fbo.color[i].height], [8, 4])
  }

  //Resize keeps the mask and does not re-route
  const callsBefore = ctx.drawBufferCalls().length
  fbo.shape = [16, 32]
  assert.deepStrictEqual(fbo.drawBuffers, [true, false, true])
  assert.strictEqual(ctx.drawBufferCalls().length, callsBefore, 'resize must not re-emit drawBuffers')
  for(let i = 0; i < 3; ++i) {
    assert.deepStrictEqual([fbo.color[i].width, fbo.color[i].height], [16, 32])
  }

  //Bind keeps the mask too
  fbo.bind()
  assert.deepStrictEqual(fbo.drawBuffers, [true, false, true])
})

test('mutating input or returned arrays does not leak into the FBO', () => {
  const { createFBO, gl } = makeKit()
  const mask = [true, false, true]
  const fbo = createFBO(gl, 4, 4, { color: 3, depth: false, drawBuffers: mask })
  mask[0] = false
  mask[2] = false
  assert.deepStrictEqual(fbo.drawBuffers, [true, false, true])

  const read = fbo.drawBuffers
  read[0] = false
  read[1] = true
  assert.deepStrictEqual(fbo.drawBuffers, [true, false, true])

  const update = [false, true, false]
  fbo.setDrawBuffers(update)
  update[0] = true
  assert.deepStrictEqual(fbo.drawBuffers, [false, true, false])
})

test('device with 4 attachments but only 2 draw buffers', () => {
  const { createFBO, ctx, gl } = makeKit({ maxColorAttachments: 4, maxDrawBuffers: 2 })

  //Three attachments with only the first enabled is fine
  const fbo = createFBO(gl, 4, 4, { color: 3, depth: false, drawBuffers: [true, false, false] })
  assert.deepStrictEqual(fbo.drawBuffers, [true, false, false])
  assert.deepStrictEqual(ctx.drawBufferCalls().pop().buffers, [CA0])

  //Two enabled slots also fit
  fbo.setDrawBuffers([false, true, false])
  assert.deepStrictEqual(ctx.drawBufferCalls().pop().buffers, [NONE, CA0 + 1])

  //Enabling the third slot exceeds MAX_DRAW_BUFFERS_WEBGL: throws, old mask kept
  assert.throws(() => fbo.setDrawBuffers([false, false, true]), /draw buffers/)
  assert.deepStrictEqual(fbo.drawBuffers, [false, true, false])
  assert.throws(() => fbo.setDrawBuffers([true, true, true]), /draw buffers/)
  assert.deepStrictEqual(fbo.drawBuffers, [false, true, false])

  //Implicit all-on default for three outputs is not expressible either
  assert.throws(() => createFBO(gl, 4, 4, { color: 3, depth: false }), /draw buffers/)
})

test('three enabled outputs succeed when limits allow', () => {
  const { createFBO, ctx, gl } = makeKit({ maxColorAttachments: 4, maxDrawBuffers: 4 })
  const fbo = createFBO(gl, 4, 4, { color: 3, depth: false })
  assert.deepStrictEqual(fbo.drawBuffers, [true, true, true])
  assert.deepStrictEqual(ctx.drawBufferCalls().pop().buffers, [CA0, CA0 + 1, CA0 + 2])
  fbo.setDrawBuffers([true, true, true])
  assert.deepStrictEqual(ctx.drawBufferCalls().pop().buffers, [CA0, CA0 + 1, CA0 + 2])
})

test('zero color attachments submit [NONE] and report an empty mask', () => {
  const { createFBO, ctx, gl } = makeKit()
  const fbo = createFBO(gl, 4, 4, { color: 0, depth: false })
  assert.deepStrictEqual(fbo.drawBuffers, [])
  assert.deepStrictEqual(ctx.drawBufferCalls().pop().buffers, [NONE])
  fbo.setDrawBuffers([])
  assert.deepStrictEqual(ctx.drawBufferCalls().pop().buffers, [NONE])
})

test('without WEBGL_draw_buffers only the default single output is allowed', () => {
  const { createFBO, ctx, gl } = makeKit({ drawBuffersExt: false })
  const fbo = createFBO(gl, 4, 4, { depth: false })
  assert.deepStrictEqual(fbo.drawBuffers, [true])
  assert.strictEqual(ctx.drawBufferCalls().length, 0, 'no drawBuffers calls without the extension')

  //Explicit default mask is fine too
  fbo.setDrawBuffers([true])
  assert.deepStrictEqual(fbo.drawBuffers, [true])

  //Masking the only output must fail, not pretend to succeed
  assert.throws(() => fbo.setDrawBuffers([false]), /WEBGL_draw_buffers/)
  assert.deepStrictEqual(fbo.drawBuffers, [true])

  //Zero color targets and multiple targets must fail too
  assert.throws(() => createFBO(gl, 4, 4, { color: 0, depth: false }), /WEBGL_draw_buffers/)
  assert.throws(() => createFBO(gl, 4, 4, { color: 2, depth: false }), /draw buffer/)
  assert.throws(() => createFBO(gl, 4, 4, { color: 1, depth: false, drawBuffers: [false] }), /WEBGL_draw_buffers/)
})

test('invalid masks are rejected before any allocation or GL call', () => {
  const { rec, createFBO, ctx, gl } = makeKit()

  assert.throws(() => createFBO(gl, 4, 4, { color: 2, depth: false, drawBuffers: [true] }), /length/)
  assert.throws(() => createFBO(gl, 4, 4, { color: 2, depth: false, drawBuffers: [true, true, true] }), /length/)
  assert.throws(() => createFBO(gl, 4, 4, { color: 2, depth: false, drawBuffers: [true, 1] }), /boolean/)
  assert.throws(() => createFBO(gl, 4, 4, { color: 2, depth: false, drawBuffers: 'yes' }), /array/)
  assert.throws(() => createFBO(gl, 4, 4, { color: 0, depth: false, drawBuffers: [true] }), /length/)

  assert.strictEqual(ctx.framebuffersCreated(), 0, 'no framebuffer allocated for invalid masks')
  assert.strictEqual(rec.allocated.length, 0, 'no texture allocated for invalid masks')
  assert.strictEqual(ctx.log.length, 0, 'no GL side effects for invalid masks')

  //Same for setDrawBuffers: old mask and attachments survive a failed set
  const fbo = createFBO(gl, 4, 4, { color: 2, depth: false, drawBuffers: [true, false] })
  const colors = fbo.color.slice()
  assert.throws(() => fbo.setDrawBuffers([true]), /length/)
  assert.throws(() => fbo.setDrawBuffers([true, 'x']), /boolean/)
  assert.throws(() => fbo.setDrawBuffers(null), /array/)
  assert.deepStrictEqual(fbo.drawBuffers, [true, false])
  assert.strictEqual(fbo.color[0], colors[0])
  assert.strictEqual(fbo.color[1], colors[1])
  assert.strictEqual(rec.disposed.length, 0, 'no attachment released by failed set')
})

test('capability state is not shared between contexts', () => {
  const rec = { allocated: [], disposed: [], serial: 0 }
  const createFBO = loadFBO(makeTextureFactory(rec))
  const wide = makeContext({ maxColorAttachments: 4, maxDrawBuffers: 4 })
  const narrow = makeContext({ maxColorAttachments: 2, maxDrawBuffers: 2 })

  //First context initializes any cached state with limit 4
  const a = createFBO(wide.gl, 2, 2, { color: 0, depth: false })
  assert.deepStrictEqual(wide.drawBufferCalls().pop().buffers, [NONE])

  //Second context must still see its own limit of 2
  assert.throws(() => createFBO(narrow.gl, 2, 2, { color: 3, depth: false }), /draw buffers/)
  const b = createFBO(narrow.gl, 2, 2, { color: 2, depth: false })
  assert.deepStrictEqual(narrow.drawBufferCalls().pop().buffers, [CA0, CA0 + 1])
  assert.deepStrictEqual(b.drawBuffers, [true, true])
  assert.deepStrictEqual(a.drawBuffers, [])
})

if(failures) {
  console.error(failures + ' test(s) failed')
  process.exit(1)
}
console.log('all tests passed')
