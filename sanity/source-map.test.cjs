'use strict'

// Sanity tests for bundle.withSourceMap. The map is verified with an
// independent Source Map v3 (base64 VLQ) decoder: every non-whitespace
// generated token is traced back to its original source position, and
// every named token is checked for its original spelling and coordinates.

const assert = require('node:assert/strict')
const tokenize = require('glsl-tokenizer/string')
const bundle = require('../')

// ---------------------------------------------------------------------
// Independent Source Map v3 decoder
// ---------------------------------------------------------------------

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'
const B64D = new Map(Array.from(B64).map((c, i) => [c, i]))

function decodeVlq (mappings, i) {
  let shift = 0
  let value = 0
  for (;;) {
    const digit = B64D.get(mappings[i++])
    assert.notEqual(digit, undefined, 'invalid base64 digit in mappings')
    value += (digit & 31) << shift
    shift += 5
    if (!(digit & 32)) break
  }
  const negate = value & 1
  value >>= 1
  return { value: negate ? -value : value, next: i }
}

function decodeMappings (map) {
  const segments = []
  const m = map.mappings
  let genLine = 0
  let genCol = 0
  let src = 0
  let origLine = 0
  let origCol = 0
  let name = 0
  let i = 0
  const more = () => i < m.length && m[i] !== ',' && m[i] !== ';'
  while (i < m.length) {
    const ch = m[i]
    if (ch === ';') { genLine++; genCol = 0; i++; continue }
    if (ch === ',') { i++; continue }
    let r = decodeVlq(m, i); i = r.next; genCol += r.value
    const seg = { genLine, genCol, source: null, origLine: null, origCol: null, name: null }
    if (more()) {
      r = decodeVlq(m, i); i = r.next; src += r.value; seg.source = src
      r = decodeVlq(m, i); i = r.next; origLine += r.value; seg.origLine = origLine
      r = decodeVlq(m, i); i = r.next; origCol += r.value; seg.origCol = origCol
      if (more()) {
        r = decodeVlq(m, i); i = r.next; name += r.value; seg.name = name
      }
    }
    segments.push(seg)
  }
  return segments
}

// ---------------------------------------------------------------------
// Position helpers (lines/columns in UTF-16 code units, i.e. JS indices)
// ---------------------------------------------------------------------

function lineStarts (src) {
  const starts = [0]
  for (let i = 0; i < src.length; i++) {
    if (src[i] === '\n') starts.push(i + 1)
  }
  return starts
}

function locateLine (starts, offset) {
  let lo = 0
  let hi = starts.length - 1
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1
    if (starts[mid] <= offset) lo = mid
    else hi = mid - 1
  }
  return lo
}

function lineColOf (src, offset) {
  const starts = lineStarts(src)
  const line = locateLine(starts, offset)
  return { line, column: offset - starts[line] }
}

function offsetOf (src, line, column) {
  return lineStarts(src)[line] + column
}

// ---------------------------------------------------------------------
// Generic map verification
// ---------------------------------------------------------------------

