'use strict'

// Vertex input layout and binding tests for gl-shader.
//
// These tests drive the real library against a recording GL stub and
// assert on the actual WebGL call sequence, so a textual match against
// the source is never enough.  No browser or GPU is involved.

const assert = require('node:assert')
const fs = require('node:fs')
const path = require('node:path')
const vm = require('node:vm')

// Same in-process module loader as the baseline test: the compiler
// error formatter is only required on a failed compile, which never
// happens here, so its stub is unreachable in practice.
const modules = new Map()
function load(file) {
  file = path.resolve(file)
  if(modules.has(file)) return modules.get(file).exports
  const module = {exports: {}}
  modules.set(file, module)
  const req = name => name === 'gl-format-compiler-error'
    ? (() => { throw Error('unexpected compiler error') })
    : load(path.resolve(path.dirname(file), name) + '.js')
  vm.runInThisContext(
    '(function(require,module,exports){' + fs.readFileSync(file, 'utf8') + '\n})',
    {filename: file}
  )(req, module, module.exports)
  return module.exports
}
const createShader = load(path.join(__dirname, '../index.js'))

function createGL() {
  const gl = {
    VERTEX_SHADER: 1,
    FRAGMENT_SHADER: 2,
    COMPILE_STATUS: 3,
    LINK_STATUS: 4,
    ACTIVE_ATTRIBUTES: 5,
    ACTIVE_UNIFORMS: 6,
    FLOAT: 0x1406,

    //Mirrors the context's real enabled generic attribute slots.
    _enabled: new Set(),

    //Raw call records used by the assertions below.
    enableCalls: [],
    disableCalls: [],
    pointerCalls: [],
    attribCalls: [],
    bindAttribCalls: [],
    useProgramCalls: [],
    programs: []
  }

  gl.createShader = () => ({})
  gl.shaderSource = () => {}
  gl.compileShader = () => {}
  gl.getShaderParameter = () => true
  gl.isShader = () => true
  gl.deleteShader = () => {}

  gl.createProgram = () => {
    const program = {id: gl.programs.length}
    gl.programs.push(program)
    return program
  }
  gl.attachShader = () => {}
  gl.bindAttribLocation = (program, location, name) => {
    gl.bindAttribCalls.push([program, location, name])
  }
  gl.linkProgram = () => {}
  gl.getProgramParameter = () => true
  gl.isProgram = () => true
  gl.deleteProgram = () => {}
  gl.getUniformLocation = () => ({})

  gl.useProgram = program => gl.useProgramCalls.push(program)
  gl.enableVertexAttribArray = location => {
    gl._enabled.add(location)
    gl.enableCalls.push(location)
  }
  gl.disableVertexAttribArray = location => {
    gl._enabled.delete(location)
    gl.disableCalls.push(location)
  }
  gl.vertexAttribPointer = (...args) => gl.pointerCalls.push(args)

  const record = suffix => (location, value) => {
    gl.attribCalls.push(['vertexAttrib' + suffix, location, value])
  }
  gl.vertexAttrib1f = record('1f')
  gl.vertexAttrib1fv = record('1fv')
  gl.vertexAttrib2f = record('2f')
  gl.vertexAttrib2fv = record('2fv')
  gl.vertexAttrib3f = record('3f')
  gl.vertexAttrib3fv = record('3fv')
  gl.vertexAttrib4f = record('4f')
  gl.vertexAttrib4fv = record('4fv')

  return gl
}

function enabledSorted(gl) {
  return [...gl._enabled].sort((a, b) => a - b)
}

