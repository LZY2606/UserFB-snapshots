/**
 * Generator for the minimal TrueType SFNT fonts used by checks/target.test.mjs.
 *
 * Every font carries real TrueType outlines (simple rectangle contours) so the
 * stretch paths can be checked control-point by control-point, plus a MATH
 * table exercising both coverage formats, unsorted variants, assemblies with
 * zero/two extenders, two distinct extender geometries, differing connector
 * bounds, a negative italics correction and a construction that cannot grow.
 */

import {
    u16, u32, i16, pad,
    makeHead, makeHhea, makeMaxp, makeOS2, makeName, makeHmtx, makeCmap, makePost,
    assembleFont,
} from '../test/font-generation-helpers.mjs';

export const NUM_GLYPHS = 17;
export const UNITS_PER_EM = 1000;
export const MIN_CONNECTOR_OVERLAP = 20;

// Glyph indices used by the MATH data.
export const GLYPH = {
    NOTDEF: 0,
    HARROW: 1,      // horizontally stretchable arrow base glyph
    HARROW_BIG: 2,  // large horizontal variant
    H_LEFT: 3,      // assembly: left end, non-extender
    H_EXT_1: 4,     // assembly: extender geometry A
    H_EXT_2: 5,     // assembly: extender geometry B
    H_RIGHT: 6,     // assembly: right end, non-extender
    FROZEN: 7,      // assembly without extenders: cannot keep growing
    FROZEN_P1: 8,
    FROZEN_P2: 9,
    VBRACKET: 10,   // vertically stretchable bracket base glyph
    VBRACKET_BIG: 11,
    V_BOTTOM: 12,
    V_EXT: 13,
    V_TOP: 14,
    PLAIN: 15,      // glyph not covered by any MATH coverage
    NEG_IC: 16      // single-extender assembly with a negative italics correction
};

// Rectangle outlines (font coordinates, y upward) keyed by glyph index.
const RECTS = {
    [GLYPH.HARROW]: [0, 0, 100, 60],
    [GLYPH.HARROW_BIG]: [0, 0, 120, 60],
    [GLYPH.H_LEFT]: [0, 0, 60, 40],
    [GLYPH.H_EXT_1]: [0, 0, 40, 40],
    [GLYPH.H_EXT_2]: [0, 0, 40, 30],
    [GLYPH.H_RIGHT]: [0, 0, 55, 40],
    [GLYPH.FROZEN]: [0, 0, 80, 50],
    [GLYPH.FROZEN_P1]: [0, 0, 50, 50],
    [GLYPH.FROZEN_P2]: [0, 10, 50, 60],
    [GLYPH.VBRACKET]: [0, 0, 60, 100],
    [GLYPH.VBRACKET_BIG]: [0, 0, 60, 120],
    [GLYPH.V_BOTTOM]: [0, 0, 40, 60],
    [GLYPH.V_EXT]: [0, 0, 40, 40],
    [GLYPH.V_TOP]: [0, 0, 40, 55],
    [GLYPH.PLAIN]: [0, 0, 70, 70],
    [GLYPH.NEG_IC]: [0, 0, 45, 45]
};

class ByteBuilder {
    constructor() {
        this.bytes = [];
    }

    get length() {
        return this.bytes.length;
    }

    pos() {
        return this.bytes.length;
    }

    u16(value) {
        this.bytes.push(...u16(value));
    }

    i16(value) {
        this.bytes.push(...i16(value));
    }

    u32(value) {
        this.bytes.push(...u32(value));
    }

    patch16(position, value) {
        const encoded = u16(value);
        this.bytes[position] = encoded[0];
        this.bytes[position + 1] = encoded[1];
    }
}

// Encode one simple single-contour rectangle glyph.
function encodeRectangleGlyf([x0, y0, x1, y1]) {
    const points = [[x0, y0], [x0, y1], [x1, y1], [x1, y0]];
    const deltas = [];
    let px = 0;
    let py = 0;
    for (const [x, y] of points) {
        deltas.push([x - px, y - py]);
        px = x;
        py = y;
    }
    const flags = [];
    const xBytes = [];
    const yBytes = [];
    for (const [dx, dy] of deltas) {
        let flag = 0x01; // on-curve
        if (dx === 0) {
            // Zero delta: reuse the previous coordinate without emitting bytes.
            flag |= 0x10;
        } else {
            flag |= 0x02; // x-Short
            if (dx > 0) {
                flag |= 0x10; // positive
            }
            xBytes.push(Math.abs(dx));
        }
        if (dy === 0) {
            flag |= 0x20;
        } else {
            flag |= 0x04; // y-Short
            if (dy > 0) {
                flag |= 0x20; // positive
            }
            yBytes.push(Math.abs(dy));
        }
        flags.push(flag);
    }
    const bytes = [
        ...i16(1), // numberOfContours
        ...i16(x0), ...i16(y0), ...i16(x1), ...i16(y1), // bounding box
        ...u16(3), // endPtsOfContours[0]
        ...u16(0), // instructionLength
        ...flags,
        ...xBytes,
        ...yBytes
    ];
    if (bytes.length % 2 !== 0) {
        bytes.push(0);
    }
    return bytes;
}

