"use strict"

// Association/snapshot tests for gl-vao, run against three backends:
//   - WebGL2 core vertex array objects
//   - OES_vertex_array_object extension
//   - pure emulation (no native VAO support)
//
// The GL double below is a passive recorder: it logs state-changing calls and
// keeps the state those calls produce, but it never implements snapshotting or
// binding restoration on the library's behalf.

var assert = require("node:assert/strict")
var createVAO = require("../vao.js")

var ARRAY_BUFFER = 34962
var ELEMENT_ARRAY_BUFFER = 34963
var ARRAY_BUFFER_BINDING = 34964
var VERTEX_ARRAY_BINDING = 34922
var MAX_VERTEX_ATTRIBS = 34921
var FLOAT = 5126
var UNSIGNED_BYTE = 5121
var UNSIGNED_SHORT = 5123
var UNSIGNED_INT = 5125
var TRIANGLES = 4

var EMULATED_CURRENT_KEY = "__gl_vao_emulated_current__"

function createRecordingGL(mode) {
  var gl = {
    ARRAY_BUFFER: ARRAY_BUFFER,
    ELEMENT_ARRAY_BUFFER: ELEMENT_ARRAY_BUFFER,
    ARRAY_BUFFER_BINDING: ARRAY_BUFFER_BINDING,
    VERTEX_ARRAY_BINDING: VERTEX_ARRAY_BINDING,
    MAX_VERTEX_ATTRIBS: MAX_VERTEX_ATTRIBS,
    FLOAT: FLOAT,
    UNSIGNED_BYTE: UNSIGNED_BYTE,
    UNSIGNED_SHORT: UNSIGNED_SHORT,
    UNSIGNED_INT: UNSIGNED_INT,
    TRIANGLES: TRIANGLES
  }

  gl.calls = []
  gl.draws = []

  var arrayBufferHandle = null
  var arrayBufferWrapper = null
  var vaoBinding = null
  var globalElementBuffer = null
  var handleId = 0
  var wrapperByHandle = new Map()
  var globalPointers = new Map()
  var constantValues = new Map()

  function log(name, args) {
    gl.calls.push({ name: name, args: args.slice() })
  }

  function newHandle() {
    return {
      id: ++handleId,
      pointers: new Map(),
      enabled: new Set(),
      disabled: new Set(),
      elementBuffer: null
    }
  }

  gl.makeBuffer = function(name, asElements) {
    var buffer = { name: name, data: [], elements: !!asElements }
    buffer.handle = { __buffer: name }
    wrapperByHandle.set(buffer.handle, buffer)
    buffer.bind = function(target) {
      if(target === undefined) {
        target = buffer.elements ? ELEMENT_ARRAY_BUFFER : ARRAY_BUFFER
      }
      gl.bindBuffer(target, buffer.handle)
    }
    return buffer
  }

  gl.bindBuffer = function(target, handle) {
    log("bindBuffer", [target, handle])
    if(target === ARRAY_BUFFER) {
      arrayBufferHandle = handle
      arrayBufferWrapper = handle ? wrapperByHandle.get(handle) : null
    } else if(target === ELEMENT_ARRAY_BUFFER) {
      var wrapper = handle ? wrapperByHandle.get(handle) : null
      if(vaoBinding) {
        vaoBinding.elementBuffer = wrapper
      } else {
        globalElementBuffer = wrapper
      }
    }
  }

  gl.enableVertexAttribArray = function(location) {
    log("enableVertexAttribArray", [location])
    if(vaoBinding) vaoBinding.enabled.add(location)
  }

  gl.disableVertexAttribArray = function(location) {
    log("disableVertexAttribArray", [location])
    if(vaoBinding) vaoBinding.disabled.add(location)
  }

  gl.vertexAttribPointer = function(location, size, type, normalized, stride, offset) {
    log("vertexAttribPointer", [location, size, type, normalized, stride, offset])
    var pointer = {
      buffer: arrayBufferWrapper,
      size: size,
      type: type,
      normalized: normalized,
      stride: stride,
      offset: offset
    }
    globalPointers.set(location, pointer)
    if(vaoBinding) vaoBinding.pointers.set(location, pointer)
  }

  function recordConstant(name, location, values) {
    log(name, [location].concat(values))
    constantValues.set(location, values.slice())
  }
  gl.vertexAttrib1f = function(l, a) { recordConstant("vertexAttrib1f", l, [a]) }
  gl.vertexAttrib2f = function(l, a, b) { recordConstant("vertexAttrib2f", l, [a, b]) }
  gl.vertexAttrib3f = function(l, a, b, c) { recordConstant("vertexAttrib3f", l, [a, b, c]) }
  gl.vertexAttrib4f = function(l, a, b, c, d) {
    recordConstant("vertexAttrib4f", l, [a, b, c, d])
  }

  gl.drawArrays = function(mode, first, count) {
    gl.draws.push({ kind: "arrays", mode: mode, first: first, count: count })
  }
  gl.drawElements = function(mode, count, type, offset) {
    gl.draws.push({ kind: "elements", mode: mode, count: count, type: type, offset: offset })
  }

  function bindVertexArray(handle) {
    log("bindVertexArray", [handle])
    vaoBinding = handle
  }
  function deleteVertexArray() {
    log("deleteVertexArray", [])
  }

  gl.getParameter = function(param) {
    if(param === MAX_VERTEX_ATTRIBS) return 2
    if(param === ARRAY_BUFFER_BINDING) return arrayBufferHandle
    if(param === VERTEX_ARRAY_BINDING) return vaoBinding
    return null
  }

  if(mode === "webgl2") {
    gl.createVertexArray = function() {
      log("createVertexArray", [])
      return newHandle()
    }
    gl.bindVertexArray = bindVertexArray
    gl.deleteVertexArray = deleteVertexArray
    gl.getExtension = function() { return null }
  } else if(mode === "oes") {
    gl.getExtension = function(name) {
      if(name !== "OES_vertex_array_object") return null
      return {
        createVertexArrayOES: function() {
          log("createVertexArrayOES", [])
          return newHandle()
        },
        bindVertexArrayOES: bindVertexArray,
        deleteVertexArrayOES: deleteVertexArray
      }
    }
  } else {
    gl.getExtension = function() { return null }
  }

  // Observation helpers (read-only; the double implements no VAO behaviour).
  gl.pointer = function(location) {
    if(vaoBinding) return vaoBinding.pointers.get(location)
    return globalPointers.get(location)
  }
  gl.constant = function(location) {
    return constantValues.get(location)
  }
  gl.boundHandle = function() { return vaoBinding }
  gl.emulatedCurrent = function() { return gl[EMULATED_CURRENT_KEY] || null }
  gl.arrayBuffer = function() { return arrayBufferWrapper }
  gl.elementBuffer = function() {
    return vaoBinding ? vaoBinding.elementBuffer : globalElementBuffer
  }
  gl.lastCall = function(name) {
    for(var i=gl.calls.length-1; i>=0; --i) {
      if(gl.calls[i].name === name) return gl.calls[i]
    }
    return null
  }
  gl.lastConstantCall = function(location) {
    for(var i=gl.calls.length-1; i>=0; --i) {
      var call = gl.calls[i]
      if(/^vertexAttrib[1-4]f$/.test(call.name) && call.args[0] === location) return call
    }
    return null
  }
  gl.isNative = mode !== "emulated"
  gl.mode = mode
  return gl
}

