'use strict'

const { test } = require('node:test')
const assert = require('node:assert/strict')
const svgMesh3d = require('../index.js')

const OUTER = 'M0 0H10V10H0Z'
const INNER_SAME = 'M2 2H8V8H2Z'
const INNER_REVERSE = 'M2 2V8H8V2Z'
const BASE = { normalize: false, randomization: 0, simplify: 0 }

function area (mesh) {
  return mesh.cells.reduce((sum, cell) => {
    const a = mesh.positions[cell[0]]
    const b = mesh.positions[cell[1]]
    const c = mesh.positions[cell[2]]
    return sum + Math.abs(
      (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
    ) / 2
  }, 0)
}

// Triangle areas are float sums, so randomization or extra Steiner points
// can shift the total by a few ULPs; compare against the hand value with a
// tight tolerance instead of strict equality.
function assertArea (mesh, expected) {
  assert.ok(
    Math.abs(area(mesh) - expected) < 1e-9,
    `area ${area(mesh)} not close to ${expected}`
  )
}

function build (path, fillRule, extra) {
  return svgMesh3d(path, Object.assign({}, BASE, extra, { fillRule }))
}

// Point-in-triangle test on the (Y-flipped) 3D output.  Sample points are
// always chosen well inside a region, never on a triangle edge.
function pointInMesh (mesh, x, y) {
  const q = [x, -y]
  for (const cell of mesh.cells) {
    const a = mesh.positions[cell[0]]
    const b = mesh.positions[cell[1]]
    const c = mesh.positions[cell[2]]
    const d1 = (q[0] - b[0]) * (a[1] - b[1]) - (a[0] - b[0]) * (q[1] - b[1])
    const d2 = (q[0] - c[0]) * (b[1] - c[1]) - (b[0] - c[0]) * (q[1] - c[1])
    const d3 = (q[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (q[1] - a[1])
    const hasNeg = d1 < 0 || d2 < 0 || d3 < 0
    const hasPos = d1 > 0 || d2 > 0 || d3 > 0
    if (!(hasNeg && hasPos)) return true
  }
  return false
}

// Separating axis test for two convex triangles; triangles sharing an edge
// do not count as overlapping.
function trianglesOverlap (a, b) {
  const tris = [a, b]
  for (let t = 0; t < 2; t++) {
    const tri = tris[t]
    for (let i = 0; i < 3; i++) {
      const p = tri[i]
      const q = tri[(i + 1) % 3]
      const dx = q[0] - p[0]
      const dy = q[1] - p[1]
      let minA = Infinity, maxA = -Infinity
      let minB = Infinity, maxB = -Infinity
      for (let k = 0; k < 3; k++) {
        const va = dx * (tri[k][1] - p[1]) - dy * (tri[k][0] - p[0])
        minA = Math.min(minA, va); maxA = Math.max(maxA, va)
        const o = tris[1 - t][k]
        const vb = dx * (o[1] - p[1]) - dy * (o[0] - p[0])
        minB = Math.min(minB, vb); maxB = Math.max(maxB, vb)
      }
      if (maxA <= minB || maxB <= minA) return false
    }
  }
  return true
}

function assertNoOverlappingTriangles (mesh) {
  const tris = mesh.cells.map((cell) => cell.map((i) => mesh.positions[i]))
  for (let i = 0; i < tris.length; i++) {
    for (let j = i + 1; j < tris.length; j++) {
      assert.equal(
        trianglesOverlap(tris[i], tris[j]), false,
        `triangles ${i} and ${j} overlap`
      )
    }
  }
}

function assertFiniteMesh (mesh) {
  for (const p of mesh.positions) {
    assert.equal(Number.isFinite(p[0]), true)
    assert.equal(Number.isFinite(p[1]), true)
    assert.equal(Number.isFinite(p[2] || 0), true)
  }
  for (const cell of mesh.cells) {
    for (const idx of cell) {
      assert.ok(idx >= 0 && idx < mesh.positions.length)
    }
  }
}

test('hand-computed areas: same-direction inner contour', () => {
  const path = OUTER + INNER_SAME
  assertArea(build(path, 'nonzero'), 100)
  assertArea(build(path, 'evenodd'), 64)
})

test('hand-computed areas: reversed inner contour', () => {
  const path = OUTER + INNER_REVERSE
  assertArea(build(path, 'nonzero'), 64)
  assertArea(build(path, 'evenodd'), 64)
})

test('hand-computed areas: coincident same-direction contours', () => {
  const path = OUTER + OUTER
  assertArea(build(path, 'nonzero'), 100)
  assertArea(build(path, 'evenodd'), 0)
})

test('hand-computed areas: coincident opposite-direction contours', () => {
  const path = OUTER + 'M0 0V10H10V0Z'
  assertArea(build(path, 'nonzero'), 0)
  assertArea(build(path, 'evenodd'), 0)
})

test('coverage samples follow the paint rule, not just total area', () => {
  const path = OUTER + INNER_SAME
  const nonzero = build(path, 'nonzero')
  const evenodd = build(path, 'evenodd')

  // ring region: filled under both rules
  assert.equal(pointInMesh(nonzero, 1, 1), true)
  assert.equal(pointInMesh(evenodd, 1, 1), true)
  // inner square: filled by nonzero, a hole under evenodd
  assert.equal(pointInMesh(nonzero, 5, 5), true)
  assert.equal(pointInMesh(evenodd, 5, 5), false)
  // outside the outer contour: empty under both rules
  assert.equal(pointInMesh(nonzero, 11, 11), false)
  assert.equal(pointInMesh(evenodd, 11, 11), false)
})

test('partially overlapping contours: union vs symmetric difference', () => {
  const path = OUTER + 'M5 0H15V10H5Z'
  const nonzero = build(path, 'nonzero')
  const evenodd = build(path, 'evenodd')

  assertArea(nonzero, 150)
  assertArea(evenodd, 100)

  // single-coverage regions: painted by both rules
  assert.equal(pointInMesh(nonzero, 2.5, 5), true)
  assert.equal(pointInMesh(evenodd, 2.5, 5), true)
  assert.equal(pointInMesh(nonzero, 12.5, 5), true)
  assert.equal(pointInMesh(evenodd, 12.5, 5), true)
  // double-coverage overlap: nonzero keeps it, evenodd empties it
  assert.equal(pointInMesh(nonzero, 7.5, 5), true)
  assert.equal(pointInMesh(evenodd, 7.5, 5), false)
  // fully outside
  assert.equal(pointInMesh(nonzero, 16, 5), false)
  assert.equal(pointInMesh(evenodd, 16, 5), false)

  assertNoOverlappingTriangles(nonzero)
  assertNoOverlappingTriangles(evenodd)
})

test('self-intersecting contour is split by the cleaned boundary', () => {
  const bowtie = 'M0 0L10 10L10 0L0 10Z'
  const nonzero = build(bowtie, 'nonzero')
  const evenodd = build(bowtie, 'evenodd')
  assertArea(nonzero, 50)
  assertArea(evenodd, 50)
  assertNoOverlappingTriangles(nonzero)
  assertNoOverlappingTriangles(evenodd)
})

test('single subpath winding twice: rules must still disagree', () => {
  // one contour: outer square, connector, inner square, connector back.
  // Adjacent duplicate junction points carry no winding of their own.
  const spiral = 'M0 0L10 0L10 10L0 10L0 0L2 2L8 2L8 8L2 8L2 2L0 0'
  const nonzero = build(spiral, 'nonzero')
  const evenodd = build(spiral, 'evenodd')
  assertArea(nonzero, 100)
  assertArea(evenodd, 64)
  assert.equal(pointInMesh(nonzero, 5, 5), true)
  assert.equal(pointInMesh(evenodd, 5, 5), false)
  assertNoOverlappingTriangles(nonzero)
  assertNoOverlappingTriangles(evenodd)
})

test('three levels: outer fill, hole, inner island', () => {
  const path = OUTER + INNER_REVERSE + 'M4 4H6V6H4Z'
  const nonzero = build(path, 'nonzero')
  const evenodd = build(path, 'evenodd')

  // 100 - 36 hole + 4 island
  assertArea(nonzero, 68)
  assertArea(evenodd, 68)

  for (const mesh of [nonzero, evenodd]) {
    assert.equal(pointInMesh(mesh, 1, 1), true)   // outer ring
    assert.equal(pointInMesh(mesh, 3, 3), false)  // hole
    assert.equal(pointInMesh(mesh, 5, 5), true)   // island
    assert.equal(pointInMesh(mesh, 11, 11), false)
    assertNoOverlappingTriangles(mesh)
  }
})

test('subpath ordering does not change the fill', () => {
  const a = build(OUTER + INNER_SAME, 'nonzero')
  const b = build(INNER_SAME + OUTER, 'nonzero')
  assert.equal(area(a), area(b))
  const c = build(OUTER + INNER_SAME, 'evenodd')
  const d = build(INNER_SAME + OUTER, 'evenodd')
  assert.equal(area(c), area(d))
})

test('contours without a Z command close like the default fill habit', () => {
  const open = 'M0 0H10V10H0'
  assertArea(build(open, 'nonzero'), 100)
  assertArea(build(open, 'evenodd'), 100)
})

test('adjacent duplicate points and zero-length edges add no winding', () => {
  const doubled = 'M0 0L0 0H10L10 0V10L10 10H0L0 10V0L0 0Z'
  assertArea(build(doubled, 'nonzero'), 100)
  assertArea(build(doubled, 'evenodd'), 100)
})

test('empty and zero-area paths produce empty meshes without NaN', () => {
  for (const rule of ['nonzero', 'evenodd']) {
    const empty = build('', rule)
    assert.deepEqual(empty.positions, [])
    assert.deepEqual(empty.cells, [])

    const line = build('M0 0L10 10', rule)
    assert.deepEqual(line.cells, [])
    assertFiniteMesh(line)

    const point = build('M0 0L0 0L0 0Z', rule)
    assert.deepEqual(point.cells, [])
    assertFiniteMesh(point)

    const mixed = build('M0 0L10 10' + OUTER, rule)
    assertFiniteMesh(mixed)
    assertArea(mixed, 100)
  }
})

test('retained triangles never overlap and stay finite', () => {
  const paths = [
    OUTER + INNER_SAME,
    OUTER + INNER_REVERSE,
    OUTER + OUTER,
    OUTER + 'M5 0H15V10H5Z',
    'M0 0L10 10L10 0L0 10Z'
  ]
  for (const path of paths) {
    for (const rule of ['nonzero', 'evenodd']) {
      const mesh = build(path, rule)
      assertFiniteMesh(mesh)
      assertNoOverlappingTriangles(mesh)
    }
  }
})

test('invalid fillRule and conflicting options throw TypeError', () => {
  assert.throws(() => svgMesh3d(OUTER, { fillRule: 'winding' }), TypeError)
  assert.throws(() => svgMesh3d(OUTER, { fillRule: 'even-odd' }), TypeError)
  assert.throws(() => svgMesh3d(OUTER, { fillRule: 'nonzero', clean: false }), TypeError)
  assert.throws(() => svgMesh3d(OUTER, { fillRule: 'nonzero', interior: false }), TypeError)
  assert.throws(() => svgMesh3d(OUTER, { fillRule: 'nonzero', exterior: true }), TypeError)
  assert.throws(() => svgMesh3d(OUTER, { fillRule: 'nonzero', infinity: true }), TypeError)
})

test('omitting fillRule keeps the legacy cdt2d output and option passthrough', () => {
  const path = OUTER + INNER_SAME
  // legacy parity behaviour: same-direction inner is a hole
  assertArea(svgMesh3d(path, BASE), 64)
  // explicit undefined is the same as omitting the option
  assertArea(svgMesh3d(path, Object.assign({}, BASE, { fillRule: undefined })), 64)
  // legacy cdt2d options keep being forwarded untouched
  const interior = svgMesh3d(path, Object.assign({}, BASE))
  const withExterior = svgMesh3d(path, Object.assign({}, BASE, { exterior: true }))
  assert.ok(withExterior.cells.length > interior.cells.length)
})

test('caller options object is never mutated', () => {
  const opt = Object.freeze(Object.assign({}, BASE, { fillRule: 'nonzero' }))
  assert.doesNotThrow(() => svgMesh3d(OUTER + INNER_SAME, opt))
  assert.deepEqual(opt, Object.freeze(Object.assign({}, BASE, { fillRule: 'nonzero' })))
})

test('normalize, Y flip and delaunay switches keep working in fill rule mode', () => {
  const path = OUTER + INNER_SAME

  const normalized = build(path, 'nonzero', { normalize: true })
  for (const p of normalized.positions) {
    assert.ok(Math.abs(p[0]) <= 1 + 1e-12)
    assert.ok(Math.abs(p[1]) <= 1 + 1e-12)
  }
  // Y flip: input y in [0, 10] becomes negative; fill must not be inverted
  const raw = build(path, 'nonzero', { normalize: false })
  for (const p of raw.positions) {
    assert.ok(p[1] <= 0)
    assert.equal(p[2], 0)
  }
  assertArea(raw, 100)
  assert.equal(pointInMesh(raw, 5, 5), true)

  const noDelaunay = build(path, 'nonzero', { delaunay: false })
  assertFiniteMesh(noDelaunay)
  assertArea(noDelaunay, 100)
  assertNoOverlappingTriangles(noDelaunay)

  const randomized = build(path, 'nonzero', { randomization: 5 })
  assertFiniteMesh(randomized)
  assertArea(randomized, 100)
})
