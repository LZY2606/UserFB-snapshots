'use strict'

module.exports = createAttributeWrapper

var GLError = require("./GLError")

//Byte size of each WebGL vertex pointer element type.  Unknown types
//default to 4 bytes, matching the size of a FLOAT.
var TYPE_SIZE_IN_BYTES = {}
TYPE_SIZE_IN_BYTES[0x1400] = 1  //BYTE
TYPE_SIZE_IN_BYTES[0x1401] = 1  //UNSIGNED_BYTE
TYPE_SIZE_IN_BYTES[0x1402] = 2  //SHORT
TYPE_SIZE_IN_BYTES[0x1403] = 2  //UNSIGNED_SHORT
TYPE_SIZE_IN_BYTES[0x1404] = 4  //INT
TYPE_SIZE_IN_BYTES[0x1405] = 4  //UNSIGNED_INT
TYPE_SIZE_IN_BYTES[0x1406] = 4  //FLOAT
TYPE_SIZE_IN_BYTES[0x140b] = 2  //HALF_FLOAT

function typeSizeInBytes(type) {
  var size = TYPE_SIZE_IN_BYTES[type]
  return size === undefined ? 4 : size
}

//Keep the context-wide set of enabled slots in sync with the raw GL
//calls so shader switches can diff against the real hardware state.
function enableSlot(gl, location) {
  gl.enableVertexAttribArray(location)
  if(gl._enabledAttribs) {
    gl._enabledAttribs.add(location)
  }
}

function disableSlot(gl, location) {
  gl.disableVertexAttribArray(location)
  if(gl._enabledAttribs) {
    gl._enabledAttribs.delete(location)
  }
}

function ShaderAttribute(
    gl
  , wrapper
  , index
  , locations
  , dimension
  , constFunc) {
  this._gl        = gl
  this._wrapper   = wrapper
  this._index     = index
  this._locations = locations
  this._dimension = dimension
  this._constFunc = constFunc
}

var proto = ShaderAttribute.prototype

proto.pointer = function setAttribPointer(
    type
  , normalized
  , stride
  , offset) {

  var self      = this
  var gl        = self._gl
  var location  = self._locations[self._index]

  gl.vertexAttribPointer(
      location
    , self._dimension
    , type || gl.FLOAT
    , !!normalized
    , stride || 0
    , offset || 0)
  enableSlot(gl, location)
  //Back in array mode after any earlier constant assignment.
  self._wrapper._attribMode[self._index] = true
}

proto.set = function(x0, x1, x2, x3) {
  return this._constFunc(this._locations[this._index], x0, x1, x2, x3)
}

Object.defineProperty(proto, 'location', {
  get: function() {
    return this._locations[this._index]
  }
  , set: function(v) {
    if(v !== this._locations[this._index]) {
      this._locations[this._index] = v|0
      this._wrapper.program = null
    }
    return v|0
  }
})

var allFns = [
  function (gl, v, x0) {
    if (x0.length === undefined) {
      return gl.vertexAttrib1f(v, x0)
    } else {
      return gl.vertexAttrib1fv(v, x0)
    }
  },

  function (gl, v, x0, x1) {
    if (x0.length === undefined) {
      return gl.vertexAttrib2f(v, x0, x1)
    } else {
      return gl.vertexAttrib2fv(v, x0)
    }
  },

  function (gl, v, x0, x1, x2) {
    if (x0.length === undefined) {
      return gl.vertexAttrib3f(v, x0, x1, x2)
    } else {
      return gl.vertexAttrib3fv(v, x0)
    }
  },

  function (gl, v, x0, x1, x2, x3) {
    if (x0.length === undefined) {
      return gl.vertexAttrib4f(v, x0, x1, x2, x3)
    } else {
      return gl.vertexAttrib4fv(v, x0)
    }
  }
]

//Adds a vector attribute to obj
function addVectorAttribute(
    gl
  , wrapper
  , index
  , locations
  , dimension
  , obj
  , name) {

  //allFns is zero indexed: entry 0 handles one component values.
  var constFunc = allFns[dimension - 1]

  //Create attribute wrapper
  var attr = new ShaderAttribute(
      gl
    , wrapper
    , index
    , locations
    , dimension
    , constFunc)

  //Attributes default to array (vertex buffer) mode.
  wrapper._attribMode[index] = true
  wrapper._attribSubmit[index] = function() {
    constFunc(gl, locations[index], wrapper._attribConst[index])
  }

  //Create accessor
  Object.defineProperty(obj, name, {
    set: function(x) {
      disableSlot(gl, locations[index])
      constFunc(gl, locations[index], x)
      //Constant mode: bind() must keep this slot disabled and
      //re-submit the value every time this shader is used again.
      wrapper._attribMode[index] = false
      wrapper._attribConst[index] = x
      return x
    }
    , get: function() {
      return attr
    }
    , enumerable: true
  })
}