function testSnapshotOnCreate(gl) {
  var buf = gl.makeBuffer("buf-a")
  var desc = [{ buffer: buf, size: 2 }, [7, 8]]
  var vaoA = createVAO(gl, desc)

  // Mutate everything the caller owns after creation.
  desc[0].offset = 8
  desc[0].size = 3
  desc[0].stride = 24
  desc[1].push(9)
  desc.push({ buffer: gl.makeBuffer("buf-z"), size: 1 })

  vaoA.bind()
  var pointer = gl.pointer(0)
  assert.equal(pointer.buffer, buf, "buffer resource stays by reference")
  assert.equal(pointer.offset, 0, "offset snapshotted")
  assert.equal(pointer.size, 2, "size snapshotted")
  assert.equal(pointer.stride, 0, "stride snapshotted")
  assert.equal(gl.pointer(2), undefined, "outer array length snapshotted")
  assert.deepEqual(gl.constant(1), [7, 8], "constant components snapshotted")
  assert.equal(gl.lastConstantCall(1).name, "vertexAttrib2f")

  // Reuse the (mutated) description for a second mesh.
  desc.length = 2
  var vaoB = createVAO(gl, desc)
  vaoB.bind()
  var pointerB = gl.pointer(0)
  assert.equal(pointerB.offset, 8)
  assert.equal(pointerB.size, 3)
  assert.deepEqual(gl.constant(1), [7, 8, 9])
  assert.equal(gl.lastConstantCall(1).name, "vertexAttrib3f")

  // The first VAO is still anchored to its own snapshot.
  vaoA.bind()
  assert.equal(gl.pointer(0).offset, 0)
  assert.equal(gl.pointer(0).size, 2)
  assert.deepEqual(gl.constant(1), [7, 8])
  assert.equal(gl.lastConstantCall(1).name, "vertexAttrib2f")
}

