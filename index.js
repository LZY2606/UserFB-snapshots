/* eslint-disable no-redeclare */

var hash = require('murmurhash-js/murmurhash3_gc')
var trim = require('glsl-token-whitespace-trim')
var tokenize = require('glsl-tokenizer/string')
var inject = require('glsl-inject-defines')
var defines = require('glsl-token-defines')
var descope = require('glsl-token-descope')
var clean = require('./lib/clean-suffixes')
var string = require('glsl-token-string')
var scope = require('glsl-token-scope')
var depth = require('glsl-token-depth')
var topoSort = require('./lib/topo-sort')
var copy = require('shallow-copy')

module.exports = bundle
module.exports.withSourceMap = withSourceMap

function bundle (deps) {
  return inject(Bundle(deps).src, {
    GLSLIFY: 1
  })
}

function withSourceMap (deps, opts) {
  opts = opts || {}

  // Bundle first: bundling errors (e.g. cyclic dependencies)
  // are thrown before any map is produced, so a partial
  // result is never returned.
  var bundled = Bundle(deps)
  var src = inject(bundled.src, {
    GLSLIFY: 1
  })

  return {
    source: src,
    map: bundled.sourceMap(src, opts.file)
  }
}

function Bundle (deps) {
  if (!(this instanceof Bundle)) return new Bundle(deps)

  // Reorder dependencies topologically
  deps = topoSort(deps)

  this.depList = deps
  this.depIndex = indexBy(deps, 'id')
  this.exported = {}
  this.cache = {}
  this.varCounter = 0

  this.src = []

  for (var i = 0; i < deps.length; i++) {
    deps[i].sourceIndex = i
    this.preprocess(deps[i])
  }

  for (var i = 0; i < deps.length; i++) {
    if (deps[i].entry) {
      this.src = this.src.concat(this.bundle(deps[i]))
    }
  }

  // Flatten to a string, remembering which token each character
  // span belongs to so that token origins survive the
  // retokenization below.
  var flat = flatten(this.src)
  var tokens = tokenize(flat.src)
  inheritOrigins(tokens, flat.spans)

  tokens = clean(trim(tokens))

  // Final flatten: token data may have been rewritten by
  // trim/clean, so generated offsets are computed from the
  // final tokens themselves.
  var out = flatten(tokens)
  this.src = out.src
  this.mapTokens = out.spans
}

var proto = Bundle.prototype

proto.preprocess = function (dep) {
  var tokens = tokenize(dep.source)
  var imports = []
  var exports = null

  // Tag every token with its origin so that generated tokens
  // can be traced back to this source later on. Offsets address
  // the CRLF-normalized source, matching the tokenizer.
  for (var i = 0; i < tokens.length; i++) {
    tokens[i].origin = {
      source: dep.sourceIndex,
      offset: tokens[i].position,
      name: tokens[i].type === 'ident' ? tokens[i].data : null
    }
  }

  depth(tokens)
  scope(tokens)

  for (var i = 0; i < tokens.length; i++) {
    var token = tokens[i]
    if (token.type !== 'preprocessor') continue
    if (!glslifyPreprocessor(token.data)) continue

    var exported = glslifyExport(token.data)
    var imported = glslifyImport(token.data)

    if (exported) {
      exports = exported[1]
      tokens.splice(i--, 1)
    } else if (imported) {
      var name = imported[1]
      var maps = imported[2].split(/\s?,\s?/g)
      var path = maps.shift()
        .trim()
        .replace(/^'|'$/g, '')
        .replace(/^"|"$/g, '')
      var target = this.depIndex[dep.deps[path]]
      imports.push({
        name: name,
        path: path,
        target: target,
        maps: toMapping(maps),
        index: i
      })
      tokens.splice(i--, 1)
    }
  }

  var eof = tokens[tokens.length - 1]
  if (eof && eof.type === 'eof') {
    tokens.pop()
  }

  if (dep.entry) {
    exports = exports || 'main'
  }

  if (!exports) {
    throw new Error(dep.file + ' does not export any symbols')
  }

  dep.parsed = {
    tokens: tokens,
    imports: imports,
    exports: exports
  }
}

