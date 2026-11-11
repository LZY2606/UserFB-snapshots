'use strict'

var createUniformWrapper   = require('./lib/create-uniforms')
var createAttributeWrapper = require('./lib/create-attributes')
var makeReflect            = require('./lib/reflect')
var shaderCache            = require('./lib/shader-cache')
var runtime                = require('./lib/runtime-reflect')
var GLError                = require("./lib/GLError")

//Shader object
function Shader(gl) {
  this.gl = gl

  //Context-wide record of which generic vertex attribute slots are
  //currently enabled, and which shader last enabled each one.  Creating
  //a shader must not disturb this state, so it is initialised once.
  if(!gl._enabledAttribs) {
    gl._enabledAttribs = new Set()
    gl._attribOwner    = new Map()
  }

  //Default initialize these to null
  this._vref      =
  this._fref      =
  this._relink    =
  this.vertShader =
  this.fragShader =
  this.program    =
  this.attributes =
  this.uniforms   =
  this.types      =
  this._attribLocations =
  this._attribMode      =
  this._attribConst     =
  this._attribSubmit    = null
}

var proto = Shader.prototype

proto.bind = function() {
  if(!this.program) {
    this._relink()
  }

  var gl        = this.gl
  var enabled   = gl._enabledAttribs
  var owner     = gl._attribOwner
  var locations = this._attribLocations
  var mode      = this._attribMode

  //Drive the enabled slot set from this shader's real locations
  //instead of guessing a contiguous range from ACTIVE_ATTRIBUTES.
  for(var i=0; i<locations.length; ++i) {
    var location = locations[i]
    if(mode[i]) {
      //Array mode: make sure the slot is enabled.
      if(!enabled.has(location)) {
        gl.enableVertexAttribArray(location)
        enabled.add(location)
      }
      owner.set(location, this)
    } else {
      //Constant mode: the slot stays disabled while this shader is bound.
      if(enabled.has(location)) {
        gl.disableVertexAttribArray(location)
        enabled.delete(location)
      }
      owner.delete(location)
    }
  }

  //Release slots left enabled by the previously bound shader that this
  //shader does not use at all.
  enabled.forEach(function(location) {
    if(locations.indexOf(location) < 0) {
      gl.disableVertexAttribArray(location)
      enabled.delete(location)
      owner.delete(location)
    }
  })

  this.gl.useProgram(this.program)

  //Constant vertex attributes are global GL state rather than program
  //state, so each shader re-submits its own constants on every bind.
  for(i=0; i<locations.length; ++i) {
    if(!mode[i] && typeof this._attribSubmit[i] === 'function') {
      this._attribSubmit[i]()
    }
  }
}

proto.dispose = function() {
  var gl        = this.gl
  var locations = this._attribLocations || []

  //Only shut down slots owned by this shader.  Another shader may still
  //be drawing through the same generic attribute location.
  for(var i=0; i<locations.length; ++i) {
    var location = locations[i]
    if(gl._attribOwner.get(location) === this) {
      if(gl._enabledAttribs.has(location)) {
        gl.disableVertexAttribArray(location)
        gl._enabledAttribs.delete(location)
      }
      gl._attribOwner.delete(location)
    }
  }

  if(this._fref) {
    this._fref.dispose()
  }
  if(this._vref) {
    this._vref.dispose()
  }
  this.attributes =
  this.types      =
  this.vertShader =
  this.fragShader =
  this.program    =
  this._relink    =
  this._fref      =
  this._vref      =
  this._attribLocations =
  this._attribMode      =
  this._attribConst     =
  this._attribSubmit    = null
}

function compareAttributes(a, b) {
  if(a.name < b.name) {
    return -1
  }
  return 1
}