function makeGlyfAndLoca() {
    const glyf = [];
    const loca = [0];
    // .notdef: empty glyph (header only, no contours).
    const notdef = pad([...i16(0), ...i16(0), ...i16(0), ...i16(0), ...i16(0)]);
    glyf.push(...notdef);
    loca.push(glyf.length / 2);
    for (let gid = 1; gid < NUM_GLYPHS; gid += 1) {
        const record = pad(encodeRectangleGlyf(RECTS[gid]));
        glyf.push(...record);
        loca.push(glyf.length / 2);
    }
    return {
        glyf: glyf,
        // indexToLocFormat = 0: each loca offset is a 16-bit half-unit value.
        loca: loca.reduce((encoded, offset) => encoded.concat(u16(offset)), [])
    };
}

function partRecord(builder, glyphIndex, startConnector, endConnector, fullAdvance, isExtender) {
    builder.u16(glyphIndex);
    builder.u16(startConnector);
    builder.u16(endConnector);
    builder.u16(fullAdvance);
    builder.u16(isExtender ? 1 : 0);
}

// Build the MathVariants blob. All offsets are relative to its own start.
function buildMathVariants({ badPartGlyphIndex = false } = {}) {
    const b = new ByteBuilder();

    // MathVariants header.
    b.u16(MIN_CONNECTOR_OVERLAP); // minConnectorOverlap
    const vertCoverageOffsetPos = b.pos();
    b.u16(0);
    const horizCoverageOffsetPos = b.pos();
    b.u16(0);
    b.u16(1); // vertGlyphCount
    const vertConstructionOffsetPos = b.pos();
    b.u16(0);
    b.u16(3); // horizGlyphCount
    const horizConstructionOffsetPositions = [];
    for (let i = 0; i < 3; i += 1) {
        horizConstructionOffsetPositions.push(b.pos());
        b.u16(0);
    }

    // Vertical coverage: format 2, single range covering VBRACKET.
    b.patch16(vertCoverageOffsetPos, b.pos());
    b.u16(2); // coverageFormat
    b.u16(1); // rangeCount
    b.u16(GLYPH.VBRACKET);
    b.u16(GLYPH.VBRACKET);
    b.u16(0); // startCoverageIndex

    // Horizontal coverage: format 1, explicit glyph list.
    b.patch16(horizCoverageOffsetPos, b.pos());
    b.u16(1); // coverageFormat
    b.u16(3); // glyphCount
    b.u16(GLYPH.HARROW);
    b.u16(GLYPH.FROZEN);
    b.u16(GLYPH.NEG_IC);

    // Vertical construction for VBRACKET: unsorted variants + one assembly.
    b.patch16(vertConstructionOffsetPos, b.pos());
    const vertAssemblyOffsetPos = b.pos();
    b.u16(0);
    b.u16(2); // variantCount, deliberately unsorted
    b.u16(GLYPH.VBRACKET_BIG);
    b.u16(160);
    b.u16(GLYPH.VBRACKET);
    b.u16(100);
    b.patch16(vertAssemblyOffsetPos, b.pos());
    b.i16(0); // italicsCorrection design value
    b.u16(0); // italicsCorrection device offset (unused)
    b.u16(3); // partCount
    partRecord(b, GLYPH.V_BOTTOM, 30, 24, 100, false);
    partRecord(b, GLYPH.V_EXT, 22, 22, 80, true);
    partRecord(b, GLYPH.V_TOP, 23, 20, 100, false);

    // Horizontal construction for HARROW: unsorted variants (including an
    // advance tie) + a two-extender assembly.
    b.patch16(horizConstructionOffsetPositions[0], b.pos());
    const arrowAssemblyOffsetPos = b.pos();
    b.u16(0);
    b.u16(4); // variantCount, deliberately not size-sorted
    b.u16(GLYPH.HARROW_BIG);
    b.u16(170);
    b.u16(GLYPH.HARROW);
    b.u16(120);
    b.u16(GLYPH.H_RIGHT);
    b.u16(140);
    b.u16(GLYPH.H_LEFT);
    b.u16(140); // same advance as the previous record: tie breaks on glyph index
    b.patch16(arrowAssemblyOffsetPos, b.pos());
    b.i16(35); // positive italics correction design value
    b.u16(0);
    b.u16(4); // partCount
    partRecord(b, GLYPH.H_LEFT, 30, 26, 100, false);
    partRecord(b, GLYPH.H_EXT_1, 22, 22, 80, true);
    partRecord(b, GLYPH.H_EXT_2, 21, 23, 80, true);
    partRecord(b, GLYPH.H_RIGHT, 25, 30, 100, false);

    // Frozen construction: no variants, no extenders, fixed reachable length.
    b.patch16(horizConstructionOffsetPositions[1], b.pos());
    const frozenAssemblyOffsetPos = b.pos();
    b.u16(0);
    b.u16(0); // variantCount
    b.patch16(frozenAssemblyOffsetPos, b.pos());
    b.i16(0);
    b.u16(0);
    b.u16(2); // partCount
    partRecord(b, badPartGlyphIndex ? 0xFFF0 : GLYPH.FROZEN_P1, 25, 25, 100, false);
    partRecord(b, GLYPH.FROZEN_P2, 25, 25, 100, false);

    // Negative-italics construction: a single extender part, so r = 0
    // produces an empty (non-viable) sequence.
    b.patch16(horizConstructionOffsetPositions[2], b.pos());
    const negAssemblyOffsetPos = b.pos();
    b.u16(0);
    b.u16(0); // variantCount
    b.patch16(negAssemblyOffsetPos, b.pos());
    b.i16(-40); // negative italics correction design value
    b.u16(0);
    b.u16(1); // partCount
    partRecord(b, GLYPH.H_EXT_1, 22, 22, 80, true);

    return b.bytes;
}