proto.bundle = function (entry) {
  var resolved = {}
  var result = resolve(entry, [])[1]

  return result

  function resolve (dep, bindings) {
    // Compute suffix for module
    bindings.sort()
    var ident = bindings.join(':') + ':' + dep.id
    var suffix = '_' + hash(ident)

    if (dep.entry) {
      suffix = ''
    }

    // Test if export is already resolved
    var exportName = dep.parsed.exports + suffix
    if (resolved[exportName]) {
      return [exportName, []]
    }

    // Initialize map for variable renamings based on bindings
    var rename = {}
    for (var i = 0; i < bindings.length; ++i) {
      var binding = bindings[i]
      rename[binding[0]] = binding[1]
    }

    // Resolve all dependencies
    var imports = dep.parsed.imports
    var edits = []
    for (var i = 0; i < imports.length; ++i) {
      var data = imports[i]

      var importMaps = data.maps
      var importName = data.name
      var importTarget = data.target

      var importBindings = Object.keys(importMaps).map(function (id) {
        var value = importMaps[id]

        // floats/ints should not be renamed
        if (value.match(/^\d+(?:\.\d+?)?$/g)) {
          return [id, value]
        }

        // properties (uVec.x, ray.origin, ray.origin.xy etc.) should
        // have their host identifiers renamed
        var parent = value.match(/^([^.]+)\.(.+)$/)
        if (parent) {
          return [id, (rename[parent[1]] || (parent[1] + suffix)) + '.' + parent[2]]
        }

        return [id, rename[value] || (value + suffix)]
      })

      var importTokens = resolve(importTarget, importBindings)
      rename[importName] = importTokens[0]
      edits.push([data.index, importTokens[1]])
    }

    // Rename tokens
    var parsedTokens = dep.parsed.tokens.map(copy)
    var parsedDefs = defines(parsedTokens)
    var tokens = descope(parsedTokens, function (local, token) {
      if (parsedDefs[local]) return local
      if (rename[local]) return rename[local]

      return local + suffix
    })

    // Insert edits
    edits.sort(function (a, b) {
      return b[0] - a[0]
    })

    for (var i = 0; i < edits.length; ++i) {
      var edit = edits[i]
      tokens = tokens.slice(0, edit[0])
        .concat(edit[1])
        .concat(tokens.slice(edit[0]))
    }

    resolved[exportName] = true
    return [exportName, tokens]
  }
}

// Builds a Source Map v3 object for `src`, which must be the
// result of injecting defines into this bundle's source.
proto.sourceMap = function (src, file) {
  // Define injection only ever inserts a contiguous block of
  // text (e.g. the GLSLIFY define after a #version header), so
  // locate it by diffing against the pre-injection source and
  // shift the generated offsets that follow it.
  var shift = insertionPoint(this.src, src, this.mapTokens)
  var genLines = lineStarts(src)

  var sources = []
  var sourcesContent = []
  var origLines = []
  for (var i = 0; i < this.depList.length; i++) {
    var dep = this.depList[i]
    sources.push(dep.file)
    sourcesContent.push(dep.source)
    // Token offsets address the CRLF-normalized source, as
    // produced by the tokenizer. Line/column results are the
    // same as in the original source.
    origLines.push(lineStarts(dep.source.replace(/\r\n/g, '\n')))
  }

  var names = []
  var nameIndex = {}
  var mappings = ''
  var genLine = 0
  var prevGenColumn = 0
  var prevSource = 0
  var prevOrigLine = 0
  var prevOrigColumn = 0
  var prevName = 0

  for (var i = 0; i < this.mapTokens.length; i++) {
    var token = this.mapTokens[i].token
    var origin = token.origin
    if (!origin) continue
    if (token.type === 'whitespace') continue

    var offset = this.mapTokens[i].start
    if (offset >= shift.at) offset += shift.length

    var line = locateLine(genLines, offset)
    var column = offset - genLines[line]
    var origLine = locateLine(origLines[origin.source], origin.offset)
    var origColumn = origin.offset - origLines[origin.source][origLine]

    var name = -1
    if (origin.name !== null) {
      if (!Object.prototype.hasOwnProperty.call(nameIndex, origin.name)) {
        nameIndex[origin.name] = names.length
        names.push(origin.name)
      }
      name = nameIndex[origin.name]
    }

    while (genLine < line) {
      mappings += ';'
      prevGenColumn = 0
      genLine++
    }
    if (mappings.length > 0 && mappings.charAt(mappings.length - 1) !== ';') {
      mappings += ','
    }

    mappings += vlq(column - prevGenColumn)
    prevGenColumn = column
    mappings += vlq(origin.source - prevSource)
    prevSource = origin.source
    mappings += vlq(origLine - prevOrigLine)
    prevOrigLine = origLine
    mappings += vlq(origColumn - prevOrigColumn)
    prevOrigColumn = origColumn
    if (name !== -1) {
      mappings += vlq(name - prevName)
      prevName = name
    }
  }

  return {
    version: 3,
    file: file,
    sources: sources,
    sourcesContent: sourcesContent,
    names: names,
    mappings: mappings
  }
}