function testSnapshotOnUpdate(gl) {
  var buf = gl.makeBuffer("buf-update")
  var vao = createVAO(gl, [{ buffer: gl.makeBuffer("buf-old"), size: 2 }])
  var next = [{
    buffer: buf,
    size: 3,
    type: gl.UNSIGNED_BYTE,
    normalized: true,
    stride: 12,
    offset: 4
  }]
  vao.update(next)
  next[0].size = 1
  next[0].offset = 40
  next[0].normalized = false
  next[0] = { buffer: gl.makeBuffer("buf-replaced"), size: 1 }

  vao.bind()
  var pointer = gl.pointer(0)
  assert.equal(pointer.buffer, buf)
  assert.equal(pointer.size, 3)
  assert.equal(pointer.type, gl.UNSIGNED_BYTE)
  assert.equal(pointer.normalized, true)
  assert.equal(pointer.stride, 12)
  assert.equal(pointer.offset, 4)
}

function testFailedUpdateBetweenDraws(gl) {
  var buf = gl.makeBuffer("buf-fail")
  var vao = createVAO(gl, [{ buffer: buf, size: 2, offset: 0 }, [3, 4]])
  vao.bind()
  vao.draw(gl.TRIANGLES, 5)
  var firstDraw = gl.draws[0]

  var badDescriptions = [
    [{ buffer: buf, size: 1 }, { buffer: buf, size: 1 }, { buffer: buf, size: 1 }],
    [null],
    [undefined],
    [[1, 2, 3, 4, 5]],
    [[]],
    [{}]
  ]
  for(var i=0; i<badDescriptions.length; ++i) {
    var callsBefore = gl.calls.length
    assert.throws(function() { vao.update(badDescriptions[i]) },
      "bad description #" + i + " must be rejected")
    assert.equal(gl.calls.length, callsBefore,
      "rejected update #" + i + " must not issue GL calls")
  }

  // The previous successful description is still the drawing state.
  vao.draw(gl.TRIANGLES, 5)
  assert.equal(gl.draws.length, 2)
  assert.deepEqual(gl.draws[1], firstDraw)
  assert.equal(gl.pointer(0).offset, 0)
  assert.equal(gl.pointer(0).size, 2)
  assert.deepEqual(gl.constant(1), [3, 4])
}