function addMatrixAttribute(
    gl
  , wrapper
  , indices
  , locations
  , dimension
  , obj
  , name) {

  var parts = new Array(dimension)
  var attrs = new Array(dimension)
  for(var i=0; i<dimension; ++i) {
    addVectorAttribute(
        gl
      , wrapper
      , indices[i]
      , locations
      , dimension
      , parts
      , i)
    attrs[i] = parts[i]
  }

  Object.defineProperty(parts, 'location', {
    set: function(v) {
      if(Array.isArray(v)) {
        for(var i=0; i<dimension; ++i) {
          attrs[i].location = v[i]
        }
      } else {
        for(var i=0; i<dimension; ++i) {
          attrs[i].location = v + i
        }
      }
      return v
    }
    , get: function() {
      var result = new Array(dimension)
      for(var i=0; i<dimension; ++i) {
        result[i] = locations[indices[i]]
      }
      return result
    }
    , enumerable: true
  })

  parts.pointer = function(type, normalized, stride, offset) {
    type       = type || gl.FLOAT
    normalized = !!normalized
    var bytesPerElement = typeSizeInBytes(type)
    //WebGL stride and offset are both in bytes.  With no stride (or a
    //zero stride) the matrix columns are tightly packed; a user supplied
    //stride is already byte sized and must not be scaled again.
    stride = stride || dimension * dimension * bytesPerElement
    offset = offset || 0
    for(var i=0; i<dimension; ++i) {
      var slotIndex = indices[i]
      var location = locations[slotIndex]
      gl.vertexAttribPointer(
            location
          , dimension
          , type
          , normalized
          , stride
          , offset + i * dimension * bytesPerElement)
      enableSlot(gl, location)
      wrapper._attribMode[slotIndex] = true
    }
  }

  var scratch = new Array(dimension)
  var vertexAttrib = gl['vertexAttrib' + dimension + 'fv']

  Object.defineProperty(obj, name, {
    set: function(x) {
      var nested = Array.isArray(x[0])
      for(var i=0; i<dimension; ++i) {
        var slotIndex = indices[i]
        var loc = locations[slotIndex]
        var column
        if(nested) {
          column = x[i]
          vertexAttrib.call(gl, loc, column)
        } else {
          for(var j=0; j<dimension; ++j) {
            scratch[j] = x[dimension*i + j]
          }
          vertexAttrib.call(gl, loc, scratch)
          column = x.slice(dimension*i, dimension*(i+1))
        }
        disableSlot(gl, loc)
        //Constant mode for every column; the per-column submit
        //closures re-apply these values on the next bind().
        wrapper._attribMode[slotIndex] = false
        wrapper._attribConst[slotIndex] = column
      }
      return x
    }
    , get: function() {
      return parts
    }
    , enumerable: true
  })
}

//Create shims for attributes
function createAttributeWrapper(
    gl
  , wrapper
  , attributes
  , locations) {

  //Per-slot state, indexed in parallel with locations:
  //  _attribMode:    true  = vertex array (default), false = constant
  //  _attribConst:   last value written in constant mode
  //  _attribSubmit:  re-applies the stored constant to its GL location
  wrapper._attribMode   = []
  wrapper._attribConst  = []
  wrapper._attribSubmit = []

  var obj = {}
  for(var i=0, n=attributes.length; i<n; ++i) {

    var a = attributes[i]
    var name = a.name
    var type = a.type
    var locs = a.locations

    switch(type) {
      case 'bool':
      case 'int':
      case 'float':
        addVectorAttribute(
            gl
          , wrapper
          , locs[0]
          , locations
          , 1
          , obj
          , name)
      break

      default:
        if(type.indexOf('vec') >= 0) {
          var d = type.charCodeAt(type.length-1) - 48
          if(d < 2 || d > 4) {
            throw new GLError('', 'Invalid data type for attribute ' + name + ': ' + type)
          }
          addVectorAttribute(
              gl
            , wrapper
            , locs[0]
            , locations
            , d
            , obj
            , name)
        } else if(type.indexOf('mat') >= 0) {
          var d = type.charCodeAt(type.length-1) - 48
          if(d < 2 || d > 4) {
            throw new GLError('', 'Invalid data type for attribute ' + name + ': ' + type)
          }
          addMatrixAttribute(
              gl
            , wrapper
            , locs
            , locations
            , d
            , obj
            , name)
        } else {
          throw new GLError('', 'Unknown data type for attribute ' + name + ': ' + type)
        }
      break
    }
  }
  return obj
}
