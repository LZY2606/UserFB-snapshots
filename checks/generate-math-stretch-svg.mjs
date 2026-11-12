/**
 * Renders an SVG showcasing the MATH stretch support: one vertically
 * assembled bracket and one horizontally assembled long arrow, both drawn
 * through Font.getStretchPath() from the generated micro SFNT test font.
 *
 * Usage: node checks/generate-math-stretch-svg.mjs
 * Output: docs/math-stretch.svg
 */

import { writeFileSync, mkdirSync } from 'fs';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';
import { parse } from '../src/opentype.mjs';
import { buildMathFont, GLYPH } from './math-test-font.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));

const bytes = buildMathFont();
const font = parse(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength));

const fontSize = 200;
const scale = 1 / font.unitsPerEm * fontSize;

// Vertical bracket assembled from bottom/extender/top parts.
const bracketTarget = 480;
const bracketPlan = font.getStretchGlyph(GLYPH.VBRACKET, 'vertical', bracketTarget);
const bracketPath = font.getStretchPath(GLYPH.VBRACKET, 'vertical', bracketTarget, 60, 140, fontSize);

// Horizontal arrow assembled from the two distinct extender geometries.
const arrowTarget = 620;
const arrowPlan = font.getStretchGlyph(GLYPH.HARROW, 'horizontal', arrowTarget);
const arrowPath = font.getStretchPath(GLYPH.HARROW, 'horizontal', arrowTarget, 130, 140, fontSize);

const width = 320;
const height = 220;
const svg = [
    '<?xml version="1.0" encoding="UTF-8"?>',
    '<svg xmlns="http://www.w3.org/2000/svg" width="' + width + '" height="' + height + '"',
    '     viewBox="0 0 ' + width + ' ' + height + '" role="img"',
    '     aria-label="MATH stretch glyph assembly: vertical bracket and horizontal arrow">',
    '  <rect width="100%" height="100%" fill="white"/>',
    '  <text x="12" y="24" font-family="sans-serif" font-size="13" fill="#333">MATH assembly: ' +
        bracketPlan.parts.length + ' bracket parts / ' + arrowPlan.parts.length + ' arrow parts</text>',
    '  <g fill="#1a4f8b">',
    '    ' + bracketPath.toSVG({ decimalPlaces: 2, optimize: false, flipY: false }),
    '  </g>',
    '  <g fill="#8b1a4f">',
    '    ' + arrowPath.toSVG({ decimalPlaces: 2, optimize: false, flipY: false }),
    '  </g>',
    '</svg>',
    ''
].join('\n');

const outDir = join(__dirname, '..', 'docs');
mkdirSync(outDir, { recursive: true });
const outPath = join(outDir, 'math-stretch.svg');
writeFileSync(outPath, svg);
console.log('wrote', outPath, 'scale=' + scale,
    'bracket advance=' + bracketPlan.advance + ' arrow advance=' + arrowPlan.advance);