function testInactiveUpdatePreservesActive(gl) {
  var bufA = gl.makeBuffer("buf-a")
  var bufB = gl.makeBuffer("buf-b")
  var indexBuffer = gl.makeBuffer("index", true)
  var spare = gl.makeBuffer("spare-array")

  var vaoA = createVAO(gl, [{ buffer: bufA, size: 2 }, [1, 1]], indexBuffer)
  vaoA.bind()
  var activeHandle = gl.boundHandle()

  // Caller-visible ARRAY_BUFFER binding before the inactive updates.
  spare.bind(gl.ARRAY_BUFFER)
  var arrayBufferBefore = gl.arrayBuffer()

  // Creating and updating B while A is active must not touch A's state.
  var vaoB = createVAO(gl, [[9, 9, 9]])
  vaoB.update([{ buffer: bufB, size: 1, offset: 4 }, [9]])

  if(gl.isNative) {
    assert.equal(gl.boundHandle(), activeHandle, "native A stays bound")
    assert.equal(gl.boundHandle(), vaoA.handle)
  } else {
    assert.equal(gl.emulatedCurrent(), vaoA, "emulated A stays current")
  }
  assert.equal(gl.arrayBuffer(), arrayBufferBefore, "ARRAY_BUFFER binding preserved")
  assert.equal(gl.constant(0), undefined, "B's constant was never applied")
  assert.deepEqual(gl.constant(1), [1, 1], "A's constant survives")

  // A rejected update on B is equally invisible.
  assert.throws(function() { vaoB.update([null]) })
  if(gl.isNative) assert.equal(gl.boundHandle(), vaoA.handle)
  assert.equal(gl.elementBuffer(), indexBuffer, "A's index buffer still associated")

  // Drawing A without an explicit rebind uses A's associations.
  gl.draws.length = 0
  vaoA.draw(gl.TRIANGLES, 6, 2)
  assert.equal(gl.draws.length, 1)
  assert.deepEqual(gl.draws[0], {
    kind: "elements", mode: gl.TRIANGLES, count: 6,
    type: gl.UNSIGNED_SHORT, offset: 2
  })
  assert.equal(gl.pointer(0).buffer, bufA)
  assert.equal(gl.pointer(0).size, 2)

  // Explicit bind/unbind sequencing still works and re-applies constants.
  vaoA.unbind()
  if(gl.isNative) assert.equal(gl.boundHandle(), null)
  else assert.equal(gl.emulatedCurrent(), null)
  vaoA.bind()
  assert.deepEqual(gl.constant(1), [1, 1])
}

function testActiveUpdateTakesEffectImmediately(gl) {
  var buf = gl.makeBuffer("buf-active")
  var vao = createVAO(gl, [{ buffer: gl.makeBuffer("buf-old"), size: 2 }])
  vao.bind()
  var handleBefore = gl.boundHandle()

  vao.update([{ buffer: buf, size: 1, offset: 16 }])

  if(gl.isNative) {
    assert.equal(gl.boundHandle(), handleBefore, "active VAO is not unbound")
    assert.equal(gl.boundHandle(), vao.handle)
  } else {
    assert.equal(gl.emulatedCurrent(), vao)
  }
  // New pointers are live without another bind().
  assert.deepEqual(gl.lastCall("vertexAttribPointer").args,
    [0, 1, gl.FLOAT, false, 0, 16])
  assert.equal(gl.pointer(0).offset, 16)
  assert.equal(gl.pointer(0).buffer, buf)
  gl.draws.length = 0
  vao.draw(gl.TRIANGLES, 4)
  assert.deepEqual(gl.draws[0], { kind: "arrays", mode: gl.TRIANGLES, first: 0, count: 4 })
}

function testSharedBufferContentChange(gl) {
  var shared = gl.makeBuffer("shared")
  var vaoA = createVAO(gl, [{ buffer: shared, size: 2, offset: 0 }])
  var vaoB = createVAO(gl, [{ buffer: shared, size: 2, offset: 8 }])

  // Simulate a GPU-side content update after both VAOs exist.
  shared.data.push(10, 20, 30)

  vaoA.bind()
  assert.equal(gl.pointer(0).buffer, shared)
  assert.equal(gl.pointer(0).offset, 0)
  assert.deepEqual(gl.pointer(0).buffer.data, [10, 20, 30])

  vaoB.bind()
  assert.equal(gl.pointer(0).buffer, shared)
  assert.equal(gl.pointer(0).offset, 8)
  assert.deepEqual(gl.pointer(0).buffer.data, [10, 20, 30])
}

