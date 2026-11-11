// Offline test for GPU timer query epoch handling.
//
// Covers, against the real lib/timer.js driven by a scripted
// EXT_disjoint_timer_query stand-in:
//   - partial / out-of-order query results (no busy-waiting in update)
//   - nested and overlapping scopes with independent sums
//   - GPU_DISJOINT_EXT invalidation (results unread, records dropped)
//   - restore() and clear() ending the attribution of old records
//   - recovery of normal timing after disjoint / restore / clear
// and, through the real regl command compilation entry point with a
// minimal recording GL fixture:
//   - the gpuTimeDropped stats field on compiled commands
//
// Run with: node test/offline/timer-epochs.test.cjs

const assert = require('node:assert/strict')
const createTimer = require('../../lib/timer')
const createREGL = require('../../regl')

const GL_QUERY_RESULT_EXT = 0x8866
const GL_QUERY_RESULT_AVAILABLE_EXT = 0x8867
const GL_GPU_DISJOINT_EXT = 0x8FBB

let passCount = 0
function ok (cond, msg) {
  assert.ok(cond, msg)
  passCount++
}

// ---------------------------------------------------------------------------
// Scripted timer extension.  The test controls, per query, when it becomes
// available and which result it reports, plus the global disjoint flag.
// All summation/attribution happens inside the real timer module.
// ---------------------------------------------------------------------------
function createTimerExt () {
  const state = {
    disjoint: false,
    created: 0,
    queries: new Map(), // id -> query object
    begun: [],          // query id of every beginQueryEXT call, in order
    reads: [],          // {id, pname} of every getQueryObjectEXT call
    deletes: []         // query id of every deleteQueryEXT call
  }
  const ext = {
    GPU_DISJOINT_EXT: GL_GPU_DISJOINT_EXT,
    createQueryEXT () {
      const query = { id: ++state.created, available: false, result: 0 }
      state.queries.set(query.id, query)
      return query
    },
    beginQueryEXT (target, query) {
      state.begun.push(query.id)
      query.available = false
      query.result = 0
    },
    endQueryEXT () {},
    getQueryObjectEXT (query, pname) {
      state.reads.push({ id: query.id, pname: pname })
      return pname === GL_QUERY_RESULT_AVAILABLE_EXT
        ? query.available
        : query.result
    },
    deleteQueryEXT (query) {
      state.deletes.push(query.id)
    }
  }
  return { state, ext }
}

// Mark the query used by the n-th beginQuery (0-based) as available
// with a result of `ms` milliseconds.
function setResult (state, beginIndex, ms) {
  const query = state.queries.get(state.begun[beginIndex])
  query.available = true
  query.result = ms * 1e6
}

function makeStats () {
  return { gpuTime: 0, gpuTimeDropped: 0, cpuTime: 0, count: 0 }
}

function createBackend () {
  const backend = createTimerExt()
  const gl = {
    getParameter (pname) {
      if (pname === GL_GPU_DISJOINT_EXT) {
        return backend.state.disjoint
      }
      return 0
    }
  }
  backend.timer = createTimer(gl, { ext_disjoint_timer_query: backend.ext })
  return backend
}

// ---------------------------------------------------------------------------
// 1. Partial and out-of-order results; a single update must not busy-wait.
// ---------------------------------------------------------------------------
{
  const b = createBackend()
  const a = makeStats()
  const c = makeStats()
  b.timer.beginQuery(a); b.timer.endQuery()
  b.timer.beginQuery(c); b.timer.endQuery()

  b.timer.update() // nothing available yet: returns immediately
  ok(a.gpuTime === 0 && c.gpuTime === 0, 'no results yet, no time published')
  ok(b.timer.getNumPendingQueries() === 2, 'both queries still pending')

  setResult(b.state, 1, 2) // second query completes first
  b.timer.update()
  ok(a.gpuTime === 0, 'first command still waiting')
  ok(c.gpuTime === 2, 'second command settled out of order')
  ok(b.timer.getNumPendingQueries() === 1, 'one query still pending')

  setResult(b.state, 0, 3)
  b.timer.update()
  ok(a.gpuTime === 3, 'first command settled on later frame')
  ok(b.timer.getNumPendingQueries() === 0, 'no queries left pending')
  ok(a.gpuTimeDropped === 0 && c.gpuTimeDropped === 0, 'nothing dropped')
  console.log('ok - partial and out-of-order results')
}