// ---------------------------------------------------------------------------
// 1. Matrix pointer layout: one pointer per column, everything in bytes.
// ---------------------------------------------------------------------------
function testMatrixLayout() {
  const gl = createGL()
  const shader = createShader(gl, 'v', 'f', [], [
    {name: 'position', type: 'vec3', location: 1},
    {name: 'model', type: 'mat3', location: 4}
  ])
  shader.bind()

  //A mat3 occupies three real slots; all of them must be enabled.
  assert.deepStrictEqual(enabledSorted(gl), [1, 4, 5, 6],
    'mat3 must enable all three column slots, not a single slot')

  shader.attributes.model.pointer(gl.FLOAT, false)
  assert.deepStrictEqual(gl.pointerCalls, [
    [4, 3, gl.FLOAT, false, 36, 0],
    [5, 3, gl.FLOAT, false, 36, 12],
    [6, 3, gl.FLOAT, false, 36, 24]
  ], 'packed FLOAT mat3: stride 36 bytes, column offsets 0/12/24')

  gl.pointerCalls.length = 0
  shader.attributes.model.pointer(gl.FLOAT, false, 64, 8)
  assert.deepStrictEqual(gl.pointerCalls.map(p => [p[0], p[4], p[5]]), [
    [4, 64, 8],
    [5, 64, 20],
    [6, 64, 32]
  ], 'user stride is already bytes: 64 stays 64, offsets 8/20/32')

  gl.pointerCalls.length = 0
  shader.attributes.model.pointer(gl.FLOAT, false, 0)
  assert.deepStrictEqual(gl.pointerCalls.map(p => [p[4], p[5]]), [
    [36, 0],
    [36, 12],
    [36, 24]
  ], 'stride 0 falls back to the packed byte stride')

  //Scalar/vector pointers keep passing stride/offset through verbatim.
  gl.pointerCalls.length = 0
  shader.attributes.position.pointer(gl.FLOAT, false, 12, 4)
  assert.deepStrictEqual(gl.pointerCalls, [
    [1, 3, gl.FLOAT, false, 12, 4]
  ], 'vec3 pointer keeps the existing byte convention')

  //mat2 and mat4 follow the same column major byte layout.
  const gl2 = createGL()
  const m2 = createShader(gl2, 'v', 'f', [], [
    {name: 'rot', type: 'mat2', location: 3}
  ])
  m2.bind()
  m2.attributes.rot.pointer(gl2.FLOAT, false)
  assert.deepStrictEqual(gl2.pointerCalls.map(p => [p[0], p[1], p[4], p[5]]), [
    [3, 2, 16, 0],
    [4, 2, 16, 8]
  ], 'packed FLOAT mat2: stride 16, offsets 0/8')

  const gl4 = createGL()
  const m4 = createShader(gl4, 'v', 'f', [], [
    {name: 'projection', type: 'mat4', location: 0}
  ])
  m4.bind()
  m4.attributes.projection.pointer(gl4.FLOAT, false)
  assert.deepStrictEqual(gl4.pointerCalls.map(p => [p[0], p[1], p[4], p[5]]), [
    [0, 4, 64, 0],
    [1, 4, 64, 16],
    [2, 4, 64, 32],
    [3, 4, 64, 48]
  ], 'packed FLOAT mat4: stride 64, offsets 0/16/32/48')

  //Scalar attribute pointer still passes size 1 through untouched.
  const glS = createGL()
  const scalar = createShader(glS, 'v', 'f', [], [
    {name: 'scale', type: 'float', location: 2}
  ])
  scalar.bind()
  scalar.attributes.scale.pointer(glS.FLOAT, false, 4, 0)
  assert.deepStrictEqual(glS.pointerCalls, [
    [2, 1, glS.FLOAT, false, 4, 0]
  ], 'float pointer keeps size 1 and byte stride')
}

// ---------------------------------------------------------------------------
// 2. Explicit, non-contiguous matrix locations [2,5,7].
// ---------------------------------------------------------------------------
function testNonContiguousLocations() {
  const gl = createGL()
  const shader = createShader(gl, 'v', 'f', [], [
    {name: 'model', type: 'mat3', location: [2, 5, 7]}
  ])
  shader.bind()

  assert.deepStrictEqual(enabledSorted(gl), [2, 5, 7],
    'only the explicitly assigned slots may be touched')
  for(const slot of [0, 1, 3, 4, 6]) {
    assert.ok(!gl.enableCalls.includes(slot),
      'slot ' + slot + ' must not be enabled from an ACTIVE_ATTRIBUTES guess')
  }

  //The program itself must be linked against the same slots.
  assert.deepStrictEqual(
    gl.bindAttribCalls.map(c => c[1]).sort((a, b) => a - b),
    [2, 5, 7],
    'bindAttribLocation must use the explicit non-contiguous slots'
  )

  shader.attributes.model.pointer(gl.FLOAT, false)
  assert.deepStrictEqual(gl.pointerCalls.map(p => [p[0], p[4], p[5]]), [
    [2, 36, 0],
    [5, 36, 12],
    [7, 36, 24]
  ], 'column pointers follow the explicit locations')
}

