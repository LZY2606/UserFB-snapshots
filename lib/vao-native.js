"use strict"

var bindSnapshot = require("./do-bind.js")
var applyConstants = bindSnapshot.applyConstants
var buildSnapshot = require("./snapshot.js")

var ACTIVE_KEY = "__gl_vao_native_active__"

function VAONative(gl, ext, handle) {
  this.gl = gl
  this._ext = ext
  this.handle = handle
  this._snapshot = null
}

VAONative.prototype.bind = function() {
  this._ext.bindVertexArrayOES(this.handle)
  // Constant attribute values are not stored in a native VAO, so they have
  // to be re-applied on every bind.
  applyConstants(this.gl, this._snapshot)
  this.gl[ACTIVE_KEY] = this
}

VAONative.prototype.unbind = function() {
  if(this.gl[ACTIVE_KEY] === this) {
    this.gl[ACTIVE_KEY] = null
  }
  this._ext.bindVertexArrayOES(null)
}

VAONative.prototype.dispose = function() {
  if(this.gl[ACTIVE_KEY] === this) {
    this.gl[ACTIVE_KEY] = null
  }
  this._ext.deleteVertexArrayOES(this.handle)
}

VAONative.prototype.update = function(attributes, elements, elementsType) {
  // Validate and snapshot first: if the description cannot be represented by
  // this backend, throw before binding any buffer or mutating this VAO.
  var snapshot = buildSnapshot(this.gl, attributes, elements, elementsType)

  var gl = this.gl
  var ext = this._ext
  var active = gl[ACTIVE_KEY] || null
  var wasActive = active === this
  var hasArrayBufferBinding = gl.ARRAY_BUFFER_BINDING !== undefined
  var previousArrayBuffer = hasArrayBufferBinding
    ? gl.getParameter(gl.ARRAY_BUFFER_BINDING)
    : null

  // Repopulate this VAO's own handle.  doBind never writes constant
  // attribute values, so an inactive VAO's update cannot leave its
  // constants in global state.
  ext.bindVertexArrayOES(this.handle)
  bindSnapshot(gl, snapshot)
  if(hasArrayBufferBinding) {
    gl.bindBuffer(gl.ARRAY_BUFFER, previousArrayBuffer)
  }

  if(wasActive) {
    // We were the active VAO: stay bound and make the new constants live
    // immediately, so the new description is the drawing state at once.
    applyConstants(gl, snapshot)
    gl[ACTIVE_KEY] = this
  } else if(active) {
    // Another VAO was active: rebind it and re-apply its constants.
    ext.bindVertexArrayOES(active.handle)
    applyConstants(gl, active._snapshot)
  } else {
    ext.bindVertexArrayOES(null)
  }

  this._snapshot = snapshot
}

VAONative.prototype.draw = function(mode, count, offset) {
  offset = offset || 0
  var gl = this.gl
  var snapshot = this._snapshot
  if(snapshot.elements) {
    gl.drawElements(mode, count, snapshot.elementsType, offset)
  } else {
    gl.drawArrays(mode, offset, count)
  }
}

function createVAONative(gl, ext) {
  return new VAONative(gl, ext, ext.createVertexArrayOES())
}

module.exports = createVAONative