function testConstantInterleaveAcrossObjects(gl) {
  var vaoP = createVAO(gl, [[1, 1]])
  var vaoQ = createVAO(gl, [[2, 2, 2]])

  vaoP.bind()
  assert.equal(gl.lastConstantCall(0).name, "vertexAttrib2f")
  assert.deepEqual(gl.constant(0), [1, 1])

  vaoQ.bind()
  assert.equal(gl.lastConstantCall(0).name, "vertexAttrib3f")
  assert.deepEqual(gl.constant(0), [2, 2, 2])

  vaoP.bind()
  assert.equal(gl.lastConstantCall(0).name, "vertexAttrib2f")
  assert.deepEqual(gl.constant(0), [1, 1])

  // Updating inactive Q must not leave Q's constants in global state.
  vaoP.bind()
  vaoQ.update([[5, 5, 5, 5]])
  assert.deepEqual(gl.constant(0), [1, 1], "P's constant restored after Q.update")
  assert.equal(gl.lastConstantCall(0).name, "vertexAttrib2f")

  vaoQ.bind()
  assert.deepEqual(gl.constant(0), [5, 5, 5, 5])
  assert.equal(gl.lastConstantCall(0).name, "vertexAttrib4f")
}

function testDrawRoutingAndElementType(gl) {
  var buf = gl.makeBuffer("buf-draw")
  var indices = gl.makeBuffer("indices", true)

  var withElements = createVAO(gl, [{ buffer: buf, size: 2 }], indices)
  withElements.bind()
  withElements.draw(gl.TRIANGLES, 3, 4)
  assert.deepEqual(gl.draws[0], {
    kind: "elements", mode: gl.TRIANGLES, count: 3,
    type: gl.UNSIGNED_SHORT, offset: 4
  })

  var explicitType = createVAO(gl, [{ buffer: buf, size: 2 }], indices, gl.UNSIGNED_INT)
  explicitType.bind()
  explicitType.draw(gl.TRIANGLES, 9)
  assert.deepEqual(gl.draws[1], {
    kind: "elements", mode: gl.TRIANGLES, count: 9,
    type: gl.UNSIGNED_INT, offset: 0
  })

  var arraysOnly = createVAO(gl, [{ buffer: buf, size: 2 }])
  arraysOnly.bind()
  arraysOnly.draw(gl.TRIANGLES, 3, 2)
  assert.deepEqual(gl.draws[2], {
    kind: "arrays", mode: gl.TRIANGLES, first: 2, count: 3
  })
}

var CASES = [
  ["description reuse snapshots every input", testSnapshotOnCreate],
  ["update snapshots every input", testSnapshotOnUpdate],
  ["rejected update between draws keeps previous state", testFailedUpdateBetweenDraws],
  ["inactive create/update preserves active VAO", testInactiveUpdatePreservesActive],
  ["active update takes effect without unbinding", testActiveUpdateTakesEffectImmediately],
  ["shared buffer content stays visible through references", testSharedBufferContentChange],
  ["constant attributes interleave across objects", testConstantInterleaveAcrossObjects],
  ["drawArrays/drawElements routing and elementsType", testDrawRoutingAndElementType]
]

var backends = [
  ["webgl2-core", "webgl2"],
  ["oes-extension", "oes"],
  ["emulated", "emulated"]
]

var totalFailures = 0
for(var b=0; b<backends.length; ++b) {
  var label = backends[b][0]
  var mode = backends[b][1]
  var passed = 0
  for(var c=0; c<CASES.length; ++c) {
    var gl = createRecordingGL(mode)
    try {
      CASES[c][1](gl)
      passed += 1
    } catch(err) {
      totalFailures += 1
      console.log("  FAIL [" + label + "] " + CASES[c][0] + ": " + err.message)
    }
  }
  console.log(label + ": " + passed + "/" + CASES.length + " cases passed")
}

if(totalFailures > 0) {
  console.log(totalFailures + " case(s) failed")
  process.exit(1)
}
