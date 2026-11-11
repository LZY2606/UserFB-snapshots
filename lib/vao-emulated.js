"use strict"

var bindSnapshot = require("./do-bind.js")
var applyConstants = bindSnapshot.applyConstants
var buildSnapshot = require("./snapshot.js")

var CURRENT_KEY = "__gl_vao_emulated_current__"

function VAOEmulated(gl) {
  this.gl = gl
  this._snapshot = null
}

VAOEmulated.prototype.bind = function() {
  this.gl[CURRENT_KEY] = this
  var gl = this.gl
  var hasArrayBufferBinding = gl.ARRAY_BUFFER_BINDING !== undefined
  var previousArrayBuffer = hasArrayBufferBinding
    ? gl.getParameter(gl.ARRAY_BUFFER_BINDING)
    : null
  bindSnapshot(gl, this._snapshot)
  applyConstants(gl, this._snapshot)
  if(hasArrayBufferBinding) {
    gl.bindBuffer(gl.ARRAY_BUFFER, previousArrayBuffer)
  }
}

VAOEmulated.prototype.unbind = function() {
  if(this.gl[CURRENT_KEY] === this) {
    this.gl[CURRENT_KEY] = null
  }
}

VAOEmulated.prototype.update = function(attributes, elements, elementsType) {
  // Build and validate the whole snapshot before touching any GL state,
  // so a rejected update leaves both this object and the current bindings
  // on the previous description.
  var snapshot = buildSnapshot(this.gl, attributes, elements, elementsType)
  this._snapshot = snapshot
  if(this.gl[CURRENT_KEY] === this) {
    // This VAO is the active one: new description must take effect at once,
    // not only on the next bind().
    this.bind()
  }
}

VAOEmulated.prototype.dispose = function() {
  this.unbind()
}

VAOEmulated.prototype.draw = function(mode, count, offset) {
  offset = offset || 0
  var gl = this.gl
  var snapshot = this._snapshot
  if(snapshot.elements) {
    gl.drawElements(mode, count, snapshot.elementsType, offset)
  } else {
    gl.drawArrays(mode, offset, count)
  }
}

function createVAOEmulated(gl) {
  return new VAOEmulated(gl)
}

module.exports = createVAOEmulated