function verifyMap (generated, map, deps) {
  assert.equal(map.version, 3)
  assert.ok(Array.isArray(map.sources))
  assert.ok(Array.isArray(map.names))
  assert.equal(typeof map.mappings, 'string')
  assert.equal(map.sources.length, map.sourcesContent.length)

  // sources/sourcesContent come straight from the dependency records
  const byFile = new Map(deps.map(d => [d.file, d.source]))
  assert.deepEqual(new Set(map.sources), new Set(deps.map(d => d.file)))
  map.sources.forEach((file, i) => {
    assert.equal(map.sourcesContent[i], byFile.get(file), 'sourcesContent must be the original source')
  })

  const segments = decodeMappings(map)
  assert.ok(segments.length > 0, 'expected at least one mapping segment')

  // Segments must be strictly ordered by generated position, and every
  // emitted segment must point at an original source.
  const byPos = new Map()
  let pl = 0
  let pc = -1
  for (const seg of segments) {
    assert.ok(seg.genLine > pl || (seg.genLine === pl && seg.genCol > pc), 'segments strictly ordered')
    pl = seg.genLine
    pc = seg.genCol
    assert.notEqual(seg.source, null, 'every emitted segment has a source')
    byPos.set(seg.genLine + ':' + seg.genCol, seg)
  }

  const genStarts = lineStarts(generated)
  const toks = tokenize(generated).filter(t => t.type !== 'whitespace' && t.type !== 'eof')

  function segFor (tok) {
    const line = locateLine(genStarts, tok.position)
    const col = tok.position - genStarts[line]
    const seg = byPos.get(line + ':' + col)
    return seg ? { seg, line, col } : null
  }

  let sawGlslify = 0
  for (const tok of toks) {
    const line = locateLine(genStarts, tok.position)
    const col = tok.position - genStarts[line]

    // Content inserted by the bundler itself must not pretend to come
    // from the entry file (or anywhere else).
    if (tok.type === 'preprocessor' && /^#define GLSLIFY\b/.test(tok.data)) {
      sawGlslify++
      assert.ok(!byPos.has(line + ':' + col), 'GLSLIFY define must have no original source')
      assert.ok(!segments.some(s => s.genLine === line), 'no segment may cover the injected define line')
      continue
    }

    const hit = byPos.get(line + ':' + col)
    assert.ok(hit, `no segment for generated token ${JSON.stringify(tok.data)} at ${line}:${col}`)

    const file = map.sources[hit.source]
    const content = map.sourcesContent[hit.source]
    const slice = content.slice(offsetOf(content, hit.origLine, hit.origCol))

    if (hit.name !== null) {
      // Named token: the original spelling at the mapped coordinates
      // must be exactly the recorded name.
      const name = map.names[hit.name]
      assert.ok(slice.startsWith(name),
        `original spelling at ${file}:${hit.origLine}:${hit.origCol} should be ${JSON.stringify(name)}, got ${JSON.stringify(slice.slice(0, 30))}`)
      assert.ok(!/[A-Za-z0-9_$]/.test(slice.charAt(name.length)),
        `name ${JSON.stringify(name)} must be the full original identifier`)
    } else {
      // Unnamed token: generated text must be unchanged from the original.
      assert.ok(slice.startsWith(tok.data),
        `generated token ${JSON.stringify(tok.data)} must match original text at ${file}:${hit.origLine}:${hit.origCol}`)
    }

    if (tok.type === 'ident') {
      assert.notEqual(hit.name, null, `generated identifier ${JSON.stringify(tok.data)} must carry its original name`)
    }
  }
  assert.ok(sawGlslify >= 1, 'expected the injected GLSLIFY define')

  return { segments, byPos, genStarts, toks, segFor }
}

// ---------------------------------------------------------------------
// Shader set A: specialization, shared deps, collisions, nested scopes,
// macros, #version header, CRLF and non-ASCII comments. Declared out of
// topological order with non-contiguous ids on purpose.
// ---------------------------------------------------------------------

const GAIN = [
  '// 增益模块 gain module 💧',
  'float /* 💧💧 */ gain(float x) {',
  '  float y = x;',
  '  {',
  '    float y = x * 2.0; // 遮蔽 shadow',
  '    y = y + 1.0;',
  '  }',
  '  return factor * y;',
  '}',
  '#pragma glslify: export(gain)'
].join('\r\n')

const NOISE = [
  'float noise(float p) {',
  '  return fract(sin(p) * 43758.5453);',
  '}',
  '#pragma glslify: export(noise)'
].join('\n')

const MIXER = [
  '#pragma glslify: nz = require(./noise)',
  'float gain(float x) { return x * x; }',
  'float mix2(float a, float b) { return mix(a, b, nz(a)); }',
  '#pragma glslify: export(mix2)'
].join('\n')

const SCALE = [
  'float scale(float x) { return x * scaleBy; }',
  '#pragma glslify: export(scale)'
].join('\n')

const VIEWDIR = [
  'float viewLen(vec3 p) { return length(from); }',
  '#pragma glslify: export(viewLen)'
].join('\n')

const ENTRY = [
  '#version 300 es',
  '// 入口 entry 💧',
  'precision mediump float;',
  '#define MUL_ADD(a, b, c) \\',
  '  ((a) * (b) + (c))',
  '#pragma glslify: cold = require(./gain, factor=2.0)',
  '#pragma glslify: warm = require(./gain, factor=3.0)',
  '#pragma glslify: n1 = require(./noise)',
  '#pragma glslify: mixA = require(./mixer)',
  '#pragma glslify: sc = require(./scale, scaleBy=base)',
  '#pragma glslify: vl = require(./viewdir, from=origin.xyz)',
  'float base = 4.0;',
  'vec3 origin = vec3(0.0, 1.0, 0.0);',
  'out vec4 fragColor;',
  'void main() {',
  '  float v = MUL_ADD(cold(1.0), warm(1.0), n1(0.5));',
  '  fragColor = vec4(v + mixA(v, base) + sc(v) + vl(origin), 0.0, 0.0, 1.0);',
  '}'
].join('\n')

