"use strict"

var createVAONative = require("./lib/vao-native.js")
var createVAOEmulated = require("./lib/vao-emulated.js")

function ExtensionShim (gl) {
  this.bindVertexArrayOES = gl.bindVertexArray.bind(gl)
  this.createVertexArrayOES = gl.createVertexArray.bind(gl)
  this.deleteVertexArrayOES = gl.deleteVertexArray.bind(gl)
}

function createVAO(gl, attributes, elements, elementsType) {
  var ext = gl.createVertexArray
    ? new ExtensionShim(gl)
    : gl.getExtension('OES_vertex_array_object')
  var vao = ext
    ? createVAONative(gl, ext)
    : createVAOEmulated(gl)

  try {
    vao.update(attributes, elements, elementsType)
  } catch(err) {
    // Validation failed before any state was applied: release the unused
    // native handle instead of leaking it.
    vao.dispose()
    throw err
  }
  return vao
}

module.exports = createVAO