// ---------------------------------------------------------------------------
// 3. Two shaders with interleaved slots: creation is side effect free and
//    rebinding restores each shader's own slot set.
// ---------------------------------------------------------------------------
function testShaderSwitching() {
  const gl = createGL()
  //A uses 1/4/5/6, B uses 0/2 so the slot sets interleave.
  const a = createShader(gl, 'v', 'f', [], [
    {name: 'position', type: 'vec3', location: 1},
    {name: 'model', type: 'mat3', location: 4}
  ])
  const b = createShader(gl, 'v', 'f', [], [
    {name: 'p0', type: 'vec3', location: 0},
    {name: 'p1', type: 'vec3', location: 2}
  ])

  a.bind()
  assert.deepStrictEqual(enabledSorted(gl), [1, 4, 5, 6])

  //Merely creating B must not rewrite A's enabled set.
  const enabledAfterCreate = enabledSorted(gl)
  assert.deepStrictEqual(enabledAfterCreate, [1, 4, 5, 6],
    'creating shader B must leave shader A slots enabled')

  b.bind()
  assert.deepStrictEqual(enabledSorted(gl), [0, 2],
    'binding B enables 0/2 and retires every A-only slot')
  for(const slot of [1, 4, 5, 6]) {
    assert.ok(gl.disableCalls.includes(slot),
      'A slot ' + slot + ' must be disabled when B takes over')
  }

  a.bind()
  assert.deepStrictEqual(enabledSorted(gl), [1, 4, 5, 6],
    'binding A again must remember and restore its own slots')
  for(const slot of [0, 2]) {
    assert.ok(gl.disableCalls.includes(slot),
      'B slot ' + slot + ' must be disabled when A takes over')
  }
}

// ---------------------------------------------------------------------------
// 4. Constant vs array mode survives A/B/A switches, and pointer() turns a
//    constant back into an array attribute.
// ---------------------------------------------------------------------------
function testConstantAndArrayModes() {
  const gl = createGL()
  const a = createShader(gl, 'v', 'f', [], [
    {name: 'position', type: 'vec3', location: 1},
    {name: 'model', type: 'mat3', location: 4}
  ])
  const b = createShader(gl, 'v', 'f', [], [
    {name: 'p0', type: 'vec3', location: 0},
    {name: 'p1', type: 'vec3', location: 2}
  ])

  a.bind()
  //position becomes a constant; its slot stays disabled.
  a.attributes.position = [1, 2, 3]
  assert.deepStrictEqual(enabledSorted(gl), [4, 5, 6])
  assert.ok(gl.disableCalls.includes(1))
  assert.ok(gl.attribCalls.some(c =>
    c[0] === 'vertexAttrib3fv' && c[1] === 1 &&
    c[2].join(',') === '1,2,3'))

  //The whole matrix as a column major constant disables all three slots.
  gl.attribCalls.length = 0
  a.attributes.model = [
    1, 0, 0,
    0, 1, 0,
    0, 0, 1
  ]
  assert.deepStrictEqual(enabledSorted(gl), [])
  assert.deepStrictEqual(
    gl.attribCalls.map(c => [c[0], c[1]]),
    [
      ['vertexAttrib3fv', 4],
      ['vertexAttrib3fv', 5],
      ['vertexAttrib3fv', 6]
    ],
    'matrix constants are submitted one vertexAttrib call per column'
  )

  b.bind()
  assert.deepStrictEqual(enabledSorted(gl), [0, 2])

  //Switching back re-submits every A constant while keeping slots disabled.
  gl.attribCalls.length = 0
  a.bind()
  assert.deepStrictEqual(enabledSorted(gl), [],
    'constant slots must remain disabled after rebinding')
  //Re-submission order follows the shader's internal attribute order;
  //only the per-location values matter, so compare as an unordered set.
  assert.deepStrictEqual(
    gl.attribCalls
      .map(c => [c[0], c[1], c[2].join(',')])
      .sort((x, y) => x[1] - y[1]),
    [
      ['vertexAttrib3fv', 1, '1,2,3'],
      ['vertexAttrib3fv', 4, '1,0,0'],
      ['vertexAttrib3fv', 5, '0,1,0'],
      ['vertexAttrib3fv', 6, '0,0,1']
    ],
    'all constants (vector and matrix columns) are re-submitted on bind'
  )

  //pointer() restores array mode for the vector and every matrix column.
  a.attributes.position.pointer(gl.FLOAT)
  a.attributes.model.pointer(gl.FLOAT)
  b.bind()
  gl.enableCalls.length = 0
  gl.attribCalls.length = 0
  a.bind()
  assert.deepStrictEqual(enabledSorted(gl), [1, 4, 5, 6],
    'pointer() must restore array mode across shader switches')
  assert.deepStrictEqual([...new Set(gl.enableCalls)].sort((x, y) => x - y),
    [1, 4, 5, 6])
  const constantCallsWhileArray = gl.attribCalls.length
  assert.strictEqual(constantCallsWhileArray, 0,
    'array-mode attributes must not be re-submitted as constants')

  //A single matrix column can also be a constant on its own.
  a.attributes.model[1] = [7, 8, 9]
  b.bind()
  gl.attribCalls.length = 0
  a.bind()
  assert.deepStrictEqual(enabledSorted(gl), [1, 4, 6],
    'only the constant column slot 5 is disabled')
  assert.deepStrictEqual(gl.attribCalls, [
    ['vertexAttrib3fv', 5, [7, 8, 9]]
  ])
}