//Update export hook for glslify-live
proto.update = function(
    vertSource
  , fragSource
  , uniforms
  , attributes) {

  //If only one object passed, assume glslify style output
  if(!fragSource || arguments.length === 1) {
    var obj = vertSource
    vertSource = obj.vertex
    fragSource = obj.fragment
    uniforms   = obj.uniforms
    attributes = obj.attributes
  }

  var wrapper = this
  var gl      = wrapper.gl

  //Compile vertex and fragment shaders
  var pvref = wrapper._vref
  wrapper._vref = shaderCache.shader(gl, gl.VERTEX_SHADER, vertSource)
  if(pvref) {
    pvref.dispose()
  }
  wrapper.vertShader = wrapper._vref.shader
  var pfref = this._fref
  wrapper._fref = shaderCache.shader(gl, gl.FRAGMENT_SHADER, fragSource)
  if(pfref) {
    pfref.dispose()
  }
  wrapper.fragShader = wrapper._fref.shader

  //If uniforms/attributes is not specified, use RT reflection
  if(!uniforms || !attributes) {

    //Create initial test program
    var testProgram = gl.createProgram()
    gl.attachShader(testProgram, wrapper.fragShader)
    gl.attachShader(testProgram, wrapper.vertShader)
    gl.linkProgram(testProgram)
    if(!gl.getProgramParameter(testProgram, gl.LINK_STATUS)) {
      var errLog = gl.getProgramInfoLog(testProgram)
      throw new GLError(errLog, 'Error linking program:' + errLog)
    }

    //Load data from runtime
    uniforms   = uniforms   || runtime.uniforms(gl, testProgram)
    attributes = attributes || runtime.attributes(gl, testProgram)

    //Release test program
    gl.deleteProgram(testProgram)
  }

  //Sort attributes lexicographically
  // overrides undefined WebGL behavior for attribute locations
  attributes = attributes.slice()
  attributes.sort(compareAttributes)

  //Convert attribute types, read out locations
  var attributeUnpacked  = []
  var attributeNames     = []
  var attributeLocations = []
  var i
  for(i=0; i<attributes.length; ++i) {
    var attr = attributes[i]
    if(attr.type.indexOf('mat') >= 0) {
      var size = attr.type.charAt(attr.type.length-1)|0
      var locVector = new Array(size)
      for(var j=0; j<size; ++j) {
        locVector[j] = attributeLocations.length
        attributeNames.push(attr.name + '[' + j + ']')
        if(typeof attr.location === 'number') {
          attributeLocations.push(attr.location + j)
        } else if(Array.isArray(attr.location) &&
                  attr.location.length === size &&
                  typeof attr.location[j] === 'number') {
          attributeLocations.push(attr.location[j]|0)
        } else {
          attributeLocations.push(-1)
        }
      }
      attributeUnpacked.push({
        name: attr.name,
        type: attr.type,
        locations: locVector
      })
    } else {
      attributeUnpacked.push({
        name: attr.name,
        type: attr.type,
        locations: [ attributeLocations.length ]
      })
      attributeNames.push(attr.name)
      if(typeof attr.location === 'number') {
        attributeLocations.push(attr.location|0)
      } else {
        attributeLocations.push(-1)
      }
    }
  }

  //For all unspecified attributes, assign them lexicographically min attribute
  var curLocation = 0
  for(i=0; i<attributeLocations.length; ++i) {
    if(attributeLocations[i] < 0) {
      while(attributeLocations.indexOf(curLocation) >= 0) {
        curLocation += 1
      }
      attributeLocations[i] = curLocation
    }
  }

  //Rebuild program and recompute all uniform locations
  var uniformLocations = new Array(uniforms.length)
  function relink() {
    wrapper.program = shaderCache.program(
        gl
      , wrapper._vref
      , wrapper._fref
      , attributeNames
      , attributeLocations)

    for(var i=0; i<uniforms.length; ++i) {
      uniformLocations[i] = gl.getUniformLocation(
          wrapper.program
        , uniforms[i].name)
    }
  }

  //Perform initial linking, reuse program used for reflection
  relink()

  //Save relinking procedure, defer until runtime
  wrapper._relink = relink

  //Generate type info
  wrapper.types = {
    uniforms:   makeReflect(uniforms),
    attributes: makeReflect(attributes)
  }

  //Generate attribute wrappers
  //The real, possibly non-contiguous GL locations of every slot; the
  //attribute wrappers track per-slot array/constant state alongside it.
  wrapper._attribLocations = attributeLocations
  wrapper.attributes = createAttributeWrapper(
      gl
    , wrapper
    , attributeUnpacked
    , attributeLocations)

  //Generate uniform wrappers
  Object.defineProperty(wrapper, 'uniforms', createUniformWrapper(
      gl
    , wrapper
    , uniforms
    , uniformLocations))
}

//Compiles and links a shader program with the given attribute and vertex list
function createShader(
    gl
  , vertSource
  , fragSource
  , uniforms
  , attributes) {

  var shader = new Shader(gl)

  shader.update(
      vertSource
    , fragSource
    , uniforms
    , attributes)

  return shader
}

module.exports = createShader