function makeMathTable(options) {
    const variants = buildMathVariants(options);
    const builder = new ByteBuilder();
    builder.u16(1); // majorVersion
    builder.u16(0); // minorVersion
    builder.u16(0); // MathConstants offset (NULL)
    builder.u16(0); // MathGlyphInfo offset (NULL)
    builder.u16(10); // MathVariants offset
    builder.bytes.push(...variants);
    return pad(builder.bytes);
}

/**
 * Assemble the test font.
 * @param {Object} [options]
 * @param {boolean} [options.includeMath=true] - emit the MATH table.
 * @param {boolean} [options.badPartGlyphIndex=false] - emit an out-of-range part index.
 * @param {boolean} [options.truncatedMath=false] - cut the MATH table bytes short.
 * @returns {Uint8Array}
 */
export function buildMathFont(options = {}) {
    const {
        includeMath = true,
        badPartGlyphIndex = false,
        truncatedMath = false
    } = options;
    const { glyf, loca } = makeGlyfAndLoca();
    const tables = {
        'OS/2': makeOS2(),
        'cmap': makeCmap(undefined),
        'glyf': glyf,
        'head': makeHead({ indexToLocFormat: 0 }),
        'hhea': makeHhea(NUM_GLYPHS),
        'hmtx': makeHmtx(NUM_GLYPHS, 500),
        'loca': loca,
        'maxp': makeMaxp(NUM_GLYPHS),
        'name': makeName('MathStretchTest'),
        'post': makePost()
    };
    if (includeMath) {
        const mathBytes = makeMathTable({ badPartGlyphIndex: badPartGlyphIndex });
        tables.MATH = truncatedMath ? mathBytes.slice(0, 12) : mathBytes;
    }
    return assembleFont(tables);
}

/**
 * Build a font whose MATH directory entry points past the end of the file,
 * so the parser must reject it instead of reading neighbouring table data.
 */
export function buildFontWithDanglingMathOffset() {
    const bytes = Array.from(buildMathFont());
    // Locate the MATH table record in the table directory.
    const numTables = u16FromBytes(bytes, 4);
    let mathRecordOffset = -1;
    for (let i = 0; i < numTables; i += 1) {
        const recordOffset = 12 + i * 16;
        const tag = String.fromCharCode(...bytes.slice(recordOffset, recordOffset + 4));
        if (tag === 'MATH') {
            mathRecordOffset = recordOffset;
            break;
        }
    }
    if (mathRecordOffset < 0) {
        throw new Error('test font has no MATH record');
    }
    // Corrupt the MathVariants offset inside the MATH table itself.
    const mathTableOffset = u32FromBytes(bytes, mathRecordOffset + 8);
    const mathVariantsFieldOffset = mathTableOffset + 8;
    bytes[mathVariantsFieldOffset] = 0xFF;
    bytes[mathVariantsFieldOffset + 1] = 0xFF;
    return new Uint8Array(bytes);
}

function u16FromBytes(bytes, offset) {
    return (bytes[offset] << 8) | bytes[offset + 1];
}

function u32FromBytes(bytes, offset) {
    return ((bytes[offset] << 24) | (bytes[offset + 1] << 16) |
        (bytes[offset + 2] << 8) | bytes[offset + 3]) >>> 0;
}