// ---------------------------------------------------------------------------
// 5. Disposing a shader that is not current never closes another shader's
//    slots.
// ---------------------------------------------------------------------------
function testDisposeSafety() {
  const gl = createGL()
  const a = createShader(gl, 'v', 'f', [], [
    {name: 'position', type: 'vec3', location: 1},
    {name: 'model', type: 'mat3', location: 4}
  ])
  const b = createShader(gl, 'v', 'f', [], [
    {name: 'p0', type: 'vec3', location: 0},
    {name: 'p1', type: 'vec3', location: 2}
  ])

  a.bind()
  assert.deepStrictEqual(enabledSorted(gl), [1, 4, 5, 6])

  //B was never bound: disposing it cannot touch A's live slots.
  gl.disableCalls.length = 0
  b.dispose()
  assert.deepStrictEqual(enabledSorted(gl), [1, 4, 5, 6],
    'disposing an unbound shader must leave active slots alone')
  assert.strictEqual(gl.disableCalls.length, 0)
  assert.doesNotThrow(() => a.bind(), 'the active shader stays usable')

  //Once B is current, disposing A must not close B's slots either.
  const gl2 = createGL()
  const a2 = createShader(gl2, 'v', 'f', [], [
    {name: 'position', type: 'vec3', location: 1}
  ])
  const b2 = createShader(gl2, 'v', 'f', [], [
    {name: 'p0', type: 'vec3', location: 0}
  ])
  a2.bind()
  b2.bind()
  assert.deepStrictEqual(enabledSorted(gl2), [0])
  gl2.disableCalls.length = 0
  a2.dispose()
  assert.deepStrictEqual(enabledSorted(gl2), [0],
    'disposing a non-current shader must not disable the current shader slot')
  assert.strictEqual(gl2.disableCalls.length, 0)

  //Two shaders sharing a physical slot: the active shader owns it.
  const gl3 = createGL()
  const shared = createShader(gl3, 'v', 'f', [], [
    {name: 'a', type: 'vec3', location: 1}
  ])
  const holder = createShader(gl3, 'v', 'f', [], [
    {name: 'b', type: 'vec3', location: 1}
  ])
  shared.bind()
  holder.bind()
  assert.deepStrictEqual(enabledSorted(gl3), [1])
  shared.dispose()
  assert.deepStrictEqual(enabledSorted(gl3), [1],
    'disposing a shader that never owned the shared slot keeps it enabled')
  assert.doesNotThrow(() => holder.bind())

  //Disposing the currently active shader does clean up its own slots.
  holder.dispose()
  assert.deepStrictEqual(enabledSorted(gl3), [],
    'the active shader can still release its own slots on dispose')
}

