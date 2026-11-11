"use strict"

// Validate and snapshot a vertex array description.
//
// The outer attributes array, every buffer descriptor and every constant
// value array are copied, so later mutation of the caller's arrays can never
// reach an already created/updated VAO.  Buffer and elements resources are
// kept by reference: their GPU contents are not copied.
//
// Validation happens entirely on the JS side before any GL state changes,
// so a rejected update leaves the previous description intact.

function Snapshot(attributes, elements, elementsType) {
  this.attributes = attributes
  this.elements = elements || null
  this.elementsType = elementsType
}

function fail(message) {
  throw new Error("gl-vao: " + message)
}

function validateConstant(attrib) {
  if(typeof attrib === "number") {
    return { constant: true, dimension: 1, values: [attrib] }
  }
  if(Array.isArray(attrib)) {
    var n = attrib.length
    if(n < 1 || n > 4) {
      fail("Invalid vertex attribute")
    }
    var values = new Array(n)
    for(var j=0; j<n; ++j) {
      values[j] = attrib[j]
    }
    return { constant: true, dimension: n, values: values }
  }
  fail("Invalid vertex attribute")
}

function buildSnapshot(gl, attributes, elements, elementsType) {
  if(attributes === null || attributes === undefined) {
    attributes = []
  } else if(!Array.isArray(attributes)) {
    fail("attributes must be an array")
  }

  var maxAttribs = gl.getParameter(gl.MAX_VERTEX_ATTRIBS) | 0
  if(attributes.length > maxAttribs) {
    fail("Too many vertex attributes")
  }

  var copy = new Array(attributes.length)
  for(var i=0; i<attributes.length; ++i) {
    var attrib = attributes[i]
    if(attrib === null || attrib === undefined) {
      fail("Empty vertex attribute at index " + i)
    }
    if(attrib.buffer) {
      copy[i] = {
        buffer: attrib.buffer,
        size: attrib.size || 4,
        type: attrib.type || gl.FLOAT,
        normalized: !!attrib.normalized,
        stride: attrib.stride || 0,
        offset: attrib.offset || 0
      }
    } else {
      copy[i] = validateConstant(attrib)
    }
  }

  return new Snapshot(copy, elements, elementsType || gl.UNSIGNED_SHORT)
}

module.exports = buildSnapshot