function makeDepsA () {
  return [
    { id: 42, file: 'entry.frag', entry: true, deps: { './gain': 7, './noise': 1001, './mixer': 29, './scale': 500, './viewdir': 913 }, source: ENTRY },
    { id: 500, file: 'scale.glsl', deps: {}, source: SCALE },
    { id: 7, file: 'gain.glsl', deps: {}, source: GAIN },
    { id: 29, file: 'mixer.glsl', deps: { './noise': 1001 }, source: MIXER },
    { id: 913, file: 'viewdir.glsl', deps: {}, source: VIEWDIR },
    { id: 1001, file: 'noise.glsl', deps: {}, source: NOISE }
  ]
}

// A second, unrelated dependency graph for interleaving checks.
function makeDepsB () {
  return [
    { id: 3, file: 'b-entry.frag', entry: true, deps: { './b-mod': 9 }, source: '#pragma glslify: f = require(./b-mod)\nvoid main(){gl_FragColor=vec4(f(1.0));}' },
    { id: 9, file: 'b-mod.glsl', deps: {}, source: 'float f(float x){return x;}\n#pragma glslify: export(f)' }
  ]
}

// ---------------------------------------------------------------------
// Run both entries against set A
// ---------------------------------------------------------------------

{
  const deps = makeDepsA()
  const snapshot = JSON.stringify(deps)

  const plain = bundle(deps)
  const result = bundle.withSourceMap(deps, { file: 'shader.frag' })

  assert.equal(typeof bundle.withSourceMap, 'function')
  assert.equal(result.source, plain, 'mapped source must be byte-identical to bundle()')
  assert.equal(bundle(deps), plain, 'bundle() must be deterministic')
  assert.equal(JSON.stringify(deps), snapshot, 'neither entry may mutate the caller\'s deps')

  const generated = result.source
  const map = result.map
  assert.equal(map.file, 'shader.frag')

  const genLines = generated.split('\n')
  assert.equal(genLines[0], '#version 300 es')
  // the bundler injects its define after the last #version/precision
  // statement — wherever it lands, it must not be mapped
  const defineLine = genLines.indexOf('#define GLSLIFY 1')
  assert.ok(defineLine > genLines.indexOf('precision mediump float;'))

  const ctx = verifyMap(generated, map, deps)
  const { segments, toks, segFor } = ctx
  const nameOf = seg => map.names[seg.name]
  const sourceOf = seg => map.sources[seg.source]

  // --- #version header maps to the very start of the entry file ---
  {
    const ver = toks.find(t => t.type === 'preprocessor' && t.data.startsWith('#version'))
    const hit = segFor(ver)
    assert.equal(sourceOf(hit.seg), 'entry.frag')
    assert.equal(hit.seg.origLine, 0)
    assert.equal(hit.seg.origCol, 0)
  }

  // --- dual specialization: factor=2.0 and factor=3.0 both map back to
  //     the `factor` identifier inside gain.glsl ---
  {
    const gainNorm = GAIN.replace(/\r\n/g, '\n')
    const fLc = lineColOf(gainNorm, gainNorm.indexOf('factor'))
    const hits = segments.filter(s => s.name !== null && nameOf(s) === 'factor')
    assert.equal(hits.length, 2, 'both specializations map to the one `factor` identifier')
    const genSpots = new Set()
    for (const s of hits) {
      assert.equal(sourceOf(s), 'gain.glsl')
      assert.equal(s.origLine, fLc.line)
      assert.equal(s.origCol, fLc.column)
      genSpots.add(s.genLine + ':' + s.genCol)
      const text = genLines[s.genLine].slice(s.genCol, s.genCol + 3)
      assert.ok(text === '2.0' || text === '3.0', 'generated token is the bound value')
    }
    assert.equal(genSpots.size, 2, 'the two bindings appear at two distinct generated positions')
  }

  // --- suffix collisions: three `gain` definitions (two specializations
  //     of gain.glsl plus mixer's local gain) clean up to gain_0/1/2,
  //     and names must record the original spelling, not the short name ---
  {
    const gainLike = toks.filter(t => /^gain_\d+$/.test(t.data))
    assert.deepEqual(Array.from(new Set(gainLike.map(t => t.data))).sort(), ['gain_0', 'gain_1', 'gain_2'])

    const defs = gainLike.map(t => ({ tok: t, hit: segFor(t) }))
    for (const { hit } of defs) {
      assert.ok(['gain', 'cold', 'warm'].includes(nameOf(hit.seg)),
        'names must hold the original identifier, not the cleaned suffix')
    }

    const gainNorm = GAIN.replace(/\r\n/g, '\n')
    const defLc = lineColOf(gainNorm, gainNorm.indexOf('gain('))
    assert.equal(defLc.column, 'float /* 💧💧 */ '.length,
      'columns count UTF-16 units across the non-ASCII block comment')

    const fromGain = defs.filter(d => nameOf(d.hit.seg) === 'gain' && sourceOf(d.hit.seg) === 'gain.glsl')
    assert.equal(fromGain.length, 2, 'two specializations of the same definition')
    assert.ok(fromGain.every(d => d.hit.seg.origLine === defLc.line && d.hit.seg.origCol === defLc.column))
    assert.notEqual(
      fromGain[0].hit.line + ':' + fromGain[0].hit.col,
      fromGain[1].hit.line + ':' + fromGain[1].hit.col)

    const fromMixer = defs.filter(d => nameOf(d.hit.seg) === 'gain' && sourceOf(d.hit.seg) === 'mixer.glsl')
    assert.equal(fromMixer.length, 1)

    const calls = defs.filter(d => sourceOf(d.hit.seg) === 'entry.frag')
    assert.deepEqual(calls.map(d => nameOf(d.hit.seg)).sort(), ['cold', 'warm'])
  }

  // --- shared sub-dependency: noise is imported by both the entry and
  //     mixer, but expanded exactly once ---
  {
    assert.equal((generated.match(/float noise\(/g) || []).length, 1, 'shared dependency expanded once')
    const noiseDefs = segments.filter(s => sourceOf(s) === 'noise.glsl' && s.name !== null && nameOf(s) === 'noise')
    assert.equal(noiseDefs.length, 1)
    assert.equal(noiseDefs[0].origLine, 0)
  }

  // --- nested scopes / shadowing keep the existing rename rules, and
  //     every `y` still maps back into gain.glsl ---
  {
    assert.equal((generated.match(/float y = \w+ \* 2\.0;/g) || []).length, 2)
    // the shadowed inner `y` keeps its name, the outer one is suffixed
    const ys = toks.filter(t => segFor(t) && nameOf(segFor(t).seg) === 'y')
    assert.equal(ys.length, 10, 'five `y` identifiers per specialization')
    assert.ok(ys.some(t => t.data === 'y'))
    assert.ok(ys.some(t => /^y_\d+$/.test(t.data)))
    for (const t of ys) {
      assert.equal(sourceOf(segFor(t).seg), 'gain.glsl')
    }
  }

  // --- macros: the multi-line #define maps to its start in the entry,
  //     and macro identifiers are not renamed ---
  {
    const macro = toks.find(t => t.type === 'preprocessor' && t.data.startsWith('#define MUL_ADD'))
    assert.ok(macro.data.includes('\\\n'), 'multi-line macro preserved')
    const hit = segFor(macro)
    assert.equal(sourceOf(hit.seg), 'entry.frag')
    assert.equal(hit.seg.origLine, 3)
    assert.equal(hit.seg.origCol, 0)

    const call = toks.find(t => t.type === 'ident' && t.data === 'MUL_ADD')
    const callHit = segFor(call)
    assert.equal(nameOf(callHit.seg), 'MUL_ADD')
    const callLc = lineColOf(ENTRY, ENTRY.indexOf('MUL_ADD(cold'))
    assert.equal(callHit.seg.origLine, callLc.line)
    assert.equal(callHit.seg.origCol, callLc.column)
  }

  // --- identifier binding: scaleBy bound to the entry's `base`; the
  //     generated `base` token maps to `scaleBy` inside scale.glsl ---
  {
    const hits = toks.filter(t => t.data === 'base')
      .map(t => ({ t, hit: segFor(t) }))
      .filter(d => nameOf(d.hit.seg) === 'scaleBy')
    assert.equal(hits.length, 1)
    const scLc = lineColOf(SCALE, SCALE.indexOf('scaleBy'))
    assert.equal(sourceOf(hits[0].hit.seg), 'scale.glsl')
    assert.equal(hits[0].hit.seg.origLine, scLc.line)
    assert.equal(hits[0].hit.seg.origCol, scLc.column)
  }

  // --- property binding: `from` bound to `origin.xyz`; every generated
  //     token of the substitution maps to `from` in viewdir.glsl ---
  {
    const fromLc = lineColOf(VIEWDIR, VIEWDIR.indexOf('from'))
    const hits = toks.map(t => ({ t, hit: segFor(t) }))
      .filter(d => d.hit && d.hit.seg.name !== null && nameOf(d.hit.seg) === 'from')
    assert.deepEqual(hits.map(d => d.t.data), ['origin', '.', 'xyz'])
    for (const d of hits) {
      assert.equal(sourceOf(d.hit.seg), 'viewdir.glsl')
      assert.equal(d.hit.seg.origLine, fromLc.line)
      assert.equal(d.hit.seg.origCol, fromLc.column)
    }
  }

  // --- CRLF sources keep their original bytes in sourcesContent, and
  //     positions after CRLF lines still line up ---
  {
    const gi = map.sources.indexOf('gain.glsl')
    assert.ok(map.sourcesContent[gi].includes('\r\n'))
    assert.equal(map.sourcesContent[gi], GAIN)

    const ret = toks.find(t => t.type === 'keyword' && t.data === 'return')
    const hit = segFor(ret)
    assert.equal(sourceOf(hit.seg), 'gain.glsl')
    const retLc = lineColOf(GAIN.replace(/\r\n/g, '\n'), GAIN.replace(/\r\n/g, '\n').indexOf('return'))
    assert.equal(hit.seg.origLine, retLc.line)
    assert.equal(hit.seg.origCol, retLc.column)
  }
}

// ---------------------------------------------------------------------
// Interleaving: mapped and unmapped entries, different dependency
// graphs, repeated calls — results must be stable and self-contained.
// ---------------------------------------------------------------------

{
  const depsA1 = makeDepsA()
  const depsA2 = makeDepsA()
  const depsB = makeDepsB()

  const a1 = bundle.withSourceMap(depsA1, { file: 'a.frag' })
  const bPlain = bundle(depsB)
  const b1 = bundle.withSourceMap(depsB, { file: 'b.frag' })
  const a2 = bundle.withSourceMap(depsA2, { file: 'a.frag' })
  const aPlain = bundle(makeDepsA())

  assert.equal(a1.source, a2.source)
  assert.equal(a1.source, aPlain)
  assert.deepEqual(a1.map, a2.map)
  assert.equal(b1.source, bPlain)

  // no residue from other graphs
  assert.deepEqual(new Set(b1.map.sources), new Set(['b-entry.frag', 'b-mod.glsl']))
  assert.ok(!a1.map.sources.some(f => f.startsWith('b-')))
  assert.ok(!b1.map.names.includes('factor'))
}

// ---------------------------------------------------------------------
// Cyclic dependencies must keep failing loudly from both entries —
// no partial, plausible-looking map may be returned.
// ---------------------------------------------------------------------

{
  const cyclic = [
    { id: 1, file: 'a.glsl', entry: true, deps: { './b': 2 }, source: '#pragma glslify: b = require(./b)\nvoid main(){b();}' },
    { id: 2, file: 'b.glsl', deps: { './a': 1 }, source: '#pragma glslify: a = require(./a)\nvoid b(){a();}\n#pragma glslify: export(b)' }
  ]
  assert.throws(() => bundle(cyclic), /cyclic dependency/)
  assert.throws(() => bundle.withSourceMap(cyclic, { file: 'x.frag' }), /cyclic dependency/)

  // the failure must not corrupt later calls
  const deps = makeDepsB()
  assert.equal(bundle.withSourceMap(deps, { file: 'ok.frag' }).source, bundle(deps))
}

console.log('GLSL_SOURCE_MAP_OK')