// ---------------------------------------------------------------------------
// 2. Nested and overlapping scopes accumulate independently.
// ---------------------------------------------------------------------------
{
  const b = createBackend()
  const cmd1 = makeStats()
  const cmd2 = makeStats()
  const inner = makeStats()
  const outer = makeStats()

  const outerStart = b.timer.getNumPendingQueries()
  b.timer.beginQuery(cmd1); b.timer.endQuery()
  const innerStart = b.timer.getNumPendingQueries()
  b.timer.beginQuery(cmd2); b.timer.endQuery()
  b.timer.pushScopeStats(innerStart, b.timer.getNumPendingQueries(), inner)
  b.timer.pushScopeStats(outerStart, b.timer.getNumPendingQueries(), outer)

  setResult(b.state, 0, 1)
  setResult(b.state, 1, 2)
  b.timer.update()

  ok(cmd1.gpuTime === 1, 'first command gets its own query')
  ok(cmd2.gpuTime === 2, 'second command gets its own query')
  ok(inner.gpuTime === 2, 'inner scope covers only the second query')
  ok(outer.gpuTime === 3, 'outer scope covers both queries')
  ok(b.timer.getNumPendingQueries() === 0, 'no queries left pending')
  console.log('ok - nested and overlapping scopes')
}

// ---------------------------------------------------------------------------
// 3. Disjoint invalidation: pending results are dropped unread, partial
//    scope sums are discarded, settled values are kept, timing recovers.
// ---------------------------------------------------------------------------
{
  const b = createBackend()

  const settled = makeStats()
  b.timer.beginQuery(settled); b.timer.endQuery()
  setResult(b.state, 0, 2)
  b.timer.update()
  ok(settled.gpuTime === 2, 'command settled before the disjoint')

  const d = makeStats()
  const e = makeStats()
  const m = makeStats()
  const scope = makeStats()
  const empty = makeStats()
  const scopeStart = b.timer.getNumPendingQueries()
  b.timer.beginQuery(d); b.timer.endQuery()
  b.timer.beginQuery(e); b.timer.endQuery()
  b.timer.beginQuery(m); b.timer.endQuery()
  b.timer.beginQuery(m); b.timer.endQuery() // same stats, second record
  b.timer.pushScopeStats(scopeStart, b.timer.getNumPendingQueries(), scope)

  setResult(b.state, 1, 4) // only d's query completes
  b.timer.update()
  ok(d.gpuTime === 4, 'd settled before the disjoint')
  ok(scope.gpuTime === 0, 'partial scope sum not yet published')
  ok(b.timer.getNumPendingQueries() === 3, 'three queries still pending')

  // an empty scope covers no queries
  const emptyStart = b.timer.getNumPendingQueries()
  b.timer.pushScopeStats(emptyStart, emptyStart, empty)

  b.state.disjoint = true
  const readsBefore = b.state.reads.length
  b.timer.update()
  ok(b.state.reads.length === readsBefore, 'disjoint update reads no results')
  ok(b.state.deletes.length === 0, 'disjoint update deletes nothing')
  ok(settled.gpuTime === 2, 'earlier settled command kept')
  ok(d.gpuTime === 4, 'earlier settled command in same epoch kept')
  ok(e.gpuTime === 0 && e.gpuTimeDropped === 1, 'pending command dropped once')
  ok(m.gpuTime === 0 && m.gpuTimeDropped === 2, 'two independent records counted separately')
  ok(scope.gpuTime === 0 && scope.gpuTimeDropped === 1, 'partial scope sum discarded and counted')
  ok(empty.gpuTimeDropped === 0, 'empty scope has no queries and is not counted')
  ok(b.timer.getNumPendingQueries() === 0, 'no queries pending after disjoint')

  b.state.disjoint = false
  const f = makeStats()
  b.timer.beginQuery(f); b.timer.endQuery()
  setResult(b.state, 5, 1)
  b.timer.update()
  ok(f.gpuTime === 1 && f.gpuTimeDropped === 0, 'timing recovers after disjoint clears')
  ok(b.timer.getNumPendingQueries() === 0, 'no queries left pending')
  console.log('ok - disjoint invalidation and recovery')
}

