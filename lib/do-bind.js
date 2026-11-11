"use strict"

// Apply the VAO-contained part of a validated snapshot to the current vertex
// array state: element buffer binding, vertex pointers and enable flags.
// Constant attribute values are global GL state and are handled separately
// by applyConstants(), so repopulating an inactive VAO never touches them.

function applyConstants(gl, snapshot) {
  var attributes = snapshot.attributes
  for(var i=0; i<attributes.length; ++i) {
    var attrib = attributes[i]
    if(!attrib.constant) continue
    var v = attrib.values
    switch(attrib.dimension) {
      case 1: gl.vertexAttrib1f(i, v[0]); break
      case 2: gl.vertexAttrib2f(i, v[0], v[1]); break
      case 3: gl.vertexAttrib3f(i, v[0], v[1], v[2]); break
      case 4: gl.vertexAttrib4f(i, v[0], v[1], v[2], v[3]); break
    }
    gl.disableVertexAttribArray(i)
  }
}

function doBind(gl, snapshot) {
  var attributes = snapshot.attributes
  var nattribs = gl.getParameter(gl.MAX_VERTEX_ATTRIBS) | 0

  if(snapshot.elements) {
    snapshot.elements.bind()
  } else {
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, null)
  }

  for(var i=0; i<attributes.length; ++i) {
    var attrib = attributes[i]
    if(attrib.constant) continue
    attrib.buffer.bind()
    gl.enableVertexAttribArray(i)
    gl.vertexAttribPointer(i, attrib.size, attrib.type, attrib.normalized,
      attrib.stride, attrib.offset)
  }
  for(; i<nattribs; ++i) {
    gl.disableVertexAttribArray(i)
  }
}

doBind.applyConstants = applyConstants
module.exports = doBind