// Flattens tokens into a string (matching glsl-token-string),
// recording the span each token occupies in the output.
function flatten (tokens) {
  var src = ''
  var spans = []

  for (var i = 0; i < tokens.length; i++) {
    var token = tokens[i]
    if (token.type === 'eof') continue
    spans.push({
      token: token,
      start: src.length,
      end: src.length + token.data.length
    })
    src += token.data
  }

  return { src: src, spans: spans }
}

// Transfers origins from the tokens that produced a string onto
// its retokenized form. A retokenized token only inherits an
// origin when it lies fully within a single original token.
function inheritOrigins (tokens, spans) {
  for (var i = 0, j = 0; i < tokens.length; i++) {
    var token = tokens[i]
    if (token.type === 'eof') continue

    var start = token.position
    var end = start + token.data.length
    while (j < spans.length && spans[j].end <= start) j++
    if (j < spans.length && spans[j].start <= start && end <= spans[j].end) {
      token.origin = spans[j].token.origin
    }
  }
}

// Locates the contiguous block of text that `after` adds to
// `before`, e.g. an injected #define. A pure text diff can be
// ambiguous when the inserted text shares characters with its
// surroundings, so the result is snapped to a token boundary
// (injection always happens between tokens).
function insertionPoint (before, after, spans) {
  var inserted = after.length - before.length
  if (inserted < 0) {
    throw new Error('unable to align injected defines with bundled source')
  }

  var prefix = 0
  var max = Math.min(before.length, after.length)
  while (prefix < max && before.charAt(prefix) === after.charAt(prefix)) prefix++

  var suffix = 0
  while (suffix < before.length &&
    before.charAt(before.length - 1 - suffix) === after.charAt(after.length - 1 - suffix)) {
    suffix++
  }

  // Any split point within [lo, hi] explains the diff.
  var lo = before.length - suffix
  var hi = prefix
  if (lo > hi) {
    throw new Error('unable to align injected defines with bundled source')
  }

  for (var i = 0; i < spans.length; i++) {
    if (spans[i].start >= lo && spans[i].start <= hi) {
      return { at: spans[i].start, length: inserted }
    }
  }
  if (before.length >= lo && before.length <= hi) {
    return { at: before.length, length: inserted }
  }

  throw new Error('unable to align injected defines with bundled source')
}

// Starting offsets of every line in `src`. Columns are counted
// in UTF-16 code units, i.e. plain JavaScript string indices.
function lineStarts (src) {
  var starts = [0]
  for (var i = 0; i < src.length; i++) {
    if (src.charAt(i) === '\n') starts.push(i + 1)
  }
  return starts
}

function locateLine (starts, offset) {
  var lo = 0
  var hi = starts.length - 1
  while (lo < hi) {
    var mid = (lo + hi + 1) >> 1
    if (starts[mid] <= offset) lo = mid
    else hi = mid - 1
  }
  return lo
}

var BASE64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'

// Encodes a single signed integer as base64 VLQ.
function vlq (value) {
  var shifted = value < 0 ? ((-value) << 1) | 1 : value << 1
  var out = ''
  do {
    var digit = shifted & 31
    shifted >>>= 5
    if (shifted > 0) digit |= 32
    out += BASE64.charAt(digit)
  } while (shifted > 0)
  return out
}

function glslifyPreprocessor (data) {
  return /#pragma glslify:/.test(data)
}

function glslifyExport (data) {
  return /#pragma glslify:\s*export\(([^)]+)\)/.exec(data)
}

function glslifyImport (data) {
  return /#pragma glslify:\s*([^=\s]+)\s*=\s*require\(([^)]+)\)/.exec(data)
}

function indexBy (deps, key) {
  return deps.reduce(function (deps, entry) {
    deps[entry[key]] = entry
    return deps
  }, {})
}

function toMapping (maps) {
  if (!maps) return false

  return maps.reduce(function (mapping, defn) {
    defn = defn.split(/\s?=\s?/g)

    var expr = defn.pop()

    defn.forEach(function (key) {
      mapping[key] = expr
    })

    return mapping
  }, {})
}