// ---------------------------------------------------------------------------
// 4. restore(): ends attribution of old records without touching the dead
//    extension handle; the next epoch is timed cleanly.
// ---------------------------------------------------------------------------
{
  const b = createBackend()
  const a = makeStats()
  const c = makeStats()
  const scope = makeStats()
  const scopeStart = b.timer.getNumPendingQueries()
  b.timer.beginQuery(a); b.timer.endQuery()
  b.timer.beginQuery(c); b.timer.endQuery()
  b.timer.pushScopeStats(scopeStart, b.timer.getNumPendingQueries(), scope)

  b.timer.restore()
  ok(a.gpuTimeDropped === 1, 'first command record dropped on restore')
  ok(c.gpuTimeDropped === 1, 'second command record dropped on restore')
  ok(scope.gpuTimeDropped === 1, 'scope record dropped on restore')
  ok(b.state.deletes.length === 0, 'restore deletes nothing (context is lost)')
  ok(b.state.reads.length === 0, 'restore reads nothing (context is lost)')
  ok(b.timer.getNumPendingQueries() === 0, 'no queries pending after restore')

  const fresh = makeStats()
  b.timer.beginQuery(fresh); b.timer.endQuery()
  setResult(b.state, 2, 2)
  b.timer.update()
  ok(fresh.gpuTime === 2 && fresh.gpuTimeDropped === 0, 'new epoch timed normally')
  ok(a.gpuTime === 0 && c.gpuTime === 0 && scope.gpuTime === 0,
    'old records do not absorb queries from the new epoch')
  ok(b.timer.getNumPendingQueries() === 0, 'no queries left pending')
  console.log('ok - restore ends old attribution, next epoch recovers')
}

// ---------------------------------------------------------------------------
// 5. clear(): deletes every live query (pending + pooled) exactly once,
//    repeated clear deletes nothing, old records cannot absorb new queries.
// ---------------------------------------------------------------------------
{
  const b = createBackend()

  const x = makeStats()
  b.timer.beginQuery(x); b.timer.endQuery()
  setResult(b.state, 0, 1)
  b.timer.update() // x settles, its query goes back to the pool
  ok(x.gpuTime === 1, 'first command settled')

  const y = makeStats()
  const z = makeStats()
  const w = makeStats()
  b.timer.beginQuery(y); b.timer.endQuery() // reuses the pooled query
  b.timer.beginQuery(z); b.timer.endQuery()
  b.timer.beginQuery(w); b.timer.endQuery()
  setResult(b.state, 3, 4)
  b.timer.update() // w settles, its query goes back to the pool
  ok(w.gpuTime === 4, 'third command settled')
  ok(b.state.deletes.length === 0, 'nothing deleted before clear')

  b.timer.clear()
  const deleted = b.state.deletes.slice().sort((p, q) => p - q)
  assert.deepEqual(deleted, [1, 2, 3])
  ok(true, 'clear deletes each pooled and pending query exactly once')
  ok(y.gpuTimeDropped === 1 && z.gpuTimeDropped === 1, 'pending records dropped on clear')
  ok(x.gpuTimeDropped === 0 && w.gpuTimeDropped === 0, 'settled records not dropped on clear')

  b.timer.clear()
  ok(b.state.deletes.length === 3, 'repeated clear deletes nothing more')

  const v = makeStats()
  b.timer.beginQuery(v); b.timer.endQuery()
  setResult(b.state, 4, 8)
  b.timer.update()
  ok(v.gpuTime === 8 && v.gpuTimeDropped === 0, 'timing works after clear')
  ok(y.gpuTime === 0 && z.gpuTime === 0, 'dropped records stay at zero')
  ok(b.state.deletes.length === 3, 'normal updates never delete')
  ok(b.timer.getNumPendingQueries() === 0, 'no queries left pending')
  console.log('ok - clear deletes each query exactly once')
}

