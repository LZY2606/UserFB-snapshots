/**
 * Regression tests for line-level bidi reordering and supplementary-plane
 * (surrogate pair) handling. These exercise the public functions exported by
 * src/index.js only, and use UTF-16 code unit indexing throughout.
 *
 * Run with:  node --test checks/bidi_regression.test.mjs
 */
import { test } from 'node:test'
import assert from 'node:assert/strict'
import { existsSync, readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import * as bidi from '../src/index.js'

const RLO = String.fromCharCode(0x202E)
const PDF = String.fromCharCode(0x202C)
const LRI = String.fromCharCode(0x2066)
const RLI = String.fromCharCode(0x2067)
const PDI = String.fromCharCode(0x2069)
/** Escape every non-trivial code unit so the source string is unambiguous. */
function esc (s) {
  let out = '"'
  for (let i = 0; i < s.length; i++) {
    const cu = s.charCodeAt(i)
    if (cu === 0x22) out += '\\"'
    else if (cu === 0x5C) out += '\\\\'
    else if (cu >= 0x20 && cu < 0x7F) out += s[i]
    else out += '\\u' + cu.toString(16).padStart(4, '0')
  }
  return out + '"'
}

/** First position at which two arrays differ, or null when identical. */
function firstDiff (actual, expected) {
  const n = Math.max(actual.length, expected.length)
  for (let i = 0; i < n; i++) {
    if (actual[i] !== expected[i]) {
      return { index: i, actual: actual[i], expected: expected[i] }
    }
  }
  return null
}

/**
 * Diagnostic context attached to every assertion failure: escaped source
 * string, the inclusive code-unit range, the resolved levels, and the first
 * differing index of the values under comparison.
 */
function context (s, range, levels, actual, expected) {
  const parts = [
    `source=${esc(s)} (length ${s.length})`,
    `range=${range ? `[${range[0]},${range[1]}]` : 'default'}`,
    `levels=${JSON.stringify([...levels])}`
  ]
  const diff = firstDiff(actual, expected)
  if (diff) {
    parts.push(`first differing index=${diff.index} actual=${diff.actual} expected=${diff.expected}`)
  }
  return '\n  ' + parts.join('\n  ')
}

function assertIndices (s, range, r, expected) {
  const actual = range
    ? bidi.getReorderedIndices(s, r, range[0], range[1])
    : bidi.getReorderedIndices(s, r)
  assert.deepEqual(actual, expected, context(s, range, r.levels, actual, expected))
  return actual
}

function assertLevels (s, r, expected) {
  const actual = [...r.levels]
  assert.deepEqual(actual, expected, context(s, null, r.levels, actual, expected))
}

/** Invariants of the public UTF-16 code unit contract. */
function assertUtf16Contract (s, r, indices) {
  assert.equal(r.levels.length, s.length, `levels.length must equal string.length for ${esc(s)}`)
  assert.equal(indices.length, s.length, `index count must equal string.length for ${esc(s)}`)
  // the index array is a permutation of every original code unit position
  assert.deepEqual([...indices].sort((a, b) => a - b), s.split('').map((_, i) => i),
    `indices must be a permutation of code unit positions for ${esc(s)}`)
  // both halves of a pair share a level, stay adjacent in the output, high surrogate first
  for (let i = 0; i < s.length - 1; i++) {
    const cu = s.charCodeAt(i)
    if (cu >= 0xD800 && cu <= 0xDBFF && (s.charCodeAt(i + 1) & 0xFC00) === 0xDC00) {
      assert.equal(r.levels[i], r.levels[i + 1], `pair halves must share a level for ${esc(s)} at ${i}`)
      const out = indices.indexOf(i)
      assert.equal(indices[out + 1], i + 1, `pair must stay adjacent, high surrogate first for ${esc(s)} at ${i}`)
    }
  }
}

/**
 * Apply the documented reversal segments at Unicode scalar granularity
 * (segment endpoints are always scalar boundaries), returning code-unit indices.
 */
function applySegments (s, segments) {
  const leaders = []
  const scalarIndexOfUnit = new Uint32Array(s.length)
  for (let i = 0; i < s.length; i++) {
    scalarIndexOfUnit[i] = leaders.length
    leaders.push(i)
    const cu = s.charCodeAt(i)
    if (cu >= 0xD800 && cu <= 0xDBFF && (s.charCodeAt(i + 1) & 0xFC00) === 0xDC00) {
      scalarIndexOfUnit[i + 1] = leaders.length - 1
      i++
    }
  }
  for (const [a, b] of segments) {
    let lo = scalarIndexOfUnit[a]
    let hi = scalarIndexOfUnit[b]
    while (lo < hi) {
      const t = leaders[lo]
      leaders[lo] = leaders[hi]
      leaders[hi] = t
      lo++
      hi--
    }
  }
  const out = []
  for (const leader of leaders) {
    out.push(leader)
    const cu = s.charCodeAt(leader)
    if (cu >= 0xD800 && cu <= 0xDBFF && (s.charCodeAt(leader + 1) & 0xFC00) === 0xDC00) {
      out.push(leader + 1)
    }
  }
  return out
}

// ---------------------------------------------------------------------------
// L1 is applied per line on a local copy: trailing whitespace stays right
// ---------------------------------------------------------------------------

test('L1 trailing whitespace on a wrapped line stays at the right of that line', () => {
  const s = 'a אב  ג'
  const r = bidi.getEmbeddingLevels(s, 'ltr')
  const levelsBefore = [...r.levels]
  assertLevels(s, r, [0, 0, 1, 1, 1, 1, 1])

  // reorder only line interval [2,5]: the two trailing spaces must not jump left
  assertIndices(s, [2, 5], r, [0, 1, 3, 2, 4, 5, 6])

  // getEmbeddingLevels' own array is never mutated by a line reorder
  assert.deepEqual([...r.levels], levelsBefore,
    context(s, [2, 5], r.levels, [...r.levels], levelsBefore))
  assertUtf16Contract(s, r, bidi.getReorderedIndices(s, r, 2, 5))

  // whole-paragraph default behavior is unchanged
  assertIndices(s, null, r, [0, 1, 6, 5, 4, 3, 2])
})

test('lines processed repeatedly in any order never influence each other', () => {
  const s = 'a אב  ג'
  const r = bidi.getEmbeddingLevels(s, 'ltr')
  const levelsBefore = [...r.levels]
  const expected = [0, 1, 3, 2, 4, 5, 6]

  for (let round = 0; round < 3; round++) {
    assertIndices(s, [6, 6], r, [0, 1, 2, 3, 4, 5, 6])
    assertIndices(s, [0, 1], r, [0, 1, 2, 3, 4, 5, 6])
    assertIndices(s, [2, 5], r, expected)
    assert.deepEqual([...r.levels], levelsBefore, 'levels mutated across line calls')
  }
})

test('a range crossing a paragraph boundary leaves every unit reordered within its own paragraph', () => {
  const s = 'ab\nאב  ג'
  const r = bidi.getEmbeddingLevels(s, 'ltr')
  const levelsBefore = [...r.levels]
  assert.equal(r.paragraphs.length, 2)

  // full range spans both paragraphs
  assertIndices(s, [0, 7], r, [0, 1, 2, 7, 6, 5, 4, 3])
  // a range that starts in paragraph 1 and ends in paragraph 2
  assertIndices(s, [2, 5], r, [0, 1, 2, 4, 3, 5, 6, 7])
  // units outside the requested interval keep their original positions
  const partial = bidi.getReorderedIndices(s, r, 3, 4)
  assert.equal(partial[0], 0)
  assert.equal(partial[1], 1)
  assert.equal(partial[2], 2)
  assert.equal(partial[5], 5)
  assert.equal(partial[6], 6)
  assert.equal(partial[7], 7)
  assert.deepEqual([...r.levels], levelsBefore, 'levels mutated by cross-paragraph call')
})

// ---------------------------------------------------------------------------
// Supplementary plane: valid surrogate pairs are one Unicode scalar
// ---------------------------------------------------------------------------

test('RLO override text keeps an emoji surrogate pair intact', () => {
  const s = RLO + '😀A' + PDF // RLO, emoji surrogate pair, A, PDF
  const r = bidi.getEmbeddingLevels(s, 'ltr')
  assertLevels(s, r, [0, 1, 1, 1, 0])

  const indices = assertIndices(s, null, r, [0, 3, 1, 2, 4])
  assertUtf16Contract(s, r, indices)

  const visual = bidi.getReorderedString(s, r)
  const units = Array.from({ length: visual.length }, (_, i) => visual.charCodeAt(i))
  assert.deepEqual(units, [0x202E, 0x41, 0xD83D, 0xDE00, 0x202C],
    context(s, null, r.levels, units, [0x202E, 0x41, 0xD83D, 0xDE00, 0x202C]))
  assert.equal(visual, RLO + 'A😀' + PDF)
})

test('explicit ltr gives a supplementary R scalar level 1 on both code units', () => {
  const s = '𐤀A'
  const r = bidi.getEmbeddingLevels(s, 'ltr')
  assertLevels(s, r, [1, 1, 0])
  assert.equal(bidi.getBidiCharTypeName('𐤀'), 'R')
  assertUtf16Contract(s, r, bidi.getReorderedIndices(s, r))
})

test('Hebrew with an emoji reorders the emoji as one scalar', () => {
  const s = 'אב😀ג'
  const r = bidi.getEmbeddingLevels(s, 'ltr')
  assertLevels(s, r, [1, 1, 1, 1, 1])
  const indices = assertIndices(s, null, r, [4, 2, 3, 1, 0])
  assertUtf16Contract(s, r, indices)
  assert.equal(bidi.getReorderedString(s, r), 'ג😀בא')
})

test('lone surrogate code units are retained as default L-type characters', () => {
  const s = 'a\uD800b\uDE00c'
  const r = bidi.getEmbeddingLevels(s, 'ltr')
  assert.equal(r.levels.length, 5)
  assert.equal(bidi.getBidiCharTypeName('\uD800'), 'L')
  assert.equal(bidi.getBidiCharTypeName('\uDE00'), 'L')
  const indices = bidi.getReorderedIndices(s, r)
  assert.deepEqual(indices, [0, 1, 2, 3, 4])
  assert.equal(bidi.getReorderedString(s, r), s) // nothing dropped, nothing reordered
})

test('a line boundary that splits a surrogate pair throws RangeError', () => {
  const s = 'a𐤀b' // units: 0='a', 1=high, 2=low, 3='b'
  const r = bidi.getEmbeddingLevels(s, 'ltr')
  for (const [start, end] of [[0, 1], [2, 3], [2, 2]]) {
    assert.throws(() => bidi.getReorderSegments(s, r, start, end), RangeError)
    assert.throws(() => bidi.getReorderedIndices(s, r, start, end), RangeError)
    assert.throws(() => bidi.getReorderedString(s, r, start, end), RangeError)
  }
  // ranges containing the whole pair are accepted
  assert.doesNotThrow(() => {
    bidi.getReorderedIndices(s, r, 1, 2)
    bidi.getReorderedIndices(s, r, 0, 2)
    bidi.getReorderedIndices(s, r)
  })
})

test('supplementary R character mixed with brackets mirrors correctly and keeps the pair whole', () => {
  const s = 'a(𐤀)b'
  const r = bidi.getEmbeddingLevels(s, 'ltr')
  assertLevels(s, r, [0, 0, 1, 1, 0, 0])
  let indices = bidi.getReorderedIndices(s, r)
  assertUtf16Contract(s, r, indices)
  assert.deepEqual(indices, [0, 1, 2, 3, 4, 5])
  assert.equal(bidi.getReorderedString(s, r), 'a(𐤀)b')

  const rtl = 'אב(𐤀)ג'
  const rr = bidi.getEmbeddingLevels(rtl, 'ltr')
  assertLevels(rtl, rr, [1, 1, 1, 1, 1, 1, 1])
  indices = assertIndices(rtl, null, rr, [6, 5, 3, 4, 2, 1, 0])
  assertUtf16Contract(rtl, rr, indices)
  assert.equal(bidi.getReorderedString(rtl, rr), 'ג(𐤀)בא')
})

// ---------------------------------------------------------------------------
// Consistency between the three public reordering entry points
// ---------------------------------------------------------------------------

test('getReorderSegments applied in order matches getReorderedIndices and getReorderedString', () => {
  const samples = [
    ['a אב  ג', null],
    ['a אב  ג', [2, 5]],
    ['אב😀ג', null],
    [RLO + '😀A' + PDF, null],
    ['𐤀A', null],
    ['a(1) bאב!', null],
    ['אב(𐤀)ג', [2, 5]],
    ['ab\nאב  ג', [3, 6]]
  ]
  for (const [s, range] of samples) {
    const r = bidi.getEmbeddingLevels(s, 'ltr')
    const segments = range
      ? bidi.getReorderSegments(s, r, range[0], range[1])
      : bidi.getReorderSegments(s, r)
    // every segment endpoint lies on a scalar boundary
    for (const [a, b] of segments) {
      assert.ok((s.charCodeAt(a) & 0xFC00) !== 0xDC00, `segment start inside pair: ${esc(s)}`)
      assert.ok((s.charCodeAt(b) & 0xFC00) !== 0xD800, `segment end inside pair: ${esc(s)}`)
    }
    const viaSegments = applySegments(s, segments)
    const viaIndices = range
      ? bidi.getReorderedIndices(s, r, range[0], range[1])
      : bidi.getReorderedIndices(s, r)
    assert.deepEqual(viaSegments, viaIndices, context(s, range, r.levels, viaSegments, viaIndices))
    assertUtf16Contract(s, r, viaIndices)

    // the reordered string follows that same order, applying mirroring on odd levels
    const expectedString = viaIndices.map(i =>
      ((r.levels[i] & 1) ? bidi.getMirroredCharacter(s[i]) : null) || s[i]).join('')
    const actualString = range
      ? bidi.getReorderedString(s, r, range[0], range[1])
      : bidi.getReorderedString(s, r)
    assert.equal(actualString, expectedString,
      context(s, range, r.levels, Array.from(actualString), Array.from(expectedString)))
  }
})

// ---------------------------------------------------------------------------
// Additional line-level scenarios
// ---------------------------------------------------------------------------

test('a whitespace-only line is left untouched and does not mutate levels', () => {
  const s = 'abc   def'
  const r = bidi.getEmbeddingLevels(s, 'ltr')
  const levelsBefore = [...r.levels]
  assertIndices(s, [3, 5], r, [0, 1, 2, 3, 4, 5, 6, 7, 8])
  assert.deepEqual([...r.levels], levelsBefore)
})

test('nested isolates resolve and reorder without losing formatting characters', () => {
  const s = 'a' + LRI + RLI + 'א' + PDI + 'b' + PDI + 'c'
  const r = bidi.getEmbeddingLevels(s, 'ltr')
  assertLevels(s, r, [0, 0, 2, 3, 2, 2, 0, 0])
  const indices = bidi.getReorderedIndices(s, r)
  assertUtf16Contract(s, r, indices)
  assert.deepEqual(indices, [0, 1, 2, 3, 4, 5, 6, 7])
  assert.equal(bidi.getReorderedString(s, r), s)
})

test('soft wrap of the second paragraph applies L1 to that line only', () => {
  const s = 'ab\nאב  ג'
  const r = bidi.getEmbeddingLevels(s, 'ltr')
  const levelsBefore = [...r.levels]

  // line [4,7] = ב, space, space, ג -> ג, space, space, ב ; units before the line stay put
  assertIndices(s, [4, 7], r, [0, 1, 2, 3, 7, 6, 5, 4])
  // line [3,6] = א, ב, space, space -> ב, א, space, space
  assertIndices(s, [3, 6], r, [0, 1, 2, 4, 3, 5, 6, 7])

  assert.equal(bidi.getReorderedString(s, r, 4, 7), 'ab\nאג  ב')
  assert.equal(bidi.getReorderedString(s, r, 3, 6), 'ab\nבא  ג')
  assert.deepEqual([...r.levels], levelsBefore, 'paragraph levels must not be mutated')
})

test('empty string stays empty', () => {
  const r = bidi.getEmbeddingLevels('', 'ltr')
  assert.equal(r.levels.length, 0)
  assert.deepEqual(bidi.getReorderedIndices('', r), [])
  assert.equal(bidi.getReorderedString('', r), '')
})

// ---------------------------------------------------------------------------
// Cross-check against the local Unicode fixture, converting codepoint
// positions in the file to public UTF-16 code unit positions explicitly
// ---------------------------------------------------------------------------

test('supplementary substitution matches the Unicode fixture after codepoint-to-UTF16 conversion', () => {
  const fixturePath = fileURLToPath(new URL('../test/BidiCharacterTest.txt', import.meta.url))
  if (!existsSync(fixturePath)) {
    // Fixture is optional in this environment; nothing to cross-check.
    return
  }
  const line = readFileSync(fixturePath, 'utf-8').split('\n')[41] // 1-based line 42
  const [input, , , expectedCpLevels, expectedCpOrder] = line.split(';')
  const codepoints = input.trim().split(/\s+/).map(h => parseInt(h, 16))

  // Swap the first BMP R character for a supplementary R character (Phoenician alef, U+10900)
  codepoints[0] = 0x10900
  const s = String.fromCodePoint(...codepoints)

  // Explicit codepoint -> leading UTF-16 code unit offset table
  const unitStart = []
  let unit = 0
  for (const cp of codepoints) {
    unitStart.push(unit)
    unit += cp > 0xFFFF ? 2 : 1
  }
  const unitLength = unit

  const cpLevels = expectedCpLevels.trim().split(/\s+/).map(Number)
  const expectedUnitLevels = []
  codepoints.forEach((cp, i) => {
    expectedUnitLevels.push(cpLevels[i])
    if (cp > 0xFFFF) expectedUnitLevels.push(cpLevels[i])
  })
  const expectedUnitOrder = []
  for (const cpi of expectedCpOrder.trim().split(/\s+/).map(Number)) {
    expectedUnitOrder.push(unitStart[cpi])
    if (codepoints[cpi] > 0xFFFF) expectedUnitOrder.push(unitStart[cpi] + 1)
  }

  // The two arrays have different lengths precisely because of the surrogate pair,
  // so they must never be compared against each other directly.
  assert.notEqual(cpLevels.length, expectedUnitLevels.length)
  assert.equal(s.length, unitLength)

  const r = bidi.getEmbeddingLevels(s, 'ltr')
  const actualLevels = [...r.levels]
  assert.deepEqual(actualLevels, expectedUnitLevels,
    context(s, null, r.levels, actualLevels, expectedUnitLevels))
  const actualOrder = bidi.getReorderedIndices(s, r)
  assert.deepEqual(actualOrder, expectedUnitOrder,
    context(s, null, r.levels, actualOrder, expectedUnitOrder))
  assertUtf16Contract(s, r, actualOrder)
})
