// The `MATH` table provides glyph constructions for mathematical typesetting.
// Only the MathVariants subtree is interpreted here: stretch glyph variants
// and assemblies, plus the two coverage tables that index them.
// MathConstants and MathGlyphInfo (MathItalicsCorrectionInfo /
// MathTopAccentAttachment) are skipped, as they are not needed for stretching.
// https://learn.microsoft.com/en-us/typography/opentype/spec/math

import check from '../check.mjs';
import { Parser } from '../parse.mjs';

// MathValueRecord: a signed design value followed by an optional
// Device/VariationIndex table offset. Only the design value is used; the
// device offset is consumed but never dereferenced (no Device adjustment).
function parseMathValueRecord() {
    const value = this.parseShort();
    this.parseOffset16();
    return value;
}

// GlyphPartRecord, all fields are inline (no offsets).
function parseGlyphPartRecord() {
    return {
        glyphIndex: this.parseUShort(),
        startConnectorLength: this.parseUShort(),
        endConnectorLength: this.parseUShort(),
        fullAdvance: this.parseUShort(),
        flags: this.parseUShort()
    };
}

// GlyphAssembly: italics correction plus an inline list of parts.
function parseGlyphAssembly(tableEnd) {
    // MathValueRecord (4) plus the part count (2) must be readable.
    check.argument(this.offset + 6 <= tableEnd, 'MATH GlyphAssembly is truncated.');
    const italicsCorrection = parseMathValueRecord.call(this);
    const partCount = this.parseUShort();
    // Ten bytes per GlyphPartRecord follow the six-byte header.
    check.argument(this.offset + this.relativeOffset + partCount * 10 <= tableEnd,
        'MATH GlyphAssembly is truncated.');
    const parts = new Array(partCount);
    for (let i = 0; i < partCount; i += 1) {
        parts[i] = parseGlyphPartRecord.call(this);
    }
    return { italicsCorrection: italicsCorrection, parts: parts };
}

// MathGlyphConstruction: an inline variant count/list and a (possibly NULL)
// offset to a GlyphAssembly. The assembly offset is relative to the start of
// the MathVariants table, per the spec.
function parseMathGlyphConstruction(mathVariantsStart, tableEnd) {
    const recordStart = this.offset + this.relativeOffset;
    // The two header shorts must both be readable before touching them.
    check.argument(recordStart + 4 <= tableEnd,
        'MATH MathGlyphConstruction is truncated.');
    const glyphAssemblyOffset = this.parseOffset16();
    const variantCount = this.parseUShort();
    // Four bytes per MathGlyphVariantRecord follow the header.
    check.argument(recordStart + 4 + variantCount * 4 <= tableEnd,
        'MATH MathGlyphConstruction is truncated.');
    const mathGlyphVariants = new Array(variantCount);
    for (let i = 0; i < variantCount; i += 1) {
        mathGlyphVariants[i] = {
            glyphIndex: this.parseUShort(),
            // MathGlyphVariantRecord's second field is the advance measurement
            // in the growth direction (width for horizontal, height for vertical).
            advance: this.parseUShort()
        };
    }
    let glyphAssembly = undefined;
    if (glyphAssemblyOffset !== 0) {
        const assemblyStart = mathVariantsStart + glyphAssemblyOffset;
        check.argument(assemblyStart < tableEnd,
            'MATH GlyphAssembly offset is out of bounds.');
        const assemblyParser = new Parser(this.data, assemblyStart);
        glyphAssembly = parseGlyphAssembly.call(assemblyParser, tableEnd);
    }
    return { glyphAssembly: glyphAssembly, mathGlyphVariants: mathGlyphVariants };
}

// Resolve the coverage index of a glyph.
function coverageIndexOf(coverage, glyphIndex) {
    if (!coverage) {
        return -1;
    }
    if (coverage.format === 1) {
        const glyphs = coverage.glyphs;
        for (let i = 0; i < glyphs.length; i += 1) {
            if (glyphs[i] === glyphIndex) {
                return i;
            }
        }
        return -1;
    }
    const ranges = coverage.ranges;
    for (let i = 0; i < ranges.length; i += 1) {
        const range = ranges[i];
        if (glyphIndex >= range.start && glyphIndex <= range.end) {
            return range.index + (glyphIndex - range.start);
        }
    }
    return -1;
}

// Read one of the two MathVariants construction subtables
// (vertGlyphCoverage / horizGlyphCoverage + offset list).
function parseConstructionSubtable(p, mathVariantsStart, tableEnd, coverageOffset, constructionOffsets) {
    const count = constructionOffsets.length;
    check.argument(coverageOffset !== 0, 'MATH construction coverage offset is NULL.');
    const coverageStart = mathVariantsStart + coverageOffset;
    check.argument(coverageStart + 4 <= tableEnd,
        'MATH construction coverage offset is out of bounds.');
    const coverageParser = new Parser(p.data, coverageStart);
    const coverageFormat = coverageParser.parseUShort();
    const coverageCount = coverageParser.parseUShort();
    const coverageSpan = coverageFormat === 2 ? coverageCount * 6 : coverageCount * 2;
    check.argument(coverageStart + 4 + coverageSpan <= tableEnd,
        'MATH construction coverage is truncated.');
    coverageParser.relativeOffset = 0;
    const coverage = coverageParser.parseCoverage();
    check.argument(coverageParser.offset + coverageParser.relativeOffset <= tableEnd,
        'MATH construction coverage is truncated.');
    const constructions = new Array(count);
    for (let i = 0; i < count; i += 1) {
        const constructionOffset = constructionOffsets[i];
        check.argument(constructionOffset !== 0, 'MATH MathGlyphConstruction offset is NULL.');
        const constructionStart = mathVariantsStart + constructionOffset;
        check.argument(constructionStart < tableEnd,
            'MATH MathGlyphConstruction offset is out of bounds.');
        const constructionParser = new Parser(p.data, constructionStart);
        constructions[i] = parseMathGlyphConstruction.call(constructionParser, mathVariantsStart, tableEnd);
    }
    return { coverage: coverage, constructions: constructions };
}