// ---------------------------------------------------------------------------
// 6. Through the real regl compile entry point with a minimal recording GL
//    fixture: the gpuTimeDropped stats field is initialized and tracked.
// ---------------------------------------------------------------------------
{
  const backend = createTimerExt()
  const state = backend.state

  const GL_LIMITS = {
    0x0D50: 4, // SUBPIXEL_BITS
    0x0D52: 8, 0x0D53: 8, 0x0D54: 8, 0x0D55: 8, // RED/GREEN/BLUE/ALPHA_BITS
    0x0D56: 24, 0x0D57: 8, // DEPTH/STENCIL_BITS
    0x846D: [1, 1024], 0x846E: [1, 1024], // ALIASED_POINT_SIZE/LINE_WIDTH_RANGE
    0x0D33: 4096, // MAX_TEXTURE_SIZE
    0x0D3A: [4096, 4096], // MAX_VIEWPORT_DIMS
    0x8869: 16, // MAX_VERTEX_ATTRIBS
    0x8DFB: 128, 0x8DFC: 16, 0x8B4D: 16, 0x8B4C: 16, 0x8872: 16, 0x8DFD: 64,
    0x851C: 4096, 0x84E8: 4096,
    0x1F00: 'regl-fixture', 0x1F01: 'regl-fixture',
    0x1F02: 'WebGL 1.0', 0x8B8C: 'WebGL GLSL ES 1.0'
  }

  const calls = []
  const target = {
    canvas: null,
    drawingBufferWidth: 64,
    drawingBufferHeight: 64,
    COMPILE_STATUS: 0x8B81,
    LINK_STATUS: 0x8B82,
    FRAGMENT_SHADER: 0x8B30,
    VERTEX_SHADER: 0x8B31,
    getExtension (name) {
      return name === 'ext_disjoint_timer_query' ? backend.ext : null
    },
    getParameter (pname) {
      if (pname === GL_GPU_DISJOINT_EXT) {
        return state.disjoint
      }
      return GL_LIMITS[pname] !== undefined ? GL_LIMITS[pname] : 0
    },
    getShaderParameter () { return true },
    getProgramParameter (program, pname) {
      // no active uniforms or attributes
      if (pname === 0x8B86 || pname === 0x8B89) { return 0 }
      return true
    },
    getActiveUniform () { return null },
    getActiveAttrib () { return null },
    getUniformLocation () { return {} },
    getAttribLocation () { return 0 },
    getContextAttributes () {
      return {
        alpha: true,
        depth: true,
        stencil: false,
        antialias: false,
        premultipliedAlpha: true,
        preserveDrawingBuffer: false
      }
    },
    isContextLost () { return false },
    getError () { return 0 },
    getShaderInfoLog () { return '' },
    getProgramInfoLog () { return '' },
    checkFramebufferStatus () { return 0x8CD5 },
    createShader () { return { shader: true } },
    createProgram () { return { program: true } },
    createBuffer () { return { buffer: true } },
    createTexture () { return { texture: true } },
    createFramebuffer () { return { framebuffer: true } },
    createRenderbuffer () { return { renderbuffer: true } }
  }
  // any other WebGL entry point is a recorded no-op
  const gl = new Proxy(target, {
    get (t, prop) {
      if (prop in t) { return t[prop] }
      if (typeof prop !== 'string') { return undefined }
      const f = function () { calls.push(prop) }
      t[prop] = f
      return f
    }
  })

  const regl = createREGL({
    gl: gl,
    optionalExtensions: ['ext_disjoint_timer_query']
  })
  ok(!!regl, 'regl created on recording fixture')

  const draw = regl({
    frag: 'void main() { gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0); }',
    vert: 'void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }',
    count: 3,
    profile: true
  })
  ok(typeof draw.stats.gpuTimeDropped === 'number', 'compiled command stats carry gpuTimeDropped')
  ok(draw.stats.gpuTimeDropped === 0, 'gpuTimeDropped starts at 0')

  draw()
  setResult(state, 0, 2)
  regl.poll()
  ok(draw.stats.gpuTime === 2, 'compiled command accumulates gpuTime')
  ok(draw.stats.gpuTimeDropped === 0, 'nothing dropped yet')
  ok(draw.stats.count === 1, 'count tracked')

  // disjoint invalidates the in-flight query
  draw()
  state.disjoint = true
  regl.poll()
  ok(draw.stats.gpuTime === 2, 'invalid query time not added to gpuTime')
  ok(draw.stats.gpuTimeDropped === 1, 'invalid record counted as dropped')

  // timing recovers once the disjoint clears
  state.disjoint = false
  draw()
  setResult(state, 2, 3)
  regl.poll()
  ok(draw.stats.gpuTime === 5, 'timing recovers after disjoint')
  ok(draw.stats.gpuTimeDropped === 1, 'drop count unchanged')

  // batch: one timer subscription per batch invocation
  draw(2)
  setResult(state, 3, 4)
  regl.poll()
  ok(draw.stats.gpuTime === 9, 'batch accumulates gpuTime once')
  ok(draw.stats.count === 5, 'batch increments count per invocation')

  // scope: overlapping subscription with an independent sum
  const scoped = regl({ profile: true })
  scoped(function () { draw() })
  setResult(state, 4, 1)
  regl.poll()
  ok(draw.stats.gpuTime === 10, 'inner command gets its own time')
  ok(scoped.stats.gpuTime === 1, 'scope covers the nested command query')
  ok(scoped.stats.gpuTimeDropped === 0, 'scope dropped count starts at 0')

  // destroy clears the pending query and drops its record
  draw()
  regl.destroy()
  ok(state.deletes.length === 1, 'destroy deletes the pending query exactly once')
  ok(draw.stats.gpuTimeDropped === 2, 'destroyed pending record counted as dropped')
  console.log('ok - regl compile entry exposes gpuTimeDropped')
}

console.log(
  'timer-epochs: ' + passCount + ' checks passed, ' +
  'dropped accounted, final pending queries = 0')