// ---------------------------------------------------------------------------
// 6. Runtime location changes: relink picks up the new slots while constant
//    and array modes keep working.
// ---------------------------------------------------------------------------
function testRuntimeRelocation() {
  const gl = createGL()
  const shader = createShader(gl, 'v', 'f', [], [
    {name: 'position', type: 'vec3', location: 1},
    {name: 'model', type: 'mat3', location: 4}
  ])
  shader.bind()

  //model becomes a constant before the relocation.
  shader.attributes.model = [
    1, 0, 0,
    0, 1, 0,
    0, 0, 1
  ]
  assert.deepStrictEqual(enabledSorted(gl), [1])

  //Move both attributes to a new non-contiguous layout.
  shader.attributes.position.location = 3
  shader.attributes.model.location = [2, 5, 7]
  assert.strictEqual(shader.program, null,
    'changing a location defers relinking until the next bind()')

  gl.attribCalls.length = 0
  shader.bind()
  assert.deepStrictEqual(
    gl.bindAttribCalls.slice(-4).map(c => c[1]).sort((a, b) => a - b),
    [2, 3, 5, 7],
    'the relinked program must use the new locations'
  )
  assert.deepStrictEqual(enabledSorted(gl), [3],
    'position is still array mode; matrix columns stay constant/disabled')
  assert.deepStrictEqual(
    gl.attribCalls.map(c => [c[1], c[2].join(',')]),
    [
      [2, '1,0,0'],
      [5, '0,1,0'],
      [7, '0,0,1']
    ],
    'matrix constants are re-submitted at the new column slots'
  )

  //Restoring array mode also targets the new slots.
  shader.attributes.model.pointer(gl.FLOAT)
  assert.deepStrictEqual(enabledSorted(gl), [2, 3, 5, 7])
  assert.deepStrictEqual(gl.pointerCalls.map(p => p[0]), [2, 5, 7])
}

// ---------------------------------------------------------------------------
// 7. Scalar and vector constants work for every dimension and are re-submitted
//    on every bind; nested matrix constants keep the column convention.
// ---------------------------------------------------------------------------
function testScalarAndVectorConstants() {
  const gl = createGL()
  const shader = createShader(gl, 'v', 'f', [], [
    {name: 's', type: 'float', location: 0},
    {name: 'v2', type: 'vec2', location: 1},
    {name: 'v3', type: 'vec3', location: 2},
    {name: 'v4', type: 'vec4', location: 3}
  ])
  shader.bind()

  shader.attributes.s = 2.5
  shader.attributes.v2 = [1, 2]
  shader.attributes.v3 = [3, 4, 5]
  shader.attributes.v4 = [6, 7, 8, 9]
  assert.deepStrictEqual(
    gl.attribCalls.map(c => c[0]),
    ['vertexAttrib1f', 'vertexAttrib2fv', 'vertexAttrib3fv', 'vertexAttrib4fv'],
    'each dimension must dispatch to its matching vertexAttrib entry'
  )
  assert.deepStrictEqual(enabledSorted(gl), [], 'all constant slots are disabled')

  //Repeated binds re-submit the stored constants unchanged.
  gl.attribCalls.length = 0
  shader.bind()
  assert.deepStrictEqual(
    gl.attribCalls
      .map(c => [c[0], c[1], Array.isArray(c[2]) ? c[2].join(',') : c[2]])
      .sort((a, b) => a[1] - b[1]),
    [
      ['vertexAttrib1f', 0, 2.5],
      ['vertexAttrib2fv', 1, '1,2'],
      ['vertexAttrib3fv', 2, '3,4,5'],
      ['vertexAttrib4fv', 3, '6,7,8,9']
    ]
  )

  //Nested (array of columns) matrix constants follow the same column order.
  const glm = createGL()
  const m = createShader(glm, 'v', 'f', [], [
    {name: 'model', type: 'mat2', location: [4, 6]}
  ])
  m.bind()
  m.attributes.model = [[1, 2], [3, 4]]
  assert.deepStrictEqual(
    glm.attribCalls.map(c => [c[1], c[2].join(',')]),
    [[4, '1,2'], [6, '3,4']],
    'nested matrix constants are applied column by column'
  )
  glm.attribCalls.length = 0
  m.bind()
  assert.deepStrictEqual(
    glm.attribCalls.map(c => [c[1], c[2].join(',')]),
    [[4, '1,2'], [6, '3,4']],
    'nested matrix constants are re-submitted on rebind'
  )
}

const tests = [
  ['matrix pointer layout (bytes, per column)', testMatrixLayout],
  ['non-contiguous matrix locations [2,5,7]', testNonContiguousLocations],
  ['A/B interleaved slot switching', testShaderSwitching],
  ['constant/array mode across switches', testConstantAndArrayModes],
  ['dispose does not touch foreign slots', testDisposeSafety],
  ['runtime relocation and relink', testRuntimeRelocation],
  ['scalar/vector constants for every dimension', testScalarAndVectorConstants]
]

let failures = 0
for(const [name, fn] of tests) {
  try {
    fn()
    console.log('ok - ' + name)
  } catch(err) {
    failures += 1
    console.log('not ok - ' + name)
    console.log(err.stack || err.message)
  }
}

if(failures) {
  console.log(failures + ' test(s) failed')
  process.exit(1)
}
console.log('all ' + tests.length + ' vertex input tests passed')