// Find the MathGlyphConstruction for a glyph in one growth direction,
// or return null if the glyph is not covered in that direction.
function findConstruction(subtable, glyphIndex, numGlyphs) {
    const index = coverageIndexOf(subtable.coverage, glyphIndex);
    if (index < 0) {
        return null;
    }
    check.argument(index < subtable.constructions.length,
        'MATH coverage index is out of bounds.');
    const construction = subtable.constructions[index];
    if (construction.glyphAssembly) {
        const parts = construction.glyphAssembly.parts;
        for (let i = 0; i < parts.length; i += 1) {
            check.argument(parts[i].glyphIndex < numGlyphs,
                'MATH GlyphPartRecord glyph index is out of bounds.');
        }
    }
    const variants = construction.mathGlyphVariants;
    for (let i = 0; i < variants.length; i += 1) {
        check.argument(variants[i].glyphIndex < numGlyphs,
            'MATH MathGlyphVariantRecord glyph index is out of bounds.');
    }
    return construction;
}

function parseMATHTable(data, start, tableLength) {
    start = start || 0;
    // Declared table length makes truncation detectable even when another
    // table immediately follows in the SFNT.
    const tableEnd = tableLength !== undefined ? start + tableLength : data.byteLength;
    check.argument(tableEnd <= data.byteLength, 'MATH table length is out of bounds.');
    // A MATH header always contains the version and three 16-bit offsets.
    check.argument(tableEnd - start >= 10, 'MATH table is truncated.');
    const p = new Parser(data, start);
    const tableVersion = p.parseVersion(1);
    check.argument(tableVersion === 1, 'Unsupported MATH table version.');

    // The remaining top-level fields are all 16-bit offsets (from the start
    // of the MATH table) to optional subtables.
    const mathConstantsOffset = p.parseOffset16();
    const mathGlyphInfoOffset = p.parseOffset16();
    const mathVariantsOffset = p.parseOffset16();

    const result = {
        version: tableVersion,
        mathConstantsOffset: mathConstantsOffset,
        mathGlyphInfoOffset: mathGlyphInfoOffset,
        mathVariants: undefined
    };

    if (mathVariantsOffset === 0) {
        // No stretch data at all: still a valid table.
        return result;
    }
    const mathVariantsStart = start + mathVariantsOffset;
    check.argument(mathVariantsStart < tableEnd,
        'MATH MathVariants offset is out of bounds.');
    // The fixed MathVariants header is eight bytes.
    check.argument(mathVariantsStart + 8 <= tableEnd,
        'MATH MathVariants header is truncated.');

    const mv = new Parser(data, mathVariantsStart);
    const minConnectorOverlap = mv.parseUShort();
    const vertCoverageOffset = mv.parseOffset16();
    const horizCoverageOffset = mv.parseOffset16();
    const vertVariantCount = mv.parseUShort();
    check.argument(mv.offset + mv.relativeOffset + vertVariantCount * 2 <= tableEnd,
        'MATH MathVariants vertical offset list is truncated.');
    const vertConstructionOffsets = mv.parseUShortList(vertVariantCount);
    const horizVariantCount = mv.parseUShort();
    check.argument(mv.offset + mv.relativeOffset + horizVariantCount * 2 <= tableEnd,
        'MATH MathVariants horizontal offset list is truncated.');
    const horizConstructionOffsets = mv.parseUShortList(horizVariantCount);
    check.argument(mv.offset + mv.relativeOffset <= tableEnd,
        'MATH MathVariants header is truncated.');
    check.argument(mathVariantsStart + vertCoverageOffset < tableEnd,
        'MATH vertical coverage offset is out of bounds.');
    check.argument(mathVariantsStart + horizCoverageOffset < tableEnd,
        'MATH horizontal coverage offset is out of bounds.');

    const vertical = parseConstructionSubtable(mv, mathVariantsStart, tableEnd,
        vertCoverageOffset, vertConstructionOffsets);
    check.argument(vertical.constructions.length === vertVariantCount,
        'MATH vertical construction count is inconsistent.');
    const horizontal = parseConstructionSubtable(mv, mathVariantsStart, tableEnd,
        horizCoverageOffset, horizConstructionOffsets);
    check.argument(horizontal.constructions.length === horizVariantCount,
        'MATH horizontal construction count is inconsistent.');

    // Glyph-index bounds are checked lazily in findConstruction(), because
    // the MATH table is dispatched before maxp gives us the glyph count.
    result.mathVariants = {
        minConnectorOverlap: minConnectorOverlap,
        vertical: vertical,
        horizontal: horizontal
    };
    return result;
}

export default { parse: parseMATHTable, findConstruction: findConstruction, coverageIndexOf: coverageIndexOf };
