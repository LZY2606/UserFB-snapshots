#!/usr/bin/env node
'use strict'

// Diagnostic only: renders the nonzero and evenodd triangulations of one
// SVG path side by side.  Correctness is decided by the numeric tests in
// checks/fill_rule.test.cjs, not by this picture.
//
// Usage:
//   NODE_PATH=checks/runtime/node_modules node checks/fill_rule_svg.cjs [out.svg] ["<svg path>"]

const fs = require('node:fs')
const svgMesh3d = require('../index.js')

const outFile = process.argv[2] || 'fill_rule_comparison.svg'
const inputPath = process.argv[3] ||
  'M0 0H10V10H0Z' + 'M2 2H8V8H2Z'

const base = { normalize: false, randomization: 0, simplify: 0 }
const meshes = [
  { title: 'fillRule: nonzero', mesh: svgMesh3d(inputPath, Object.assign({ fillRule: 'nonzero' }, base)) },
  { title: 'fillRule: evenodd', mesh: svgMesh3d(inputPath, Object.assign({ fillRule: 'evenodd' }, base)) }
]

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

// Reuse the pre-flip coordinates: mesh y has been negated for OpenGL.
function xy (p) {
  return [p[0], -p[1]]
}

function render (entry, offsetX, minY, span) {
  const mesh = entry.mesh
  const parts = []
  parts.push(`<g transform="translate(${offsetX},0)">`)
  parts.push(`<text x="10" y="${-minY + 24}" font-family="monospace" font-size="16">${entry.title} (area ${area(mesh).toFixed(3)})</text>`)
  parts.push(`<g transform="translate(10,${-minY + 40})">`)

  for (const cell of mesh.cells) {
    const pts = cell.map((i) => xy(mesh.positions[i]).join(',')).join(' ')
    parts.push(`<polygon points="${pts}" fill="#7aa6ff" fill-opacity="0.55" stroke="#1d3f8f" stroke-width="0.08" />`)
  }
  for (const p of mesh.positions) {
    const [x, y] = xy(p)
    parts.push(`<circle cx="${x}" cy="${y}" r="0.12" fill="#b00020" />`)
  }

  parts.push('</g></g>')
  return parts.join('\n')
}

const allX = []
const allY = []
for (const entry of meshes) {
  for (const p of entry.mesh.positions) {
    allX.push(p[0])
    allY.push(-p[1])
  }
}
const minX = Math.min.apply(null, allX), maxX = Math.max.apply(null, allX)
const minY = Math.min.apply(null, allY), maxY = Math.max.apply(null, allY)
const spanX = Math.max(maxX - minX, 1)
const spanY = Math.max(maxY - minY, 1)
const panel = spanX + 20
const width = panel * 2 + 10
const height = spanY + 60

const svg = [
  '<?xml version="1.0" encoding="UTF-8"?>',
  `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}" viewBox="0 0 ${width} ${height}">`,
  '<rect width="100%" height="100%" fill="white"/>',
  `<line x1="${width / 2}" y1="0" x2="${width / 2}" y2="${height}" stroke="#cccccc" />`,
  render(meshes[0], 0, minY, spanX),
  render(meshes[1], panel, minY, spanX),
  '</svg>',
  ''
].join('\n')

fs.writeFileSync(outFile, svg)
console.log(`wrote ${outFile} for path: ${inputPath}`)
